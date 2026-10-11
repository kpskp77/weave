// wait_for 语义自检（asyncio.wait_for，3.12+ 超时语义）：
// 超时 → 取消任务并等取消完成 → 抛 Timeout；任务先完成 → 正常返回；
// 调用时任务已失败 → 同步路径立即重抛；任务拒绝取消 → Timeout 照抛。
// 时序仍完全事件驱动（ready 锁交接保证被测任务已阻塞），
// 超时时长只是被测机制本身的输入，不用于表达用例间次序。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。
// 与 gather_semantics.cpp 共用同一断言体风格，由 weave_sc_smoke /
// weave_svdpi_smoke 两个后端回归。

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(wait_for_semantics) {
  // case 1: 任务在超时前完成 → 正常返回，任务 DONE，不抛 Timeout
  run_case(__func__, "case1", [&] {
    Event ready, gate;
    auto t = create_task("fast1", [&] {
      ready.set();
      gate.wait();
    });
    ready.wait();  // t 已阻塞在 gate.wait()
    gate.set();
    wait_for(t, 1_s);
    if (!t.done() || t.cancelled()) throw std::logic_error("case1: task state wrong");
  });

  // case 2: 超时 → 抛 Timeout；任务已被取消且取消已走完（await 直接给终态）
  run_case(__func__, "case2", [&] {
    Event ready;
    auto slow = create_task("slow2", [&] {
      ready.set();
      Event never;
      never.wait();
    });
    ready.wait();  // slow 已阻塞，超时必先于其自身完成
    try {
      wait_for(slow, 1_ns);
      throw std::logic_error("case2: wait_for must throw Timeout");
    } catch (const Timeout&) {
      // 预期路径
    }
    if (!slow.done() || !slow.cancelled())
      throw std::logic_error("case2: timed-out task must be cancelled after wait_for returns");
    try {
      slow.await();
      throw std::logic_error("case2: await on timed-out task must throw");
    } catch (const Cancelled&) {
      // 预期路径：取消已传播完成
    }
  });

  // case 3: 调用时任务已失败 → 同步路径立即重抛原异常，不涉及时长
  run_case(__func__, "case3", [&] {
    auto bad = create_task("bad3", [] { throw std::runtime_error("pre3"); });
    try {
      bad.await();
      throw std::logic_error("case3: await should rethrow");
    } catch (const std::runtime_error&) {
      // 消费异常，使 bad 进入 FAILED 终态
    }
    try {
      wait_for(bad, 1_s);
      throw std::logic_error("case3: wait_for on failed task must rethrow");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "pre3")
        throw std::logic_error("case3: wrong exception from pre-failed task");
    }
  });

  // case 4: 任务拒绝取消（吞掉 Cancelled）→ wait_for 仍抛 Timeout，
  // 但任务自身跑完 body → DONE（asyncio 3.12+：超时不因取消被拒而失效）
  run_case(__func__, "case4", [&] {
    Event ready, finished;
    auto stubborn = create_task("stubborn4", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
      } catch (const Cancelled&) {
        // 吞掉取消
      }
      finished.set();
    });
    ready.wait();
    try {
      wait_for(stubborn, 1_ns);
      throw std::logic_error("case4: wait_for must throw Timeout even if cancel is resisted");
    } catch (const Timeout&) {
      // 预期路径
    }
    finished.wait();  // 拒绝取消后 body 仍跑完
    stubborn.await();
    if (!stubborn.done() || stubborn.cancelled())
      throw std::logic_error("case4: resisted-cancel task must end DONE");
  });

  report(__func__);
}
