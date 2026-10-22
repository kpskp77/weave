#pragma once

// Task primitives: Task / create_task / sleep / Cancelled. Semantics follow asyncio.Task.
// create_task only schedules, it does not run immediately: a child task is scheduled only after
// the parent reaches its next blocking point (or finishes), which the backend process semantics
// guarantee.

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <weave/time.hpp>

namespace weave {

class Task;
struct Cancelled;

namespace detail {
struct TaskRecord;  // core bookkeeping (defined in src/core/core_internal.hpp)

// Constructs a Cancelled carrying a message (msg is owned by a TaskRecord; the view stays valid
// for the lifetime of the throw). Takes a view rather than a const char*: a view does not own
// data, consistent with the borrowed semantics of Cancelled::msg_, and callers (always
// std::string members) can pass the whole string without a strlen.
// Invariant: the view must come from a std::string's full contents (NUL immediately after),
// because Cancelled::what() returns data() directly and uses it as a C string.
// Exposed to detail:: so the dispatch path in sync.cpp can preserve the original message.
Cancelled cancelled_with_msg(std::string_view msg) noexcept;
}  // namespace detail

// Cancellation signal. Corresponds to asyncio.CancelledError, injected at a task's blocking
// point. Deliberately does not derive from std::exception: matching the hierarchy where
// CancelledError belongs to BaseException since asyncio 3.8 — the idiomatic
// catch (const std::exception&) backstop must not silently swallow cancellation (swallowing it
// would make the task wrongly finish as DONE and break gather / TaskGroup / wait_for's judgement
// of the CANCELLED terminal state). To catch it, write catch (const weave::Cancelled&) or
// catch (...) explicitly.
//
// The message matches asyncio Task.cancel(msg) (Python 3.9+): msg is an optional description
// passed by the canceller; no message (plain cancellation) corresponds to asyncio's msg=None and
// is expressed as nullopt. The view returned by msg() points at TaskRecord::cancel_msg and is
// valid only for the lifetime of the same throw that carried the exception.
//
// what() is a **virtual function** with an **out-of-line definition** (src/core/task.cpp) and is
// diagnostics only. Both facts are deliberate: an out-of-line virtual function acts as the key
// function, so the vtable/typeinfo is emitted exactly once, inside the library, while consumer
// TUs keep only undefined references; only then does a cross-DSO catch match (libc++abi compares
// type_info by pointer; if the type stayed non-polymorphic, typeinfo would degrade into one weak
// definition per TU and catch would silently mismatch under hidden-visibility builds). The cost
// is that a polymorphic class is no longer trivially copyable (the standard makes the copy
// constructor of a class with virtual functions non-trivial) — catch correctness is chosen here,
// and the assertions instead watch the properties that really matter (see the static_assert
// below).
struct Cancelled {
  // A polymorphic type must declare a virtual destructor: Cancelled is constructed/destroyed on
  // the heap through paths such as std::make_exception_ptr (gather(return_exceptions)), and a
  // non-virtual destructor triggers -Wdelete-non-abstract-non-virtual-dtor (a hard compile
  // failure under -Werror).
  virtual ~Cancelled() = default;
  virtual const char* what() const noexcept;
  // msg=None → nullopt (no longer a nullptr sentinel)
  std::optional<std::string_view> msg() const noexcept { return msg_; }

private:
  std::optional<std::string_view> msg_;
  friend Cancelled detail::cancelled_with_msg(std::string_view msg) noexcept;
};

// The exception object = "a nullable view + a discriminant", with no heap members and a nothrow
// copy: taking the exception path allocates nothing extra. Note that trivially_copyable cannot be
// required: what() is the key function emitted only in the library (see above), and the standard
// makes the copy constructor of a polymorphic class non-trivial — RTTI being unique across DSOs
// (catch correctness) takes priority over that trait. What is watched here instead are the two
// things actually wanted: nothrow copy/destruction, and no heap members at all.
static_assert(std::is_nothrow_copy_constructible_v<Cancelled> &&
                  std::is_nothrow_destructible_v<Cancelled>,
              "Cancelled must be cheap to throw: nothrow copy, no heap members");

class Task;

namespace detail {
// Combinator entry points (defined in src/core/sync.cpp); declared here only for Task's friends.
void gather_impl(std::initializer_list<Task> tasks);
// Handle/id conversion inside combinators (defined in src/core/task.cpp).
int task_id(const Task& t);
Task task_from_id(int id);
// Backend bootstrap entry (defined in src/core/registry.cpp); only for the backend adapter layer.
int run_registered_test();
}  // namespace detail

class Task {
public:
  Task() = default;

  // Waits for the task to finish. A FAILED task rethrows its exception; a CANCELLED task throws
  // Cancelled. Must be called inside a task context (it is a blocking primitive).
  void await();

  // Reached any terminal state (DONE / FAILED / CANCELLED), matching asyncio.Task.done()
  [[nodiscard]] bool done() const;
  [[nodiscard]] bool cancelled() const;
  [[nodiscard]] bool valid() const noexcept { return id_ >= 0; }
  const std::string& get_name() const;  // Matches asyncio.Task.get_name()

  // Cooperative cancellation (matching asyncio Task.cancel(msg), Python 3.9+):
  // PENDING → the body does not run; RUNNING → Cancelled(msg) is thrown at the next blocking
  // point; WAITING → woken immediately and Cancelled(msg) thrown at the blocking point. A task
  // may catch Cancelled to refuse cancellation.
  // msg is an optional cancellation reason; an empty string means a plain cancellation; whoever
  // awaits this task sees a Cancelled carrying the same msg.
  // const: Task is a handle, and cancelling changes the bookkeeping state of the referenced task,
  // not the handle itself (same semantics as shared_ptr's const operator*), so cancellation may
  // be initiated through a const Task& (e.g. gather's cascade cancellation).
  void cancel(std::string msg = {}) const;

private:
  explicit Task(int id) : id_(id) {}
  int id_ = -1;

  friend Task create_task(std::string name, std::function<void()> body);
  friend void detail::gather_impl(std::initializer_list<Task> tasks);
  friend int detail::task_id(const Task& t);
  friend Task detail::task_from_id(int id);
  friend int detail::run_registered_test();
};

// Creates a task. As in asyncio: the new task does not run immediately, it is scheduled only after
// the current task reaches its next blocking point (guaranteed by backend process semantics).
Task create_task(std::string name, std::function<void()> body);

template <typename F>
Task create_task(std::string name, F&& body) {
  static_assert(std::is_invocable_v<F&>, "weave: task body must be callable with no arguments");
  return create_task(std::move(name), std::function<void()>(std::forward<F>(body)));
}

// ── Time waits ──────────────────────────────────────────────────────────
// sleep(0) is special-cased to a zero-time yield (corresponding to asyncio.sleep(0)).
void sleep(duration t);

}  // namespace weave
