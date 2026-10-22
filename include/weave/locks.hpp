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

#include <cstddef>
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

// ── Condition（asyncio.Condition）──────────────────────────────────────

// 条件变量，语义对齐 asyncio.Condition：
//   · 构造即持有一把底层 Lock（可传入已有 Lock 复用，对齐 asyncio.Condition(lock)）；
//   · acquire()/release()/locked() 委托底层锁（≡ `async with cond:` 的进入/退出，
//     持锁区间也可用 LockGuard guard(cond.mutex()) 表达）；
//   · wait()：要求已持锁。原子地「入队 → 放锁 → 阻塞」，被 notify 唤醒后
//     **重新取得锁**才返回（与 asyncio 一致：wait 返回时谓词检查所依赖的
//     临界区仍然成立）。等待期间被取消 → 先从队列摘除、重新取锁、再抛
//     Cancelled（锁不随取消泄漏，对齐 asyncio 3.13+ 的「先 re-acquire 再传播
//     取消」）；
//   · wait_for(pred)：反复 wait() 直到 pred() 为真（对齐 asyncio 的
//     `while not predicate(): await cond.wait()`）；
//   · notify(n=1)/notify_all()：唤醒队首 n 个/全部等待者，要求已持锁
//     （≡ asyncio 的 RuntimeError("cannot notify on un-acquired lock")）。
//     唤醒只是放行：被唤醒者仍要按 Lock 的 FIFO 排队重新取锁，notify 不交接锁。
//
// 全部建于 Lock + Event + 唯一阻塞点之上，后端零改动。
// 句柄语义与 Lock/Event 一致：拷贝共享同一个条件变量。
class Condition {
public:
  Condition();
  explicit Condition(Lock lock);  // 复用已有锁，对齐 asyncio.Condition(lock)

  void acquire();  // 取得底层锁（可能挂起）
  void release();  // 释放底层锁
  [[nodiscard]] bool locked() const noexcept;
  [[nodiscard]] Lock& mutex() noexcept;  // 底层锁句柄（asyncio 的 .lock 属性）

  // 要求已持锁；返回时锁已被重新取得。见类注释的取消语义。
  void wait();

  // 反复 wait() 直至 pred() 为真；返回时锁已持有。
  template <typename Pred>
  void wait_for(Pred pred) {
    while (!pred()) wait();
  }

  void notify(std::size_t n = 1);  // 唤醒队首至多 n 个等待者；要求已持锁
  void notify_all();               // 唤醒全部等待者；要求已持锁

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace weave
