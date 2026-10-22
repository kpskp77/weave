// Weave C++ bookkeeping self-check: verifies wait's asyncio.wait semantics on a real SystemC
// kernel (weave_smoke_sc). Synchronisation is fully event-driven (pure delta cycles) and does not
// depend on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp).

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(wait_semantics) {
  // case 1: ALL_COMPLETED (the default): returns after everything finishes, done holds them all
  run_case(__func__, "case1", [&] {
    auto a = create_task("w1_a", [] {});
    auto b = create_task("w1_b", [] {});
    auto r = weave::wait({a, b});
    if (r.done.size() != 2 || !r.pending.empty())
      throw std::logic_error("case1: ALL_COMPLETED must return with all done");
    if (!a.done() || !b.done()) throw std::logic_error("case1: tasks must be done");
  });

  // case 2: FIRST_COMPLETED: returns on the first completion, other tasks are unaffected
  run_case(__func__, "case2", [&] {
    Event gate;
    auto a = create_task("w2_a", [] {});
    auto b = create_task("w2_b", [&] { gate.wait(); });
    auto r = weave::wait({a, b}, ReturnWhen::FIRST_COMPLETED);
    if (r.done.size() != 1 || r.pending.size() != 1)
      throw std::logic_error("case2: FIRST_COMPLETED must return after first completion");
    if (!a.done() || b.done()) throw std::logic_error("case2: done/pending grouping wrong");
    gate.set();
    b.await();  // b was not cancelled, it finishes naturally after being released
    if (b.cancelled()) throw std::logic_error("case2: wait must not cancel pending tasks");
  });

  // case 3: FIRST_EXCEPTION: only FAILED triggers an early return, CANCELLED does not count
  run_case(__func__, "case3", [&] {
    Event gate;
    auto victim = create_task("w3_victim", [&] { gate.wait(); });
    auto slow = create_task("w3_slow", [&] { gate.wait(); });
    auto failer = create_task("w3_failer", [] { throw std::runtime_error("w3"); });
    // killer and failer are scheduled in the same batch: victim's cancellation and failer's
    // failure happen almost simultaneously. If CANCELLED triggered FIRST_EXCEPTION, wait could
    // return before failer fails — asserting that failer is done proves it did not return early.
    auto killer = create_task("w3_killer", [&] { victim.cancel(); });
    auto r = weave::wait({victim, failer, slow}, ReturnWhen::FIRST_EXCEPTION);
    if (!failer.done())
      throw std::logic_error("case3: FIRST_EXCEPTION returned before any failure");
    // victim's cancellation and failer's failure land in the same slot, so whether victim ends up
    // in done or pending depends on the backend's notification order — its placement is not
    // asserted; only the deterministic parts are: failer is finished and slow is still pending.
    if (slow.done()) throw std::logic_error("case3: slow must still be pending");
    if (r.done.size() + r.pending.size() != 3)
      throw std::logic_error("case3: done/pending must partition all tasks");
    killer.await();
    try {
      victim.await();
      throw std::logic_error("case3: victim should throw Cancelled");
    } catch (const Cancelled&) {
      // Expected path
    }
    gate.set();
    slow.await();
  });

  // case 4: on timeout it returns the grouping at that moment, without cancelling any task
  run_case(__func__, "case4", [&] {
    Event gate;
    auto a = create_task("w4_a", [&] { gate.wait(); });
    auto b = create_task("w4_b", [&] { gate.wait(); });
    auto r = weave::wait({a, b}, ReturnWhen::ALL_COMPLETED, 1_ms);
    if (!r.done.empty() || r.pending.size() != 2)
      throw std::logic_error("case4: timeout must return current grouping");
    if (a.cancelled() || b.cancelled())
      throw std::logic_error("case4: timeout must not cancel tasks");
    gate.set();
    a.await();
    b.await();
  });

  // case 5: wait itself is cancelled → Cancelled propagates and the child tasks keep running
  run_case(__func__, "case5", [&] {
    Event block, ready;
    auto child = create_task("w5_child", [&] { block.wait(); });
    auto waiter = create_task("w5_waiter", [&] {
      ready.set();
      try {
        weave::wait({child});
        throw std::logic_error("case5: unreachable");
      } catch (const Cancelled&) {
        // Expected path
      }
    });
    ready.wait();  // waiter is now blocked inside wait (the hand-off itself synchronises)
    waiter.cancel();
    waiter.await();
    if (child.done()) throw std::logic_error("case5: wait cancellation must not affect children");
    block.set();
    child.await();
  });

  // case 6: timeout=0 is special-cased to a direct snapshot: no blocking, no yield
  run_case(__func__, "case6", [&] {
    Event gate;
    auto a = create_task("w6_a", [&] { gate.wait(); });
    auto r = weave::wait({a}, ReturnWhen::ALL_COMPLETED, duration{0});
    if (!r.done.empty() || r.pending.size() != 1)
      throw std::logic_error("case6: timeout=0 must snapshot immediately");
    if (a.done())  // The scheduler was never yielded to, so a has not run yet
      throw std::logic_error("case6: timeout=0 must not yield");
    gate.set();
    a.await();
  });

  // case 7: a task was already completed at registration → FIRST_COMPLETED returns synchronously
  run_case(__func__, "case7", [&] {
    Event gate;
    auto a = create_task("w7_a", [] {});
    a.await();  // a is already DONE
    auto b = create_task("w7_b", [&] { gate.wait(); });
    auto r = weave::wait({a, b}, ReturnWhen::FIRST_COMPLETED);
    if (r.done.size() != 1 || r.pending.size() != 1 || !r.done.front().done())
      throw std::logic_error("case7: pre-completed task must satisfy FIRST_COMPLETED");
    gate.set();
    b.await();
  });

  // case 8: empty task group → returns an empty grouping immediately
  run_case(__func__, "case8", [&] {
    auto r = weave::wait({});
    if (!r.done.empty() || !r.pending.empty())
      throw std::logic_error("case8: empty wait must return empty grouping");
  });

  // case 9: the timeout is expressed as optional<duration> (replacing the duration{-1} sentinel) —
  // nullopt means no timeout (covered by the default argument in case1-3); a negative duration no
  // longer means "no timeout" and is treated as a non-positive duration: a direct snapshot, no
  // blocking, no yield.
  run_case(__func__, "case9", [&] {
    Event gate;
    auto a = create_task("w9_a", [&] { gate.wait(); });
    auto r = weave::wait({a}, ReturnWhen::ALL_COMPLETED, duration{-1});
    if (!r.done.empty() || r.pending.size() != 1)
      throw std::logic_error("case9: negative timeout must snapshot immediately");
    if (a.done()) throw std::logic_error("case9: negative timeout must not yield");
    gate.set();
    a.await();
  });

  report(__func__);
}
