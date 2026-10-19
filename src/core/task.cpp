// 任务簿记：TaskRecord 存储、唯一阻塞点、任务分派入口。
// 后端无关——所有后端交互都经由 port 契约（port.hpp）。

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
  // 索引 = id - 1：id 从 1 单调递增且从不回收，直接下标即可，无哈希查找
  if (id < 1 || static_cast<std::size_t>(id) > g_tasks.size())
    throw std::logic_error("weave: invalid task handle");
  return g_tasks[static_cast<std::size_t>(id - 1)];
}

TaskRecord& current() {
  if (g_current < 0) throw std::logic_error("weave: blocking primitive outside task context");
  return record(g_current);
}

int backend_wait(int ev_id, std::int64_t timeout_ticks) {
  TaskRecord& me = current();  // 被挂起者恒为当前任务（只能挂起自身）
  if (me.cancel_armed) {       // 挂起的取消在下一个阻塞点注入
    me.cancel_armed = false;
    throw cancelled_with_msg(me.cancel_msg);
  }
  me.state = TaskState::WAITING;
  const int saved = g_current;  // 挂起期间其他任务的切片会改写 g_current
  int status = 0;
  port::wait(ev_id, timeout_ticks, me.abort_ev, &status);  // ← 唯一挂起点
  g_current = saved;                                       // 恢复本切片的任务身份
  me.state = TaskState::RUNNING;
  if (status == WS_ABORTED) {
    me.cancel_armed = false;
    // 重建 abort 事件：已触发事件的"triggered"状态在后端会滞留
    // （SystemC 的 change_stamp 判等、SV 的同槽 triggered 标志），
    // 任务捕获 Cancelled 后在同一 abort_ev 上再次阻塞会被误判为取消命中。
    port::destroy_event(me.abort_ev);
    me.abort_ev = port::create_event();
    throw cancelled_with_msg(me.cancel_msg);
  }
  return status;
}

void dispatch_completed(TaskRecord& r) {
  if (r.state == TaskState::FAILED) std::rethrow_exception(r.eptr);
  // CANCELLED 终态重投带原 msg 的 Cancelled（对齐 asyncio：await 取消任务者收到
  // 同一 CancelledError(msg)）
  if (r.state == TaskState::CANCELLED) throw cancelled_with_msg(r.cancel_msg);
}

void add_done_callback(int id, std::function<void()> cb) {
  auto& r = record(id);
  if (terminal(r.state))
    cb();  // 已终结：立即调用
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
  if (!r.cancel_armed) r.cancel_msg = std::move(msg);  // 首个取消者的 msg 生效（对齐 asyncio）
  switch (r.state) {
    case detail::TaskState::PENDING:  // body 永不执行（任务入口拦截）
    case detail::TaskState::RUNNING:  // 下一个阻塞点注入
      r.cancel_armed = true;
      break;
    case detail::TaskState::WAITING:  // 立即唤醒并在阻塞点注入
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
  // id 由簿记长度推导（从 1 起），保证 index = id - 1 在下方任何一步抛异常时
  // 都不会错位（用独立计数器的话，create_event 失败会白白消费一个 id）。
  const int id = static_cast<int>(detail::g_tasks.size()) + 1;
  detail::TaskRecord r;
  r.name = std::move(name);
  r.body = std::move(body);
  r.abort_ev = port::create_event();
  r.done_ev = port::create_event();
  detail::g_tasks.push_back(std::move(r));  // 索引 = id - 1
  port::spawn(id);
  return Task(id);
}

void sleep(duration t) {
  detail::backend_wait(-1, detail::to_ticks(t));
}

namespace detail {

Cancelled cancelled_with_msg(std::string_view msg) noexcept {
  Cancelled c;
  // msg 是 TaskRecord::cancel_msg 的视图，由簿记所有；视图仅在本次 throw
  // 及沿途捕获的生命周期内有效（TaskRecord 在任务终结前不销毁）。
  // 空串视为无 msg（对齐 asyncio 的 None），Cancelled::msg() 返回 nullopt。
  if (!msg.empty()) c.msg_ = msg;
  return c;
}

}  // namespace detail

}  // namespace weave

// ── 任务分派入口（由各后端进程调用）────────────────────────────────────

extern "C" void weave_task_main(int task_id) {
  auto& r = weave::detail::record(task_id);
  const int saved = weave::detail::g_current;
  weave::detail::g_current = task_id;
  if (r.cancel_armed) {
    // 取消发生在 body 首次调度之前：body 永不执行
    r.cancel_armed = false;
    r.state = weave::detail::TaskState::CANCELLED;
  } else {
    r.state = weave::detail::TaskState::RUNNING;
    try {
      r.body();
      r.state = weave::detail::TaskState::DONE;  // 吞掉 Cancelled 后正常返回 → DONE
    } catch (const weave::Cancelled&) {
      r.state = weave::detail::TaskState::CANCELLED;
    } catch (...) {
      r.state = weave::detail::TaskState::FAILED;
      r.eptr = std::current_exception();
    }
  }
  weave::detail::g_current = saved;
  auto cbs = std::exchange(r.done_callbacks, {});
  for (auto& cb : cbs) cb();        // gather 计数等（无任务上下文执行）
  weave::port::trigger(r.done_ev);  // 唤醒 await 者 / 测试等待链
}
