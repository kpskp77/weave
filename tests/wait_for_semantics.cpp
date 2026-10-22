// wait_for semantic self-check (asyncio.wait_for, 3.12+ timeout semantics):
// timeout → cancel the task, wait for the cancellation to complete, then throw Timeout; the task
// finishing first → returns normally; a task already failed at call time → the synchronous path
// rethrows immediately; a task refusing cancellation → Timeout is still thrown.
// Timing stays fully event-driven (the ready hand-off guarantees the task under test is blocked);
// the timeout duration is merely an input to the mechanism under test, never a way to express
// ordering between cases.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp). It shares the assertion-body style with gather_semantics.cpp and is regressed
// by both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(wait_for_semantics) {
  // case 1: the task finishes before the timeout → returns normally, task is DONE, no Timeout
  run_case(__func__, "case1", [&] {
    Event ready, gate;
    auto t = create_task("fast1", [&] {
      ready.set();
      gate.wait();
    });
    ready.wait();  // t is now blocked in gate.wait()
    gate.set();
    wait_for(t, 1_s);
    if (!t.done() || t.cancelled()) throw std::logic_error("case1: task state wrong");
  });

  // case 2: timeout → throws Timeout; the task is cancelled and the cancellation has completed
  // (await immediately yields a terminal state)
  run_case(__func__, "case2", [&] {
    Event ready;
    auto slow = create_task("slow2", [&] {
      ready.set();
      Event never;
      never.wait();
    });
    ready.wait();  // slow is blocked, so the timeout necessarily fires before it finishes
    try {
      wait_for(slow, 1_ns);
      throw std::logic_error("case2: wait_for must throw Timeout");
    } catch (const Timeout&) {
      // Expected path
    }
    if (!slow.done() || !slow.cancelled())
      throw std::logic_error("case2: timed-out task must be cancelled after wait_for returns");
    try {
      slow.await();
      throw std::logic_error("case2: await on timed-out task must throw");
    } catch (const Cancelled&) {
      // Expected path: the cancellation has propagated and completed
    }
  });

  // case 3: the task was already failed at call time → the synchronous path rethrows the original
  // exception immediately, regardless of duration
  run_case(__func__, "case3", [&] {
    auto bad = create_task("bad3", [] { throw std::runtime_error("pre3"); });
    try {
      bad.await();
      throw std::logic_error("case3: await should rethrow");
    } catch (const std::runtime_error&) {
      // Consume the exception so bad reaches its FAILED terminal state
    }
    try {
      wait_for(bad, 1_s);
      throw std::logic_error("case3: wait_for on failed task must rethrow");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "pre3")
        throw std::logic_error("case3: wrong exception from pre-failed task");
    }
  });

  // case 4: the task refuses cancellation (swallows Cancelled) → wait_for still throws Timeout,
  // but the task itself runs its body to the end → DONE (asyncio 3.12+: a timeout is not voided by
  // cancellation being refused)
  run_case(__func__, "case4", [&] {
    Event ready, finished;
    auto stubborn = create_task("stubborn4", [&] {
      ready.set();
      try {
        Event never;
        never.wait();
      } catch (const Cancelled&) {
        // Swallow the cancellation
      }
      finished.set();
    });
    ready.wait();
    try {
      wait_for(stubborn, 1_ns);
      throw std::logic_error("case4: wait_for must throw Timeout even if cancel is resisted");
    } catch (const Timeout&) {
      // Expected path
    }
    finished.wait();  // The body still runs to completion after refusing cancellation
    stubborn.await();
    if (!stubborn.done() || stubborn.cancelled())
      throw std::logic_error("case4: resisted-cancel task must end DONE");
  });

  // case 5: a non-positive timeout (duration arithmetic may go negative) → times out immediately
  // instead of taking the "no timeout leg" path: to_ticks clamps non-positive durations to 0 ticks,
  // since a negative tick means no timeout leg in the port wire format, and wait_for would then
  // wait until the task finishes on its own. Same semantics as asyncio 3.12+ (timeout<=0 cancels
  // immediately).
  run_case(__func__, "case5", [&] {
    Event ready;
    auto slow = create_task("slow5", [&] {
      ready.set();
      Event never;
      never.wait();
    });
    ready.wait();  // slow is blocked: a non-positive timeout necessarily fires first
    try {
      wait_for(slow, duration{} - 1_ns);
      throw std::logic_error("case5: non-positive timeout must throw Timeout");
    } catch (const Timeout&) {
      // Expected path
    }
    if (!slow.done() || !slow.cancelled())
      throw std::logic_error("case5: timed-out task must be cancelled after wait_for returns");
  });

  report(__func__);
}
