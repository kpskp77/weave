#pragma once

// 组合原语：gather / wait_for。语义对齐 asyncio.gather(return_exceptions=False)
// 与 asyncio.wait_for（3.12+ 超时语义）。
// 后续扩展（TaskGroup、shield、as_completed、asyncio.wait 等）放本头文件：
// 它们都建于 Event + 唯一阻塞点之上，后端无需改动。

#include <initializer_list>
#include <weave/task.hpp>
#include <weave/time.hpp>

namespace weave {

struct Timeout {};  // 对应 asyncio.TimeoutError，wait_for 超时抛出

// 等待任务组：
//   · 任一任务到达 FAILED/CANCELLED 终态 → 立即按完成顺序重抛其异常，
//     其余任务不被取消、继续运行（取消兄弟任务是 TaskGroup 的语义）
//   · 全部正常完成 → 返回
//   · gather 自身被取消 → 先取消所有未完成任务（asyncio：gather 被取消时
//     未完成的子任务一并取消），再抛 Cancelled
template <typename... Ts>
void gather(Ts&... tasks) {
  detail::gather_impl({tasks...});
}

// 超时即 cancel 任务并等待取消完成，然后抛 Timeout（asyncio 3.12+ 语义）。
void wait_for(Task& task, duration timeout);

}  // namespace weave
