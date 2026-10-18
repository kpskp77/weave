#pragma once

// 用例级结果打印（仅测试侧使用，不进公共头/核心 API）。
//
//   WEAVE_TEST(my_semantics) {
//     run_case(__func__, "case1", [&] { ...用例体... });
//     ...
//     report(__func__);   // 末尾汇总，并重置计数供下一个测试使用
//   }
//
// 用例体正常返回 → 打印 [PASS]；抛出（约定 throw std::logic_error("caseN: ...")）
// → 打印 [FAIL] 并原样重抛，由后端汇报进程把整个测试判为 FAILED。
// 本助手只做打印与计数，不引入任何等待：时序仍完全事件驱动。

#include <cstdio>
#include <exception>
#include <weave/task.hpp>

namespace weave_test {

inline int& passed_count() {
  static int n = 0;
  return n;
}

inline int& failed_count() {
  static int n = 0;
  return n;
}

// 运行单个用例并打印结果；失败时先打印再重抛，保留原有失败传播路径。
template <typename F>
void run_case(const char* test, const char* name, F&& body) {
  try {
    body();
  } catch (const weave::Cancelled& e) {
    // Cancelled 刻意不派生 std::exception（见 include/weave/task.hpp），
    // 必须在 std::exception 之前单独接住，否则只会落到 catch (...)。
    ++failed_count();
    std::printf("[FAIL] %s::%s: %s\n", test, name, e.what());
    std::fflush(stdout);
    throw;
  } catch (const std::exception& e) {
    ++failed_count();
    std::printf("[FAIL] %s::%s: %s\n", test, name, e.what());
    std::fflush(stdout);
    throw;
  } catch (...) {
    ++failed_count();
    std::printf("[FAIL] %s::%s: non-std exception\n", test, name);
    std::fflush(stdout);
    throw;
  }
  ++passed_count();
  std::printf("[PASS] %s::%s\n", test, name);
  std::fflush(stdout);
}

// 测试末尾汇总，并重置计数（测试串行运行，计数即本测试的通过/失败数）。
inline void report(const char* test) {
  std::printf("== %s: %d passed, %d failed ==\n", test, passed_count(), failed_count());
  std::fflush(stdout);
  passed_count() = 0;
  failed_count() = 0;
}

}  // namespace weave_test
