# Weave 架构

面向需要理解或修改 Weave 实现的人。操作层面的不变量清单见 [AGENTS.md](../AGENTS.md)，后端接入见 [backends.md](backends.md)。

## 核心/后端分离

Weave 分两层，中间是稳定的端口契约：

```
C++ 测试逻辑（WEAVE_TEST）
        │  公共 API（include/weave/）
        ▼
核心簿记（src/core/）── 任务/事件/同步/时间/注册表
        │  唯一依赖：port.hpp（7 个原语）
        ▼
后端适配（src/backend/）── svdpi.cpp（默认）/ systemc.cpp
        │
        ▼
仿真内核（VCS/IES/Xcelium 或 SystemC）
```

- 核心层**只允许**依赖 `src/core/port.hpp` 声明的原语，不得直接引用 DPI / SystemC / 线程 API。
- 新增后端 = 实现 port 契约 + 提供引导入口，核心代码零改动。
- 后端选择的唯一开关是编译期宏（`include/weave/config.hpp`，互斥，默认 SVDPI）；xmake 的 `--backend=` 只是定义宏。

## port 契约

`src/core/port.hpp` 头注释是契约的完整描述，四条保证摘要：

1. **进程模型**：`spawn(id)` 创建执行 `weave_task_main(id)` 的仿真进程；子进程在父进程碰到下一个挂起点（或结束）后才运行——≡ asyncio `create_task` 的"调度而非立即执行"。
2. **单一活动进程**：任一时刻至多一个进程执行 C++ 簿记（核心靠此维护全局状态，`g_current` 等无锁）。
3. **一次性事件**：`create/trigger/destroy`；`trigger` 不得作用于已 `destroy` 的 id。
4. **唯一挂起点** `wait(ev, timeout, abort, status)`：三腿竞速（事件/超时/取消），同槽冲突**取消腿优先**；负 id 禁用对应腿。

## 关键设计决策

### 为什么不用 C++20 coroutine

Weave 的用户代码运行在仿真器进程内，挂起/恢复由仿真内核调度（SV 进程、SystemC SC_THREAD 挂起时整个 C++ 调用栈冻结）。用 coroutine 反而需要自建调度器，且与仿真时间/事件模型各说各话。阻塞式 API + 后端进程承载挂起，是与仿真内核最自然的契合。

### 为什么取消是协作式

`cancel()` 只置标志 + 唤醒，在阻塞点注入 `Cancelled` 异常。禁用 SV `disable` / `process::kill()`：强杀会在任意点撕裂 C++ 调用栈，RAII 不展开、资源泄漏、簿记状态错乱。这与 asyncio 的协作式取消（`CancelledError` 在 await 点抛出、可捕获拒绝）同构。

### Event 的 edge 语义

`set()` 快照当前等待者并逐个触发一次性唤醒事件，被快照即放行（即使随后 `clear()`）。这与 SV 事件触发语义一致，避免"set 后瞬间 clear 丢唤醒"的竞态。实现要点：等待返回（成功与取消**两路径**）必须先摘除再销毁自己的唤醒事件，否则后续 `set()` 会对已销毁 id 触发。

### 时间与精度

内部 int64 飞秒（10⁻¹⁵ s，对齐 SV 最大精度），提交后端时按 `ticks_per_sec()` 向上取整——保证"至少睡够"而不是可能睡短。`sleep(0)` 特判为 0 tick（后端零时间让出，≡ `asyncio.sleep(0)`）。

## 头文件分层

依赖方向自上而下，每个公共头必须可独立编译：

```
weave/config.hpp   后端选择宏（默认 SVDPI）
weave/time.hpp     duration（int64 飞秒）+ 字面量
weave/task.hpp     Task / create_task / sleep / Cancelled
weave/event.hpp    Event
weave/sync.hpp     gather / wait_for / Timeout（组合原语扩展入口）
weave/test.hpp     WEAVE_TEST 注册
weave/sc.hpp       SystemC 后端引导入口
weave.hpp          umbrella：只依赖本文件即获得全部公共 API
```

核心内部状态只放 `src/core/*_internal.hpp`（不进公共头，不跨出 src/）。

## 边界

Weave 专注并发行为编排，不覆盖 EDA 验证的全部功能。**不提供**：DUT signal-level 读写；总线 driver/monitor；UVM component hierarchy；transaction 与 coverage；SystemC TLM 建模；仿真内核本身；C++ 线程与 coroutine 调度。这些由 SystemVerilog、UVM、cocotb、SystemC 等既有工具提供——Weave 把 C++ 测试行为以统一接口接入它们。

## 面向大模型的设计理念

Weave 假设大模型熟悉 asyncio 模式，但不假设其理解硬件语义，因此把问题分两层：Weave 提供**小而稳定**的任务/事件/同步编排接口（见 README 的映射表），协议、模型和测试约束由用户与专用验证组件提供。对 AI 辅助验证，Weave 提供的是更适合生成与反馈修正的仿真控制接口，而非"自动理解硬件"的能力。
