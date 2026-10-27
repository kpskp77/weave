#pragma once

// Test registration. Users define tests via WEAVE_TEST(name) { ... };
// they are run by the backend bootstrap entry point (SVDPI: weave_start, reached from the
// package's weave_run task; SystemC: weave::sc::run()). All registered tests run serially in name
// order (any failure fails the whole run); plusarg selection is left as a future extension.

#include <string>

namespace weave {

using test_fn = void (*)();
bool register_test(std::string name, test_fn fn);  // The true return is just for static init

}  // namespace weave

#define WEAVE_TEST(name)                                                             \
  static void name();                                                                \
  static const bool name##_weave_registered_ = ::weave::register_test(#name, &name); \
  static void name()
