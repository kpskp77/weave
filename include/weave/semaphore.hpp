#pragma once

// 通信原语：Semaphore / SemaphoreGuard / BoundedSemaphore /
// BoundedSemaphoreGuard，语义分别对齐 asyncio.Semaphore、
// `async with semaphore:`、asyncio.BoundedSemaphore。
// 全部建于 Event + 唯一阻塞点之上（每个排队者持一个一次性 Event），后端零改动。
// 句柄语义与 Lock/Event 一致：拷贝共享同一个信号量，不是深拷贝。

#include <cstddef>
#include <memory>

namespace weave {

// ── Semaphore（asyncio.Semaphore）─────────────────────────────────────
//
// 计数信号量：
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

// ── SemaphoreGuard ────────────────────────────────────────────────────
//
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

// ── BoundedSemaphore（asyncio.BoundedSemaphore）────────────────────────
//
// 有界信号量：与 Semaphore 完全同构（同样的计数取得、FIFO 排队、barging
// 防护、取消不丢许可，不变量见上方 Semaphore 一节），唯一差异是 release()
// 带上界检查——内部计数达到构造传入的上界（= 初始许可数）后再 release
// 抛 std::logic_error（对应 asyncio 的
// ValueError("BoundedSemaphore released too many times")）。
// 与 Lock 的「未持有 release 抛错」同源：上界是信号量版的二值约束，越界释放
// 会让计数簿记失去意义。检查直接读真实计数（value >= bound），与 CPython 的
// `self._value >= self._bound_value` 一一对应——不能用外层镜像计数替代，
// 许可可经 handoff 回流后由非持有者 release，镜像在那些路径下会漏判超界。
//
// 实现上与 Semaphore 共享同一份核心簿记（src/core/semaphore.cpp 的
// SemaphoreCore），仅多了 bound 成员与 release 这一处检查；不采用公有继承：
// 值语义句柄会被切片/被 SemaphoreGuard 绕过上界检查。
class BoundedSemaphore {
public:
  explicit BoundedSemaphore(std::size_t value = 1);

  // 取得一个许可；计数为 0 时挂起至被放行。排队期间被取消 → 抛 Cancelled，
  // 此时不持有许可。与 Semaphore::acquire 完全一致。
  void acquire();

  // 放一个许可；计数已达上界（已放许可数等于初始值）→ 抛 std::logic_error。
  // 不挂起。除此之外与 Semaphore::release 一致。
  void release();

  [[nodiscard]] bool locked() const noexcept;  // acquire() 当前是否会阻塞

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// ── BoundedSemaphoreGuard ─────────────────────────────────────────────
//
// asyncio 的 `async with bounded_semaphore:` 在 C++ 侧的对应物：作用域即
// 持许可区间。构造调用 acquire()（可能挂起；抛 Cancelled 时构造失败、不持
// 许可），析构调用 release()（不挂起）。取消沿栈展开时同样释放。
// 持有句柄副本（BoundedSemaphore 可拷贝、共享同一个信号量），不依赖调用方
// 句柄的寿命。guard 与 acquire 严格配对——误用（析构多于构造导致的超界
// release）按 noexcept 语义 terminate。
class BoundedSemaphoreGuard {
public:
  explicit BoundedSemaphoreGuard(BoundedSemaphore& sem);
  ~BoundedSemaphoreGuard();

  BoundedSemaphoreGuard(const BoundedSemaphoreGuard&) = delete;
  BoundedSemaphoreGuard& operator=(const BoundedSemaphoreGuard&) = delete;

private:
  BoundedSemaphore sem_;
};

}  // namespace weave
