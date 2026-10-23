#pragma once

// Combinators: gather / wait_for / wait / shield / as_completed / TaskGroup.
// Semantics match the asyncio primitives of the same names: asyncio.gather (default form plus the
// return_exceptions form), asyncio.wait_for (3.12+ timeout semantics), asyncio.wait,
// asyncio.shield, asyncio.as_completed, asyncio.TaskGroup.
// All of them are built on task bookkeeping + Event + the single blocking point, so the backend
// needs no changes.

#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <weave/task.hpp>
#include <weave/time.hpp>

namespace weave {

// TimeoutError signal. Corresponds to asyncio.TimeoutError, thrown when wait_for times out.
// Unlike CancelledError, it derives from std::exception: asyncio's TimeoutError is an ordinary
// Exception (an OSError subclass), since a timeout is a normal failure path that a generic
// catch (const std::exception&) backstop may handle.
//
// what() must be **defined out-of-line** (src/core/sync.cpp) — it is the key function of this
// type: with it, the vtable and typeinfo are each emitted exactly once in the TU that defines it
// (libweave.so), and every consumer TU keeps only an undefined reference that the dynamic linker
// resolves to that copy in the library. Written back as inline (or removed), typeinfo degrades
// into one weak definition per TU: libc++abi compares type_info by pointer, so catch would
// silently mismatch across DSOs.
struct TimeoutError : std::exception {
  const char* what() const noexcept override;
};

// ── gather ───────────────────────────────────────────────────────────────────

// Waits for a group of tasks (default form, matching asyncio.gather(return_exceptions=False)):
//   · any task reaching FAILED/CANCELLED → rethrows its exception immediately, in completion
//     order; the other tasks are not cancelled and keep running (cancelling siblings is
//     TaskGroup's semantics)
//   · all complete normally → returns
//   · gather itself is cancelled → cancel all unfinished tasks first (asyncio: when gather is
//     cancelled, its unfinished child tasks are cancelled too), then throw CancelledError
template <typename... Ts>
void gather(Ts&... tasks) {
  detail::gather_impl({tasks...});
}

// Tag for the return_exceptions form: corresponding to asyncio.gather(*aws, return_exceptions=True)
struct return_exceptions_t {
  explicit constexpr return_exceptions_t() = default;
};
inline constexpr return_exceptions_t return_exceptions{};

// The return_exceptions form: never returns early because a child task failed; once everything has
// landed it returns the results in input order — DONE → nullptr; FAILED → its exception;
// CANCELLED → an exception_ptr wrapping CancelledError. If gather itself is cancelled it still
// follows the default form's semantics: cancel all unfinished tasks, then propagate CancelledError.
// The result must be consumed: ignoring it equals waiting a whole round for nothing (the child
// tasks have all landed but their exceptions are dropped).
[[nodiscard]] std::vector<std::exception_ptr> gather(std::initializer_list<Task> tasks,
                                                     return_exceptions_t);

// ── wait_for ─────────────────────────────────────────────────────────────────

// On timeout, cancels the task and waits for the cancellation to complete, then throws TimeoutError
// (asyncio 3.12+ semantics).
void wait_for(const Task& task, duration timeout);

// ── wait (asyncio.wait) ──────────────────────────────────────────────────────

enum class ReturnWhen { ALL_COMPLETED, FIRST_COMPLETED, FIRST_EXCEPTION };

struct WaitResult {
  std::vector<Task> done;     // Reached a terminal state (input order preserved)
  std::vector<Task> pending;  // Not finished (input order preserved)
};

// Matches asyncio.wait: it only groups tasks by their state at that moment, never rethrows child
// exceptions, and never cancels child tasks. FIRST_EXCEPTION triggers an early return on FAILED
// only (CANCELLED does not count, matching CPython's not f.cancelled() and f.exception() is not
// None). On timeout the tasks are grouped by their state at that moment; nullopt (the default,
// corresponding to asyncio's timeout=None) means no timeout; duration{0} is special-cased to an
// immediate snapshot (no blocking, no yield), and non-positive durations are handled the same way.
// If the waiter itself is cancelled → CancelledError propagates and the child tasks are unaffected.
WaitResult wait(std::initializer_list<Task> tasks,
                ReturnWhen return_when = ReturnWhen::ALL_COMPLETED,
                std::optional<duration> timeout = std::nullopt);

// ── shield (asyncio.shield) ──────────────────────────────────────────────────

// Returns a proxy task: the proxy mirrors inner's terminal state and exception, but cancelling the
// proxy / a wait_for timeout / a gather cascade cancellation stops at the proxy layer, and inner
// keeps running. (Task::await never propagates cancellation to the task being awaited anyway — as
// in asyncio; shield is for combinators that do cancel child tasks.)
[[nodiscard]] Task shield(Task& inner);

// ── as_completed (asyncio.as_completed) ──────────────────────────────────────

class AsCompleted {
public:
  [[nodiscard]] bool pending() const noexcept;  // There are still tasks to produce

  // Blocks until the next task completes and returns it in completion order (already in a
  // terminal state, so .await() immediately yields the result or rethrows the exception); calling
  // it after everything has been produced throws std::logic_error.
  // Cancellation only interrupts next() itself: neither the completed queue nor the child tasks
  // are affected. Single consumer.
  [[nodiscard]] Task next();

private:
  struct Impl;
  explicit AsCompleted(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;

  friend AsCompleted as_completed(std::initializer_list<Task> tasks);
};

AsCompleted as_completed(std::initializer_list<Task> tasks);

// ── TaskGroup (asyncio.TaskGroup) ────────────────────────────────────────────

// Structured concurrency: child tasks are created with the group's create_task, and wait() blocks
// until all of them finish:
//   · all finished (successfully or cancelled externally) → returns normally
//   · the first FAILED (in completion order) → cancel all unfinished siblings, wait for them all to
//     land, then rethrow its exception
//   · the waiter is cancelled → cascade-cancel all unfinished child tasks, wait for them to land,
//     then throw CancelledError
// A child task cancelled externally (CANCELLED) is not a failure and does not trigger sibling
// cancellation (matching asyncio.TaskGroup's tolerance of externally cancelled child tasks).
// Deviation: no ExceptionGroup, only the first failure is rethrown.
// After wait(), create_task or a second wait() throws std::logic_error.
//
// cancel() (asyncio.TaskGroup.cancel, Python 3.15+): actively cancels all unfinished child tasks
// from outside the group, with msg matching Task.cancel(msg). A child task cancelled externally is
// not a failure and does not trigger sibling cancellation; the completion order and terminal states
// seen by wait() are unaffected.
class TaskGroup {
public:
  TaskGroup();

  Task create_task(std::string name, std::function<void()> body);
  template <typename F>
  Task create_task(std::string name, F&& body) {
    static_assert(std::is_invocable_v<F&>, "weave: task body must be callable with no arguments");
    return create_task(std::move(name), std::function<void()>(std::forward<F>(body)));
  }

  // Cancels all unfinished child tasks; without a msg the cascade to siblings carries no message
  // (matching TaskGroup semantics).
  void cancel(std::string msg = {});

  void wait();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace weave
