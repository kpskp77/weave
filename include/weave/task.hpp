#pragma once

// 任务原语：Task / create_task / sleep / Cancelled。语义对齐 asyncio.Task。
// create_task 只调度不立即运行：子任务等父任务碰到下一个阻塞点
// （或结束）后才获得调度，由后端进程语义保证。

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <weave/time.hpp>

namespace weave {

class Task;
struct Cancelled;

namespace detail {
struct TaskRecord;  // 核心簿记（定义于 src/core/core_internal.hpp）

// 构造带消息的 Cancelled（msg 由 TaskRecord 所有，视图在 throw 生命周期内有效）。
// 传视图而非 const char*：视图不拥有数据，与 Cancelled::msg_ 的借用语义一致，
// 且调用方（均为 std::string 成员）传整串时无需 strlen。
// 不变量：视图必须源自 std::string 的完整内容（尾部紧跟 NUL），
// 因为 Cancelled::what() 直接返回 data() 当 C 串用。
// 暴露给 detail:: 以便 sync.cpp 的 dispatch 路径保留原消息。
Cancelled cancelled_with_msg(std::string_view msg) noexcept;
}  // namespace detail

// 取消信号。对应 asyncio.CancelledError，在任务阻塞点注入。
// 刻意不派生 std::exception：对齐 asyncio 3.8 起 CancelledError 归入 BaseException
// 的分层——用户惯用的 catch (const std::exception&) 兜底不得静默吞掉取消（吞掉会使
// 任务错误地以 DONE 结束，破坏 gather / TaskGroup / wait_for 对 CANCELLED 终态的
// 判定）。捕获请显式写 catch (const weave::Cancelled&) 或 catch (...)。
//
// 消息对齐 asyncio Task.cancel(msg)（Python 3.9+）：msg 为取消者可选传入的说明，
// 未传消息（普通取消）对应 asyncio 的 msg=None，用 nullopt 表达。
// msg() 返回的视图指向 TaskRecord::cancel_msg，仅在捕获该异常的同一 throw
// 生命周期内有效。
//
// what() 是**虚函数**且**外联定义**（src/core/task.cpp），仅作诊断。这两点都是刻意
// 的：外联的虚函数充当 key function，使 vtable/typeinfo 只在库内生成一份，消费方
// 的 TU 只留未定义引用，跨 DSO 的 catch 才对得上（libc++abi 按指针比较 type_info；
// 类型若保持非多态，typeinfo 会退化成每个 TU 一份的 weak 定义，catch 在隐藏可见性
// 的构建下静默失配）。代价是多态类不再平凡可拷贝（标准规定含虚函数的类拷贝构造
// 不平凡）——这里选择 catch 正确性，断言改成盯真正关心的性质（见下方 static_assert）。
struct Cancelled {
  // 多态类型必须声明虚析构：Cancelled 会经 std::make_exception_ptr 等路径
  // 在堆上构造/销毁（gather(return_exceptions)），非虚析构会触发
  // -Wdelete-non-abstract-non-virtual-dtor（-Werror 下直接编译失败）。
  virtual ~Cancelled() = default;
  virtual const char* what() const noexcept;
  // msg=None → nullopt（不再用 nullptr 哨兵）
  std::optional<std::string_view> msg() const noexcept { return msg_; }

private:
  std::optional<std::string_view> msg_;
  friend Cancelled detail::cancelled_with_msg(std::string_view msg) noexcept;
};

// 异常对象 = "一个可空视图 + 判别"，不含堆成员、拷贝不抛：跳异常路径不额外分配。
// 注意不能要求 trivially_copyable：what() 是库内唯一生成的 key function（见上），
// 而标准规定多态类的拷贝构造不平凡——RTTI 跨 DSO 单份（catch 正确性）优先于这个
// 类型特征。这里改盯真正想要的两件事：拷贝/析构 nothrow，且无任何堆成员。
static_assert(std::is_nothrow_copy_constructible_v<Cancelled> &&
                  std::is_nothrow_destructible_v<Cancelled>,
              "Cancelled must be cheap to throw: nothrow copy, no heap members");

class Task;

namespace detail {
// 组合原语入口（定义于 src/core/sync.cpp）；此处声明仅供 Task 友元引用。
void gather_impl(std::initializer_list<Task> tasks);
// 组合原语内部的句柄/id 互转（定义于 src/core/task.cpp）。
int task_id(const Task& t);
Task task_from_id(int id);
// 后端引导入口（定义于 src/core/registry.cpp）；仅供后端适配层使用。
int run_registered_test();
}  // namespace detail

class Task {
public:
  Task() = default;

  // 等待任务完成。FAILED 任务重抛其异常；CANCELLED 任务抛 Cancelled。
  // 必须在任务上下文内调用（阻塞原语）。
  void await();

  // 到达任一终态（DONE / FAILED / CANCELLED），对齐 asyncio.Task.done()
  [[nodiscard]] bool done() const;
  [[nodiscard]] bool cancelled() const;
  [[nodiscard]] bool valid() const noexcept { return id_ >= 0; }
  const std::string& get_name() const;  // 对齐 asyncio.Task.get_name()

  // 协作式取消（对齐 asyncio Task.cancel(msg)，Python 3.9+）：
  // PENDING → body 不执行；RUNNING → 下一个阻塞点抛 Cancelled(msg)；
  // WAITING → 立即唤醒并在阻塞点抛 Cancelled(msg)。任务可捕获 Cancelled 拒绝取消。
  // msg 为可选的取消原因说明，空串表示普通取消；await 该任务者会看到带同一 msg 的 Cancelled。
  // const：Task 是句柄，取消改的是被引用任务的簿记状态而非句柄本身（同 shared_ptr 的
  // operator* const 语义），因此允许经由 const Task& 发起取消（如 gather 的级联取消）。
  void cancel(std::string msg = {}) const;

private:
  explicit Task(int id) : id_(id) {}
  int id_ = -1;

  friend Task create_task(std::string name, std::function<void()> body);
  friend void detail::gather_impl(std::initializer_list<Task> tasks);
  friend int detail::task_id(const Task& t);
  friend Task detail::task_from_id(int id);
  friend int detail::run_registered_test();
};

// 创建任务。与 asyncio 一致：新任务不会立即运行，
// 等当前任务碰到下一个阻塞点后才被调度（由后端进程语义保证）。
Task create_task(std::string name, std::function<void()> body);

template <typename F>
Task create_task(std::string name, F&& body) {
  static_assert(std::is_invocable_v<F&>, "weave: task body must be callable with no arguments");
  return create_task(std::move(name), std::function<void()>(std::forward<F>(body)));
}

// ── 时间等待 ──────────────────────────────────────────────────────────
// sleep(0) 特判为零时间让出（对应 asyncio.sleep(0)）。
void sleep(duration t);

}  // namespace weave
