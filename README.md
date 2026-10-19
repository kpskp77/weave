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
| `gather(t1, t2, ...)` | `await asyncio.gather(*aws)` | 默认形态（首失败即抛） |
| `gather({t1, ...}, return_exceptions)` | `await asyncio.gather(*aws, return_exceptions=True)` | 返回 `vector<exception_ptr>`，不提前抛出 |
| `wait_for(t, d)` | `await asyncio.wait_for(aw, t)` | asyncio 3.12+ 超时语义 |
| `wait({t1, ...}, return_when, timeout)` | `await asyncio.wait(fs, ...)` | 返回 `{done, pending}`；不取消子任务 |
| `shield(t)` | `asyncio.shield(aw)` | 返回代理任务；取消/超时止于代理 |
| `as_completed({t1, ...})` | `asyncio.as_completed(fs)` | `next()` 按完成顺序阻塞产出 |
| `TaskGroup` + `tg.create_task` / `tg.wait()` | `asyncio.TaskGroup` | 结构化并发；首失败取消兄弟 |
| `Cancelled` / `Timeout` | `CancelledError` / `TimeoutError` | 取消不派生 `std::exception`（对齐 `BaseException` 分层）；`Timeout` 派生 |
| `WEAVE_TEST` + 注册表 | pytest / 各类 runner | 无对应原语；由后端引导入口运行 |

规划中的扩展一一对齐 asyncio：`Queue` / `Lock` / `Semaphore` / `Condition`，见[演进方向](#演进方向)。

### 模型差异（相对 Python，必读）

映射表之外有三条结构性差异，误用大多源于此处：

- **无 coroutine / await**：组合原语是任务上下文内的普通阻塞调用，不是可等待对象；`gather(...)` 直接阻塞到完成，不返回 future。
- **无事件循环对象**：调度由仿真内核承担。`create_task` 创建的任务在**父任务碰到下一个阻塞点之后**才开始运行（后端进程语义保证，对应 asyncio 的调度时机）。
- **取消不破坏调用栈**：协作式取消 + 禁止强杀仿真进程，C++ RAII 始终正常展开（区别于线程取消）。

### 已知偏差

- `TaskGroup` 无 `ExceptionGroup`：`wait()` 重抛首个失败（按完成顺序）；
- gather 提前返回后，其余任务晚到的失败仅留在任务簿记中（无 Python 的 "exception was never retrieved" 告警）。

## 目录结构

```
include/weave/    公共 API：weave.hpp(umbrella) + 按领域拆分
                  config（后端宏）/ time / task / event / sync / test / sc
src/core/         后端无关核心 + port.hpp 后端契约（7 个原语）
src/backend/      后端适配（宏二选一）：svdpi.cpp（默认）/ systemc.cpp
sv/               SVDPI 后端协议（事件表、三腿等待、spawn、tb 骨架）
examples/         示例：按域演示全部公共 API（task / event / sync / taskgroup）
                  外加一个端到端用例；纳入 xmake smoke 的**双后端**回归
tests/            SystemC 内核上的语义自检（断言与后端 main 分离成文件）
docs/             技术细节：architecture.md / backends.md
AGENTS.md         给 AI 编码助手的操作手册
```

架构与关键设计决策（为什么不用 coroutine、协作式取消、edge 语义、时间精度、头文件分层、边界）：[docs/architecture.md](docs/architecture.md)。

## 语言标准：C++17

Weave 以 **C++17** 为最低标准，全仓库统一（`xmake.lua` 的 `set_languages("c++17")` 是唯一开关），不启用 C++20 及更高特性。

- **调用方同样要求 C++17+**：公共 API 直接暴露 C++17 标准库类型——`Cancelled::msg()` 返回 `std::optional<std::string_view>`，`wait(...)` 的 timeout 是 `std::optional<duration>`。用 `-std=c++17`（或更高）编译测试库即可，无需额外宏或依赖。
- **为什么是 17 而不是 20**：本项目需要的特性集（`std::optional` / `std::string_view` / `inline` 变量 / if-init / `[[nodiscard]]` / 结构化绑定 / 放宽的 `constexpr`）在 C++17 已齐备；**不使用 C++20 coroutine** 是设计决定而非标准限制——挂起由仿真内核承载（见 [docs/architecture.md](docs/architecture.md)）。
- **不引入 C++20+ 特性**：concepts / ranges / coroutine / `std::format` / designated initializers / `std::string::starts_with` / `std::span` / `std::move_only_function` 均不使用，以保持两条工具链（VCS DPI 侧宿主编译器、SystemC 3.x）都能编译。

## 构建与运行

```bash
xmake smoke      # 语义自检回归：构建全部目标 + 运行全部 *_smoke（期望 PASSED）
xmake examples   # 示例回归（独立，与 smoke 不同集）：一次跑完两种后端
```

- `xmake examples` 与 smoke 用例**分离**：示例只用公共头、链接 `libweave.so`（不内联核心
  源码），因此真实跨过 DSO 边界。它会在同一次调用里依次跑两种后端（结束后还原调用者的
  `--backend`）：
  - **SystemC**：`weave_examples_sc`（可执行文件，`weave::sc::run()` 引导）本地真实内核运行；
  - **SVDPI**：`weave_examples_svdpi`（共享库）+ VCS 驱动，与 `libweave.so` 一起经 `-sv_lib`
    加载运行；无 VCS 时只做编译验证并打印 SKIP。
- 产物（`build/<plat>/<arch>/<mode>/`）：`libweave.so`（后端由 `--backend` 选择）、
  示例两种形态 `weave_examples_sc`（可执行）与 `libweave_examples_svdpi.so`（共享库），
  以及 `*_smoke` 自检二进制。
- VCS 手工调试（SVDPI 后端；`xmake examples` 已自动完成编译与运行，下列命令仅供手工排查）：
  `vcs -full64 -sverilog sv/weave_pkg.sv sv/weave_tb.sv -sv_lib .../weave
  -sv_lib .../weave_examples_svdpi && ./simv`。
- 后端切换（`--backend=systemc`）、SystemC 集成（`weave::sc::run()`）、新增后端指南：[docs/backends.md](docs/backends.md)。
- 自检跑在真实 SystemC 内核上（xmake-repo 提供，首次构建自动安装），delta-cycle 语义保证结论确定性可复现。
- 把 `libweave.so` 链进**自己的可执行文件**（如 SystemC 应用）时，需加 `-rdynamic` 并
  用 `-fvisibility=default` 编公共头：`Cancelled` / `Timeout` 由库抛出、由你的代码 catch，
  而 libc++ 按指针比较 `type_info`——可执行文件里那份 RTTI 必须能经动态符号解析到与库里同一份。
  用 `-sv_lib` 加载两个共享库（VCS 场景）时由动态链接器统合，不受此限。

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

- **通信原语**：Queue / Lock / Semaphore / Condition（新头文件 queue.hpp / locks.hpp）
- **后端**：契约稳定于 `src/core/port.hpp`；SVDPI 与 SystemC 均已在各自工具链验证
