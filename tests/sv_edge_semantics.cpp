// sv::Edge semantic self-check: one named edge both languages fire and wait on. The local half
// (case2, case3) runs on both backends; the cross-language half runs only where a testbench claimed
// the names — tests/weave_tb.sv creates cl_request/cl_ack before the run starts
// (SystemVerilog-first binding: C++ adopts their ids) and adopts "cpp_first" after C++ registered
// it (C++-first binding). Each case prints [PASS]/[FAIL] through run_case, and report summarises at
// the end (see case_runner.hpp); the file is regressed by both weave_smoke_sc and
// weave_smoke_svdpi.

#include <stdexcept>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(sv_edge_semantics) {
  // case 1: round trip on a name the testbench created first — C++ adopts that id, a fire from C++
  // reaches the SystemVerilog waiter, and a fire from SystemVerilog reaches the C++ waiter
  run_case(__func__, "case1", [&] {
    sv::Edge request{"cl_request"};
    sv::Edge ack{"cl_ack"};
    if (!request.bound() || !ack.bound()) return;  // No testbench (the SystemC backend)
    const duration t0 = now();
    request.fire();  // Ask; the testbench answers on the same edge 10ns later
    request.wait();
    if (now() - t0 != 10_ns)
      throw std::logic_error("case1: the testbench must answer 10ns after the request");
    ack.fire();  // Reverse direction: a SystemVerilog wait() must wake and answer
    ack.wait();  // Its answer is what proves it woke
  });

  // case 2: the name is the binding — two Edge objects built from one name share the event, so a
  // fire() on one releases a wait() on the other
  run_case(__func__, "case2", [&] {
    sv::Edge fired{"local_edge"};
    sv::Edge waited{"local_edge"};
    Event parked;
    auto t = create_task("local_edge_waiter", [&] {
      parked.set();
      waited.wait();
    });
    parked.wait();  // The child has suspended in wait() (process scheduling barrier)
    fired.fire();
    t.await();
  });

  // case 3: edge semantics — a fire() with no waiter is gone, so a later wait() still blocks (the
  // cancellation is what proves it never returned, not a timeout)
  run_case(__func__, "case3", [&] {
    sv::Edge e{"unlatched"};
    e.fire();  // Nobody is waiting: the edge is gone
    Event parked;
    auto t = create_task("late_waiter", [&] {
      parked.set();
      e.wait();  // Must not return because of the earlier fire
    });
    parked.wait();
    t.cancel();
    bool cancelled = false;
    try {
      t.await();
    } catch (const CancelledError&) {
      cancelled = true;
    }
    if (!cancelled) throw std::logic_error("case3: a fire before the wait must not be remembered");
  });

  // case 4: C++ registers the name first, the testbench adopts that id instead of creating its own
  // (the gate is opened only after the C++ waiter is parked on the edge, so no edge can be lost)
  run_case(__func__, "case4", [&] {
    sv::Edge adopted{"cpp_first"};  // Registered under this name before the testbench hears of it
    sv::Edge gate{"probe_gate"};
    if (!gate.bound()) return;  // No testbench (the SystemC backend)
    Event armed;
    auto echo = create_task("cpp_first_echo", [&] {
      armed.set();
      adopted.wait();  // Blocks until the testbench adopts this id and fires it
    });
    armed.wait();  // The child has suspended in wait(); the testbench is still on the gate
    gate.fire();   // Let it adopt; it echoes on the same edge
    echo.await();
    if (!adopted.bound())
      throw std::logic_error("case4: the testbench must adopt the id C++ registered first");
  });

  // case 5: an Edge is a handle, so a copy is the *same* edge rather than a private impl — fire()
  // on the copy must release a wait() on the original (case2 covers the same thing for two
  // independent constructors; this one pins the copy path, which is what the pointer layout buys)
  run_case(__func__, "case5", [&] {
    sv::Edge original{"copied_edge"};
    sv::Edge copy = original;  // Trivially copyable handle: no name string, no allocation
    if (copy.bound() != original.bound())
      throw std::logic_error("case5: a copy must answer bound() from the shared entry");
    Event parked;
    auto t = create_task("copied_edge_waiter", [&] {
      parked.set();
      original.wait();
    });
    parked.wait();  // The child has suspended on the original (process scheduling barrier)
    copy.fire();    // The copy is the same edge
    t.await();
  });

  report(__func__);
}
