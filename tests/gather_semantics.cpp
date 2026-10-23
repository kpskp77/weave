// Weave C++ bookkeeping self-check: verifies gather's asyncio semantics on a real SystemC kernel
// (weave_smoke_sc). Synchronisation is fully event-driven (pure delta cycles) and does not depend
// on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp). The root task finishing normally means PASSED (see the reporting process in
// tests/sc_main.cpp).

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(gather_semantics) {
  // case 1: everything succeeds → gather returns normally
  run_case(__func__, "case1", [&] {
    Event sa, sb, sc;  // Tasks in place
    Event ga, gb, gc;  // Release
    auto a = create_task("ok_a", [&] {
      sa.set();
      ga.wait();
    });
    auto b = create_task("ok_b", [&] {
      sb.set();
      gb.wait();
    });
    auto c = create_task("ok_c", [&] {
      sc.set();
      gc.wait();
    });
    sa.wait();
    sb.wait();
    sc.wait();  // All three tasks are blocked on their own ga/gb/gc
    ga.set();
    gb.set();
    gc.set();
    gather(a, b, c);
    if (!a.done() || !b.done() || !c.done())
      throw std::logic_error("case1: gather returned before all peers done");
  });

  // case 2: the first failure (in completion order) is rethrown immediately; slow tasks are not
  // awaited and siblings are neither cancelled nor dragged down
  run_case(__func__, "case2", [&] {
    Event go_fast, go_slow;
    auto fast = create_task("fail_fast", [&] {
      go_fast.wait();
      throw std::runtime_error("fast");
    });
    auto slow = create_task("fail_slow", [&] {
      go_slow.wait();
      throw std::runtime_error("slow");
    });
    go_fast.set();  // The fast task fails first; the slow one is still blocked on go_slow
    try {
      gather(fast, slow);
      throw std::logic_error("case2: gather should rethrow");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "fast")
        throw std::logic_error("case2: first failure by completion order must win");
    }
    if (slow.done())
      throw std::logic_error("case2: gather must return at first failure, not wait for peers");
    if (slow.cancelled())
      throw std::logic_error("case2: sibling must not be cancelled on peer failure");
    // A sibling's late failure stays only in the task bookkeeping (no unretrieved warning, which
    // the MVP accepts); this also covers "an Event is set() again after gather returned early"
    go_slow.set();
    try {
      slow.await();
      throw std::logic_error("case2: slow should rethrow its late failure");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "slow") throw;
    }
  });

  // case 3: after gather returns early due to a failure, the sibling keeps running to natural
  // completion
  run_case(__func__, "case3", [&] {
    Event go_failer, go_peer;
    auto failer = create_task("failer3", [&] {
      go_failer.wait();
      throw std::runtime_error("boom3");
    });
    auto peer = create_task("peer3", [&] { go_peer.wait(); });
    go_failer.set();
    try {
      gather(failer, peer);
      throw std::logic_error("case3: gather should rethrow");
    } catch (const std::runtime_error&) {
      // Expected path
    }
    if (peer.done() || peer.cancelled())
      throw std::logic_error("case3: peer must keep running after gather failure");
    go_peer.set();
    peer.await();
    if (!peer.done() || peer.cancelled())
      throw std::logic_error("case3: peer should finish on its own");
  });

  // case 4: a cancelled child task counts as a failure → gather throws CancelledError
  run_case(__func__, "case4", [&] {
    Event block;
    auto victim = create_task("victim4", [&] { block.wait(); });
    [[maybe_unused]] auto killer = create_task("killer4", [&victim] { victim.cancel(); });
    try {
      gather(victim);
      throw std::logic_error("case4: gather should throw CancelledError");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (!victim.cancelled()) throw std::logic_error("case4: victim should be cancelled");
  });

  // case 5: gather itself is cancelled → cancel all unfinished tasks first, then propagate
  // CancelledError. The hand-off itself synchronises: when grp_ready.wait returns, grp has
  // necessarily registered its callbacks and is blocked inside gather.
  run_case(__func__, "case5", [&] {
    Event block;
    Event grp_ready;
    auto child = create_task("child5", [&] { block.wait(); });
    auto grp = create_task("grp5", [&] {
      grp_ready.set();
      try {
        gather(child);
        throw std::logic_error("case5: unreachable");
      } catch (const CancelledError&) {
        // Expected path: the outer cancellation propagates through gather
      }
    });
    grp_ready.wait();
    grp.cancel();
    grp.await();
    try {
      child.await();
      throw std::logic_error("case5: child should throw CancelledError");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (!child.cancelled())
      throw std::logic_error("case5: outer cancel of gather must cancel pending children");
    if (grp.cancelled())
      throw std::logic_error("case5: gatherer swallowed CancelledError and finished DONE");
  });

  // case 6: a task was already failed when gather is called → the synchronous callback path
  // rethrows immediately
  run_case(__func__, "case6", [&] {
    auto bad = create_task("bad6", [] { throw std::runtime_error("pre"); });
    try {
      bad.await();
      throw std::logic_error("case6: await should rethrow");
    } catch (const std::runtime_error&) {
      // Consume the exception so bad reaches its FAILED terminal state
    }
    Event started, go;
    auto good = create_task("good6", [&] {
      started.set();
      go.wait();
    });
    try {
      gather(bad, good);
      throw std::logic_error("case6: gather should rethrow pre-failed task");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "pre")
        throw std::logic_error("case6: wrong exception from pre-failed task");
    }
    started.wait();  // good has not started yet (gather took the synchronous path, no start gate
                     // was opened)
    go.set();
    good.await();
    if (!good.done()) throw std::logic_error("case6: good should finish on its own");
  });

  // case 7: empty task group → returns immediately
  run_case(__func__, "case7", [&] { gather(); });

  // case 8: return_exceptions form: everything succeeds → all nullptr
  run_case(__func__, "case8", [&] {
    auto a = create_task("re8_a", [] {});
    auto b = create_task("re8_b", [] {});
    auto res = gather({a, b}, return_exceptions);
    if (res.size() != 2) throw std::logic_error("case8: result count mismatch");
    if (res[0] || res[1]) throw std::logic_error("case8: all-success results must be nullptr");
  });

  // case 9: return_exceptions form: mixed terminal states, returning only after everything has
  // landed, mapped in input order
  //   DONE → nullptr; FAILED → its exception; CANCELLED → a wrapped CancelledError
  run_case(__func__, "case9", [&] {
    Event block;
    auto ok = create_task("re9_ok", [] {});
    auto bad = create_task("re9_bad", [] { throw std::runtime_error("re9_fail"); });
    auto victim = create_task("re9_victim", [&] { block.wait(); });
    auto killer = create_task("re9_killer", [&] { victim.cancel(); });
    auto res = gather({ok, bad, victim}, return_exceptions);  // No early throw
    if (res.size() != 3) throw std::logic_error("case9: result count mismatch");
    if (res[0]) throw std::logic_error("case9: DONE must map to nullptr");
    if (!res[1]) throw std::logic_error("case9: FAILED must map to its exception");
    try {
      std::rethrow_exception(res[1]);
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "re9_fail")
        throw std::logic_error("case9: wrong exception mapped");
    }
    if (!res[2]) throw std::logic_error("case9: CANCELLED must map to CancelledError");
    try {
      std::rethrow_exception(res[2]);
      throw std::logic_error("case9: unreachable");
    } catch (const CancelledError&) {
      // Expected path
    }
    // Returns only once everything has landed: victim is already terminal
    if (!victim.cancelled()) throw std::logic_error("case9: victim should be settled");
    killer.await();
  });

  // case 10: the return_exceptions form itself is cancelled → cancel the unfinished tasks and
  // propagate (same as the default form; nothing is collected into the result vector)
  run_case(__func__, "case10", [&] {
    Event block;
    Event grp_ready;
    auto child = create_task("re10_child", [&] { block.wait(); });
    auto grp = create_task("re10_grp", [&] {
      grp_ready.set();
      try {
        (void)gather({child}, return_exceptions);  // Expected to throw: the result is meaningless
        throw std::logic_error("case10: unreachable");
      } catch (const CancelledError&) {
        // Expected path
      }
    });
    grp_ready.wait();
    grp.cancel();
    grp.await();
    try {
      child.await();
      throw std::logic_error("case10: child should throw CancelledError");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (!child.cancelled())
      throw std::logic_error("case10: outer cancel must cancel pending children");
  });

  report(__func__);
}
