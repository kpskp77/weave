// Weave C++ 簿记层自检：在真实 SystemC 内核上（weave_sc_smoke）验证
// as_completed 的 asyncio.as_completed 语义。同步完全事件驱动（纯 delta
// cycle），不依赖真实时钟。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(as_completed_semantics) {
  // case 1: 按完成顺序产出（与创建顺序无关）
  run_case(__func__, "case1", [&] {
    Event g1, g2, g3;
    auto a = create_task("ac1_a", [&] { g1.wait(); });
    auto b = create_task("ac1_b", [&] { g2.wait(); });
    auto c = create_task("ac1_c", [&] { g3.wait(); });
    auto ac = as_completed({a, b, c});
    g3.set();  // 只放行 c：next() 阻塞期间 c 完成
    if (ac.next().get_name() != "ac1_c")
      throw std::logic_error("case1: first completed must come first");
    g1.set();
    if (ac.next().get_name() != "ac1_a")
      throw std::logic_error("case1: completion order must be c, a, b");
    g2.set();
    if (ac.next().get_name() != "ac1_b")
      throw std::logic_error("case1: completion order must be c, a, b");
    if (ac.pending()) throw std::logic_error("case1: all tasks produced");
  });

  // case 2: 全部产出后 next() 抛 logic_error；pending() 为 false
  run_case(__func__, "case2", [&] {
    auto a = create_task("ac2_a", [] {});
    auto ac = as_completed({a});
    ac.next().await();
    if (ac.pending()) throw std::logic_error("case2: pending must be false at end");
    try {
      (void)ac.next();  // 期望抛出：返回值无意义
      throw std::logic_error("case2: unreachable");
    } catch (const std::logic_error& e) {
      if (std::string(e.what()).find("no more tasks") == std::string::npos) throw;
    }
  });

  // case 3: 失败任务照常产出，异常由 await 重抛
  run_case(__func__, "case3", [&] {
    auto bad = create_task("ac3_bad", [] { throw std::runtime_error("ac3_fail"); });
    auto good = create_task("ac3_good", [] {});
    auto ac = as_completed({bad, good});
    auto first = ac.next();  // delta-0 完成，spawn 顺序 bad 在前
    try {
      first.await();
      throw std::logic_error("case3: unreachable");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "ac3_fail")
        throw std::logic_error("case3: failure must surface via await");
    }
    ac.next().await();
    if (ac.pending()) throw std::logic_error("case3: all tasks produced");
  });

  // case 4: 消费者在 next() 中被取消 → 取消只中断 next()，队列与子任务完好
  run_case(__func__, "case4", [&] {
    Event block, ready;
    auto child = create_task("ac4_child", [&] { block.wait(); });
    auto consumer = create_task("ac4_consumer", [&] {
      auto ac = as_completed({child});
      ready.set();
      try {
        (void)ac.next();  // 期望抛出：返回值无意义
        throw std::logic_error("case4: unreachable");
      } catch (const Cancelled&) {
        // 预期路径：取消只中断本次 next()
      }
      if (!ac.pending()) throw std::logic_error("case4: state must survive cancellation");
      block.set();
      ac.next().await();  // 放行后正常取到
      if (ac.pending()) throw std::logic_error("case4: all tasks produced");
    });
    ready.wait();  // consumer 已阻塞在 next() 内（锁交接即同步）
    consumer.cancel();
    consumer.await();
    if (consumer.cancelled())
      throw std::logic_error("case4: consumer swallowed Cancelled and finished DONE");
    if (!child.done() || child.cancelled())
      throw std::logic_error("case4: child must not be affected by consumer cancel");
  });

  // case 5: 注册时已完成（FAILED 亦然）的任务立即产出，不阻塞
  run_case(__func__, "case5", [&] {
    auto a = create_task("ac5_a", [] {});
    a.await();  // a 已 DONE
    Event gate;
    auto b = create_task("ac5_b", [&] { gate.wait(); });
    auto ac = as_completed({a, b});
    if (ac.next().get_name() != "ac5_a")
      throw std::logic_error("case5: pre-completed task must be produced first");
    gate.set();
    ac.next().await();
    if (ac.pending()) throw std::logic_error("case5: all tasks produced");
  });

  // case 6: 空任务组 → pending() 立即为 false，next() 抛 logic_error
  run_case(__func__, "case6", [&] {
    auto ac = as_completed({});
    if (ac.pending()) throw std::logic_error("case6: empty set must not be pending");
    try {
      (void)ac.next();  // 期望抛出：返回值无意义
      throw std::logic_error("case6: unreachable");
    } catch (const std::logic_error& e) {
      if (std::string(e.what()).find("no more tasks") == std::string::npos) throw;
    }
  });

  report(__func__);
}
