// 同步原语：Semaphore

#include <cstddef>
#include <deque>
#include <memory>
#include <utility>
#include <weave/event.hpp>
#include <weave/semaphore.hpp>

namespace weave {

// ── Semaphore ─────────────────────────────────────────────────────────
//
// Semaphore 实现：Lock 的计数推广——每个排队等待者持一个一次性 Event（放行
// 信号），release() 只 set() 队首那一个，其余等待者继续睡眠。全部建于
// Event + 唯一阻塞点之上，后端无需改动。
//
// 两条不变量：
//   1. value > 0 ⇒ waiters 为空。acquire 只在计数 > 0 时同步取得，release 只在
//      队列空时累加计数，故不存在「计数可用但有人排队」的空闲窗口，新到的
//      acquire 也不会插队（barging 防护，对齐 asyncio 3.10+ 的公平语义）。
//   2. 许可不在取消路径上丢失：取消与放行落在同一个 delta 时（后端取消腿
//      优先，等待者在已 granted 的情况下拿到 Cancelled），转交下一个等待者；
//      队列已空则许可归还计数。

struct Semaphore::Impl {
  struct Waiter {
    Event ev;              // 放行信号：一次性，被 set() 即取得许可
    bool granted = false;  // 已放行（但取得者可能因取消未消费）
  };

  std::size_t value = 0;
  std::deque<std::shared_ptr<Waiter>> waiters;

  // 把许可交给队首等待者；队列空则归还计数（value + 1）。
  void handoff() {
    if (waiters.empty()) {
      ++value;
      return;
    }
    const std::shared_ptr<Waiter> next = std::move(waiters.front());
    waiters.pop_front();
    next->granted = true;
    next->ev.set();
  }
};

Semaphore::Semaphore(std::size_t value) : impl_(std::make_shared<Impl>()) {
  impl_->value = value;
}

bool Semaphore::locked() const noexcept {
  // 不变量 1：排队非空 ⇒ value == 0，故只需看计数。
  return impl_->value == 0;
}

void Semaphore::acquire() {
  if (impl_->value > 0) {
    --impl_->value;
    return;  // 有可用计数同步取得：不挂起、不让出（≡ asyncio 的无等待分支）
  }
  const std::shared_ptr<Impl::Waiter> me = std::make_shared<Impl::Waiter>();
  impl_->waiters.push_back(me);
  try {
    me->ev.wait();
  } catch (...) {
    // 排队期间被取消（Event::wait 只会抛 Cancelled；仍用 catch (...) 守住
    // 「许可不丢」这条不变量）。两个分支互斥——granted 只在出队后置位：
    //   · 尚未放行 → 把自己从队列摘除（许可从未发出）
    //   · 已放行   → 转交队首/归还计数，不能让许可随取消一起消失
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

void Semaphore::release() {
  impl_->handoff();
}

SemaphoreGuard::SemaphoreGuard(Semaphore& sem) : sem_(sem) {
  sem_.acquire();
}

SemaphoreGuard::~SemaphoreGuard() {
  sem_.release();
}

}  // namespace weave
