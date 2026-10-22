// Weave C++ bookkeeping self-check: verifies TaskGroup's asyncio.TaskGroup semantics on a real
// SystemC kernel (weave_smoke_sc). Synchronisation is fully event-driven (pure delta cycles) and
// does not depend on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp).

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(taskgroup_semantics) {
  // case 1: everything succeeds → wait returns normally and every child is finished
  run_case(__func__, "case1", [&] {
    Event ga, gb;
    TaskGroup tg;
    auto a = tg.create_task("tg1_a", [&] { ga.wait(); });
    auto b = tg.create_task("tg1_b", [&] { gb.wait(); });
    ga.set();
    gb.set();
    tg.wait();
    if (!a.done() || !b.done() || a.cancelled() || b.cancelled())
      throw std::logic_error("case1: all children must finish DONE");
  });

  // case 2: first failure → cancel siblings and rethrow the first exception after all have
  // landed
  run_case(__func__, "case2", [&] {
    Event block;
    TaskGroup tg;
    [[maybe_unused]] auto failer =
        tg.create_task("tg2_failer", [] { throw std::runtime_error("tg2_boom"); });
    auto victim = tg.create_task("tg2_victim", [&] { block.wait(); });
    try {
      tg.wait();
      throw std::logic_error("case2: unreachable");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "tg2_boom")
        throw std::logic_error("case2: first failure must be rethrown");
    }
    if (!victim.cancelled()) throw std::logic_error("case2: sibling must be cancelled on failure");
    if (!victim.done())
      throw std::logic_error("case2: siblings must be settled before wait returns");
  });

  // case 3: a child cancelled externally → tolerated, no sibling cancellation, wait returns
  // normally
  run_case(__func__, "case3", [&] {
    Event block, go;
    TaskGroup tg;
    auto victim = tg.create_task("tg3_victim", [&] { block.wait(); });
    auto good = tg.create_task("tg3_good", [&] { go.wait(); });
    auto killer = create_task("tg3_killer", [&] { victim.cancel(); });  // A task outside the group
    go.set();
    tg.wait();  // victim being cancelled is not a failure: returns normally
    if (!victim.cancelled()) throw std::logic_error("case3: victim should be cancelled");
    if (!good.done() || good.cancelled())
      throw std::logic_error("case3: external cancel must not abort siblings");
    killer.await();
  });

  // case 4: the caller of wait is cancelled → cascade-cancel every child and throw Cancelled once
  // they have landed
  run_case(__func__, "case4", [&] {
    Event block, ready;
    TaskGroup tg;  // waiter references tg/block from this frame; waiter.await() guarantees it
                   // finishes first
    auto c1 = tg.create_task("tg4_c1", [&] { block.wait(); });
    auto c2 = tg.create_task("tg4_c2", [&] { block.wait(); });
    auto waiter = create_task("tg4_waiter", [&] {
      ready.set();
      try {
        tg.wait();
        throw std::logic_error("case4: unreachable");
      } catch (const Cancelled&) {
        // Expected path: propagate after the cascade cancellation
      }
    });
    ready.wait();  // waiter is now blocked inside tg.wait() (the hand-off itself synchronises)
    waiter.cancel();
    waiter.await();
    if (waiter.cancelled())
      throw std::logic_error("case4: waiter swallowed Cancelled and finished DONE");
    if (!c1.cancelled() || !c2.cancelled())
      throw std::logic_error("case4: outer cancel must cascade to all children");
  });

  // case 5: create_task after wait / a second wait → logic_error; an empty group returns
  // immediately
  run_case(__func__, "case5", [&] {
    TaskGroup tg;
    tg.wait();  // Empty group: does not block, returns normally
    try {
      tg.create_task("tg5_x", [] {});
      throw std::logic_error("case5: unreachable");
    } catch (const std::logic_error& e) {
      if (std::string(e.what()).find("after wait") == std::string::npos) throw;
    }
    try {
      tg.wait();
      throw std::logic_error("case5: unreachable");
    } catch (const std::logic_error& e) {
      if (std::string(e.what()).find("twice") == std::string::npos) throw;
    }
  });

  // case 6: failures follow completion order, not creation order: a later-created task that fails
  // earlier wins
  run_case(__func__, "case6", [&] {
    Event gate;
    TaskGroup tg;
    auto late = tg.create_task("tg6_late", [&] {
      gate.wait();
      throw std::runtime_error("tg6_late");
    });
    [[maybe_unused]] auto early =
        tg.create_task("tg6_early", [] { throw std::runtime_error("tg6_early"); });
    try {
      tg.wait();
      throw std::logic_error("case6: unreachable");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "tg6_early")
        throw std::logic_error("case6: first failure by completion order must win");
    }
    if (!late.cancelled()) throw std::logic_error("case6: pending sibling must be cancelled");
  });

  // case 7: cancel() from outside the group (Python 3.15+) → all unfinished children are cancelled
  // externally, which is not a failure and does not trigger chain sibling cancellation; wait
  // returns normally
  run_case(__func__, "case7", [&] {
    Event block;
    TaskGroup tg;
    auto a = tg.create_task("tg7_a", [&] { block.wait(); });
    auto b = tg.create_task("tg7_b", [&] { block.wait(); });
    tg.cancel();  // Without a msg
    tg.wait();    // The external-cancellation tolerance path: returns normally
    if (!a.cancelled() || !b.cancelled())
      throw std::logic_error("case7: cancel() must cancel all unfinished children");
  });

  // case 8: cancel(msg): a child catching Cancelled can read the msg; already-DONE children are
  // unaffected
  run_case(__func__, "case8", [&] {
    Event block, ready;
    std::string seen;
    TaskGroup tg;
    auto done_child = tg.create_task("tg8_done", [] {});
    [[maybe_unused]] auto watcher = tg.create_task("tg8_watcher", [&] {
      ready.set();  // Reports having advanced to just before the blocking point
      try {
        block.wait();
      } catch (const Cancelled& c) {
        if (c.msg()) seen = std::string(*c.msg());
      }
    });
    // The event hand-off is a synchronisation barrier: ready.wait returning guarantees watcher is
    // blocked on block (done_child was created first, ran first, and is already DONE)
    ready.wait();
    tg.cancel("shutdown");
    tg.wait();
    if (seen != "shutdown")
      throw std::logic_error("case8: child must observe cancel msg from TaskGroup.cancel()");
    if (!done_child.done() || done_child.cancelled())
      throw std::logic_error("case8: already-DONE child must be untouched");
  });

  report(__func__);
}
