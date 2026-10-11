#pragma once

// 测试注册。用户通过 WEAVE_TEST(name) { ... } 定义测试；
// 由后端引导入口运行（SVDPI: weave_run；SystemC: weave::sc::run()）。
// 所有注册测试按名字序串行运行（任一失败即整体失败）；plusarg 选择留作扩展。

#include <string>

namespace weave {

using test_fn = void (*)();
bool register_test(std::string name, test_fn fn);  // 返回 true 仅供静态初始化

}  // namespace weave

#define WEAVE_TEST(name)                                                             \
  static void name();                                                                \
  static const bool name##_weave_registered_ = ::weave::register_test(#name, &name); \
  static void name()
