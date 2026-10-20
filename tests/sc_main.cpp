// SystemC 后端自检二进制入口（WEAVE_BACKEND_SYSTEMC）。
// 在真实 SystemC 内核上运行 tests/ 的全部语义断言。

#include <weave/sc.hpp>

// 本 xmake-repo 的 SystemC 构建按 C 链接寻找 sc_main（sc_main_main.cpp 引用
// 未修饰符号），故此处显式 extern "C"。若目标平台的 SystemC 按 C++ 链接，
// 去掉 extern "C" 即可。
extern "C" __attribute__((visibility("default"))) int sc_main(int, char**);

extern "C" __attribute__((visibility("default"))) int sc_main(int, char**) {
  return weave::sc::run();
}
