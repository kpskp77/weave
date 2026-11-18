// Periodic cadence self-check: Every keeps a consumer on a fixed grid (a fixed number of ticks
// lands exactly on period×ticks, no accumulated error), re-anchors and counts drift instead of
// firing missed ticks back to back after an overrun, paces independently when copied, rejects a
// non-positive period, and lets cancellation out of wait() like any other blocking point. Each case
// prints [PASS]/[FAIL] through run_case, and report summarises at the end (see case_runner.hpp). It
// shares the assertion-body style with the other *_semantics.cpp files and is regressed by both the
// weave_smoke_sc and weave_smoke_svdpi backends.

#include <cstdint>
#include <stdexcept>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(every_semantics) {
  // case1: the grid does not accumulate error — n ticks take exactly n×period
  run_case(__func__, "case1", [&] {
    Every tick(10_us);
    const time_point start = now();
    for (int i = 1; i <= 5; ++i) {
      tick.wait();
      if (now() - start != 10_us * i) throw std::logic_error("case1: a tick must land on the grid");
    }
    if (tick.drift() != 0) throw std::logic_error("case1: keeping up must not count as drift");
    if (tick.period() != 10_us) throw std::logic_error("case1: period() must echo the cadence");
  });

  // case2: an overrun past a whole period re-anchors onto the current time and counts the skip
  run_case(__func__, "case2", [&] {
    Every tick(1_us);
    const time_point start = now();
    tick.wait();  // Consumes the tick at start + 1us
    if (now() - start != 1_us) throw std::logic_error("case2: first tick is one period away");
    sleep(3_us);  // The consumer took 3 periods' worth of work
    tick.wait();  // Must not fire the two missed ticks back to back
    if (now() - start != 5_us)
      throw std::logic_error("case2: a late consumer re-anchors one period after now");
    if (tick.drift() != 1) throw std::logic_error("case2: the skipped ticks must be counted");
    // ... and the grid continues from the new anchor
    tick.wait();
    if (now() - start != 6_us) throw std::logic_error("case2: the cadence must resume on the grid");
  });

  // case3: copying an Every paces independently (a cadence is a value, not a shared handle)
  run_case(__func__, "case3", [&] {
    Every a(1_us);
    Every b = a;
    const time_point start = now();
    a.wait();  // Only a has ticked; now = start + 1us
    if (now() - start != 1_us) throw std::logic_error("case3: a must tick one period in");
    b.wait();  // b never advanced, so its first tick is one period after now
    if (now() - start != 2_us)
      throw std::logic_error("case3: a copy must not share the anchor with its source");
    if (a.drift() != 0 || b.drift() != 0)
      throw std::logic_error("case3: neither cadence should have drifted");
  });

  // case4: a non-positive period is a caller bug, rejected at construction
  run_case(__func__, "case4", [&] {
    bool zero_threw = false;
    try {
      Every bad(0_s);
    } catch (const std::invalid_argument&) {
      zero_threw = true;
    }
    if (!zero_threw) throw std::logic_error("case4: a zero period must be rejected");

    bool negative_threw = false;
    try {
      Every bad(-1_ms);
    } catch (const std::invalid_argument&) {
      negative_threw = true;
    }
    if (!negative_threw) throw std::logic_error("case4: a negative period must be rejected");
  });

  // case5: wait() is an ordinary blocking point, so cancellation comes out of it
  run_case(__func__, "case5", [&] {
    Every tick(1_us);
    Event entered;
    Task waiter = create_task("every_waiter", [&] {
      entered.set();
      for (;;) tick.wait();
    });
    entered.wait();  // The waiter is on the grid before we cancel it
    sleep(1_us);
    waiter.cancel("stop ticking");
    bool cancelled = false;
    try {
      waiter.await();
    } catch (const CancelledError&) {
      cancelled = true;
    }
    if (!cancelled) throw std::logic_error("case5: Every::wait() must propagate cancellation");
  });

  report(__func__);
}
