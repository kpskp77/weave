# Weave 架构（中文版）

面向需要理解或修改 Weave 实现的人。可直接执行的不变量清单见 [AGENTS.md](../AGENTS.md)；后端集成
部分见 [backends](backends_zh.md)。

## 核心/后端分离

Weave 分为两层，中间是一份稳定的端口契约：

```
C++ 测试逻辑 (WEAVE_TEST)
        │  公共 API (include/weave/)
        ▼
核心状态管理 (src/core/) ── task/event/sync/time/registry + locks/queue/semaphore/barrier
        │  唯一依赖：port.hpp（6 个原语 + weave_task_main 派发入口）
        ▼
后端适配器 (src/backend/) ── svdpi.cpp（默认）/ systemc.cpp
        │
        ▼
仿真内核（VCS/IES 自动驱动，SystemC 本地运行；其他 DPI 仿真器只需满足同一契约
                    —— Xcelium 尚未验证）
```

- 核心层**只能**依赖 `src/core/port.hpp` 中声明的原语，绝不能直接引用 DPI / SystemC / 线程 API。
- 新增后端 = 实现端口契约 + 提供一个 bootstrap 入口点，核心代码零改动。
- 后端选择只有一个开关，即编译期宏（`src/core/config.hpp`，互斥，默认 SVDPI；公共头不依赖
  该宏，因此它不属于对外 API）；xmake 的
  `--backend=` 只是选择该宏（并因此决定链接两个包中的哪一个）。

## 端口契约

`src/core/port.hpp` 的文件头注释是契约的完整描述，四条保证概述如下：

1. **进程模型**：`spawn(id)` 创建一个仿真进程来执行 `weave_task_main(id)`；子进程只有在父进程到达
   下一个挂起点（或结束）之后才运行，等价于 asyncio `create_task` 的"已调度，但不立即执行"。
2. **同一时刻只有一个活跃进程**：任意时刻至多有一个进程执行 C++ 状态维护代码（核心依赖这一点维持全局
   状态一致，因此 `g_current` 之类的变量无需加锁）。
3. **一次性事件**：`create/trigger/destroy`；`trigger` 绝不能指向一个已被销毁的 id。
4. **唯一的挂起点** `wait(ev, timeout, abort, status)`：事件、超时、取消三路竞争，同一时隙冲突时
   **取消一路优先**。`ev_id < 0` 表示没有事件一路，`timeout_ticks < 0` 表示没有超时一路，但
   **`abort_id` 必须始终是有效 id**：SystemC 适配器会无条件解析它（`event_by_id(abort_id)`），只有
   SV 侧恰好能容忍它缺失。

## 关键设计决策

### 为什么不用 C++20 协程

Weave 的用户代码运行在仿真器进程内部，其挂起与恢复由仿真内核调度：当 SV process 或 SystemC
SC_THREAD 挂起时，整个 C++ 调用栈随之冻结。若改用协程，就必须自建一套调度器，而这套调度器必然会与
仿真时间/事件模型冲突。相比之下，把挂起交给后端进程实现的阻塞式 API，才是与仿真内核最自然的搭配。

### 为什么取消是协作式的

`cancel()` 只置位一个标志并唤醒任务，在阻塞点注入 `CancelledError` 异常。对*任务*进程使用 SV
`disable` / `process::kill()` 是被禁止的：在任意点杀掉进程会撕裂 C++ 调用栈，RAII 无法展开，资源
泄漏，内部状态也会被破坏。（后端中唯一一处 `disable fork` 属于定时器服务，见 `sv/weave_pkg.sv`：
它只会 disable 自己的服务局部支路，永远触及不到其他等待者的栈帧；这是有据可查的唯一例外，并非
疏漏。）这与 asyncio 的协作式取消同构：在 await 点抛出 `CancelledError`，可捕获、也可拒绝。
`cancel(msg)` 与 asyncio 3.9+ 的 `Task.cancel(msg)` 一致：msg 保存在任务状态中，阻塞点注入的
`CancelledError` 与之后 `await` / `gather` 重抛之处都会保留同一个 msg；组合子级联取消兄弟任务时
传递的是空 msg（msg 属于发起取消者的上下文）。

取消信号的类型层次同样遵循 asyncio：`CancelledError` **刻意不派生自 `std::exception`**（对应
Python 3.8+ 中 `CancelledError` 是 `BaseException`），这样惯用的
`catch (const std::exception&)` 兜底就不会悄悄吞掉取消——一旦吞掉，任务会错误地以 DONE 结束，
破坏 gather / TaskGroup / wait_for 对 CANCELLED 终态的判定。反之，`TimeoutError` **确实派生自
`std::exception`**（对应 asyncio 的 `TimeoutError`，它是普通的 `Exception`），因为超时属于正常
失败路径，通用的日志与兜底代码应当能够处理。

### 为什么 Event 采用边沿触发而非水平触发

`set()` 会对当时的等待者做快照并逐个触发其一次性唤醒事件：只要被快照到，就一定会被释放，即便紧接着
发生了 `clear()`。标志在**进入时**依然会被检查（`wait()` 在已置位的事件上直接返回，不挂起）；纯边沿
的只是入队*之后*的那次判定。被否决的另一种做法是记住 `set()` 发生过、等待者恢复后再检查一次标志，但
这样在 set/clear 落在同一调度步时会丢失唤醒：等待者恢复、读到 `false`、然后再次阻塞。asyncio.Event
的处理方式相同（它在 `set()` 时直接设置当时位于 `_waiters` 中那些 future 的结果），所以快照取自
asyncio 而非 Weave 独创；Weave 特有的只是它的代价。

这个代价无法回避：每个等待者阻塞在自己的、一次性后端事件 id 上，因此在**两条返回路径**（成功与取消）
上都必须先解链、再销毁该 id，否则后续的 `set()` 会触发一个已销毁的 id。这一顺序就是那条不变量（由
一个 `detail::scope_exit` 保证，顺序写在 lambda 体内），每个代码点的推理保留在
`src/core/event.cpp` 的文件头注释里。

### 为什么 Lock 在 release() 内部完成所有权交接

`release()` 在**同一次不挂起的调用**里把所有权交给队首等待者（FIFO），期间 `held` 始终为 true：
不存在"已释放但尚未被接管"的窗口，`locked()` 在交接间隙也不会短暂变为 false。asyncio 的做法不同，
它的交接要等被唤醒的等待者重新被调度后才完成：`release()` 清除 `_locked` 并唤醒队首（这两条语句的
先后在不同 CPython 版本中不同——3.6 先降标志，3.12+ 先唤醒——因此两种顺序都不应被依赖），等待者
恢复后才重新举起标志，于是所有权虽已转出，`locked()` 却会在一段时间内报告 `false`（新来者仍无法插队，
因为尚未移除的 future 使其快路径保持关闭）。同步交接消除了这个窗口，而不是把它写进文档；又因为每个
排队等待者都持有一个一次性 `Event` 作为交接信号，锁仍由 `Event` 加单一阻塞点构成，后端无需任何改动。

取消不能让锁消失，这一点与 asyncio 一致：如果取消与交接落在同一调度步（`release()` 刚把锁交给某个
等待者，该等待者随即被 `cancel()`，且后端的取消一路获胜），那么该等待者在 `acquire()` 中收到的是
`CancelledError` 而不是锁，所有权必须转交给下一位（见 `src/core/locks.cpp` 中的 `handoff()`）。
不过成因不同：asyncio 在 `acquire()` 的 `except CancelledError` 分支里重新唤醒队首，是因为它等待
的 future 可能已被 resolve；这里的情形则来自端口契约第 4 条保证（取消一路赢下同一时隙冲突）。

### 时间与精度

内部以 int64 飞秒（10⁻¹⁵ s，与 SV 的最高精度一致）表示时间，提交给后端时按 `ticks_per_sec()` 向上
取整，以保证"至少睡这么久"，而不是可能偏短。`sleep(0)` 被钳制为 0 tick，但仍经过唯一的阻塞点：调用方
真的挂起并让出调度步，而仿真时钟不前进（≡ `asyncio.sleep(0)`）。零 tick 等待并不是绕过端口的空操作
——若 sleep 不挂起就返回，所有仅做让出的循环都会失效（由 `tests/task_semantics.cpp` 的
case10/case11 固定）。

**算术**：`duration` 支持 `+ - * /`（含 `k * d`、`d * k`、`d += …` 等复合赋值，以及
`duration / duration` 得到整数倍），因此时延可以由运行时变量计算（`rounds * 100_ns`）。三条约束：

1. **溢出绝不静默回绕**：与超范围字面量的编译期检查遵循同一原则，运行时算术溢出抛
   `std::overflow_error`，除零抛 `std::domain_error`。这不是风格问题：int64 飞秒回绕会变成负 tick，
   而负 tick 在端口侧表示"没有超时一路"，一旦放过就会永久阻塞。
2. **有符号量，非正时延钳为 0**：`d1 - d2` 可以为负（差值与偏移都是合法值），但交给 `sleep` /
   `wait_for` 的非正时延一律按 0 tick 处理（≡ asyncio 的 `delay <= 0`，让出一次；因此 `wait_for`
   会立即超时并取消任务）。钳制发生在 `to_ticks()`，它是负 tick 抵达后端的唯一关口。
3. **只做整数缩放**：不做浮点乘除，因为仿真时间必须可复现——浮点比例会在两端引入舍入差异；需要比例时
   先在整数域内换算（`n * d / 8`）。浮点操作数由 `= delete` 的模板在**编译期拒绝**（否则
   `d * 1.5` 会经隐式转换静默变成 `d * 1`，与内建类型的表现不同且毫无提示）；除以整数时向零截断。

## 头文件分层

依赖只向下指，且每个公共头都必须能独立编译：

```
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
UVM 组件层次；transaction 与覆盖率；SystemC TLM 建模；仿真内核本身；C++ 线程与协程调度。上述能力
来自 SystemVerilog、UVM、cocotb、SystemC 等既有工具；Weave 所做的是以统一接口把 C++ 测试行为
接入其中。

## 面向大语言模型的设计哲学

Weave 假定大模型熟悉 asyncio 的范式，但不假定它理解硬件语义，因此把问题拆成两层：Weave 提供
**小而稳定**的任务/事件/同步编排接口（见 README 中的映射表），协议、模型与测试约束则由用户和专用
验证组件提供。对 AI 辅助验证而言，Weave 提供的是一套更适合生成式与反馈驱动修正的仿真控制接口，而不是
"自动理解硬件"的能力。
