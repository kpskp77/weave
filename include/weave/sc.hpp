#pragma once

// SystemC 后端引导入口（仅 WEAVE_BACKEND_SYSTEMC 构建提供实现）。
// 典型用法（测试侧提供 sc_main）：
//
//   #include <weave/weave.hpp>
//   int sc_main(int, char**) { return weave::sc::run(); }

namespace weave {
namespace sc {

// 在 sc_main 中调用：
//   1. spawn 根任务（第一个注册的 WEAVE_TEST）与汇报进程；
//   2. sc_start() 启动仿真，汇报进程在根任务终态时打印 PASSED/FAILED 并 sc_stop()；
//   3. 返回 root_status()（0=DONE 1=FAILED 2=CANCELLED）作为进程退出码。
int run();

}  // namespace sc
}  // namespace weave
