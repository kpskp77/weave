# AGENTS.md

For AI coding assistants working in this repository: the build/verification commands and the rules
every change must preserve. Rationale lives elsewhere — [README](README.md) (positioning,
C++↔asyncio mapping), [architecture](docs/architecture.md) (design decisions),
[backends](docs/backends.md) (DPI/SystemC backend, ABI notes) — and each rule's per-code
reason is in the header comment of the file it names.

## The project in one sentence

Weave is a behaviour orchestration layer between C++ test logic and EDA simulation kernels: the API
is borrowed from Python asyncio, but **does not use C++20 coroutines** — blocking semantics are
carried by backend simulation processes (when an SV process / SystemC SC_THREAD suspends, the entire
C++ call stack freezes).

## Build and verify (mandatory after changes)

**Principle: after every change, regress every case — no cherry-picking, no skipping.**

```bash
just smoke      # Semantic self-checks: build smoke payloads + run each weave_smoke_* via xmake run (once per backend)
just examples   # Example regression (independent): runs both the systemc and svdpi backends in one go
```

The regression recipes live in the `justfile`; the build targets they invoke live in `xmake.lua`
(xmake defines no custom tasks — `xmake smoke` and friends no longer exist).

- Cross-backend changes (`port.hpp`, backend files, `src/core/config.hpp`, xmake macro mapping) are covered
  by `just smoke` and `just examples`, which each switch `--backend` themselves (systemc first,
  then svdpi, restoring the default svdpi configuration). To regress one backend by hand:

```bash
xmake f --backend=svdpi   && xmake run weave_smoke_svdpi_vcs
xmake f --backend=systemc && xmake run weave_smoke_sc
xmake f --backend=svdpi   # restore the default configuration
```

- Example-only changes need only a single `just examples` run.
- A new self-check target (a `weave_smoke_*` binary, or a `weave_smoke_*` phony target whose
  `on_run` executes a script, like `weave_smoke_svdpi_vcs`) must also be added to the `smoke`
  recipe in the `justfile`; running always goes through `xmake run` (see the pitfalls below).
- The `tests/` and `examples/` payloads are strictly separated: examples link `libweave.so` only via
  `add_deps("weave")` (never inlining core sources) and are not compiled into any `weave_smoke_*`
  payload; the reverse holds too.
- Without VCS/IES `tests/svdpi/run.sh` prints SKIP and returns 0, i.e. a compile-only check.

## Architecture: core/backend separation

```
include/weave/    Public API: weave.hpp (umbrella) + time/task/event/sync/test[/sc]
src/core/         Backend-agnostic: task/event/sync/time/registry + locks/queue/semaphore/barrier
                  + port.hpp (contract) + config.hpp (backend-selection macro, internal)
                  + *_internal.hpp
src/backend/      svdpi.cpp (default) / systemc.cpp, whole files guarded by WEAVE_BACKEND_* macros
```

- Core bookkeeping **may only depend on `port.hpp`** — its 6 primitives (`spawn` / `create_event` /
  `trigger` / `destroy_event` / `ticks_per_sec` / `wait`) plus the `weave_task_main` dispatch entry —
  and internal headers, and must never reference DPI/SystemC/thread APIs directly.
- Backend selection is a compile-time macro only (`src/core/config.hpp`, mutually exclusive, SVDPI
  by default); `--backend=` only picks it (and therefore the matching package). The public headers
  never branch on the macro, so it is not part of the installed API.
- A new backend = one macro-guarded `src/backend/*.cpp` implementing the port contract + a bootstrap
  entry point, with **zero changes to core code**.
- **Touch the port contract only when a new suspension shape is required**; its four guarantees are
  in the `port.hpp` header comment.

## Invariants (check each one when changing `src/core/`)

- **Single blocking point**: every suspension goes through `detail::backend_wait()` → `port::wait`;
  nothing else may suspend.
- **Cooperative cancellation**: `cancel(msg)` only arms `cancel_armed`, records the msg and triggers
  the private abort event; `CancelledError(msg)` is injected at the blocking point, and a body that
  swallows it and returns normally ends DONE. Killing processes is forbidden; cascade cancellation
  of siblings passes an empty msg (injection and the rethrow at `await` keep the original).
- **The exception type hierarchy must not change**: `CancelledError` **does not derive** from
  `std::exception`, `TimeoutError` **does**.
- **`what()` and `~BrokenBarrierError` stay out-of-line** (key functions → one typeinfo in
  libweave.so, which is what makes cross-DSO `catch` match; see the ABI notes in backends.md), and
  `CancelledError` stays polymorphic with a virtual destructor.
- **Event edge semantics**: `set()` snapshots the current waiters; on **both** paths (success and
  cancellation) `wait()` must unlink its wake event from `wake_evs` before destroying it, carried by
  a single `detail::scope_exit`.
- **Lock hand-off never loses ownership**: `release()` hands the lock to the queue head inside the
  same non-suspending call with `held` staying true; a waiter cancelled after it was `granted` must
  `handoff()` to the next one.
- **`detail::on_scope_exit`** guards state with readers after the frame dies (`Event::wait`,
  `AsCompleted::next`, `wait`/`TaskGroup::wait`); guard destructors neither throw nor suspend, so
  cleanup that must suspend stays in explicit code.
- **Flag reset is coupled with event destruction only when cancellation escapes**: in `wait()` the
  "clear `waiting` + destroy event" pair must sit inside the same guard, while `TaskGroup::wait()`
  drains in place and only the destruction is guarded.
- **Time**: int64 femtoseconds, `to_ticks` rounds up, and **non-positive durations clamp to 0 ticks**
  (a negative tick means "no timeout leg"); out-of-range literals are **compile errors**, runtime
  `+ - * /` overflow throws and division by zero throws — nothing **wraps silently**.
- **Failure propagation**: `gather` rethrows the first failure it observes (siblings keep running;
  cancelling them is TaskGroup semantics) and cascade-cancels them only if gather itself is
  cancelled; `wait_for` timeout → cancel, wait for the landing, then `TimeoutError`.

## Where new content goes

| Content | Location |
|---|---|
| New sync primitive (TaskGroup/shield/as_completed/…) | `include/weave/sync.hpp` + `src/core/sync.cpp` |
| New communication primitive (Queue/Lock/Semaphore/…) | New public header `include/weave/<domain>.hpp` + add the include to the umbrella |
| New backend | `src/backend/<name>.cpp` (macro-guarded) + macro in `src/core/config.hpp` + xmake mapping |
| New self-check assertions | `tests/<domain>_semantics.cpp`, added to both `weave_smoke_sc` and `weave_smoke_svdpi` |
| New example | `examples/<domain>_demo.cpp` (picked up automatically by glob, no xmake.lua change needed) |

Principle: **a primitive composable from "task bookkeeping + Event + three-legged wait" goes into the
core layer; only a new suspension shape justifies touching the port contract.** Public header
layering (dependencies point downwards): time → task/event → sync → test, umbrella
`weave.hpp` last; core internal state lives only in `src/core/*_internal.hpp`.

## Naming and language baseline

- **Naming follows asyncio**: a primitive with an asyncio counterpart reuses its name — including
  the `Error` suffix (`CancelledError` / `TimeoutError` / `BrokenBarrierError`), never a bare state
  noun. Check asyncio first, do not invent. Project-specific concepts (`duration`, `WEAVE_TEST`,
  `weave::sc::run()`) follow C++ conventions; `detail::`/`g_*` are unconstrained.
- **C++17 is the floor, not the ceiling**: the library is built as C++17 and its public capabilities
  must be complete there — never treat C++20+ features as required — while the public headers must
  also compile cleanly under C++20/23 with `-Wall -Wextra -Werror`, each one standalone.

## Test-writing conventions

- Synchronisation is **fully event-driven** (ordering carried by `Event`); expressing timing through
  `sleep` durations is forbidden.
- One `run_case(__func__, "caseN", [&] { ... })` per case, failing with
  `throw std::logic_error("caseN: description")`; every `WEAVE_TEST` ends with `report(__func__)`.
- "The waiting side returned from the blocking call" already implies the triggering side advanced
  past its blocking point (lock hand-off / process scheduling barrier) — use that, not sleeps.
- Assertion bodies live in `tests/`, separated from the backend entry points, and are shared by both
  smoke payloads (the same set except `sc_main.cpp`, a symmetry kept on purpose); tests run serially
  in name order and any failure fails the whole run.
- **A task body must not reference a stack frame that dies earlier**: a lambda capturing a local
  `Event`/`Task` by reference must be guaranteed to finish before that frame exits (`await`/`gather`,
  or draining the siblings on the exception path), otherwise resuming is a use-after-free.
- Examples: no `run_case`, one final `printf` line, event-driven, and passing on both backends.

## Code style (mandatory before committing)

```bash
clang-format -i <changed .cpp/.hpp>                                        # Google base, see .clang-format
verible-verilog-format --inplace --flagfile=.verible-format <changed .sv>  # see .verible-format
stylua <changed .xmake>                                                    # see .stylua.toml
```

- Only format the files you changed; no unrelated whole-repository reshuffles. The two config files
  are authoritative for the exact parameters (2-space indent, column width 100, etc.).
- `sv/weave_pkg.sv` contains `->` event trigger statements beyond verible's parser (syntax error):
  keep that file manually consistent with the configured parameters and do not force verible on it.

## Pitfalls in this environment (already hit)

- **A triggered event's state lingers**: `backend_wait` therefore destroys and recreates `abort_ev`
  after injecting cancellation, and combinators using raw port events need a waiting flag so a
  trigger always has a waiter.
- **Never rebuild the SV wait from `fork … join_any`** (nor observe an event-table entry through
  `@ev`): it is a synchronous registry write in `weave_sv_trigger` plus a durable waiter flag
  observed as `if (w_state[wd] == 0) @(w_evt[wd])` on the waiter's own event — **never** a
  level-sensitive `wait (w_state[wd] != 0)`, which makes VCS re-evaluate every waiter on every
  registration (O(N²): ~10⁴ live waits already looks like a hang, $time frozen) — and **timeouts
  belong to the persistent timer service**, never to a per-wait `#delay` leg. The timer service's
  deadlines must live in a min-heap, never `foreach`-scanned per wake: the two-scan form is
  O(armed) per wake, also quadratic over staggered deadlines. Postmortem: the `sv/weave_pkg.sv`
  header.
- xmake-repo's SystemC looks for `sc_main` with **C linkage** (see `tests/sc_main.cpp`).
- **Every run goes through `xmake run` — never work around the missing rpath** (mandatory): xmake
  adds no rpath for package libs (`add_packages(systemc/simlog/…)`), and `xmake run` is what injects
  the package `LD_LIBRARY_PATH`. The injection is inherited by child processes, so the phony VCS
  runners (`tests/svdpi/run.sh` → `simv`, loading the payloads via `-sv_lib`) resolve
  libsimlog.so / libsystemc.so exactly this way. Do **not** bake package libdirs into rpath
  (`add_rpathdirs`/`after_load` hacks) to make direct execution work — running a payload outside
  `xmake run` (plain `./simv`, `./binary`, manual VCS invocations) is unsupported.
- This xmake version: `add_requires` may only appear at top-level scope (never after option/target
  definitions, scope leakage); `has_config(name, value)` is unreliable for set_values options — use
  `get_config(name) == "..."`.
- **Cross-DSO `catch` needs no `-fvisibility=default` / `-rdynamic`**; check with
  `nm -D <exe> | grep _ZTIN5weave` (all `U`, definitions only in libweave.so — a `D`/`V` in the exe
  means a `what()` was written back as inline). See the ABI notes in backends.md.
- Time conversion uses `__int128` to prevent overflow; `ticks_per_sec()` is queried once and cached.
