// Wait-graph diagnostics: walk the task bookkeeping and report who is blocked where, for how long.
// Backend-agnostic — the elapsed time comes from the clock query (port::now_ticks via
// weave::now_ticks), never from a wall clock: simulation time is virtual, and in SVDPI a wall clock
// has no relation to it at all.

#include <algorithm>
#include <cstdio>
#include <weave/debug.hpp>

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

}  // namespace debug
}  // namespace weave
