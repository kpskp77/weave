// Wait-graph diagnostics self-check: a blocked task reports its suspension point and how long it
// has been held there (in simulation time), a PENDING task is live but not a waiter, a landed task
// leaves the graph, dump() is callable from a task and from outside any task without throwing, and
// the progress watchdog reports (and optionally aborts) a stall without tripping on steady
// progress. Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp). It shares the assertion-body style with task_semantics.cpp and is regressed by
// both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

namespace {

const debug::Waiter* find_waiter(const debug::Snapshot& snap, int task_id) {
  for (const debug::Waiter& w : snap.waiters) {
    if (w.task_id == task_id) return &w;
  }
  return nullptr;
}

bool has_live(const debug::Snapshot& snap, const std::string& name) {
  for (const std::string& n : snap.pending_tasks) {
    if (n == name) return true;
  }
  return false;
}

}  // namespace

WEAVE_TEST(debug_semantics) {
  // case 1: a task blocked in Event::wait shows up with its id, name and backend event, and leaves
  // the graph once it has landed
  run_case(__func__, "case1", [&] {
    Event gate, parked;
    auto t = create_task("parked_waiter", [&] {
      parked.set();
      gate.wait();
    });
    parked.wait();  // The child has suspended in gate.wait()
    const debug::Snapshot snap = debug::snapshot();
    const debug::Waiter* w = find_waiter(snap, t.id());
    if (w == nullptr) throw std::logic_error("case1: a blocked task must appear in the wait graph");
    if (w->task_name != "parked_waiter")
      throw std::logic_error("case1: the waiter must carry the task name");
    if (w->ev_id < 0) throw std::logic_error("case1: the waiter must carry its backend event id");
    if (w->since.count() < 0) throw std::logic_error("case1: the wait age must not be negative");
    if (!has_live(snap, "parked_waiter"))
      throw std::logic_error("case1: a blocked task is also a live task");
    gate.set();
    t.await();
    const debug::Snapshot after = debug::snapshot();
    if (find_waiter(after, t.id()) != nullptr)
      throw std::logic_error("case1: a landed task must leave the wait graph");
    if (has_live(after, "parked_waiter"))
      throw std::logic_error("case1: a terminal task must leave the live list");
  });

  // case 2: the age of a wait is simulation time — it advances with the simulation clock, not with
  // a wall clock, while the task stays parked
  run_case(__func__, "case2", [&] {
    Event gate, parked;
    auto t = create_task("aged_waiter", [&] {
      parked.set();
      gate.wait();  // No timeout leg: nothing but gate.set() can release it
    });
    parked.wait();
    sleep(2_ms);
    const debug::Snapshot snap = debug::snapshot();
    const debug::Waiter* w = find_waiter(snap, t.id());
    if (w == nullptr) throw std::logic_error("case2: the parked task must still be in the graph");
    if (w->since < 2_ms)
      throw std::logic_error("case2: the wait age must follow simulation time (>= 2ms)");
    gate.set();
    t.await();
  });

  // case 3: a task that has been created but has not run yet is live, not a waiter
  run_case(__func__, "case3", [&] {
    Event gate, started;
    auto t = create_task("never_started", [&] {
      started.set();
      gate.wait();
    });
    const debug::Snapshot snap = debug::snapshot();  // No suspension point crossed yet
    if (!has_live(snap, "never_started"))
      throw std::logic_error("case3: a PENDING task must be listed as live");
    if (find_waiter(snap, t.id()) != nullptr)
      throw std::logic_error("case3: a PENDING task is not blocked on anything yet");
    started.wait();
    gate.set();
    t.await();
  });

  // case 4: a non-Event suspension (the timeout leg of sleep) is reported as well, and dump()
  // prints without suspending or throwing, from a task and from a plain call
  run_case(__func__, "case4", [&] {
    Event parked;
    auto t = create_task("sleeping_waiter", [&] {
      parked.set();
      sleep(1_ms);  // Suspension on a timer leg, not on an Event waiter
    });
    parked.wait();
    const debug::Snapshot snap = debug::snapshot();
    const debug::Waiter* w = find_waiter(snap, t.id());
    if (w == nullptr) throw std::logic_error("case4: a sleeping task must appear in the graph");
    if (w->since >= 1_ms)
      throw std::logic_error("case4: a just-blocked task must not look expired yet");
    debug::dump("case4-blocked");  // Must print the waiter above and return
    t.await();
    debug::dump(nullptr);  // A null tag must fall back to the default prefix
    debug::dump("");       // Likewise an empty tag
    const debug::Snapshot after = debug::snapshot();
    if (find_waiter(after, t.id()) != nullptr)
      throw std::logic_error("case4: the woken task must have left the graph");
  });

  // case 5: a stall trips the watchdog, which reports the wait graph through on_fire, then finishes
  run_case(__func__, "case5", [&] {
    Event parked;
    Event gate;  // Never set until the end: this task is the stall the watchdog must notice
    auto stuck = create_task("stuck_waiter", [&] {
      parked.set();
      gate.wait();
    });
    parked.wait();
    bool fired = false;
    int reported = -1;
    Task w = debug::watchdog(1_ms, /*abort_on_fire=*/false, [&](const debug::Snapshot& snap) {
      fired = true;
      const debug::Waiter* waiter = find_waiter(snap, stuck.id());
      if (waiter != nullptr) reported = waiter->task_id;
    });
    sleep(2_ms);  // Longer than the idle window: nothing resumes in the first one
    if (!fired) throw std::logic_error("case5: a stalled system must trip the watchdog");
    if (reported != stuck.id())
      throw std::logic_error("case5: the report must name the stalled waiter");
    w.await();  // Fires once (no abort), so the watchdog task finishes on its own
    if (!w.done()) throw std::logic_error("case5: the watchdog must finish after firing");
    gate.set();
    stuck.await();
  });

  // case 6: steady progress never trips it, and the handle is a task, so cancel() stops it
  run_case(__func__, "case6", [&] {
    Event ticks_done;
    auto ticker = create_task("ticker", [&] {
      for (int i = 0; i < 6; ++i) sleep(500_us);  // A win: a resumption in every half window
      ticks_done.set();
    });
    bool fired = false;
    Task w = debug::watchdog(1_ms, /*abort_on_fire=*/false,
                             [&](const debug::Snapshot&) { fired = true; });
    ticks_done.wait();  // 3ms of steady progress
    if (fired) throw std::logic_error("case6: steady progress must not trip the watchdog");
    w.cancel();
    bool cancelled = false;
    try {
      w.await();
    } catch (const CancelledError&) {
      cancelled = true;
    }
    if (!cancelled) throw std::logic_error("case6: cancel() must stop the watchdog");
    ticker.await();
  });

  // case 7: abort_on_fire delivers a CancelledError into the stalled wait instead of printing only
  run_case(__func__, "case7", [&] {
    Event parked;
    Event aborted;  // Set by the victim when the injected CancelledError reaches it
    auto stuck = create_task("stuck_abort", [&] {
      parked.set();
      try {
        Event never;  // No setter: only the watchdog's abort can end this wait
        never.wait();
      } catch (const CancelledError&) {
        aborted.set();
      }
    });
    parked.wait();
    debug::watchdog(1_ms, /*abort_on_fire=*/true);
    // This task is live too, so the abort reaches it as well: absorb that injection, then wait for
    // the victim to report the one it received.
    try {
      aborted.wait();
    } catch (const CancelledError&) {
      aborted.wait();
    }
    stuck.await();  // The victim swallowed the cancellation, so it lands DONE
  });

  report(__func__);
}
