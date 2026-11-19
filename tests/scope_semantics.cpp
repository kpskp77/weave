// Task identity self-check (port::owner_task): which task a suspension belongs to. The identity is
// held by the backend per simulation process, so these cases pin down exactly what a core-side
// scalar could not: a task that parks keeps its own identity while another task's slice runs around
// it, and an interruption arriving at the blocking point (the cancel leg of a timeout) still leaves
// the waiting task as the one executing afterwards. Ordering stays fully event-driven — the ready
// hand-off (a task parks immediately after setting an Event) is what guarantees the other side is
// blocked, and no duration ever expresses ordering. Each case prints [PASS]/[FAIL] through
// run_case, and report summarises at the end (see case_runner.hpp); the same assertion bodies are
// regressed by both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

namespace {

// -1 is what a backend reports for "no task body"; observing it inside a running body is a bug.
int id_now() {
  const std::optional<Task> t = current_task();
  return t ? t->id() : -1;
}

}  // namespace

WEAVE_TEST(scope_semantics) {
  // case 1: a parked task reads back its own id at every blocking point, although the parent's
  // slice ran in between (it was woken by the trigger while this task was still parked)
  run_case(__func__, "case1", [&] {
    Event started, gate;
    std::vector<int> seen;
    auto t = create_task("ident1", [&] {
      started.set();
      gate.wait();
      seen.push_back(id_now());
      sleep(1_ns);
      seen.push_back(id_now());
    });
    started.wait();  // t set `started` before parking, so it is now blocked in gate.wait()
    gate.set();
    t.await();
    if (seen.size() != 2) throw std::logic_error("case1: not both blocking points observed");
    if (seen[0] != t.id() || seen[1] != t.id())
      throw std::logic_error("case1: a suspension was attributed to another task");
  });

  // case 2: the timeout leg interrupts a *child* at its blocking point; the task that called
  // wait_for is still itself once the TimeoutError has been delivered
  run_case(__func__, "case2", [&] {
    Event ready;
    std::optional<int> seen;
    auto t = create_task("ident2", [&] {
      Event never;  // only this body's frame owns it; the cancellation unwinds through it
      auto child = create_task("ident2_child", [&] {
        ready.set();
        never.wait();
      });
      ready.wait();  // child is blocked in never.wait()
      try {
        wait_for(child, 1_ns);
        throw std::logic_error("case2: wait_for did not time out");
      } catch (const TimeoutError&) {
      }
      seen = id_now();
    });
    t.await();  // wait_for waited for the child's landing, so child.unwind precedes this
    if (seen != std::optional<int>(t.id()))
      throw std::logic_error("case2: identity lost across the timeout leg");
  });

  report(__func__);
}
