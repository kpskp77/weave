// Performance regression self-check: exercises the three workloads that used to be quadratic in the
// SVDPI backend — (1) the timer service's per-wake deadline rescan, (2) the level-sensitive wait
// whose registration re-evaluated every prior waiter, and (3) the full-registry trigger walk — each
// under a wall-clock budget.
//
// Wall-clock is used only as a *budget*, never for synchronisation: each case is fully event-driven
// (a counter + Event rendezvous, then set()/wait()), so ordering never rests on timing. The budgets
// sit ~10x above the fixed backend's measured cost (VCS O-2018.09-SP2: 0.42 / 0.34 / 0.15 s) and
// ~an order of magnitude below the quadratic forms they guard against (58.7 / 47.7 / 10.7 s), so a
// regression fails loudly instead of silently slowing the suite down (or pegging the simulator at
// $time == 0). The measured time is printed for diagnosis.

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

namespace {

using steady = std::chrono::steady_clock;

long elapsed_ms(steady::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - t0).count();
}

[[noreturn]] void budget_fail(const char* name, long ms, long budget) {
  throw std::logic_error(std::string(name) + ": " + std::to_string(ms) + " ms > budget " +
                         std::to_string(budget) + " ms (quadratic regression?)");
}

}  // namespace

WEAVE_TEST(performance_semantics) {
  // case 1: staggered timeouts. The timer service used to rescan every armed deadline on every
  // wake (O(N²)); the deadline min-heap keeps it O(N log N).
  run_case(__func__, "staggered_timeouts", [&] {
    constexpr int kN = 10000;
    constexpr long kBudgetMs = 5000;
    const steady::time_point t0 = steady::now();
    TaskGroup tg;
    for (int i = 0; i < kN; i++) {
      tg.create_task("s" + std::to_string(i), [i] { weave::sleep(i * 1_ns); });
    }
    tg.wait();
    const long ms = elapsed_ms(t0);
    std::printf("[perf] staggered_timeouts: %ld ms\n", ms);
    if (ms > kBudgetMs) budget_fail("staggered_timeouts", ms, kBudgetMs);
  });

  // case 2: many waiters on one event. The wait used to observe the registry array
  // level-sensitively, so every registration re-evaluated every prior waiter (O(N²)).
  run_case(__func__, "shared_event_wake", [&] {
    constexpr int kN = 10000;
    constexpr long kBudgetMs = 4000;
    Event ev, all_arrived;
    int arrived = 0;
    const steady::time_point t0 = steady::now();
    TaskGroup tg;
    for (int i = 0; i < kN; i++) {
      tg.create_task("w" + std::to_string(i), [&] {
        if (++arrived == kN) all_arrived.set();
        ev.wait();
      });
    }
    all_arrived.wait();  // every waiter is now blocked in ev.wait()
    ev.set();            // one set() releases all kN waiters
    tg.wait();
    const long ms = elapsed_ms(t0);
    std::printf("[perf] shared_event_wake: %ld ms\n", ms);
    if (ms > kBudgetMs) budget_fail("shared_event_wake", ms, kBudgetMs);
  });

  // case 3: sequential triggers over many live waiters. The trigger used to walk the whole
  // registry per trigger (O(N²) across N triggers); the reverse index rings only that id's waiters.
  run_case(__func__, "sequential_triggers", [&] {
    constexpr int kN = 5000;
    constexpr long kBudgetMs = 2000;
    std::vector<Event> evs(kN);
    Event all_arrived;
    int arrived = 0;
    const steady::time_point t0 = steady::now();
    TaskGroup tg;
    for (int i = 0; i < kN; i++) {
      tg.create_task("t" + std::to_string(i), [i, &evs, &arrived, &all_arrived] {
        if (++arrived == kN) all_arrived.set();
        evs[i].wait();
      });
    }
    all_arrived.wait();                         // all kN waiters are blocked, each on its own event
    for (int i = 0; i < kN; i++) evs[i].set();  // trigger each event while all waiters are alive
    tg.wait();
    const long ms = elapsed_ms(t0);
    std::printf("[perf] sequential_triggers: %ld ms\n", ms);
    if (ms > kBudgetMs) budget_fail("sequential_triggers", ms, kBudgetMs);
  });

  report(__func__);
}
