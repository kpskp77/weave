// Weave C++ bookkeeping self-check: verifies as_completed's asyncio.as_completed semantics on a
// real SystemC kernel (weave_smoke_sc). Synchronisation is fully event-driven (pure delta cycles)
// and does not depend on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp).

#include <stdexcept>
#include <string>
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

  report(__func__);
}
