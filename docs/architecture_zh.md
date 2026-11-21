# Weave 架构（中文版）

面向需要理解或修改 Weave 实现的人。可直接执行的不变量清单在 [AGENTS.md](../AGENTS.md)；后端集成部分
见 [backends](backends_zh.md)。

## 核心与后端分离

Weave 分两层，中间夹着一份稳定的端口契约：

```
C++ 测试逻辑 (WEAVE_TEST)
        │  公共 API (include/weave/)
        ▼
核心状态管理 (src/core/) ── task/event/sync/time/registry + locks/queue/semaphore/barrier
        │  唯一依赖：port.hpp（6 个原语 + 不挂起的 now_ticks / owner_task 查询
        │  + set_owner_task 身份写入 + weave_task_main 派发入口）
        ▼
后端适配器 (src/backend/) ── svdpi.cpp（默认）/ systemc.cpp
        │
        ▼
仿真内核（VCS/IES 自动驱动，SystemC 本地运行；其他 DPI 仿真器只需满足同一契约
                    —— Xcelium 尚未验证）
```

- 核心层**只能**依赖 `src/core/port.hpp` 声明的原语，绝不可直接引用 DPI、SystemC 或线程 API。
- 新增后端 = 实现端口契约 + 提供一个 bootstrap 入口，核心代码零改动。
- 后端选择只有一个开关：`src/core/config.hpp` 里的编译期宏（互斥，默认 SVDPI；公共头不依赖该宏，
  因此它不属于对外 API）。xmake 的 `--backend=` 只是挑这个宏，并由此决定链接哪一个包。

## 端口契约

`src/core/port.hpp` 的文件头注释是契约的完整描述，四条保证概述如下：

1. **进程模型**：`spawn(id)` 创建一个仿真进程来执行 `weave_task_main(id)`；子进程要等父进程走到下一个
   挂起点（或结束）才运行——等同于 asyncio `create_task` 的“已调度，但不立即执行”。
2. **同一时刻只有一个活跃进程**：任意时刻至多一个进程在执行 C++ 状态维护代码，而且它不会被打断——
   任务主体一路跑到下一个阻塞点，中途没有任何进程能插进来。所以一个主体在两个阻塞点之间做的事，
   对其他任务来说都是原子的：改注册表、把等待者从队列里摘掉、把锁交给队首、清一个标志，都不需要
   `std::mutex` / `std::atomic` / 自旋锁 / `std::thread`——在仿真器里、跨 DPI 时它们本来也没有意义。
   由此还得出一条规则：**要跨越挂起存活的状态，必须在进入 `port::wait` 之前就写成一致状态、并对其他
   进程可见**，因为从那一刻起别人就可能读取或改写它。（“此刻在跑的是哪个任务”这份身份按进程存在后端，
   见 [backends_zh](backends_zh.md) 的身份一节。）
3. **一次性事件**：`create/trigger/destroy`；`trigger` 绝不能指向一个已被销毁的 id。
4. **唯一的挂起点** `wait(ev, timeout, abort, status)`：事件、超时、取消三条通道竞争，同一时刻有多条
   就绪时**取消优先**。`ev_id < 0` 表示没有事件通道，`timeout_ticks < 0` 表示没有超时通道，
   `abort_id < 0` 表示没有取消通道（此时 status 绝不会报 2）——无主叶子等待用的就是这个形式；
   三条通道全关的等待会被拒绝。

## 关键设计决策

### 为什么不用 C++20 协程

用户代码跑在仿真器进程里，挂起和恢复都由仿真内核调度：SV process 或 SystemC SC_THREAD 一挂起，
整个 C++ 调用栈就跟着冻住。改用协程就得自己写一套调度器，而这套调度器必然与仿真时间/事件模型打架。
反过来，把挂起交给后端进程去做、让 API 保持阻塞语义，才和仿真内核最合拍。

### 为什么取消是协作式的

`cancel()` 只干三件事：置位取消标志、记下 msg、触发私有的 abort 事件；真正的 `CancelledError` 是在
阻塞点注入的。对任务进程使用 SV `disable` 或 `process::kill()` 是明令禁止的：在任意一点杀掉进程，
C++ 调用栈都会从中间断开，RAII 展开不了，资源泄漏，内部状态也被破坏。（后端里唯一一处
`disable fork` 属于定时器服务，见 `sv/weave_pkg.sv`：它只 disable 自己那段服务分支，碰不到别的
等待者的栈帧。这是唯一有据可查的例外，不是疏漏。）这和 asyncio 的协作式取消是一回事：在 await 点抛出
`CancelledError`，可以捕获，也可以不理——捕获后清理干净、正常返回的 body，最终状态就是 DONE。
取消是请求，不是处决。`cancel(msg)` 与 asyncio 3.9+ 的 `Task.cancel(msg)` 对齐：msg 存进任务状态，
阻塞点注入的 `CancelledError` 以及之后 `await` / `gather` 重抛的地方都带着同一个 msg；组合子级联
取消兄弟任务时传空 msg（msg 属于发起取消的那一方）。

取消异常的类型层次同样照 asyncio：`CancelledError` **刻意不继承 `std::exception`**（对应 Python 3.8+
里它是 `BaseException`），这样惯用的 `catch (const std::exception&)` 兜底就不会把取消悄悄吞掉——一旦
吞掉，任务会错误地以 DONE 收场，gather / TaskGroup / wait_for 对 CANCELLED 终态的判定全部失效。
反过来，`TimeoutError` **确实继承 `std::exception`**（对应 asyncio 里它是普通的 `Exception`），因为
超时属于正常失败，通用的日志和兜底代码理应能处理。

### 失败传播：谁取消谁，谁等谁结束

各组合子遵循 asyncio 的默认规则（不隐式取消兄弟任务），并且每一种的写法都保证：没有子任务能比捕获它
的那个栈帧活得更久。

- **`gather` 只上报，不接管。** 它看到第一个失败就重抛，兄弟任务照旧运行——取消它们是 `TaskGroup`
  的事，不是 `gather` 的。只有当 `gather` 自己在阻塞点被取消时，它才级联取消，因为此时调用方正在
  退栈，否则子任务会让一个已经消失的栈帧继续活着。
- **`wait_for` 超时不是抛出去就完事。** 超时先胜出，接着取消子任务、**等它真正结束**，
  `TimeoutError` 才从这次调用里出去——调用方的下一句绝不会在子任务还在跑的时候执行。这次内部取消
  带着标好来源的 msg（`"wait_for timeout"`），即 asyncio 3.12+ 的约定，让“内部超时”与“外部取消”
  能区分开。
- **被取消的等待者不会把子任务丢在运行中。** 如果是 `wait_for` 的*调用方*在阻塞点被取消，就先级联
  取消被等的任务、等它结束，再把原来的 `CancelledError` 重抛出来——msg 原样保留，级联那一路不带
  msg（msg 属于发起取消的一方）。`timeout(body, limit)` 就是这一套：body 被包成一个任务，与
  `wait_for` 共用同一份实现，所以“超时”和“调用方被取消”不可能给出两种行为。
- **`TaskGroup` 是结构化并发的那一半**，默认行为正好相反：第一个 *FAILED* 的子任务（CANCELLED 不算）
  会取消尚未结束的兄弟任务；所有失败——包括收尾过程中才结束的——都汇总进 `ExceptionGroup`，
  `wait()` 要等每个子任务都结束才抛。它自己的等待者被取消不算失败：子任务照样被收掉，向外传播的是
  调用方的 `CancelledError`（连 msg 一起）。

“等它结束”取决于子任务是否真的停下来，跟超时无关。所以一个卡在外部阻塞里的子任务，会把 `wait_for` /
`TaskGroup::wait()` 拖过截止时间——这个代价为什么可以接受，下一节再说。

### weave 管不到的挂起：外部阻塞与无主等待

weave 只认一个挂起点，代码却可能从两处越出契约。这两种情况都不是记错账，而是账本根本看不见：

- **外部挂起**：任务 body 卡在 weave 不负责的调用上——走另一个 `export "DPI-C"` 进到 SV 里等、
  在用到的 import 里写 `#delay`、调用阻塞式的第三方库或 HLS。此时 body 确实没在跑，但
  `TaskRecord::state` 还是 `RUNNING`，`wait_ev` 还是 -1，`debug::snapshot()` 里也没有等待节点：
  不是记错，是看不见。
- **无主等待**：调用进程根本没有在跑任务 body——直接调用库导出函数的测试台进程、`sc_main`、
  不是任务的 `SC_THREAD`，以及 done 回调。

**外部挂起的代价。** 取消只在阻塞点送达，所以对一个正被外部阻塞的任务调用 `cancel()`，只是把取消标志
置上然后等：要等 body 返回、走到下一个 weave 阻塞点，取消才生效；如果 body 返回后不再阻塞，这个取消
永远等不到生效，任务直接以 DONE 结束——和 asyncio 取消一个不再 await 的协程一样。这种延迟没法靠
抢占式杀进程来缩短，而本设计禁止那条路（见“为什么取消是协作式的”）。

还要说清楚一点：受影响的只是“取消之后很快生效”这个承诺，超时本身没有失真。`wait_for(t, d)` 会在 `d`
到期时如实发出取消，然后等 `t` 结束，而这一程不受 `d` 约束；`TaskGroup::wait()` 取消兄弟任务后，
同样要等它们全部结束才返回。被取消的 `gather` 不一样：它级联取消后立刻上抛，子任务继续跑，不会被
外部阻塞拖住。所以要避开的是“对可能被外部阻塞的任务用 `wait_for` / `TaskGroup::wait()`”这个组合：
超时的含义没变（`d` 仍然是发出取消的时刻），只是函数返回得不会早于外部阻塞结束。

**符合契约的做法：等代理任务。** `shield` 给外部 worker 提供一个可取消的前台：所有调用方（包括各个
组合子）等的都是代理任务，而代理的 body 就一句 `inner.await()`——一次三条通道齐全的 weave 等待。
这样 `wait_for` 能在 `d` 准时触发，`TaskGroup` / `gather` 的收尾也能立刻返回，worker 在下面继续跑。
留在契约之外的只剩 worker 自己：它的副作用、资源生命周期和及时性，由调用方负责。这是故意的——
weave 中断不了它，就不该装作能。

如果这个阻塞本身可以改造，有两条更省事的路：

- **把通知改成等待**：让 SV 侧 fire 一个共享的 `weave_edge`（`weave_edge::fire()`），而不是回调
  一个会阻塞的导出函数；C++ 侧改成等 `sv::Edge::wait()` 或等 `Event::set()`。这样等待本身就在
  weave 里，取消和超时直接生效，连代理都不需要。
- **分块之间协作式轮询**：能拆成若干步的 body，可以在每步之间调一次 `sleep(0)`。它经过阻塞点，
  已置位的取消会在这里注入，body 按普通异常展开退出，代价是每块一次调度。

**无主等待**是同一个问题的另一半，`backend_wait` 按“这个原语需不需要身份”来区分。叶子等待（`sleep`、
`Event::wait`、`Pulse::wait`、`sv::Edge::wait`）允许没有身份：它退化为一次普通的后端等待，没有
取消通道，也不进等待图，代价是丢掉取消和诊断信息。除此之外的一切——所有组合子、`Task::await`、
带所有权的原语（`Lock`、`Semaphore`、`Barrier`、`Queue`）——对无主调用方一律**直接终止运行**：这类
原语要么得把 `TaskRecord` 交给队头，要么得有取消通道；而在无主的调用点上抛异常，要么经使用方自己的
DPI export 退栈（未定义行为），要么被人接住，最后照样打印通过标记。同一条终止路径也覆盖 body 返回
**之后**的挂起（done 回调）：那会把终态记录挂进等待图，而且永远取消不了。身份本身由后端按仿真进程
保存（见下文“死锁诊断”的“相关的一致性规则”），正因如此，非任务的 `SC_THREAD` 才能在一个任务挂起
期间回调进 weave，看到的是 `current_task() == nullopt`，而不是那个挂起任务的名字。

诊断上有个必须承认的局限：被取消、但仍卡在外部阻塞里的任务，和一个正在计算的任务分不出来——都是
`RUNNING`、都没有等待节点——而 `cancelled()` 只反映终态，公共 API 里没有任何东西能把这两者分开。

### 死锁诊断：只诊断 core 看得见的边

内部状态确实能看见的等待环，**直接终止运行**，不抛异常：`Task::await()` 自己（拿正在执行的任务 id 和
句柄 id 比一下）、在组内某个子任务里调用 `TaskGroup::wait()`（组要等所有子任务结束，包括调用者自己）、
任务在自身 body 里 `shield()` 自己（代理要等它的调用方）。三者都走 `detail::fatal_violation` →
`port::fatal`，先打诊断再 dump 等待图（见下文“契约违例为何直接终止运行”）。这三处检查都只是对已有
数据做一次比较，开销可以忽略；`TaskGroup` 的检查在关闭任务组之前，`shield` 的检查在创建代理任务之前。

刻意**不**诊断的是经过 `Event` 的环：API 层面根本表达不出“谁会 set 我正在等的事件”——`Event` 是值
对象，谁都可以 `set()`——core 硬要建这张图，只能靠猜。这类挂死只能“勘察”，无法“证明”：
`debug::snapshot()`（include/weave/debug.hpp）导出等待图——唯一的阻塞点会记下 `TaskRecord::wait_ev`
和 `wait_since`，快照因此能说出谁阻塞在哪个后端事件上、按仿真时间阻塞了多久，而且这两项由守卫回收，
取消抛出时不会留下过期的等待者；`debug::dump()` 把图打印出来，由人或失败的测试来判断。同样的限制也
适用于互相 `Task::await` 的环（`a` 等 `b`、`b` 等 `a`），以及经另一个任务的 `shield` 绕出来的环
（`a` 屏蔽 `b`，而 `b` 在等 `a`）：这些边没有任何地方记录，core 只看得到终态事件。

相关的一致性规则：执行中任务的身份由**后端按仿真进程**保存（`port::owner_task`），不是核心里的全局
变量。所以一个在自己进程里挂起的任务，再也干扰不到“从仿真代码回调进 C++ 的那个进程”的身份——过去
测试台调用库的导出函数、同时另一个任务正处于冻结状态时，它看到的是“最后挂起的那个任务”。派发入口
在 body 前后写入身份（`port::set_owner_task`，并保存旧值，使同一栈上嵌套的 body 结束后原样归还），
并在 done 回调运行前清空，因此 `current_task()` 的含义就是“此刻正在执行的任务”，任务上下文之外返回
`nullopt`。叶子等待（`sleep`、`Event::wait`、`Pulse::wait`、`sv::Edge::wait`）允许完全没有身份地
运行：它退化为一次普通后端等待，没有取消通道，也不进等待图。

### 契约违例为何直接终止运行

有些违例不是某次运行失败，而是运行前提就不成立。这类问题 core 不能指望调用方配合：异常总是能被捕获，
一旦被吞掉，运行就只剩下挂死——没有 `TaskRecord` 进入 FAILED，`root_status()` 保持 0，于是照样打出
`WEAVE TEST PASSED`。无主站点更糟：调用方可能是使用方自己的 `export "DPI-C"` 回调进来的进程，经
仿真器退栈进入那个 export 是未定义行为，weave 拦不住。类型层次在这里也没用——`catch (...)` 总是写
得出来。

所以 port 多了第五个原语 `port::fatal`：`detail::fatal_violation`（src/core/task.cpp）把诊断拼好，
以 `critical` 级别写进 simlog——致命诊断本身就是关于仿真状态的报告，理应和其余 trace 走同一个流、
同一个仿真时间戳（`WEAVE_SITE` 提供 `file:line`）：

```
[           0 ps][weave.debug][critical] weave: fatal: a task cannot await itself (site src/core/task.cpp:185, task 2 'fatal_semantics')
```

这样它才进得了仿真器自己的记录（SVDPI 的 sink 走 `vpi_printf`，所以 `simv -l run.log` 里能看到），
也进得了使用方的文件 sink——直接写 `stderr` 两边都不会有。接着 `debug::dump("fatal")` 打印等待图，
`detail::flush_trace()` 刷 sink，最后调 `port::fatal`，由各后端映射到自己的失败通道：SVDPI 调
`sv/weave_pkg.sv` 里的 `weave_sv_fatal`（`$fatal(1, …)`，走既有的 DPI scope guard），SystemC 报
`SC_REPORT_FATAL("weave", …)`。第二条通道会把同一行诊断再带一遍，所以就算 `SIMLOG_LEVEL` 把所有
weave sink 都静音，诊断依然在——两条路都不会让一次运行被误当成通过。全程不展开栈，仿真以非零状态
结束。回归测试盯的就是这一点：`tests/fatal_semantics.cpp`（故意自 await）编进独立的一组载荷，由
`tests/expect_fatal.sh` 和 `tests/svdpi/run.sh --expect-fatal` 以**反向**预期判定——诊断行必须出现，
通过标记必须不出现。

五处致命站点：可诊断的等待环（`Task::await()` 自己、组内子任务里的 `TaskGroup::wait()`、任务 body 里
`shield()` 自己）、在没有 weave 任务的进程里调用阻塞原语、body 返回之后的挂起（done 回调）。其余
调用方能恢复的误用——`Lock`/`Semaphore` 协议错误、`as_completed`/`TaskGroup` 误用、无效任务句柄——
都在任何状态被改动之前抛出，仍然是普通的 `std::logic_error`。

### 为什么 Event 是边沿触发而不是水平触发

`set()` 会给当时的等待者拍快照并逐个触发它的一次性唤醒事件：只要进了快照，就一定会被放行，哪怕紧接着
发生 `clear()`。标志在**进入时**照样会看（`wait()` 在一个已置位的事件上直接返回，不挂起）；纯边沿的
只是入队*之后*那一次判断。当时考虑过、后来否掉的做法是：记住 `set()` 发生过，等等待者恢复后再查一次
标志——但 set 与 clear 落在同一个调度步时这样会丢唤醒：等待者恢复、读到 `false`、又睡回去。
asyncio.Event 的解法一样（它在 `set()` 时直接给当时 `_waiters` 里的那些 future 设结果），所以快照
这个做法是从 asyncio 借来的，不是 Weave 独创；真正属于 Weave 的只有它的代价。

这个代价躲不掉：每个等待者都阻塞在自己那个一次性后端事件 id 上，所以在**两条返回路径**（成功和取消）
上都必须先把事件从等待队列里摘掉，再销毁这个 id，否则后面的 `set()` 会触发一个已销毁的 id。这个顺序
就是那条不变量（由一个 `detail::scope_exit` 保证，顺序写在 lambda 里），每个代码点的理由都留在
`src/core/event.cpp` 的文件头注释里。

这段清理交给 `detail::on_scope_exit` 这类守卫：它的析构函数既不抛异常也不挂起——会挂起的守卫等于把
清理变成第二个阻塞点，也就是退栈过程中第二个能被取消的地方。守卫覆盖到哪一步，取决于
`CancelledError` 会不会逃出这个栈帧：`Event::wait()` / `Pulse::wait()` 是把异常抛出函数的，所以
清标志和销毁事件这一对放在同一个守卫里（`src/core/sync.cpp` 里 asyncio.wait 组合子也是同一对写法）；
`TaskGroup::wait()` 就地捕获取消，等子任务全部结束才重抛，其间没有东西能逃出去，于是只给销毁这一步
加守卫。

**`Pulse` 是同一套机制的纯边沿版本**——唤醒事件的记账一模一样，只是不留标志。它存在的理由就是
SystemVerilog 的 `-> ev` 天生是这个语义；硬塞进 sticky 的 `Event` 会产生悄无声息的误触发：一个在
pulse 之后才开始等的等待者，会消费掉一个本不属于它的边沿。`Pulse::generation()` 是留给这类调用方的：
它需要知道不在等待的这段时间里有没有边沿溜过，而原语本身没法告诉它；为它缓存一个边沿，那又是另一个
原语了。两者共有“先解链、再销毁”这条不变量——`src/core/event.cpp` 陈述一次，`Pulse::wait()` 照同样
的写法再写一遍。

### 为什么 Lock 在 release() 内部就把所有权交出去

`release()` 在**同一次不挂起的调用**里把所有权交给队首等待者（FIFO），全程 `held` 保持 true：不存在
“锁已释放却还没人接管”的窗口，`locked()` 在交接的空隙里也不会短暂变成 false。asyncio 的做法不同，
它的交接要等被唤醒的等待者再次被调度才完成：`release()` 先清 `_locked`、再唤醒队首（这两句的先后在
不同 CPython 版本里不一样——3.6 先降标志，3.12+ 先唤醒——所以哪种顺序都不能依赖），等待者恢复后才
重新把标志置起来，于是所有权已经转出，`locked()` 却还会在一段时间里报 `false`（新来者仍然插不了队，
因为那个尚未移除的 future 让它的快路径保持关闭）。在同一时刻完成交接，等于把这个窗口消掉，而不是写进
文档里说明。又因为每个排队的等待者都拿着一个一次性 `Event` 当交接信号，锁依然由 `Event` 加唯一的
阻塞点构成，后端不需要任何改动。

取消不能让锁凭空消失，这一点与 asyncio 一致：如果取消和交接落在同一个调度步（`release()` 刚把锁交给
某个等待者，那个等待者随即被 `cancel()`，而且后端的取消通道胜出），它在 `acquire()` 里收到的是
`CancelledError` 而不是锁，所有权必须转给下一位（见 `src/core/locks.cpp` 的 `handoff()`）。不过成因
不同：asyncio 在 `acquire()` 的 `except CancelledError` 分支里重新唤醒队首，是因为它等的 future 可能
已经被 resolve 了；这里的情形来自端口契约第 4 条保证（取消通道赢下同一时刻的冲突）。

### 时间与精度

内部用 int64 飞秒（10⁻¹⁵ s，与 SV 的最高精度一致）表示时间；交给后端时按 `ticks_per_sec()` 向上
取整，保证“至少睡够这么久”，而不是可能偏短。`now()` 走反方向：向后端要当前 tick 数
（`port::now_ticks`，端口契约两个不挂起查询之一），用同一个 tick 率换算，因此
`wait_until(now() + d)` 与 `sleep(d)` 落在同一个 tick 上。读时钟不会推进时钟（仿真器在两个挂起点
之间本来就不走时间），core 还会把返回值按单调不减处理：统计空闲区间的调用方不会看到时间倒退。`sleep(0)`
按 0 tick 算，但照样经过唯一的阻塞点：调用方真的挂起、让出一个调度步，而仿真时钟不动
（≡ `asyncio.sleep(0)`）。零 tick 等待不是绕过端口的空操作——如果 sleep 不挂起就返回，所有只为了
让出执行权而写的循环都会失效（`tests/task_semantics.cpp` 的 case10/case11 覆盖了这一点）。

**算术**：`duration` 支持 `+ - * /`（含 `k * d`、`d * k`、`d += …` 这类复合赋值，以及
`duration / duration` 得到整数倍），所以时延可以用运行时变量算（`rounds * 100_ns`）。三条约束：

1. **溢出绝不静默回绕**：与超范围字面量的编译期检查出于同一原则——运行时算术溢出抛
   `std::overflow_error`，除零抛 `std::domain_error`。这不是风格问题：int64 飞秒一回绕就变成负
   tick，而负 tick 在端口那边表示“没有超时通道”，放过去就是永久阻塞。
2. **有符号量，非正时延按 0 处理**：`d1 - d2` 可以是负的（差值和偏移都是合法值），但交给 `sleep` /
   `wait_for` 的非正时延一律当 0 tick（≡ asyncio 的 `delay <= 0`，让出一次；因此 `wait_for` 会
   立刻超时并取消任务）。钳制发生在 `to_ticks()`，它是负 tick 抵达后端前唯一的关口。
3. **只做整数缩放**：不碰浮点乘除，因为仿真时间必须可复现——浮点比例会在两端引入舍入差异；真需要
   比例就在整数域里换算（`n * d / 8`）。浮点操作数由 `= delete` 的模板在**编译期拒绝**（否则
   `d * 1.5` 会经隐式转换悄悄变成 `d * 1`，与内建类型的行为不同，还没有任何提示）；除以整数时向零
   截断。

## 头文件分层

依赖只向下指，而且每个公共头都必须能独立编译：

```
weave/time.hpp     duration（int64 飞秒）+ 字面量
weave/task.hpp     Task / create_task / sleep / CancelledError
weave/event.hpp    Event / Pulse
weave/sv.hpp       sv::Edge（具名跨语言边沿；无依赖）
weave/debug.hpp    debug::snapshot / debug::dump / debug::take_unretrieved_failures
weave/locks.hpp    Lock / LockGuard / Condition（通信原语，构建在 Event 之上）
weave/queue.hpp    Queue<T> / PriorityQueue<T> / LifoQueue<T> + QueueEmpty / QueueFull /
                  QueueShutDown（通信原语，构建在 Event 之上；是 detail::QueueBase<T, Store>
                  之上的仅头文件别名；异常的关键函数位于 src/core/queue.cpp）
weave/semaphore.hpp Semaphore / SemaphoreGuard / BoundedSemaphore / BoundedSemaphoreGuard
                  （通信原语，构建在 Event 之上）
weave/barrier.hpp Barrier / BrokenBarrierError（通信原语，构建在 Condition 之上）
weave/sync.hpp     gather / wait_for / wait / shield / as_completed / TaskGroup +
                  ExceptionGroup（组合子与结构化并发）
weave/test.hpp     WEAVE_TEST 注册
weave/sc.hpp       SystemC 后端 bootstrap 入口点
weave.hpp          伞头：仅包含这一个文件即可获得完整公共 API
```

“构建在 X 之上”指的是*实现*依赖，不是头文件包含关系：`locks.hpp` / `queue.hpp` / `semaphore.hpp` /
`barrier.hpp` 不引入任何其他 weave 头（只引入 `<memory>` 这类依赖，以及 queue 需要的
`weave/event.hpp`），因为共享的 `Event` / `Condition` 机制在它们各自的 `src/core/*.cpp` 翻译单元里。
真正存在的包含链只向下指（`task → time`、`sync → task + time`、`queue → event`、
`debug → task + time`、`sv → 无`）。

核心内部状态只存在于 `src/core/*_internal.hpp`（绝不出现在公共头里，也绝不出现在 src/ 之外）。

## 范围

Weave 只做并发编排，不覆盖 EDA 验证的全部内容。**不提供**：DUT 信号级访问；总线 driver/monitor；
UVM 组件层次；transaction 与覆盖率；SystemC TLM 建模；仿真内核本身；C++ 线程与协程调度。这些能力
在 SystemVerilog、UVM、cocotb、SystemC 等既有工具里；Weave 做的是用一套统一接口把 C++ 测试行为
接进去。

## 面向大语言模型的设计哲学

Weave 假定大模型熟悉 asyncio 的范式，但不假定它懂硬件语义，因此把问题分成两层：Weave 提供**小而
稳定**的任务/事件/同步编排接口（映射表见 README），协议、模型和测试约束由用户和专用验证组件提供。
对 AI 辅助验证来说，Weave 给的是一套更适合“生成 + 反馈式修正”的仿真控制接口，而不是“自动理解
硬件”的能力。
