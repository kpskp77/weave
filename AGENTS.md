# AGENTS.md

面向在本仓库工作的 AI 编码助手：构建/验证命令，以及改动时必须守住的不变量。项目定位见 [README.md](README.md)（含 C++↔asyncio 映射），架构理由见 [docs/architecture.md](docs/architecture.md)，后端接入见 [docs/backends.md](docs/backends.md)。

## 项目一句话

Weave 是 C++ 测试逻辑与 EDA 仿真内核之间的行为编排层：API 借鉴 Python asyncio，但**不用 C++20 coroutine**——阻塞语义由后端仿真进程承载（SV 进程 / SystemC SC_THREAD 挂起时整个 C++ 调用栈冻结）。

## 构建与验证（改动后必做）

**原则：每次改动都回归所有用例，不挑跑、不省略。**

```bash
xmake smoke      # 语义自检：构建 smoke 载荷 + 逐个 xmake run 运行 weave_smoke_* 自检二进制
xmake examples   # 示例回归（独立）：一次跑完 systemc + svdpi 两种后端
```

- 跨后端的改动（`port.hpp`、后端文件、`config.hpp`、xmake 宏映射）必须在两种配置下各跑一次：

```bash
xmake f --backend=svdpi   && xmake smoke
xmake f --backend=systemc && xmake smoke
xmake f --backend=svdpi   # 还原默认配置
```

- 自检二进制按 `weave_smoke_*` 命名才会被 smoke 任务逐个运行；新增自检目标还需把它加进 `smoke` 任务
  的 `xmake build` 列表（运行统一走 `xmake run`，见下方「本环境的坑」）。
- `tests/` 与 `examples/` 载荷严格分离：示例只经 `add_deps("weave")` 链接 `libweave.so`（不内联核心
  源码），不编进任何 `weave_smoke_*` 载荷；反向亦然。改公共头或示例只需跑一次 `xmake examples`。
- 无 VCS/IES 时 SVDPI 驱动打印 SKIP 并返回 0（等价于只做编译验证），跳过 SVDPI 侧即可。
- 首次构建自动安装 xmake-repo 的 SystemC；自检跑在真实内核上，delta-cycle 语义保证结论确定性。

## 架构：核心/后端分离

```
include/weave/    公共 API：weave.hpp(umbrella) + config/time/task/event/sync/test[/sc]
src/core/         后端无关：task/event/sync/time/registry + port.hpp(契约) + *_internal.hpp
src/backend/      svdpi.cpp(默认) / systemc.cpp，整文件由 WEAVE_BACKEND_* 宏守卫
```

核心簿记**只允许依赖 `port.hpp` 的 7 个原语**与内部头，不得直接引用 DPI/SystemC/线程 API。后端选择的
唯一开关是编译期宏（`config.hpp`，互斥，默认 SVDPI），`--backend=` 只是定义宏。新增后端 = 一个宏守卫的
`src/backend/*.cpp` 实现 port 契约 + 引导入口，**核心代码零改动**。

port 契约的四条保证见 `port.hpp` 头注释（摘要见 architecture.md）：spawn 是"调度而非立即执行"、任一时刻
至多一个进程执行 C++ 簿记、事件一次性且 trigger 不作用于已 destroy 的 id、`wait` 是唯一挂起点（三腿竞速，
取消腿优先，负 id 禁用该腿）。**只有需要新的挂起形态时才动契约。**

## 不变量（改 `src/core/` 时逐条自查）

- **唯一阻塞点**：一切挂起都经 `detail::backend_wait()` → `port::wait`，别处不得引入挂起。
- **协作式取消**：`cancel(msg)` 只置 `cancel_armed` + 记录 msg + 触发私有 abort 事件，`Cancelled(msg)` 在
  阻塞点注入；body 吞掉 `Cancelled` 后正常返回 → 任务 DONE。禁止强杀进程（破坏 RAII 展开）；级联取消
  兄弟传空 msg，注入点与 `await` 重投处保留原 msg。
- **异常类型分层不可改**：`Cancelled` **不派生** `std::exception`（对齐 `BaseException`，不被兜底 catch
  静默吞掉），`Timeout` **派生**（对齐 `TimeoutError`）。
- **两个异常类型的 `what()` 必须外联定义在库里**（`src/core/sync.cpp` / `src/core/task.cpp`）：它是
  两个类型的 key function，vtable/typeinfo 因此只在 libweave.so 生成一份，消费方 TU 只留未定义引用。
  一旦写回内联，typeinfo 退化成每 TU 一份的 weak 定义，libc++abi 按指针比对 → 跨 DSO 的 catch
  静默失配（编译、链接都不报错）。因此 `Cancelled` 是多态类型且带虚析构（不再是 trivially
  copyable，断言改为 nothrow 拷贝 + 无堆成员）。
- **Event edge 语义**：`set()` 快照当前等待者并逐个触发，放行后不复查 flag；`wait()` 成功与取消**两条
  路径**都必须先从 `wake_evs` 摘除再销毁唤醒事件，否则后续 `set()` 会对已销毁 id 触发。该清场现由单个
  `detail::scope_exit` 守卫承担（摘除与销毁的顺序写在 lambda 体内，不依赖析构逆序）。
- **`detail::on_scope_exit` 用在「帧死后仍有读者」的状态上**：`Event::wait()`（`wake_evs` 挂在共享
  `Impl` 上）、`AsCompleted::next()`（`waiting` 挂在跨调用的 `*impl_` 上）、`wait()`/`TaskGroup::wait()`
  （`WaitState`/`GroupState` 虽局部，但被未完成子任务的 done_callback 按 `shared_ptr` 持有，帧死后
  仍活着，回调仍会读 `waiting`/`signalled`）。
- **标志复位与事件销毁的耦合看取消是否逸出**：`wait()`（asyncio.wait）设计上把 `Cancelled` 原样往上传
  （不取消子任务），逸出时子任务仍 pending、`st` 仍被其回调持有，所以「先落 `waiting` 再 `destroy_event`」
  必须成对写在同一个守卫里——拆成「守卫 + 直线代码」，直线那句在取消路径上执行不到，子任务晚到完成
  就会对已销毁 id `trigger`（SystemC 侧抛 `invalid event id`）。`TaskGroup::wait()` 不同：`Cancelled`
  就地捕获并 drain 完兄弟才外抛，标志复位留在 `try` 之后就够，守卫只需销毁事件。
- 守卫的析构不抛、不挂起；需要挂起的收尾（TaskGroup 级联取消后等落地）永远留在显式代码里。
- **时间**：内部 int64 飞秒，`to_ticks` 向上取整保证至少睡够，**非正时长一律夹到 0 tick**
  （`sleep(0)` 保持 0 tick；负 tick 在线格式里是"无超时腿"，漏下去就是永久阻塞）。飞秒上限 ≈ 2.56 h
  仿真时间，越界字面量是**编译错误**（`time.hpp` 的 constexpr 守卫），运行期算术（`+ - * /`）
  越界抛 `std::overflow_error`、除零抛 `std::domain_error`，两边都**不静默回绕**。
- **失败传播**：`gather` 首个失败按**完成顺序**立即重抛、兄弟不取消（取消兄弟是 TaskGroup 语义）；gather
  自身被取消则先级联取消未完成任务；`wait_for` 超时 → 取消并等取消完成 → 抛 `Timeout`。

## 添加新内容的落点

| 内容 | 位置 |
|---|---|
| 新同步原语（TaskGroup/shield/as_completed/…） | `include/weave/sync.hpp` + `src/core/sync.cpp` |
| 新通信原语（Queue/Lock/Semaphore/…） | 新公共头 `include/weave/<域>.hpp` + umbrella 加入 include |
| 新后端 | `src/backend/<name>.cpp`（宏守卫）+ `config.hpp` 加宏 + xmake 映射 |
| 新自检断言 | `tests/<域>_semantics.cpp`，同时加入 `weave_smoke_sc` 与 `weave_smoke_svdpi` |
| 新示例 | `examples/<域>_demo.cpp`（glob 自动纳入，无需改 xmake.lua） |

原则：**能由"任务簿记 + Event + 三腿等待"组合出的原语进核心层；需新挂起形态的才动 port 契约。**
公共头分层（依赖方向自上而下）：config → time → task/event → sync → test，umbrella `weave.hpp` 最后；
每个公共头必须可独立编译。核心内部状态只放 `src/core/*_internal.hpp`，不进公共头。

## 命名与语言基线

- **命名以 asyncio 为准**：凡有 asyncio 对应物的原语必须同名（`done`/`cancelled`/`cancel`/`get_name`、
  `create_task`、`sleep`、`Event::wait/set/clear/is_set`、`gather`、`wait_for`、`TaskGroup`、`shield`、
  `as_completed`、`Cancelled`、`Timeout`），新增原语也不例外——先查 asyncio 再动手，不自行发明。无对应物
  的项目自有概念（`duration`、`WEAVE_TEST`、`weave::sc::run()`）按 C++ 惯例；`detail::`/`g_*` 不受约束。
- **C++17 是下限而非上限**：库自身按 C++17 构建（`set_languages("c++17")` 是唯一开关），公开能力
  必须在 C++17 下功能完备——不得把 C++20+ 特性当必需；同时公共头必须能在更新的标准（C++20/23）下
  干净编译（不依赖随标准改变含义/被弃用/被移除的写法，`-Wall -Wextra -Werror` 下无告警），调用方可
  自选 `-std=`。公共头必须能独立编译（用到的头显式 include，不靠传递包含）。已评估的边界（`string_view`
  不能替代需所有权的参数、port 线格式哨兵仍用负值、无类型分支不加 `if constexpr`、`Cancelled` 的
  `what()` 必须外联以保 typeinfo 单库唯一）见 architecture.md 的「语言基线」。

## 测试编写约定

- 同步必须**完全事件驱动**（`Event` 传递次序），禁止用 `sleep` 时长表达时序。
- 每个用例包 `run_case(__func__, "caseN", [&] { ... })`，失败 `throw std::logic_error("caseN: 描述")`；
  每个 `WEAVE_TEST` 末尾 `report(__func__)`。不包裹则回归日志看不到逐条结果。
- 利用不变量做同步："等待方从阻塞返回"本身保证触发方已推进到阻塞点之后（锁交接/进程调度屏障）。
- 断言体集中在 `tests/` 且与后端 main 入口分离，由两个 smoke 载荷共用（除 `sc_main.cpp` 外同集，
  这是刻意保持的对称）；注册的 `WEAVE_TEST` 按名字序串行运行，任一失败即整体失败（`registry.cpp`）。
- **任务 body 不得引用会先销毁的栈帧**：lambda 按引用捕获局部 `Event`/`Task` 时，必须保证任务在该栈帧
  退出前完成（`await`/`gather`，或异常路径先收尾兄弟），否则 resume 即 use-after-free。
- 示例不用 `run_case`，末尾一行 `printf`；同样要事件驱动，且不得依赖只在某一后端成立的时序——它要在
  SystemC 可执行与 VCS 双库两种形态下都跑通。

## 代码风格（提交前必做）

```bash
clang-format -i <改动的 .cpp/.hpp>                                        # Google 基线，见 .clang-format
verible-verilog-format --inplace --flagfile=.verible-format <改动的 .sv>  # 见 .verible-format
stylua <改动的 .xmake>  # 见 .stylua.toml
```

- 只格式化自己改动的文件，不做全仓库无关重排；具体参数以两个配置文件为准（2 空格缩进、列宽 100 等）。
- `sv/weave_pkg.sv` 含 `->` 事件触发语句，超出 verible 解析器能力（报 syntax error）：该文件按配置参数
  手工保持一致，不要用 verible 强行格式化。

## 本环境的坑（已踩过）

- **已触发事件的 triggered 状态会滞留**（SystemC 按 `change_stamp`、SV 按标志持续）：任务捕获 `Cancelled`
  后在同一 abort_ev 上再次阻塞会被误判为取消命中，故 `backend_wait` 注入取消后必须销毁并重建
  `abort_ev`。同理，组合原语使用裸 port 事件时必须用 waiting 标志保证 trigger 必有等待者（无等待者的
  trigger 会留下未消费的 delta notification，destroy 后后端侧是未定义行为）。
- xmake-repo 的 SystemC 按 **C 链接**找 `sc_main`：入口必须 `extern "C"`（见 `tests/sc_main.cpp`）。
- **SystemC 自检必须经 `xmake run` 跑**：`add_packages(systemc)` 不会产生 rpath（xmake 只为 shared 目标
  依赖自动补 `$ORIGIN`），运行期靠 `xmake run` 注入包带的 `LD_LIBRARY_PATH`；`os.execv(targetfile)`
  直接执行会报 `library "libsystemc.so.3.0" not found`。故 smoke / examples 任务统一走 `xmake run <target>`。
- 此 xmake 版本：`add_requires` 只能在顶层作用域且不能跟在 option/target 定义之后（作用域泄漏）；
  `has_config(name, value)` 对 set_values 选项不可靠，用 `get_config(name) == "..."`。
- 时间换算用 `__int128` 防溢出；`ticks_per_sec()` 首次经 port 查询后缓存（static local）。
- **跨 DSO 的 catch 依赖 typeinfo 唯一**：已由「异常类型的 `what()` 外联定义」保证（见不变量一节），
  消费方可执行文件**不需要**任何 `-fvisibility=default` / `-rdynamic`；即便 xmake `mode.release` 对非
  shared 目标默认设了 `symbols=hidden`（`-fvisibility=hidden -fvisibility-inlines-hidden`，与 package 无关）
  也照样能 catch。判别：`nm -D <exe> | grep _ZTIN5weave` 应全是 `U`，定义只在 libweave.so；
  反之（exe 里出现 `D`/`V` 定义）说明 key function 被写回内联了。详见
  [docs/backends.md](docs/backends.md) 的「ABI 提示」。
