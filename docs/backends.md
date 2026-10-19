# Weave 后端

后端是 port 契约（见 [architecture.md](architecture.md)）的实现。本文面向需要切换、集成或新增后端的人。

## 后端一览

| 宏 | 后端 | 引导入口 | 状态 |
|---|---|---|---|
| `WEAVE_BACKEND_SVDPI`（默认） | SystemVerilog DPI（VCS/IES/Xcelium） | `extern "C" void weave_run()`，由 sv/weave_tb.sv 调用 | 真实 VCS 内核运行验证（weave_svdpi_smoke，无 VCS 自动 SKIP） |
| `WEAVE_BACKEND_SYSTEMC` | SystemC（xmake-repo 3.0.2） | `int weave::sc::run()`，在 sc_main 中调用 | 真实内核运行验证（weave_sc_smoke） |

切换：

```bash
xmake f --backend=systemc && xmake   # 默认 svdpi
```

手写构建：编译时定义对应宏即可；一个宏都不定义则 `include/weave/config.hpp` 默认 SVDPI。

## SVDPI 后端

DPI 边界（`src/backend/svdpi.cpp` + `sv/weave_pkg.sv`）：

- **import（SV 提供）**：`weave_sv_spawn`（fork...join_none 创建进程）、`weave_sv_trigger`、`weave_sv_create_event` / `destroy_event`（事件表）、`weave_sv_ticks_per_sec`、`weave_sv_set_root_done_ev`、`weave_sv_wait`（三腿等待，唯一阻塞原语）。
- **export（C++ 提供）**：`weave_task_main`（任务分派，SV 经 weave_sv_spawn 间接触发）、`weave_run` / `weave_test_status`（tb 引导）。

运行示意（手工调试用；`xmake examples` / `xmake smoke` 会自动完成同样的步骤）：

```bash
vcs -full64 -sverilog sv/weave_pkg.sv sv/weave_tb.sv \
    -sv_lib build/linux/x86_64/release/weave \
    -sv_lib build/linux/x86_64/release/weave_examples_svdpi
./simv   # 期望输出 "WEAVE TEST PASSED"
```

自动化回归：`tests/svdpi_driver.cpp` 是唯一的 VCS 驱动，编成两个目标、只差载荷：

| 目标 | `-sv_lib` 载荷 | 内容 | 入口 |
|---|---|---|---|
| `weave_svdpi_smoke` | `libweave_svdpi_smokelib` | `tests/` 断言体 | `xmake smoke` |
| `weave_examples_svdpi_driver` | `libweave_examples_svdpi` | `examples/` 示例 | `xmake examples` |

两者都自行定位 vcs、编译上述骨架、运行 simv 并以输出标记 `WEAVE TEST PASSED` 判 Pass；
找不到 vcs（`VCS_HOME`/`PATH`）时打印 SKIP 并以 0 退出。

## SystemC 后端

`src/backend/systemc.cpp` 把 port 映射到 SystemC 原语：

- `spawn` → `sc_spawn` 动态进程（父进程挂起后子进程才运行，与 fork...join_none 语义一致）
- 事件 → `sc_event`；`wait` 三腿竞速 → `sc_event_or_list` + 内核定时等待
- 时间 → `sc_get_time_resolution` 决定 fs→tick 换算

集成（测试侧提供 sc_main）——注意 xmake-repo 的 SystemC 按 **C 链接**寻找 `sc_main`：

```cpp
#include <weave/weave.hpp>
extern "C" int sc_main(int, char**);
extern "C" int sc_main(int, char**) { return weave::sc::run(); }
// run()：spawn 根任务与汇报进程 → sc_start() 至 sc_stop()
// → 返回 root_status() 作进程退出码；若目标平台的 SystemC 按 C++ 链接，去掉 extern "C"
```

**ABI 提示（可执行文件链接 libweave 时）**：`Cancelled` / `Timeout` 是公共头里定义、
由库抛出、由用户代码 catch 的类型。libc++ 在 Linux/Android 上按 Itanium ABI 假设类型信息
全局唯一（`type_info` 只比较指针），因此两侧必须绑定同一份 typeinfo：

- 用 `-fvisibility=default` 编译（xmake-repo 的 systemc 包会给依赖目标带
  `-fvisibility=hidden` 与 `-fvisibility-inlines-hidden`，把公共异常类型的 RTTI 弱化甚至丢弃，
  使 `-rdynamic` 也导不出去，实测 catch 全部失配）；
- 可执行文件链接时加 `-rdynamic`（= `-Wl,--export-dynamic`），让库内 throw 处的引用
  经动态符号解析到可执行文件导出的那份 RTTI。

用 `-sv_lib` 加载 `weave` 与示例/测试库（VCS 场景）时，由动态链接器在加载期统合两份
RTTI，无需上述设置。仓库内的 `weave_examples_sc` 目标（`xmake examples`，仅 --backend=systemc）
就是按这套消费者设置构建的；`weave_examples_svdpi`（共享库形态）则不需要。

## 新增后端

1. 新增 `include/weave/config.hpp` 宏（加入互斥校验与默认逻辑）。
2. 新建 `src/backend/<name>.cpp`，整文件 `#if defined(WEAVE_BACKEND_<NAME>)` 守卫，实现 `weave::port` 的 7 个原语 + 自选引导入口（参考 svdpi.cpp / systemc.cpp）。
3. xmake 映射：`--backend` 选项加值，目标内 `get_config` 比较定义宏与依赖。
4. 回归：`xmake smoke` 必须全绿（新增自检目标命名为 `*_smoke` 即自动纳入）。
