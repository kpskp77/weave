// Weave C++ 簿记层自检：在真实 SystemC 内核上（weave_sc_smoke）验证 wait 的
// asyncio.wait 语义。同步完全事件驱动（纯 delta cycle），不依赖真实时钟。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(wait_semantics) {
  // case 1: ALL_COMPLETED（默认）：全部完成后返回，done 含全部
  run_case(__func__, "case1", [&] {
    auto a = create_task("w1_a", [] {});
    auto b = create_task("w1_b", [] {});
    auto r = weave::wait({a, b});
    if (r.done.size() != 2 || !r.pending.empty())
      throw std::logic_error("case1: ALL_COMPLETED must return with all done");
    if (!a.done() || !b.done()) throw std::logic_error("case1: tasks must be done");
  });

  // case 2: FIRST_COMPLETED：首个完成即返回，其余任务不受影响
  run_case(__func__, "case2", [&] {
    Event gate;
    auto a = create_task("w2_a", [] {});
    auto b = create_task("w2_b", [&] { gate.wait(); });
    auto r = weave::wait({a, b}, ReturnWhen::FIRST_COMPLETED);
    if (r.done.size() != 1 || r.pending.size() != 1)
      throw std::logic_error("case2: FIRST_COMPLETED must return after first completion");
    if (!a.done() || b.done()) throw std::logic_error("case2: done/pending grouping wrong");
    gate.set();
    b.await();  // b 未被取消，放行后自然完成
    if (b.cancelled()) throw std::logic_error("case2: wait must not cancel pending tasks");
  });

  // case 3: FIRST_EXCEPTION：仅 FAILED 触发提前返回，CANCELLED 不算
  run_case(__func__, "case3", [&] {
    Event gate;
    auto victim = create_task("w3_victim", [&] { gate.wait(); });
    auto slow = create_task("w3_slow", [&] { gate.wait(); });
    auto failer = create_task("w3_failer", [] { throw std::runtime_error("w3"); });
    // killer 与 failer 同一批调度：victim 的取消与 failer 的失败几乎同时发生。
    // 若 CANCELLED 会触发 FIRST_EXCEPTION，wait 可能在 failer 失败前返回——
    // 断言 failer 必在 done 中即证明未提前返回。
    auto killer = create_task("w3_killer", [&] { victim.cancel(); });
    auto r = weave::wait({victim, failer, slow}, ReturnWhen::FIRST_EXCEPTION);
    if (!failer.done())
      throw std::logic_error("case3: FIRST_EXCEPTION returned before any failure");
    // victim 的取消与 failer 的失败同槽发生，victim 落在 done/pending 取决于
    // 后端通知顺序——不断言其归属；只断言确定性的部分：failer 已完成、
    // slow 仍在 pending。
    if (slow.done()) throw std::logic_error("case3: slow must still be pending");
    if (r.done.size() + r.pending.size() != 3)
      throw std::logic_error("case3: done/pending must partition all tasks");
    killer.await();
    try {
      victim.await();
      throw std::logic_error("case3: victim should throw Cancelled");
    } catch (const Cancelled&) {
      // 预期路径
    }
    gate.set();
    slow.await();
  });

  // case 4: timeout 到点按当时状态分组返回，不取消任何任务
  run_case(__func__, "case4", [&] {
    Event gate;
    auto a = create_task("w4_a", [&] { gate.wait(); });
    auto b = create_task("w4_b", [&] { gate.wait(); });
    auto r = weave::wait({a, b}, ReturnWhen::ALL_COMPLETED, 1_ms);
    if (!r.done.empty() || r.pending.size() != 2)
      throw std::logic_error("case4: timeout must return current grouping");
    if (a.cancelled() || b.cancelled())
      throw std::logic_error("case4: timeout must not cancel tasks");
    gate.set();
    a.await();
    b.await();
  });

  // case 5: wait 自身被取消 → 传播 Cancelled，子任务继续运行
  run_case(__func__, "case5", [&] {
    Event block, ready;
    auto child = create_task("w5_child", [&] { block.wait(); });
    auto waiter = create_task("w5_waiter", [&] {
      ready.set();
      try {
        weave::wait({child});
        throw std::logic_error("case5: unreachable");
      } catch (const Cancelled&) {
        // 预期路径
      }
    });
    ready.wait();  // waiter 已阻塞在 wait 内（锁交接即同步）
    waiter.cancel();
    waiter.await();
    if (child.done()) throw std::logic_error("case5: wait cancellation must not affect children");
    block.set();
    child.await();
  });

  // case 6: timeout=0 特判为直接快照，不阻塞不让出
  run_case(__func__, "case6", [&] {
    Event gate;
    auto a = create_task("w6_a", [&] { gate.wait(); });
    auto r = weave::wait({a}, ReturnWhen::ALL_COMPLETED, duration{0});
    if (!r.done.empty() || r.pending.size() != 1)
      throw std::logic_error("case6: timeout=0 must snapshot immediately");
    if (a.done())  // 未让出过调度，a 尚未运行
      throw std::logic_error("case6: timeout=0 must not yield");
    gate.set();
    a.await();
  });

  // case 7: 注册时已有任务完成 → FIRST_COMPLETED 同步返回
  run_case(__func__, "case7", [&] {
    Event gate;
    auto a = create_task("w7_a", [] {});
    a.await();  // a 已 DONE
    auto b = create_task("w7_b", [&] { gate.wait(); });
    auto r = weave::wait({a, b}, ReturnWhen::FIRST_COMPLETED);
    if (r.done.size() != 1 || r.pending.size() != 1 || !r.done.front().done())
      throw std::logic_error("case7: pre-completed task must satisfy FIRST_COMPLETED");
    gate.set();
    b.await();
  });

  // case 8: 空任务组 → 立即返回空分组
  run_case(__func__, "case8", [&] {
    auto r = weave::wait({});
    if (!r.done.empty() || !r.pending.empty())
      throw std::logic_error("case8: empty wait must return empty grouping");
  });

  // case 9: timeout 用 optional<duration> 表达（取代 duration{-1} 哨兵）——
  // nullopt 即无超时（由 case1-3 的默认实参覆盖）；负时长不再意味着"无超时"，
  // 按非正时长处理：直接快照，不阻塞不让出。
  run_case(__func__, "case9", [&] {
    Event gate;
    auto a = create_task("w9_a", [&] { gate.wait(); });
    auto r = weave::wait({a}, ReturnWhen::ALL_COMPLETED, duration{-1});
    if (!r.done.empty() || r.pending.size() != 1)
      throw std::logic_error("case9: negative timeout must snapshot immediately");
    if (a.done()) throw std::logic_error("case9: negative timeout must not yield");
    gate.set();
    a.await();
  });

  report(__func__);
}
