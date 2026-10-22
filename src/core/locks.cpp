// Lock 实现：每个排队等待者持一个一次性 Event（交接信号），release() 只
// set() 队首那一个，其余等待者继续睡眠。全部建于 Event + 唯一阻塞点之上，
// 后端无需改动。
//
// 两条不变量：
//   1. held 只在"队列为空"时才转 false —— 交接期间保持 true，故不存在
//      "已释放但无人接手"的窗口，新来者（acquire）也不会插队。
//   2. 所有权不在取消路径上丢失：取消与交接落在同一个 delta 时（后端取消腿
//      优先，等待者在已 granted 的情况下拿到 Cancelled），转交下一个等待者。

#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <weave/event.hpp>
#include <weave/locks.hpp>

#include "core_internal.hpp"

namespace weave {

struct Lock::Impl {
  struct Waiter {
    Event ev;              // 交接信号：一次性，被 set() 即放行
    bool granted = false;  // 已交接（但持有者可能因取消未消费）
  };

  bool held = false;
  std::deque<std::shared_ptr<Waiter>> waiters;

  // 把所有权交给队首等待者；队列空则真正解锁。
  void handoff() {
    if (waiters.empty()) {
      held = false;
      return;
    }
    const std::shared_ptr<Waiter> next = std::move(waiters.front());
    waiters.pop_front();
    next->granted = true;
    next->ev.set();  // held 保持 true：所有权直接转移，不经过空闲态
  }
};

// ── Condition ─────────────────────────────────────────────────────────
//
// 等待者队列每个成员持一个一次性 Event（放行信号）。notify 只 set() 队首至多 n 个，
// 不交接锁：被唤醒者返回前经 Lock::acquire() 按 FIFO 重新取锁，期间锁归 notify 方。
//
// 取消路径守住两条不变量：
//   1. 队列摘除与重新取锁都在 catch 内完成，Cancelled 传播时锁已复位（不泄漏）；
//   2. 若取消与 notify 同槽（已被出队、拿到的是 Cancelled），摘除失败是预期形态——
//      放行已被消费，直接重新取锁后传播。

struct Condition::Impl {
  struct Waiter {
    Event ev;  // 放行信号：一次性，被 set() 即出队（由 notify 出队后置位）
  };

  Lock mutex;
  std::deque<std::shared_ptr<Waiter>> waiters;
};

Condition::Condition() : impl_(std::make_shared<Impl>()) {}

Condition::Condition(Lock lock) : impl_(std::make_shared<Impl>()) {
  impl_->mutex = lock;
}

void Condition::acquire() {
  impl_->mutex.acquire();
}

void Condition::release() {
  impl_->mutex.release();
}

bool Condition::locked() const noexcept {
  return impl_->mutex.locked();
}

Lock& Condition::mutex() noexcept {
  return impl_->mutex;
}

void Condition::wait() {
  if (!impl_->mutex.locked()) throw std::logic_error("weave: Condition.wait() on un-acquired lock");
  const std::shared_ptr<Impl::Waiter> me = std::make_shared<Impl::Waiter>();
  impl_->waiters.push_back(me);
  impl_->mutex.release();  // 原子语义的第一步：入队与放锁之间没有挂起点
  try {
    me->ev.wait();
  } catch (...) {
    // 取消（Event::wait 只会抛 Cancelled；仍用 catch (...) 守住不变量）。
    // 尚未被 notify 出队 → 摘除；已被出队（与 notify 同槽）→ 摘除失败即预期形态。
    auto& q = impl_->waiters;
    for (auto it = q.begin(); it != q.end(); ++it) {
      if (it->get() == me.get()) {
        q.erase(it);
        break;
      }
    }
    impl_->mutex.acquire();  // 先复位锁再传播（对齐 asyncio 3.13+）
    throw;
  }
  impl_->mutex.acquire();  // 唤醒后重新取锁，wait 返回时临界区仍成立
}

void Condition::notify(std::size_t n) {
  if (!impl_->mutex.locked())
    throw std::logic_error("weave: Condition.notify() on un-acquired lock");
  auto& q = impl_->waiters;
  while (n > 0 && !q.empty()) {
    const std::shared_ptr<Impl::Waiter> next = std::move(q.front());
    q.pop_front();  // 先出队再放行：被唤醒者返回队列里已没有自己
    next->ev.set();
    --n;
  }
}

void Condition::notify_all() {
  notify(impl_->waiters.size());
}

Lock::Lock() : impl_(std::make_shared<Impl>()) {}

bool Lock::locked() const noexcept {
  return impl_->held;
}

void Lock::acquire() {
  if (!impl_->held) {
    impl_->held = true;
    return;  // 空闲锁同步取得：不挂起、不让出（≡ asyncio 的无等待分支）
  }
  const std::shared_ptr<Impl::Waiter> me = std::make_shared<Impl::Waiter>();
  impl_->waiters.push_back(me);
  try {
    me->ev.wait();
  } catch (...) {
    // 排队期间被取消（Event::wait 只会抛 Cancelled；仍用 catch (...) 守住
    // "所有权不丢"这条不变量）。两个分支互斥——granted 只在出队后置位：
    //   · 尚未交接 → 把自己从队列摘除，锁仍归当前持有者
    //   · 已交接   → 转交队首，不能让锁随取消一起消失
    if (me->granted) {
      impl_->handoff();
    } else {
      auto& q = impl_->waiters;
      for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->get() == me.get()) {
          q.erase(it);
          break;
        }
      }
    }
    throw;
  }
}

void Lock::release() {
  if (!impl_->held) throw std::logic_error("weave: Lock.release() on unlocked lock");
  impl_->handoff();
}

LockGuard::LockGuard(Lock& lock) : lock_(lock) {
  lock_.acquire();
}

LockGuard::~LockGuard() {
  lock_.release();
}

}  // namespace weave
