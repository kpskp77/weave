#pragma once

// 任务原语：Task / create_task / sleep / Cancelled。语义对齐 asyncio.Task。
// create_task 只调度不立即运行：子任务等父任务碰到下一个阻塞点
// （或结束）后才获得调度，由后端进程语义保证。

#include <functional>
#include <initializer_list>
#include <string>
#include <weave/time.hpp>

namespace weave {

struct Cancelled {};  // 对应 asyncio.CancelledError，在任务阻塞点注入

class Task;

namespace detail {
// 组合原语入口（定义于 src/core/sync.cpp）；此处声明仅供 Task 友元引用。
void gather_impl(std::initializer_list<Task> tasks);
// 组合原语内部的句柄/id 互转（定义于 src/core/task.cpp）。
int task_id(const Task& t);
Task task_from_id(int id);
// 后端引导入口（定义于 src/core/registry.cpp）；仅供后端适配层使用。
int run_registered_test();
}  // namespace detail

class Task {
public:
  Task() = default;

  // 等待任务完成。FAILED 任务重抛其异常；CANCELLED 任务抛 Cancelled。
  // 必须在任务上下文内调用（阻塞原语）。
  void await();

  bool done() const;  // 到达任一终态（DONE / FAILED / CANCELLED），对齐 asyncio.Task.done()
  bool cancelled() const;
  bool valid() const { return id_ >= 0; }
  const std::string& get_name() const;  // 对齐 asyncio.Task.get_name()

  // 协作式取消：PENDING → body 不执行；RUNNING → 下一个阻塞点抛 Cancelled；
  // WAITING → 立即唤醒并在阻塞点抛 Cancelled。任务可捕获 Cancelled 拒绝取消。
  void cancel();

private:
  explicit Task(int id) : id_(id) {}
  int id_ = -1;

  friend Task create_task(std::string name, std::function<void()> body);
  friend void detail::gather_impl(std::initializer_list<Task> tasks);
  friend int detail::task_id(const Task& t);
  friend Task detail::task_from_id(int id);
  friend void wait_for(Task& task, duration timeout);
  friend int detail::run_registered_test();
};

// 创建任务。与 asyncio 一致：新任务不会立即运行，
// 等当前任务碰到下一个阻塞点后才被调度（由后端进程语义保证）。
Task create_task(std::string name, std::function<void()> body);

template <typename F>
Task create_task(std::string name, F&& body) {
  return create_task(std::move(name), std::function<void()>(std::forward<F>(body)));
}

// ── 时间等待 ──────────────────────────────────────────────────────────
// sleep(0) 特判为零时间让出（对应 asyncio.sleep(0)）。
void sleep(duration t);

}  // namespace weave
