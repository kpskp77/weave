#pragma once

// Core internals shared across the core: task bookkeeping + the single blocking point. Not a
// public API; only src/core and src/backend may use it.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <weave/task.hpp>
#include <weave/time.hpp>

#include "port.hpp"

namespace weave {
namespace detail {

enum class TaskState { PENDING, RUNNING, WAITING, DONE, FAILED, CANCELLED };

inline bool terminal(TaskState s) {
  return s == TaskState::DONE || s == TaskState::FAILED || s == TaskState::CANCELLED;
}

enum WaitStatus : int { WS_EVENT = 0, WS_TIMEOUT = 1, WS_ABORTED = 2 };

// Scope-exit action: runs f on destruction (unless release() was called). f must be written to
// noexcept semantics — destructors do not throw, so any violation of port::destroy_event's
// invariant terminates instead of silently replacing the exception currently propagating. Use it
// only for cleanup "inside a scope" (unlinking/destroying/resetting flags); ownership that crosses
// scopes or object lifetimes still needs an explicit member + destructor (see AsCompleted::Impl,
// TaskRecord::abort_ev).
template <class F>
class scope_exit {
public:
  explicit scope_exit(F f) noexcept : f_(std::move(f)) {}
  scope_exit(scope_exit&& o) noexcept
      : f_(std::move(o.f_)), armed_(std::exchange(o.armed_, false)) {}
  scope_exit(const scope_exit&) = delete;
  scope_exit& operator=(const scope_exit&) = delete;
  scope_exit& operator=(scope_exit&&) = delete;
  ~scope_exit() noexcept {
    if (armed_) f_();
  }
  void release() noexcept { armed_ = false; }

private:
  F f_;
  bool armed_ = true;
};

// Factory: saves writing the type name at call sites (C++17 floor, no CTAD reliance).
template <class F>
scope_exit<F> on_scope_exit(F f) {
  return scope_exit<F>(std::move(f));
}

struct TaskRecord {
  std::string name;
  std::function<void()> body;
  TaskState state = TaskState::PENDING;
  std::exception_ptr eptr;
  int abort_ev = -1;  // Per-task private cancel event (attached as a leg to every blocking wait)
  int done_ev = -1;   // Task completion event (monotonic, may be awaited by any number of parties)
  bool cancel_armed = false;
  // Matches asyncio Task.cancel(msg)'s _cancel_message: the cancellation reason recorded by
  // cancel(), whose c_str() is used when injecting CancelledError; it is kept after the task enters
  // CANCELLED so that a rethrow at await yields the same msg.
  std::string cancel_msg;
  std::vector<std::function<void()>> done_callbacks;
  // Current suspension point for the wait-graph diagnostics (include/weave/debug.hpp): the backend
  // event the task is held on and the clock reading at which it blocked. wait_ev stays -1 for a
  // PENDING/RUNNING task, so a snapshot can tell "blocked" from "not blocked" without a second
  // state field; the pair is cleared on every exit path from backend_wait, including the
  // cancellation throw.
  int wait_ev = -1;
  std::int64_t wait_since = 0;
  // Set for a debug::watchdog task: its own wakeups advance the clock without any other task making
  // progress, so they must not be counted as progress (see g_progress).
  bool watchdog = false;
};

// Defined in task.cpp. Index = id - 1: ids increase from 1 and are never recycled, hence a
// sequential container instead of a hash map; deque's push_back does not invalidate references to
// existing elements, which satisfies "a TaskRecord& may be held across a suspension point"
// (vector reallocation would move them, so it cannot be used).
// Note: ids are derived from g_tasks.size() (see create_task) rather than kept in a separate
// counter — otherwise, if create_event/push_back threw, the id would already be consumed while the
// record never landed, permanently skewing the id→index mapping.
extern std::deque<TaskRecord> g_tasks;
extern int g_current;  // Task id of the currently executing slice; -1 = no task context
                       // (harness chain)
extern int g_root;

// Monotonic count of "some task resumed from the single blocking point", i.e. the progress signal
// the debug::watchdog measures idleness against. Advanced by backend_wait and never reset; watchdog
// tasks themselves are excluded (their sleep is the measurement, not progress).
extern std::uint64_t g_progress;

TaskRecord& record(int id);
TaskRecord& current();

// The single blocking point: every primitive enters the backend wait through here. Duties: cancel
// pre-check → state marking → saving/restoring g_current → cancellation injection.
// The suspended party is always current(): under asyncio semantics a suspension is always a
// self-suspension (operating on other tasks is always wakeup/cancellation, never suspension), so
// no external task record is accepted as a parameter.
int backend_wait(int ev_id, std::int64_t timeout_ticks);

void dispatch_completed(TaskRecord& r);
void add_done_callback(int id, std::function<void()> cb);

// Defined in time.cpp: femtoseconds → backend ticks (rounded up; 0 stays 0) and its inverse
// (truncating; used by the wait-graph diagnostics to age a blocked task in simulation time).
std::int64_t to_ticks(duration d);
duration from_ticks(std::int64_t ticks);

}  // namespace detail
}  // namespace weave
