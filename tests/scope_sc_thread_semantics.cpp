// SystemC-only self-check (src/backend/systemc.cpp): task identity is per *process*, so a plain
// SC_THREAD that suspends while a task body is parked must see no identity at all.
//
// This is the one assertion body without an SVDPI twin: the foreign process here is a raw
// sc_spawn thread, created by the test itself, while on the SVDPI side the equivalent actor is an
// SV `initial`/package process that can only be built in the testbench (tests/weave_tb.sv), not in
// C++. It is therefore linked into weave_smoke_sc alone (see the file lists in xmake.lua).
//
// What goes wrong without the per-process key: the parked task's identity leaks to the foreign
// suspension, which then writes its wait into that task's record (state / wait_ev / cancel leg) —
// the silent mis-attribution described in bugs.md.

#include <optional>
#include <stdexcept>
#include <systemc>
#include <utility>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

namespace {

int owner_id() {
  const std::optional<Task> t = current_task();
  return t ? t->id() : -1;
}

}  // namespace

WEAVE_TEST(scope_sc_thread_semantics) {
  run_case(__func__, "case1", [&] {
    Event parked;
    Event release;
    Event checked;
    int before = -7;
    int after = -7;
    int task_after = -7;

    Task t = create_task("sc_foreign", [&] {
      parked.set();
      release.wait();
      task_after = owner_id();
    });
    parked.wait();  // the task now sits inside release.wait(), holding its identity

    // A process that is not a weave task (the SystemC analogue of an SV `initial`): it suspends
    // while the task above is parked.
    sc_core::sc_spawn([&] {
      before = owner_id();
      sleep(1_ns);
      after = owner_id();
      checked.set();
    });
    checked.wait();  // the spawned process returned: ordering is carried by events only

    if (before != -1 || after != -1)
      throw std::logic_error("case1: a non-task process inherited a parked task's identity");

    // The foreign suspension must also leave the parked task's own entry alone.
    release.set();
    t.await();
    if (task_after != t.id())
      throw std::logic_error("case1: the parked task lost its identity to the foreign suspension");
  });
  report(__func__);
}
