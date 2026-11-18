// Wait-graph diagnostics self-check: a blocked task reports its suspension point and how long it
// has been held there (in simulation time), a PENDING task is live but not a waiter, a landed task
// leaves the graph, and dump() is callable from a task and from outside any task without throwing
// (it reports through simlog, "weave.debug", so a run shows the graph in the same stream as the
// core trace). Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end
// (see case_runner.hpp). It shares the assertion-body style with task_semantics.cpp and is
// regressed by both the weave_smoke_sc and weave_smoke_svdpi backends.

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

  report(__func__);
}
