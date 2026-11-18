// Event / Pulse semantic self-check: edge semantics (set snapshots the waiters, clear does not take
// back a release already decided), waking several waiters at once, set() never touching an already
// destroyed wake event after a waiter was cancelled, and — for Pulse — that a fire nobody waited
// for is not remembered (case5), that one fire wakes every current waiter (case6), that a cancelled
// Pulse waiter unlinks its wake event (case7), that generation() is the caller's reconciliation
// handle (case8), and that a cross-language handle (case9) is one edge both languages fire and wait
// on — with the testbench half only running where a testbench handed an id in.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp). It shares the assertion-body style with gather_semantics.cpp and is regressed
// by both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <cstdint>
#include <stdexcept>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(event_semantics) {
  // case 1: basic is_set/clear semantics; Event::wait returns immediately when already set
  run_case(__func__, "case1", [&] {
    Event e;
    if (e.is_set()) throw std::logic_error("case1: fresh Event must be unset");
    e.set();
    if (!e.is_set()) throw std::logic_error("case1: is_set must reflect set()");
    e.wait();  // Already set: returns immediately, no suspension
    e.clear();
    if (e.is_set()) throw std::logic_error("case1: is_set must reflect clear()");
  });

  // case 2: edge semantics — a release decided by set() is not taken back by clear(); this also
  // verifies "the waiter returning from the block ⇒ the triggerer has advanced past the blocking
  // point" (lock hand-off)
  run_case(__func__, "case2", [&] {
    Event ev, ready, proceeded;
    auto w = create_task("edge_w", [&] {
      ready.set();
      ev.wait();
      proceeded.set();  // ev was set then cleared, yet we must still get here
    });
    ready.wait();  // w is necessarily blocked in ev.wait() at this point
    ev.set();
    ev.clear();  // The flag is reset, but w was already released by set()'s snapshot
    proceeded.wait();
    w.await();
    if (!w.done() || w.cancelled()) throw std::logic_error("case2: waiter state wrong");
  });

  // case 3: a single set() wakes every current waiter
  run_case(__func__, "case3", [&] {
    Event ev, r1, r2, d1, d2;
    auto w1 = create_task("multi_w1", [&] {
      r1.set();
      ev.wait();
      d1.set();
    });
    auto w2 = create_task("multi_w2", [&] {
      r2.set();
      ev.wait();
      d2.set();
    });
    r1.wait();  // Both waiters are in place
    r2.wait();
    ev.set();  // A single set releases both
    d1.wait();
    d2.wait();
    gather(w1, w2);
  });

  // case 4: cancelled while waiting → CancelledError is injected at the blocking point; the wake
  // event has been unlinked from wake_evs and destroyed (required on both the success and
  // cancellation paths), and a later set() on the same Event must not trigger the destroyed wake
  // event (SVDPI dereferenced a null pointer here); a new waiter passes immediately because the
  // flag is set
  run_case(__func__, "case4", [&] {
    Event ev, ready, caught, ok;
    auto w = create_task("cancel_w", [&] {
      ready.set();
      try {
        ev.wait();
        throw std::logic_error("case4: cancelled waiter must not return from wait()");
      } catch (const CancelledError&) {
        caught.set();  // CancelledError is injected inside ev.wait()
        throw;         // Keep propagating → the task is CANCELLED
      }
    });
    ready.wait();  // w is blocked in ev.wait()
    w.cancel();
    caught.wait();  // CancelledError has been injected (w was woken and advanced past the block)
    try {
      w.await();
      throw std::logic_error("case4: await on cancelled task must throw");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (!w.cancelled()) throw std::logic_error("case4: w should be CANCELLED");
    // w's wake event is destroyed; this set must not trigger it (a null dereference really
    // happened here once)
    ev.set();
    auto w2 = create_task("after_w", [&] {
      ev.wait();  // The flag is set: passes immediately
      ok.set();
    });
    ok.wait();
    gather(w2);
  });

  // case 5: Pulse is edge-only — a fire() that happened before the wait() is not stored, so the
  // waiter blocks until the *next* one (this is exactly the silent false positive a sticky Event
  // would produce when an SV `-> ev` is mapped onto it)
  run_case(__func__, "case5", [&] {
    Pulse p;
    if (p.generation() != 0) throw std::logic_error("case5: a fresh Pulse must have generation 0");
    p.fire();  // Nobody is waiting: the edge is gone
    if (p.generation() != 1) throw std::logic_error("case5: fire must advance generation");
    Event started, released;
    auto late = create_task("late_waiter", [&] {
      started.set();
      p.wait();  // Must not return because of the earlier fire
      released.set();
    });
    started.wait();  // The child has suspended in p.wait() (process scheduling barrier)
    if (released.is_set())
      throw std::logic_error("case5: a fire before the wait must not be remembered");
    p.fire();
    released.wait();
    late.await();
    if (p.generation() != 2) throw std::logic_error("case5: generation must count every fire");
  });

  // case 6: one fire() wakes every waiter present at that instant
  run_case(__func__, "case6", [&] {
    Pulse p;
    Event queued_a, queued_b;
    int woken = 0;
    auto a = create_task("pulse_a", [&] {
      queued_a.set();
      p.wait();
      ++woken;
    });
    auto b = create_task("pulse_b", [&] {
      queued_b.set();
      p.wait();
      ++woken;
    });
    queued_a.wait();
    queued_b.wait();
    if (woken != 0) throw std::logic_error("case6: nothing may wake before the fire");
    p.fire();
    gather(a, b);
    if (woken != 2) throw std::logic_error("case6: one fire must wake every current waiter");
  });

  // case 7: a waiter cancelled inside Pulse::wait unlinks and destroys its wake event, so the next
  // fire never touches a dead id (the same invariant Event::wait carries)
  run_case(__func__, "case7", [&] {
    Pulse p;
    Event queued;
    auto t = create_task("cancelled_pulse_waiter", [&] {
      queued.set();
      p.wait();
    });
    queued.wait();
    t.cancel();
    bool cancelled = false;
    try {
      t.await();
    } catch (const CancelledError&) {
      cancelled = true;
    }
    if (!cancelled) throw std::logic_error("case7: the waiter must land CANCELLED");
    p.fire();  // Must not trigger the destroyed wake event
    if (p.generation() != 1) throw std::logic_error("case7: fire must still count");
  });

  // case 8: generation() is the reconciliation handle — copies share the pulse, and a waiter
  // released by a fire observes the count already advanced
  run_case(__func__, "case8", [&] {
    Pulse p;
    Pulse alias = p;  // Copies share the same pulse, like Event
    const auto before = alias.generation();
    Event started, released;
    std::uint64_t seen = 0;
    auto t = create_task("generation_observer", [&] {
      started.set();
      alias.wait();
      seen = p.generation();
      released.set();
    });
    started.wait();
    p.fire();
    released.wait();
    t.await();
    if (seen != before + 1)
      throw std::logic_error("case8: the released waiter must observe the advanced generation");
    if (alias.generation() != seen)
      throw std::logic_error("case8: Pulse copies must share their state");
  });

  // case 9: cross-language handle — an id both languages speak, so a C++ fire wakes a SystemVerilog
  // process and a SystemVerilog trigger wakes a C++ wait (tests/weave_tb.sv hands two ids in)
  run_case(__func__, "case9", [&] {
    const sv::EventHandle request = sv::handed(0);
    const sv::EventHandle ack = sv::handed(1);
    if (request && ack) {
      const duration t0 = now();
      sv::fire(request);  // Ask; the testbench answers on the same id 10ns later
      sv::wait(request);
      if (now() - t0 != 10_ns)
        throw std::logic_error("case9: the testbench must answer 10ns after the request");
      sv::fire(ack);  // Reverse direction: a SystemVerilog @(ev_table[id]) must wake and answer
      sv::wait(ack);  // Its answer is what proves it woke
    }
    // The half that needs no testbench, so it also covers the SystemC backend: one edge, one owner
    const sv::EventHandle own = sv::make_event();
    Event parked;
    auto t = create_task("handle_waiter", [&] {
      parked.set();
      sv::wait(own);
    });
    parked.wait();
    sv::fire(own);
    t.await();
    sv::destroy(own);
    bool rejected = false;
    try {
      sv::fire(sv::EventHandle{});
    } catch (const std::logic_error&) {
      rejected = true;
    }
    if (!rejected) throw std::logic_error("case9: an invalid handle must be rejected");
  });

  report(__func__);
}
