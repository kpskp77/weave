// 组合原语：gather（asyncio.gather 语义）与 wait_for（asyncio.wait_for 语义）。
// 仅依赖核心簿记与 port，后端无关。

#include <memory>
#include <weave/event.hpp>
#include <weave/sync.hpp>

#include "core_internal.hpp"

namespace weave {
namespace detail {

// 语义对齐 asyncio.gather(return_exceptions=False)：
//   · 任一任务到达 FAILED/CANCELLED 终态 → 立即按完成顺序重抛其异常，
//     不等其余任务（asyncio 不因此取消兄弟任务——那是 TaskGroup 语义）
//   · 全部正常完成 → 返回
//   · gather 自身被取消 → 先取消所有未完成任务（asyncio：gather 被取消时
//     未完成的子任务一并取消），再传播 Cancelled
void gather_impl(std::initializer_list<Task> tasks) {
  if (tasks.size() == 0) return;

  struct GatherState {
    long remaining;
    bool failed = false;
    int first_failed_id = -1;  // 按完成顺序记录的首个失败
    Event wake;                // 首个失败 或 全部完成 时置位
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
        st->wake.set();  // 首个失败：立即唤醒，不等其余任务
      }
      if (--st->remaining == 0) st->wake.set();
    });
  }

  try {
    st->wake.wait();
  } catch (const Cancelled&) {
    // gather 被取消：取消所有未完成任务后传播（asyncio 语义）
    for (const Task& t : tasks) {
      if (!terminal(record(t.id_).state)) Task(t.id_).cancel();
    }
    throw;
  }
  if (st->failed) dispatch_completed(record(st->first_failed_id));
}

}  // namespace detail

void wait_for(Task& task, duration timeout) {
  auto& r = detail::record(task.id_);
  if (detail::terminal(r.state)) {
    detail::dispatch_completed(r);
    return;
  }
  auto& me = detail::current();
  const int st = detail::backend_wait(r.done_ev, detail::to_ticks(timeout), me);
  if (st == detail::WS_EVENT) {
    detail::dispatch_completed(r);
    return;
  }
  // 超时：取消并等待取消完成（asyncio 语义），随后抛 Timeout
  task.cancel();
  detail::backend_wait(r.done_ev, -1, me);
  throw Timeout{};
}

}  // namespace weave
