// duration 算术语义自检：+ - * / 与整数缩放、复合赋值、越界/除零守卫、
// 负时长（含交给 sleep / wait_for 时的 0 tick 语义）。
// 前六个用例是纯值运算；后两个用例用事件传递确认算出来的时延确实能被后端
// 承载（不挂死、不被当成"无超时腿"），同步仍完全事件驱动。
// 每个用例经 run_case 打印 [PASS]/[FAIL]，末尾 report 汇总（见 case_runner.hpp）。

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
  // case 1: 加减与复合赋值——两段时长拼接/相减，累加变量时延
  run_case(__func__, "case1", [&] {
    if ((2_ns + 3_ns).count() != (5_ns).count())
      throw std::logic_error("case1: duration + duration wrong");
    if ((10_ns - 4_ns).count() != (6_ns).count())
      throw std::logic_error("case1: duration - duration wrong");
    if ((1_s + 500_ms).count() != (1500_ms).count())
      throw std::logic_error("case1: large durations must add exactly");
    duration total;  // 默认 0
    total += 100_ns;
    total += 50_ns + 50_ns;
    total -= 50_ns;
    if (total.count() != (150_ns).count())
      throw std::logic_error("case1: compound assignment must accumulate");
  });

  // case 2: 整数缩放——k * d 与 d * k 等价，是变量时延的主要写法
  run_case(__func__, "case2", [&] {
    const int rounds = 7;  // 运行期变量，而非字面量
    if ((rounds * 100_ns).count() != (700_ns).count())
      throw std::logic_error("case2: int * duration wrong");
    if ((100_ns * rounds).count() != (rounds * 100_ns).count())
      throw std::logic_error("case2: scaling must be commutative");
    if ((3_ns * 0).count() != 0) throw std::logic_error("case2: scaling by 0 must be 0");
    duration d = 10_ns;
    d *= 3;
    if (d.count() != (30_ns).count()) throw std::logic_error("case2: operator*= wrong");
  });

  // case 3: 除法——duration/整数 向零截断，duration/duration 给整数倍数
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

  // case 4: duration 是有符号量——差值可为负，一元取负可还原
  run_case(__func__, "case4", [&] {
    const duration back = 3_ns - 5_ns;
    if (back.count() != -(2_ns).count())
      throw std::logic_error("case4: difference may be negative");
    if ((-back).count() != (2_ns).count()) throw std::logic_error("case4: unary minus wrong");
    if ((back + 2_ns).count() != 0)
      throw std::logic_error("case4: negative duration must add back");
  });

  // case 5: 越界不静默回绕——加/减/乘/除/取负都抛 std::overflow_error
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
    // 边界值本身仍可用：不得因守卫把合法运算判成越界
    if ((max + duration{}).count() != INT64_MAX)
      throw std::logic_error("case5: max + 0 must stay exact");
    if ((max - max).count() != 0) throw std::logic_error("case5: max - max must be 0");
    if ((min + duration{}).count() != INT64_MIN)
      throw std::logic_error("case5: min + 0 must stay exact");
  });

  // case 6: 除零（含 duration/duration）抛 std::domain_error
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

  // case 7: 非正时长交给 sleep → 0 tick 让出（≡ asyncio：delay <= 0 只让出一次）。
  // 关键点：负值不得作为负 tick 漏进 port（负 tick 在线格式里是"无超时腿"，
  // 会让 sleep 变成永久阻塞）。
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

  // case 8: 用变量算出的时延真正驱动一次后端挂起（算数结果必须能落到 port）
  run_case(__func__, "case8", [&] {
    Event started, finished;
    const int rounds = 3;
    const duration step = 20_ns;
    auto t = create_task("var_delay", [&] {
      started.set();
      sleep(rounds * step);  // 3 * 20ns，运行期算出
      finished.set();
    });
    started.wait();  // t 已阻塞在算出来的时延上
    finished.wait();
    t.await();
    if (!t.done()) throw std::logic_error("case8: computed delay must complete");
  });

  report(__func__);
}
