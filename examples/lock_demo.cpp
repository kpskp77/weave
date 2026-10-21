// 通信原语常见用法：Lock（互斥锁）。
//   · LockGuard：作用域即临界区（≡ Python 的 `async with lock:`）
//   · 互斥 + FIFO 交接：排队者按 acquire() 顺序依次进入临界区
//   · 排队者被取消：锁仍归持有者，取消者不持有、也不算失败
// 同步完全靠事件传递，不假设任何"睡多久之后应该发生什么"。

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(locks_demo) {
  // ── LockGuard：离开作用域自动释放 ─────────────────────────────────────
  {
    Lock l;
    {
      LockGuard g(l);
      if (!l.locked()) throw std::logic_error("locks_demo: guard must hold the lock");
      std::printf("[locks_demo] inside critical section\n");
    }
    if (l.locked()) throw std::logic_error("locks_demo: guard must release at scope exit");
    std::printf("[locks_demo] LockGuard released at scope exit\n");
  }

  // ── 互斥 + FIFO：a 先排队，b 在 a 释放后接手 ──────────────────────────
  {
    Lock l;
    l.acquire();  // 主任务先占住，让两个任务排队
    Event queued_a, queued_b, done_b;
    auto a = create_task("lock_a", [&] {
      queued_a.set();
      l.acquire();
      std::printf("[locks_demo] a in critical section\n");
      l.release();
    });
    auto b = create_task("lock_b", [&] {
      queued_a.wait();  // a 先入队，b 才起身（事件驱动，不靠睡眠时长）
      queued_b.set();
      l.acquire();
      std::printf("[locks_demo] b in critical section\n");
      done_b.set();
      l.release();
    });
    queued_a.wait();
    queued_b.wait();  // a、b 都已按顺序排队
    l.release();      // 交接给 a；a 释放后 b 接手
    gather(a, b);
    if (!done_b.is_set()) throw std::logic_error("locks_demo: b must run after a");
    std::printf("[locks_demo] FIFO handoff ok\n");
  }

  // ── 排队者被取消：锁不跟着走 ──────────────────────────────────────────
  {
    Lock l;
    l.acquire();  // 主任务持有
    Event queued;
    auto waiter = create_task("lock_waiter", [&] {
      queued.set();
      try {
        l.acquire();
      } catch (const Cancelled&) {
        std::printf("[locks_demo] waiter cancelled while queued\n");
        throw;  // 取消继续向上传播（吞掉会让任务错误地以 DONE 结束）
      }
    });
    queued.wait();    // waiter 已入队
    waiter.cancel();  // 队列中的等待者被取消
    try {
      waiter.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!l.locked()) throw std::logic_error("locks_demo: lock must stay with its owner");
    l.release();
    std::printf("[locks_demo] cancel while queued keeps the lock with its owner\n");
  }
}
