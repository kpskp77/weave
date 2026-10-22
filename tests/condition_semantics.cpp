// 通信原语自检：Condition 的 asyncio.Condition 语义——持锁要求（wait/notify 未持锁
// 抛 std::logic_error）、wait 原子放锁与唤醒后重新取锁、notify(n) 只放行队首 n 个、
// notify_all 全放行、谓词轮询（wait_for = 反复 wait）、等待者取消（摘除 + 重新取锁
// 后再传播，锁不泄漏）、复用外部锁与句柄拷贝共享同一条件变量。
// 同步完全事件驱动（纯 delta cycle），不依赖真实时钟。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）；
// 与其它 *_semantics.cpp 共用断言体，由 weave_smoke_sc / weave_smoke_svdpi
// 两个后端回归。

#include <stdexcept>
#include <vector>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(condition_semantics) {
  // case 1: 构造即持独立锁且未锁定；wait()/notify() 未持锁 → std::logic_error
  // （≡ asyncio 的 RuntimeError("cannot wait/notify on un-acquired lock")）
  run_case(__func__, "case1", [&] {
    Condition c;
    if (c.locked()) throw std::logic_error("case1: fresh Condition must be unlocked");
    bool threw_wait = false;
    try {
      c.wait();
    } catch (const std::logic_error&) {
      threw_wait = true;
    }
    if (!threw_wait) throw std::logic_error("case1: wait on un-acquired lock must throw");
    bool threw_notify = false;
    try {
      c.notify();
    } catch (const std::logic_error&) {
      threw_notify = true;
    }
    if (!threw_notify) throw std::logic_error("case1: notify on un-acquired lock must throw");
    c.acquire();
    c.notify();  // 空等待队列 + 已持锁：合法 no-op
    c.release();
  });

  // case 2: wait 原子放锁——等待者阻塞后锁即释放（第三方 acquire 即刻可取）；
  // notify 放行后 wait 返回前锁被重新取得（wait 返回时临界区成立）
  run_case(__func__, "case2", [&] {
    Condition c;
    Event r, done;
    auto w = create_task("c2_waiter", [&] {
      c.acquire();
      r.set();   // 持锁标记：r.wait() 返回时 waiter 必然尚未 wait
      c.wait();  // 原子：入队 + 放锁 + 阻塞
      if (!c.locked()) throw std::logic_error("case2: wait must re-acquire before return");
      c.release();
      done.set();
    });
    r.wait();
    c.acquire();  // 阻塞至 waiter 在 wait() 内放锁 → 「等待期间锁已释放」得证
    c.notify();   // 放行唯一等待者；锁仍归主任务
    c.release();  // waiter 重新取锁、检查、释放
    done.wait();
    w.await();
    if (c.locked()) throw std::logic_error("case2: waiter must have released");
  });

  // case 3: notify(n) 只放行队首 n 个（FIFO），其余继续等待；
  // notify_all 放行剩余者
  // 入队次序靠「前一个等待者先 set 入队标记、后一个才开始」的链式事件确定。
  run_case(__func__, "case3", [&] {
    Condition c;
    Event r1, r2, r3, d1, d2, d3;
    auto w1 = create_task("c3_1", [&] {
      c.acquire();
      r1.set();
      c.wait();
      d1.set();
      c.release();
    });
    auto w2 = create_task("c3_2", [&] {
      r1.wait();
      c.acquire();
      r2.set();
      c.wait();
      d2.set();
      c.release();
    });
    auto w3 = create_task("c3_3", [&] {
      r2.wait();
      c.acquire();
      r3.set();
      c.wait();
      d3.set();
      c.release();
    });
    r1.wait();
    r2.wait();
    r3.wait();  // 三个等待者已按 w1,w2,w3 入队且都在 wait() 内阻塞，锁空闲
    c.acquire();
    c.notify(2);  // 只放行 w1,w2
    c.release();  // 交出锁：w1 经 Lock FIFO 先取，逐个走完
    d2.wait();    // d2 完成 ⟹ w1 已先完成（锁 FIFO）
    if (!d1.is_set()) throw std::logic_error("case3: w1 must finish before w2");
    if (d3.is_set()) throw std::logic_error("case3: w3 must still be waiting after notify(2)");
    c.acquire();  // w1/w2 已释放，锁空闲
    c.notify_all();
    c.release();
    d3.wait();
    gather(w1, w2, w3);
  });

  // case 4: 谓词轮询——wait 被放行但谓词仍为假时必须再入队等下一轮
  // （wait_for 的循环语义）；每轮 wait 都先放锁、唤醒后重新取锁
  run_case(__func__, "case4", [&] {
    Condition c;
    int wakes = 0;
    Event r1, r2;
    auto w = create_task("c4_waiter", [&] {
      c.acquire();
      r1.set();
      c.wait();
      ++wakes;
      r2.set();  // 即将第二次 wait（此刻持锁）
      c.wait();
      ++wakes;
      c.release();
    });
    r1.wait();
    c.acquire();  // 阻塞至第一次放锁
    c.notify_all();
    c.release();  // 第一次放行
    r2.wait();
    c.acquire();  // 阻塞至第二次放锁
    if (wakes != 1) throw std::logic_error("case4: waiter must be back in wait after false wake");
    c.notify_all();
    c.release();  // 第二次放行
    w.await();
    if (wakes != 2) throw std::logic_error("case4: waiter must wake exactly twice");
  });

  // case 5: 等待者取消 → 从队列摘除、重新取锁、再传播 Cancelled；
  // 摘除后 notify 不得再放行它（否则在已销毁的栈帧上 resume），锁也不随取消泄漏
  run_case(__func__, "case5", [&] {
    Condition c;
    Event r, cancelled, ack;
    auto victim = create_task("c5_victim", [&] {
      c.acquire();
      r.set();
      try {
        c.wait();
        throw std::logic_error("case5: cancelled waiter must not pass wait");
      } catch (const Cancelled&) {
        if (!c.locked()) throw std::logic_error("case5: lock must be re-acquired before Cancelled");
        cancelled.set();
        ack.wait();  // 等主任务验证完毕再放锁（避免主任务等待时死锁）
        c.release();
        throw;
      }
    });
    r.wait();
    c.acquire();  // 阻塞至 victim 在 wait() 内放锁；此后锁归主任务，victim 在队列中
    victim.cancel();
    // victim 的 catch 要先重新取锁，主任务必须先把锁让出
    c.release();
    cancelled.wait();  // victim 已重新取锁、通过 locked() 检查
    if (!c.locked()) throw std::logic_error("case5: re-acquired lock must belong to victim");
    ack.set();
    try {
      victim.await();
      throw std::logic_error("case5: victim must end CANCELLED");
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (c.locked()) throw std::logic_error("case5: victim must have released in its catch");
    // 关键不变量：victim 已摘除——notify 不得再 set 它的一次性事件
    c.acquire();
    c.notify_all();
    c.release();
  });

  // case 6: 取消与 notify 同槽——notify_all() 与 cancel() 之间无挂起点，victim
  // 处于「已出队但拿到 Cancelled」的竞态形态（后端取消腿优先）：放行已被消费，
  // 锁被重新取得后传播取消，且不影响其余等待者
  run_case(__func__, "case6", [&] {
    Condition c;
    Event r1, r2, cancelled, ack, got;
    auto victim = create_task("c6_victim", [&] {
      c.acquire();
      r1.set();
      try {
        c.wait();
        throw std::logic_error("case6: cancelled waiter must not pass wait");
      } catch (const Cancelled&) {
        if (!c.locked()) throw std::logic_error("case6: lock must be re-acquired before Cancelled");
        cancelled.set();
        ack.wait();
        c.release();
        throw;
      }
    });
    auto peer = create_task("c6_peer", [&] {
      r1.wait();
      c.acquire();
      r2.set();
      c.wait();
      got.set();
      c.release();
    });
    r1.wait();
    r2.wait();  // 条件队列为 victim,peer；锁空闲
    c.acquire();
    c.notify_all();   // 两者出队放行
    victim.cancel();  // 与放行同槽：victim 拿 Cancelled，peer 正常
    c.release();
    cancelled.wait();
    ack.set();
    got.wait();  // peer 不受 victim 取消影响，正常走完
    peer.await();
    try {
      victim.await();
      throw std::logic_error("case6: victim must end CANCELLED");
    } catch (const Cancelled&) {
      // 预期路径
    }
  });

  // case 7: wait_for 谓词已为真时不阻塞、不让出（同步通过，≡ asyncio 的无等待分支）
  run_case(__func__, "case7", [&] {
    Condition c;
    int items = 0;
    c.acquire();
    ++items;
    c.release();
    Event ran;
    auto consumer = create_task("c7_consumer", [&] {
      c.acquire();
      if (c.locked() == false) throw std::logic_error("case7: consumer must hold the lock");
      c.wait_for([&] { return items > 0; });  // 谓词已真：不得挂起
      if (!c.locked()) throw std::logic_error("case7: wait_for must hold the lock on exit");
      --items;
      c.release();
      ran.set();
    });
    consumer.await();
    if (!ran.is_set()) throw std::logic_error("case7: consumer must have run");
    if (items != 0) throw std::logic_error("case7: item must be consumed");
  });

  // case 8: 复用外部锁 + 句柄拷贝共享同一条件变量（与 Lock/Event 句柄语义一致）
  run_case(__func__, "case8", [&] {
    Lock l;
    Condition c(l);  // 对齐 asyncio.Condition(lock)：底层即传入的锁
    c.acquire();
    if (!l.locked()) throw std::logic_error("case8: condition must hold the given lock");
    Condition d = c;  // 拷贝共享
    d.release();
    if (l.locked()) throw std::logic_error("case8: copies must share state with the given lock");
  });

  report(__func__);
}
