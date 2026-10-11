#pragma once

// 后端引导用的内部入口。非公共 API，仅供 src/backend 使用。

namespace weave {
namespace detail {

// 创建并 spawn 根任务（第一个注册的测试）。
// 返回根任务 done 事件 id；无注册测试返回 -1。
int run_registered_test();

// 根任务状态：0=DONE 1=FAILED 2=CANCELLED -1=未完成
int root_status();

}  // namespace detail
}  // namespace weave
