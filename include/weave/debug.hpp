#pragma once

// Diagnostic surface: a read-only view of the task wait graph, plus a watchdog that reports a stall
// on its own. Not a synchronisation primitive — snapshot()/dump() neither suspend nor change
// library state, so they are safe to call from a failing test, a done callback, or the harness
// chain (where no task context exists).
//
// What it can and cannot tell you: the core knows which event each task is blocked on and for how
// long (simulation time), because every suspension passes through the single blocking point. It
// does NOT know who is supposed to wake that event — Event/Pulse are value objects and any party
// may set()/fire() — so a snapshot describes a hang, it does not prove a cycle (see the deadlock
// section of docs/architecture.md).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <weave/task.hpp>
#include <weave/time.hpp>

namespace weave {
namespace debug {

// One task blocked at a suspension point at the moment of the snapshot.
struct Waiter {
  int task_id = -1;
  std::string task_name;
  // Backend wait-event id it is suspended on; -1 when the suspension has only a timer leg (sleep,
  // wait_for's deadline) and therefore no event to be woken by.
  int ev_id = -1;
  duration since;  // How long it has been blocked, in simulation time
};

struct Snapshot {
  std::vector<Waiter> waiters;  // Every task blocked right now, in task-id order
  // Every task created but not yet terminal, blocked or not (a PENDING task has been scheduled and
  // has not run yet). Names, not handles: a hang report is read, not driven.
  std::vector<std::string> pending_tasks;
};

// Pure export: no suspension, no state change. Only allocation may fail (std::bad_alloc).
Snapshot snapshot();

// One line per waiter and one per live task, prefixed with tag, on stderr. Never suspends and never
// throws (allocation failures are swallowed), so it can be called while diagnosing a hang.
void dump(const char* tag = "weave") noexcept;

// Progress watchdog: a task that waits idle of *simulation time* for some other task to resume, and
// reports the wait graph when the window passes with nothing happening. "Progress" is a return
// from the single blocking point — some task's wait ended — so a system that keeps ticking never
// trips it.
//
// Must be started from a task context; the returned handle is the watchdog task itself, cancel()
// stops it (an unawaited watchdog keeps watching). It fires once and then finishes.
//
// on_fire, when given, runs inside the watchdog task before the report is printed, so a client can
// log or inspect the snapshot in its own format; it must not suspend. abort_on_fire turns the
// report into an abort: every live task except the watchdog and the root is cancelled with
// "watchdog: no progress for <idle>" (processes are never killed), which unwinds the stalled await
// chain into a cancelled run.
//
// Two blind spots are inherent to measuring simulation time:
//  - a zero-delay livelock (tasks spinning on sleep(0)) advances the clock not at all, so the
//    watchdog's own deadline never arrives and it cannot fire;
//  - a single sleep() longer than idle looks exactly like a stall, which is why the default is to
//    report rather than to abort.
Task watchdog(duration idle, bool abort_on_fire = false,
              std::function<void(const Snapshot&)> on_fire = {});

}  // namespace debug
}  // namespace weave
