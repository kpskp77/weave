# Weave

A **behaviour orchestration layer** between C++ test logic and EDA simulation kernels. The API is
modelled after Python asyncio, but does not use C++20 coroutines — blocking semantics are carried
entirely by the backend: every task runs in a simulation process context (an SV process or a
SystemC SC_THREAD), and `sleep`/`wait` freeze the whole C++ call stack through a backend
suspension point.

Weave focuses on concurrency orchestration (tasks, time, events, combinators, timeouts,
cancellation, state queries); it does not provide DUT signal access, bus drivers/monitors, UVM
components, transactions/coverage, TLM modelling, or the simulation kernel itself. Those are
provided by existing tools such as SystemVerilog/UVM/cocotb/SystemC, and Weave plugs C++ test
behaviour into them behind one uniform interface.

## C++ ↔ asyncio mapping

Weave's API semantics **follow asyncio** — the table below is the complete reference; the
differences from asyncio are listed under [known deviations](#known-deviations).

| C++ (weave) | Python (asyncio) | Notes |
|---|---|---|
| `create_task(name, fn)` | `asyncio.create_task(coro)` | fn is a plain function; scheduled, not run immediately |
| `task.await()` | `await task` | FAILED rethrows its exception; CANCELLED throws `CancelledError` |
| `task.cancel()` / `task.cancel(msg)` | `task.cancel(msg)` | Cooperative; msg matches asyncio 3.9+, preserved both on injection and on rethrow at `await` |
| `task.done()` / `task.cancelled()` | `task.done()` / `task.cancelled()` | |
| `task.get_name()` | `task.get_name()` | |
| `sleep(d)` | `await asyncio.sleep(t)` | `sleep(0)` yields for zero time |
| `Event::wait/set/clear` | `asyncio.Event` | set/clear races use edge semantics |
| `Lock::acquire/release/locked` | `asyncio.Lock` | FIFO hand-off; `LockGuard` ≡ `async with lock` |
| `Semaphore::acquire/release/locked` | `asyncio.Semaphore` | Counting semaphore; `SemaphoreGuard` ≡ `async with semaphore` |
| `BoundedSemaphore::acquire/release/locked` | `asyncio.BoundedSemaphore` | Same as Semaphore; releasing past the bound throws `std::logic_error` (≡ `ValueError`) |
| `Barrier::wait/abort/reset/parties/n_waiting/broken` | `asyncio.Barrier` | Fixed-party rendezvous; `wait()` returns this party's arrival index |
| `Queue<T>::put/put_nowait/get/get_nowait` | `asyncio.Queue` | FIFO queue; bounded (`maxsize`) or unbounded; blocking calls queue in FIFO order |
| `Queue<T>::qsize/empty/full/maxsize` | `asyncio.Queue` | State queries; `full()` is always false on an unbounded queue |
| `Queue<T>::join/task_done` | `asyncio.Queue` | "All work processed" barrier; extra `task_done()` throws `std::logic_error` (≡ `ValueError`) |
| `Queue<T>::shutdown(immediate)` | `asyncio.Queue.shutdown` (3.13+) | Closes puts, winds down gets; blocked callers get `QueueShutDown`; `immediate` drains and settles `join` |
| `gather(t1, t2, ...)` | `await asyncio.gather(*aws)` | Default form (throws on first failure) |
| `gather({t1, ...}, return_exceptions)` | `await asyncio.gather(*aws, return_exceptions=True)` | Returns `vector<exception_ptr>`, does not throw early |
| `wait_for(t, d)` | `await asyncio.wait_for(aw, t)` | asyncio 3.12+ timeout semantics |
| `wait({t1, ...}, return_when, timeout)` | `await asyncio.wait(fs, ...)` | Returns `{done, pending}`; does not cancel child tasks |
| `shield(t)` | `asyncio.shield(aw)` | Returns a proxy task; cancellation/timeout stops at the proxy |
| `as_completed({t1, ...})` | `asyncio.as_completed(fs)` | `next()` blocks and yields in completion order |
| `TaskGroup` + `tg.create_task` / `tg.wait()` | `asyncio.TaskGroup` | Structured concurrency; first failure cancels siblings |
| `CancelledError` / `TimeoutError` | `CancelledError` / `TimeoutError` | Names match asyncio incl. the `Error` suffix; hierarchy as documented in [architecture.md](docs/architecture.md) |
| `BrokenBarrierError` | `asyncio.BrokenBarrierError` | Barrier-broken/reset signal; derives from `std::runtime_error` (≡ `RuntimeError`) |
| `QueueEmpty` / `QueueFull` | `asyncio.QueueEmpty` / `asyncio.QueueFull` | Thrown by the `*_nowait` forms; derive from `std::runtime_error` (≡ ordinary `Exception`) |
| `QueueShutDown` | `asyncio.QueueShutDown` (3.13+) | Thrown by put/get on a shut-down queue; derives from `std::runtime_error` (≡ ordinary `Exception`) |
| `WEAVE_TEST` + registry | pytest / assorted runners | No asyncio counterpart; driven by the backend bootstrap entry point |

Further extensions follow asyncio in the same one-to-one way: `LifoQueue` / `PriorityQueue`,
see [roadmap](#roadmap).

### Model differences (versus Python, must read)

Beyond the mapping table there are three structural differences; most misuse stems from these:

- **No coroutine / await**: the combinators are ordinary blocking calls inside a task context, not
  awaitables; `gather(...)` blocks until completion and does not return a future.
- **No event loop object**: scheduling is carried by the simulation kernel. A task created by
  `create_task` starts running only **after the parent task reaches its next blocking point**
  (matching asyncio's scheduling timing).
- **Cancellation never tears down the call stack**: cooperative cancellation plus a ban on killing
  the simulation process means C++ RAII always unwinds normally (unlike thread cancellation).

### Known deviations

- `TaskGroup` has no `ExceptionGroup`: `wait()` rethrows the first failure (in completion order);
- after `gather` returns early, later failures of the remaining tasks stay only in the task
  bookkeeping (there is no Python "exception was never retrieved" warning);
- `Barrier` has no `async with` form (a guard cannot hand back `wait()`'s index — call `wait()`
  explicitly), and `abort()`/`reset()` are plain non-suspending member functions even though
  asyncio spells them `async def`.

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

## Build and run

```bash
xmake smoke      # Semantic self-checks: build smoke payloads + run each weave_smoke_* via xmake run (once per backend)
xmake examples   # Example regression: runs both systemc and svdpi in one go
```

The two have different responsibilities and neither is optional: the assertion bodies in `tests/`
and the programs in `examples/` are never compiled into each other's payloads; examples are built
the way a user would use the library (public headers only, linking `libweave.so`, genuinely
crossing the DSO boundary). Artifacts land in `build/<plat>/<arch>/<mode>/`; without VCS/IES the
SVDPI driver prints SKIP and returns 0 (equivalent to a compile-only check). Backend switching, the
SystemC bootstrap entry point, manual VCS runs, and adding a new backend are covered in
[docs/backends.md](docs/backends.md).

> Linking `libweave.so` into **your own executable** needs **no** RTTI visibility/export flags
> (`-fvisibility=default` / `-rdynamic` are not required): `what()` for both exception types is the
> key function, defined out-of-line in the library, so the typeinfo exists exactly once in the
> library and consumers only keep undefined references (see the "ABI notes" section of backends.md).

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

See the "Language baseline" section of [docs/architecture.md](docs/architecture.md) for details.

## Directory layout

```
include/weave/    Public API: weave.hpp (umbrella) +
                  config/time/task/event/locks/semaphore/barrier/sync/test[/sc]
src/core/         Backend-agnostic core + port.hpp backend contract
src/backend/      Backend adapters (exactly one, selected by macro): svdpi.cpp (default) / systemc.cpp
sv/               SVDPI backend protocol (event table, three-legged wait, spawn, tb skeleton)
examples/         Per-domain demos of the whole public API + one end-to-end case (xmake examples regression)
tests/            Semantic self-check assertion bodies (xmake smoke regression)
docs/             architecture.md / backends.md
```

## Further reading

- [docs/architecture.md](docs/architecture.md): design decisions and implementation constraints (why no coroutines, cancellation, edge semantics, time, language baseline)
- [docs/backends.md](docs/backends.md): DPI protocol, SystemC integration, ABI notes, guide to adding a backend
- [AGENTS.md](AGENTS.md): build/verification conventions and invariants for AI coding assistants

## Roadmap

- **Communication primitives**: `Queue` lives in `include/weave/queue.hpp` (`Lock` / `Condition`
  in `include/weave/locks.hpp`, `Semaphore` in `include/weave/semaphore.hpp`, `Barrier` in
  `include/weave/barrier.hpp`); `LifoQueue` / `PriorityQueue` remain on the asyncio list
- **Backends**: the contract is stable in `src/core/port.hpp`; both SVDPI and SystemC are verified
  on their respective toolchains
