#pragma once

// Weave: C++ 测试逻辑与 EDA 仿真内核之间的行为编排层。
// API 组织方式借鉴 Python asyncio，不使用 C++20 coroutine。
// 阻塞语义完全由后端承载：每个任务运行在一个仿真进程上下文上，
// sleep/wait 经由后端挂起点冻结整个 C++ 调用栈。
//
// 本头文件为 umbrella——只依赖 <weave/weave.hpp> 即可获得全部公共 API。
// 头文件分层（依赖方向自上而下）：
//   config.hpp   后端选择（编译期宏，默认 SVDPI）
//   time.hpp     时间表示（int64 飞秒 + 字面量）
//   task.hpp     任务原语（Task / create_task / sleep / Cancelled）
//   event.hpp    事件原语（Event）
//   sync.hpp     组合原语（gather / wait_for / Timeout）
//   test.hpp     测试注册（WEAVE_TEST）

#include <weave/config.hpp>
#include <weave/event.hpp>
#include <weave/sync.hpp>
#include <weave/task.hpp>
#include <weave/test.hpp>
#include <weave/time.hpp>
