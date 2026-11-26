// Weave C++ bookkeeping self-check: verifies as_completed's asyncio.as_completed semantics on a
// real SystemC kernel (weave_smoke_sc). Synchronisation is fully event-driven (pure delta cycles)
// and does not depend on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp).

#include <stdexcept>
#include <string>
#include <vector>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(as_completed_semantics) {
  // case 1: yields in completion order (independent of creation order)
  run_case(__func__, "case1", [&] {
    Event g1, g2, g3;
    auto a = create_task("ac1_a", [&] { g1.wait(); });
    auto b = create_task("ac1_b", [&] { g2.wait(); });
    auto c = create_task("ac1_c", [&] { g3.wait(); });
    auto ac = as_completed({a, b, c});
    g3.set();  // Release only c: it completes while next() is blocking
    if (ac.next().get_name() != "ac1_c")
      throw std::logic_error("case1: first completed must come first");
    g1.set();
    if (ac.next().get_name() != "ac1_a")
      throw std::logic_error("case1: completion order must be c, a, b");
    g2.set();
    if (ac.next().get_name() != "ac1_b")
      throw std::logic_error("case1: completion order must be c, a, b");
    if (ac.pending()) throw std::logic_error("case1: all tasks produced");
  });

  // case 2: after everything is produced next() throws logic_error; pending() is false
  run_case(__func__, "case2", [&] {
    auto a = create_task("ac2_a", [] {});
    auto ac = as_completed({a});
    ac.next().await();
    if (ac.pending()) throw std::logic_error("case2: pending must be false at end");
    try {
      (void)ac.next();  // Expected to throw: the return value is meaningless
      throw std::logic_error("case2: unreachable");
    } catch (const std::logic_error& e) {
      if (std::string(e.what()).find("no more tasks") == std::string::npos) throw;
    }
  });

  // case 3: a failed task is produced as usual, and await rethrows its exception
  run_case(__func__, "case3", [&] {
    auto bad = create_task("ac3_bad", [] { throw std::runtime_error("ac3_fail"); });
    auto good = create_task("ac3_good", [] {});
    auto ac = as_completed({bad, good});
    auto first = ac.next();  // Both complete at delta 0, and bad was spawned first
    try {
      first.await();
      throw std::logic_error("case3: unreachable");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "ac3_fail")
        throw std::logic_error("case3: failure must surface via await");
    }
    ac.next().await();
    if (ac.pending()) throw std::logic_error("case3: all tasks produced");
  });

  // case 4: the consumer is cancelled inside next() → only that next() is interrupted, the queue
  // and the child tasks stay intact
  run_case(__func__, "case4", [&] {
    Event block, ready;
    auto child = create_task("ac4_child", [&] { block.wait(); });
    auto consumer = create_task("ac4_consumer", [&] {
      auto ac = as_completed({child});
      ready.set();
      try {
        (void)ac.next();  // Expected to throw: the return value is meaningless
        throw std::logic_error("case4: unreachable");
      } catch (const CancelledError&) {
        // Expected path: the cancellation only interrupts this next() call
      }
      if (!ac.pending()) throw std::logic_error("case4: state must survive cancellation");
      block.set();
      ac.next().await();  // Retrieved normally after the release
      if (ac.pending()) throw std::logic_error("case4: all tasks produced");
    });
    ready.wait();  // consumer is now blocked inside next() (the hand-off itself synchronises)
    consumer.cancel();
    consumer.await();
    if (consumer.cancelled())
      throw std::logic_error("case4: consumer swallowed CancelledError and finished DONE");
    if (!child.done() || child.cancelled())
      throw std::logic_error("case4: child must not be affected by consumer cancel");
  });

  // case 5: tasks already completed at registration (FAILED too) are produced immediately, without
  // blocking
  run_case(__func__, "case5", [&] {
    auto a = create_task("ac5_a", [] {});
    a.await();  // a is already DONE
    Event gate;
    auto b = create_task("ac5_b", [&] { gate.wait(); });
    auto ac = as_completed({a, b});
    if (ac.next().get_name() != "ac5_a")
      throw std::logic_error("case5: pre-completed task must be produced first");
    gate.set();
    ac.next().await();
    if (ac.pending()) throw std::logic_error("case5: all tasks produced");
  });

  // case 6: empty task set → pending() is false immediately and next() throws logic_error
  run_case(__func__, "case6", [&] {
    auto ac = as_completed({});
    if (ac.pending()) throw std::logic_error("case6: empty set must not be pending");
    try {
      (void)ac.next();  // Expected to throw: the return value is meaningless
      throw std::logic_error("case6: unreachable");
    } catch (const std::logic_error& e) {
      if (std::string(e.what()).find("no more tasks") == std::string::npos) throw;
    }
  });

  // case 7: one raw wake event reused by consecutive waits inside a single time slot.
  // as_completed() creates its wake event once and every next() re-waits on that same id, so the
  // releases and the next() calls below all happen at the same simulation time (nothing in this
  // case sleeps). This is the canonical shape a backend must support: each wait may only be
  // released by a trigger that happens *after* it started waiting. A backend that reports an older
  // trigger of the reused id as a fresh hit (the SV `triggered` flag is sticky for the whole time
  // slot) makes next() spin in a zero-delay loop here instead of returning — simv then sits at 100%
  // CPU with $time frozen, which is exactly the SVDPI hang this case guards against.
  run_case(__func__, "case7", [&] {
    constexpr int kTasks = 5;
    // Each task captures its own gate *by value* (Event is a shared handle), so a failure below can
    // throw without leaving a pending task pointing at this frame.
    Event gates[kTasks];
    std::vector<Task> tasks;
    tasks.reserve(kTasks);
    for (int i = 0; i < kTasks; ++i)
      tasks.push_back(
          create_task("ac7_" + std::to_string(i), [gate = gates[i]]() mutable { gate.wait(); }));
    auto ac = as_completed({tasks[0], tasks[1], tasks[2], tasks[3], tasks[4]});
    // Release one task at a time, alternating with next(): every next() blocks on the same reused
    // wake event and must be released only by the completion of the task released just before.
    const int order[kTasks] = {2, 4, 1, 3, 0};
    for (const int idx : order) {
      gates[idx].set();
      const Task produced = ac.next();
      if (produced.get_name() != "ac7_" + std::to_string(idx))
        throw std::logic_error("case7: production order must follow the release order");
    }
    if (ac.pending()) throw std::logic_error("case7: all tasks must have been produced");
  });

  report(__func__);
}
