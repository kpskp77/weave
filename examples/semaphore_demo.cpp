// 通信原语常见用法：Semaphore（计数信号量）。
//   · SemaphoreGuard：作用域即持许可区间（≡ Python 的 `async with semaphore:`）
//   · 计数互斥：2 个许可允许两个任务同时进入临界区，第三个排队
//   · 排队者被取消：许可不跟着走，仍在信号量里
// 同步完全靠事件传递，不假设任何"睡多久之后应该发生什么"。

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(semaphore_demo) {
  // ── SemaphoreGuard：离开作用域自动归还许可 ────────────────────────────
  {
    Semaphore s;
    {
      SemaphoreGuard g(s);
      if (!s.locked()) throw std::logic_error("semaphore_demo: guard must hold the permit");
      std::printf("[semaphore_demo] inside guarded section\n");
    }
    if (s.locked()) throw std::logic_error("semaphore_demo: guard must release at scope exit");
    std::printf("[semaphore_demo] SemaphoreGuard released at scope exit\n");
  }

  // ── 计数互斥：a、b 同时持有（2 个许可），c 排队等其中一个归还 ─────────
  {
    Semaphore s(2);
    int depth = 0;
    int max_depth = 0;
    Event a_in;
    auto worker = [&](const char* name, Event* wake) {
      if (wake) wake->wait();
      s.acquire();
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      std::printf("[semaphore_demo] %s in critical section (depth=%d)\n", name, depth);
      sleep(0_fs);  // 让出：给其它任务竞争的机会
      --depth;
      s.release();
    };
    auto a = create_task("sem_a", [&] {
      worker("a", nullptr);
      a_in.set();
    });
    auto b = create_task("sem_b", [&] { worker("b", &a_in); });
    auto c = create_task("sem_c", [&] { worker("c", &a_in); });
    gather(a, b, c);
    if (max_depth != 2) throw std::logic_error("semaphore_demo: depth exceeded permit count");
    std::printf("[semaphore_demo] counting ok: max depth == permit count\n");
  }

  // ── 排队者被取消：许可仍留在信号量里 ──────────────────────────────────
  {
    Semaphore s(1);
    s.acquire();  // 主任务持有唯一许可
    Event queued;
    auto waiter = create_task("sem_waiter", [&] {
      queued.set();
      try {
        s.acquire();
      } catch (const Cancelled&) {
        std::printf("[semaphore_demo] waiter cancelled while queued\n");
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
    if (!s.locked())
      throw std::logic_error("semaphore_demo: permit must not leak with the cancelled waiter");
    s.release();  // 许可仍在：释放后信号量回到空闲
    std::printf("[semaphore_demo] cancel while queued keeps the permit in the semaphore\n");
  }
}
