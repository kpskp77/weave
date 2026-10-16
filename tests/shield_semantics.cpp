// Weave C++ 簿记层自检：在真实 SystemC 内核上（weave_sc_smoke）验证 shield 的
// asyncio.shield 语义。同步完全事件驱动（纯 delta cycle），不依赖真实时钟。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(shield_semantics) {
  // case 1: wait_for 超时只取消代理，inner 继续运行
  run_case(__func__, "case1", [&] {
    Event go;
    auto inner = create_task("s1_inner", [&] { go.wait(); });
    auto proxy = shield(inner);
    try {
      wait_for(proxy, 1_ms);
      throw std::logic_error("case1: wait_for should timeout");
    } catch (const Timeout&) {
      // 预期路径
    }
    if (!proxy.cancelled()) throw std::logic_error("case1: proxy should be cancelled");
    if (inner.done()) throw std::logic_error("case1: shield must protect inner from timeout");
    go.set();
    inner.await();
    if (!inner.done() || inner.cancelled())
      throw std::logic_error("case1: inner should finish on its own");
  });

  // case 2: gather 被取消 → 级联取消停在代理层，inner 存活
  run_case(__func__, "case2", [&] {
    Event block, go, grp_ready;
    auto inner = create_task("s2_inner", [&] { go.wait(); });
    auto proxy = shield(inner);
    auto other = create_task("s2_other", [&] { block.wait(); });
    auto grp = create_task("s2_grp", [&] {
      grp_ready.set();
      try {
        gather(proxy, other);
        throw std::logic_error("case2: unreachable");
      } catch (const Cancelled&) {
        // 预期路径
      }
    });
    grp_ready.wait();  // grp 已阻塞在 gather 内（锁交接即同步）
    grp.cancel();
    grp.await();
    // gather 取消兄弟后不等其落地：proxy/other 的取消切片与 main 的唤醒
    // 同槽，先后由后端调度——显式 await 到终态再断言（确定性）。
    try {
      proxy.await();
      throw std::logic_error("case2: proxy should throw Cancelled");
    } catch (const Cancelled&) {
      // 预期路径
    }
    try {
      other.await();
      throw std::logic_error("case2: other should throw Cancelled");
    } catch (const Cancelled&) {
      // 预期路径
    }
    if (!proxy.cancelled()) throw std::logic_error("case2: proxy should be cancelled");
    if (!other.cancelled()) throw std::logic_error("case2: unshielded sibling cancelled");
    if (inner.done()) throw std::logic_error("case2: shield must protect inner from gather");
    go.set();
    inner.await();
    if (!inner.done() || inner.cancelled())
      throw std::logic_error("case2: inner should finish on its own");
  });

  // case 3: 代理镜像 inner 的失败
  run_case(__func__, "case3", [&] {
    Event go;
    auto inner = create_task("s3_inner", [&] {
      go.wait();
      throw std::runtime_error("s3_inner_fail");
    });
    auto proxy = shield(inner);
    go.set();
    try {
      proxy.await();
      throw std::logic_error("case3: proxy should rethrow inner failure");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "s3_inner_fail")
        throw std::logic_error("case3: proxy must mirror inner's exception");
    }
    try {
      inner.await();
      throw std::logic_error("case3: inner should rethrow its failure");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "s3_inner_fail") throw;
    }
  });

  // case 4: inner 在 shield 前已终态 → 代理直接镜像完成
  run_case(__func__, "case4", [&] {
    auto inner = create_task("s4_inner", [] {});
    inner.await();  // inner 已 DONE
    auto proxy = shield(inner);
    proxy.await();
    if (!proxy.done() || proxy.cancelled())
      throw std::logic_error("case4: proxy should mirror pre-completed inner");
  });

  report(__func__);
}
