// Combinators: gather (both forms) / wait_for / wait / shield / as_completed /
// TaskGroup. Depends only on core bookkeeping and the port, hence backend-agnostic.
//
// Convention for raw port events (wait / TaskGroup): trigger is only issued when a waiter really
// exists (guarded by a waiting flag) — a trigger with no waiter leaves an unconsumed wakeup, and
// destroying an event that still has a pending delta notification is undefined behaviour on the
// backend side.

#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include <weave/event.hpp>
#include <weave/sync.hpp>

#include "core_internal.hpp"

namespace weave {

// Out-of-line definition of Timeout::what().
// Keeping the definition in the library (rather than inline in the header) makes it the key
// function of this type: the vtable and typeinfo are emitted exactly once, in this TU, and consumer
// TUs keep only undefined references that the dynamic linker resolves to the same copy. Written
// back inline, typeinfo would degrade into one weak definition per TU, and since libc++abi compares
// type_info by pointer, catch across DSOs would silently mismatch.
const char* Timeout::what() const noexcept {
  return "weave::Timeout";
}

namespace detail {

// Semantics follow asyncio.gather(return_exceptions=False):
//   · any task reaching FAILED/CANCELLED → rethrow its exception immediately, in completion order,
//     without waiting for the others (asyncio does not cancel siblings because of it — that is
//     TaskGroup's semantics)
//   · all complete normally → return
//   · gather itself is cancelled → cancel all unfinished tasks first (asyncio: when gather is
//     cancelled, its unfinished child tasks are cancelled too), then propagate Cancelled
void gather_impl(std::initializer_list<Task> tasks) {
  if (tasks.size() == 0) return;

  struct GatherState {
    long remaining;
    bool failed = false;
    int first_failed_id = -1;  // The first failure, in completion order
    Event wake;                // Set on the first failure or when everything has completed
  };
  auto st = std::make_shared<GatherState>();
  st->remaining = static_cast<long>(tasks.size());

  for (const Task& t : tasks) {
    const int id = t.id_;
    add_done_callback(id, [st, id]() {
      auto& r = record(id);
      if (!st->failed && (r.state == TaskState::FAILED || r.state == TaskState::CANCELLED)) {
        st->failed = true;
        st->first_failed_id = id;
        st->wake.set();  // First failure: wake up immediately, without waiting for the rest
      }
      if (--st->remaining == 0) st->wake.set();
    });
  }

  try {
    st->wake.wait();
  } catch (const Cancelled&) {
    // gather was cancelled: cancel all unfinished tasks and then propagate (asyncio semantics).
    // The cascade-cancelled sibling tasks do not carry the gather caller's msg (that is the parent
    // task's context; the siblings were cancelled alongside it and the msg belongs to whoever
    // initiated the cancellation), so an empty string is passed.
    for (const Task& t : tasks) {
      if (!t.done()) t.cancel();
    }
    throw;
  }
  if (st->failed) dispatch_completed(record(st->first_failed_id));
}

}  // namespace detail

// ── gather: the return_exceptions form ────────────────────────────────────

std::vector<std::exception_ptr> gather(std::initializer_list<Task> tasks, return_exceptions_t) {
  if (tasks.size() == 0) return {};

  struct GatherState {
    long remaining;
    Event wake;  // Set once everything has landed (Event may safely be set repeatedly: waiters
                 // unlink themselves before returning)
    std::vector<std::exception_ptr> results;
  };
  auto st = std::make_shared<GatherState>();
  st->remaining = static_cast<long>(tasks.size());
  st->results.resize(tasks.size());

  size_t i = 0;
  for (const Task& t : tasks) {
    const int id = detail::task_id(t);
    detail::add_done_callback(id, [st, i, id]() {
      auto& r = detail::record(id);
      if (r.state == detail::TaskState::FAILED)
        st->results[i] = r.eptr;
      else if (r.state == detail::TaskState::CANCELLED)
        // CANCELLED mapping for the return_exceptions form: rethrow a Cancelled carrying the
        // original msg, matching how asyncio.gather(*aws, return_exceptions=True) preserves
        // CancelledError(msg)
        st->results[i] = std::make_exception_ptr(detail::cancelled_with_msg(r.cancel_msg));
      if (--st->remaining == 0) st->wake.set();
    });
    ++i;
  }

  try {
    st->wake.wait();
  } catch (const Cancelled&) {
    // gather was cancelled: cancel all unfinished tasks and then propagate (same as the default
    // form)
    for (const Task& t : tasks) {
      if (!t.done()) t.cancel();
    }
    throw;
  }
  return std::move(st->results);
}

// ── wait_for ──────────────────────────────────────────────────────────

void wait_for(const Task& task, duration timeout) {
  auto& r = detail::record(detail::task_id(task));
  if (detail::terminal(r.state)) {
    detail::dispatch_completed(r);
    return;
  }
  const int st = detail::backend_wait(r.done_ev, detail::to_ticks(timeout));
  if (st == detail::WS_EVENT) {
    detail::dispatch_completed(r);
    return;
  }
  // Timeout: cancel and wait for the cancellation to complete (asyncio semantics), then throw
  // Timeout. The cancellation carries a msg stating the reason (matching asyncio 3.12+: the
  // internal cancellation on timeout is tagged with its origin).
  task.cancel("wait_for timeout");
  detail::backend_wait(r.done_ev, -1);
  throw Timeout{};
}

// ── wait（asyncio.wait）────────────────────────────────────────────────

WaitResult wait(std::initializer_list<Task> tasks, ReturnWhen return_when,
                std::optional<duration> timeout) {
  WaitResult res;
  if (tasks.size() == 0) return res;

  struct WaitState {
    ReturnWhen when;
    long remaining;
    int wake_ev = -1;
    bool waiting = false;    // The waiter is blocked on wake_ev (so trigger always has a waiter)
    bool signalled = false;  // The wakeup condition is met (waking once is enough)
  };
  auto st = std::make_shared<WaitState>();
  st->when = return_when;
  st->remaining = static_cast<long>(tasks.size());
  st->wake_ev = port::create_event();

  // Covers both the exception and the normal path. waiting must be cleared before destroying:
  // a late completion triggers only while waiting is true, but a flag left behind as true would
  // trigger an already destroyed id (the cleanup on the catch path would not be reached, hence both
  // things must happen as a pair inside the same lambda).
  auto destroy_wake = detail::on_scope_exit([&st] {
    st->waiting = false;
    port::destroy_event(st->wake_ev);
  });

  for (const Task& t : tasks) {
    const int id = detail::task_id(t);
    detail::add_done_callback(id, [st, id]() {
      if (st->signalled)
        return;  // Waking once is enough; this also blocks late completions after
                 // the waiter has returned
      const auto state = detail::record(id).state;
      --st->remaining;
      // FIRST_EXCEPTION is triggered by FAILED only (CANCELLED does not count, matching CPython)
      if (st->remaining == 0 || st->when == ReturnWhen::FIRST_COMPLETED ||
          (st->when == ReturnWhen::FIRST_EXCEPTION && state == detail::TaskState::FAILED)) {
        st->signalled = true;
        if (st->waiting) port::trigger(st->wake_ev);
      }
    });
  }

  // nullopt = no timeout; a non-positive duration (duration{0} and negative values) is
  // special-cased to a direct snapshot (matching asyncio.wait(timeout=0)'s immediate return),
  // without a zero-time yield — otherwise the result would depend on delta scheduling order and be
  // nondeterministic.
  const bool snapshot_only = timeout.has_value() && timeout->count() <= 0;
  if (!st->signalled && !snapshot_only) {
    st->waiting = true;
    detail::backend_wait(st->wake_ev, timeout.has_value() ? detail::to_ticks(*timeout) : -1);
    st->waiting = false;
  }

  for (const Task& t : tasks) {
    if (t.done())
      res.done.push_back(t);
    else
      res.pending.push_back(t);
  }
  return res;
}

// ── shield (asyncio.shield) ────────────────────────────────────────────

Task shield(Task& inner) {
  // The proxy task waits on inner as-is: Task::await never propagates cancellation to the task
  // being awaited anyway, so cancelling the proxy / a timeout / a cascade cancellation stops at the
  // proxy layer and inner keeps running; the proxy mirrors inner's terminal state and exception
  // through await. Captured by value (a handle is just an id), so no reference to the caller's
  // stack frame.
  return create_task(inner.get_name() + "/shield", [inner]() mutable { inner.await(); });
}

// ── as_completed (asyncio.as_completed) ────────────────────────────────

struct AsCompleted::Impl {
  long total = 0;
  long produced = 0;      // Number already delivered to the consumer
  std::deque<int> queue;  // Completed, awaiting delivery (in completion order)
  int wake_ev = -1;
  bool waiting = false;  // The consumer is blocked on wake_ev (so trigger has a waiter)

  ~Impl() {
    // The shared_ptr held by the callbacks keeps Impl alive until every task has completed; the
    // waiting guard guarantees there is no unconsumed wakeup left at this point.
    if (wake_ev >= 0) port::destroy_event(wake_ev);
  }
};

AsCompleted::AsCompleted(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

bool AsCompleted::pending() const noexcept {
  return impl_->produced < impl_->total;
}

Task AsCompleted::next() {
  auto& st = *impl_;
  for (;;) {
    if (!st.queue.empty()) {
      const int id = st.queue.front();
      st.queue.pop_front();
      ++st.produced;
      return detail::task_from_id(id);
    }
    if (st.produced >= st.total) throw std::logic_error("as_completed: no more tasks");
    st.waiting = true;
    // This must be exception-safe: st is *impl_ and outlives each next() call — if the flag stayed
    // true, a later completion would trigger wake_ev with no waiter.
    auto clear_waiting = detail::on_scope_exit([&st] { st.waiting = false; });
    detail::backend_wait(st.wake_ev, -1);  // Cancellation only interrupts next() itself; the queue
                                           // and the child tasks are unaffected
  }
}

AsCompleted as_completed(std::initializer_list<Task> tasks) {
  auto impl = std::make_shared<AsCompleted::Impl>();
  impl->total = static_cast<long>(tasks.size());
  impl->wake_ev = port::create_event();
  for (const Task& t : tasks) {
    const int id = detail::task_id(t);
    detail::add_done_callback(id, [impl, id]() {
      impl->queue.push_back(id);
      if (impl->waiting) port::trigger(impl->wake_ev);
    });
  }
  return AsCompleted(std::move(impl));
}

// ── TaskGroup (asyncio.TaskGroup) ──────────────────────────────────────

struct TaskGroup::Impl {
  std::vector<Task> children;
  bool exited = false;  // wait() has been called
};

TaskGroup::TaskGroup() : impl_(std::make_shared<Impl>()) {}

Task TaskGroup::create_task(std::string name, std::function<void()> body) {
  if (impl_->exited) throw std::logic_error("TaskGroup: create_task after wait");
  Task t = weave::create_task(std::move(name), std::move(body));
  impl_->children.push_back(t);
  return t;
}

void TaskGroup::cancel(std::string msg) {
  // Only unfinished tasks are cancelled; child tasks already in a terminal state are left alone
  // (matching asyncio.TaskGroup.cancel skipping finished child tasks). Each task is cancelled
  // cooperatively: no blocking, no waiting for landing — landing still happens in wait().
  for (const Task& t : impl_->children) {
    if (!t.done()) t.cancel(msg);
  }
}

void TaskGroup::wait() {
  if (impl_->exited) throw std::logic_error("TaskGroup: wait called twice");
  impl_->exited = true;
  auto& children = impl_->children;
  if (children.empty()) return;

  struct GroupState {
    long remaining;
    bool failed = false;
    int first_failed = -1;  // The first FAILED, in completion order
    int wake_ev = -1;       // Phase one: first failure or everything completed
    int drain_ev = -1;      // Phase two: wait for everything to land after cancelling siblings
    bool waiting = false;   // Blocked on wake_ev
    bool draining = false;  // Blocked on drain_ev
  };
  auto st = std::make_shared<GroupState>();
  st->remaining = static_cast<long>(children.size());
  st->wake_ev = port::create_event();
  st->drain_ev = port::create_event();

  // The guard destroys both raw events on scope exit. There is no need to reset flags inside the
  // guard as wait() does: Cancelled is caught in place and only rethrown after the siblings have
  // drained, so the only things that can escape are invariant violations of current()/record()
  // (already bug paths), and the original implementation did not reset st for those either.
  auto destroy_events = detail::on_scope_exit([&st] {
    port::destroy_event(st->wake_ev);
    port::destroy_event(st->drain_ev);
  });

  for (const Task& t : children) {
    const int id = detail::task_id(t);
    detail::add_done_callback(id, [st, id]() {
      const auto state = detail::record(id).state;
      --st->remaining;
      // CANCELLED tolerance (matching asyncio.TaskGroup's handling of externally cancelled child
      // tasks): only a genuine FAILED triggers sibling cancellation
      const bool first_failure = state == detail::TaskState::FAILED && !st->failed;
      if (first_failure) {
        st->failed = true;
        st->first_failed = id;
      }
      if (st->waiting && (first_failure || st->remaining == 0)) port::trigger(st->wake_ev);
      if (st->draining && st->remaining == 0) port::trigger(st->drain_ev);
    });
  }

  bool cancelled = false;
  if (!st->failed && st->remaining > 0) {  // Block only if the wakeup condition was not met during
                                           // registration
    st->waiting = true;
    try {
      detail::backend_wait(st->wake_ev, -1);
    } catch (const Cancelled&) {
      cancelled = true;  // The waiter was cancelled: cascade-cancel all unfinished child tasks
    }
    st->waiting = false;
  }

  if (cancelled || st->failed) {
    // First FAILED or the waiter was cancelled: cancel all unfinished siblings (matching asyncio
    // TaskGroup). The cascade-cancelled siblings do not carry the caller's msg (msg belongs to the
    // context of whoever initiated the cancellation).
    for (const Task& t : children) {
      if (!t.done()) t.cancel();
    }
    // Only throw after every child task has landed: child tasks may reference objects of this
    // stack frame, and they must never keep running after wait returns. A second cancellation
    // during the drain is swallowed too; the first cancellation/failure is what counts in the
    // end.
    while (st->remaining > 0) {
      st->draining = true;
      try {
        detail::backend_wait(st->drain_ev, -1);
      } catch (const Cancelled&) {
        // Second cancellation: keep waiting for landing
      }
      st->draining = false;
    }
  }

  if (cancelled) {
    // The TaskGroup waiter was cancelled: throw a Cancelled carrying the calling task's current
    // cancel_msg, so that an outer layer awaiting this task sees the outer cancellation reason
    // (matching the propagation of asyncio's CancelledError(msg)).
    throw detail::cancelled_with_msg(detail::current().cancel_msg);
  }
  if (st->failed) detail::dispatch_completed(detail::record(st->first_failed));
}

}  // namespace weave
