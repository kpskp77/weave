#include "../core/config.hpp"

#if defined(WEAVE_BACKEND_SYSTEMC)

// SystemC backend (WEAVE_BACKEND_SYSTEMC): maps the port contract onto sc_event/sc_spawn.
//
// Process model: when an SC_THREAD suspends, the C++ call stack freezes, just like an SV process,
// so the core bookkeeping layer is reused as-is; scheduling/activation is entirely carried by the
// SystemC kernel:
//   - spawn   → an sc_spawn dynamic process (the child runs only after the parent suspends,
//               consistent with fork...join_none / create_task semantics)
//   - events  → sc_event; the three-legged wait uses sc_event_or_list + a kernel timed
//               wait (wait(sc_time, or_list))
//   - time    → sc_get_time_resolution determines the fs→tick conversion
//
// Usage (the test side provides sc_main) — xmake-repo's SystemC looks for sc_main with C linkage:
//   #include <weave/weave.hpp>
//   extern "C" int sc_main(int, char**);
//   extern "C" int sc_main(int, char**) { return weave::sc::run(); }
//   // run() spawns the root task and the reporting process: on a terminal state it prints
//   // PASSED/FAILED, sc_start() runs until sc_stop(), and root_status() becomes the exit code
//
// Verified on xmake-repo SystemC 3.0.2 (the weave_smoke_sc target, all gather semantics assertions
// passing on a real kernel); interface behaviour mirrors sv/weave_pkg.sv.

#if defined(__has_include)
#if !__has_include(<systemc>)
#error "WEAVE_BACKEND_SYSTEMC requires SystemC headers (<systemc> / systemc.h)"
#endif
#endif

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <systemc>
#include <unordered_map>

#include "../core/core_internal.hpp"
#include "../core/registry_internal.hpp"

namespace weave {
namespace {

std::unordered_map<int, std::unique_ptr<sc_core::sc_event>> g_events;
int g_next_ev = 0;
// Task identity per process (src/core/port.hpp — owner_task): the same table the SV package keeps
// as p_owner[process], keyed by the process object instead. The whole simulation is one OS thread
// and coroutines interleave on it, and a process need not be a task at all (a plain SC_THREAD or
// the reporting process may call into weave while a task body is parked), so the identity must be
// keyed by *who is asking*, never by what the last writer left behind. Absent = no task body.
std::unordered_map<const sc_core::sc_object*, int> g_owner;

// The process asking right now, or nullptr outside any process (sc_main, elaboration).
const sc_core::sc_object* current_process() {
  const sc_core::sc_process_handle self = sc_core::sc_get_current_process_handle();
  return self.valid() ? self.get_process_object() : nullptr;
}

sc_core::sc_event& event_by_id(int ev_id) {
  const auto it = g_events.find(ev_id);
  if (it == g_events.end()) throw std::out_of_range("weave: invalid event id");
  return *it->second;
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
  event_by_id(ev_id).notify();
}

int create_event() {
  const int id = g_next_ev++;
  g_events.try_emplace(id, std::make_unique<sc_core::sc_event>());
  return id;
}

void destroy_event(int ev_id) {
  // Reports an unknown id the same way event_by_id does (otherwise a double destroy would be
  // silently swallowed); erase(key) returns the deletion count, saving a find, and the unique_ptr
  // does the actual release
  if (g_events.erase(ev_id) == 0) throw std::out_of_range("weave: invalid event id");
}

std::int64_t ticks_per_sec() {
  return static_cast<std::int64_t>(1.0 / tick_seconds() + 0.5);
}

std::int64_t now_ticks() {
  // sc_time_stamp().value() is already expressed in units of sc_get_time_resolution (the same tick
  // ticks_per_sec describes), so no conversion takes place — and none of the double rounding that
  // to_seconds()/tick_seconds() would introduce.
  return static_cast<std::int64_t>(sc_core::sc_time_stamp().value());
}

int owner_task() {
  const sc_core::sc_object* self = current_process();
  if (self == nullptr) return 0;
  const auto it = g_owner.find(self);
  return it == g_owner.end() ? 0 : it->second;
}

void set_owner_task(int task_id) {
  const sc_core::sc_object* self = current_process();
  if (self == nullptr) return;  // no process to publish for (sc_main): nobody can read it either
  if (task_id == 0)
    g_owner.erase(self);
  else
    g_owner[self] = task_id;
}

void wait(int ev_id, std::int64_t timeout_ticks, int abort_id, int* status) {
  const bool has_to = timeout_ticks >= 0;
  const bool has_ab =
      abort_id >= 0;  // No cancel leg: an ownerless suspension has nothing to cancel
  // The identity is keyed by the asking process, so nothing here has to be saved and restored: a
  // parked task keeps its own entry while another process suspends (and possibly blocks with no
  // identity at all) on top of it.
  // Without a timeout leg, do not build an sc_time from a negative tick (negative time is
  // meaningless); SC_ZERO_TIME is used as a placeholder
  const sc_core::sc_time to =
      has_to ? sc_core::sc_time(timeout_ticks * tick_seconds(), sc_core::SC_SEC)
             : sc_core::SC_ZERO_TIME;
  const sc_core::sc_event* e = ev_id >= 0 ? &event_by_id(ev_id) : nullptr;
  const sc_core::sc_event* ab = has_ab ? &event_by_id(abort_id) : nullptr;
  if (e != nullptr && ab != nullptr) {
    if (has_to)
      sc_core::wait(to, *e | *ab);
    else
      sc_core::wait(*e | *ab);
  } else if (e != nullptr) {
    if (has_to)
      sc_core::wait(to, *e);
    else
      sc_core::wait(*e);
  } else if (ab != nullptr) {
    if (has_to)
      sc_core::wait(to, *ab);
    else
      sc_core::wait(*ab);
  } else if (has_to) {
    sc_core::wait(to);  // Timeout leg only (ownerless sleep: no event to hit, nothing can cancel)
  } else {
    throw std::logic_error("weave: wait with no leg");
  }
  // On a same-slot conflict the cancel leg wins (consistent with weave_sv_wait in sv/weave_pkg.sv)
  if (ab != nullptr && ab->triggered())
    *status = 2;
  else if (e != nullptr && e->triggered())
    *status = 0;
  else
    *status = 1;
}

}  // namespace port

namespace sc {

int run() {
  const int done_ev = detail::run_registered_test();
  if (done_ev < 0) return -1;
  // Reporting process: waits for the root task's terminal state, prints the result and stops the
  // simulation. sc_event has no memory: if the root task is already terminal
  // before the reporting process's first wait (e.g. the first test fails at delta 0), the delta
  // notification would be missed — so check the state first and only wait when it is not terminal;
  // no other process can interleave between the state check and the wait (single active process),
  // so there is no race.
  sc_core::sc_spawn([done_ev] {
    while (detail::root_status() < 0) sc_core::wait(event_by_id(done_ev));
    const int st = detail::root_status();
    if (st == 0)
      std::printf("WEAVE TEST PASSED\n");
    else
      std::printf("WEAVE TEST FAILED (status=%d)\n", st);
    std::fflush(stdout);
    sc_core::sc_stop();
  });
  sc_core::sc_start();  // Runs until sc_stop (called by the reporting process on the root's
                        // terminal state)
  return detail::root_status();
}

}  // namespace sc
}  // namespace weave

#endif  // WEAVE_BACKEND_SYSTEMC
