// 同步原语：Semaphore / BoundedSemaphore
//
// 两者共享同一份核心簿记（下方匿名命名空间的 SemaphoreCore）：计数、FIFO 等待者
// 队列、handoff、唯一阻塞点上的排队等待与取消转交。差异只有一处——BoundedSemaphore
// 的 release() 多一步上界检查，故 release() 留在各自类内（Semaphore 永不抛，
// BoundedSemaphore 在 value >= bound 时抛），其余全部复用。
//
// 不用公有继承归一：Semaphore 是值语义句柄，`Semaphore s = bounded;` 会切片成
// 普通句柄共享同一份 Impl，SemaphoreGuard 也只认 Semaphore& —— 两者都能绕过上界
// 检查。堵住它们就得把 release 变虚，代价远大于收益。
// 共享而非各写一份的理由：下面 acquire() 的取消/同槽转交路径是仓库里最易出错的
// 代码（见 AGENTS.md 的「本环境的坑」），两份实现一旦漂移，只会在某个 delta 竞态
// 下暴露成后端相关的未定义行为。

#include <cstddef>
#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <weave/event.hpp>
#include <weave/semaphore.hpp>

namespace weave {
namespace {

// ── 共享核心簿记 ──────────────────────────────────────────────────────
//
// 两条不变量（改这里时逐条自查）：
//   1. value > 0 ⇒ waiters 为空。acquire 只在计数 > 0 时同步取得，handoff 只在
//      队列空时累加计数，故不存在「计数可用但有人排队」的空闲窗口，新到的
//      acquire 也不会插队（barging 防护，对齐 asyncio 3.10+ 的公平语义）。
//   2. 许可不在取消路径上丢失：取消与放行落在同一个 delta 时（后端取消腿优先，
//      等待者在已 granted 的情况下拿到 Cancelled），转交下一个等待者；队列已空
//      则许可归还计数。
struct SemaphoreCore {
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

  // 取得一个许可：计数 > 0 时同步取得（不挂起、不让出），计数为 0 则 FIFO 排队。
  void acquire() {
    if (value > 0) {
      --value;
      return;  // 有可用计数同步取得 ≡ asyncio 的无等待分支
    }
    const std::shared_ptr<Waiter> me = std::make_shared<Waiter>();
    waiters.push_back(me);
    try {
      me->ev.wait();
    } catch (...) {
      // 排队期间被取消（Event::wait 只会抛 Cancelled；仍用 catch (...) 守住
      // 不变量 2）。两个分支互斥——granted 只在出队后置位：
      //   · 尚未放行 → 把自己从队列摘除（许可从未发出）
      //   · 已放行   → 转交队首/归还计数，不能让许可随取消一起消失
      if (me->granted) {
        handoff();
      } else {
        for (auto it = waiters.begin(); it != waiters.end(); ++it) {
          if (it->get() == me.get()) {
            waiters.erase(it);
            break;
          }
        }
      }
      throw;
    }
  }
};

}  // namespace

// ── Semaphore ─────────────────────────────────────────────────────────
//
// Impl 只做类型区分（两张句柄各自持自己的 Impl，拷贝共享、跨类型互不转换），
// 状态与逻辑全在 SemaphoreCore。

struct Semaphore::Impl : SemaphoreCore {};

Semaphore::Semaphore(std::size_t value) : impl_(std::make_shared<Impl>()) {
  impl_->value = value;
}

bool Semaphore::locked() const noexcept {
  // 不变量 1：排队非空 ⇒ value == 0，故只需看计数。
  return impl_->value == 0;
}

void Semaphore::acquire() {
  impl_->acquire();
}

void Semaphore::release() {
  impl_->handoff();  // 无上界：放许可永远成功（≡ asyncio.Semaphore）
}

SemaphoreGuard::SemaphoreGuard(Semaphore& sem) : sem_(sem) {
  sem_.acquire();
}

SemaphoreGuard::~SemaphoreGuard() {
  sem_.release();
}

// ── BoundedSemaphore ──────────────────────────────────────────────────

// bound 只存在于 BoundedSemaphore 的 Impl：上界是它独有的概念，不写进共享核心。
// 不变量 1 在这里加强为 value <= bound——release 的检查保证进入 handoff 时
// value < bound，故空队列 +1 不越界；取消路径的 handoff 归还的是已放行的许可，
// 同理不越界。

struct BoundedSemaphore::Impl : SemaphoreCore {
  std::size_t bound = 0;  // 上界 = 初始许可数
};

BoundedSemaphore::BoundedSemaphore(std::size_t value) : impl_(std::make_shared<Impl>()) {
  impl_->value = value;
  impl_->bound = value;
}

bool BoundedSemaphore::locked() const noexcept {
  return impl_->value == 0;
}

void BoundedSemaphore::acquire() {
  impl_->acquire();  // 与 Semaphore 同一份实现（含取消转交）
}

void BoundedSemaphore::release() {
  // value >= bound ⟺ CPython 的 self._value >= self._bound_value：再放会让计数
  // 越过初值，属于 API 误用（≡ asyncio 的 ValueError）。
  // 检查必须读真实计数：许可经 handoff 回流后可能由非持有者 release，外层镜像
  // 计数（只跟踪配对调用）在这些路径下会漏判超界。
  if (impl_->value >= impl_->bound)
    throw std::logic_error("weave: BoundedSemaphore.release() released too many times");
  impl_->handoff();
}

BoundedSemaphoreGuard::BoundedSemaphoreGuard(BoundedSemaphore& sem) : sem_(sem) {
  sem_.acquire();
}

BoundedSemaphoreGuard::~BoundedSemaphoreGuard() {
  sem_.release();
}

}  // namespace weave
