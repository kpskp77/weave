#include <weave/config.hpp>

#if defined(WEAVE_BACKEND_SVDPI)

// SVDPI backend (default): maps the port contract onto the DPI primitives exported by
// sv/weave_pkg.sv, and provides the bootstrap entries the testbench top needs.
//
// DPI boundary:
//   imports (provided by SV): weave_sv_* — event table, three-legged wait, spawn
//   exports (this file)     : weave_start / weave_test_status — tb bootstrap
//   exports (core)          : weave_task_main — task dispatch, reached indirectly by SV through
//                             weave_sv_spawn's forked process

#include <svdpi.h>

#include <cstdint>

#include "../core/registry_internal.hpp"

extern "C" {
// DPI imports (implemented in sv/weave_pkg.sv, provided by the SV side)
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
  weave_sv_spawn(task_id);
}
void trigger(int ev_id) {
  weave_sv_trigger(ev_id);
}
int create_event() {
  return weave_sv_create_event();
}
void destroy_event(int ev_id) {
  weave_sv_destroy_event(ev_id);
}
std::int64_t ticks_per_sec() {
  return weave_sv_ticks_per_sec();
}
void wait(int ev_id, std::int64_t timeout_ticks, int abort_id, int* status) {
  weave_sv_wait(ev_id, timeout_ticks, abort_id, status);
}

}  // namespace port
}  // namespace weave

// ── DPI exports (called by the SV side) ────────────────────────────────────────

extern "C" void weave_start() {
  const int done_ev = weave::detail::run_registered_test();
  if (done_ev >= 0) {
    weave_sv_set_root_done_ev(done_ev);
  }
}

extern "C" int weave_test_status() {
  return weave::detail::root_status();
}

#endif  // WEAVE_BACKEND_SVDPI
