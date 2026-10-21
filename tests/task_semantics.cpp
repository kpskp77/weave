// 任务原语语义自检：create_task 调度时机、cancel 各路径
// （PENDING 不执行 body / 阻塞点注入 / 拒绝取消）、终态任务 cancel 幂等、
// 句柄属性（valid/get_name）、sleep(0) 零时间让出。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。
// 与 gather_semantics.cpp 共用同一断言体风格，由 weave_smoke_sc /
// weave_smoke_svdpi 两个后端回归。

#include <stdexcept>
#include <string_view>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(task_semantics) {
  // case 1: create_task 只调度不立即运行——父任务不碰阻塞点前子任务不得启动
  run_case(__func__, "case1", [&] {
    Event started, gate;
    auto child = create_task("lazy", [&] {
      started.set();
      gate.wait();
    });
    if (started.is_set()) throw std::logic_error("case1: child must not run before parent blocks");
    started.wait();  // 父任务挂起后子任务才被调度
    if (!child.valid()) throw std::logic_error("case1: child handle must be valid");
    gate.set();
    child.await();
  });

  // case 2: PENDING 任务被取消 → body 永不执行，直接进入 CANCELLED
  run_case(__func__, "case2", [&] {
    Event started, release;
    auto victim = create_task("pending_victim", [&] {
      started.set();  // 若被执行必然置位
      release.wait();
    });
    victim.cancel();  // 此时 victim 尚未被调度（PENDING）
    release.set();    // 父任务让出，victim 获得一次调度机会
    sleep(0_fs);      // 再让出一拍，确保 victim 已被内核调度过
    if (started.is_set()) throw std::logic_error("case2: PENDING cancel must skip body");
    try {
      victim.await();
      throw std::logic_error("case2: await on cancelled task must throw");
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!victim.cancelled()) throw std::logic_error("case2: victim should be CANCELLED");
  });

  // case 3: 阻塞中的任务被取消 → 立即唤醒并在阻塞点注入 Cancelled
  run_case(__func__, "case3", [&] {
    Event ready, caught;
    auto victim = create_task("waiting_victim", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
        throw std::logic_error("case3: cancelled waiter must not return from wait()");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    ready.wait();  // victim 已阻塞在 never.wait()
    victim.cancel();
    caught.wait();  // Cancelled 已在 victim 的阻塞点注入
    try {
      victim.await();
      throw std::logic_error("case3: await must rethrow Cancelled");
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!victim.cancelled()) throw std::logic_error("case3: victim should be CANCELLED");
  });

  // case 4: 任务可捕获 Cancelled 拒绝取消 → 正常返回即 DONE（协作式语义）
  run_case(__func__, "case4", [&] {
    Event ready, finished;
    auto stubborn = create_task("stubborn", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
      } catch (const Cancelled&) {
        // 吞掉取消：拒绝取消
      }
      finished.set();  // 仍执行到正常结束
    });
    ready.wait();
    stubborn.cancel();
    finished.wait();   // 拒绝取消后跑完 body
    stubborn.await();  // DONE：不抛异常
    if (!stubborn.done() || stubborn.cancelled())
      throw std::logic_error("case4: swallowed cancel must end DONE, not CANCELLED");
  });

  // case 5: 终态任务 cancel 幂等（DONE / CANCELLED 均不受影响）
  run_case(__func__, "case5", [&] {
    auto done_task = create_task("done_t", [] {});
    done_task.await();
    done_task.cancel();  // 不得抛异常、不得改变终态
    if (!done_task.done() || done_task.cancelled())
      throw std::logic_error("case5: cancel on DONE task must be a no-op");
    if (done_task.get_name() != "done_t")
      throw std::logic_error("case5: get_name must return the create_task name");
  });

  // case 6: sleep(0) 零时间让出——不挂死，任务照常推进（子任务先睡 0 再置位）
  run_case(__func__, "case6", [&] {
    Event done;
    auto t = create_task("zero_sleep", [&] {
      sleep(0_fs);
      done.set();
    });
    done.wait();
    t.await();
    if (!t.done()) throw std::logic_error("case6: task after sleep(0) must complete");
  });

  // case 7: cancel(msg) 对齐 asyncio Task.cancel(msg)（Python 3.9+）——
  // msg 在阻塞点注入的 Cancelled 中可见，且在 await 重投处保留同一 msg。
  run_case(__func__, "case7", [&] {
    Event ready;
    auto victim = create_task("msg_victim", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
        throw std::logic_error("case7: cancelled waiter must not return");
      } catch (const Cancelled& e) {
        if (e.msg() != std::string_view{"shutdown"})
          throw std::logic_error("case7: Cancelled must carry cancel msg at injection point");
        throw;
      }
    });
    ready.wait();  // victim 已阻塞在 never.wait()
    victim.cancel("shutdown");
    try {
      victim.await();
      throw std::logic_error("case7: await must rethrow Cancelled");
    } catch (const Cancelled& e) {
      if (e.msg() != std::string_view{"shutdown"})
        throw std::logic_error("case7: await must rethrow Cancelled with original msg");
    }
    if (!victim.cancelled()) throw std::logic_error("case7: victim should be CANCELLED");
  });

  // case 8: cancel() 不带 msg → Cancelled::msg() 为 nullopt（对应 asyncio 的 msg=None），
  // what() 回落到默认文案。
  run_case(__func__, "case8", [&] {
    Event ready;
    auto victim = create_task("plain_victim", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
        throw std::logic_error("case8: unreachable");
      } catch (const Cancelled& e) {
        if (e.msg()) throw std::logic_error("case8: plain cancel must have no msg");
        throw;
      }
    });
    ready.wait();
    victim.cancel();
    try {
      victim.await();
      throw std::logic_error("case8: await must rethrow Cancelled");
    } catch (const Cancelled& e) {
      if (e.msg()) throw std::logic_error("case8: plain cancel rethrown must have no msg");
    }
  });

  // case 9: 取消腿与事件腿同槽 → 取消优先（port 契约第 4 条）。
  // set() 与 cancel() 之间没有挂起点，victim 不可能被调度，两腿必然落在同一
  // 时间槽；后端必须让取消腿胜出，否则组合原语会把"已被取消"误判成正常放行。
  run_case(__func__, "case9", [&] {
    Event gate, ready;
    auto victim = create_task("same_slot_victim", [&] {
      ready.set();
      try {
        gate.wait();
        throw std::logic_error("case9: cancel must win over the event leg");
      } catch (const Cancelled&) {
        throw;  // 预期路径：取消在阻塞点注入
      }
    });
    ready.wait();     // victim 已阻塞在 gate.wait()
    gate.set();       // 事件腿
    victim.cancel();  // 同槽的取消腿
    try {
      victim.await();
      throw std::logic_error("case9: await must rethrow Cancelled");
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!victim.cancelled()) throw std::logic_error("case9: victim must be CANCELLED");
  });

  report(__func__);
}
