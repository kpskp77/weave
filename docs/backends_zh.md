# Weave 后端（中文版）

后端就是端口契约的一种实现（见 [architecture](architecture_zh.md)）。本文面向需要切换、集成或
新增后端的人。

## 后端总览

| 宏 | 后端 | Bootstrap 入口 | 状态 |
|---|---|---|---|
| `WEAVE_BACKEND_SVDPI`（默认） | SystemVerilog DPI（VCS/IES/Xcelium） | `extern "C" void weave_start()`，由 sv/weave_pkg.sv 里的 `weave_run` 任务调用 | 已在真实 VCS 内核上验证（weave_smoke_svdpi_driver；无 VCS 时自动 SKIP） |
| `WEAVE_BACKEND_SYSTEMC` | SystemC（xmake-repo 3.0.2） | `int weave::sc::run()`，由 sc_main 调用 | 已在真实内核上验证（weave_smoke_sc） |

切换方式：

```bash
xmake f --backend=systemc && xmake   # 默认是 svdpi
```

手工构建：只需定义对应宏；未定义任何宏时，`include/weave/config.hpp` 默认选择 SVDPI。

## SVDPI 后端

DPI 边界（`src/backend/svdpi.cpp` + `sv/weave_pkg.sv`）：

- **import（由 SV 提供）**：`weave_sv_spawn`（通过 fork...join_none 创建进程）、
  `weave_sv_trigger`（一次性唤醒：触发事件并标记注册在该 id 上的等待者）、
  `weave_sv_create_event` / `destroy_event`（事件表）、`weave_sv_ticks_per_sec`、
  `weave_sv_set_root_done_ev`、`weave_sv_wait`（三腿等待，唯一的阻塞原语：在包的等待者注册表中
  注册，然后在等待者自己的唤醒事件上挂起 `if (w_state[wd] == 0) @(w_evt[wd])` —— 一个持久标志
  加一条每等待者边沿，因为对注册表数组做水平敏感的 `wait` 在 VCS 上是 O(N²) 开销（见
  `sv/weave_pkg.sv` 文件头）；超时腿由常驻定时器服务承担，其截止时间放在小顶堆里，使"找最早"与
  "清出已到期"各为 O(log N)，而不是 O(armed) 的两次扫描 —— 绝不用 `fork … join_any`，参见
  [../AGENTS.md](../AGENTS.md) 的"Pitfalls"一节）。
- **export（由 C++ 提供）**：`weave_task_main`（任务派发，SV 侧可经 weave_sv_spawn 到达）、
  `weave_start` / `weave_test_status`（tb bootstrap；包里的 `weave_run` 任务会调用它们，所以 DPI
  作用域已经是 `weave_pkg`，无需 `svSetScope`）。

手工运行方式（用于手动调试；`xmake examples` / `xmake smoke` 会自动执行同样的步骤）：

```bash
vcs -full64 -sverilog sv/weave_pkg.sv tests/weave_tb.sv \
    -sv_lib build/linux/x86_64/release/weave \
    -sv_lib build/linux/x86_64/release/weave_examples_svdpi
./simv   # 预期输出："WEAVE TEST PASSED"
```

自动化回归：`tests/svdpi_driver.cpp` 是唯一的 VCS 驱动，被编译进两个只有 payload 不同的目标：

| 目标 | `-sv_lib` payload | 内容 | 入口 |
|---|---|---|---|
| `weave_smoke_svdpi_driver` | `libweave_smoke_svdpi` | `tests/` 断言体 | `xmake smoke` |
| `weave_examples_svdpi_driver` | `libweave_examples_svdpi` | `examples/` 程序 | `xmake examples` |

两者都自行定位 vcs、编译 `sv/weave_pkg.sv` 与 `tests/weave_tb.sv`、运行 simv，并以输出标记
`WEAVE TEST PASSED` 判定通过；当找不到 vcs（`VCS_HOME`/`PATH`）时，它们打印 SKIP 并以 0 退出。

## SystemC 后端

`src/backend/systemc.cpp` 把端口映射到 SystemC 原语：

- `spawn` → 一个 `sc_spawn` 动态进程（子进程只在父进程挂起后才运行，与 fork...join_none 语义一致）
- 事件 → `sc_event`；三腿 `wait` → `sc_event_or_list` + 内核定时等待
- 时间 → 由 `sc_get_time_resolution` 决定 fs→tick 的换算

集成方式（测试侧提供 sc_main）—— 注意 xmake-repo 的 SystemC 查找的是**C 链接**的 `sc_main`：

```cpp
#include <weave/weave.hpp>
extern "C" int sc_main(int, char**);
extern "C" int sc_main(int, char**) { return weave::sc::run(); }
// run()：spawn 根任务与汇报进程 → 一直 sc_start() 到 sc_stop()
// → 把 root_status() 作为进程退出码返回；如果你的目标平台上 SystemC 使用 C++ 链接，去掉 extern "C"
```

**ABI 说明（跨 DSO 的异常类型）**：`CancelledError` / `TimeoutError` 由库抛出、由用户代码捕获。在
Linux/Android 上，libc++abi 遵循 Itanium ABI 且假定类型信息全局唯一（`type_info` 按指针比较），
因此两侧必须共享**同一个** typeinfo 对象。若在公共头里直接写内联定义，GCC/Clang 会视之为 vague
linkage：每个用到该类型的 TU 都会发出一个弱定义；又因为例如 xmake `mode.release` 默认给所有非
shared 目标设 `symbols = hidden`（`-fvisibility=hidden -fvisibility-inlines-hidden`），消费方
可执行文件里的那份副本就变成**局部符号** —— 与库里的副本不同，于是每个 `catch` 都会静默失效
（链接期、启动期都没有报错；异常只是直接飞到了最外层）。

修复点在于**类型定义本身**，而不在构建开关：两个类型的 `what()` 都**只在公共头中声明，并在库中
出线（out-of-line）定义**（`src/core/sync.cpp` / `src/core/task.cpp`）。一个出线的、非内联的虚函数
充当**关键函数（key function）**，于是 vtable 与 typeinfo 只会在那一个库 TU 中被发出；消费方 TU 只
保留未定义引用，由动态链接器在加载时解析到同一份副本。因此：

- 消费方**不需要** `-fvisibility=default`，也**不需要** `-rdynamic`（仓库里的 `weave_examples_sc`
  两者都没有，且在 `-fvisibility=hidden` 的 release 模式下依然工作）；
- 检查方式：`nm -D <exe> | grep _ZTIN5weave` 应全为 `U`（未定义），`D`/`V` 定义只出现在
  `libweave.so` 中；一旦把 `what()` 挪回内联定义，定义就会重新出现在这里，而 `catch` 会退化为
  不匹配；
- 代价：`CancelledError` 因此成为多态类型（带有虚析构，不再可平凡拷贝）；其不变量收缩为"借用视图、
  无堆成员、拷贝不抛"（`task.hpp` 中的 `static_assert` 正好守着这三点）。

通过 `-sv_lib` 加载 `weave` 与示例/测试库（VCS 场景）的机制相同：库中唯一的一份 RTTI 通过动态链接器
共享。

## 新增后端

1. 在 `include/weave/config.hpp` 中加入一个宏（包括互斥检查与默认逻辑）。
2. 创建 `src/backend/<name>.cpp`，整个文件用 `#if defined(WEAVE_BACKEND_<NAME>)` 保护，实现
   7 个 `weave::port` 原语，外加一个自选的 bootstrap 入口点（参考 svdpi.cpp / systemc.cpp）。
3. xmake 映射：给 `--backend` 选项加一个取值，在目标内比较 `get_config` 来定义宏与依赖。
4. 回归：`xmake smoke` 必须全绿（新增自检目标请命名为 `weave_smoke_*`，并加入 `smoke` 任务的构建
   列表）。
