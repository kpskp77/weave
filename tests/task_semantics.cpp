// Task primitive semantic self-check: create_task scheduling timing, the various cancel paths
// (PENDING skips the body / injection at the blocking point / refusing cancellation), cancel
// idempotence on terminal tasks, handle attributes (valid/get_name), and sleep(0)'s zero-time
// yield (it must really hand control over, on every call — case6 only proves that a 0-tick wait
// returns, case10/case11 pin the hand-over itself).
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp). It shares the assertion-body style with gather_semantics.cpp and is regressed
// by both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(task_semantics) {
  // case 1: create_task only schedules, it does not run immediately — the child must not start
  // before the parent hits a blocking point
  run_case(__func__, "case1", [&] {
    Event started, gate;
    auto child = create_task("lazy", [&] {
      started.set();
      gate.wait();
    });
    if (started.is_set()) throw std::logic_error("case1: child must not run before parent blocks");
    started.wait();  // The child is scheduled only after the parent suspends
    if (!child.valid()) throw std::logic_error("case1: child handle must be valid");
    gate.set();
    child.await();
  });

  // case 2: cancelling a PENDING task → the body never runs and it goes straight to CANCELLED
  run_case(__func__, "case2", [&] {
    Event started, release;
    auto victim = create_task("pending_victim", [&] {
      started.set();  // Would certainly be set if it ever ran
      release.wait();
    });
    victim.cancel();  // victim has not been scheduled yet (PENDING)
    release.set();    // The parent yields so victim gets a chance to be scheduled
    sleep(0_fs);      // Yield once more to be sure the kernel has scheduled victim
    if (started.is_set()) throw std::logic_error("case2: PENDING cancel must skip body");
    try {
      victim.await();
      throw std::logic_error("case2: await on cancelled task must throw");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (!victim.cancelled()) throw std::logic_error("case2: victim should be CANCELLED");
  });

  // case 3: cancelling a blocked task → wake it immediately and inject CancelledError at the
  // blocking point
  run_case(__func__, "case3", [&] {
    Event ready, caught;
    auto victim = create_task("waiting_victim", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
        throw std::logic_error("case3: cancelled waiter must not return from wait()");
      } catch (const CancelledError&) {
        caught.set();
        throw;
      }
    });
    ready.wait();  // victim is now blocked in never.wait()
    victim.cancel();
    caught.wait();  // CancelledError has been injected at victim's blocking point
    try {
      victim.await();
      throw std::logic_error("case3: await must rethrow CancelledError");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (!victim.cancelled()) throw std::logic_error("case3: victim should be CANCELLED");
  });

  // case 4: a task may catch CancelledError to refuse cancellation → returning normally means DONE
  // (cooperative semantics)
  run_case(__func__, "case4", [&] {
    Event ready, finished;
    auto stubborn = create_task("stubborn", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
      } catch (const CancelledError&) {
        // Swallow the cancellation: refuse it
      }
      finished.set();  // Still runs through to a normal finish
    });
    ready.wait();
    stubborn.cancel();
    finished.wait();   // The body runs to completion after refusing cancellation
    stubborn.await();  // DONE: does not throw
    if (!stubborn.done() || stubborn.cancelled())
      throw std::logic_error("case4: swallowed cancel must end DONE, not CANCELLED");
  });

  // case 5: cancel is idempotent on terminal tasks (neither DONE nor CANCELLED is affected)
  run_case(__func__, "case5", [&] {
    auto done_task = create_task("done_t", [] {});
    done_task.await();
    done_task.cancel();  // Must not throw and must not change the terminal state
    if (!done_task.done() || done_task.cancelled())
      throw std::logic_error("case5: cancel on DONE task must be a no-op");
    if (done_task.get_name() != "done_t")
      throw std::logic_error("case5: get_name must return the create_task name");
  });

  // case 6: sleep(0) is a zero-time yield — no deadlock, the task progresses as usual (the child
  // sleeps 0 then sets the flag). Note: this only proves that the 0-tick wait returns; it does not
  // prove that it suspended (a sleep(0) implemented as a no-op passes it) — that is case10/case11.
  run_case(__func__, "case6", [&] {
    Event done;
    auto t = create_task("zero_sleep", [&] {
      sleep(0_fs);
      done.set();
    });
    done.wait();
    t.await();
    if (!t.done()) throw std::logic_error("case6: task after sleep(0) must complete");
  });

  // case 7: cancel(msg) matches asyncio Task.cancel(msg) (Python 3.9+) — the msg is visible in the
  // CancelledError injected at the blocking point and is preserved when rethrown at await.
  run_case(__func__, "case7", [&] {
    Event ready;
    auto victim = create_task("msg_victim", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
        throw std::logic_error("case7: cancelled waiter must not return");
      } catch (const CancelledError& e) {
        if (e.msg() != std::string_view{"shutdown"})
          throw std::logic_error("case7: CancelledError must carry cancel msg at injection point");
        throw;
      }
    });
    ready.wait();  // victim is now blocked in never.wait()
    victim.cancel("shutdown");
    try {
      victim.await();
      throw std::logic_error("case7: await must rethrow CancelledError");
    } catch (const CancelledError& e) {
      if (e.msg() != std::string_view{"shutdown"})
        throw std::logic_error("case7: await must rethrow CancelledError with original msg");
    }
    if (!victim.cancelled()) throw std::logic_error("case7: victim should be CANCELLED");
  });

  // case 8: cancel() without a msg → CancelledError::msg() is nullopt (corresponding to asyncio's
  // msg=None), and what() falls back to the default text.
  run_case(__func__, "case8", [&] {
    Event ready;
    auto victim = create_task("plain_victim", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
        throw std::logic_error("case8: unreachable");
      } catch (const CancelledError& e) {
        if (e.msg()) throw std::logic_error("case8: plain cancel must have no msg");
        throw;
      }
    });
    ready.wait();
    victim.cancel();
    try {
      victim.await();
      throw std::logic_error("case8: await must rethrow CancelledError");
    } catch (const CancelledError& e) {
      if (e.msg()) throw std::logic_error("case8: plain cancel rethrown must have no msg");
    }
  });

  // case 9: cancel leg and event leg in the same slot → cancellation wins (port contract
  // guarantee 4).
  // There is no suspension point between set() and cancel(), so victim cannot be scheduled and
  // both legs necessarily land in the same time slot; the backend must let the cancel leg win,
  // otherwise combinators would mistake "already cancelled" for a normal release.
  run_case(__func__, "case9", [&] {
    Event gate, ready;
    auto victim = create_task("same_slot_victim", [&] {
      ready.set();
      try {
        gate.wait();
        throw std::logic_error("case9: cancel must win over the event leg");
      } catch (const CancelledError&) {
        throw;  // Expected path: the cancellation is injected at the blocking point
      }
    });
    ready.wait();     // victim is now blocked in gate.wait()
    gate.set();       // The event leg
    victim.cancel();  // The same-slot cancel leg
    try {
      victim.await();
      throw std::logic_error("case9: await must rethrow CancelledError");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (!victim.cancelled()) throw std::logic_error("case9: victim must be CANCELLED");
  });

  // case 10: sleep(0) really hands control over, like asyncio.sleep(0) (which yields to the event
  // loop, so already-scheduled tasks run before the sleeper resumes). The child is merely
  // *scheduled* by create_task (case1: it must not run before the parent suspends), so it can have
  // run only if the sleeper really suspended — it is already finished when the sleeper resumes. A
  // sleep(0) that short-circuits out of the port (0 ticks is an already-expired timeout leg, so
  // returning without waiting is the tempting optimisation) leaves the flag false here.
  run_case(__func__, "case10", [&] {
    // shared_ptr, not a captured reference: on the failure path the child is still PENDING and
    // would resume after this frame dies, so a reference capture would be a use-after-free
    auto ran = std::make_shared<bool>(false);
    auto child = create_task("yielded_to", [ran] { *ran = true; });
    sleep(0_ns);
    if (!*ran)
      throw std::logic_error("case10: sleep(0) must yield so an already scheduled task runs first");
    child.await();  // Already terminal: returns without suspending
    if (!child.done()) throw std::logic_error("case10: the yielded-to task must be complete");
  });

  // case 11: the yield happens on *every* call, not only on the first one — each round schedules a
  // fresh observer and sleeps 0, and that round's observer must have run when the sleeper resumes
  // (an implementation whose zero-time yield is a one-shot state is caught from round 1 on).
  run_case(__func__, "case11", [&] {
    const int rounds = 3;
    auto ran = std::make_shared<int>(0);  // shared_ptr for the same reason as case10
    std::vector<Task> observers;
    for (int i = 0; i < rounds; ++i) {
      observers.push_back(create_task("round_observer", [ran] { ++*ran; }));
      sleep(0_ns);
      if (*ran != i + 1)
        throw std::logic_error("case11: every sleep(0) must yield, not only the first");
    }
    for (auto& t : observers) t.await();  // All terminal already; drains the handles
  });

  report(__func__);
}
