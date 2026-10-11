#include <weave/config.hpp>

#if defined(WEAVE_BACKEND_SVDPI)

// SVDPI 后端（默认）：把 port 契约映射到 sv/weave_pkg.sv 导出的 DPI 原语，
// 并提供 SV 测试骨架（sv/weave_tb.sv）需要的引导入口。
//
// DPI 边界：
//   import（SV 提供）   : weave_sv_* —— 事件表、三腿等待、spawn
//   export（本文件）   : weave_run / weave_test_status —— tb 引导
//   export（core）     : weave_task_main —— 任务分派，SV 经 weave_sv_spawn
//                         的 fork 进程间接触发

#include <svdpi.h>

#include <cstdint>

#include "../core/registry_internal.hpp"

extern "C" {
// DPI imports（实现在 sv/weave_pkg.sv，由 SV 侧提供）
void weave_sv_spawn(int task_id);
void weave_sv_trigger(int ev_id);
int weave_sv_create_event();
void weave_sv_destroy_event(int ev_id);
long long weave_sv_ticks_per_sec();
void weave_sv_set_root_done_ev(int ev_id);
void weave_sv_wait(int ev_id, long long timeout_ticks, int abort_id, int* status);
}

namespace weave {
namespace port {

void spawn(int task_id) {
  svScope scope = svGetScope();
  svSetScope(svGetScopeFromName("weave_pkg"));
  weave_sv_spawn(task_id);
  svSetScope(scope);
}
void trigger(int ev_id) {
  svScope scope = svGetScope();
  svSetScope(svGetScopeFromName("weave_pkg"));
  weave_sv_trigger(ev_id);
  svSetScope(scope);
}
int create_event() {
  svScope scope = svGetScope();
  svSetScope(svGetScopeFromName("weave_pkg"));
  int ev_id = weave_sv_create_event();
  svSetScope(scope);
  return ev_id;
}
void destroy_event(int ev_id) {
  svScope scope = svGetScope();
  svSetScope(svGetScopeFromName("weave_pkg"));
  weave_sv_destroy_event(ev_id);
  svSetScope(scope);
}
std::int64_t ticks_per_sec() {
  svScope scope = svGetScope();
  svSetScope(svGetScopeFromName("weave_pkg"));
  std::int64_t ticks = weave_sv_ticks_per_sec();
  svSetScope(scope);
  return ticks;
}
void wait(int ev_id, std::int64_t timeout_ticks, int abort_id, int* status) {
  svScope scope = svGetScope();
  svSetScope(svGetScopeFromName("weave_pkg"));
  weave_sv_wait(ev_id, timeout_ticks, abort_id, status);
  svSetScope(scope);
}

}  // namespace port
}  // namespace weave

// ── DPI exports（由 SV 侧调用）────────────────────────────────────────

extern "C" void weave_run() {
  const int done_ev = weave::detail::run_registered_test();
  if (done_ev >= 0) {
    svScope scope = svGetScope();
    svSetScope(svGetScopeFromName("weave_pkg"));
    weave_sv_set_root_done_ev(done_ev);
    svSetScope(scope);
  }
}

extern "C" int weave_test_status() {
  return weave::detail::root_status();
}

#endif  // WEAVE_BACKEND_SVDPI
