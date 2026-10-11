// Event 语义自检：edge 语义（set 快照等待者，clear 不收回已判定的放行）、
// 多等待者同时唤醒、等待中被取消后 set() 不得触碰已销毁的唤醒事件。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。
// 与 gather_semantics.cpp 共用同一断言体风格，由 weave_sc_smoke /
// weave_svdpi_smoke 两个后端回归。

#include <stdexcept>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(event_semantics) {
  // case 1: is_set/clear 基本语义；已置位 Event::wait 立即返回
  run_case(__func__, "case1", [&] {
    Event e;
    if (e.is_set()) throw std::logic_error("case1: fresh Event must be unset");
    e.set();
    if (!e.is_set()) throw std::logic_error("case1: is_set must reflect set()");
    e.wait();  // 已置位：立即返回，不挂起
    e.clear();
    if (e.is_set()) throw std::logic_error("case1: is_set must reflect clear()");
  });

  // case 2: edge 语义——set() 判定放行后 clear() 不收回；
  // 同时验证"等待方从阻塞返回 ⇒ 触发方已推进到阻塞点之后"（锁交接）
  run_case(__func__, "case2", [&] {
    Event ev, ready, proceeded;
    auto w = create_task("edge_w", [&] {
      ready.set();
      ev.wait();
      proceeded.set();  // ev 已被 set+clear，仍须走到这里
    });
    ready.wait();  // w 此刻必然阻塞在 ev.wait() 上
    ev.set();
    ev.clear();  // 标志已复位，但 w 已被 set() 快照判定放行
    proceeded.wait();
    w.await();
    if (!w.done() || w.cancelled()) throw std::logic_error("case2: waiter state wrong");
  });

  // case 3: 单次 set() 唤醒所有当前等待者
  run_case(__func__, "case3", [&] {
    Event ev, r1, r2, d1, d2;
    auto w1 = create_task("multi_w1", [&] {
      r1.set();
      ev.wait();
      d1.set();
    });
    auto w2 = create_task("multi_w2", [&] {
      r2.set();
      ev.wait();
      d2.set();
    });
    r1.wait();  // 两个等待者均已就位
    r2.wait();
    ev.set();  // 一次 set 同时放行
    d1.wait();
    d2.wait();
    gather(w1, w2);
  });

  // case 4: 等待中被取消 → 阻塞点注入 Cancelled；
  // 唤醒事件已从 wake_evs 摘除并销毁（成功/取消两路径都要），
  // 之后对同一 Event 再 set() 不得触发已销毁的唤醒事件（SVDPI 空指针解引用）；
  // 新等待者在置位标志上立即通过
  run_case(__func__, "case4", [&] {
    Event ev, ready, caught, ok;
    auto w = create_task("cancel_w", [&] {
      ready.set();
      try {
        ev.wait();
        throw std::logic_error("case4: cancelled waiter must not return from wait()");
      } catch (const Cancelled&) {
        caught.set();  // Cancelled 在 ev.wait() 内被注入
        throw;         // 继续传播 → 任务 CANCELLED
      }
    });
    ready.wait();  // w 阻塞在 ev.wait()
    w.cancel();
    caught.wait();  // Cancelled 已注入（w 已被唤醒并推进过阻塞点）
    try {
      w.await();
      throw std::logic_error("case4: await on cancelled task must throw");
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!w.cancelled()) throw std::logic_error("case4: w should be CANCELLED");
    // w 的唤醒事件已销毁；此次 set 不得对它触发（曾真实出过空指针解引用）
    ev.set();
    auto w2 = create_task("after_w", [&] {
      ev.wait();  // 标志已置位：立即通过
      ok.set();
    });
    ok.wait();
    gather(w2);
  });

  report(__func__);
}
