# Weave

C++ 测试逻辑与 EDA 仿真内核之间的**行为编排层**。API 组织方式借鉴 Python asyncio，不使用 C++20 coroutine——阻塞语义完全由后端承载：每个任务运行在一个仿真进程上下文（SV 进程 / SystemC SC_THREAD）上，`sleep`/`wait` 经由后端挂起点冻结整个 C++ 调用栈。

Weave 专注并发行为编排（任务、时间、事件、组合、超时、取消、状态查询）；不提供 DUT 信号读写、总线 driver/monitor、UVM component、transaction/coverage、TLM 建模、仿真内核本身。这些由 SystemVerilog/UVM/cocotb/SystemC 等既有工具提供，Weave 以统一接口把 C++ 测试行为接入它们。

## C++ ↔ asyncio 映射

Weave 的 API 语义**以 asyncio 为准**——下表即完整参考；与 asyncio 的差异只列在[已知偏差](#已知偏差)。

| C++（weave） | Python（asyncio） | 备注 |
|---|---|---|
| `create_task(name, fn)` | `asyncio.create_task(coro)` | fn 为普通函数；调度而非立即运行 |
| `task.await()` | `await task` | FAILED 重抛其异常；CANCELLED 抛 `Cancelled` |
| `task.cancel()` / `task.cancel(msg)` | `task.cancel(msg)` | 协作式；msg 对齐 asyncio 3.9+，注入与 await 重投时保留 |
| `task.done()` / `task.cancelled()` | `task.done()` / `task.cancelled()` | |
| `task.get_name()` | `task.get_name()` | |
| `sleep(d)` | `await asyncio.sleep(t)` | `sleep(0)` 为零时间让出 |
| `Event::wait/set/clear` | `asyncio.Event` | set/clear 竞态采用 edge 语义 |
| `Lock::acquire/release/locked` | `asyncio.Lock` | FIFO 交接；`LockGuard` ≡ `async with lock` |
| `Semaphore::acquire/release/locked` | `asyncio.Semaphore` | 计数信号量；`SemaphoreGuard` ≡ `async with semaphore` |
| `gather(t1, t2, ...)` | `await asyncio.gather(*aws)` | 默认形态（首失败即抛） |
| `gather({t1, ...}, return_exceptions)` | `await asyncio.gather(*aws, return_exceptions=True)` | 返回 `vector<exception_ptr>`，不提前抛出 |
| `wait_for(t, d)` | `await asyncio.wait_for(aw, t)` | asyncio 3.12+ 超时语义 |
| `wait({t1, ...}, return_when, timeout)` | `await asyncio.wait(fs, ...)` | 返回 `{done, pending}`；不取消子任务 |
| `shield(t)` | `asyncio.shield(aw)` | 返回代理任务；取消/超时止于代理 |
| `as_completed({t1, ...})` | `asyncio.as_completed(fs)` | `next()` 按完成顺序阻塞产出 |
| `TaskGroup` + `tg.create_task` / `tg.wait()` | `asyncio.TaskGroup` | 结构化并发；首失败取消兄弟 |
| `Cancelled` / `Timeout` | `CancelledError` / `TimeoutError` | 取消不派生 `std::exception`（对齐 `BaseException` 分层）；`Timeout` 派生 |
| `WEAVE_TEST` + 注册表 | pytest / 各类 runner | 无对应原语；由后端引导入口运行 |

规划中的扩展同样一一对齐 asyncio：`Queue` 等，见[演进方向](#演进方向)。

### 模型差异（相对 Python，必读）

映射表之外有三条结构性差异，误用大多源于此处：

- **无 coroutine / await**：组合原语是任务上下文内的普通阻塞调用，不是可等待对象；`gather(...)` 直接阻塞到完成，不返回 future。
- **无事件循环对象**：调度由仿真内核承担。`create_task` 创建的任务在**父任务碰到下一个阻塞点之后**才开始运行（对应 asyncio 的调度时机）。
- **取消不破坏调用栈**：协作式取消 + 禁止强杀仿真进程，C++ RAII 始终正常展开（区别于线程取消）。

### 已知偏差

- `TaskGroup` 无 `ExceptionGroup`：`wait()` 重抛首个失败（按完成顺序）；
- gather 提前返回后，其余任务晚到的失败仅留在任务簿记中（无 Python 的 "exception was never retrieved" 告警）。

## 快速开始

```cpp
#include <weave/weave.hpp>
using namespace weave;
using namespace weave::literals;

WEAVE_TEST(my_test) {
    Event ev;
    auto t1 = create_task("producer", [&] { sleep(100_ns); ev.set(); });
    auto t2 = create_task("consumer", [&] { ev.wait(); /* ... */ });
    wait_for(t1, 1_us);          // 超时即取消任务并抛 weave::Timeout
    gather(t1, t2);
}
```

## 构建与运行

```bash
xmake smoke      # 语义自检：构建 smoke 载荷 + 用 xmake run 逐个跑 weave_smoke_*（两种后端各跑一次）
xmake examples   # 示例回归：一次跑完 systemc + svdpi
```

两者职责不同、都不可省：`tests/` 的断言体与 `examples/` 的示例互不编入对方载荷；示例按用户用法编译
（只用公共头、链接 `libweave.so`，真实跨过 DSO 边界）。产物在 `build/<plat>/<arch>/<mode>/`；无 VCS/IES
时 SVDPI 侧驱动打印 SKIP 并返回 0（等价于只做编译验证）。后端切换、SystemC 引导入口、VCS 手工运行、
新增后端见 [docs/backends.md](docs/backends.md)。

> 把 `libweave.so` 链进**自己的可执行文件**时**无需**任何 RTTI 可见性/导出标志（`-fvisibility=default`
> / `-rdynamic` 都不需要）：两个异常类型的 `what()` 是库内外联定义的 key function，typeinfo 在库里
> 只有一份、消费方只留未定义引用（见 backends.md 的「ABI 提示」）。

## 语言标准：C++17 起

全仓库以 **C++17 为最低标准**（`xmake.lua` 的 `set_languages("c++17")` 是唯一开关）：库自身按 C++17
构建，且公开能力在 C++17 下必须功能完备。**更新的标准同样支持**——调用方可以自己选 `-std=c++20` /
`c++23` / `c++2c` 编译，公共头在 C++17/20/23 下均验证可干净编译。

- **17 即完备**：公共头不把任何 C++20+ 特性当作必需（`Cancelled::msg()` 是
  `std::optional<std::string_view>`，`wait` 的 timeout 是 `std::optional<duration>`）。
- **更高标准也可用**：公共头不依赖随标准版本改变含义、已被弃用或已移除的写法，因此升级 `-std=` 不会
  引入告警或行为变化。
- **不使用 C++20 coroutine 是设计决定而非标准限制**：挂起由仿真内核承载，与标准版本无关。

详见 [docs/architecture.md](docs/architecture.md) 的「语言基线」一节。

## 目录结构

```
include/weave/    公共 API：weave.hpp(umbrella) + config/time/task/event/locks/sync/test[/sc]
src/core/         后端无关核心 + port.hpp 后端契约
src/backend/      后端适配（宏二选一）：svdpi.cpp（默认）/ systemc.cpp
sv/               SVDPI 后端协议（事件表、三腿等待、spawn、tb 骨架）
examples/         按域演示全部公共 API + 一个端到端用例（xmake examples 回归）
tests/            语义自检断言体（xmake smoke 回归）
docs/             architecture.md / backends.md
```

## 延伸阅读

- [docs/architecture.md](docs/architecture.md)：设计决策与实现约束（为何不用 coroutine、取消、edge 语义、时间、语言基线）
- [docs/backends.md](docs/backends.md)：DPI 协议、SystemC 集成、ABI 注意事项、新增后端指南
- [AGENTS.md](AGENTS.md)：面向 AI 编码助手的构建/验证约定与不变量

## 演进方向

- **通信原语**：Queue（`Lock` / `Condition` 见 `include/weave/locks.hpp`，
  `Semaphore` 见 `include/weave/semaphore.hpp`；其余新头文件 queue.hpp）
- **后端**：契约稳定于 `src/core/port.hpp`；SVDPI 与 SystemC 均已在各自工具链验证
