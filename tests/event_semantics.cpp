// Event semantic self-check: edge semantics (set snapshots the waiters, clear does not take back a
// release already decided), waking several waiters at once, and set() never touching an already
// destroyed wake event after a waiter was cancelled.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp). It shares the assertion-body style with gather_semantics.cpp and is regressed
// by both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <stdexcept>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
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

  // case 4: cancelled while waiting → Cancelled is injected at the blocking point; the wake event
  // has been unlinked from wake_evs and destroyed (required on both the success and cancellation
  // paths), and a later set() on the same Event must not trigger the destroyed wake event (SVDPI
  // dereferenced a null pointer here); a new waiter passes immediately because the flag is set
  run_case(__func__, "case4", [&] {
    Event ev, ready, caught, ok;
    auto w = create_task("cancel_w", [&] {
      ready.set();
      try {
        ev.wait();
        throw std::logic_error("case4: cancelled waiter must not return from wait()");
      } catch (const Cancelled&) {
        caught.set();  // Cancelled is injected inside ev.wait()
        throw;         // Keep propagating → the task is CANCELLED
      }
    });
    ready.wait();  // w is blocked in ev.wait()
    w.cancel();
    caught.wait();  // Cancelled has been injected (w was woken and advanced past the block)
    try {
      w.await();
      throw std::logic_error("case4: await on cancelled task must throw");
    } catch (const Cancelled&) {
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

  report(__func__);
}
