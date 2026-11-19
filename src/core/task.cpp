// Task bookkeeping: TaskRecord storage, the single blocking point, the task dispatch entry point.
// Backend-agnostic — every backend interaction goes through the port contract (port.hpp).

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <weave/task.hpp>

#include "core_internal.hpp"
#include "trace.hpp"

namespace weave {
namespace detail {

std::deque<TaskRecord> g_tasks;
int g_root = -1;

TaskRecord& record(int id) {
  // Index = id - 1: ids increase monotonically from 1 and are never recycled, so a direct
  // subscript suffices, with no hash lookup
  if (id < 1 || static_cast<std::size_t>(id) > g_tasks.size())
    throw std::logic_error("weave: invalid task handle");
  return g_tasks[static_cast<std::size_t>(id - 1)];
}

int current_id() noexcept {
  // port::owner_task reports the process's task, or 0 when the process runs none; 0 is not a task
  // id (ids start at 1). An out-of-range id is unreachable (an id is only published while its
  // record lives, and records are never destroyed) — map it to "no task" rather than throwing,
  // because "never throws" is part of this query's contract.
  const int id = port::owner_task();
  return id >= 1 && static_cast<std::size_t>(id) <= g_tasks.size() ? id : -1;
}

TaskRecord& current() {
  const int id = current_id();
  if (id < 0) throw std::logic_error("weave: blocking primitive outside task context");
  return g_tasks[static_cast<std::size_t>(id - 1)];
}

int backend_wait(int ev_id, std::int64_t timeout_ticks, Ownerless ownerless) {
  const int id = current_id();
  if (id < 0) {
    if (ownerless != Ownerless::Allow)
      throw std::logic_error("weave: blocking primitive outside task context");
    // Ownerless leaf wait: the process runs no task body (a testbench process, a backend bootstrap
    // callback), so there is no TaskRecord to mark, no wait-graph node to publish and nothing that
    // could cancel the wait — the abort leg is left out instead of attached to some task's event.
    int status = 0;
    port::wait(ev_id, timeout_ticks, -1, &status);
    return status;
  }
  TaskRecord& me = g_tasks[static_cast<std::size_t>(id - 1)];
  if (terminal(me.state))
    // The body already returned and the done callbacks are running (inside weave_task_main, after
    // the identity has been cleared for the outer context): a suspension here would attach a
    // terminal record to the wait graph and could never be cancelled.
    throw std::logic_error("weave: blocking primitive outside task context");
  if (me.cancel_armed) {  // A cancellation issued while suspended is injected at the next
                          // blocking point
    me.cancel_armed = false;
    trace_task()->debug("task {} '{}': CancelledError injected at blocking point ({})", id, me.name,
                        me.cancel_msg.empty() ? "no reason" : me.cancel_msg);
    throw cancelled_with_msg(me.cancel_msg);
  }
  me.state = TaskState::WAITING;
  trace_task()->trace("task {} '{}': suspend on ev={} timeout={}", id, me.name, ev_id,
                      timeout_ticks < 0 ? "none" : std::to_string(timeout_ticks));
  // Publish the suspension for the wait-graph diagnostics (debug::snapshot), and retract it on
  // every way out of this function — the cancellation throw included, which is why it is a guard
  // rather than a statement after port::wait.
  me.wait_ev = ev_id;
  me.wait_since = weave::now_ticks();
  auto retract = on_scope_exit([&me] { me.wait_ev = -1; });
  // No identity juggling around the suspension: the identity is owned by the backend per process,
  // so a slice of another task (in its own process) cannot overwrite it, and this process is parked
  // here — its entry keeps naming the task that is stopped. The port's single-active-process
  // guarantee is what makes the read at the top of this function and the wait below one consistent
  // observation.
  int status = 0;
  port::wait(ev_id, timeout_ticks, me.abort_ev, &status);  // ← the only suspension point
  me.state = TaskState::RUNNING;
  if (status == WS_ABORTED) {
    me.cancel_armed = false;
    // Recreate the abort event: the "triggered" state of an already triggered event lingers on the
    // backend (SystemC compares via change_stamp, SV via a same-slot triggered flag), so a task
    // that catches CancelledError and blocks on the same abort_ev again would be misdetected as a
    // cancellation hit.
    port::destroy_event(me.abort_ev);
    me.abort_ev = port::create_event();
    trace_task()->debug("task {} '{}': woken by cancellation, injecting CancelledError ({})", id,
                        me.name, me.cancel_msg.empty() ? "no reason" : me.cancel_msg);
    throw cancelled_with_msg(me.cancel_msg);
  }
  trace_task()->trace("task {} '{}': resumed by {}", id, me.name,
                      status == WS_TIMEOUT ? "timeout" : "event");
  return status;
}

void dispatch_completed(TaskRecord& r) {
  if (r.state == TaskState::FAILED) std::rethrow_exception(r.eptr);
  // Rethrow a CancelledError carrying the original msg for the CANCELLED terminal state (as in
  // asyncio: whoever awaits a cancelled task receives the same CancelledError(msg))
  if (r.state == TaskState::CANCELLED) throw cancelled_with_msg(r.cancel_msg);
}

void add_done_callback(int id, std::function<void()> cb) {
  auto& r = record(id);
  if (terminal(r.state))
    cb();  // Already terminal: call immediately
  else
    r.done_callbacks.push_back(std::move(cb));
}

int task_id(const Task& t) {
  return t.id_;
}

Task task_from_id(int id) {
  return Task(id);
}

}  // namespace detail

// ── Task ──────────────────────────────────────────────────────────────

void Task::await() {
  // The only deadlock edge the core can see: a task waiting for itself. Without the check this is
  // an eternal block (the task can never reach its own terminal state), which no timer can break
  // and no diagnostics can attribute.
  if (id_ == detail::current_id()) throw DeadlockError("weave: a task cannot await itself");
  auto& r = detail::record(id_);
  if (!detail::terminal(r.state)) detail::backend_wait(r.done_ev, -1);
  detail::dispatch_completed(r);
}

bool Task::done() const {
  return detail::terminal(detail::record(id_).state);
}

bool Task::cancelled() const {
  return detail::record(id_).state == detail::TaskState::CANCELLED;
}

void Task::cancel(std::string msg) const {
  auto& r = detail::record(id_);
  if (!r.cancel_armed)
    r.cancel_msg = std::move(msg);  // The first canceller's msg wins (as in
                                    // asyncio)
  switch (r.state) {
    case detail::TaskState::PENDING:  // The body never runs (intercepted at the task entry)
    case detail::TaskState::RUNNING:  // Injected at the next blocking point
      r.cancel_armed = true;
      detail::trace_task()->debug("task {} '{}': cancellation armed ({})", id_, r.name,
                                  r.cancel_msg.empty() ? "no reason" : r.cancel_msg);
      break;
    case detail::TaskState::WAITING:  // Woken immediately and injected at the blocking point
      r.cancel_armed = true;
      port::trigger(r.abort_ev);
      detail::trace_task()->debug("task {} '{}': cancellation armed, abort event triggered ({})",
                                  id_, r.name, r.cancel_msg.empty() ? "no reason" : r.cancel_msg);
      break;
    default:
      break;
  }
}

const std::string& Task::get_name() const {
  return detail::record(id_).name;
}

Task create_task(std::string name, std::function<void()> body) {
  // The id is derived from the bookkeeping size (starting at 1), guaranteeing index = id - 1 stays
  // aligned even if any step below throws (with a separate counter, a failed create_event would
  // needlessly consume an id).
  const int id = static_cast<int>(detail::g_tasks.size()) + 1;
  detail::TaskRecord r;
  r.name = std::move(name);
  r.body = std::move(body);
  r.abort_ev = port::create_event();
  r.done_ev = port::create_event();
  detail::g_tasks.push_back(std::move(r));  // Index = id - 1
  port::spawn(id);
  detail::trace_task()->debug("task {} '{}': spawned", id, detail::record(id).name);
  return Task(id);
}

void sleep(duration t) {
  // A leaf wait: usable outside a task context too (a testbench process has nothing to cancel and
  // no wait-graph node to publish).
  detail::backend_wait(-1, detail::to_ticks(t), detail::Ownerless::Allow);
}

std::optional<Task> current_task() noexcept {
  // record()/task_from_id() throw on an id that is out of range; the identity is only set to a live
  // id, so this is unreachable, but "never throws" is part of the contract here.
  try {
    const int id = detail::current_id();
    if (id < 0) return std::nullopt;
    return detail::task_from_id(id);
  } catch (...) {
    return std::nullopt;
  }
}

// Out-of-line definition of CancelledError::what().
// This definition (not the declaration) lives in the library and is the key function of the type:
// the vtable and typeinfo are emitted only in this TU, and downstream TUs keep undefined
// references that the dynamic linker resolves to the same copy. The message view always comes from
// std::string::c_str(), so data() is guaranteed NUL-terminated and can be returned directly as a C
// string.
const char* CancelledError::what() const noexcept {
  return msg_ ? msg_->data() : "weave::CancelledError";
}

// Out-of-line destructor: the key function of DeadlockError, so its vtable/typeinfo are emitted
// only here (cross-DSO catch, same reason as CancelledError::what() above). The message comes from
// the inherited std::logic_error, so there is nothing else to define.
DeadlockError::~DeadlockError() = default;

namespace detail {

CancelledError cancelled_with_msg(std::string_view msg) noexcept {
  CancelledError c;
  // msg is a view of TaskRecord::cancel_msg, owned by the bookkeeping; the view is valid only for
  // this throw and the catches along the way (a TaskRecord is not destroyed before its task is
  // terminal). An empty string counts as no msg (as in asyncio's None), and CancelledError::msg()
  // returns nullopt.
  if (!msg.empty()) c.msg_ = msg;
  return c;
}

}  // namespace detail

}  // namespace weave

// ── Task dispatch entry (called by each backend's processes) ────────────────────────────────────

extern "C" void weave_task_main(int task_id) {
  auto& r = weave::detail::record(task_id);
  // The identity is the backend's, per simulation process (port::owner_task), so it is published
  // and restored here — through the port, because only the backend can write it. Saving the
  // previous value is what makes a body nested on this same stack (a harness calling
  // weave_task_main from a cross-language callback) give the identity back to its caller; a slice
  // of a *different* task lives in its own process and cannot reach this value at all.
  const int saved_owner = weave::port::owner_task();
  weave::port::set_owner_task(task_id);
  if (r.cancel_armed) {
    // The cancellation happened before the body was first scheduled: the body never runs
    r.cancel_armed = false;
    r.state = weave::detail::TaskState::CANCELLED;
    weave::detail::trace_task()->debug("task {} '{}': cancelled before first run", task_id, r.name);
  } else {
    r.state = weave::detail::TaskState::RUNNING;
    weave::detail::trace_task()->trace("task {} '{}': running", task_id, r.name);
    try {
      r.body();
      r.state = weave::detail::TaskState::DONE;  // Swallowing CancelledError and returning → DONE
      weave::detail::trace_task()->debug("task {} '{}': done", task_id, r.name);
    } catch (const weave::CancelledError& e) {
      r.state = weave::detail::TaskState::CANCELLED;
      weave::detail::trace_task()->debug("task {} '{}': cancelled ({})", task_id, r.name,
                                         e.msg().value_or("no reason"));
    } catch (const std::exception& e) {
      r.state = weave::detail::TaskState::FAILED;
      r.eptr = std::current_exception();
      weave::detail::trace_task()->debug("task {} '{}': failed: {}", task_id, r.name, e.what());
    } catch (...) {
      r.state = weave::detail::TaskState::FAILED;
      r.eptr = std::current_exception();
      // Not a std::exception (CancelledError is caught above): no what() to print
      weave::detail::trace_task()->debug("task {} '{}': failed (non-standard exception)", task_id,
                                         r.name);
    }
  }
  // Done callbacks run with no task context (they count completions, they do not suspend): 0 is
  // published instead of the previous value, and the outer identity is restored on the way out.
  weave::port::set_owner_task(0);
  auto cbs = std::exchange(r.done_callbacks, {});
  for (auto& cb : cbs) cb();        // e.g. gather counting (runs with no task context)
  weave::port::trigger(r.done_ev);  // Wakes awaiters / the test wait chain
  weave::port::set_owner_task(saved_owner);
}
