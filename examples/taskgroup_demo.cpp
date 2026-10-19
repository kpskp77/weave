// TaskGroup 常见用法：结构化并发——组内任务全部完成才返回；首个失败（按
// 完成顺序）取消其余兄弟，等全部落地后再重抛；外层取消则级联取消全部子任务。

#include <cstdio>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(taskgroup_demo) {
  // ── 正常路径：wait() 阻塞到组内全部任务完成 ────────────────────────────
  {
    TaskGroup tg;
    tg.create_task("tg_fast", [] { sleep(10_ns); });
    tg.create_task("tg_slow", [] { sleep(20_ns); });
    tg.wait();  // 两个都完成才返回
    std::printf("[taskgroup_demo] all children done\n");
  }

  // ── 失败路径：首个失败取消兄弟，wait() 等全部落地后重抛该异常 ──────────
  {
    TaskGroup tg;
    Event block;
    [[maybe_unused]] auto failer =
        tg.create_task("tg_failer", [] { throw std::runtime_error("boom"); });
    auto victim = tg.create_task("tg_victim", [&] { block.wait(); });
    try {
      tg.wait();
      throw std::logic_error("taskgroup_demo: wait must rethrow the first failure");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "boom") throw;
    }
    if (!victim.cancelled())
      throw std::logic_error("taskgroup_demo: siblings must be cancelled on failure");
    std::printf("[taskgroup_demo] failure cascaded to siblings\n");
  }

  // ── 外层取消：wait() 的调用者被取消 → 级联取消全部子任务 ──────────────
  {
    TaskGroup tg;
    Event block;
    auto a = tg.create_task("tg_outer_a", [&] { block.wait(); });
    auto b = tg.create_task("tg_outer_b", [&] { block.wait(); });
    auto runner = create_task("tg_runner", [&] {
      try {
        tg.wait();
      } catch (const Cancelled&) {
        // 预期：wait_for 超时取消了 runner，TaskGroup 已级联取消子任务
      }
    });
    try {
      wait_for(runner, 1_ns);
      throw std::logic_error("taskgroup_demo: wait_for must time out");
    } catch (const Timeout&) {
      // 预期路径
    }
    if (!a.cancelled() || !b.cancelled())
      throw std::logic_error("taskgroup_demo: outer cancel must cascade to all children");
    std::printf("[taskgroup_demo] outer cancel cascaded\n");
  }
}
