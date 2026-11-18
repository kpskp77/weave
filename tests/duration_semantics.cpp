// duration arithmetic semantic self-check: + - * / with integer scaling, compound assignment, the
// overflow/division-by-zero guards, and negative durations (including the 0-tick semantics when
// handed to sleep / wait_for).
// The first six cases are pure value arithmetic; the last two use event passing to confirm that a
// computed delay is really carried by the backend (no deadlock, not mistaken for "no timeout leg"),
// while synchronisation stays fully event-driven. Cases 9-12 cover absolute time (time_point
// arithmetic, the now/now_ticks clock query and wait_until).
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(duration_semantics) {
  // case 1: addition/subtraction and compound assignment — concatenating/differencing two spans,
  // accumulating a variable delay
  run_case(__func__, "case1", [&] {
    if ((2_ns + 3_ns).count() != (5_ns).count())
      throw std::logic_error("case1: duration + duration wrong");
    if ((10_ns - 4_ns).count() != (6_ns).count())
      throw std::logic_error("case1: duration - duration wrong");
    if ((1_s + 500_ms).count() != (1500_ms).count())
      throw std::logic_error("case1: large durations must add exactly");
    duration total;  // 0 by default
    total += 100_ns;
    total += 50_ns + 50_ns;
    total -= 50_ns;
    if (total.count() != (150_ns).count())
      throw std::logic_error("case1: compound assignment must accumulate");
  });

  // case 2: integer scaling — k * d and d * k are equivalent, the main way to build variable
  // delays
  run_case(__func__, "case2", [&] {
    const int rounds = 7;  // A runtime variable, not a literal
    if ((rounds * 100_ns).count() != (700_ns).count())
      throw std::logic_error("case2: int * duration wrong");
    if ((100_ns * rounds).count() != (rounds * 100_ns).count())
      throw std::logic_error("case2: scaling must be commutative");
    if ((3_ns * 0).count() != 0) throw std::logic_error("case2: scaling by 0 must be 0");
    duration d = 10_ns;
    d *= 3;
    if (d.count() != (30_ns).count()) throw std::logic_error("case2: operator*= wrong");
  });

  // case 3: division — duration/int truncates toward zero, duration/duration yields an integer
  // multiple
  run_case(__func__, "case3", [&] {
    if ((1_us / 4).count() != (250_ns).count())
      throw std::logic_error("case3: duration / int wrong");
    if ((5_fs / 2).count() != 2)
      throw std::logic_error("case3: division must truncate toward zero");
    if ((duration{-5} / 2).count() != -2)
      throw std::logic_error("case3: negative division must truncate toward zero");
    if (1_us / 100_ns != 10) throw std::logic_error("case3: duration / duration must be a ratio");
    duration d = 1_us;
    d /= 4;
    if (d.count() != (250_ns).count()) throw std::logic_error("case3: operator/= wrong");
  });

  // case 4: duration is signed — a difference may be negative, and unary minus restores it
  run_case(__func__, "case4", [&] {
    const duration back = 3_ns - 5_ns;
    if (back.count() != -(2_ns).count())
      throw std::logic_error("case4: difference may be negative");
    if ((-back).count() != (2_ns).count()) throw std::logic_error("case4: unary minus wrong");
    if ((back + 2_ns).count() != 0)
      throw std::logic_error("case4: negative duration must add back");
  });

  // case 5: overflow never wraps silently — add/sub/mul/div/negate all throw std::overflow_error
  run_case(__func__, "case5", [&] {
    const duration max = duration(INT64_MAX);
    const duration min = duration(INT64_MIN);
    const auto expect_overflow = [](const char* what, auto&& f) {
      try {
        f();
      } catch (const std::overflow_error&) {
        return;
      }
      throw std::logic_error(std::string("case5: ") + what + " must throw std::overflow_error");
    };
    expect_overflow("max + 1_fs", [&] { (void)(max + 1_fs); });
    expect_overflow("min - 1_fs", [&] { (void)(min - 1_fs); });
    expect_overflow("max * 2", [&] { (void)(max * 2); });
    expect_overflow("min * -1", [&] { (void)(min * -1); });
    expect_overflow("min / -1", [&] { (void)(min / -1); });
    expect_overflow("-min", [&] { (void)(-min); });
    expect_overflow("d += 1_fs at max", [&] {
      duration d = max;
      d += 1_fs;
    });
    // The boundary values themselves remain usable: the guards must not flag legal operations as
    // overflow
    if ((max + duration{}).count() != INT64_MAX)
      throw std::logic_error("case5: max + 0 must stay exact");
    if ((max - max).count() != 0) throw std::logic_error("case5: max - max must be 0");
    if ((min + duration{}).count() != INT64_MIN)
      throw std::logic_error("case5: min + 0 must stay exact");
  });

  // case 6: division by zero (including duration/duration) throws std::domain_error
  run_case(__func__, "case6", [&] {
    const auto expect_domain_error = [](const char* what, auto&& f) {
      try {
        f();
      } catch (const std::domain_error&) {
        return;
      }
      throw std::logic_error(std::string("case6: ") + what + " must throw std::domain_error");
    };
    expect_domain_error("d / 0", [&] { (void)(1_ns / 0); });
    expect_domain_error("d /= 0", [&] {
      duration d = 1_ns;
      d /= 0;
    });
    expect_domain_error("d / zero duration", [&] { (void)(1_ns / duration{}); });
  });

  // case 7: a non-positive duration handed to sleep → a 0-tick yield (≡ asyncio: delay <= 0 yields
  // once).
  // Key point: a negative value must not leak into the port as a negative tick (a negative tick
  // means "no timeout leg" in the wire format, which would turn sleep into a permanent block).
  run_case(__func__, "case7", [&] {
    Event finished;
    auto t = create_task("non_positive_sleep", [&] {
      sleep(-1_s);
      sleep(0_fs);
      sleep(duration{} - 5_ms);
      sleep(-(3_ns * 2));
      finished.set();
    });
    finished.wait();
    t.await();
    if (!t.done() || t.cancelled())
      throw std::logic_error("case7: non-positive delay must yield once, not block");
  });

  // case 8: a delay computed from variables really drives a backend suspension (the arithmetic
  // result must reach the port)
  run_case(__func__, "case8", [&] {
    Event started, finished;
    const int rounds = 3;
    const duration step = 20_ns;
    auto t = create_task("var_delay", [&] {
      started.set();
      sleep(rounds * step);  // 3 * 20ns, computed at runtime
      finished.set();
    });
    started.wait();  // t is now blocked on the computed delay
    finished.wait();
    t.await();
    if (!t.done()) throw std::logic_error("case8: computed delay must complete");
  });

  // case 9: time_point arithmetic — a time point plus/minus a duration stays a time point, the
  // difference of two time points is the elapsed duration, and comparisons order them
  run_case(__func__, "case9", [&] {
    const time_point a(1_us);
    const time_point b = a + 250_ns;
    if (b.time_since_epoch().count() != (1250_ns).count())
      throw std::logic_error("case9: time_point + duration wrong");
    if ((250_ns + a).time_since_epoch().count() != (1250_ns).count())
      throw std::logic_error("case9: duration + time_point wrong");
    if ((b - 250_ns).time_since_epoch().count() != (1_us).count())
      throw std::logic_error("case9: time_point - duration wrong");
    if ((b - a).count() != (250_ns).count())
      throw std::logic_error("case9: time_point - time_point must yield the elapsed duration");
    if (!(a < b) || a >= b || a == b || !(a != b))
      throw std::logic_error("case9: time_point ordering wrong");
    time_point c = a;
    c += 1_ms;
    c -= 1_us;
    if (c.time_since_epoch().count() != (1_ms).count())
      throw std::logic_error("case9: time_point compound assignment wrong");
    if (time_point{} != time_point(duration{}))
      throw std::logic_error("case9: default time_point must be the epoch");
  });

  // case 10: the clock query (now / now_ticks) — non-decreasing, consistent with sleep: a
  // suspension advances it by at least the requested delay and by at most one rounding-up tick
  run_case(__func__, "case10", [&] {
    const duration t0 = now();
    const std::int64_t k0 = now_ticks();
    if (now() < t0 || now_ticks() < k0)
      throw std::logic_error("case10: the clock must not go backwards");

    sleep(3_ns);
    const duration t1 = now();
    const std::int64_t k1 = now_ticks();
    if (t1 - t0 < 3_ns)
      throw std::logic_error("case10: sleep must advance the clock by at least the delay");
    if (t1 - t0 > 3_ns + 1_ps)
      throw std::logic_error("case10: sleep must not overshoot by more than one tick");
    if (k1 <= k0) throw std::logic_error("case10: tick query must advance with the delay");

    // A sub-tick delay is rounded up, never dropped: the clock advances by one tick at most.
    const duration before = now();
    sleep(1_fs);
    const duration step = now() - before;
    if (step < 1_fs || step > 1_ps)
      throw std::logic_error("case10: sub-tick delay must round up to one tick");
  });

  // case 11: wait_until on an expired deadline returns without suspending. Proof: a task spawned
  // beforehand has not run yet (spawn schedules, it does not run immediately), so if wait_until
  // had performed even a 0-tick yield the child would have set `ran`.
  run_case(__func__, "case11", [&] {
    Event ran;
    auto t = create_task("child_sets_event", [&] { ran.set(); });
    const std::int64_t before = now_ticks();
    wait_until(now() - 1_ms);
    if (now_ticks() != before)
      throw std::logic_error("case11: an expired deadline must not advance time");
    if (ran.is_set()) throw std::logic_error("case11: an expired deadline must not even yield");
    ran.wait();
    t.await();
    if (!t.done()) throw std::logic_error("case11: child must finish");
  });

  // case 12: wait_until on a future deadline really suspends until backend time reaches it (the
  // clock never lands before the deadline: the fs→tick conversion rounds up)
  run_case(__func__, "case12", [&] {
    const time_point deadline = now() + 2_ns;
    wait_until(deadline);
    if (now() < deadline) throw std::logic_error("case12: wait_until returned before the deadline");
    if (now() - deadline > 2_ps)
      throw std::logic_error("case12: wait_until must not wake more than a rounding tick late");
  });

  report(__func__);
}
