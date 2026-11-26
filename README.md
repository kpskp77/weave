# Weave

English | [简体中文](docs/README_zh.md)

A **behaviour orchestration layer** between C++ test logic and EDA simulation kernels. The API is
modelled after Python asyncio, but does not use C++20 coroutines — blocking semantics are carried
entirely by the backend: every task runs in a simulation process context (an SV process or a
SystemC SC_THREAD), and `sleep`/`wait` freeze the whole C++ call stack through a backend
suspension point.

Weave focuses on concurrency orchestration (tasks, time, events, combinators, timeouts,
cancellation, state queries); it does not provide DUT signal access, bus drivers/monitors, UVM
components, transactions/coverage, TLM modelling, or the simulation kernel itself. Those are
provided by existing tools such as SystemVerilog/UVM/SystemC, and Weave plugs C++ test
behaviour into them behind one uniform interface.

## C++ ↔ asyncio mapping

Weave's API semantics **follow asyncio** — the table below is the complete reference; the
differences from asyncio are listed under [known deviations](#known-deviations).

| C++ (weave) | Python (asyncio) | Notes |
|---|---|---|
| `create_task(name, fn)` | `asyncio.create_task(coro)` | fn is a plain function; scheduled, not run immediately |
| `task.await()` | `await task` | FAILED rethrows its exception; CANCELLED throws `CancelledError` |
| `task.cancel(msg)` | `task.cancel(msg)` | Cooperative; msg matches asyncio 3.9+, preserved both on injection and on rethrow at `await` |
| `task.done()` / `task.cancelled()` | `task.done()` / `task.cancelled()` | |
| `task.get_name()` | `task.get_name()` | |
| `task.id()` | `id(task)` | Stable, monotonically increasing, never recycled; -1 for an invalid handle |
| `current_task()` | `asyncio.current_task()` | `optional<Task>`; `nullopt` outside a task context (harness chain, done callbacks); non-suspending |
| `sleep(d)` | `await asyncio.sleep(t)` | `sleep(0)` yields for zero time |
| `now()` / `now_ticks()` | `loop.time()` | Absolute simulation time; no suspension, non-decreasing |
| `time_point` + `wait_until(tp)` | `loop.call_at` | `wait_until(now() + d)` ≡ `sleep(d)`; an expired deadline returns without suspending |
| `Event` | `asyncio.Event` | set/clear races use edge semantics; the flag is sticky |
| `Pulse` | — | Pure edge (no asyncio counterpart, so C++ naming): `fire()` wakes only the waiters present right now and keeps no flag; a fire before your `wait()` is *not* remembered; `generation()` lets a caller reconcile missed edges |
| `sv::Edge` | — | Named cross-language edge (no asyncio counterpart, so C++ naming): `sv::Edge e{"clk"}` binds C++ and the SystemVerilog `weave_edge` to one event by name, in either creation order; `fire()`/`wait()` have `Pulse` semantics (a fire before the peer waits is lost); `bound()` tells whether the testbench claimed the name (false on the SystemC backend) |
| `Lock` | `asyncio.Lock` | FIFO hand-off; `LockGuard` ≡ `async with lock` |
| `Semaphore` | `asyncio.Semaphore` | Counting semaphore; `SemaphoreGuard` ≡ `async with semaphore` |
| `BoundedSemaphore` | `asyncio.BoundedSemaphore` | Same as Semaphore; releasing past the bound throws `std::logic_error` (≡ `ValueError`); `BoundedSemaphoreGuard` ≡ `async with bounded_semaphore` |
| `Barrier` | `asyncio.Barrier` | Fixed-party rendezvous |
| `Queue<T>` | `asyncio.Queue` | FIFO queue; bounded (`maxsize`) or unbounded; blocking calls queue in FIFO order |
| `PriorityQueue<T>` / `LifoQueue<T>` | `asyncio.PriorityQueue` / `asyncio.LifoQueue` | Same API as `Queue`; lowest-first (min-heap ≡ heapq) / last-in-first-out |
| `gather(t1, t2, ...)` | `await asyncio.gather(*aws)` | Default form (throws on first failure) |
| `gather({t1, ...}, return_exceptions)` | `await asyncio.gather(*aws, return_exceptions=True)` | Returns `vector<exception_ptr>`, does not throw early |
| `wait_for(t, d)` | `await asyncio.wait_for(aw, t)` | asyncio 3.12+ timeout semantics; cancelling the *waiter* also cancels and lands `t` |
| `timeout(body, d)` | `async with asyncio.timeout(d)` | Bounds a block of work: the body runs as a child task, `TimeoutError` after it is cancelled and landed; the deadline covers the body's whole execution (no coroutine block to arm) |
| `wait({t1, ...}, return_when, timeout)` | `await asyncio.wait(fs, ...)` | Returns `{done, pending}`; does not cancel child tasks |
| `shield(t)` | `asyncio.shield(aw)` | Returns a proxy task; cancellation/timeout stops at the proxy |
| `as_completed({t1, ...})` | `asyncio.as_completed(fs)` | `next()` blocks and yields in completion order |
| `TaskGroup` + `tg.create_task` / `tg.wait()` | `asyncio.TaskGroup` | Structured concurrency; a child failure cancels siblings and is thrown as `ExceptionGroup` after all have landed |
| `CancelledError` | `asyncio.CancelledError` | Cancel signal; does NOT derive from `std::exception`, check more details in [architecture](docs/architecture.md) |
| `ExceptionGroup` | `ExceptionGroup` (raised by `asyncio.TaskGroup`) | Aggregates a `TaskGroup`'s child failures, in completion order; even a single failure is wrapped |
| `TimeoutError` | `TimeoutError` | Timeout signal; derives from `std::exception` |
| `BrokenBarrierError` | `asyncio.BrokenBarrierError` | Barrier-broken/reset signal; derives from `std::runtime_error` (≡ `RuntimeError`) |
| `QueueEmpty` / `QueueFull` / `QueueShutDown` | `asyncio.QueueEmpty` / `asyncio.QueueFull` / `asyncio.QueueShutDown` | Thrown by the `*_nowait` forms; derive from `std::runtime_error` (≡ `RuntimeError`) |
| `weave::fatal(why)` | — | Ends the run (no asyncio counterpart, so C++ naming): prints `weave: fatal: <why> (task N '<name>')`, dumps the wait graph and terminates through the backend's failure channel (`$fatal` / `SC_REPORT_FATAL`); never throws, never returns |
| `WEAVE_TEST` + registry | pytest / assorted runners | No asyncio counterpart; driven by the backend bootstrap entry point |

### Model differences (versus Python, must read)

Beyond the mapping table there are five structural differences; most misuse stems from these:

- **Contract violations end the run**: the violations the core can prove are unrecoverable — a
  diagnosed wait cycle, a blocking call in a process that runs no task, a suspension after a body
  returned — terminate the simulation (`$fatal` / `SC_REPORT_FATAL`) after printing
  `weave: fatal: <what> (site <file:line>, task N '<name>')` and dumping the wait graph. They are
  deliberately not exceptions: a `catch (...)` anywhere would turn a guaranteed hang into a run that
  merely looks slow, and at an ownerless site the unwind would go straight into the simulator
  through the consumer's own DPI export. Misuse the caller can recover from (`Lock`/`Semaphore`
  protocol errors, combinator misuse) still throws `std::logic_error`.
- **No coroutine / await**: the combinators are ordinary blocking calls, not awaitables, and need a
  task context (outside one they throw `std::logic_error`). Nothing returns a future: the default
  `gather(...)` blocks until the tasks land or the first failure/cancellation and returns nothing.
- **No event loop object**: scheduling is carried by the simulation kernel. A task created by
  `create_task` starts running only **after the parent reaches its next real suspension point (or
  finishes)**, matching asyncio's `create_task` timing.
- **Cancellation never tears down the call stack**: `cancel()` only arms a flag and wakes the task,
  and `CancelledError` is injected at the blocking point, so the stack unwinds through ordinary
  exception propagation and RAII always runs; killing a simulation process (`process::kill()`, SV
  `disable`) is banned, so nothing is ever torn down asynchronously.
- **Code that suspends outside weave is not cancellable there**: `cancel()` lands only at weave's own
  blocking points, so a body blocked in another DPI export, an SV wait or a third-party blocking call
  keeps running (state `RUNNING`, no wait-graph node), and `wait_for` / `TaskGroup` teardown can only
  return once it does. Route such a region through `shield` (or convert the notification into an
  `Event` / `sv::Edge`); outside a task body only leaf waits may suspend. See
  [architecture](docs/architecture.md#suspension-outside-weave-foreign-blocking-and-ownerless-waits).

### Known deviations

- `Barrier` has no `async with` form: a guard cannot hand `wait()`'s index back to the body, which
  Python's `async with barrier` can (it returns `wait()`'s index), so call `wait()` explicitly;
  `abort()`/`reset()` are plain non-suspending member functions even though asyncio spells them
  `async def`.

## Quick start

```cpp
#include <weave/weave.hpp>
using namespace weave;
using namespace weave::literals;

WEAVE_TEST(my_test) {
    Event ev;
    auto t1 = create_task("producer", [&] { sleep(100_ns); ev.set(); });
    auto t2 = create_task("consumer", [&] { ev.wait(); /* ... */ });
    wait_for(t1, 1_us);          // On timeout cancels the task and throws weave::TimeoutError
    gather(t1, t2);
}
```

### Diagnosing a hang

A deadlock never times out, so the last resort is `weave::debug`: `debug::snapshot()` returns the
task wait graph — which backend event each live task is blocked on, and for how long in **simulation**
time — and `debug::dump(tag)` logs it through simlog (`weave.debug`, info level), one line per
task. Neither suspends nor changes state, so both are callable from a done callback or the harness
chain, and the dump shares the core trace's stream, sim-time stamp and `SIMLOG_LEVEL` switch
(`SIMLOG_LEVEL="weave*=debug"` for the lifecycle trace). A snapshot describes a hang;
it does not prove a cycle (any party may `set()` an `Event`, so "who should wake this" is not
recorded anywhere). See [architecture](docs/architecture.md#deadlock-diagnostics-only-the-edges-the-core-can-see).

When the snapshot settles the question — or when a test's own retry loop cannot converge — end the
run with `weave::fatal(why)`. It takes the same path as the core's own violations: the
`weave: fatal: <why> (task N '<name>')` headline through simlog, the wait-graph dump, the flush, and
the backend's failure channel. Prefer it to the alternatives: a `throw` can be caught above the call
site (or inside a combinator) and leaves a run that merely looks slow — and then green, since no
task ever reaches FAILED; `std::abort()` skips the flush and never reaches the simulator's
transcript; and `$fatal` / `SC_REPORT_FATAL` are backend-specific, so portable test code cannot use
them without `#ifdef`.

## Dependencies and xmake integration

weave and every library it depends on are published as xmake packages in one repository,
[`kpskp77/xmake-repo`](https://github.com/kpskp77/xmake-repo):

| Package | What it provides | Pulled in by |
|---|---|---|
| `weave` | `include/weave/*.hpp` and `libweave.so` | you |
| `simlog` | the logger behind every diagnostic and the optional trace, stamped with the simulator clock | `weave` — a public link, so depending on weave is enough |
| `svdpi` | the SystemVerilog DPI-C headers | `weave` with `backend = "svdpi"` (the default) |
| `systemc` | the Accellera SystemC class library | `weave` with `backend = "systemc"` |

Register the repository once — globally, or per project inside `xmake.lua`:

```console
$ xrepo add-repo eda https://github.com:kpskp77/xmake-repo.git
```

```lua
add_repositories("eda https://github.com:kpskp77/xmake-repo.git")
```

Then require weave; the `backend` config picks the kernel it is built against and pulls the matching
package in:

```lua
add_requires("weave 0.1.0", { configs = { backend = "svdpi" } })  -- or backend = "systemc"

target("my_test")
    set_languages("c++17")
    set_kind("shared")          -- SVDPI: the simulator loads the target with -sv_lib
    add_files("src/*.cpp")
    add_packages("weave")
```

`add_packages("weave")` supplies the include directories, the library itself and the transitive
links. Requirements: xmake >= 2.8, a C++17 compiler, and CMake, which the package recipes build
through.

Two things the package does not cover:

- **The SystemVerilog side of the SVDPI backend.** `sv/weave_pkg.sv` ships with the weave source
  tree, not with the package (which installs headers and the library only): compile it next to your
  testbench, `import weave_pkg::*;` there, and call `weave_run(status)` to bootstrap the run.
- **The runtime library path.** `simlog` (and `systemc`) are shared, so anything that loads
  `libweave.so` — `simv` included — has to find them: start it through `xmake run`, which puts the
  package directories on `LD_LIBRARY_PATH`, or export those directories yourself.

Per-backend integration details, `sc_main` included: [backends](docs/backends.md).

## Build and run

```bash
just test-all     # self-checks: build smoke payloads, run each weave_smoke_* via xmake run, once per backend
just example-all  # example regression, both backends
```

The two have different responsibilities and neither is optional: the assertion bodies in `tests/`
and the programs in `examples/` are never compiled into each other's payloads; examples are built
the way a user would use the library (public headers only, linking `libweave.so`, genuinely
crossing the DSO boundary). Artifacts land in `build/<plat>/<arch>/<mode>/`; without VCS/IES the
SVDPI runner script prints SKIP and returns 0 (equivalent to a compile-only check). Backend
switching, the
SystemC bootstrap entry point, and adding a new backend are covered in [backends](docs/backends.md).

## Language standard: C++17 and up

The whole repository uses **C++17 as the minimum standard** (`set_languages("c++17")` in
`xmake.lua` is the only switch): the library itself is built as C++17, and its public capabilities
must be complete under C++17. **Newer standards are supported as well** — callers may choose
`-std=c++20` / `c++23` / `c++2c` for their own translation units, and the public headers are
verified to compile cleanly under C++17/20/23.

- **17 is complete**: no public header treats any C++20+ feature as required
  (`CancelledError::msg()` is a `std::optional<std::string_view>`, the `wait` timeout is a
  `std::optional<duration>`).
- **Newer standards work too**: the public headers avoid constructs whose meaning changes by
  standard version or that are deprecated/removed, so bumping `-std=` introduces no warnings and no
  behavioural change.
- **Not using C++20 coroutines is a design decision, not a standard limitation**: suspension is
  carried by the simulation kernel, independently of the standard version.

## Directory layout

```
include/weave/    Public API: weave.hpp (umbrella) + time/task/event/locks/semaphore/barrier/sync/test[/sc]
src/core/         Backend-agnostic core + port.hpp backend contract + config.hpp (internal macro)
src/backend/      Backend adapters (exactly one, selected by macro): svdpi.cpp (default) / systemc.cpp
sv/               SVDPI backend protocol, SV side (event table, three-legged wait, spawn)
examples/         Per-domain demos of the whole public API + one end-to-end case (just example-all regression)
tests/            Semantic self-check assertion bodies + the VCS runner script (tests/run-eda.sh)
                  with its SV testbench top (just test-all regression)
docs/             architecture.md / backends.md and Chinese version of README_zh.md / architecture_zh.md / backends_zh.md
```

## Further reading

- [architecture](docs/architecture.md): design decisions and implementation constraints (why no coroutines, cancellation, edge semantics, time)
- [backends](docs/backends.md): DPI protocol, SystemC integration, ABI notes, guide to adding a backend
