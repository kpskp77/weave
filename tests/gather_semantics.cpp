// Weave C++ 簿记层自检：在真实 SystemC 内核上（weave_sc_smoke）验证 gather 的
// asyncio 语义。同步完全事件驱动（纯 delta cycle），不依赖真实时钟。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。
// 根任务正常结束即 PASSED（见 tests/sc_main.cpp 的汇报进程）。

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(gather_semantics) {
  // case 1: 全部成功 → gather 正常返回
  run_case(__func__, "case1", [&] {
    Event sa, sb, sc;  // 任务已就位
    Event ga, gb, gc;  // 放行
    auto a = create_task("ok_a", [&] {
      sa.set();
      ga.wait();
    });
    auto b = create_task("ok_b", [&] {
      sb.set();
      gb.wait();
    });
    auto c = create_task("ok_c", [&] {
      sc.set();
      gc.wait();
    });
    sa.wait();
    sb.wait();
    sc.wait();  // 三个任务都已在各自的 ga/gb/gc 上阻塞
    ga.set();
    gb.set();
    gc.set();
    gather(a, b, c);
    if (!a.done() || !b.done() || !c.done())
      throw std::logic_error("case1: gather returned before all peers done");
  });

  // case 2: 首个失败（按完成顺序）立即重抛；不等慢任务，不取消/拖走兄弟
  run_case(__func__, "case2", [&] {
    Event go_fast, go_slow;
    auto fast = create_task("fail_fast", [&] {
      go_fast.wait();
      throw std::runtime_error("fast");
    });
    auto slow = create_task("fail_slow", [&] {
      go_slow.wait();
      throw std::runtime_error("slow");
    });
    go_fast.set();  // 快任务先行失败；慢任务仍阻塞在 go_slow 上
    try {
      gather(fast, slow);
      throw std::logic_error("case2: gather should rethrow");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "fast")
        throw std::logic_error("case2: first failure by completion order must win");
    }
    if (slow.done())
      throw std::logic_error("case2: gather must return at first failure, not wait for peers");
    if (slow.cancelled())
      throw std::logic_error("case2: sibling must not be cancelled on peer failure");
    // 兄弟任务晚到的失败仅留在任务簿记中（无 unretrieved 告警，MVP 接受）；
    // 同时覆盖"gather 提前返回后 Event 再次被 set()"的路径
    go_slow.set();
    try {
      slow.await();
      throw std::logic_error("case2: slow should rethrow its late failure");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "slow") throw;
    }
  });

  // case 3: gather 因失败提前返回后，兄弟任务继续运行至自然完成
  run_case(__func__, "case3", [&] {
    Event go_failer, go_peer;
    auto failer = create_task("failer3", [&] {
      go_failer.wait();
      throw std::runtime_error("boom3");
    });
    auto peer = create_task("peer3", [&] { go_peer.wait(); });
    go_failer.set();
    try {
      gather(failer, peer);
      throw std::logic_error("case3: gather should rethrow");
    } catch (const std::runtime_error&) {
      // 预期路径
    }
    if (peer.done() || peer.cancelled())
      throw std::logic_error("case3: peer must keep running after gather failure");
    go_peer.set();
    peer.await();
    if (!peer.done() || peer.cancelled())
      throw std::logic_error("case3: peer should finish on its own");
  });

  // case 4: 子任务被取消视为失败 → gather 抛 Cancelled
  run_case(__func__, "case4", [&] {
    Event block;
    auto victim = create_task("victim4", [&] { block.wait(); });
    auto killer = create_task("killer4", [&victim] { victim.cancel(); });
    try {
      gather(victim);
      throw std::logic_error("case4: gather should throw Cancelled");
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!victim.cancelled()) throw std::logic_error("case4: victim should be cancelled");
    (void)killer;
  });

  // case 5: gather 自身被取消 → 先取消所有未完成任务，再传播 Cancelled。
  // 锁交接即同步：grp_ready.wait 返回时，grp 必然已注册回调并阻塞在 gather 内。
  run_case(__func__, "case5", [&] {
    Event block;
    Event grp_ready;
    auto child = create_task("child5", [&] { block.wait(); });
    auto grp = create_task("grp5", [&] {
      grp_ready.set();
      try {
        gather(child);
        throw std::logic_error("case5: unreachable");
      } catch (const Cancelled&) {
        // 预期路径：外层取消经 gather 传播
      }
    });
    grp_ready.wait();
    grp.cancel();
    grp.await();
    try {
      child.await();
      throw std::logic_error("case5: child should throw Cancelled");
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!child.cancelled())
      throw std::logic_error("case5: outer cancel of gather must cancel pending children");
    if (grp.cancelled())
      throw std::logic_error("case5: gatherer swallowed Cancelled and finished DONE");
  });

  // case 6: gather 调用时已有任务失败 → 同步回调路径立即重抛
  run_case(__func__, "case6", [&] {
    auto bad = create_task("bad6", [] { throw std::runtime_error("pre"); });
    try {
      bad.await();
      throw std::logic_error("case6: await should rethrow");
    } catch (const std::runtime_error&) {
      // 消费异常，使 bad 进入 FAILED 终态
    }
    Event started, go;
    auto good = create_task("good6", [&] {
      started.set();
      go.wait();
    });
    try {
      gather(bad, good);
      throw std::logic_error("case6: gather should rethrow pre-failed task");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "pre")
        throw std::logic_error("case6: wrong exception from pre-failed task");
    }
    started.wait();  // good 尚未启动（gather 走同步路径未开过启动门）
    go.set();
    good.await();
    if (!good.done()) throw std::logic_error("case6: good should finish on its own");
  });

  // case 7: 空任务组 → 立即返回
  run_case(__func__, "case7", [&] { gather(); });

  report(__func__);
}
