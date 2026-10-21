// 通信原语自检：Lock 的 asyncio.Lock 语义——互斥、FIFO 交接、未持有 release
// 报错、排队者取消（含取消与交接同槽时所有权不丢失）、句柄拷贝共享同一把锁、
// LockGuard 作用域释放与取消展开释放。
// 同步完全事件驱动（纯 delta cycle），不依赖真实时钟。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）；
// 与其它 *_semantics.cpp 共用断言体，由 weave_smoke_sc / weave_smoke_svdpi
// 两个后端回归。

#include <stdexcept>
#include <vector>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(lock_semantics) {
  // case 1: 空闲锁 → acquire 同步取得（不挂起、不让出），locked() 跟随状态
  run_case(__func__, "case1", [&] {
    Lock l;
    if (l.locked()) throw std::logic_error("case1: fresh Lock must be unlocked");
    Event started;
    auto probe = create_task("c1_probe", [&] { started.set(); });
    l.acquire();  // 空闲锁：不得让出调度
    if (started.is_set()) throw std::logic_error("case1: acquire on free lock must not yield");
    if (!l.locked()) throw std::logic_error("case1: locked() must reflect acquire");
    l.release();
    if (l.locked()) throw std::logic_error("case1: locked() must reflect release");
    probe.await();  // probe 此刻才获得调度
    if (!started.is_set()) throw std::logic_error("case1: probe must run after yield");
  });

  // case 2: 互斥——持有者跨挂起点（sleep(0) 让出一拍）仍在临界区内，
  // 竞争者必须等 release，于是临界区进入深度恒为 1
  run_case(__func__, "case2", [&] {
    Lock l;
    int depth = 0;
    int max_depth = 0;
    Event a_in;
    auto a = create_task("c2_a", [&] {
      l.acquire();
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      a_in.set();
      sleep(0_fs);  // 主动让出：给竞争者尝试 acquire 的机会
      --depth;
      l.release();
    });
    auto b = create_task("c2_b", [&] {
      a_in.wait();
      l.acquire();  // a 仍持锁 → 阻塞
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      --depth;
      l.release();
    });
    gather(a, b);  // 两个任务都落地（b 只在 a 释放后才进临界区）
    if (max_depth != 1)
      throw std::logic_error("case2: two tasks entered the critical section at once");
  });

  // case 3: FIFO 公平——等待者按 acquire() 调用顺序获得锁
  // 队列次序靠"前一个等待者先入队、后一个才开始"的链式事件确定：
  // r1.set() 在 acquire 之前，故 r1.wait() 返回时 t1 必然已排队。
  run_case(__func__, "case3", [&] {
    Lock l;
    l.acquire();  // 主任务先持有，制造排队
    std::vector<int> order;
    Event r1, r2, r3;
    auto t1 = create_task("c3_1", [&] {
      r1.set();
      l.acquire();
      order.push_back(1);
      l.release();
    });
    auto t2 = create_task("c3_2", [&] {
      r1.wait();
      r2.set();
      l.acquire();
      order.push_back(2);
      l.release();
    });
    auto t3 = create_task("c3_3", [&] {
      r2.wait();
      r3.set();
      l.acquire();
      order.push_back(3);
      l.release();
    });
    r1.wait();    // t1 已在队列首
    r2.wait();    // t2 排在其后
    r3.wait();    // t3 排在队尾
    l.release();  // 逐个交接
    gather(t1, t2, t3);
    if (order != std::vector<int>({1, 2, 3}))
      throw std::logic_error("case3: waiters must be served in FIFO order");
    if (l.locked()) throw std::logic_error("case3: lock must be free after last release");
  });

  // case 4: release() 未持有 → std::logic_error（≡ asyncio 的 RuntimeError）
  run_case(__func__, "case4", [&] {
    Lock l;
    bool threw = false;
    try {
      l.release();
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case4: release on unlocked Lock must throw");
    l.acquire();
    l.release();  // 正常路径不受影响
  });

  // case 5: 排队者被取消 → 抛 Cancelled、不持有锁，锁仍归持有者并交给其余等待者
  run_case(__func__, "case5", [&] {
    Lock l;
    l.acquire();  // 主任务持有
    Event r1, r2, caught, got;
    auto victim = create_task("c5_victim", [&] {
      r1.set();
      try {
        l.acquire();
        throw std::logic_error("case5: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c5_peer", [&] {
      r1.wait();
      r2.set();
      l.acquire();  // 排在 victim 之后
      got.set();
      l.release();
    });
    r1.wait();        // victim 已排队
    r2.wait();        // peer 已排在 victim 之后
    victim.cancel();  // 只取消 victim，此时尚未交接
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!l.locked()) throw std::logic_error("case5: lock must stay with its owner");
    if (got.is_set()) throw std::logic_error("case5: cancel must not hand off the lock");
    l.release();  // 摘除 victim 后，队列首是 peer
    got.wait();
    peer.await();
    if (l.locked()) throw std::logic_error("case5: peer must have released the lock");
  });

  // case 6: 取消与交接同槽——release() 已把所有权交给 victim，紧接着的
  // cancel() 让它在阻塞点拿到 Cancelled（后端取消腿优先）。所有权不得丢失，
  // 必须转交给下一个等待者。
  run_case(__func__, "case6", [&] {
    Lock l;
    l.acquire();
    Event r1, r2, caught, got;
    auto victim = create_task("c6_victim", [&] {
      r1.set();
      try {
        l.acquire();
        throw std::logic_error("case6: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c6_peer", [&] {
      r1.wait();
      r2.set();
      l.acquire();
      got.set();
      l.release();
    });
    r1.wait();
    r2.wait();
    // release() 与 cancel() 之间没有挂起点，victim 在两者之间不可能被调度：
    // 它必然处于"已交接但拿到的是取消"的竞态形态（两后端均取消腿优先）。
    l.release();
    victim.cancel();
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    got.wait();  // 关键：所有权已转交给 peer，没有随取消消失
    peer.await();
    if (l.locked()) throw std::logic_error("case6: peer must have released after handoff");
  });

  // case 7: 句柄语义——拷贝共享同一把锁（与 Event 一致），release 可跨句柄
  run_case(__func__, "case7", [&] {
    Lock a;
    Lock b = a;
    a.acquire();
    if (!b.locked()) throw std::logic_error("case7: copies must share the same lock state");
    b.release();  // 锁不记录持有者，任意句柄都能释放
    if (a.locked()) throw std::logic_error("case7: release through a copy must unlock");
  });

  // case 8: LockGuard——构造取得、析构释放（作用域即临界区）
  run_case(__func__, "case8", [&] {
    Lock l;
    {
      LockGuard g(l);
      if (!l.locked()) throw std::logic_error("case8: guard must hold the lock");
    }
    if (l.locked()) throw std::logic_error("case8: guard must release at scope exit");
  });

  // case 9: LockGuard 在取消展开时释放（≡ async with 的 __aexit__）
  run_case(__func__, "case9", [&] {
    Lock l;
    Event in_crit;
    auto holder = create_task("c9_holder", [&] {
      LockGuard g(l);
      in_crit.set();
      Event never;
      never.wait();  // 持锁阻塞，随后被取消
    });
    in_crit.wait();
    if (!l.locked()) throw std::logic_error("case9: guard must hold the lock");
    holder.cancel();
    try {
      holder.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (l.locked()) throw std::logic_error("case9: guard must release while unwinding");
    l.acquire();  // 锁确实可用：再取一次不阻塞
    l.release();
  });

  report(__func__);
}
