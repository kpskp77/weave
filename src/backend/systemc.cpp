#include <weave/config.hpp>

#if defined(WEAVE_BACKEND_SYSTEMC)

// SystemC 后端（WEAVE_BACKEND_SYSTEMC）：把 port 契约映射到 sc_event/sc_spawn。
//
// 进程模型：SC_THREAD 挂起时 C++ 调用栈冻结，与 SV 进程一致，
// 因此核心簿记层原样复用；调度/激活全部由 SystemC 内核承担：
//   - spawn   → sc_spawn 动态进程（父进程挂起后子进程才运行，与
//               fork...join_none / create_task 语义一致）
//   - 事件    → sc_event；wait 三腿竞速用 sc_event_or_list +
//               内核定时等待（wait(sc_time, or_list)）
//   - 时间    → sc_get_time_resolution 决定 fs→tick 换算
//
// 用法（测试侧提供 sc_main）——xmake-repo 的 SystemC 按 C 链接寻找 sc_main：
//   #include <weave/weave.hpp>
//   extern "C" int sc_main(int, char**);
//   extern "C" int sc_main(int, char**) { return weave::sc::run(); }
//   // run() spawn 根任务与汇报进程：终态打印 PASSED/FAILED，
//   // sc_start() 至 sc_stop()，返回 root_status() 作进程退出码
//
// 已在 xmake-repo SystemC 3.0.2 上验证（weave_sc_smoke 目标，
// 全部 gather 语义断言在真实内核上通过）；接口行为对照 sv/weave_pkg.sv。

#if defined(__has_include)
#if !__has_include(<systemc>)
#error "WEAVE_BACKEND_SYSTEMC requires SystemC headers (<systemc> / systemc.h)"
#endif
#endif

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <systemc>
#include <unordered_map>

#include "../core/core_internal.hpp"
#include "../core/registry_internal.hpp"

namespace weave {
namespace {

std::unordered_map<int, sc_core::sc_event*> g_events;
int g_next_ev = 0;

sc_core::sc_event* event_by_id(int ev_id) {
  const auto it = g_events.find(ev_id);
  if (it == g_events.end()) throw std::out_of_range("weave: invalid event id");
  return it->second;
}

double tick_seconds() {
  return sc_core::sc_get_time_resolution().to_seconds();
}

}  // namespace

namespace port {

void spawn(int task_id) {
  sc_core::sc_spawn([task_id] { weave_task_main(task_id); });
}

void trigger(int ev_id) {
  event_by_id(ev_id)->notify();
}

int create_event() {
  const int id = g_next_ev++;
  g_events[id] = new sc_core::sc_event();
  return id;
}

void destroy_event(int ev_id) {
  delete g_events[ev_id];
  g_events.erase(ev_id);
}

std::int64_t ticks_per_sec() {
  return static_cast<std::int64_t>(1.0 / tick_seconds() + 0.5);
}

void wait(int ev_id, std::int64_t timeout_ticks, int abort_id, int* status) {
  const sc_core::sc_event& ab = *event_by_id(abort_id);
  const bool has_to = timeout_ticks >= 0;
  const sc_core::sc_time to(timeout_ticks * tick_seconds(), sc_core::SC_SEC);
  if (ev_id >= 0) {
    const sc_core::sc_event& e = *event_by_id(ev_id);
    if (has_to)
      sc_core::wait(to, e | ab);
    else
      sc_core::wait(e | ab);
  } else {
    if (has_to)
      sc_core::wait(to, ab);
    else
      sc_core::wait(ab);
  }
  // 同槽冲突时取消腿优先（与 sv/weave_pkg.sv 的 weave_sv_wait 一致）
  if (ab.triggered())
    *status = 2;
  else if (ev_id >= 0 && event_by_id(ev_id)->triggered())
    *status = 0;
  else
    *status = 1;
}

}  // namespace port

namespace sc {

int run() {
  const int done_ev = detail::run_registered_test();
  if (done_ev < 0) return -1;
  // 汇报进程：等待根任务终态，输出结果并停止仿真（≡ sv/weave_tb.sv）。
  // sc_event 无记忆：若根任务在汇报进程首次 wait 之前就已终态（如首个测试
  // delta 0 即失败），delta notification 会被错过——故先查状态，未终态才等待；
  // 查状态与 wait 之间无其他进程插空（单活动进程），不会竞态。
  sc_core::sc_spawn([done_ev] {
    while (detail::root_status() < 0) sc_core::wait(*event_by_id(done_ev));
    const int st = detail::root_status();
    if (st == 0)
      std::printf("WEAVE TEST PASSED\n");
    else
      std::printf("WEAVE TEST FAILED (status=%d)\n", st);
    std::fflush(stdout);
    sc_core::sc_stop();
  });
  sc_core::sc_start();  // 运行至 sc_stop（汇报进程在根任务终态时调用）
  return detail::root_status();
}

}  // namespace sc
}  // namespace weave

#endif  // WEAVE_BACKEND_SYSTEMC
