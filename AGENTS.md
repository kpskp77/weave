# AGENTS.md

Constraints for changes in this repository. Rules only: no rationale, no mechanism descriptions, no
file maps — those live in [docs/architecture.md](docs/architecture.md) (design decisions),
[docs/backends.md](docs/backends.md) (backend, ABI), [README](README.md) (positioning), and in the
header comment of the file a rule names.

**Rule = one imperative constraint you can check against a diff.** If a sentence explains *why*, it
belongs in `docs/architecture.md`, not here.

## The premise

Weave is a behaviour orchestration layer between C++ test logic and EDA simulation kernels; its API
is borrowed from asyncio. **A simulation process is a coroutine, not an OS process**: exactly one
runs at a CPU instant, it suspends only at `port::wait`, and it is never preempted. Every guarantee
below follows from that model — when no rule covers your case, derive the answer from the model
instead of guessing.

## Guarantees (every `src/core/` change)

- No `std::mutex`, `std::atomic`, spinlock or `std::thread` in `src/core/`.
- One blocking point only: `detail::backend_wait()` → `port::wait`. Nothing else may suspend.
- Publish before you suspend: a body leaves consistent, published state before entering `port::wait`.
- Never kill a task process.
- Never order anything with a sleep (`#delay`, `sleep(d)`, "let the other side catch up"). Use an
  `Event` or a preceding blocking point.
- Port surface frozen: `spawn`, `create_event`, `trigger`, `destroy_event`, `ticks_per_sec`, `wait`
  (suspending); `now_ticks`, `owner_task` (queries); `set_owner_task` (identity write);
  `weave_task_main` (dispatch). No DPI/SystemC/thread API in `src/core/`.
- Backend selection is the compile-time macro in `src/core/config.hpp` only. Public headers never
  branch on it.
- A new backend is one macro-guarded `src/backend/*.cpp` plus a bootstrap entry point, with zero
  changes to core.
- Touch the port contract only for a new suspension shape; add a fatal site only for a failure an
  exception cannot carry.

## Build and verify (mandatory after changes)

- After **every** change run both recipes; no cherry-picking, no skipping:

```bash
just smoke      # self-checks: build smoke payloads, run each weave_smoke_* via xmake run, once per backend
just examples   # example regression, both backends
```

- Cross-backend changes (`port.hpp`, `src/backend/`, `src/core/config.hpp`, xmake macro mapping) need
  both. Example-only changes need `just examples` alone.
- Single backend by hand: `xmake f --backend=svdpi && xmake run weave_smoke_svdpi_vcs`, then
  `xmake f --backend=systemc && xmake run weave_smoke_sc`, then `xmake f --backend=svdpi`.
- A new self-check target (a `weave_smoke_*` binary, or a `weave_smoke_*` phony target with an
  `on_run` script such as `weave_smoke_svdpi_vcs`) goes into the `smoke` recipe of the `justfile`.
- Do not add custom xmake tasks: the build targets live in `xmake.lua`, the recipes in `justfile`.
- `tests/` and `examples/` stay separated: examples link `libweave.so` only via `add_deps("weave")`
  and never inline core sources; no example is compiled into a `weave_smoke_*` payload, and vice
  versa.
- Without VCS/IES, `tests/svdpi/run.sh` printing SKIP and returning 0 counts as a compile-only pass.

## Definition of done

- `just smoke` and `just examples` pass (or the narrower recipe this change requires).
- Only the files you changed were formatted (`clang-format` / `verible-verilog-format` / `stylua`).
- A changed public header compiles standalone under C++17, C++20 and C++23 with
  `-Wall -Wextra -Werror`.
- A changed English doc has its `_zh` twin updated.
- No debug `printf`, no commented-out code, no temporary files left behind.

## `src/core/` code rules

### Blocking and cancellation

- Through `backend_wait` you publish `TaskRecord::wait_ev` / `wait_since` and retract them through a
  guard.
- `cancel(msg)` arms `cancel_armed`, records the msg, triggers the private abort event. Nothing else.
  `CancelledError(msg)` is injected at the blocking point.
- Cascade cancellation of siblings passes an empty msg; injection and the rethrow at `await` keep the
  original.
- Type hierarchy is frozen: `CancelledError` does not derive from `std::exception`; `TimeoutError`
  derives from `std::runtime_error`.
- `what()` and `~BrokenBarrierError` stay out-of-line. `CancelledError` stays polymorphic with a
  virtual destructor.

### Failures that end the run

- Route them through `detail::fatal_violation(WEAVE_SITE, what)` → `port::fatal`. Never `throw`, never
  a bare `stderr` write. That covers: a blocking call with no task context (or after the body
  returned), `Task::await()` on itself, `TaskGroup::wait()` from its own child, `shield()` on the
  running task.
- Recoverable misuse throws `std::logic_error` before mutating anything: `Lock`/`Semaphore` protocol
  errors, `as_completed`/`TaskGroup` misuse, an invalid handle.
- No catchable type for a contract violation. Do not reintroduce `DeadlockError`.

### Event

- `set()` snapshots the current waiters.
- On both paths (success and cancellation) `wait()` unlinks its wake event from `wake_evs` before
  destroying it, carried by a single `detail::scope_exit`.
- In `wait()` the "clear `waiting` + destroy event" pair is one guard; in `TaskGroup::wait()` only the
  destruction is guarded.

### Lock

- `release()` hands ownership to the queue head inside the same non-suspending call, `held` staying
  true.
- A waiter cancelled after it was `granted` calls `handoff()` to the next one.

### Guards

- `detail::on_scope_exit` guards state that has readers after the frame dies: `Event::wait`,
  `AsCompleted::next`, `wait`, `TaskGroup::wait`.
- Guard destructors neither throw nor suspend. Cleanup that must suspend stays in explicit code.

### Time

- int64 femtoseconds. `to_ticks` rounds up; non-positive durations clamp to 0 ticks.
- Out-of-range literals are compile errors. Runtime `+ - * /` overflow and division by zero throw.
  Nothing wraps.
- The clock is clamped monotonically non-decreasing.
- `wait_until` returns without suspending for an expired deadline.
- `now()` saturates when a backend reports `ticks_per_sec() == 0`.
- `ticks_per_sec()` is queried once and cached; conversions use `__int128`.

### Failure propagation

- `gather` rethrows the first failure it observes and leaves the siblings running; it cascade-cancels
  them only when gather itself is cancelled.
- `wait_for` timeout: cancel the task, wait for its landing, then throw `TimeoutError`.
- `wait_for` with a cancelled caller: cascade-cancel the awaited task, wait for its landing, then
  rethrow the original `CancelledError` with its msg (the cascade carries none).
- `timeout(body, limit)` = that pair with the body spawned as a task. Do not reimplement it.

## Where new content goes

| Content | Location |
|---|---|
| New public diagnostic header (debug/…) | `include/weave/debug.hpp` + `src/core/debug.cpp` + umbrella include |
| New event-domain primitive (Pulse/…) | `include/weave/event.hpp` + `src/core/event.cpp` |
| New cross-language surface (sv/…) | `include/weave/sv.hpp` + `src/core/sv.cpp` + umbrella include |
| New sync primitive (TaskGroup/shield/as_completed/…) | `include/weave/sync.hpp` + `src/core/sync.cpp` |
| New communication primitive (Queue/Lock/Semaphore/…) | New public header `include/weave/<domain>.hpp` + include it from the umbrella |
| New backend | `src/backend/<name>.cpp` (macro-guarded) + macro in `src/core/config.hpp` + xmake mapping |
| New self-check assertions | `tests/<domain>_semantics.cpp`, used by both `weave_smoke_sc` and `weave_smoke_svdpi` |
| Backend-only assertion body | its own file and target, e.g. `tests/svdpi_scope_semantics.cpp` |
| Self-check on a body that must end the run | `tests/fatal_semantics.cpp` → `weave_fatal_*` payloads, judged inverted by `tests/expect_fatal.sh` / `tests/svdpi/run.sh --expect-fatal` |
| New example | `examples/<domain>_demo.cpp` (globbed, no `xmake.lua` change) |

- Header layering: time → task/event → sync → test, umbrella `weave.hpp` last.
- Core internal state lives only in `src/core/*_internal.hpp`.

## Naming

- A primitive with an asyncio counterpart reuses its name, including the `Error` suffix
  (`CancelledError`, `TimeoutError`, `BrokenBarrierError`). Never a bare state noun. Check asyncio
  first, do not invent.
- Project-specific concepts keep C++ conventions (`duration`, `WEAVE_TEST`, `weave::sc::run()`).
  `detail::` and `g_*` are unconstrained.
- Build as C++17, and keep the public API complete there. Do not require a C++20+ feature.
- Public headers compile standalone under C++17/20/23 with `-Wall -Wextra -Werror`.

## Tests

- One `run_case(__func__, "caseN", [&] { ... })` per case; failure is
  `throw std::logic_error("caseN: description")`; every `WEAVE_TEST` ends with `report(__func__)`.
- Use "the waiting side returned" as the signal that the triggering side passed its blocking point.
- Assertion bodies live in `tests/`, separate from backend entry points, shared by both smoke
  payloads. Tests run serially in name order; any failure fails the run.
- `tests/scope_sc_thread_semantics.cpp` (raw `sc_spawn`, non-task `SC_THREAD`) is linked only into
  `weave_smoke_sc`; `tests/svdpi_scope_semantics.cpp` (`svdpi.h` scope switching) only into
  `weave_smoke_svdpi`.
- A body that must die goes into `tests/fatal_semantics.cpp`, never a smoke payload.
- A task body must not capture a stack frame that can die before it finishes: guarantee it finishes
  via `await`/`gather`, or drain the siblings on the exception path.
- Examples: no `run_case`, one final `printf`, event-driven, passing on both backends.

## Code style (before committing)

```bash
clang-format -i <changed .cpp/.hpp>                                        # .clang-format
verible-verilog-format --inplace --flagfile=.verible-format <changed .sv>  # .verible-format
stylua <changed .xmake>                                                    # .stylua.toml
```

- Format only the files you changed.
- Do not run verible on `sv/weave_pkg.sv` (its `->` statements break the parser); keep it manually
  consistent with the same parameters.

## Docs and commits

- Follow schema from [here](https://gist.githubusercontent.com/kpskp77/66866026b906531fa9682c851af83a0f/raw/352e036650bbc31d774a7e62f27ad6c5b3c6bff4/documentation-schema.md)
- `README.md` and `docs/*.md` exist as English + `_zh` twins. Change one, update the other in the same
  commit. The `_zh` version is not a translation: read the English for meaning, then write what a
  Chinese engineer would write — established Chinese technical terms, Chinese sentence order, no
  calque (stacked `的`, long attributive chains, `被`-passives, machine-translation cadence).
- Code, comments, identifiers, test names and commit messages are English.
- Commits are Conventional Commits: `type(scope): subject`, one line, imperative-ish, no body unless
  a *why* is genuinely non-obvious. Types: `feat`, `fix`, `docs`, `chore`, `refactor`, `test`,
  `build`. Scopes: `core`, `task`, `event`, `sync`, `time`, `test`, `svdpi`, `systemc`, `api`,
  `docs`, `sv`, `build`, `release`.
- One topic per commit. Do not commit unless asked.

## Pitfalls in this environment (already hit)

- A triggered event's state lingers: `backend_wait` destroys and recreates `abort_ev` after injecting
  cancellation, and combinators on raw port events keep a waiting flag so a trigger always has a
  waiter.
- The SV wait is a synchronous registry write in `weave_sv_trigger` plus
  `if (w_state[wd] == 0) @(w_evt[wd])` on the waiter's own event. Never `fork … join_any`, never `@ev`
  on an event-table entry, never a level-sensitive `wait (w_state[wd] != 0)`.
- Timeouts only via the persistent timer service; deadlines in a min-heap, never a per-wake `foreach`
  scan, never a per-wait `#delay` leg.
- `sc_main` needs C linkage (see `tests/sc_main.cpp`).
- Run payloads only through `xmake run`; it injects the package `LD_LIBRARY_PATH`. Never add
  `add_rpathdirs` / `after_load` hacks and never run `./simv` or a payload directly.
- `add_requires` only at top-level scope in `xmake.lua`. For set_values options use
  `get_config(name) == "..."`, not `has_config(name, value)`.
- No `-fvisibility=default` / `-rdynamic`. Verify cross-DSO `catch`: `nm -D <exe> | grep _ZTIN5weave`
  must be all `U`.
- Every DPI import in `src/backend/svdpi.cpp` goes through the scope guard (cached
  `svGetScopeFromName("weave_pkg")`, caller's scope restored). Same for new imports.
- `sv::Edge`: the testbench constructs `weave_edge` before the C++ side fires. A leg that creates its
  edge later must wait until the C++ side is parked in `wait()`, ordered by an event or another edge,
  never by a `#delay` assumption. Never destroy a name-table entry; `bound()` is the only probe.
