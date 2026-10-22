#pragma once

// 通信原语：Semaphore（计数信号量）。语义对齐 asyncio.Semaphore：
//   · 构造传入许可数（默认 1，≡ asyncio.Semaphore(value=1)；0 合法，表示
//     首个 acquire 必须排队）；
//   · acquire()：计数 > 0 时立即取得（不挂起、不让出），计数为 0 则 FIFO
//     排队等待放行——唯一的阻塞点在排队上；
//   · release()：放一个许可——有等待者则直接交给队首，无人等待则计数 +1。
//     与 Lock 不同，release 永远成功、不要求先 acquire（信号量不记录持有者，
//     对齐 asyncio）；
//   · locked()：acquire() 当前是否会阻塞。
//
// 与 Lock 同一套两条不变量（详见 src/core/semaphore.cpp）：
//   1. 计数为 0 才可能有排队者——不存在「计数可用但有人排队」的空闲窗口，
//      新到的 acquire 不会插队（barging 防护，对齐 asyncio 3.10+ 的公平语义）；
//   2. 许可不在取消路径上丢失：取消与放行同槽发生（已 granted 但拿到的是
//      Cancelled）时，许可转交下一个等待者；队列已空则归还计数。
//
// 全部建于 Event + 唯一阻塞点之上（每个排队者持一个一次性 Event），后端零改动。
// 句柄语义与 Lock/Event 一致：拷贝共享同一个信号量，不是深拷贝。

#include <cstddef>
#include <memory>

namespace weave {

class Semaphore {
public:
  explicit Semaphore(std::size_t value = 1);

  // 取得一个许可；计数为 0 时挂起至被放行。排队期间被取消 → 抛 Cancelled，
  // 此时不持有许可（取消在阻塞点注入，是协作式取消的常规形态）。
  void acquire();

  // 放一个许可：等待者非空则直接交给队首，否则计数 +1。不挂起、永远成功。
  void release();

  [[nodiscard]] bool locked() const noexcept;  // acquire() 当前是否会阻塞

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// asyncio 的 `async with semaphore:` 在 C++ 侧的对应物：作用域即持许可区间。
// 构造调用 acquire()（可能挂起；抛 Cancelled 时构造失败、不持许可），
// 析构调用 release()（不挂起）。取消沿栈展开时同样释放，等价于靠
// __aexit__ 释放。
// 持有句柄副本（Semaphore 可拷贝、共享同一个信号量），不依赖调用方句柄的寿命。
// 与 Semaphore 本身一致，guard 析构的 release 永远成功（信号量不记录持有者）。
class SemaphoreGuard {
public:
  explicit SemaphoreGuard(Semaphore& sem);
  ~SemaphoreGuard();

  SemaphoreGuard(const SemaphoreGuard&) = delete;
  SemaphoreGuard& operator=(const SemaphoreGuard&) = delete;

private:
  Semaphore sem_;
};

}  // namespace weave
