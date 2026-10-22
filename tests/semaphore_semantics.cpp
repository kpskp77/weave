// 通信原语自检：Semaphore / BoundedSemaphore 的 asyncio 语义——计数取得
// （不挂起、不让出）、计数耗尽后 FIFO 排队、N 个许可允许 N 个并发持有者、
// release 逐个放行等待者、排队者取消（含取消与放行同槽时许可不丢失）、
// BoundedSemaphore 上界检查（含 handoff 回流路径不误判）、句柄拷贝共享、
// Guard 作用域释放与取消展开释放。
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

WEAVE_TEST(semaphore_semantics) {
  // case 1: 计数取得——默认 1 个许可，显式构造 N 个；有空闲计数时 acquire
  // 同步取得（不挂起、不让出），locked() 跟随剩余计数
  run_case(__func__, "case1", [&] {
    Semaphore dflt;  // 默认 1 个许可
    if (dflt.locked()) throw std::logic_error("case1: fresh Semaphore must be unlocked");
    Semaphore zero(0);  // 0 许可合法：首个 acquire 必须排队
    if (!zero.locked()) throw std::logic_error("case1: Semaphore(0) must start locked");
    Semaphore s(2);
    Event started;
    auto probe = create_task("c1_probe", [&] { started.set(); });
    s.acquire();
    if (started.is_set()) throw std::logic_error("case1: acquire with permits must not yield");
    s.acquire();
    if (!s.locked()) throw std::logic_error("case1: locked() must reflect exhausted permits");
    probe.await();  // probe 此刻才获得调度
    if (!started.is_set()) throw std::logic_error("case1: probe must run after yield");
  });

  // case 2: 计数互斥——2 个许可允许两个任务同时在临界区，第三个必须等放行，
  // 临界区进入深度恒为 2
  run_case(__func__, "case2", [&] {
    Semaphore s(2);
    int depth = 0;
    int max_depth = 0;
    Event a_in;
    auto a = create_task("c2_a", [&] {
      s.acquire();
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      a_in.set();
      sleep(0_fs);  // 主动让出：给竞争者尝试 acquire 的机会
      --depth;
      s.release();
    });
    auto b = create_task("c2_b", [&] {
      a_in.wait();
      s.acquire();
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      sleep(0_fs);
      --depth;
      s.release();
    });
    auto c = create_task("c2_c", [&] {
      a_in.wait();
      s.acquire();  // a、b 各占一个许可 → 阻塞至其中一个 release
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      --depth;
      s.release();
    });
    gather(a, b, c);
    if (max_depth != 2) throw std::logic_error("case2: three tasks entered with only two permits");
  });

  // case 3: FIFO 公平——等待者按 acquire() 调用顺序获得许可
  // 队列次序靠"前一个等待者先入队、后一个才开始"的链式事件确定：
  // r1.set() 在 acquire 之前，故 r1.wait() 返回时 t1 必然已排队。
  run_case(__func__, "case3", [&] {
    Semaphore s(1);
    s.acquire();  // 主任务持有唯一许可，制造排队
    std::vector<int> order;
    Event r1, r2, r3;
    auto t1 = create_task("c3_1", [&] {
      r1.set();
      s.acquire();
      order.push_back(1);
      s.release();
    });
    auto t2 = create_task("c3_2", [&] {
      r1.wait();
      r2.set();
      s.acquire();
      order.push_back(2);
      s.release();
    });
    auto t3 = create_task("c3_3", [&] {
      r2.wait();
      r3.set();
      s.acquire();
      order.push_back(3);
      s.release();
    });
    r1.wait();    // t1 已在队列首
    r2.wait();    // t2 排在其后
    r3.wait();    // t3 排在队尾
    s.release();  // 逐个放行
    gather(t1, t2, t3);
    if (order != std::vector<int>({1, 2, 3}))
      throw std::logic_error("case3: waiters must be served in FIFO order");
    if (s.locked()) throw std::logic_error("case3: semaphore must be free after last release");
  });

  // case 4: 每个 release 恰好放行一个等待者——放行后等待者尚未自行 release 时，
  // 下一个等待者不会被唤醒（用 proceed 事件把 t1 卡在「已取得、未释放」区间，
  // 使该检查在事件驱动下确定）
  run_case(__func__, "case4", [&] {
    Semaphore s(1);
    s.acquire();  // 主任务持有唯一许可
    Event r1, r2, got1, got2, proceed;
    auto t1 = create_task("c4_1", [&] {
      r1.set();
      s.acquire();  // 被主任务的 release 放行
      got1.set();
      proceed.wait();  // 卡住：已取得许可但暂不 release
      s.release();
    });
    auto t2 = create_task("c4_2", [&] {
      r1.wait();
      r2.set();
      s.acquire();  // 排在 t1 之后
      got2.set();
      s.release();
    });
    r1.wait();  // t1 已排队
    r2.wait();  // t2 排在 t1 之后
    s.release();
    got1.wait();  // t1 已取得许可
    if (got2.is_set()) throw std::logic_error("case4: one release must wake exactly one waiter");
    proceed.set();  // 允许 t1 release → 接力放行 t2
    got2.wait();
    gather(t1, t2);
  });

  // case 5: 排队者被取消 → 抛 Cancelled、不持有许可，许可仍随主任务的 release
  // 交给其余等待者
  run_case(__func__, "case5", [&] {
    Semaphore s(1);
    s.acquire();  // 主任务持有唯一许可
    Event r1, r2, caught, got;
    auto victim = create_task("c5_victim", [&] {
      r1.set();
      try {
        s.acquire();
        throw std::logic_error("case5: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c5_peer", [&] {
      r1.wait();
      r2.set();
      s.acquire();  // 排在 victim 之后
      got.set();
      s.release();
    });
    r1.wait();        // victim 已排队
    r2.wait();        // peer 排在 victim 之后
    victim.cancel();  // 只取消 victim，此时尚未放行
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (got.is_set()) throw std::logic_error("case5: cancel must not release a permit");
    s.release();  // 摘除 victim 后，队列首是 peer
    got.wait();
    peer.await();
    if (s.locked()) throw std::logic_error("case5: peer must have released the permit");
  });

  // case 6: 取消与放行同槽——release() 已把许可交给 victim，紧接着的
  // cancel() 让它在阻塞点拿到 Cancelled（后端取消腿优先）。许可不得丢失，
  // 必须转交给下一个等待者。
  run_case(__func__, "case6", [&] {
    Semaphore s(1);
    s.acquire();
    Event r1, r2, caught, got;
    auto victim = create_task("c6_victim", [&] {
      r1.set();
      try {
        s.acquire();
        throw std::logic_error("case6: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c6_peer", [&] {
      r1.wait();
      r2.set();
      s.acquire();
      got.set();
      s.release();
    });
    r1.wait();
    r2.wait();
    // release() 与 cancel() 之间没有挂起点，victim 在两者之间不可能被调度：
    // 它必然处于"已放行但拿到的是取消"的竞态形态（两后端均取消腿优先）。
    s.release();
    victim.cancel();
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    got.wait();  // 关键：许可已转交给 peer，没有随取消消失
    peer.await();
    if (s.locked()) throw std::logic_error("case6: peer must have released after handoff");
  });

  // case 7: 句柄语义——拷贝共享同一个信号量（与 Event 一致）
  run_case(__func__, "case7", [&] {
    Semaphore a(1);
    Semaphore b = a;
    a.acquire();
    if (!b.locked()) throw std::logic_error("case7: copies must share the same semaphore");
    b.release();  // 信号量不记录持有者，任意句柄都能释放
    if (a.locked()) throw std::logic_error("case7: release through a copy must free it");
  });

  // case 8: SemaphoreGuard——构造取得、析构释放（作用域即临界区）
  run_case(__func__, "case8", [&] {
    Semaphore s(1);
    {
      SemaphoreGuard g(s);
      if (!s.locked()) throw std::logic_error("case8: guard must hold the permit");
    }
    if (s.locked()) throw std::logic_error("case8: guard must release at scope exit");
  });

  // case 9: SemaphoreGuard 在取消展开时释放（≡ async with 的 __aexit__）
  run_case(__func__, "case9", [&] {
    Semaphore s(1);
    Event in_crit;
    auto holder = create_task("c9_holder", [&] {
      SemaphoreGuard g(s);
      in_crit.set();
      Event never;
      never.wait();  // 持许可阻塞，随后被取消
    });
    in_crit.wait();
    if (!s.locked()) throw std::logic_error("case9: guard must hold the permit");
    holder.cancel();
    try {
      holder.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (s.locked()) throw std::logic_error("case9: guard must release while unwinding");
    s.acquire();  // 许可确实可用：再取一次不阻塞
    s.release();
  });

  // case 10: 上界检查——release 次数达到初始值后再放抛 std::logic_error
  // （≡ asyncio 的 ValueError）；抛错不改状态，簿记仍一致
  run_case(__func__, "case10", [&] {
    BoundedSemaphore d;  // 默认上界 1
    if (d.locked()) throw std::logic_error("case10: fresh BoundedSemaphore must be unlocked");
    BoundedSemaphore b(2);
    b.acquire();
    b.acquire();
    if (!b.locked()) throw std::logic_error("case10: locked() must reflect exhausted permits");
    b.release();
    b.release();
    if (b.locked()) throw std::logic_error("case10: locked() must reflect released permits");
    bool threw = false;
    try {
      b.release();  // 第三次 release 越过初值 2
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case10: release beyond bound must throw");
    b.acquire();  // 抛错不污染状态：簿记仍一致
    b.release();
  });

  // case 11: 排队者被取消后上界簿记不漂移——取消路径不消耗也不归还许可，
  // 主任务归还后计数恰好回到上界，再放必抛
  run_case(__func__, "case11", [&] {
    BoundedSemaphore b(1);
    b.acquire();  // 主任务持有唯一许可
    Event r1, caught;
    auto victim = create_task("c11_victim", [&] {
      r1.set();
      try {
        b.acquire();
        throw std::logic_error("case11: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    r1.wait();        // victim 已排队
    victim.cancel();  // 队列中取消：摘除，许可从未发出
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    b.release();  // 主任务归还：计数回到上界
    bool threw = false;
    try {
      b.release();  // 若取消路径弄脏簿记，这里的判定会出错
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case11: permits taken must equal permits released");
    b.acquire();  // 恰好一个许可可用
    b.release();
  });

  // case 12: 上界检查必须读真实计数——许可经「放行 → 同槽取消 → handoff 转交」
  // 回流后由非持有者 release，此时计数已暗中回到上界，再放必抛。
  // 外层镜像计数（只跟踪配对调用）在这条路径下会漏判，与 asyncio 产生偏差。
  run_case(__func__, "case12", [&] {
    BoundedSemaphore b(1);
    b.acquire();  // 主任务持有唯一许可
    Event r1, r2, caught, got;
    auto victim = create_task("c12_victim", [&] {
      r1.set();
      try {
        b.acquire();
        throw std::logic_error("case12: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c12_peer", [&] {
      r1.wait();
      r2.set();
      b.acquire();  // 经 handoff 拿到 victim 的许可
      got.set();
      b.release();  // 非持有者的 release：计数回到上界
    });
    r1.wait();
    r2.wait();
    // release() 与 cancel() 之间没有挂起点：victim 必处于「已放行但拿到的是
    // 取消」的同槽竞态形态，其许可由 handoff 转交 peer。
    b.release();
    victim.cancel();
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // 预期路径
    }
    got.wait();
    peer.await();
    bool threw = false;
    try {
      b.release();  // peer 已把计数放回上界，再放必抛
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case12: over-release through handoff must be caught");
    if (b.locked()) throw std::logic_error("case12: one permit must be available");
    b.acquire();  // 簿记正确：恰好一个许可
    b.release();
  });

  // case 13: BoundedSemaphoreGuard——构造取得、析构释放（作用域即临界区）
  run_case(__func__, "case13", [&] {
    BoundedSemaphore b(1);
    {
      BoundedSemaphoreGuard g(b);
      if (!b.locked()) throw std::logic_error("case13: guard must hold the permit");
    }
    if (b.locked()) throw std::logic_error("case13: guard must release at scope exit");
  });

  report(__func__);
}
