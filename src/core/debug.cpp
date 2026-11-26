// Wait-graph diagnostics: walk the task bookkeeping and report who is blocked where, for how long.
// Backend-agnostic — the elapsed time comes from the clock query (port::now_ticks via
// weave::now_ticks), never from a wall clock: simulation time is virtual, and in SVDPI a wall clock
// has no relation to it at all.
//
// dump() reports through simlog ("weave.debug", info) instead of stderr: the lines are about
// simulation state, so they carry the same sim-time stamp as the rest of the trace and obey the
// same SIMLOG_LEVEL switch.

#include <algorithm>
#include <cstring>
#include <exception>
#include <weave/debug.hpp>

#include "core_internal.hpp"
#include "trace.hpp"

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

std::vector<UnretrievedFailure> take_unretrieved_failures() {
  std::vector<UnretrievedFailure> out;
  for (std::size_t i = 0; i < detail::g_tasks.size(); ++i) {
    detail::TaskRecord& r = detail::g_tasks[i];
    if (r.state != detail::TaskState::FAILED || r.eptr_retrieved) continue;
    const int id = static_cast<int>(i) + 1;  // Index = id - 1 (see core_internal.hpp)
    // The root task's failure is the run's own result, already surfaced by the runner ([FAIL] /
    // root_status): it is never an "unretrieved" failure.
    if (id == detail::g_root) continue;
    UnretrievedFailure f;
    f.task_id = id;
    f.task_name = r.name;
    try {
      std::rethrow_exception(r.eptr);
    } catch (const std::exception& e) {
      f.what = e.what();
    } catch (...) {
      // A foreign throw (weave_task_main stores whatever is not CancelledError); there is no
      // portable type name to show, so report the fact itself.
      f.what = "non-std exception";
    }
    r.eptr_retrieved = true;  // Report at most once per task, as asyncio's warning does
    out.push_back(std::move(f));
  }
  return out;
}

void dump(const char* tag) noexcept {
  const char* prefix = (tag != nullptr && tag[0] != '\0') ? tag : "weave";
  try {
    const auto log = detail::trace_debug();
    const Snapshot snap = snapshot();
    log->info("[{}] weave snapshot: {} waiting task(s), {} live task(s)", prefix,
              snap.waiters.size(), snap.pending_tasks.size());
    for (const Waiter& w : snap.waiters) {
      const long long age = static_cast<long long>(w.since.count());
      if (w.ev_id >= 0)
        log->info("[{}]   waiting: task {} '{}' on ev {} for {} fs", prefix, w.task_id, w.task_name,
                  w.ev_id, age);
      else
        log->info("[{}]   waiting: task {} '{}' on its timeout leg for {} fs", prefix, w.task_id,
                  w.task_name, age);
    }
    for (const std::string& name : snap.pending_tasks) {
      log->info("[{}]   live: task '{}'", prefix, name);
    }
  } catch (...) {
    // Diagnostics must never be the reason a run dies (allocation failure, or a logger that cannot
    // format). Nothing to report through: logging itself is what failed.
    try {
      detail::trace_debug()->info("[{}] weave snapshot: unavailable", prefix);
    } catch (...) {
    }
  }
}

}  // namespace debug

void fatal(std::string_view why) noexcept {
  // Fixed-buffer copy: the report path must not allocate (a std::bad_alloc would replace the
  // caller's diagnosis with an unrelated one) and a string_view is not NUL-terminated. Long text is
  // truncated here, and again by fatal_report when the composed line exceeds its own buffer.
  char reason[1024];
  const std::size_t n = std::min(why.size(), sizeof reason - 1);
  if (n > 0) std::memcpy(reason, why.data(), n);
  reason[n] = '\0';
  detail::fatal_report(nullptr, reason);
}

}  // namespace weave
