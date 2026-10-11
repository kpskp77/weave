# Weave

C++ 测试逻辑与 EDA 仿真内核之间的**行为编排层**。API 组织方式借鉴 Python asyncio，不使用 C++20 coroutine——阻塞语义完全由后端承载：每个任务运行在一个仿真进程上下文（SV 进程 / SystemC SC_THREAD）上，`sleep`/`wait` 经由后端挂起点冻结整个 C++ 调用栈。

Weave 专注并发行为编排（任务、时间、事件、组合、超时、取消、状态查询）；不提供 DUT 信号读写、总线 driver/monitor、UVM component、transaction/coverage、TLM 建模、仿真内核本身。这些由 SystemVerilog/UVM/cocotb/SystemC 等既有工具提供，Weave 以统一接口把 C++ 测试行为接入它们。

## C++ ↔ asyncio 映射

Weave 的 API 语义**以 asyncio 为准**——下表即完整参考；与 asyncio 的差异只列在[已知偏差](#已知偏差)。

| C++（weave） | Python（asyncio） | 备注 |
|---|---|---|
| `create_task(name, fn)` | `asyncio.create_task(coro)` | fn 为普通函数；调度而非立即运行 |
| `task.await()` | `await task` | FAILED 重抛其异常；CANCELLED 抛 `Cancelled` |
| `task.cancel()` | `task.cancel()` | 协作式：下一阻塞点注入，可捕获拒绝 |
| `task.done()` / `task.cancelled()` | `task.done()` / `task.cancelled()` | |
| `task.get_name()` | `task.get_name()` | |
| `sleep(d)` | `await asyncio.sleep(t)` | `sleep(0)` 为零时间让出 |
| `Event::wait/set/clear` | `asyncio.Event` | set/clear 竞态采用 edge 语义 |
| `gather(t1, t2, ...)` | `await asyncio.gather(*aws)` | 默认形态（无 `return_exceptions`） |
| `wait_for(t, d)` | `await asyncio.wait_for(aw, t)` | asyncio 3.12+ 超时语义 |
| `Cancelled` / `Timeout` | `CancelledError` / `TimeoutError` | |
| `WEAVE_TEST` + 注册表 | pytest / 各类 runner | 无对应原语；由后端引导入口运行 |

规划中的扩展一一对齐 asyncio：`TaskGroup`（结构化并发）、`Queue` / `Lock` / `Semaphore` / `Condition`，见[演进方向](#演进方向)。

### 模型差异（相对 Python，必读）

映射表之外有三条结构性差异，误用大多源于此处：

- **无 coroutine / await**：组合原语是任务上下文内的普通阻塞调用，不是可等待对象；`gather(...)` 直接阻塞到完成，不返回 future。
- **无事件循环对象**：调度由仿真内核承担。`create_task` 创建的任务在**父任务碰到下一个阻塞点之后**才开始运行（后端进程语义保证，对应 asyncio 的调度时机）。
- **取消不破坏调用栈**：协作式取消 + 禁止强杀仿真进程，C++ RAII 始终正常展开（区别于线程取消）。

### 已知偏差

- 无 `return_exceptions=True` 形态；
- gather 提前返回后，其余任务晚到的失败仅留在任务簿记中（无 Python 的 "exception was never retrieved" 告警）。

## 目录结构

```
include/weave/    公共 API：weave.hpp(umbrella) + 按领域拆分
                  config（后端宏）/ time / task / event / sync / test / sc
src/core/         后端无关核心 + port.hpp 后端契约（7 个原语）
src/backend/      后端适配（宏二选一）：svdpi.cpp（默认）/ systemc.cpp
sv/               SVDPI 后端协议（事件表、三腿等待、spawn、tb 骨架）
examples/         示例：覆盖全部 MVP 原语
tests/            SystemC 内核上的语义自检（断言与后端 main 分离成文件）
docs/             技术细节：architecture.md / backends.md
AGENTS.md         给 AI 编码助手的操作手册
```

架构与关键设计决策（为什么不用 coroutine、协作式取消、edge 语义、时间精度、头文件分层、边界）：[docs/architecture.md](docs/architecture.md)。

## 构建与运行

```bash
xmake smoke    # 回归入口：构建全部目标 + 运行全部 *_smoke 自检（期望 PASSED）
```

- 默认产出 `build/<plat>/<arch>/<mode>/libweave.so / libweave_demo.so`（SVDPI 后端）。
- VCS 运行：`vcs -full64 -sverilog sv/weave_pkg.sv sv/weave_tb.sv -sv_lib .../weave -sv_lib .../weave_demo && ./simv`
- 后端切换（`--backend=systemc`）、SystemC 集成（`weave::sc::run()`）、新增后端指南：[docs/backends.md](docs/backends.md)。
- 自检跑在真实 SystemC 内核上（xmake-repo 提供，首次构建自动安装），delta-cycle 语义保证结论确定性可复现。

## 在 C++ 中写测试

```cpp
#include <weave/weave.hpp>
using namespace weave;
using namespace weave::literals;

WEAVE_TEST(my_test) {
    Event ev;
    auto t1 = create_task("producer", [&] { sleep(100_ns); ev.set(); });
    auto t2 = create_task("consumer", [&] { ev.wait(); /* ... */ });
    wait_for(t1, 1_us);          // 超时即取消并抛 weave::Timeout
    gather(t1, t2);
}
```

## 演进方向

- **sync.hpp**：TaskGroup（失败即取消兄弟，补齐 gather 不取消兄弟的另一面）、gather 的 `return_exceptions`、shield、as_completed、asyncio.wait（FIRST_COMPLETED）——均建于 Event + 唯一阻塞点之上，后端零改动
- **通信原语**：Queue / Lock / Semaphore / Condition（新头文件 queue.hpp / locks.hpp）
- **后端**：契约稳定于 `src/core/port.hpp`；SVDPI 与 SystemC 均已在各自工具链验证
