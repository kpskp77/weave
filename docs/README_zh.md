# Weave（中文版）

[English](../README.md) | 简体中文

**C++ 测试逻辑与 EDA 仿真内核之间的行为编排层**。API 参照 Python asyncio 设计，但**不使用 C++20
协程**：阻塞语义完全由后端提供，每个任务都运行在一个仿真进程（SV process 或 SystemC SC_THREAD）
中，`sleep` / `wait` 通过后端的挂起点冻结整个 C++ 调用栈。

Weave 只负责并发编排（任务、时间、事件、组合子、超时、取消、状态查询），不提供 DUT 信号访问、总线
driver/monitor、UVM 组件、transaction/覆盖率、TLM 建模，也不包含仿真内核本身。这些能力由
SystemVerilog/UVM/SystemC 等既有工具提供，Weave 所做的是以统一接口把 C++ 测试行为接入其中。

## C++ ↔ asyncio 映射

Weave 的 API 语义**遵循 asyncio**，下表为完整对照；与 asyncio 的差异集中列在[已知偏差](#已知偏差)
一节。

| C++（weave） | Python（asyncio） | 说明 |
|---|---|---|
| `create_task(name, fn)` | `asyncio.create_task(coro)` | fn 是普通函数；只被调度，不会立即执行 |
| `task.await()` | `await task` | FAILED 时重抛其异常；CANCELLED 时抛 `CancelledError` |
| `task.cancel(msg)` | `task.cancel(msg)` | 协作式；msg 与 asyncio 3.9+ 一致，注入时与在 `await` 处重抛时都会保留 |
| `task.done()` / `task.cancelled()` | `task.done()` / `task.cancelled()` | |
| `task.get_name()` | `task.get_name()` | |
| `task.id()` | `id(task)` | 稳定、单调递增、不回收；无效句柄为 -1 |
| `current_task()` | `asyncio.current_task()` | 返回 `optional<Task>`；无任务上下文（harness 链、done 回调）为 `nullopt`；不挂起 |
| `sleep(d)` | `await asyncio.sleep(t)` | `sleep(0)` 让出零时间 |
| `now()` / `now_ticks()` | `loop.time()` | 绝对仿真时间；不挂起，单调不减 |
| `time_point` + `wait_until(tp)` | `loop.call_at` | `wait_until(now() + d)` ≡ `sleep(d)`；已到期的截止时间不挂起直接返回 |
| `Event` | `asyncio.Event` | set/clear 竞争按边沿语义处理；标志会保持（sticky） |
| `Pulse` | — | 纯边沿（asyncio 无对应物，故用 C++ 命名）：`fire()` 只唤醒此刻在场的等待者、不留标志；先 fire 后 wait **不会**被记住；`generation()` 供调用方核对是否漏了边沿 |
| `sv::EventHandle` | — | 跨语言事件句柄（asyncio 无对应物，故用 C++ 命名）：`sv::make_event()`/`sv::fire`/`sv::wait`/`sv::destroy` 直接作用于后端事件 id，SV 测试台与 C++ 层因此可以约定在同一个事件上；`sv::handed(tag)` 取回测试台用 `weave_sv_hand_off` 交过来的 id |
| `Every(d)` | — | 固定在仿真时间网格上的周期节拍（asyncio 无对应物；Python 的做法是每拍重建 `loop.call_later`，会累积漂移）：`wait()` 对准周期的整数倍，落后整整一个周期的消费者改为重新锚定而不是连发错过的节拍，`drift()` 记录这些跳过的次数 |
| `Lock` | `asyncio.Lock` | FIFO 交接；`LockGuard` ≡ `async with lock` |
| `Semaphore` | `asyncio.Semaphore` | 计数信号量；`SemaphoreGuard` ≡ `async with semaphore` |
| `BoundedSemaphore` | `asyncio.BoundedSemaphore` | 与 Semaphore 相同；超出上界释放会抛 `std::logic_error`（≡ `ValueError`）；`BoundedSemaphoreGuard` ≡ `async with bounded_semaphore` |
| `Barrier` | `asyncio.Barrier` | 固定参与方数量的会合点 |
| `Queue<T>` | `asyncio.Queue` | FIFO 队列；有界（`maxsize`）或无界；阻塞调用按 FIFO 排队 |
| `PriorityQueue<T>` / `LifoQueue<T>` | `asyncio.PriorityQueue` / `asyncio.LifoQueue` | API 与 `Queue` 相同；最小优先（小顶堆 ≡ heapq）/ 后进先出 |
| `gather(t1, t2, ...)` | `await asyncio.gather(*aws)` | 默认形式（首个失败即抛出） |
| `gather({t1, ...}, return_exceptions)` | `await asyncio.gather(*aws, return_exceptions=True)` | 返回 `vector<exception_ptr>`，不提前抛出 |
| `wait_for(t, d)` | `await asyncio.wait_for(aw, t)` | asyncio 3.12+ 超时语义；取消**等待者**同样会取消并等 `t` 落地 |
| `timeout(body, d)` | `async with asyncio.timeout(d)` | 给一段工作设定上界：body 作为子任务运行，超时先取消并等其落地再抛 `TimeoutError`；期限覆盖 body 的整体执行（没有协程块可以用来"上弦"） |
| `wait({t1, ...}, return_when, timeout)` | `await asyncio.wait(fs, ...)` | 返回 `{done, pending}`；不取消子任务 |
| `shield(t)` | `asyncio.shield(aw)` | 返回代理任务；取消/超时止步于代理 |
| `as_completed({t1, ...})` | `asyncio.as_completed(fs)` | `next()` 阻塞并按完成顺序产出 |
| `TaskGroup` + `tg.create_task` / `tg.wait()` | `asyncio.TaskGroup` | 结构化并发；首个失败会取消兄弟任务 |
| `CancelledError` | `asyncio.CancelledError` | 取消信号；**不**派生自 `std::exception`，详见 [architecture](architecture_zh.md) |
| `DeadlockError` | — | asyncio 无对应物（Python 对 join 自身抛 `RuntimeError`）：可诊断的等待环——自 await、`TaskGroup` 子任务等待自己的组。派生自 `std::logic_error` |
| `TimeoutError` | `TimeoutError` | 超时信号；派生自 `std::exception` |
| `BrokenBarrierError` | `asyncio.BrokenBarrierError` | 屏障被破坏/重置信号；派生自 `std::runtime_error`（≡ `RuntimeError`） |
| `QueueEmpty` / `QueueFull` / `QueueShutDown` | `asyncio.QueueEmpty` / `asyncio.QueueFull` / `asyncio.QueueShutDown` | 由 `*_nowait` 形式抛出；派生自 `std::runtime_error`（≡ `RuntimeError`） |
| `WEAVE_TEST` + 注册表 | pytest / 各类 runner | asyncio 无对应物；由后端 bootstrap 入口点驱动 |

### 模型差异（相对 Python，务必先读）

除映射表之外还有三处结构性差异，绝大多数误用都源于此：

- **没有协程，也没有 await**：组合子是普通的阻塞调用，不是 awaitable，并且需要在任务上下文中调用
  （在任务外调用会抛 `std::logic_error`）。所有组合子都不返回 future：默认的 `gather(...)` 会阻塞到
  任务结束或出现首个失败/取消，并且不返回值。
- **没有事件循环对象**：调度由仿真内核负责。`create_task` 创建的任务只有在**父任务到达下一个真正的
  挂起点（或结束）之后**才开始运行，与 asyncio `create_task` 的时序一致。
- **取消永不撕裂调用栈**：`cancel()` 只置位标志并唤醒任务，`CancelledError` 在阻塞点注入，因此栈通过
  普通异常传播展开，RAII 总会执行；杀掉仿真进程（`process::kill()`、SV `disable`）是被禁止的，因此
  不会有任何东西被异步拆除。

### 已知偏差

- `TaskGroup` 没有 `ExceptionGroup`：`wait()` 直接重抛首个失败，即仍在等待时按完成顺序取第一个
  FAILED（在调用 `wait()` 之前就已结束的子任务则按创建顺序回放）；
- `gather` 提前返回后，其余任务后续的失败不会报告到任何地方：仍可通过任务句柄取回，并且完全不存在
  "异常从未被取出"这类诊断（asyncio 在这里同样抑制该警告，因为 `gather` 的 `_done_callback` 会把
  遗留任务标记为已取出；但对无人 await 的任务，asyncio 是会告警的）；
- `Barrier` 没有 `async with` 形式：守卫无法把 `wait()` 的索引交还给函数体，而 Python 的
  `async with barrier` 可以（它返回 `wait()` 的索引），所以请显式调用 `wait()`；
  `abort()` / `reset()` 是普通的非挂起成员函数，尽管 asyncio 把它们写作 `async def`。

## 快速开始

```cpp
#include <weave/weave.hpp>
using namespace weave;
using namespace weave::literals;

WEAVE_TEST(my_test) {
    Event ev;
    auto t1 = create_task("producer", [&] { sleep(100_ns); ev.set(); });
    auto t2 = create_task("consumer", [&] { ev.wait(); /* ... */ });
    wait_for(t1, 1_us);          // 超时则取消该任务并抛 weave::TimeoutError
    gather(t1, t2);
}
```

### 诊断挂死

死锁永远不会超时，所以最后手段是 `weave::debug`：`debug::snapshot()` 返回任务等待图——每个存活任务
阻塞在哪个后端事件上、已阻塞多久（**仿真**时间）——`debug::dump(tag)` 把它逐行打到 stderr。两者都不
挂起、也不改状态，因此可以从 done 回调或 harness 链里调用。快照能描述挂死，但**不能**证明存在环
（任何人都可以 `set()` 一个 `Event`，"谁该唤醒它"没有任何地方记录）。

`debug::watchdog(idle, abort_on_fire = false)` 不等人来问，自己盯着：若仿真时间过去 `idle` 而**没有任何**
任务被唤醒，它就打印快照；带 `abort_on_fire` 时还会取消除 root 之外的所有存活任务，让卡住的 await 链以
`CancelledError` 收场——绝不杀进程。它本身是个任务，对返回的句柄 `cancel()` 即停止。两个盲区是本质的：
零时延 livelock 不推进时钟，它永远醒不来；单个长于 `idle` 的 `sleep()` 看起来和挂死一模一样——所以默认
行为是报告而不是 abort。

## 构建与运行

```bash
just smoke      # 语义自检：构建 smoke payload，并通过 xmake run 逐个运行 weave_smoke_*（每个后端各跑一次）
just examples   # 示例回归：一次跑通 systemc 与 svdpi 两个后端
```

两者职责不同，且缺一不可：`tests/` 中的断言体与 `examples/` 中的程序永远不会被编译进对方的
payload；示例完全按用户的方式构建（只用公共头、链接 `libweave.so`，真正跨越 DSO 边界）。产物位于
`build/<plat>/<arch>/<mode>/`；没有 VCS/IES 时，SVDPI 驱动会打印 SKIP 并返回 0（等价于只做编译
检查）。后端切换、SystemC bootstrap 入口点以及新增后端的方法，见 [backends](backends_zh.md)。

## 语言标准：C++17 及以上

整个仓库以 **C++17 作为最低标准**（`xmake.lua` 中的 `set_languages("c++17")` 是唯一的开关）：库本身
以 C++17 构建，其公共能力在 C++17 下必须完整。**同时支持更新的标准**：调用方可以为自己的翻译单元选择
`-std=c++20` / `c++23` / `c++2c`，公共头已验证可在 C++17/20/23 下干净编译。

- **C++17 即已完整**：没有任何公共头把 C++20+ 的特性当作必需（`CancelledError::msg()` 是
  `std::optional<std::string_view>`，`wait` 的超时参数是 `std::optional<duration>`）。
- **更新的标准同样可用**：公共头避免了语义随标准版本变化、或已被弃用/移除的构造，因此升级
  `-std=` 不会引入警告，也不会改变行为。
- **不用 C++20 协程是设计决策，而非受标准限制**：挂起由仿真内核负责，与标准版本无关。

## 目录结构

```
include/weave/    公共 API：weave.hpp（伞头）+
                  config/time/task/event/locks/semaphore/barrier/sync/test[/sc]
src/core/         与后端无关的核心 + port.hpp 后端契约
src/backend/      后端适配器（恰好一个，由宏选择）：svdpi.cpp（默认）/ systemc.cpp
sv/               SVDPI 后端协议的 SV 侧（事件表、三路 wait、spawn）
examples/         按领域划分的公共 API 演示 + 一个端到端用例（just examples 回归）
tests/            语义自检断言体 + VCS 运行脚本及其 SV 测试顶层（just smoke 回归）
docs/             architecture.md / backends.md 及其中文版 README_zh.md / architecture_zh.md / backends_zh.md
```

## 延伸阅读

- [architecture](architecture_zh.md)：设计决策与实现约束（为什么不用协程、取消、边沿语义、时间）
- [backends](backends_zh.md)：DPI 协议、SystemC 集成、ABI 说明、新增后端指南
