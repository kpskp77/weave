// 通信原语常见用法：Condition（条件变量）。
//   · 生产者/消费者：队列为空时消费者挂起，produce 后 notify 放行
//   · wait_for：谓词轮询，返回时锁已重新取得、临界区成立
//   · 等待者被取消：从队列摘除、重新取锁后再传播，锁不泄漏
// 同步完全靠事件传递，不假设任何"睡多久之后应该发生什么"。

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;

WEAVE_TEST(condition_demo) {
  // ── 生产者/消费者：空队列挂起，produce 后放行 ────────────────────────
  {
    Condition c;
    int items = 0;
    Event consumer_in;
    auto consumer = create_task("cond_consumer", [&] {
      c.acquire();
      consumer_in.set();  // 即将首次检查谓词
      c.wait_for([&] { return items > 0; });
      --items;  // 谓词在临界区内成立才消费
      c.release();
      std::printf("[condition_demo] consumer got an item\n");
    });
    consumer_in.wait();
    c.acquire();  // 阻塞至 consumer 在 wait() 内放锁（队列为空、确已挂起）
    if (items != 0) throw std::logic_error("condition_demo: queue must start empty");
    ++items;     // produce（临界区内修改谓词）
    c.notify();  // 放行等待者
    c.release();
    consumer.await();
    if (items != 0) throw std::logic_error("condition_demo: item must be consumed");
    std::printf("[condition_demo] produce/notify/consume handshake ok\n");
  }

  // ── wait_for 谓词已为真：同步通过，不挂起 ─────────────────────────────
  {
    Condition c;
    int items = 1;
    c.acquire();
    ++items;
    c.release();
    auto consumer = create_task("cond_consumer2", [&] {
      c.acquire();
      c.wait_for([&] { return items > 0; });  // 立即返回
      --items;
      c.release();
    });
    consumer.await();
    if (items != 1) throw std::logic_error("condition_demo: must consume exactly one");
    std::printf("[condition_demo] wait_for with true predicate passes without blocking\n");
  }

  // ── 等待者被取消：锁随 catch 内的重新取锁/释放回到可用态 ───────────────
  {
    Condition c;
    Event waiting;
    auto waiter = create_task("cond_waiter", [&] {
      c.acquire();
      waiting.set();
      try {
        c.wait();
        throw std::logic_error("condition_demo: cancelled waiter must not pass wait");
      } catch (const Cancelled&) {
        c.release();  // wait 已重新取锁；释放后传播取消
        throw;
      }
    });
    waiting.wait();
    c.acquire();  // 阻塞至 waiter 在 wait() 内放锁
    waiter.cancel();
    c.release();  // 让出锁：wait 的取消路径要先重新取锁
    try {
      waiter.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (c.locked()) throw std::logic_error("condition_demo: lock must be free after cancel");
    std::printf("[condition_demo] cancelled waiter re-acquired then released the lock\n");
  }
}
