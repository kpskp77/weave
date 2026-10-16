#pragma once

// 组合原语：gather / wait_for / wait / shield / as_completed / TaskGroup。
// 语义对齐 asyncio 同名原语：asyncio.gather（默认形态 + return_exceptions
// 形态）、asyncio.wait_for（3.12+ 超时语义）、asyncio.wait、asyncio.shield、
// asyncio.as_completed、asyncio.TaskGroup。
// 全部建于任务簿记 + Event + 唯一阻塞点之上，后端无需改动。

#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <weave/task.hpp>
#include <weave/time.hpp>

namespace weave {

struct Timeout {};  // 对应 asyncio.TimeoutError，wait_for 超时抛出

// ── gather ────────────────────────────────────────────────────────────

// 等待任务组（默认形态，对齐 asyncio.gather(return_exceptions=False)）：
//   · 任一任务到达 FAILED/CANCELLED 终态 → 立即按完成顺序重抛其异常，
//     其余任务不被取消、继续运行（取消兄弟任务是 TaskGroup 的语义）
//   · 全部正常完成 → 返回
//   · gather 自身被取消 → 先取消所有未完成任务（asyncio：gather 被取消时
//     未完成的子任务一并取消），再抛 Cancelled
template <typename... Ts>
void gather(Ts&... tasks) {
  detail::gather_impl({tasks...});
}

// return_exceptions 形态标签：对应 asyncio.gather(*aws, return_exceptions=True)
struct return_exceptions_t {
  explicit constexpr return_exceptions_t() = default;
};
inline constexpr return_exceptions_t return_exceptions{};

// return_exceptions 形态：永不因子任务失败提前返回；等全部落地后按输入顺序
// 返回结果——DONE → nullptr；FAILED → 其异常；CANCELLED → 包装 Cancelled
// 的 exception_ptr。gather 自身被取消仍按默认形态语义：
// 取消所有未完成任务后传播 Cancelled。
std::vector<std::exception_ptr> gather(std::initializer_list<Task> tasks, return_exceptions_t);

// ── wait_for ──────────────────────────────────────────────────────────

// 超时即 cancel 任务并等待取消完成，然后抛 Timeout（asyncio 3.12+ 语义）。
void wait_for(Task& task, duration timeout);

// ── wait（asyncio.wait）────────────────────────────────────────────────

enum class ReturnWhen { ALL_COMPLETED, FIRST_COMPLETED, FIRST_EXCEPTION };

struct WaitResult {
  std::vector<Task> done;     // 已终态（保持输入顺序）
  std::vector<Task> pending;  // 未完成（保持输入顺序）
};

// 对齐 asyncio.wait：只按当时状态分组返回，永不重抛子任务异常、
// 永不取消子任务。FIRST_EXCEPTION 仅 FAILED 触发提前返回（CANCELLED
// 不算，对齐 CPython 的 not f.cancelled() and f.exception() is not None）。
// timeout 到点按当时状态分组返回；duration{0} 特判为直接快照（不阻塞、
// 不让出）；负值（默认）= 无超时。
// 等待者自身被取消 → 传播 Cancelled，子任务不受影响。
WaitResult wait(std::initializer_list<Task> tasks,
                ReturnWhen return_when = ReturnWhen::ALL_COMPLETED,
                duration timeout = duration{-1});

// ── shield（asyncio.shield）────────────────────────────────────────────

// 返回代理任务：代理镜像 inner 的终态与异常，但对代理的 cancel /
// wait_for 超时 / gather 级联取消只停在代理层，inner 继续运行。
// （Task::await 本就不向被等任务传播取消——与 asyncio 一致；
//   shield 用于组合原语会取消子任务的场景。）
Task shield(Task& inner);

// ── as_completed（asyncio.as_completed）────────────────────────────────

class AsCompleted {
public:
  bool pending() const;  // 尚有未产出的任务

  // 阻塞至下一个任务完成，按完成顺序返回（已终态，.await() 立即取结果
  // 或重抛异常）；全部产出后调用抛 std::logic_error。
  // 取消只中断 next() 本身：已完成队列与子任务均不受影响。单消费者。
  Task next();

private:
  struct Impl;
  explicit AsCompleted(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;

  friend AsCompleted as_completed(std::initializer_list<Task> tasks);
};

AsCompleted as_completed(std::initializer_list<Task> tasks);

// ── TaskGroup（asyncio.TaskGroup）──────────────────────────────────────

// 结构化并发：组内 create_task 创建子任务，wait() 阻塞至全部完成：
//   · 全部完成（成功或被外部取消）→ 正常返回
//   · 首个 FAILED（按完成顺序）→ 取消所有未完成兄弟，等全部落地后重抛其异常
//   · 等待者被取消 → 级联取消全部未完成子任务，等落地后抛 Cancelled
// 子任务被外部取消（CANCELLED）不算失败、不触发兄弟取消
// （对齐 asyncio.TaskGroup 对外部取消子任务的容忍）。
// 偏差：无 ExceptionGroup，只重抛首个失败。
// wait() 之后 create_task 或再次 wait 抛 std::logic_error。
class TaskGroup {
public:
  TaskGroup();

  Task create_task(std::string name, std::function<void()> body);
  template <typename F>
  Task create_task(std::string name, F&& body) {
    return create_task(std::move(name), std::function<void()>(std::forward<F>(body)));
  }

  void wait();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace weave
