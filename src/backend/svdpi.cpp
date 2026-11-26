#include "../core/config.hpp"

#if defined(WEAVE_BACKEND_SVDPI)

// SVDPI backend (default): maps the port contract onto the DPI primitives exported by
// sv/weave_pkg.sv, and provides the bootstrap entries the testbench top needs.
//
// DPI boundary:
//   imports (provided by SV): weave_sv_* — event table, three-legged wait, spawn, task identity
//   exports (this file)     : weave_start / weave_test_status — tb bootstrap
//                             weave_sv_edge_id / weave_sv_edge_bind — named cross-language
//                             edges (include/weave/sv.hpp)
//   exports (core)          : weave_task_main — task dispatch, reached indirectly by SV through
//                             weave_sv_spawn's forked process

#include <svdpi.h>

#include <cstdint>
#include <cstdlib>
#include <weave/sv.hpp>

#include "../core/registry_internal.hpp"

extern "C" {
// DPI imports (implemented in sv/weave_pkg.sv, provided by the SV side)
void weave_sv_spawn(int task_id);
void weave_sv_trigger(int ev_id);
int weave_sv_create_event();
void weave_sv_destroy_event(int ev_id);
long long weave_sv_ticks_per_sec();
long long weave_sv_now_ticks();
// Task identity of the calling simulation process (0 = no task body); weave_sv_set_owner is called
// by the core dispatch entry and 0 clears the entry.
int weave_sv_current_task();
void weave_sv_set_owner(int task_id);
void weave_sv_set_root_done_ev(int ev_id);
int weave_sv_edge_id(const char* name);
void weave_sv_edge_bind(const char* name, int ev_id);
void weave_sv_wait(int ev_id, long long timeout_ticks, int abort_id, int* status);
// Terminates the simulation through $fatal (see weave_sv_fatal in sv/weave_pkg.sv): the only way
// the core can end a run it cannot continue, and the only one that keeps the report inside the
// simulator's own failure channel.
void weave_sv_fatal(const char* msg);
}

namespace weave {
namespace port {

// ── DPI scope fixing ────────────────────────────────────────────────────────────────────────────
// The engine resolves a C++→SV call (an `export "DPI-C"` in sv/weave_pkg.sv) through the DPI scope
// of the calling process. A consumer's testbench process imports its own package, not weave_pkg, so
// when C++ is reached through the consumer's own DPI import that scope has no `weave_sv_*` in it
// and VCS 2018.09 aborts the whole run with
//   Error-[DPI-DXFNF] … 'weave_sv_now_ticks' … is not defined or visible
// on the first suspension or identity query. Every library embedding weave would otherwise have to
// know, cache and restore weave's DPI scope around its own calls into weave — a backend detail
// leaking into every consumer — so the backend does it here, at the only place that knows the
// package: each import below runs under this guard. The caller's scope is restored on the way out
// because the consumer still needs it for its own exports (tests/svdpi_scope_semantics.cpp pins
// both halves).
namespace {

constexpr const char* kWeaveSvPackage = "weave_pkg";

svScope weave_scope() {
  // One package scope per simulation, and the core guarantees at most one process executes
  // bookkeeping at any moment, so a plain cache needs no locking. A null result (a design that
  // renames the package, or an engine that cannot resolve package scopes) leaves the guard inert:
  // the behaviour is then exactly what it was without the guard, never worse.
  static const svScope scope = svGetScopeFromName(kWeaveSvPackage);
  return scope;
}

class DpiScopeGuard {
public:
  DpiScopeGuard() : prev_(svGetScope()) {
    if (const svScope target = weave_scope(); target != nullptr) svSetScope(target);
  }
  ~DpiScopeGuard() {
    if (prev_ != nullptr) svSetScope(prev_);
  }
  DpiScopeGuard(const DpiScopeGuard&) = delete;
  DpiScopeGuard& operator=(const DpiScopeGuard&) = delete;

private:
  svScope prev_;
};

}  // namespace

void spawn(int task_id) {
  const DpiScopeGuard scope;
  weave_sv_spawn(task_id);
}
void trigger(int ev_id) {
  const DpiScopeGuard scope;
  weave_sv_trigger(ev_id);
}
int create_event() {
  const DpiScopeGuard scope;
  return weave_sv_create_event();
}
void destroy_event(int ev_id) {
  const DpiScopeGuard scope;
  weave_sv_destroy_event(ev_id);
}
std::int64_t ticks_per_sec() {
  const DpiScopeGuard scope;
  return weave_sv_ticks_per_sec();
}
std::int64_t now_ticks() {
  const DpiScopeGuard scope;
  return weave_sv_now_ticks();
}
int owner_task() {
  const DpiScopeGuard scope;
  return weave_sv_current_task();
}
void set_owner_task(int task_id) {
  const DpiScopeGuard scope;
  weave_sv_set_owner(task_id);
}
void wait(int ev_id, std::int64_t timeout_ticks, int abort_id, int* status) {
  const DpiScopeGuard scope;
  weave_sv_wait(ev_id, timeout_ticks, abort_id, status);
}

void fatal(const char* msg) {
  // $fatal in the package: it reports the message and ends the simulation immediately (non-zero
  // status), which is exactly the contract — no unwind through any C++ frame, no chance to
  // continue. std::abort() only covers an engine whose $fatal returns anyway, so the [[noreturn]]
  // promise holds either way.
  const DpiScopeGuard scope;
  weave_sv_fatal(msg);
  std::abort();
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

// Named cross-language edges (include/weave/sv.hpp): the testbench asks for the id C++ registered
// under a name, and publishes the id it created itself. Declared to SystemVerilog in
// sv/weave_pkg.sv as imports, so they must keep C linkage and a trivially convertible signature (a
// DPI `string` argument arrives as a NUL-terminated const char*).
extern "C" int weave_sv_edge_id(const char* name) {
  return weave::sv::lookup_edge(name);
}

extern "C" void weave_sv_edge_bind(const char* name, int ev_id) {
  weave::sv::claim_edge(name, ev_id);
}

#endif  // WEAVE_BACKEND_SVDPI
