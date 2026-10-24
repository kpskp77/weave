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
core bookkeeping (src/core/) ── task/event/sync/time/registry
        │  only dependency: port.hpp (7 primitives)
        ▼
backend adapters (src/backend/) ── svdpi.cpp (default) / systemc.cpp
        │
        ▼
simulation kernel (VCS/IES/Xcelium, or SystemC)
```

- The core layer may **only** depend on the primitives declared in `src/core/port.hpp`, and must
  never reference DPI / SystemC / thread APIs directly.
- A new backend = implement the port contract + provide a bootstrap entry point, with zero changes
  to core code.
- The only switch for backend selection is a compile-time macro (`include/weave/config.hpp`,
  mutually exclusive, SVDPI by default); xmake's `--backend=` merely defines that macro.

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
   (event/timeout/cancel), with the **cancel leg winning** same-slot conflicts; a negative id
   disables the corresponding leg.

## Key design decisions

### Why not C++20 coroutines

Weave user code runs inside the simulator process, where suspension/resumption is scheduled by the
simulation kernel (when an SV process or a SystemC SC_THREAD suspends, the entire C++ call stack
freezes). Coroutines would force us to build our own scheduler that then disagrees with the
simulation time/event model. A blocking API whose suspension is carried by backend processes is the
most natural fit for a simulation kernel.

### Why cancellation is cooperative

`cancel()` only sets a flag and wakes the task, injecting a `CancelledError` exception at the
blocking point. SV `disable` / `process::kill()` are banned: killing at an arbitrary point tears the
C++ call stack apart, RAII does not unwind, resources leak, and bookkeeping state is corrupted. This
is isomorphic to asyncio's cooperative cancellation (`CancelledError` raised at await points,
catchable and refusable). `cancel(msg)` matches asyncio 3.9+'s `Task.cancel(msg)`: msg is stored in
the task bookkeeping, and the same msg is preserved both in the `CancelledError` injected at the
blocking point and where later `await`/`gather` rethrow it; when a combinator cascade-cancels
sibling tasks it passes an empty msg (msg belongs to the context of whoever initiated the
cancellation).

The type hierarchy of the cancellation signals also follows asyncio: `CancelledError` **deliberately
does not derive from `std::exception`** (matching Python 3.8+, where `CancelledError` is a
`BaseException`), so the idiomatic `catch (const std::exception&)` backstop cannot silently swallow
cancellation — swallowing it would make the task wrongly finish as DONE and break gather / TaskGroup
/ wait_for's judgement of the CANCELLED terminal state; `TimeoutError`, by contrast, **does derive
from `std::exception`** (matching asyncio's `TimeoutError`, an ordinary `Exception`), because a
timeout is a normal failure path that generic logging/backstop code may handle.

### Why Event is edge-triggered, not level-triggered

`set()` snapshots the current waiters and triggers each one-shot wake event; being snapshotted means
you are released, even if a `clear()` follows immediately. The rejected alternative — remember that
`set()` happened and re-check the flag once the waiter resumes — loses the wakeup whenever set/clear
land in the same scheduling step: the waiter resumes, reads `false` and blocks again. asyncio.Event
settles it the same way (it sets the result of the futures that are in `_waiters` at `set()` time),
so the snapshot itself is borrowed rather than invented; the weave-specific part is its price.

The price cannot be avoided: each waiter blocks on its own one-shot backend event id, so on **both**
return paths (success and cancellation) it must first unlink and then destroy that id, otherwise a
later `set()` triggers an already destroyed id. That ordering is the invariant (one
`detail::scope_exit` carries it, with the order written inside the lambda body); its per-code
reasoning stays in the header comment of `src/core/event.cpp`.

### Why Lock hands ownership off inside release()

`release()` hands ownership to the head waiter (FIFO) **within the same non-suspending call**, with
`held` staying true throughout: there is no "released but not yet taken over" window and `locked()`
never briefly turns false in the hand-off gap. asyncio goes the other way, because its hand-off only
completes once the woken waiter is re-scheduled: `release()` wakes the head and *then* lowers
`_locked`, and the waiter raises it again after it resumes — so `locked()` reports `false` for a
while although ownership is already in flight (a newcomer still cannot jump the queue, because the
not-yet-removed future keeps its fast path closed). Handing over synchronously closes that window
instead of documenting it, and because each queued waiter holds a one-shot `Event` as the hand-off
signal, the lock stays composed of `Event` plus the single blocking point, with zero backend changes.

Cancellation must not make the lock vanish, and here the shape does coincide with asyncio: if
cancellation and hand-off land in the same delta (`release()` has just handed the lock to a waiter
that is immediately `cancel()`ed; the backend cancel leg wins), that waiter receives `CancelledError`
rather than the lock inside `acquire()`, and ownership must be handed on to the next waiter (see
`handoff()` in `src/core/locks.cpp`). The cause differs, though: asyncio needs its extra
`_wake_up_first()` because the woken future was already done, whereas here the shape is produced by
port contract guarantee 4 (the cancel leg winning a same-slot conflict).

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

Core internal state lives only in `src/core/*_internal.hpp` (never in public headers, never outside
src/).

## Language baseline: C++17 and up

**C++17 is the floor, not a ceiling.** The library itself is built as C++17 (`set_languages("c++17")`
is the only switch), and every public capability must be complete under C++17 — this is guaranteed by
the regression run. At the same time the public headers must be directly includable by callers using
a **newer standard**: they avoid constructs whose meaning changes across standards or that are
deprecated/removed, so under `-std=c++20` / `c++23` they are as `-Wall -Wextra -Werror` clean as
under C++17.

17 was chosen as the floor because the needed feature set (`std::optional` / `std::string_view` /
`inline` variables / if-init / `[[nodiscard]]` / structured bindings / relaxed `constexpr`) is all
available. **C++20+ features are never required** (concepts, ranges, coroutines, `std::format`,
`std::span`, designated initializers, `std::move_only_function`): partly for the VCS DPI-side host
compiler and the SystemC 3.x toolchain, partly so that whatever `-std=` the caller picks still
compiles; not using coroutines is a design decision (see above).

- **Raising the floor** (e.g. changing `set_languages` to c++20) counts as a cross-backend change: it
  affects both backends and the `-sv_lib` boundary, and requires one `xmake smoke` run under each
  `--backend`.
- **Callers using a newer standard** need no changes and require no build-config changes.
- How it is verified: each public header gets a TU containing only `#include <weave/X.hpp>`, compiled
  once under `-std=c++17` / `c++20` / `c++23` with `-Wall -Wextra -Werror -fsyntax-only` (`sc.hpp`
  additionally needs the SystemC include path and `WEAVE_BACKEND_SYSTEMC`).

Idioms already in use and to be kept by new code: `std::optional` (`CancelledError::msg()`, the
`wait` timeout), `std::string_view`, `[[nodiscard]]` (pure queries and resource acquisition),
`[[maybe_unused]]`, `inline constexpr`, structured bindings, `std::exchange`, `unique_ptr` (backend
event table), `static_assert` + `std::is_invocable_v` / `std::is_nothrow_copy_constructible_v`.

A few boundaries that have been evaluated — do not cross them again:

1. **Owning parameters stay `std::string`**: `create_task(name)`, `TaskGroup::create_task`,
   `Task::cancel(msg)`, `register_test(name)`. `CancelledError::msg_` is a borrowed view pointing at
   `TaskRecord::cancel_msg`, and `what()` relies on `std::string`'s NUL termination. `CancelledError`
   must have no heap members and must copy nothrow (it is thrown across the `-sv_lib` boundary); the
   message-carrying construction goes through exactly one path, `detail::cancelled_with_msg`. **It is
   no longer trivially copyable**: `what()` is the key function, defined out-of-line in the library,
   which keeps the typeinfo unique per library (polymorphic classes are not trivially copyable per
   the standard) — that trait is traded for catch correctness, see the "ABI notes" section of
   [backends.md](backends.md). Likewise, **do not move `CancelledError::what()` /
   `TimeoutError::what()` back to inline definitions**.
2. **`std::optional` never enters the port wire format**: `ev_id < 0`, `timeout_ticks < 0`,
   `abort_id < 0` are the convention with the SV DPI; changing it means touching
   `sv/weave_pkg.sv` and both backends together.
3. **No `if constexpr` without a type-dependent branch**; same for `if-init` — the iterators after
   the `find` in `record()` / `event_by_id()` are needed afterwards, so forcing the idiom in only
   makes it harder to read.
4. **`constexpr` is also used as a compile-time guard**: an out-of-range time literal (`10000_s`) is a
   compile error rather than a silent wrap (`detail::fs_scaled` in `include/weave/time.hpp`); the
   corresponding runtime arithmetic guards (`detail::checked_add/sub/mul/div`) deliberately bound
   via division instead of `__int128`, keeping the public headers free of any compiler extensions.

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
