#pragma once

// Diagnostic surface: a read-only view of the task wait graph, plus the run's last resort. Not a
// synchronisation primitive — nothing here suspends and nothing in the debug namespace changes
// library state, so it is safe to call from a failing test, a done callback, or the harness chain
// (where no task context exists). The one entry that ends the run is fatal(): it is not a query but
// the caller's own verdict, and it terminates through the backend's failure channel.
//
// What it can and cannot tell you: the core knows which event each task is blocked on and for how
// long (simulation time), because every suspension passes through the single blocking point. It
// does NOT know who is supposed to wake that event — Event/Pulse are value objects and any party
// may set()/fire() — so a snapshot describes a hang, it does not prove a cycle (see the deadlock
// section of docs/architecture.md).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
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

// One FAILED task whose exception nobody ever retrieved — the entries consumed by
// take_unretrieved_failures().
struct UnretrievedFailure {
  int task_id = -1;
  std::string task_name;
  std::string what;  // The failure's what() text ("non-std exception" for a foreign throw)
};

// Every FAILED task whose exception has never been retrieved (awaited, gathered, collected by a
// combinator) nor reported before — and marks each as reported, so the report fires at most once
// per task. This is the analogue of asyncio's "Task exception was never retrieved" warning: the
// registered-test driver (src/core/registry.cpp) calls it after every test and prints the entries;
// a task that fails and is never awaited shows up exactly once, between that test's [PASS]/[FAIL]
// line and the next test's. Like asyncio, reporting is warning-only: it does not fail the run.
// Pure bookkeeping walk: no suspension, no task context needed, never touches the simulation
// state (allocation failures propagate, as in snapshot()).
std::vector<UnretrievedFailure> take_unretrieved_failures();

// One line per waiter and one per live task, prefixed with tag, through simlog ("weave.debug",
// info) — the same stream and sim-time stamp as the rest of the core trace, so SIMLOG_LEVEL
// controls it like any other module. Never suspends and never throws (allocation and formatting
// failures are swallowed), so it can be called while diagnosing a hang.
void dump(const char* tag = "weave") noexcept;

}  // namespace debug

// Ends the run because the caller has decided that the simulation cannot continue: a retry loop
// that cannot converge, an assumption only the test can check. Writes
// `weave: fatal: <why> (task N '<name>')` through simlog at critical, dumps the wait graph, flushes
// the sinks and terminates through the backend's own failure channel ($fatal /
// SC_REPORT_FATAL) — so the diagnosis reaches the simulator's transcript and no C++ frame unwinds
// into the kernel.
//
// Deliberately not an exception: a `catch (...)` in the caller, or in a combinator above it, would
// turn a run that cannot make progress into one that merely looks slow (no task ever reaches
// FAILED, so the run still prints its pass marker). Usable from any process context — a task body,
// a done callback, an ownerless testbench export — since it needs no task; the report names the
// executing task when there is one. The backend's failure channel needs the simulation kernel, so
// call it only from simulation code, never from static initialisation, a thread of your own, or
// after the run has stopped. `why` is one line; the report is truncated if it does not fit the
// fixed 1 KiB buffer.
[[noreturn]] void fatal(std::string_view why) noexcept;

}  // namespace weave
