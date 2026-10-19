#pragma once

// 后端端口契约：核心簿记层（src/core）依赖的唯一后端接口。
// 每个后端在 src/backend/ 下实现一次，编译期由 WEAVE_BACKEND_* 宏选择
// （见 include/weave/config.hpp；默认 SVDPI）。
//
// 后端必须保证的语义：
//   1. 进程模型：spawn(id) 创建执行 weave_task_main(id) 的独立仿真进程
//      （SVDPI: fork...join_none 进程；SystemC: sc_spawn 动态进程）。
//      子进程在父进程碰到下一个挂起点（或结束）后才获得调度——
//      与 asyncio create_task 的"调度而非立即执行"一致。
//   2. 单一活动进程：任一时刻至多一个进程在执行 C++ 簿记
//      （wait 挂起 ≡ 进程交出调度权，核心依赖这一点维护全局状态）。
//   3. 事件：create/trigger/destroy 构成一次性唤醒机制；
//      trigger 不得用于已 destroy 的 id（核心已在各等待路径摘除）。
//   4. wait 是唯一挂起点：三腿竞速（事件 / 超时 / 取消），
//      同槽冲突时取消腿优先；status 0=事件命中 1=超时 2=被取消。
//      ev_id < 0 表示无事件腿；timeout_ticks < 0 表示无超时腿。
//
// 后端引导入口（不属于 port 契约，由各后端自选形式）：
//   SVDPI  : extern "C" void weave_run()          —— sv/weave_tb.sv 调用
//   SystemC : int weave::sc::run()                 —— 在 sc_main 中调用

#include <cstdint>

extern "C" void weave_task_main(int task_id);  // 任务分派入口，由核心实现

namespace weave {
namespace port {

// 创建执行 weave_task_main(task_id) 的仿真进程（见上方进程模型）。
void spawn(int task_id);

// 一次性唤醒机制。create_event 的返回值必须被保存并最终 destroy_event：
// 丢弃即泄漏一个后端事件 id（后端侧是一次真实的对象分配）。
void trigger(int ev_id);
[[nodiscard]] int create_event();
void destroy_event(int ev_id);

// 1 秒对应的后端 tick 数（决定 fs→tick 换算精度）。
std::int64_t ticks_per_sec();

// 唯一挂起点（见上方语义 4）。
void wait(int ev_id, std::int64_t timeout_ticks, int abort_id, int* status);

}  // namespace port
}  // namespace weave
