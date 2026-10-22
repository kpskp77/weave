// Task bookkeeping: TaskRecord storage, the single blocking point, the task dispatch entry point.
// Backend-agnostic — every backend interaction goes through the port contract (port.hpp).

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <weave/task.hpp>

#include "core_internal.hpp"

namespace weave {
namespace detail {

std::deque<TaskRecord> g_tasks;
int g_current = -1;
int g_root = -1;

TaskRecord& record(int id) {
  // Index = id - 1: ids increase monotonically from 1 and are never recycled, so a direct
  // subscript suffices, with no hash lookup
  if (id < 1 || static_cast<std::size_t>(id) > g_tasks.size())
    throw std::logic_error("weave: invalid task handle");
  return g_tasks[static_cast<std::size_t>(id - 1)];
}

TaskRecord& current() {
  if (g_current < 0) throw std::logic_error("weave: blocking primitive outside task context");
  return record(g_current);
}

int backend_wait(int ev_id, std::int64_t timeout_ticks) {
  TaskRecord& me = current();  // The suspended party is always the current task (only itself)
  if (me.cancel_armed) {       // A cancellation issued while suspended is injected at the next
                               // blocking point
    me.cancel_armed = false;
    throw cancelled_with_msg(me.cancel_msg);
  }
  me.state = TaskState::WAITING;
  const int saved = g_current;  // Slices of other tasks overwrite g_current while we are suspended
  int status = 0;
  port::wait(ev_id, timeout_ticks, me.abort_ev, &status);  // ← the only suspension point
  g_current = saved;                                       // Restore this slice's task identity
  me.state = TaskState::RUNNING;
  if (status == WS_ABORTED) {
    me.cancel_armed = false;
    // Recreate the abort event: the "triggered" state of an already triggered event lingers on the
    // backend (SystemC compares via change_stamp, SV via a same-slot triggered flag), so a task
    // that catches Cancelled and blocks on the same abort_ev again would be misdetected as a
    // cancellation hit.
    port::destroy_event(me.abort_ev);
    me.abort_ev = port::create_event();
    throw cancelled_with_msg(me.cancel_msg);
  }
  return status;
}

void dispatch_completed(TaskRecord& r) {
  if (r.state == TaskState::FAILED) std::rethrow_exception(r.eptr);
  // Rethrow a Cancelled carrying the original msg for the CANCELLED terminal state (as in
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
      break;
    case detail::TaskState::WAITING:  // Woken immediately and injected at the blocking point
      r.cancel_armed = true;
      port::trigger(r.abort_ev);
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
  return Task(id);
}

void sleep(duration t) {
  detail::backend_wait(-1, detail::to_ticks(t));
}

// Out-of-line definition of Cancelled::what().
// This definition (not the declaration) lives in the library and is the key function of the type:
// the vtable and typeinfo are emitted only in this TU, and downstream TUs keep undefined
// references that the dynamic linker resolves to the same copy. The message view always comes from
// std::string::c_str(), so data() is guaranteed NUL-terminated and can be returned directly as a C
// string.
const char* Cancelled::what() const noexcept {
  return msg_ ? msg_->data() : "weave::Cancelled";
}

namespace detail {

Cancelled cancelled_with_msg(std::string_view msg) noexcept {
  Cancelled c;
  // msg is a view of TaskRecord::cancel_msg, owned by the bookkeeping; the view is valid only for
  // this throw and the catches along the way (a TaskRecord is not destroyed before its task is
  // terminal). An empty string counts as no msg (as in asyncio's None), and Cancelled::msg()
  // returns nullopt.
  if (!msg.empty()) c.msg_ = msg;
  return c;
}

}  // namespace detail

}  // namespace weave

// ── Task dispatch entry (called by each backend's processes) ────────────────────────────────────

extern "C" void weave_task_main(int task_id) {
  auto& r = weave::detail::record(task_id);
  const int saved = weave::detail::g_current;
  weave::detail::g_current = task_id;
  if (r.cancel_armed) {
    // The cancellation happened before the body was first scheduled: the body never runs
    r.cancel_armed = false;
    r.state = weave::detail::TaskState::CANCELLED;
  } else {
    r.state = weave::detail::TaskState::RUNNING;
    try {
      r.body();
      r.state = weave::detail::TaskState::DONE;  // Swallowing Cancelled and returning → DONE
    } catch (const weave::Cancelled&) {
      r.state = weave::detail::TaskState::CANCELLED;
    } catch (...) {
      r.state = weave::detail::TaskState::FAILED;
      r.eptr = std::current_exception();
    }
  }
  weave::detail::g_current = saved;
  auto cbs = std::exchange(r.done_callbacks, {});
  for (auto& cb : cbs) cb();        // e.g. gather counting (runs with no task context)
  weave::port::trigger(r.done_ev);  // Wakes awaiters / the test wait chain
}
