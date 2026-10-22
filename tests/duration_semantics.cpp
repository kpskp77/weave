// duration arithmetic semantic self-check: + - * / with integer scaling, compound assignment, the
// overflow/division-by-zero guards, and negative durations (including the 0-tick semantics when
// handed to sleep / wait_for).
// The first six cases are pure value arithmetic; the last two use event passing to confirm that a
// computed delay is really carried by the backend (no deadlock, not mistaken for "no timeout leg"),
// while synchronisation stays fully event-driven.
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

  report(__func__);
}
