# Weave 架构（中文版）

面向需要理解或修改 Weave 实现的人。可操作的不变量清单在 [AGENTS.md](../AGENTS.md)；后端集成部分见
[backends](backends_zh.md)。

## 核心/后端分离

Weave 分为两层，中间是一份稳定的端口契约：

```
C++ 测试逻辑 (WEAVE_TEST)
        │  公共 API (include/weave/)
        ▼
核心簿记 (src/core/) ── task/event/sync/time/registry + locks/queue/semaphore/barrier
        │  唯一依赖：port.hpp（6 个原语 + weave_task_main 派发入口）
        ▼
后端适配器 (src/backend/) ── svdpi.cpp（默认）/ systemc.cpp
        │
        ▼
仿真内核（VCS/IES 自动驱动，SystemC 本地运行；另一个 DPI 仿真器只需满足同一契约
                    —— Xcelium 尚未验证）
```

- 核心层**只能**依赖 `src/core/port.hpp` 中声明的原语，绝不能直接引用 DPI / SystemC / 线程 API。
- 新增后端 = 实现端口契约 + 提供一个 bootstrap 入口点，核心代码零改动。
- 后端选择的唯一开关是编译期宏（`include/weave/config.hpp`，互斥，默认 SVDPI）；xmake 的
  `--backend=` 只是选择该宏（并因此决定链接两个包中的哪一个）。

## 端口契约

`src/core/port.hpp` 的文件头注释是契约的完整描述；四条保证摘要如下：

1. **进程模型**：`spawn(id)` 创建一个仿真进程来执行 `weave_task_main(id)`；子进程只有在父进程到达
   下一个挂起点（或结束）之后才运行 —— ≡ asyncio 的 `create_task`"被调度，但不立即执行"。
2. **单一活跃进程**：任意时刻最多只有一个进程在执行 C++ 簿记（核心依赖这一点来维护全局状态，因此
   `g_current` 之类的变量无需加锁）。
3. **一次性事件**：`create/trigger/destroy`；`trigger` 绝不能指向一个已经被销毁的 id。
4. **单一挂起点** `wait(ev, timeout, abort, status)`：三腿竞争（事件/超时/取消），同一时隙冲突时
   **取消腿优先**。`ev_id < 0` 去掉事件腿，`timeout_ticks < 0` 去掉超时腿，但 **`abort_id` 永远是
   一个有效 id**：SystemC 适配器会无条件解析它（`event_by_id(abort_id)`），只有 SV 侧恰好能容忍
   它缺失。

## 关键设计决策

### 为什么不用 C++20 协程

Weave 的用户代码运行在仿真器进程内部，其挂起/恢复由仿真内核调度（当 SV process 或 SystemC
SC_THREAD 挂起时，整个 C++ 调用栈被冻结）。用协程会迫使我们自己造一个调度器，然后那个调度器必然与
仿真时间/事件模型相互冲突。一个由后端进程承载挂起的阻塞式 API，才是仿真内核最自然的搭配。

### 为什么取消是协作式的

`cancel()` 只置一个标志并唤醒任务，在阻塞点注入 `CancelledError` 异常。对*任务*进程使用 SV
`disable` / `process::kill()` 是被禁止的：在任意点杀掉进程会撕裂 C++ 调用栈，RAII 不会展开，资源
泄漏，簿记状态被破坏。（后端中唯一的一处 `disable fork` —— 属于定时器服务，见 `sv/weave_pkg.sv`
—— 只会 disable 它自己的服务局部腿，永远触及不到其他等待者的栈帧；这是被记录在案的唯一例外，而不是
一个漏洞。）这与 asyncio 的协作式取消同构（在 await 点抛出 `CancelledError`，可捕获、可拒绝）。
`cancel(msg)` 与 asyncio 3.9+ 的 `Task.cancel(msg)` 一致：msg 保存在任务簿记中，无论是阻塞点注入的
`CancelledError`，还是之后 `await`/`gather` 重抛之处，都会保留同一个 msg；当组合子级联取消兄弟任务
时，它传递的是空 msg（msg 属于发起取消者的上下文）。

取消信号的类型层次同样遵循 asyncio：`CancelledError` **刻意不派生自 `std::exception`**（对应
Python 3.8+ 中 `CancelledError` 是 `BaseException`），这样惯用的 `catch (const std::exception&)`
兜底就不会悄悄吞掉取消 —— 吞掉会让任务错误地以 DONE 结束，破坏 gather / TaskGroup / wait_for 对
CANCELLED 终态的判定；相反，`TimeoutError` **确实派生自 `std::exception`**（对应 asyncio 的
`TimeoutError`，一个普通的 `Exception`），因为超时是一条正常失败路径，通用日志/兜底代码应当能够处理。

### 为什么 Event 是边沿触发而非水平触发

`set()` 会对当前等待者做快照并逐个触发其一次性唤醒事件；被快照到即意味着你被释放，即便紧接着就发生
了 `clear()`。标志在**进入时**依然会被检查（`wait()` 在一个已置位的事件上会直接返回而不挂起）；纯
边沿的只是入队*之后*的判定。被否决的替代方案 —— 记住 `set()` 发生过，等等待者恢复后再检查一次标志
—— 会在 set/clear 落在同一个调度步内时丢失唤醒：等待者恢复、读到 `false`、然后再次阻塞。asyncio.Event
的处理方式相同（它在 `set()` 时设置当时位于 `_waiters` 中那些 future 的结果），所以快照本身是借来的
而不是发明的；weave 特有的部分是这个选择的代价。

这个代价无法回避：每个等待者阻塞在自己的、一次性后端事件 id 上，因此在**两条返回路径**（成功与取消）
上都必须先解链、再销毁该 id，否则后续的 `set()` 会去触发一个已销毁的 id。这个顺序就是那条不变量（由
一个 `detail::scope_exit` 承载，顺序写在 lambda 体内），其针对每个代码点的推理保留在
`src/core/event.cpp` 的文件头注释里。

### 为什么 Lock 在 release() 内部完成所有权交接

`release()` 在**同一个非挂起调用**内把所有权交给队首等待者（FIFO），期间 `held` 始终为 true：不存
在"已释放但尚未接管"的窗口，`locked()` 在交接间隙也绝不会短暂变为 false。asyncio 走的是另一条路，
因为它的交接要等被唤醒的等待者重新被调度后才完成：`release()` 清除 `_locked` 并唤醒队首（这两条语句
的顺序在不同 CPython 版本间不同 —— 3.6 先降标志，3.12+ 先唤醒 —— 所以两种顺序都不应被依赖），而
等待者只有在恢复后才重新举起标志 —— 于是尽管所有权已在飞行途中，`locked()` 会在一段时间内报告
`false`（新来者仍无法插队，因为尚未移除的 future 使其快速通道保持关闭）。同步交接关闭了这个窗口，
而不是把它写成文档；又因为每个排队等待者都持有一个一次性 `Event` 作为交接信号，所以锁由 `Event` 加
单一阻塞点构成，后端零改动。

取消不能让锁消失，这一点在形态上确实与 asyncio 一致：如果取消与交接落在同一个 delta 内
（`release()` 刚把锁交给某个等待者，该等待者立即被 `cancel()`；后端的取消腿获胜），那么该等待者在
`acquire()` 内收到的是 `CancelledError` 而不是锁，所有权必须转交给下一位等待者（见
`src/core/locks.cpp` 中的 `handoff()`）。不过成因不同：asyncio 在 `acquire()` 的 `except
CancelledError` 分支里重新唤醒队首，是因为它等待的 future 可能已被 resolve；而这里的形态来自端口契约
的第 4 条保证（取消腿赢下同一时隙冲突）。

### 时间与精度

内部使用 int64 飞秒（10⁻¹⁵ s，与 SV 的最高精度一致）；提交给后端时按 `ticks_per_sec()` 向上取整 ——
保证"至少睡这么久"，而不是可能偏短。`sleep(0)` 被钳制为 0 tick，但它仍然经过单一阻塞点：调用方真的
挂起并把调度步让出去，而仿真时钟不前进（≡ `asyncio.sleep(0)`）。零 tick 等待并不是绕过端口的空操作
—— 一个不挂起就返回的 sleep 会让所有纯让出循环失效（由 `tests/task_semantics.cpp` 的 case10/case11
固定）。

**算术**：`duration` 支持 `+ - * /`（包括 `k * d`、`d * k`、`d += …` 等复合赋值，以及
`duration / duration` 得到整数倍），这样时延可以由运行时变量计算（`rounds * 100_ns`）。三条约束：

1. **溢出绝不静默回绕**：与超范围字面量的编译期守卫同一原则 —— 运行时算术溢出抛
   `std::overflow_error`，除零抛 `std::domain_error`。这不是风格问题：int64 飞秒回绕会变成负 tick，
   而负 tick 在端口线格式中表示"没有超时腿"，放它过去就会永久阻塞。
2. **有符号量，非正时延钳为 0**：`d1 - d2` 可以为负（差值与偏移都是合法值），但交给 `sleep` /
   `wait_for` 的非正时延一律按 0 tick 处理（≡ asyncio 的 `delay <= 0`，让出一次；因此 `wait_for`
   会立刻超时并取消任务）。钳制的落点是 `to_ticks()`，这是负 tick 通往后端的唯一闸门。
3. **仅整数缩放**：不做浮点乘除，因为仿真时间必须可复现 —— 浮点比例会在两端引入舍入差异；需要比例时
   先在整数域里换算（`n * d / 8`）。浮点操作数由 `= delete` 的模板在**编译期拒绝**（否则 `d * 1.5`
   会经隐式转换静默变成 `d * 1`，与内建类型的表现不同且毫无提示）；除以整数向零截断。

## 头文件分层

依赖只向下指，且每个公共头都必须能独立编译：

```
weave/config.hpp   后端选择宏（默认 SVDPI）
weave/time.hpp     duration（int64 飞秒）+ 字面量
weave/task.hpp     Task / create_task / sleep / CancelledError
weave/event.hpp    Event
weave/locks.hpp    Lock / LockGuard / Condition（通信原语，构建在 Event 之上）
weave/queue.hpp    Queue<T> / PriorityQueue<T> / LifoQueue<T> + QueueEmpty / QueueFull /
                  QueueShutDown（通信原语，构建在 Event 之上；是 detail::QueueBase<T, Store>
                  之上的仅头文件别名；异常的关键函数位于 src/core/queue.cpp）
weave/semaphore.hpp Semaphore / SemaphoreGuard / BoundedSemaphore / BoundedSemaphoreGuard
                  （通信原语，构建在 Event 之上）
weave/barrier.hpp Barrier / BrokenBarrierError（通信原语，构建在 Condition 之上）
weave/sync.hpp     gather / wait_for / wait / shield / as_completed / TaskGroup
weave/test.hpp     WEAVE_TEST 注册
weave/sc.hpp       SystemC 后端 bootstrap 入口点
weave.hpp          伞头：仅包含这一个文件即可获得完整公共 API
```

"构建在 X 之上"是一种*实现*依赖，而不是头文件包含关系：`locks.hpp` / `queue.hpp` /
`semaphore.hpp` / `barrier.hpp` 不会引入任何其他 weave 头（只引入 `<memory>` 一类的依赖，以及
queue 所需的 `weave/event.hpp`），因为共享的 `Event` / `Condition` 机制位于它们各自的
`src/core/*.cpp` 翻译单元中。确实存在的包含链只向下指（`task → time`、`sync → task + time`、
`queue → event`）。

核心内部状态只存在于 `src/core/*_internal.hpp`（绝不出现在公共头中，也绝不出现在 src/ 之外）。

## 范围

Weave 聚焦并发编排，不覆盖 EDA 验证的全部内容。**不提供**：DUT 信号级访问；总线 driver/monitor；
UVM 组件层次；transaction 与覆盖率；SystemC TLM 建模；仿真内核本身；C++ 线程与协程调度。这些来自
SystemVerilog、UVM、cocotb、SystemC 等既有工具 —— Weave 把 C++ 测试行为以统一接口接入其中。

## 面向大语言模型的设计哲学

Weave 假定大模型熟悉 asyncio 的范式，但不假定它理解硬件语义，因此问题被拆成两层：Weave 提供
**小而稳定**的任务/事件/同步编排接口（见 README 中的映射表），而协议、模型与测试约束由用户和专用
验证组件提供。对于 AI 辅助验证，Weave 提供的是一个更适合生成式与反馈驱动修正的仿真控制接口，而不是
"自动理解硬件"的能力。
