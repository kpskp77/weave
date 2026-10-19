# AGENTS.md

本文件面向在本仓库工作的 AI 编码助手：说明架构约束、构建/验证命令，以及改动时必须遵守的不变量。项目定位见 [README.md](README.md)（含 C++↔asyncio 映射），架构与边界见 [docs/architecture.md](docs/architecture.md)，后端接入见 [docs/backends.md](docs/backends.md)。

## 项目一句话

Weave 是 C++ 测试逻辑与 EDA 仿真内核之间的行为编排层：API 借鉴 Python asyncio，但**不用 C++20 coroutine**——阻塞语义由后端仿真进程承载（SV 进程 / SystemC SC_THREAD 挂起时整个 C++ 调用栈冻结）。

## 构建与验证（改动后必做）

**原则：每次改动都要回归所有用例，不挑跑、不省略。**

```bash
xmake smoke      # 语义自检：构建全部目标 + 运行全部 *_smoke 自检二进制
xmake examples   # 示例回归（独立）：一次跑完 systemc + svdpi 两种后端
```

- 自检目标统一以 `_smoke` 结尾（xmake.lua 中的 `smoke` 任务按此模式自动发现并纳入），
  新增用例无需修改回归命令。
- **示例与自检分离**：`examples/` 按**用户用法**编译——只用 `include/weave/` 的公共 API，
  通过 `add_deps("weave")` 链接 `libweave.so`（不把核心源码编进去），因此真实跨过 DSO 边界。
  `xmake examples` 在**一次调用里依次跑两种后端**（任务内部切换 `--backend`，结束时还原）：
  - `systemc` → `weave_examples_sc`（可执行文件，`weave::sc::run()` 引导）本地运行；
  - `svdpi`   → `weave_examples_svdpi`（共享库）+ `weave_examples_svdpi_driver`
                （同一份 `tests/svdpi_driver.cpp`）在 VCS 上 `-sv_lib` 加载运行，
                无 VCS 时驱动打印 SKIP 并返回 0（等价于只做编译验证）。
  因此改公共头/示例只需跑一次 `xmake examples`；`xmake smoke` 仍需两种 `--backend` 各跑一次。
- 跨后端的改动（`port.hpp`、后端文件、`config.hpp`、xmake 宏映射）必须在两种配置下各跑一次：

```bash
xmake f --backend=svdpi   && xmake smoke
xmake f --backend=systemc && xmake smoke
xmake f --backend=svdpi   # 还原默认配置
```

- 自检跑在真实 SystemC 内核上（xmake-repo 提供，首次构建自动安装），delta-cycle 语义保证结论确定性可复现。
- SVDPI 后端自检（`weave_svdpi_smoke`）跑在真实 VCS 上，与 `weave_sc_smoke` 共用同一
  断言体 `tests/gather_semantics.cpp`；驱动程序找不到 vcs（`VCS_HOME`/`PATH`）时打印 SKIP
  并以 0 退出。本地没有 VCS/IES 环境即自动跳过，无需手工干预。
- 断言体集中在 `tests/` 且与后端 main 入口分离成文件；新用例按现有 case 风格追加。
- 如果本地没有VCS/IES环境，跳过SVDPI后端的验证。

## 架构：核心/后端分离

核心簿记（`src/core/`）**只允许依赖 `src/core/port.hpp` 的 7 个原语**与内部头，不得直接引用任何 DPI/SystemC/线程 API：

```
include/weave/    公共 API：weave.hpp(umbrella) + config/time/task/event/sync/test[/sc]
src/core/         后端无关：task/event/sync/time/registry + port.hpp(契约) + *_internal.hpp
src/backend/      svdpi.cpp(默认) / systemc.cpp，整文件由 WEAVE_BACKEND_* 宏守卫
```

后端选择的唯一开关是编译期宏（`include/weave/config.hpp`，互斥，默认 SVDPI）；xmake 的 `--backend=` 只是定义宏。新增后端 = 新增一个宏守卫的 `src/backend/*.cpp` 实现 port 契约 + 自选引导入口，**核心代码零改动**。

### port 契约的四条保证（后端必须满足，见 port.hpp 头注释）

1. **进程模型**：`spawn(id)` 创建执行 `weave_task_main(id)` 的仿真进程；子进程在父进程碰到下一个挂起点（或结束）后才运行——≡ asyncio `create_task` 的"调度而非立即执行"。
2. **单一活动进程**：任一时刻至多一个进程执行 C++ 簿记（核心靠此维护全局状态，`g_current` 等无锁）。
3. **一次性事件**：`create/trigger/destroy`；`trigger` 不得作用于已 `destroy` 的 id（核心已在各等待路径摘除）。
4. **唯一挂起点** `wait(ev, timeout, abort, status)`：三腿竞速，同槽冲突**取消腿优先**；`status` 0=事件 1=超时 2=取消；负 id 表示该腿禁用。

## 不可破坏的设计不变量

改动 `src/core/` 时逐条自查：

- **唯一阻塞点**：一切挂起都经 `detail::backend_wait()` → `port::wait`。不得在任何其他地方引入挂起。
- **协作式取消**：`cancel(msg)` 只置 `cancel_armed` + 记录 msg + 触发私有 abort 事件；`Cancelled(msg)` 在阻塞点注入；body 吞掉 `Cancelled` 正常返回 → 任务 DONE。禁止强杀进程（破坏 C++ RAII 展开）。msg 对齐 asyncio 3.9+：注入点与 `await`/`gather` 重投处保留同一 msg，级联取消兄弟传空 msg。
  异常类型分层不可改：`Cancelled` **不派生** `std::exception`（对齐 asyncio 的 `BaseException` 分层，避免被 `catch (const std::exception&)` 兜底静默吞掉取消）；`Timeout` **派生** `std::exception`（对齐 `TimeoutError` 属普通 `Exception`）。
- **Event edge 语义**：`set()` 快照当前等待者并逐个触发一次性唤醒事件，放行后不复查 flag。`Event::wait()` 成功与取消**两条路径都必须先从 `wake_evs` 摘除再销毁唤醒事件**——否则后续 `set()`（如 gather 提前返回后晚到的兄弟完成）会对已销毁 id 触发（SV 侧空指针解引用）。此 bug 曾真实发生过。
- **时间**：内部 int64 飞秒；`to_ticks` 向上取整保证至少睡够；`sleep(0)` 保持 0 tick（零时间让出）。
  int64 飞秒上限 ≈ 9223 s ≈ 2.56 h 仿真时间，越界字面量是**编译错误**
  （`include/weave/time.hpp` 的 constexpr 检查），不会静默回绕成负数。
- **失败传播的 asyncio 语义**：`gather` 首个失败按**完成顺序**立即重抛，兄弟任务不取消（取消兄弟是 TaskGroup 语义）；gather 自身被取消时先级联取消所有未完成任务再传播 `Cancelled`。`wait_for` 超时 → 取消并等取消完成 → 抛 `Timeout`。

## 添加新内容的落点

| 内容 | 位置 |
|---|---|
| 新同步原语（TaskGroup/shield/as_completed/…） | `include/weave/sync.hpp` + `src/core/sync.cpp` |
| 新通信原语（Queue/Lock/Semaphore/…） | 新公共头 `include/weave/<域>.hpp` + umbrella 加入 include |
| 新后端 | `src/backend/<name>.cpp`（宏守卫）+ `config.hpp` 加宏 + xmake 映射 |
| 新自检断言 | `tests/<域>_semantics.cpp`（每域一个 `WEAVE_TEST`，逐用例包 `run_case` + 末尾 `report`，见测试编写约定；新文件需同时加入 xmake 的 `weave_sc_smoke` 与 `weave_svdpi_smokelib` 两个 target） |
| 新示例 | `examples/<域>_demo.cpp`（`examples/*.cpp` glob 自动纳入 `weave_examples_sc`/`weave_examples_svdpi`，无需改 xmake.lua；示例不用 `run_case`，末尾一行 `printf`；回归 `xmake examples` 会双后端各跑一遍） |

原则：**凡能由"任务簿记 + Event + 三腿等待"组合出的原语进核心层；凡需要新挂起形态的才动 port 契约**。

公共头分层（依赖方向自上而下）：config → time → task/event → sync → test；umbrella `weave.hpp` 最后。每个公共头必须可独立编译（`#include` 自足）。核心内部状态只放 `src/core/*_internal.hpp`，不进公共头。

## 命名：与 asyncio 对齐

公共 API 的命名**以 asyncio 为准**：凡语义上对应 asyncio 概念的原语，名字必须与 asyncio 一致（`done()`/`cancelled()`/`cancel()`/`get_name()`、`create_task`、`sleep`、`Event::wait/set/clear/is_set`、`gather`、`wait_for`、`Cancelled`、`Timeout`……），包括新增原语（`TaskGroup`、`shield`、`Queue`、`Lock` 等）——引入前先查 asyncio 的对应名字，不自行发明。例外只限**无 asyncio 对应物**的项目自有概念（句柄有效性检查、时间类型 `duration`、测试注册 `WEAVE_TEST`、后端引导 `weave::sc::run()` 等），这类按 C++ 惯例命名。内部簿记（`detail::`、`g_*`、`*_internal.hpp`）不受此约束。

## 测试编写约定

- 自检同步必须**完全事件驱动**（`Event` 传递次序），禁止用 `sleep` 时长表达时序——delta-cycle 语义保证确定性，真实时钟在仿真里不可靠。
- 断言失败用 `throw std::logic_error("caseN: 描述")`；预期异常路径显式捕获并校验内容。
- 每个用例经 `tests/case_runner.hpp` 的 `run_case(__func__, "caseN", [&] { ... });` 包裹，
  正常返回打印 `[PASS] <测试>::caseN`，抛出则打印 `[FAIL] <测试>::caseN: <消息>` 后原样重抛
  （后端据此判 FAILED）；每个 `WEAVE_TEST` 末尾调 `report(__func__)` 打印通过/失败汇总。
  新增用例必须按此包裹，否则回归日志里看不到逐条结果。
- 断言体集中在 `tests/` 且与后端 main 入口分离成文件：同一断言体由 `weave_sc_smoke` 与
  `weave_svdpi_smoke`（经 `weave_svdpi_smokelib` 编成 `-sv_lib` 共享库）两个后端复用；
  所有注册的 `WEAVE_TEST` 按名字序串行运行（见 `src/core/registry.cpp`，任一失败即整体失败）；
  新用例按现有 case 风格追加。
- 利用不变量做同步："等待方从阻塞返回"本身保证"触发方已推进到阻塞点之后"（锁交接/进程调度屏障）。
- **任务 body 不得引用会先销毁的栈帧**：lambda 按引用捕获测试局部 `Event`/`Task` 等时，
  必须保证任务在该栈帧退出前已完成（结束前 `await`/`gather`，或异常路径先收尾兄弟任务）——
  否则子任务 resume 即 use-after-free（子任务可能在父任务失败后继续被调度，崩在 jemalloc/free）。
- **示例不编进任何 `*_smoke` 载荷**：`weave_sc_smoke` / `weave_svdpi_smokelib` 只含
  `tests/` 的断言体（两者除 `sc_main` 外同集，这是刻意保持的对称）。示例按用户视角单独
  编译与回归（`weave_examples` / `weave_examples_svdpi`，入口 `xmake examples`），仍须守住
  同两条约束：同步完全事件驱动、不得依赖只在某一后端成立的时序/触发语义——示例要在
  SystemC 可执行与 VCS 双库两种形态下都跑通。示例不需要 `run_case`，末尾 `printf` 一行即可。

## 语言标准：C++17

全仓库固定 **C++17**，唯一开关是 `xmake.lua` 的 `set_languages("c++17")`。

- **不得引入 C++20+ 特性**：concepts / ranges / coroutine / `std::format` / designated
  initializers / `std::string::starts_with` / `std::span` / `std::move_only_function`。
  需要它们时作为"当前限制"记下来（例：任务 body 不能捕获 `unique_ptr`，因为
  `std::function` 要求可拷贝），不要私下升标准。
- **升标准是跨后端改动**：会同时影响两个后端与 `-sv_lib` 边界，必须按上面的约定在两种
  `--backend` 配置下各跑一次 `xmake smoke`，并同步 README 的「语言标准」一节。
- **公共头必须能用 `-std=c++17` 独立编译**：与"每个公共头可独立编译"是同一约束的
  两个面；用到的头要显式 include（`<optional>`、`<string_view>`、`<type_traits>` 等），
  不得靠传递包含。
- **已在用的 C++17 惯用法，新代码沿用、不要退回旧写法**：`std::optional`
  （`Cancelled::msg()`、`wait` 的 timeout）、`std::string_view`
  （`detail::cancelled_with_msg`）、`[[nodiscard]]`（纯查询与资源获取，如
  `port::create_event`）、`[[maybe_unused]]`、`inline constexpr` 变量
  （`return_exceptions`）、结构化绑定、`std::exchange`、`std::make_unique` /
  `unique_ptr`（后端事件表）、`static_assert` + `std::is_invocable_v` /
  `std::is_trivially_copyable_v`、`std::filesystem`（仅测试驱动）。
- **几条边界不要越**（均已评估，越了会破坏既有不变量）：
  1. `string_view` 不能替代需要**所有权**的参数：`create_task(name)`、
     `TaskGroup::create_task`、`Task::cancel(msg)`、`register_test(name)`。
     `Cancelled::msg_` 是借用视图，指向 `TaskRecord::cancel_msg`，且 `what()` 依赖
     `std::string` 的 NUL 结尾。
  2. `std::optional` 不能用于 port 契约的**线格式哨兵**（`ev_id < 0`、
     `timeout_ticks < 0`、`abort_id < 0`）：那是与 SV DPI 的约定，改它要同时动
     `sv/weave_pkg.sv` 与两个后端。
  3. 模板没有按类型分支时不加 `if constexpr`（`create_task`/`gather` 只是转发）；
     `if-init` 同理——`record()`/`event_by_id` 的 `find` 之后都要继续用迭代器，
     写成 `if (init; cond) ... else` 反而更难读，没有自然落点就不硬套。
- **`Cancelled` 必须保持平凡可拷贝**（`task.hpp` 有 `static_assert` 守护）：它跨
  `-sv_lib` 边界抛出，不得加 `std::string` 之类的堆成员；带消息的构造只经
  `detail::cancelled_with_msg` 这一条路（`msg` 由 `TaskRecord` 所有）。
- **`constexpr` 也用作编译期守卫**：时间字面量越界（`10000_s`）是编译错误而非静默回绕，
  见 `include/weave/time.hpp` 的 `detail::fs_scaled`。

## 代码风格（提交前必做）

**C++ 遵循 `.clang-format`**（Google 基线，项目内覆盖项）：

- 缩进 2 空格，禁止 tab；列宽 100
- 指针/引用靠左：`Task* p`、`Task& t`（`DerivePointerAlignment: false`，始终以配置为准）
- 大括号 Attach；访问控制符与 `class` 关键字同列（`AccessModifierOffset: -2`）
- 短函数仅 inline 可单行；短 `if` 仅无 `else` 可单行；循环禁止单行
- include 大小写敏感排序（`SortIncludes: CaseSensitive`）

**SystemVerilog 遵循 `.verible-format`**：列宽 100、缩进 2、续行 4（`wrap_spaces=4`）、赋值语句 flush-left 对齐。

```bash
clang-format -i <改动的 .cpp/.hpp>                              # C++
verible-verilog-format --inplace --flagfile=.verible-format <改动的 .sv>
```

已知限制：`sv/weave_pkg.sv` 含 `->` 事件触发语句，超出 verible 解析器能力（报 syntax error），该文件按上述参数手工保持一致，不要用 verible 强行格式化。

约定：只格式化自己改动的文件，不做全仓库无关重排。

## 本环境的坑（已踩过）

- **已触发事件的 triggered 状态会滞留**：SystemC 的 `sc_event::triggered()` 按
  `change_stamp` 判等、SV 的 `triggered` 标志同槽持续——任务捕获 `Cancelled` 后在
  同一 abort_ev 上再次阻塞会被误判为取消命中。因此 `backend_wait` 注入取消后必须
  销毁并重建该任务的 abort_ev（见 `src/core/task.cpp`）。同理，组合原语使用裸 port
  事件时必须用 waiting 标志保证 trigger 必有等待者（无等待者的 trigger 会留下未消费
  的 delta notification，destroy 后后端侧是未定义行为）。
- xmake-repo 的 SystemC 按 **C 链接**找 `sc_main`：入口必须 `extern "C"`（见 `tests/sc_main.cpp`）。
- 此 xmake 版本：`add_requires` 只能在顶层作用域且不能跟在 option/target 定义之后（作用域泄漏）；`has_config(name, value)` 对 set_values 选项不可靠，用 `get_config(name) == "..."`。
- 时间换算用 `__int128` 防溢出；`ticks_per_sec()` 首次经 port 查询后缓存（static local）。
- **跨 DSO 的 catch 依赖 typeinfo 唯一**：libc++ 在 Linux/Android 上按 Itanium ABI 只比较
  `type_info` 指针。把公共异常类型链进**可执行文件**时，若用 `-fvisibility-inlines-hidden`
  编译（xmake-repo 的 systemc 包会给依赖目标带上），那份 RTTI 会被弱化甚至丢弃，
  `-rdynamic` 也导不出去 → 与 `libweave.so` 里那份变成两个对象，`catch` 静默失配、异常穿透到
  最外层（表现为整个测试 FAILED 且看不出哪个 catch 漏了）。修法：消费者
  `-fvisibility=default` + `-rdynamic`（`weave_examples_sc` 即如此）。`-sv_lib` 双库加载（VCS，
  如 `weave_examples_svdpi`/`weave_svdpi_smokelib`）由动态链接器统合，不受影响。
