// Common TaskGroup usage: structured concurrency — it returns only once every task in the group
// has finished; the first failure (in completion order) cancels the remaining siblings and is
// rethrown after all of them have landed; an outer cancellation cascade-cancels every child.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(taskgroup_demo) {
  // ── normal path: wait() blocks until every task in the group finishes ──
  {
    TaskGroup tg;
    tg.create_task("tg_fast", [] { sleep(10_ns); });
    tg.create_task("tg_slow", [] { sleep(20_ns); });
    tg.wait();  // Returns only after both have finished
    std::printf("[taskgroup_demo] all children done\n");
  }

  // ── failure path: the first failure cancels siblings, and wait()
  // rethrows that exception after everything has landed ─────────────────
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

  // ── outer cancellation: the caller of wait() is cancelled → cascade
  // cancels every child task ───────────────────────────────────────────
  {
    TaskGroup tg;
    Event block;
    auto a = tg.create_task("tg_outer_a", [&] { block.wait(); });
    auto b = tg.create_task("tg_outer_b", [&] { block.wait(); });
    auto runner = create_task("tg_runner", [&] {
      try {
        tg.wait();
      } catch (const CancelledError&) {
        // Expected: wait_for timed out and cancelled runner, so TaskGroup already
        // cascade-cancelled the children
      }
    });
    try {
      wait_for(runner, 1_ns);
      throw std::logic_error("taskgroup_demo: wait_for must time out");
    } catch (const TimeoutError&) {
      // Expected path
    }
    if (!a.cancelled() || !b.cancelled())
      throw std::logic_error("taskgroup_demo: outer cancel must cascade to all children");
    std::printf("[taskgroup_demo] outer cancel cascaded\n");
  }
}
