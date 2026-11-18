// Wait-graph diagnostics: walk the task bookkeeping and report who is blocked where, for how long.
// Backend-agnostic — the elapsed time comes from the clock query (port::now_ticks via
// weave::now_ticks), never from a wall clock: simulation time is virtual, and in SVDPI a wall clock
// has no relation to it at all.

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <weave/debug.hpp>
#include <weave/sync.hpp>

#include "core_internal.hpp"

namespace weave {
namespace debug {

Snapshot snapshot() {
  Snapshot snap;
  const std::int64_t now = weave::now_ticks();
  for (std::size_t i = 0; i < detail::g_tasks.size(); ++i) {
    const detail::TaskRecord& r = detail::g_tasks[i];
    if (detail::terminal(r.state)) continue;
    const int id = static_cast<int>(i) + 1;  // Index = id - 1 (see core_internal.hpp)
    snap.pending_tasks.push_back(r.name);
    // WAITING is the only state in which the task is suspended (backend_wait sets it around the
    // port::wait call); a PENDING task has not run yet and a RUNNING one is not blocked anywhere.
    if (r.state != detail::TaskState::WAITING) continue;
    Waiter w;
    w.task_id = id;
    w.task_name = r.name;
    w.ev_id = r.wait_ev;
    // Negative elapsed is a clock quirk, not a fact about the wait: report "just started" instead
    // of a negative duration.
    w.since = detail::from_ticks(std::max<std::int64_t>(0, now - r.wait_since));
    snap.waiters.push_back(std::move(w));
  }
  return snap;
}

void dump(const char* tag) noexcept {
  const char* prefix = (tag != nullptr && tag[0] != '\0') ? tag : "weave";
  try {
    const Snapshot snap = snapshot();
    std::fprintf(stderr, "[%s] weave snapshot: %zu waiting task(s), %zu live task(s)\n", prefix,
                 snap.waiters.size(), snap.pending_tasks.size());
    for (const Waiter& w : snap.waiters) {
      const long long age = static_cast<long long>(w.since.count());
      if (w.ev_id >= 0)
        std::fprintf(stderr, "[%s]   waiting: task %d '%s' on ev %d for %lld fs\n", prefix,
                     w.task_id, w.task_name.c_str(), w.ev_id, age);
      else
        std::fprintf(stderr, "[%s]   waiting: task %d '%s' on its timeout leg for %lld fs\n",
                     prefix, w.task_id, w.task_name.c_str(), age);
    }
    for (const std::string& name : snap.pending_tasks) {
      std::fprintf(stderr, "[%s]   live: task '%s'\n", prefix, name.c_str());
    }
    std::fflush(stderr);
  } catch (...) {
    // Diagnostics must never be the reason a run dies (allocation failure on a huge wait graph).
    std::fprintf(stderr, "[%s] weave snapshot: unavailable\n", prefix);
    std::fflush(stderr);
  }
}

Task watchdog(duration idle, bool abort_on_fire, std::function<void(const Snapshot&)> on_fire) {
  if (idle.count() <= 0)
    throw std::invalid_argument("weave::debug::watchdog: idle must be positive");
  Task w = create_task("weave_watchdog", [idle, abort_on_fire, on_fire = std::move(on_fire)] {
    // Watch window by window: as long as something else resumes in between, the system is alive and
    // the watchdog keeps measuring; the first window without a single resumption is the stall.
    std::uint64_t last = detail::g_progress;
    for (;;) {
      sleep(idle);
      if (detail::g_progress != last) {
        last = detail::g_progress;
        continue;
      }
      break;
    }
    if (on_fire) {
      try {
        on_fire(snapshot());
      } catch (...) {
        // A client's reporter must not turn a diagnostic into a different failure.
      }
    }
    dump("weave-watchdog");
    if (!abort_on_fire) return;
    // Abort without killing anything: cancel every live task except this one and the root, which is
    // the reporting chain — a cancelled stall still propagates through the await edges into it, as
    // CancelledError, and the run ends visibly instead of hanging.
    const std::string msg = "watchdog: no progress for " + std::to_string(idle.count()) + " fs";
    std::vector<int> victims;
    victims.reserve(detail::g_tasks.size());
    for (std::size_t i = 0; i < detail::g_tasks.size(); ++i) {
      const detail::TaskRecord& r = detail::g_tasks[i];
      const int id = static_cast<int>(i) + 1;
      if (detail::terminal(r.state) || id == detail::g_current || id == detail::g_root) continue;
      victims.push_back(id);
    }
    for (int id : victims) detail::task_from_id(id).cancel(msg);
  });
  // Marked after create_task returns and before the watchdog's first suspension: progress
  // accounting must exclude the watchdog's own sleep from its very first window.
  detail::record(w.id()).watchdog = true;
  return w;
}

}  // namespace debug
}  // namespace weave
