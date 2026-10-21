// 组合原语：gather（两种形态）/ wait_for / wait / shield / as_completed /
// TaskGroup。仅依赖核心簿记与 port，后端无关。
//
// 裸 port 事件的使用约定（wait / TaskGroup）：trigger 只在确有等待者时发出
// （waiting 标志守卫）——无等待者的 trigger 会留下未消费的唤醒，
// destroy 一个仍挂着 delta notification 的事件在后端侧是未定义行为。

#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include <weave/event.hpp>
#include <weave/sync.hpp>

#include "core_internal.hpp"

namespace weave {

// Timeout::what() 的外联定义。
// 定义放在库里（而非内联回头文件），使它成为本类型的 key function：vtable 与
// typeinfo 只在这一个 TU 生成一份，消费方 TU 只留未定义引用、经动态链接解析到同一份。
// 若改回内联，typeinfo 会退化成每个 TU 一份的 weak 定义，libc++abi 按指针比较
// type_info，跨 DSO 的 catch 会静默失配。
const char* Timeout::what() const noexcept {
  return "weave::Timeout";
}

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
    // gather 被取消：取消所有未完成任务后传播（asyncio 语义）。
    // 级联取消的兄弟任务不携带 gather 调用者的 msg（那是父任务的上下文，
    // 兄弟任务是被连带取消，msg 属于取消发起方），故传空串。
    for (const Task& t : tasks) {
      if (!t.done()) t.cancel();
    }
    throw;
  }
  if (st->failed) dispatch_completed(record(st->first_failed_id));
}

}  // namespace detail

// ── gather：return_exceptions 形态 ────────────────────────────────────

std::vector<std::exception_ptr> gather(std::initializer_list<Task> tasks, return_exceptions_t) {
  if (tasks.size() == 0) return {};

  struct GatherState {
    long remaining;
    Event wake;  // 全部落地时置位（Event 可安全重复 set：等待者返回后已摘除）
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
        // return_exceptions 形态的 CANCELLED 映射：重投带原 msg 的 Cancelled，
        // 对齐 asyncio.gather(*aws, return_exceptions=True) 对 CancelledError(msg) 的保留
        st->results[i] = std::make_exception_ptr(detail::cancelled_with_msg(r.cancel_msg));
      if (--st->remaining == 0) st->wake.set();
    });
    ++i;
  }

  try {
    st->wake.wait();
  } catch (const Cancelled&) {
    // gather 被取消：取消所有未完成任务后传播（与默认形态一致）
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
  // 超时：取消并等待取消完成（asyncio 语义），随后抛 Timeout。
  // 取消带 msg 标明原因是 wait_for 超时（对齐 asyncio 3.12+：超时的内部取消会标记来源）。
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
    bool waiting = false;    // 等待方正阻塞在 wake_ev 上（trigger 必有等待者）
    bool signalled = false;  // 唤醒条件已满足（只需唤醒一次）
  };
  auto st = std::make_shared<WaitState>();
  st->when = return_when;
  st->remaining = static_cast<long>(tasks.size());
  st->wake_ev = port::create_event();

  // 覆盖异常与正常路径。销毁前必须先落下 waiting：晚到的完成只在 waiting 为真时
  // trigger，标志滞留为 true 就会对已销毁 id 触发（走不到 catch 的清场，所以两件事
  // 必须在同一个 lambda 里成对完成）。
  auto destroy_wake = detail::on_scope_exit([&st] {
    st->waiting = false;
    port::destroy_event(st->wake_ev);
  });

  for (const Task& t : tasks) {
    const int id = detail::task_id(t);
    detail::add_done_callback(id, [st, id]() {
      if (st->signalled) return;  // 只需唤醒一次；亦挡住等待方返回后的晚到完成
      const auto state = detail::record(id).state;
      --st->remaining;
      // FIRST_EXCEPTION 仅 FAILED 触发（CANCELLED 不算，对齐 CPython）
      if (st->remaining == 0 || st->when == ReturnWhen::FIRST_COMPLETED ||
          (st->when == ReturnWhen::FIRST_EXCEPTION && state == detail::TaskState::FAILED)) {
        st->signalled = true;
        if (st->waiting) port::trigger(st->wake_ev);
      }
    });
  }

  // nullopt = 无超时；非正时长（duration{0} 及负值）特判为直接快照（对齐
  // asyncio.wait(timeout=0) 的立即返回），不做零时间让出——否则结果依赖
  // delta 调度顺序，不确定。
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

// ── shield（asyncio.shield）────────────────────────────────────────────

Task shield(Task& inner) {
  // 代理任务原样等待 inner：Task::await 本就不向被等任务传播取消，
  // 因此对代理的 cancel / 超时 / 级联取消只停在代理层，inner 继续运行；
  // 代理经 await 镜像 inner 的终态与异常。按值捕获（句柄即 id），
  // 不引用调用方栈帧。
  return create_task(inner.get_name() + "/shield", [inner]() mutable { inner.await(); });
}

// ── as_completed（asyncio.as_completed）────────────────────────────────

struct AsCompleted::Impl {
  long total = 0;
  long produced = 0;      // 已交付消费者的个数
  std::deque<int> queue;  // 已完成待交付（完成顺序）
  int wake_ev = -1;
  bool waiting = false;  // 消费者正阻塞在 wake_ev 上（trigger 必有等待者）

  ~Impl() {
    // Impl 由回调持有的 shared_ptr 保活至全部任务完成；waiting 守卫保证
    // 此时不存在未消费的唤醒。
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
    // 这里必须异常安全：st 是 *impl_，跨 next() 调用存活——标志若留在 true，后续 completion
    // 就会对无等待者的 wake_ev 触发。
    auto clear_waiting = detail::on_scope_exit([&st] { st.waiting = false; });
    detail::backend_wait(st.wake_ev, -1);  // 取消只中断 next() 本身，队列与子任务不受影响
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

// ── TaskGroup（asyncio.TaskGroup）──────────────────────────────────────

struct TaskGroup::Impl {
  std::vector<Task> children;
  bool exited = false;  // wait() 已调用
};

TaskGroup::TaskGroup() : impl_(std::make_shared<Impl>()) {}

Task TaskGroup::create_task(std::string name, std::function<void()> body) {
  if (impl_->exited) throw std::logic_error("TaskGroup: create_task after wait");
  Task t = weave::create_task(std::move(name), std::move(body));
  impl_->children.push_back(t);
  return t;
}

void TaskGroup::cancel(std::string msg) {
  // 仅取消未完成任务；已终态的子任务不动（对齐 asyncio.TaskGroup.cancel 对
  // finished 子任务的跳过）。各任务协作式取消，不阻塞、不等落地——落地仍在 wait()。
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
    int first_failed = -1;  // 按完成顺序记录的首个 FAILED
    int wake_ev = -1;       // 阶段一：首个失败 或 全部完成
    int drain_ev = -1;      // 阶段二：取消兄弟后等待全部落地
    bool waiting = false;   // 阻塞在 wake_ev 上
    bool draining = false;  // 阻塞在 drain_ev 上
  };
  auto st = std::make_shared<GroupState>();
  st->remaining = static_cast<long>(children.size());
  st->wake_ev = port::create_event();
  st->drain_ev = port::create_event();

  // 两个裸事件由守卫在作用域退出时销毁。不必像 wait() 那样在守卫里复位标志：Cancelled 就地捕获
  // 并 drain 完兄弟后才外抛，能逸出的只剩 current()/record() 的不变量违例（已是 bug 路径），
  // 原实现同样不为它们复位 st。
  auto destroy_events = detail::on_scope_exit([&st] {
    port::destroy_event(st->wake_ev);
    port::destroy_event(st->drain_ev);
  });

  for (const Task& t : children) {
    const int id = detail::task_id(t);
    detail::add_done_callback(id, [st, id]() {
      const auto state = detail::record(id).state;
      --st->remaining;
      // CANCELLED 容忍（对齐 asyncio.TaskGroup 对外部取消子任务的处理）：
      // 仅真正的 FAILED 触发兄弟取消
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
  if (!st->failed && st->remaining > 0) {  // 注册期间未满足唤醒条件才阻塞
    st->waiting = true;
    try {
      detail::backend_wait(st->wake_ev, -1);
    } catch (const Cancelled&) {
      cancelled = true;  // 等待者被取消：级联取消全部未完成子任务
    }
    st->waiting = false;
  }

  if (cancelled || st->failed) {
    // 首个 FAILED 或等待者被取消：取消所有未完成兄弟（对齐 asyncio TaskGroup）。
    // 级联取消的兄弟不携带调用者的 msg（msg 属于取消发起方的上下文）。
    for (const Task& t : children) {
      if (!t.done()) t.cancel();
    }
    // 等所有子任务落地后才抛出：子任务可能引用本栈帧的对象，绝不能让它们
    // 在 wait 返回后继续运行。落地期间的二次取消一并吞掉，
    // 最终以首个取消/失败为准。
    while (st->remaining > 0) {
      st->draining = true;
      try {
        detail::backend_wait(st->drain_ev, -1);
      } catch (const Cancelled&) {
        // 二次取消：继续等落地
      }
      st->draining = false;
    }
  }

  if (cancelled) {
    // TaskGroup 等待者被取消：抛出携带调用者当前任务 cancel_msg 的 Cancelled，
    // 让 await 本任务的更外层能看到外层取消原因（对齐 asyncio 的 CancelledError(msg) 传播）。
    throw detail::cancelled_with_msg(detail::current().cancel_msg);
  }
  if (st->failed) detail::dispatch_completed(detail::record(st->first_failed));
}

}  // namespace weave
