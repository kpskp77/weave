#pragma once

// 通信原语：Lock（互斥锁）。语义对齐 asyncio.Lock：
//   · acquire()：锁空闲则立即取得（不挂起、不让出），已被持有则排队等待
//     ——唯一的阻塞点在排队上；
//   · release()：把锁交给队首等待者（FIFO 公平），无人排队则解锁；不挂起；
//   · locked()：当前是否被持有。
// 与 asyncio 一致，锁不记录持有者、不可重入：同一任务重复 acquire() 会等自己
// 释放（自锁），任意任务都可以 release()。
//
// 交接在 release() 内完成、held 全程为 true：不存在"已释放但还没被接手"的
// 空闲窗口（asyncio 在该窗口里 locked() 会短暂返回 False，新来者也不插队）。
// 等待者被取消时锁不会丢：若取消与交接同槽发生（锁已交接给它、但它拿到的是
// Cancelled），所有权转交下一个等待者，见 src/core/locks.cpp。
//
// 全部建于 Event + 唯一阻塞点之上（每个排队者持一个一次性 Event），后端零改动。

#include <memory>

namespace weave {

class Lock {
public:
  Lock();

  // 取得锁；已被持有时挂起至交接给自己。排队期间被取消 → 抛 Cancelled，
  // 此时不持有锁（取消在阻塞点注入，是协作式取消的常规形态）。
  void acquire();

  // 释放并交接：唤醒队首等待者，队列空则真正解锁。未持有时抛
  // std::logic_error（对应 asyncio 的 RuntimeError("Lock is not acquired.")）。
  void release();

  [[nodiscard]] bool locked() const noexcept;

private:
  struct Impl;
  // 句柄语义与 Event 一致：拷贝共享同一把锁，不是深拷贝。
  std::shared_ptr<Impl> impl_;
};

// asyncio 的 `async with lock:` 在 C++ 侧的对应物：作用域即持锁区间。
// 构造调用 acquire()（可能挂起；抛 Cancelled 时构造失败、不持锁），
// 析构调用 release()（不挂起）。取消沿栈展开时同样释放，等价于靠
// __aexit__ 释放。
// 持有句柄副本（Lock 可拷贝、共享同一把锁），不依赖调用方句柄的寿命。
// 误用（例如手工 release() 之后再析构）按 noexcept 语义 terminate。
class LockGuard {
public:
  explicit LockGuard(Lock& lock);
  ~LockGuard();

  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

private:
  Lock lock_;
};

}  // namespace weave
