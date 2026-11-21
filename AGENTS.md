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
C++ call stack freezes). The model behind every rule in this file is that **a simulation process is
a coroutine, not an OS process**: the kernel's scheduler runs exactly one simulation process at a
CPU instant (no preemption, no parallelism between tasks), a process suspends only at its explicit
blocking points — all of them the single `detail::backend_wait()` → `port::wait` path — so
everything a body does between two blocking points is atomic with respect to every other task
(registry mutation, unlinking a waiter, handing a lock to the queue head or clearing a flag needs no
`std::mutex` / `std::atomic` / spinlock / `std::thread`, which would be meaningless inside the
simulator and across DPI), while state that must outlive a suspension has to be consistent and
published *before* entering `port::wait`, because from that point another process may inspect or
mutate it. Everything else follows: cancellation is cooperative because a body cannot be preempted
and a process cannot be killed; an edge is never buffered, so ordering comes from a preceding
blocking point or event rather than from timing; and no test needs a sleep to "let the other side
catch up".

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
  `trigger` / `destroy_event` / `ticks_per_sec` / `wait`) plus the two non-suspending queries
  (`now_ticks`, `owner_task`), the identity write (`set_owner_task`) and the `weave_task_main`
  dispatch entry — and internal headers, and must never reference DPI/SystemC/thread APIs directly.
- Backend selection is a compile-time macro only (`src/core/config.hpp`, mutually exclusive, SVDPI
  by default); `--backend=` only picks it (and therefore the matching package). The public headers
  never branch on the macro, so it is not part of the installed API.
- A new backend = one macro-guarded `src/backend/*.cpp` implementing the port contract + a bootstrap
  entry point, with **zero changes to core code**.
- **Touch the port contract only when a new suspension shape is required**, and a new *failure* shape
  only when an exception cannot carry it; its guarantees are in the `port.hpp` header comment.

## Invariants (check each one when changing `src/core/`)

- **Single blocking point**: every suspension goes through `detail::backend_wait()` → `port::wait`;
  nothing else may suspend. That point also publishes the suspension (`TaskRecord::wait_ev` /
  `wait_since`) for the wait-graph snapshot and retracts it through a guard, so the cancellation throw
  cannot leave a stale waiter behind in `debug::snapshot()`.
- **Cooperative cancellation**: `cancel(msg)` only arms `cancel_armed`, records the msg and triggers
  the private abort event; `CancelledError(msg)` is injected at the blocking point, and a body that
  swallows it and returns normally ends DONE. Killing processes is forbidden; cascade cancellation
  of siblings passes an empty msg (injection and the rethrow at `await` keep the original).
- **The exception type hierarchy must not change**: `CancelledError` **does not derive** from
  `std::exception`, `TimeoutError` **does** (`std::runtime_error`).
- **`what()` and `~BrokenBarrierError` stay out-of-line** (key functions → one typeinfo in
  libweave.so, which is what makes cross-DSO `catch` match; see the ABI notes in backends.md), and
  `CancelledError` stays polymorphic with a virtual destructor.
- **Unrecoverable violations end the run, they are never thrown**: a blocking call with no task
  context (or after the body returned) and a diagnosed wait cycle (`Task::await()` on itself,
  `TaskGroup::wait()` from its own child, `shield()` on the running task) go through
  `detail::fatal_violation(WEAVE_SITE, what)` →
  `port::fatal` — headline logged through simlog at `critical` (so `simv -l` and file sinks carry it),
  `debug::dump("fatal")`, `detail::flush_trace()`, then the backend's own failure channel
  (`$fatal(1, …)` via `weave_sv_fatal`, `SC_REPORT_FATAL`) which repeats the headline — never a bare
  `stderr` write, which no simulator log records. Never `throw`: a `catch (...)` would turn a
  guaranteed hang into a run whose pass marker still prints,
  and at an ownerless site the unwind escapes through the consumer's own DPI export (UB). Misuse the
  caller can recover from — `Lock`/`Semaphore` protocol errors, `as_completed`/`TaskGroup` misuse, an
  invalid handle — still throws `std::logic_error` before mutating anything. `DeadlockError` was
  removed for this reason; do not reintroduce a catchable type for a contract violation.
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
  cancelled; `wait_for` timeout → cancel, wait for the landing, then `TimeoutError`; `wait_for`
  whose **caller** is cancelled → cascade-cancel the awaited task and wait for its landing before
  the original `CancelledError` propagates (msg preserved, cascade carries none), so a cancelled
  waiter never leaves its child running — `timeout(body, limit)` inherits exactly this pair.

## Where new content goes

| Content | Location |
|---|---|
| New public diagnostic header (debug/…) | `include/weave/debug.hpp` + `src/core/debug.cpp` + umbrella include |
| New event-domain primitive (Pulse/…) | `include/weave/event.hpp` + `src/core/event.cpp` |
| New cross-language surface (sv/…) | `include/weave/sv.hpp` + `src/core/sv.cpp` + umbrella include |
| New sync primitive (TaskGroup/shield/as_completed/…) | `include/weave/sync.hpp` + `src/core/sync.cpp` |
| New communication primitive (Queue/Lock/Semaphore/…) | New public header `include/weave/<domain>.hpp` + add the include to the umbrella |
| New backend | `src/backend/<name>.cpp` (macro-guarded) + macro in `src/core/config.hpp` + xmake mapping |
| New self-check assertions | `tests/<domain>_semantics.cpp`, added to both `weave_smoke_sc` and `weave_smoke_svdpi` (a backend-only body needs its own file and target, e.g. `tests/svdpi_scope_semantics.cpp`) |
| New self-check on a body that must **end the run** | `tests/fatal_semantics.cpp` → the `weave_fatal_*` payloads, judged with **inverted** expectation by `tests/expect_fatal.sh` / `tests/svdpi/run.sh --expect-fatal` |
| New example | `examples/<domain>_demo.cpp` (picked up automatically by glob, no xmake.lua change needed) |

Principle: **a primitive composable from "task bookkeeping + Event + three-legged wait" goes into the
core layer; only a new suspension shape justifies touching the port contract, and only a failure an
exception cannot carry justifies a new fatal site.** Public header
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
  in name order and any failure fails the whole run. Two deliberate exceptions keep the same
  symmetry from the other side: `tests/scope_sc_thread_semantics.cpp` asserts what a *non-task*
  `SC_THREAD` sees, so it needs a raw `sc_spawn` and is linked into `weave_smoke_sc` only;
  `tests/svdpi_scope_semantics.cpp` switches the DPI scope with `svdpi.h`, so it exists only in the
  SVDPI payload (`weave_smoke_svdpi`).
- **A body that is supposed to die cannot live in a smoke payload**: `expect_fatal.sh` and
  `run.sh --expect-fatal` only accept an exit that *lacks* the pass marker and *carries* the
  `weave: fatal:` headline, so such a case goes into `tests/fatal_semantics.cpp` (payloads
  `weave_fatal_sc` / `weave_fatal_svdpi`) and the smoke run keeps its normal expectation. Only one
  fatal site is covered end-to-end; the others share `detail::fatal_violation`.
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
- The clock query (`port::now_ticks`) and the identity query (`port::owner_task`) are the port's two
  non-suspending primitives; the core clamps the clock monotonically non-decreasing, `wait_until`
  must return **without suspending** for an expired deadline (sleeping 0 instead would be a yield,
  which is a different operation) and `now()` must saturate rather than throw / divide by zero when a
  backend reports `ticks_per_sec() == 0`.
- **SVDPI: the DPI scope belongs to the calling process, and a consumer's process does not import
  `weave_pkg`** — every suspension and `current_task()` call back into the package, and VCS 2018.09
  aborts with `Error-[DPI-DXFNF] … 'weave_sv_current_task' … is not defined or visible` when the
  entry scope is the consumer's testbench process (shuttle hit exactly this). The backend therefore
  wraps **each** import in a scope guard (cached `svGetScopeFromName("weave_pkg")`, caller's scope
  restored on the way out), so a library embedding weave needs no `svSetScope` of its own;
  `tests/svdpi_scope_semantics.cpp` pins that from a module which imports nothing. Any new import
  added to `src/backend/svdpi.cpp` must go through the same guard.
- **`sv::Edge` is bound by name, and an edge is never buffered**: the testbench's `weave_edge` must
  be constructed before the C++ side fires (the probe in `tests/weave_tb.sv` builds its edges before
  `weave_run`, so the responders are registered in the first delta the run suspends in). A leg that
  creates its edge later is only safe when the C++ side has already parked in `wait()` — order it
  with an event or with another edge, never with a `#delay` assumption. The name table is per
  process and nothing is destroyed (lifetime = the run); `bound()` is the only way a payload can
  tell that a testbench claimed the name.
