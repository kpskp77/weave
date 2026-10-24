# AGENTS.md

For AI coding assistants working in this repository: build/verification commands, and the
invariants you must preserve when making changes. For the project's positioning see
[README.md](README.md) (including the C++↔asyncio mapping), for the architectural rationale see
[docs/architecture.md](docs/architecture.md), and for backend integration see
[docs/backends.md](docs/backends.md).

## The project in one sentence

Weave is a behaviour orchestration layer between C++ test logic and EDA simulation kernels: the API
is borrowed from Python asyncio, but **does not use C++20 coroutines** — blocking semantics are
carried by backend simulation processes (when an SV process / SystemC SC_THREAD suspends, the entire
C++ call stack freezes).

## Build and verify (mandatory after changes)

**Principle: after every change, regress every case — no cherry-picking, no skipping.**

```bash
xmake smoke      # Semantic self-checks: build smoke payloads + run each weave_smoke_* binary via xmake run
xmake examples   # Example regression (independent): runs both the systemc and svdpi backends in one go
```

- Cross-backend changes (`port.hpp`, backend files, `config.hpp`, xmake macro mapping) must be run
  once under each configuration:

```bash
xmake f --backend=svdpi   && xmake smoke
xmake f --backend=systemc && xmake smoke
xmake f --backend=svdpi   # restore the default configuration
```

- Self-check binaries are run one by one by the smoke task only if named `weave_smoke_*`; a new
  self-check target must also be added to the `xmake build` list of the `smoke` task (running always
  goes through `xmake run`, see "Pitfalls in this environment" below).
- The `tests/` and `examples/` payloads are strictly separated: examples link `libweave.so` only via
  `add_deps("weave")` (never inlining core sources) and are not compiled into any `weave_smoke_*`
  payload; the reverse holds too. Changing public headers or examples only needs a single
  `xmake examples` run.
- Without VCS/IES the SVDPI driver prints SKIP and returns 0 (equivalent to a compile-only check), so
  the SVDPI side can be skipped.
- The first build automatically installs SystemC from xmake-repo; self-checks run on a real kernel,
  and delta-cycle semantics make the conclusions deterministic.

## Architecture: core/backend separation

```
include/weave/    Public API: weave.hpp (umbrella) + config/time/task/event/sync/test[/sc]
src/core/         Backend-agnostic: task/event/sync/time/registry + port.hpp (contract) + *_internal.hpp
src/backend/      svdpi.cpp (default) / systemc.cpp, whole files guarded by WEAVE_BACKEND_* macros
```

Core bookkeeping **may only depend on the 7 primitives of `port.hpp`** and internal headers, and must
never reference DPI/SystemC/thread APIs directly. The only switch for backend selection is a
compile-time macro (`config.hpp`, mutually exclusive, SVDPI by default); `--backend=` merely defines
the macro. A new backend = one macro-guarded `src/backend/*.cpp` implementing the port contract + a
bootstrap entry point, with **zero changes to core code**.

The four guarantees of the port contract are in the `port.hpp` header comment (summary in
architecture.md): spawn is "scheduled, not run immediately"; at any instant at most one process
executes C++ bookkeeping; events are one-shot and trigger never targets an already destroyed id;
`wait` is the only suspension point (three-legged race, cancel leg wins, a negative id disables that
leg). **Touch the contract only when a new suspension shape is required.**

## Invariants (check each one when changing `src/core/`)

- **Single blocking point**: every suspension goes through `detail::backend_wait()` → `port::wait`;
  nowhere else may introduce a suspension.
- **Cooperative cancellation**: `cancel(msg)` only sets `cancel_armed` + records msg + triggers the
  private abort event, and `CancelledError(msg)` is injected at the blocking point; if the body
  swallows `CancelledError` and returns normally → the task is DONE. Killing processes is forbidden
  (it breaks RAII unwinding); cascade cancellation of siblings passes an empty msg, while the
  injection point and the rethrow at `await` preserve the original msg.
- **The exception type hierarchy must not change**: `CancelledError` **does not derive** from
  `std::exception` (matching `BaseException`, so it is not silently swallowed by a backstop catch),
  while `TimeoutError` **does derive** (matching asyncio's `TimeoutError`, an ordinary `Exception`).
- **`what()` for both exception types must be defined out-of-line in the library**
  (`src/core/sync.cpp` / `src/core/task.cpp`): it is the key function for both types, so the
  vtable/typeinfo is emitted exactly once in libweave.so and consumer TUs keep only undefined
  references. Writing it back as inline degrades typeinfo into a per-TU weak definition, and
  libc++abi compares by pointer → cross-DSO `catch` silently mismatches (neither compilation nor
  linking reports anything). Therefore `CancelledError` is a polymorphic type with a virtual
  destructor (no longer trivially copyable; the assertions became nothrow-copy + no heap members).
- **Event edge semantics**: `set()` snapshots the current waiters and triggers each one, and does not
  re-check the flag after releasing them; on **both paths** (success and cancellation) `wait()` must
  unlink its wake event from `wake_evs` before destroying it, otherwise a later `set()` would
  trigger an already destroyed id. That cleanup is currently carried by a single
  `detail::scope_exit` guard (the unlink-then-destroy ordering is written inside the lambda body, not
  left to destruction order).
- **Lock hand-off never loses ownership**: `release()` hands the lock to the queue head within the
  same non-suspending call (`held` stays true throughout, so there is no idle window and newcomers
  cannot jump the queue); each queued waiter holds a one-shot `Event` as the hand-off signal. If a
  waiter is cancelled after it has already been `granted` (cancellation and hand-off in the same
  slot → the backend cancel leg wins, so it receives `CancelledError` instead of the lock), it must
  `handoff()` to the next waiter, otherwise the lock vanishes permanently with the cancellation.
- **`detail::on_scope_exit` is used for state that still has readers after the frame dies**:
  `Event::wait()` (`wake_evs` lives on the shared `Impl`), `AsCompleted::next()` (`waiting` lives on
  the cross-call `*impl_`), and `wait()`/`TaskGroup::wait()` (`WaitState`/`GroupState` are local, but
  they are held by `shared_ptr` from the done_callbacks of unfinished child tasks, so they are still
  alive after the frame dies and those callbacks still read `waiting`/`signalled`).
- **Whether flag reset is coupled with event destruction depends on whether cancellation escapes**:
  `wait()` (asyncio.wait) is designed to propagate `CancelledError` as-is (without cancelling child
  tasks); when it escapes, the child tasks are still pending and `st` is still held by their
  callbacks, so "clear `waiting` and then `destroy_event`" must be written as a pair inside the same
  guard — if split into "guard + straight-line code", the straight-line statement never runs on the
  cancellation path, and a child task completing later would `trigger` an already destroyed id (the
  SystemC side throws `invalid event id`). `TaskGroup::wait()` differs: it catches `CancelledError`
  in place, drains the siblings, and only then rethrows, so resetting the flag right after the `try`
  is enough and the guard only needs to destroy the event.
- Guard destructors neither throw nor suspend; cleanup that needs to suspend (waiting for landing
  after a TaskGroup cascade cancellation) always stays in explicit code.
- **Time**: internally int64 femtoseconds, `to_ticks` rounds up to guarantee sleeping at least long
  enough, and **non-positive durations are always clamped to 0 ticks** (`sleep(0)` stays 0 ticks; a
  negative tick means "no timeout leg" in the wire format, and letting it through blocks forever).
  The femtosecond ceiling is ≈ 2.56 h of simulation time; an out-of-range literal is a **compile
  error** (the constexpr guard in `time.hpp`), while runtime arithmetic (`+ - * /`) out of range
  throws `std::overflow_error` and division by zero throws `std::domain_error` — neither side
  **wraps silently**.
- **Failure propagation**: `gather` rethrows the first failure immediately in **completion order**
  and does not cancel siblings (cancelling siblings is TaskGroup semantics); if gather itself is
  cancelled it first cascade-cancels the unfinished tasks; `wait_for` timeout → cancel, wait for the
  cancellation to complete, then throw `TimeoutError`.

## Where new content goes

| Content | Location |
|---|---|
| New sync primitive (TaskGroup/shield/as_completed/…) | `include/weave/sync.hpp` + `src/core/sync.cpp` |
| New communication primitive (Queue/Lock/Semaphore/…) | New public header `include/weave/<domain>.hpp` + add the include to the umbrella |
| New backend | `src/backend/<name>.cpp` (macro-guarded) + macro in `config.hpp` + xmake mapping |
| New self-check assertions | `tests/<domain>_semantics.cpp`, added to both `weave_smoke_sc` and `weave_smoke_svdpi` |
| New example | `examples/<domain>_demo.cpp` (picked up automatically by glob, no xmake.lua change needed) |

Principle: **a primitive that can be composed from "task bookkeeping + Event + three-legged wait"
goes into the core layer; only a new suspension shape justifies touching the port contract.**
Public header layering (dependencies point downwards): config → time → task/event → sync → test, with
the umbrella `weave.hpp` last; every public header must compile standalone. Core internal state lives
only in `src/core/*_internal.hpp`, never in public headers.

## Naming and language baseline

- **Naming follows asyncio**: any primitive that has an asyncio counterpart must use the same name
  (`done`/`cancelled`/`cancel`/`get_name`, `create_task`, `sleep`,
  `Event::wait/set/clear/is_set`, `gather`, `wait_for`, `TaskGroup`, `shield`, `as_completed`,
  `CancelledError`, `TimeoutError`), and new primitives are no exception — check asyncio first, do
  not invent. The `Error` suffix is part of the name: `CancelledError` / `TimeoutError` /
  `BrokenBarrierError` keep it, never shorten it to a bare state noun (`Cancelled`, `Timeout`).
  Project-specific concepts with no counterpart (`duration`, `WEAVE_TEST`, `weave::sc::run()`) follow
  C++ conventions; `detail::`/`g_*` are unconstrained.
- **C++17 is the floor, not the ceiling**: the library itself is built as C++17
  (`set_languages("c++17")` is the only switch) and its public capabilities must be complete under
  C++17 — never treat C++20+ features as required; at the same time the public headers must compile
  cleanly under newer standards (C++20/23) (no reliance on constructs whose meaning changes, or that
  are deprecated/removed, with no warnings under `-Wall -Wextra -Werror`), so callers may pick their
  own `-std=`. Public headers must compile standalone (explicitly include the headers they use, no
  reliance on transitive inclusion). Evaluated boundaries (`string_view` cannot replace parameters
  that need ownership, port wire-format sentinels stay negative, no `if constexpr` without a
  type-dependent branch, `CancelledError::what()` must stay out-of-line to keep typeinfo unique per
  library) are covered in the "Language baseline" section of architecture.md.

## Test-writing conventions

- Synchronisation must be **fully event-driven** (ordering carried by `Event`); expressing timing
  through `sleep` durations is forbidden.
- Wrap each case in `run_case(__func__, "caseN", [&] { ... })`, failing with
  `throw std::logic_error("caseN: description")`; end each `WEAVE_TEST` with `report(__func__)`.
  Without the wrapper the regression log shows no per-case results.
- Use the invariants for synchronisation: "the waiting side returned from the blocking call" by
  itself guarantees the triggering side has advanced past the blocking point (lock hand-off/process
  scheduling barrier).
- Assertion bodies are centralised in `tests/` and separated from the backend main entry points, and
  shared by both smoke payloads (the same set except `sc_main.cpp`, a symmetry kept on purpose);
  registered `WEAVE_TEST`s run serially in name order, and any failure fails the whole run
  (`registry.cpp`).
- **Task bodies must not reference stack frames that are destroyed earlier**: when a lambda captures
  a local `Event`/`Task` by reference, the task must be guaranteed to finish before that frame exits
  (`await`/`gather`, or on the exception path finish off the siblings first), otherwise resuming is a
  use-after-free.
- Examples do not use `run_case` and end with a single `printf` line; they must be event-driven too,
  and must not rely on timing that holds on only one backend — they have to pass both as a SystemC
  executable and in the VCS two-library form.

## Code style (mandatory before committing)

```bash
clang-format -i <changed .cpp/.hpp>                                        # Google base, see .clang-format
verible-verilog-format --inplace --flagfile=.verible-format <changed .sv>  # see .verible-format
stylua <changed .xmake>                                                    # see .stylua.toml
```

- Only format the files you changed; no unrelated whole-repository reshuffles. The two config files
  are authoritative for the exact parameters (2-space indent, column width 100, etc.).
- `sv/weave_pkg.sv` contains `->` event trigger statements, which are beyond verible's parser
  (syntax error): keep that file manually consistent with the configured parameters and do not force
  verible on it.

## Pitfalls in this environment (already hit)

- **The triggered state of an already triggered event lingers** (SystemC via `change_stamp`, SV via a
  sticky flag): after a task catches `CancelledError` and blocks again on the same abort_ev, that
  would be misdetected as a cancellation hit, so `backend_wait` must destroy and recreate `abort_ev`
  after injecting cancellation. Likewise, combinators that use raw port events must use a waiting
  flag to guarantee a trigger always has a waiter (a trigger with no waiter leaves an unconsumed
  delta notification, and destroying that is undefined behaviour on the backend side).
- **SV has no atomic "register + yield" wait primitive, and the SVDPI wait is therefore not built
  from `fork … join_any`** (that shape was removed on purpose; do not reintroduce it). Two
  independent traps made it unusable: (a) forked legs are scheduled in a later delta, so the
  registration is not atomic — a trigger can land in between, and a `disable fork` issued by one wait
  kills the not-yet-started legs of *other* waits (the run then stops making progress), while
  `disable <leg>` instead makes VCS report `Error-[FENCE-STACK-CORRUPTION]`; (b) the only observation
  primitive, `wait (ev.triggered)`, is sticky for the whole time slot, so a trigger *older* than the
  wait reads as a fresh hit. The core legitimately reuses one raw event id across consecutive waits
  of the same combinator (`AsCompleted`'s wake_ev, `wait`'s wake_ev, `TaskGroup`'s wake_ev/drain_ev),
  so with sticky observation such a wait never blocks and the C++ side spins in a zero-delay loop
  (`$time` frozen in simv.log, simv at 100% CPU — exactly how `as_completed` case1 died). So
  `weave_sv_wait` = a synchronous registry write inside `weave_sv_trigger` (the `sc_event::notify`
  analogue: it marks the waiters registered on that id) plus a single `wait (w_state[wd] != 0)` for
  the suspension. `@ev` / `@(ev_table[id].ev)` is *not* an alternative: on VCS O-2018.09 a trigger on
  a class-member event never wakes an `@` wait (the legs block forever and the simulation ends
  silently at 0 ps).
- **SV timeouts are owned by the persistent timer service, never by a per-wait `#delay` leg**: a
  `join_none` child reads its captured ids only when it starts, i.e. possibly after the waiter's
  frame is gone. The service sleeps until the earliest deadline and is woken early through
  `timer_poke_gen` — a level *counter*, not an event, so a deadline registered while the service is
  between "scan the deadlines" and "go to sleep" cannot be lost; its `disable fork` is local because
  it is a standalone process with no DPI call on its stack. Wait ids are never recycled, so a late
  deadline can never hit a newer waiter. Same-slot cancel precedence still comes from the sticky
  `ev_table[abort_id].ev.triggered` test (the `ab.triggered()` analogue): `task_semantics` case9,
  `lock_semantics` case6.
- xmake-repo's SystemC looks for `sc_main` with **C linkage**: the entry point must be `extern "C"`
  (see `tests/sc_main.cpp`).
- **SystemC self-checks must be run through `xmake run`**: `add_packages(systemc)` does not produce an
  rpath (xmake only adds `$ORIGIN` automatically for shared target dependencies), so at runtime the
  package's `LD_LIBRARY_PATH` is injected by `xmake run`; executing via `os.execv(targetfile)`
  directly fails with `library "libsystemc.so.3.0" not found`. Hence the smoke / examples tasks all
  go through `xmake run <target>`.
- This xmake version: `add_requires` may only appear at top-level scope and must not follow
  option/target definitions (scope leakage); `has_config(name, value)` is unreliable for set_values
  options, use `get_config(name) == "..."`.
- Time conversion uses `__int128` to prevent overflow; `ticks_per_sec()` is queried once through the
  port and then cached (static local).
- **Cross-DSO `catch` depends on typeinfo uniqueness**: guaranteed by "`what()` for the exception
  types is defined out-of-line" (see the invariants section); consumer executables need **no**
  `-fvisibility=default` / `-rdynamic`, and `catch` still works even though xmake `mode.release` sets
  `symbols=hidden` by default on non-shared targets (`-fvisibility=hidden
  -fvisibility-inlines-hidden`, unrelated to packages). How to check: `nm -D <exe> | grep _ZTIN5weave`
  should be all `U`, with definitions only in libweave.so; the opposite (a `D`/`V` definition appearing
  in the exe) means the key function was written back as inline. See the "ABI notes" section of
  [docs/backends.md](docs/backends.md).
