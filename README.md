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
| `TaskGroup` + `tg.create_task` / `tg.wait()` | `asyncio.TaskGroup` | Structured concurrency; first failure cancels siblings |
| `CancelledError` | `asyncio.CancelledError` | Cancel signal; does NOT derive from `std::exception`, check more details in [architecture](docs/architecture.md) |
| `DeadlockError` | — | No asyncio counterpart (Python raises `RuntimeError` for join-on-self): a diagnosable wait cycle — self-await, a `TaskGroup` child waiting on its own group. Derives from `std::logic_error` |
| `TimeoutError` | `TimeoutError` | Timeout signal; derives from `std::exception` |
| `BrokenBarrierError` | `asyncio.BrokenBarrierError` | Barrier-broken/reset signal; derives from `std::runtime_error` (≡ `RuntimeError`) |
| `QueueEmpty` / `QueueFull` / `QueueShutDown` | `asyncio.QueueEmpty` / `asyncio.QueueFull` / `asyncio.QueueShutDown` | Thrown by the `*_nowait` forms; derive from `std::runtime_error` (≡ `RuntimeError`) |
| `WEAVE_TEST` + registry | pytest / assorted runners | No asyncio counterpart; driven by the backend bootstrap entry point |

### Model differences (versus Python, must read)

Beyond the mapping table there are three structural differences; most misuse stems from these:

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

### Known deviations

- `TaskGroup` has no `ExceptionGroup`: `wait()` rethrows the first failure — the first FAILED in
  completion order while it still waits (children that had already landed before the `wait()` call
  are replayed in creation order);
- after `gather` returns early, later failures of the remaining tasks are reported nowhere: they
  stay retrievable through the task handle, and there is no "exception was never retrieved"
  diagnostic at all (asyncio suppresses it here too — `gather`'s `_done_callback` marks the
  leftovers retrieved — but does warn about a task nobody ever awaits);
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

## Build and run

```bash
just smoke      # Semantic self-checks: build smoke payloads + run each weave_smoke_* via xmake run (once per backend)
just examples   # Example regression: runs both systemc and svdpi in one go
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
include/weave/    Public API: weave.hpp (umbrella) +
                  time/task/event/locks/semaphore/barrier/sync/test[/sc]
src/core/         Backend-agnostic core + port.hpp backend contract + config.hpp (internal macro)
src/backend/      Backend adapters (exactly one, selected by macro): svdpi.cpp (default) / systemc.cpp
sv/               SVDPI backend protocol, SV side (event table, three-legged wait, spawn)
examples/         Per-domain demos of the whole public API + one end-to-end case (just examples regression)
tests/            Semantic self-check assertion bodies + the VCS runner script (tests/svdpi/run.sh)
                  with its SV testbench top (just smoke regression)
docs/             architecture.md / backends.md and Chinese version of README_zh.md / architecture_zh.md / backends_zh.md
```

## Further reading

- [architecture](docs/architecture.md): design decisions and implementation constraints (why no coroutines, cancellation, edge semantics, time)
- [backends](docs/backends.md): DPI protocol, SystemC integration, ABI notes, guide to adding a backend
