#pragma once

// 核心内部共享状态：任务簿记 + 唯一阻塞点。非公共 API，
// 仅供 src/core 与 src/backend 使用。

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

// 作用域退出动作：析构时执行 f（除非 release()）。f 必须按 noexcept 语义编写——
// 析构不抛，故 port::destroy_event 的不变量一旦被破坏即 terminate，而非静默替换
// 正在传播的异常。用途限于「作用域内」的清场（摘除/销毁/复位标志）；跨作用域或
// 对象寿命的所有权仍要显式成员 + 析构（见 AsCompleted::Impl、TaskRecord::abort_ev）。
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

// 工厂：调用处免写类型名（C++17 下限，不依赖 CTAD）。
template <class F>
scope_exit<F> on_scope_exit(F f) {
  return scope_exit<F>(std::move(f));
}

struct TaskRecord {
  std::string name;
  std::function<void()> body;
  TaskState state = TaskState::PENDING;
  std::exception_ptr eptr;
  int abort_ev = -1;  // 每任务私有取消事件（每个阻塞等待都附带此腿）
  int done_ev = -1;   // 任务完成事件（单调，可被任意多方等待）
  bool cancel_armed = false;
  // 对齐 asyncio Task.cancel(msg) 的 _cancel_message：cancel() 记下的取消原因，
  // 注入 Cancelled 时取 c_str()；任务进入 CANCELLED 后仍保留，供 await 重投时取同一 msg。
  std::string cancel_msg;
  std::vector<std::function<void()>> done_callbacks;
};

// 定义于 task.cpp。索引 = id - 1：id 从 1 递增且从不回收，所以用顺序容器
// 而非哈希表；deque 的 push_back 不使已有元素的引用失效，满足"TaskRecord& 可跨
// 挂起点持有"（vector 扩容会搬家，不可用）。
// 注意：id 由 g_tasks.size() 推导（见 create_task），不另设计数器——否则
// create_event/push_back 抛异常时 id 已被消费而记录未落地，id→索引会永久错位。
extern std::deque<TaskRecord> g_tasks;
extern int g_current;  // 当前执行切片的任务 id；-1 = 非任务上下文（harness 链）
extern int g_root;

TaskRecord& record(int id);
TaskRecord& current();

// 唯一阻塞点：所有原语都经此进入后端等待。职责：取消预检 → 状态标记 →
// 保存/恢复 g_current → 取消注入。
// 被挂起者恒为 current()：asyncio 语义下挂起只会是自身挂起（作用于其他任务
// 一律是唤醒/取消，不是挂起），故不接受外部传入任务记录。
int backend_wait(int ev_id, std::int64_t timeout_ticks);

void dispatch_completed(TaskRecord& r);
void add_done_callback(int id, std::function<void()> cb);

// 定义于 time.cpp：飞秒 → 后端 tick（向上取整；0 保持 0）。
std::int64_t to_ticks(duration d);

}  // namespace detail
}  // namespace weave
