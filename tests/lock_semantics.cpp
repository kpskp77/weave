// Communication-primitive self-check: Lock's asyncio.Lock semantics — mutual exclusion, FIFO
// hand-off, release while not held raising, waiter cancellation (including not losing ownership
// when cancellation and hand-off share a slot), handle copies sharing the same lock, and
// LockGuard releasing on scope exit and on cancellation unwinding.
// Synchronisation is fully event-driven (pure delta cycles) and does not depend on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp); it shares the assertion bodies with the other *_semantics.cpp files and is
// regressed by both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <stdexcept>
#include <vector>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(lock_semantics) {
  // case 1: free lock → acquire takes it synchronously (no suspension, no yield), and locked()
  // follows the state
  run_case(__func__, "case1", [&] {
    Lock l;
    if (l.locked()) throw std::logic_error("case1: fresh Lock must be unlocked");
    Event started;
    auto probe = create_task("c1_probe", [&] { started.set(); });
    l.acquire();  // Free lock: the scheduler must not be yielded
    if (started.is_set()) throw std::logic_error("case1: acquire on free lock must not yield");
    if (!l.locked()) throw std::logic_error("case1: locked() must reflect acquire");
    l.release();
    if (l.locked()) throw std::logic_error("case1: locked() must reflect release");
    probe.await();  // Only now does probe get scheduled
    if (!started.is_set()) throw std::logic_error("case1: probe must run after yield");
  });

  // case 2: mutual exclusion — the holder stays in the critical section across a suspension point
  // (sleep(0) yields once), and a competitor must wait for release, so the entry depth stays 1
  run_case(__func__, "case2", [&] {
    Lock l;
    int depth = 0;
    int max_depth = 0;
    Event a_in;
    auto a = create_task("c2_a", [&] {
      l.acquire();
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      a_in.set();
      sleep(0_fs);  // Yield on purpose: give the competitor a chance to try acquire
      --depth;
      l.release();
    });
    auto b = create_task("c2_b", [&] {
      a_in.wait();
      l.acquire();  // a still holds the lock → blocks
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      --depth;
      l.release();
    });
    gather(a, b);  // Both tasks land (b only enters the critical section after a releases)
    if (max_depth != 1)
      throw std::logic_error("case2: two tasks entered the critical section at once");
  });

  // case 3: FIFO fairness — waiters acquire the lock in acquire() call order.
  // The queue order is fixed by chained events ("the previous waiter queues before the next one
  // starts"): r1.set() happens before acquire, so when r1.wait() returns t1 has necessarily
  // queued.
  run_case(__func__, "case3", [&] {
    Lock l;
    l.acquire();  // The main task takes it first to create a queue
    std::vector<int> order;
    Event r1, r2, r3;
    auto t1 = create_task("c3_1", [&] {
      r1.set();
      l.acquire();
      order.push_back(1);
      l.release();
    });
    auto t2 = create_task("c3_2", [&] {
      r1.wait();
      r2.set();
      l.acquire();
      order.push_back(2);
      l.release();
    });
    auto t3 = create_task("c3_3", [&] {
      r2.wait();
      r3.set();
      l.acquire();
      order.push_back(3);
      l.release();
    });
    r1.wait();    // t1 is at the head of the queue
    r2.wait();    // t2 is right behind it
    r3.wait();    // t3 is at the tail
    l.release();  // Hand off one by one
    gather(t1, t2, t3);
    if (order != std::vector<int>({1, 2, 3}))
      throw std::logic_error("case3: waiters must be served in FIFO order");
    if (l.locked()) throw std::logic_error("case3: lock must be free after last release");
  });

  // case 4: release() while not held → std::logic_error (≡ asyncio's RuntimeError)
  run_case(__func__, "case4", [&] {
    Lock l;
    bool threw = false;
    try {
      l.release();
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case4: release on unlocked Lock must throw");
    l.acquire();
    l.release();  // The normal path is unaffected
  });

  // case 5: a queued waiter is cancelled → it throws Cancelled and holds nothing; the lock stays
  // with its owner and goes to the remaining waiters
  run_case(__func__, "case5", [&] {
    Lock l;
    l.acquire();  // Held by the main task
    Event r1, r2, caught, got;
    auto victim = create_task("c5_victim", [&] {
      r1.set();
      try {
        l.acquire();
        throw std::logic_error("case5: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c5_peer", [&] {
      r1.wait();
      r2.set();
      l.acquire();  // Queues behind victim
      got.set();
      l.release();
    });
    r1.wait();        // victim has queued
    r2.wait();        // peer has queued behind victim
    victim.cancel();  // Cancel only victim, before any hand-off has happened
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    if (!l.locked()) throw std::logic_error("case5: lock must stay with its owner");
    if (got.is_set()) throw std::logic_error("case5: cancel must not hand off the lock");
    l.release();  // With victim unlinked, peer is at the head of the queue
    got.wait();
    peer.await();
    if (l.locked()) throw std::logic_error("case5: peer must have released the lock");
  });

  // case 6: cancellation and hand-off in the same slot — release() has already handed ownership to
  // victim, and the immediately following cancel() makes it receive Cancelled at the blocking point
  // (the backend cancel leg wins). Ownership must not be lost: it has to be handed to the next
  // waiter.
  run_case(__func__, "case6", [&] {
    Lock l;
    l.acquire();
    Event r1, r2, caught, got;
    auto victim = create_task("c6_victim", [&] {
      r1.set();
      try {
        l.acquire();
        throw std::logic_error("case6: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c6_peer", [&] {
      r1.wait();
      r2.set();
      l.acquire();
      got.set();
      l.release();
    });
    r1.wait();
    r2.wait();
    // There is no suspension point between release() and cancel(), so victim cannot be scheduled
    // between them: it is necessarily in the "already handed over but receiving a cancellation"
    // race shape (both backends give the cancel leg precedence).
    l.release();
    victim.cancel();
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    got.wait();  // The point: ownership was transferred to peer instead of vanishing with the
                 // cancellation
    peer.await();
    if (l.locked()) throw std::logic_error("case6: peer must have released after handoff");
  });

  // case 7: handle semantics — copies share the same lock (as with Event), so release may cross
  // handles
  run_case(__func__, "case7", [&] {
    Lock a;
    Lock b = a;
    a.acquire();
    if (!b.locked()) throw std::logic_error("case7: copies must share the same lock state");
    b.release();  // A lock records no owner, so any handle may release it
    if (a.locked()) throw std::logic_error("case7: release through a copy must unlock");
  });

  // case 8: LockGuard — acquires on construction, releases on destruction (the scope is the
  // critical section)
  run_case(__func__, "case8", [&] {
    Lock l;
    {
      LockGuard g(l);
      if (!l.locked()) throw std::logic_error("case8: guard must hold the lock");
    }
    if (l.locked()) throw std::logic_error("case8: guard must release at scope exit");
  });

  // case 9: LockGuard releases while unwinding a cancellation (≡ async with's __aexit__)
  run_case(__func__, "case9", [&] {
    Lock l;
    Event in_crit;
    auto holder = create_task("c9_holder", [&] {
      LockGuard g(l);
      in_crit.set();
      Event never;
      never.wait();  // Blocks while holding the lock, then gets cancelled
    });
    in_crit.wait();
    if (!l.locked()) throw std::logic_error("case9: guard must hold the lock");
    holder.cancel();
    try {
      holder.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    if (l.locked()) throw std::logic_error("case9: guard must release while unwinding");
    l.acquire();  // The lock really is available: acquiring again does not block
    l.release();
  });

  report(__func__);
}
