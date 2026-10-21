// Weave C++ 簿记层自检：在真实 SystemC 内核上（weave_smoke_sc）验证 TaskGroup
// 的 asyncio.TaskGroup 语义。同步完全事件驱动（纯 delta cycle），不依赖真实时钟。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(taskgroup_semantics) {
  // case 1: 全部成功 → wait 正常返回，子任务均完成
  run_case(__func__, "case1", [&] {
    Event ga, gb;
    TaskGroup tg;
    auto a = tg.create_task("tg1_a", [&] { ga.wait(); });
    auto b = tg.create_task("tg1_b", [&] { gb.wait(); });
    ga.set();
    gb.set();
    tg.wait();
    if (!a.done() || !b.done() || a.cancelled() || b.cancelled())
      throw std::logic_error("case1: all children must finish DONE");
  });

  // case 2: 首个失败 → 取消兄弟，等全部落地后重抛首个异常
  run_case(__func__, "case2", [&] {
    Event block;
    TaskGroup tg;
    [[maybe_unused]] auto failer =
        tg.create_task("tg2_failer", [] { throw std::runtime_error("tg2_boom"); });
    auto victim = tg.create_task("tg2_victim", [&] { block.wait(); });
    try {
      tg.wait();
      throw std::logic_error("case2: unreachable");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "tg2_boom")
        throw std::logic_error("case2: first failure must be rethrown");
    }
    if (!victim.cancelled()) throw std::logic_error("case2: sibling must be cancelled on failure");
    if (!victim.done())
      throw std::logic_error("case2: siblings must be settled before wait returns");
  });

  // case 3: 子任务被外部取消 → 容忍，不触发兄弟取消，wait 正常返回
  run_case(__func__, "case3", [&] {
    Event block, go;
    TaskGroup tg;
    auto victim = tg.create_task("tg3_victim", [&] { block.wait(); });
    auto good = tg.create_task("tg3_good", [&] { go.wait(); });
    auto killer = create_task("tg3_killer", [&] { victim.cancel(); });  // 组外任务
    go.set();
    tg.wait();  // victim 被取消不算失败：正常返回
    if (!victim.cancelled()) throw std::logic_error("case3: victim should be cancelled");
    if (!good.done() || good.cancelled())
      throw std::logic_error("case3: external cancel must not abort siblings");
    killer.await();
  });

  // case 4: wait 调用者被取消 → 级联取消全部子任务，等落地后抛 Cancelled
  run_case(__func__, "case4", [&] {
    Event block, ready;
    TaskGroup tg;  // waiter 引用本栈帧的 tg/block；waiter.await() 保证其先完成
    auto c1 = tg.create_task("tg4_c1", [&] { block.wait(); });
    auto c2 = tg.create_task("tg4_c2", [&] { block.wait(); });
    auto waiter = create_task("tg4_waiter", [&] {
      ready.set();
      try {
        tg.wait();
        throw std::logic_error("case4: unreachable");
      } catch (const Cancelled&) {
        // 预期路径：级联取消后传播
      }
    });
    ready.wait();  // waiter 已阻塞在 tg.wait() 内（锁交接即同步）
    waiter.cancel();
    waiter.await();
    if (waiter.cancelled())
      throw std::logic_error("case4: waiter swallowed Cancelled and finished DONE");
    if (!c1.cancelled() || !c2.cancelled())
      throw std::logic_error("case4: outer cancel must cascade to all children");
  });

  // case 5: wait 之后 create_task / 再次 wait → logic_error；空组立即返回
  run_case(__func__, "case5", [&] {
    TaskGroup tg;
    tg.wait();  // 空组：不阻塞，正常返回
    try {
      tg.create_task("tg5_x", [] {});
      throw std::logic_error("case5: unreachable");
    } catch (const std::logic_error& e) {
      if (std::string(e.what()).find("after wait") == std::string::npos) throw;
    }
    try {
      tg.wait();
      throw std::logic_error("case5: unreachable");
    } catch (const std::logic_error& e) {
      if (std::string(e.what()).find("twice") == std::string::npos) throw;
    }
  });

  // case 6: 失败按完成顺序而非创建顺序：晚创建先失败的任务胜出
  run_case(__func__, "case6", [&] {
    Event gate;
    TaskGroup tg;
    auto late = tg.create_task("tg6_late", [&] {
      gate.wait();
      throw std::runtime_error("tg6_late");
    });
    [[maybe_unused]] auto early =
        tg.create_task("tg6_early", [] { throw std::runtime_error("tg6_early"); });
    try {
      tg.wait();
      throw std::logic_error("case6: unreachable");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "tg6_early")
        throw std::logic_error("case6: first failure by completion order must win");
    }
    if (!late.cancelled()) throw std::logic_error("case6: pending sibling must be cancelled");
  });

  // case 7: 组外 cancel()（Python 3.15+）→ 全部未完成子任务被外部取消，
  // 不算失败、不触发兄弟链式取消；wait 正常返回
  run_case(__func__, "case7", [&] {
    Event block;
    TaskGroup tg;
    auto a = tg.create_task("tg7_a", [&] { block.wait(); });
    auto b = tg.create_task("tg7_b", [&] { block.wait(); });
    tg.cancel();  // 不带 msg
    tg.wait();    // 外部取消的容忍路径：正常返回
    if (!a.cancelled() || !b.cancelled())
      throw std::logic_error("case7: cancel() must cancel all unfinished children");
  });

  // case 8: cancel(msg)：子任务捕获 Cancelled 时能读到 msg；已 DONE 的子任务不受影响
  run_case(__func__, "case8", [&] {
    Event block, ready;
    std::string seen;
    TaskGroup tg;
    auto done_child = tg.create_task("tg8_done", [] {});
    [[maybe_unused]] auto watcher = tg.create_task("tg8_watcher", [&] {
      ready.set();  // 报告自己已推进到阻塞点之前
      try {
        block.wait();
      } catch (const Cancelled& c) {
        if (c.msg()) seen = std::string(*c.msg());
      }
    });
    // 事件交接即同步屏障：ready.wait 返回保证 watcher 已阻塞在 block 上
    // （done_child 先创建先运行，此时已 DONE）
    ready.wait();
    tg.cancel("shutdown");
    tg.wait();
    if (seen != "shutdown")
      throw std::logic_error("case8: child must observe cancel msg from TaskGroup.cancel()");
    if (!done_child.done() || done_child.cancelled())
      throw std::logic_error("case8: already-DONE child must be untouched");
  });

  report(__func__);
}
