#pragma once

// Per-case result printing (test side only; never part of the public headers or core API).
//
//   WEAVE_TEST(my_semantics) {
//     run_case(__func__, "case1", [&] { ...case body... });
//     ...
//     report(__func__);   // Summarises at the end and resets the counters for the next test
//   }
//
// The case body returning normally → prints [PASS]; throwing (by convention
// throw std::logic_error("caseN: ...")) → prints [FAIL] and rethrows as-is, so the backend
// reporting process judges the whole test FAILED.
// This helper only prints and counts; it introduces no waiting: timing stays fully event-driven.

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

// Runs a single case and prints its result; on failure it prints first, then rethrows, keeping the
// original failure-propagation path.
template <typename F>
void run_case(const char* test, const char* name, F&& body) {
  try {
    body();
  } catch (const weave::CancelledError& e) {
    // CancelledError deliberately does not derive from std::exception (see include/weave/task.hpp),
    // so it must be caught separately before std::exception; otherwise it would only reach catch
    // (...).
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

// Summarises at the end of a test and resets the counters (tests run serially, so the counters are
// exactly this test's pass/fail counts).
inline void report(const char* test) {
  std::printf("== %s: %d passed, %d failed ==\n", test, passed_count(), failed_count());
  std::fflush(stdout);
  passed_count() = 0;
  failed_count() = 0;
}

}  // namespace weave_test
