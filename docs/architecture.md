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
        │  only dependency: port.hpp (6 primitives + the non-suspending now_ticks / owner_task
        │  queries + the set_owner_task identity write + the weave_task_main dispatch entry)
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
- The only switch for backend selection is a compile-time macro (`src/core/config.hpp`,
  mutually exclusive, SVDPI by default); the public headers never branch on it, so it is internal
  to the library build; xmake's `--backend=` only picks that macro (and, as a
  consequence, which of the two packages is linked).

## The port contract

The header comment of `src/core/port.hpp` is the complete description of the contract; the four
guarantees in summary:

1. **Process model**: `spawn(id)` creates a simulation process that executes `weave_task_main(id)`;
   the child runs only after the parent hits its next suspension point (or finishes) — ≡ asyncio's
   `create_task` "scheduled, not run immediately".
2. **Single active process**: at any instant at most one process executes C++ bookkeeping, and it is
   never preempted — a task body runs to its next blocking point without interference. Everything a
   body does between two blocking points is therefore atomic with respect to every other task:
   mutating the registry, unlinking a waiter, handing a lock to the queue head or clearing a flag
   needs no `std::mutex` / `std::atomic` / spinlock / `std::thread` (all of which would be
   meaningless inside the simulator and across DPI). The corollary is a publication rule: state that
   must outlive a suspension has to be consistent and published *before* entering `port::wait`,
   because from that instant another process may inspect or mutate it. (The identity of the
   executing task is per process and is kept by the backend, see the identity section of
   [backends](backends.md).)
3. **One-shot events**: `create/trigger/destroy`; `trigger` must never target an already destroyed
   id.
4. **Single suspension point** `wait(ev, timeout, abort, status)`: a three-legged race
   (event/timeout/cancel), with the **cancel leg winning** same-slot conflicts. `ev_id < 0` drops
   the event leg, `timeout_ticks < 0` drops the timeout leg, and `abort_id < 0` drops the cancel leg
   (status then never reports 2) — the shape an ownerless leaf wait uses. A wait with all three legs
   off is rejected.

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
cooperative cancellation (`CancelledError` raised at await points, catchable and refusable — a body
that catches it, cleans up and returns normally ends DONE, because cancellation is a request, not a
kill). A request that never reaches the body still decides the outcome: if the body ends normally
with the flag still set (it hit no blocking point after the cancel — a foreign suspension, see
below), the dispatch entry records CANCELLED instead of DONE, mirroring asyncio's `StopIteration`
branch ("Task is cancelled right before coro stops", the result discarded); an exception the body
raises still wins and leaves it FAILED. `cancel(msg)` matches asyncio 3.9+'s `Task.cancel(msg)`: msg is stored in the task bookkeeping, and
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

### Failure propagation: who cancels whom, and who waits for the landing

The combinators follow asyncio's default rules (no implicit cancellation of siblings), and each is
shaped so that no child can outlive the stack frame it captured.

- **`gather` reports, it does not govern.** The first failure it observes is rethrown, while the
  siblings keep running — cancelling them is `TaskGroup`'s job, not `gather`'s. `gather` cascades a
  cancellation only when `gather` itself is cancelled at its own blocking point, because then the
  caller is unwinding and the children would otherwise keep a dead frame alive.
- **A `wait_for` timeout is not a bare throw.** The timeout leg wins, then the child is cancelled and
  *awaited to landing* before `TimeoutError` leaves the call, so the caller's next statement never
  runs while the child is still executing. That internal cancellation carries a msg naming its
  origin (`"wait_for timeout"`), the asyncio 3.12+ convention that keeps an internal timeout
  distinguishable from an external cancel.
- **A cancelled waiter never leaves its child running.** When the *caller* of `wait_for` is cancelled
  at its blocking point, the awaited task is cascade-cancelled, its landing awaited, and only then
  the original `CancelledError` rethrown — msg preserved, while the cascade carries none (a msg
  belongs to whoever initiated the cancellation). `timeout(body, limit)` is exactly this pair with
  the body spawned as a task: one implementation, so a timeout and a caller cancellation cannot
  disagree.
- **`TaskGroup` is the structured-concurrency counterpart** and therefore does the opposite by
  default: the first *FAILED* child (a CANCELLED one does not count) cancels the unfinished siblings,
  every failure — including those landing during the drain — is aggregated into an `ExceptionGroup`,
  and `wait()` throws only after every child has landed. Its own waiter being cancelled is not a
  failure: the children are taken down the same way and the caller's `CancelledError` (with its msg)
  is what propagates.

Waiting for a landing is bounded by the child actually stopping, not by the timeout, so a
foreign-blocked child stretches `wait_for` / `TaskGroup::wait()` past their deadline — see the next
section for why that is the accepted price.

### Suspension outside weave: foreign blocking and ownerless waits

Weave owns exactly one suspension point, so code leaves that contract in two ways. Both are
**invisible** to the bookkeeping rather than mis-recorded by it:

- **A foreign suspension**: a task body blocked in something weave does not drive — an SV wait
  reached through another `export "DPI-C"`, a `#delay` inside a consumer's import, a third-party or
  HLS call that blocks. The body is simply not running: `TaskRecord::state` stays `RUNNING`,
  `wait_ev` stays -1, and `debug::snapshot()` shows no wait node. It does not lie; it cannot see the
  wait.
- **An ownerless wait**: the calling process runs no task body at all — a testbench process calling
  a library export, `sc_main`, an `SC_THREAD` that is not a task, or a done callback.

**What a foreign suspension costs.** Cancellation is delivered only at a blocking point, so
`cancel()` on a foreign-blocked task is armed and stays armed: it lands when the body reaches its
next weave wait after returning. If the body returns without one, the terminal state is still
CANCELLED — the dispatch entry honours a still-armed flag when the body ends normally (asyncio's
`StopIteration` branch; an exception from the body still wins and leaves it FAILED), so the
bookkeeping at least stops reporting a requested cancellation as a clean finish. What no flag can
shorten is promptness: the body keeps running until it returns on its own, and a
`wait_for` / `TaskGroup::wait()` landing leg attached to it waits exactly that long. Nothing
shortens that without a preemptive kill, which this design bans (see "Why cancellation is
cooperative").

The consequence is narrower than it sounds, and it is worth stating precisely, because the promise
it strains is the *landing* one, not the timeout one: `wait_for(t, d)` reaches `d` honestly, cancels,
then waits for `t` to land, and that second leg is not bounded by `d`; `TaskGroup::wait()` drains the
same way after cancelling the siblings, so it also returns no earlier than they land. (A cancelled
`gather` is the other shape: it only cascades the cancellation and propagates at once — its children
keep running, so it is not held up by a foreign region.) So the shape to avoid is
`wait_for`/`TaskGroup::wait()` on a task that may be foreign-blocked — the timeout keeps its meaning
(d is when the cancellation is issued), but the call returns no earlier than the foreign region does.

**The shape that keeps the contract: wait on a proxy.** `shield` gives the foreign worker a
cancellable front — everyone, combinators included, waits on the proxy, whose body is only
`inner.await()`, a real weave wait with every leg attached. `wait_for` then fires at `d`,
`TaskGroup`/`gather` teardown lands immediately, and the worker keeps running underneath. What
stays outside the contract is confined to the worker: its side effects, its resource lifetime and
its promptness are the caller's business — deliberately, since weave cannot interrupt it and must
not pretend to.

Two cheaper alternatives are preferred when the blocking can be reshaped at all:

- **Convert the notification instead of blocking on it**: let the SV side fire a shared
  `weave_edge` (`weave_edge::fire()`) instead of calling into a blocking export; the C++ side waits
  on `sv::Edge::wait()` (or on an `Event::set()`). The wait is then a weave wait, so cancellation
  and timeouts apply directly and no proxy is needed.
- **Poll cooperatively between chunks**: a body that can break its work into steps may call
  `sleep(0)` between them. It passes through the blocking point, so an armed cancellation is
  injected there and the body stops with ordinary unwinding (at the cost of a scheduling step per
  chunk). The terminal state is CANCELLED under either shape — an armed flag that survives to the
  end is honoured too (see "Why cancellation is cooperative") — so what chunking buys is stopping
  *at a boundary* instead of after the whole region; that is also what bounds the landing leg, so
  the chunk size is the responsiveness you are promising.

**Ownerless waits** are the second tier of the same problem, and `backend_wait` splits them by what
the primitive does with the identity. Leaf waits (`sleep`, `Event::wait`, `Pulse::wait`,
`sv::Edge::wait`) are allowed without one: they become a plain backend wait with no abort leg and no
wait-graph node, losing exactly cancellation and the diagnostic entry. Everything else — every
combinator, `Task::await`, ownership-carrying primitives (`Lock`, `Semaphore`, `Barrier`, `Queue`) —
rejects an ownerless caller with a **fatal**: such a primitive hands a `TaskRecord` to a queue head
or needs the cancel leg, and at an ownerless site an exception would unwind into the consumer's own
DPI export (undefined behaviour) or be caught, leaving a run that still prints its pass marker. The
same fatal covers a suspension *after* the body returned (a done callback), which would attach a
terminal record to the wait graph and could never be cancelled. Identity itself is per process
(see "Related hygiene" under "Deadlock diagnostics"), which is what lets a non-task `SC_THREAD` call
into weave while a task is parked and see `current_task() == nullopt` instead of the parked task's
name.

Honest diagnostics limitation: while a cancelled task is foreign-blocked it is indistinguishable
from a busy one — `RUNNING`, no wait node — so nothing in the public API tells the two apart;
`cancelled()` has an answer only once the body ends, and then it is CANCELLED because the armed flag
is honoured at completion (see "Suspension outside weave").

### Deadlock diagnostics: only the edges the core can see

A wait cycle the bookkeeping can actually see **ends the run** instead of raising: `Task::await()` on
itself (the executing task id is compared with the handle's id), `TaskGroup::wait()` called from
one of the group's own children (the group waits for every child, itself included), and `shield()`
on the running task (the proxy would wait for its caller) go through
`detail::fatal_report` → `port::fatal`, after printing the diagnosis and dumping the wait graph
(see "Why contract violations are fatal" below). All three are cheap single comparisons on data the
core already holds; the `TaskGroup` check runs before the group is sealed and `shield`'s before the
proxy task is created.

What is deliberately **not** diagnosed is a cycle through `Event`: "who will set the event I am
blocked on" is not expressible in the API surface — `Event` is a value object and any party may call
`set()` — so any attempt to build that graph from the core would be guesswork. That class of hang can only be
surveyed, not proved: `debug::snapshot()` (include/weave/debug.hpp) exports the wait graph — the
single blocking point records `TaskRecord::wait_ev` and `wait_since`, so a snapshot can say who is
blocked on which backend event and for how long in simulation time, and retracts both through a guard
so the cancellation throw cannot leave a stale waiter behind — and `debug::dump()` prints it,
letting a human or a failing test decide. The same limit applies to a mutual
`Task::await` cycle (`a` awaits `b` while `b` awaits `a`) and to a cycle built through another
task's shield (`a` shields `b` while `b` awaits `a`): the edge is not recorded anywhere, only the
terminal-state event is.

Related hygiene: the identity of the executing task is kept **per simulation process by the backend**
(`port::owner_task`), not in a core global. A task suspended in its own process therefore cannot
disturb the identity of a process that called into C++ from simulation code — a testbench calling a
library export while another task is frozen used to see whatever task suspended last. The dispatch
entry publishes the identity around the body (`port::set_owner_task`, saving the previous value so a
body nested on the same stack gives it back) and clears it before done callbacks run, so
`current_task()` means "the task executing right now" and returns `nullopt` outside a body. A leaf
wait (`sleep`, `Event::wait`, `Pulse::wait`, `sv::Edge::wait`) is allowed to run with no identity at
all: it becomes a plain backend wait with no cancel leg and no wait-graph node.

### Why contract violations are fatal

Some violations are not failures of the run but failures of its premises, and for those the core must
not rely on the caller: an exception is always catchable, and a swallowed one leaves a run that can
only hang — no `TaskRecord` reaches FAILED, so `root_status()` stays 0 and `WEAVE TEST PASSED`
prints. At an ownerless site the situation is worse: the caller may be a consumer's process reached
through its own `export "DPI-C"`, and unwinding through the simulator into that export is undefined
behaviour weave cannot guard. Type hierarchies do not help here — `catch (...)` exists.

So the port has a fifth primitive, `port::fatal`: `detail::fatal_report` (src/core/task.cpp)
composes the diagnosis, logs it through simlog at `critical` — a fatal diagnosis is a report about
simulation state, so it belongs in the same stream, with the same sim-time stamp, as the rest of the
trace (`WEAVE_SITE` supplies `file:line`):

```
[           0 ps][weave.debug][critical] weave: fatal: a task cannot await itself (site src/core/task.cpp:185, task 2 'fatal_semantics')
```

which is what puts it into the simulator's own transcript (SVDPI's sink prints through `vpi_printf`,
so `simv -l run.log` carries it) and into a consumer's file sinks — a bare `stderr` write would be
missing from either. It then logs the wait graph through `debug::dump("fatal")`, flushes the simlog
sinks (`detail::flush_trace()`) and calls `port::fatal`, which each backend maps onto its own failure
channel: SVDPI calls `weave_sv_fatal` in `sv/weave_pkg.sv` (`$fatal(1, …)`, under the usual DPI scope
guard), SystemC reports `SC_REPORT_FATAL("weave", …)`. That second channel carries the same headline
again, so the diagnosis survives even a `SIMLOG_LEVEL` that silences every weave sink — and it is why
a run cannot be mistaken for a pass either way. Nothing unwinds; the simulation ends with a non-zero
status. The regression pins exactly that: `tests/fatal_semantics.cpp`
(a deliberate self-await) is compiled into its own payload pair and judged with **inverted**
expectation by `tests/expect_fatal.sh` and `tests/run-eda.sh --expect-fatal` — the headline must be
present, the pass marker must be absent.

The five fatal sites: a diagnosed wait cycle (`Task::await()` on itself, `TaskGroup::wait()` from one
of the group's own children, `shield()` on the running task), a blocking primitive in a process
running no weave task, and a suspension after the body returned (a done callback). Everything else that a caller can recover from
— `Lock`/`Semaphore` protocol errors, `as_completed`/`TaskGroup` misuse, an invalid task handle — is
thrown before any state is mutated and stays an ordinary `std::logic_error`.

There is one run-ending primitive, not two: `detail::fatal_report(site, what)` (src/core/task.cpp) is
the whole pipeline, and `site` says whose verdict it carries — `WEAVE_SITE` for a broken premise of
the core, `nullptr` for the public `weave::fatal(why)` (include/weave/debug.hpp), which supplies the
caller's own wording. A forwarder per flow would have added nothing but a name: the two paths differ
in one argument, and because a consumer's verdict is not a broken premise of weave it must not
borrow the core's phrasing — hence the elided `site` clause. The entry exists because every
alternative is wrong in a way the caller cannot repair: `throw` is catchable above the call site —
including by a combinator, which reproduces the silent-green failure above; `std::abort()` skips the simlog flush and never
reaches the simulator's transcript; and `$fatal` / `SC_REPORT_FATAL` are backend-specific, so a test
that wants to give up portably cannot use them without `#ifdef`. The report keeps the same
`weave: fatal:` marker, so one grep rule (`tests/expect_fatal.sh`) covers both paths, and the
caller's text is truncated into the fixed report buffer instead of allocating on the way to the
kernel. `tests/fatal_user_semantics.cpp` pins it with the same inverted judgement as the core
sites: headline with the caller's wording and identity, no pass marker, non-zero exit.

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

That cleanup is held by `detail::on_scope_exit`, a guard whose destructors neither throw nor suspend
— a guard that could suspend would turn cleanup into a second blocking point, i.e. a second place to
be cancelled inside an unwind. How much the guard covers depends on whether a `CancelledError` can
escape the frame: `Event::wait()` / `Pulse::wait()` throw out of the function, so the flag reset and
the destruction are one pair inside a single guard (the asyncio.wait combinator in
`src/core/sync.cpp` has the same pair), while `TaskGroup::wait()` catches the cancellation in place
and rethrows only after its children have drained — nothing can escape between those steps, so only
the event destruction needs guarding.

**`Pulse` is the pure-edge variant of the same carrier** — identical wake-event bookkeeping, no
flag. It exists because a SystemVerilog `-> ev` has exactly those semantics, and feeding it into the
sticky `Event` produces a silent false positive: a waiter that starts after the pulse consumes an
edge that was never meant for it. `Pulse::generation()` is the escape hatch for a caller that must
know whether an edge went by while it was not waiting; the primitive cannot tell it, and buffering one
edge for it would be a different primitive. Both share the unlink-before-destroy invariant above —
`src/core/event.cpp` states it once and `Pulse::wait()` repeats it in the same shape.

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
than possibly too short. `now()` goes the other way: it asks the backend for its current tick count
(`port::now_ticks`, one of the contract's two non-suspending queries) and converts with the same tick
rate, so `wait_until(now() + d)` and `sleep(d)` land on the same tick. Reading the clock never
advances it (a simulator does not move between two suspension points) and the core clamps the value
monotonically: a caller measuring an idle interval cannot observe time going backwards. `sleep(0)` clamps to 0 ticks, which still passes through the single
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
weave/time.hpp     duration (int64 femtoseconds) + literals
weave/task.hpp     Task / create_task / sleep / CancelledError
weave/event.hpp    Event / Pulse
weave/sv.hpp       sv::Edge (named cross-language edge; no dependencies)
weave/debug.hpp    debug::snapshot / debug::dump / debug::take_unretrieved_failures
weave/locks.hpp    Lock / LockGuard / Condition (communication primitive, built on Event)
weave/queue.hpp    Queue<T> / PriorityQueue<T> / LifoQueue<T> + QueueEmpty / QueueFull /
                  QueueShutDown (communication primitives, built on Event; header-only aliases
                  over detail::QueueBase<T, Store>; the exception key functions live in
                  src/core/queue.cpp)
weave/semaphore.hpp Semaphore / SemaphoreGuard / BoundedSemaphore / BoundedSemaphoreGuard
                  (communication primitive, built on Event)
weave/barrier.hpp Barrier / BrokenBarrierError (communication primitive, built on Condition)
weave/sync.hpp     gather / wait_for / wait / shield / as_completed / TaskGroup +
                  ExceptionGroup (combinators and structured concurrency)
weave/test.hpp     WEAVE_TEST registration
weave/sc.hpp       SystemC backend bootstrap entry point
weave.hpp          umbrella: including this file alone yields the whole public API
```

"Built on X" is an *implementation* dependency, not a header include: `locks.hpp` / `queue.hpp` /
`semaphore.hpp` / `barrier.hpp` pull in no other weave header (only `<memory>`-class dependencies
and `weave/event.hpp` for the queue), because the shared `Event` / `Condition` machinery lives in
their `src/core/*.cpp` TUs. The include chain that does exist points downwards only (`task → time`,
`sync → task + time`, `queue → event`, `debug → task + time`, `sv → nothing`).

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
