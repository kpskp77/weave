# Weave architecture

For people who need to understand or modify the Weave implementation. The operational invariant
checklist lives in [AGENTS.md](../AGENTS.md); backend integration is covered in
[backends.md](backends.md).

## Core/backend separation

Weave has two layers, with a stable port contract in between:

```
C++ test logic (WEAVE_TEST)
        │  public API (include/weave/)
        ▼
core bookkeeping (src/core/) ── task/event/sync/time/registry + locks/queue/semaphore/barrier
        │  only dependency: port.hpp (6 primitives + the weave_task_main dispatch entry)
        ▼
backend adapters (src/backend/) ── svdpi.cpp (default) / systemc.cpp
        │
        ▼
simulation kernel (VCS/IES driven automatically, SystemC locally; another DPI simulator only
                    has to satisfy the same contract — Xcelium is untested)
```

- The core layer may **only** depend on the primitives declared in `src/core/port.hpp`, and must
  never reference DPI / SystemC / thread APIs directly.
- A new backend = implement the port contract + provide a bootstrap entry point, with zero changes
  to core code.
- The only switch for backend selection is a compile-time macro (`include/weave/config.hpp`,
  mutually exclusive, SVDPI by default); xmake's `--backend=` only picks that macro (and, as a
  consequence, which of the two packages is linked).

## The port contract

The header comment of `src/core/port.hpp` is the complete description of the contract; the four
guarantees in summary:

1. **Process model**: `spawn(id)` creates a simulation process that executes `weave_task_main(id)`;
   the child runs only after the parent hits its next suspension point (or finishes) — ≡ asyncio's
   `create_task` "scheduled, not run immediately".
2. **Single active process**: at any instant at most one process executes C++ bookkeeping (the core
   relies on this to maintain global state, so `g_current` and friends need no locks).
3. **One-shot events**: `create/trigger/destroy`; `trigger` must never target an already destroyed
   id.
4. **Single suspension point** `wait(ev, timeout, abort, status)`: a three-legged race
   (event/timeout/cancel), with the **cancel leg winning** same-slot conflicts. `ev_id < 0` drops
   the event leg and `timeout_ticks < 0` drops the timeout leg, but **`abort_id` is always a live
   id**: the SystemC adapter resolves it unconditionally (`event_by_id(abort_id)`), only the SV side
   happens to tolerate a missing one.

## Key design decisions

### Why not C++20 coroutines

Weave user code runs inside the simulator process, where suspension/resumption is scheduled by the
simulation kernel (when an SV process or a SystemC SC_THREAD suspends, the entire C++ call stack
freezes). Coroutines would force us to build our own scheduler that then disagrees with the
simulation time/event model. A blocking API whose suspension is carried by backend processes is the
most natural fit for a simulation kernel.

### Why cancellation is cooperative

`cancel()` only sets a flag and wakes the task, injecting a `CancelledError` exception at the
blocking point. Using SV `disable` / `process::kill()` on a *task* process is banned: killing at an
arbitrary point tears the C++ call stack apart, RAII does not unwind, resources leak, and
bookkeeping state is corrupted. (The single `disable fork` in the backend — the timer service's, see
`sv/weave_pkg.sv` — only disables its own service-local legs and can never reach another waiter's
frame; that is the documented exception, not a loophole.) This is isomorphic to asyncio's
cooperative cancellation (`CancelledError` raised at await points, catchable and refusable).
`cancel(msg)` matches asyncio 3.9+'s `Task.cancel(msg)`: msg is stored in the task bookkeeping, and
the same msg is preserved both in the `CancelledError` injected at the blocking point and where
later `await`/`gather` rethrow it; when a combinator cascade-cancels sibling tasks it passes an
empty msg (msg belongs to the context of whoever initiated the cancellation).

The type hierarchy of the cancellation signals also follows asyncio: `CancelledError` **deliberately
does not derive from `std::exception`** (matching Python 3.8+, where `CancelledError` is a
`BaseException`), so the idiomatic `catch (const std::exception&)` backstop cannot silently swallow
cancellation — swallowing it would make the task wrongly finish as DONE and break gather / TaskGroup
/ wait_for's judgement of the CANCELLED terminal state; `TimeoutError`, by contrast, **does derive
from `std::exception`** (matching asyncio's `TimeoutError`, an ordinary `Exception`), because a
timeout is a normal failure path that generic logging/backstop code may handle.

### Why Event is edge-triggered, not level-triggered

`set()` snapshots the current waiters and triggers each one-shot wake event; being snapshotted means
you are released, even if a `clear()` follows immediately. The flag is still consulted on **entry**
(`wait()` on an already-set event returns without suspending); what is edge-only is the decision
*after* enqueueing. The rejected alternative — remember that `set()` happened and re-check the flag
once the waiter resumes — loses the wakeup whenever set/clear land in the same scheduling step: the
waiter resumes, reads `false` and blocks again. asyncio.Event settles it the same way (it sets the
result of the futures that are in `_waiters` at `set()` time), so the snapshot itself is borrowed
rather than invented; the weave-specific part is its price.

The price cannot be avoided: each waiter blocks on its own one-shot backend event id, so on **both**
return paths (success and cancellation) it must first unlink and then destroy that id, otherwise a
later `set()` triggers an already destroyed id. That ordering is the invariant (one
`detail::scope_exit` carries it, with the order written inside the lambda body); its per-code
reasoning stays in the header comment of `src/core/event.cpp`.

### Why Lock hands ownership off inside release()

`release()` hands ownership to the head waiter (FIFO) **within the same non-suspending call**, with
`held` staying true throughout: there is no "released but not yet taken over" window and `locked()`
never briefly turns false in the hand-off gap. asyncio goes the other way, because its hand-off only
completes once the woken waiter is re-scheduled: `release()` clears `_locked` and wakes the head
(the order of those two statements differs across CPython versions — 3.6 lowers the flag first,
3.12+ wakes first — so neither order should be relied on), and the waiter raises the flag again
only after it resumes — so `locked()` reports `false` for a while although ownership is already in
flight (a newcomer still cannot jump the queue, because the not-yet-removed future keeps its fast
path closed). Handing over synchronously closes that window instead of documenting it, and because
each queued waiter holds a one-shot `Event` as the hand-off signal, the lock stays composed of
`Event` plus the single blocking point, with zero backend changes.

Cancellation must not make the lock vanish, and here the shape does coincide with asyncio: if
cancellation and hand-off land in the same delta (`release()` has just handed the lock to a waiter
that is immediately `cancel()`ed; the backend cancel leg wins), that waiter receives
`CancelledError` rather than the lock inside `acquire()`, and ownership must be handed on to the
next waiter (see `handoff()` in `src/core/locks.cpp`). The cause differs, though: asyncio re-wakes
the queue head in `acquire()`'s `except CancelledError` branch because the future it was waiting on
can already be resolved, whereas here the shape is produced by port contract guarantee 4 (the cancel
leg winning a same-slot conflict).

### Time and precision

Internally int64 femtoseconds (10⁻¹⁵ s, matching SV's maximum precision); values are rounded up by
`ticks_per_sec()` when submitted to the backend — guaranteeing "sleep at least this long" rather
than possibly too short. `sleep(0)` clamps to 0 ticks, which still passes through the single
blocking point: the caller really suspends and hands the scheduling step over, while the simulated
clock does not advance (≡ `asyncio.sleep(0)`). The zero-tick wait is not a no-op that skips the
port — a sleep that returns without suspending would defeat every pure-yield loop (pinned by
`tests/task_semantics.cpp` case10/case11).

**Arithmetic**: `duration` supports `+ - * /` (including `k * d`, `d * k`, `d += …` and other
compound assignments, plus `duration / duration` yielding an integer multiple), so that delays can
be computed from runtime variables (`rounds * 100_ns`). Three constraints:

1. **No silent wrapping on overflow**: same principle as the compile-time guard on out-of-range
   literals — runtime arithmetic overflow throws `std::overflow_error` and division by zero throws
   `std::domain_error`. This is not a matter of style: an int64 femtosecond wrap becomes a negative
   tick, and a negative tick means "no timeout leg" in the port wire format, so letting it through
   would block forever.
2. **Signed quantity, non-positive durations clamped to 0**: `d1 - d2` may be negative
   (differences/offsets are legitimate values), but a non-positive duration handed to `sleep` /
   `wait_for` is always treated as 0 ticks (≡ asyncio's `delay <= 0`, which yields once; `wait_for`
   therefore times out immediately and cancels the task). The clamp point is `to_ticks()`, the sole
   gate through which a negative tick could reach the backend.
3. **Integer scaling only**: no floating-point multiply/divide, because simulation time must be
   reproducible — a floating-point ratio introduces rounding differences on both ends; when a ratio
   is needed, convert in the integer domain first (`n * d / 8`). Floating-point operands are
   **rejected at compile time** by a `= delete`d template (`d * 1.5` would otherwise silently become
   `d * 1` after an implicit conversion to int64_t, differing from the built-in types with no hint
   at all); division by an integer truncates toward zero.

## Header layering

Dependencies point downwards, and every public header must compile standalone:

```
weave/config.hpp    backend selection macro (SVDPI by default)
weave/time.hpp     duration (int64 femtoseconds) + literals
weave/task.hpp     Task / create_task / sleep / CancelledError
weave/event.hpp    Event
weave/locks.hpp    Lock / LockGuard / Condition (communication primitive, built on Event)
weave/queue.hpp    Queue<T> / PriorityQueue<T> / LifoQueue<T> + QueueEmpty / QueueFull /
                  QueueShutDown (communication primitives, built on Event; header-only aliases
                  over detail::QueueBase<T, Store>; the exception key functions live in
                  src/core/queue.cpp)
weave/semaphore.hpp Semaphore / SemaphoreGuard / BoundedSemaphore / BoundedSemaphoreGuard
                  (communication primitive, built on Event)
weave/barrier.hpp Barrier / BrokenBarrierError (communication primitive, built on Condition)
weave/sync.hpp     gather / wait_for / wait / shield / as_completed / TaskGroup
weave/test.hpp     WEAVE_TEST registration
weave/sc.hpp       SystemC backend bootstrap entry point
weave.hpp          umbrella: including this file alone yields the whole public API
```

"Built on X" is an *implementation* dependency, not a header include: `locks.hpp` / `queue.hpp` /
`semaphore.hpp` / `barrier.hpp` pull in no other weave header (only `<memory>`-class dependencies
and `weave/event.hpp` for the queue), because the shared `Event` / `Condition` machinery lives in
their `src/core/*.cpp` TUs. The include chain that does exist points downwards only (`task → time`,
`sync → task + time`, `queue → event`).

Core internal state lives only in `src/core/*_internal.hpp` (never in public headers, never outside
src/).

## Scope

Weave focuses on concurrency orchestration and does not cover all of EDA verification. **Not
provided**: DUT signal-level access; bus drivers/monitors; the UVM component hierarchy; transactions
and coverage; SystemC TLM modelling; the simulation kernel itself; C++ threads and coroutine
scheduling. Those come from existing tools such as SystemVerilog, UVM, cocotb, and SystemC — Weave
plugs C++ test behaviour into them behind a uniform interface.

## Design philosophy for large language models

Weave assumes an LLM is familiar with asyncio patterns, but does not assume it understands hardware
semantics, so the problem is split into two layers: Weave provides a **small and stable**
task/event/sync orchestration interface (see the mapping table in the README), while protocols,
models, and test constraints are supplied by the user and by dedicated verification components. For
AI-assisted verification, Weave offers a simulation control interface that is better suited to
generation and feedback-driven correction, not a capacity to "understand hardware automatically".
