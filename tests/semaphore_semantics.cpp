// Communication-primitive self-check: the asyncio semantics of Semaphore / BoundedSemaphore —
// taking a permit (no suspension, no yield), FIFO queueing once the count is exhausted, N permits
// allowing N concurrent holders, release waking waiters one at a time, waiter cancellation
// (including not losing the permit when cancellation and release share a slot), BoundedSemaphore's
// bound check (including not misjudging the handoff return path), handle copies sharing state, and
// the guard releasing on scope exit and on cancellation unwinding.
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

WEAVE_TEST(semaphore_semantics) {
  // case 1: taking a permit — 1 by default, N constructed explicitly; with a free count, acquire
  // takes it synchronously (no suspension, no yield), and locked() follows the remaining count
  run_case(__func__, "case1", [&] {
    Semaphore dflt;  // 1 permit by default
    if (dflt.locked()) throw std::logic_error("case1: fresh Semaphore must be unlocked");
    Semaphore zero(0);  // 0 permits is legal: the first acquire must queue
    if (!zero.locked()) throw std::logic_error("case1: Semaphore(0) must start locked");
    Semaphore s(2);
    Event started;
    auto probe = create_task("c1_probe", [&] { started.set(); });
    s.acquire();
    if (started.is_set()) throw std::logic_error("case1: acquire with permits must not yield");
    s.acquire();
    if (!s.locked()) throw std::logic_error("case1: locked() must reflect exhausted permits");
    probe.await();  // Only now does probe get scheduled
    if (!started.is_set()) throw std::logic_error("case1: probe must run after yield");
  });

  // case 2: counting exclusion — 2 permits let two tasks be in the critical section at once, and
  // the third must wait to be released, so the entry depth stays 2
  run_case(__func__, "case2", [&] {
    Semaphore s(2);
    int depth = 0;
    int max_depth = 0;
    Event a_in;
    auto a = create_task("c2_a", [&] {
      s.acquire();
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      a_in.set();
      sleep(0_fs);  // Yield on purpose: give the competitor a chance to try acquire
      --depth;
      s.release();
    });
    auto b = create_task("c2_b", [&] {
      a_in.wait();
      s.acquire();
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      sleep(0_fs);
      --depth;
      s.release();
    });
    auto c = create_task("c2_c", [&] {
      a_in.wait();
      s.acquire();  // a and b each hold a permit → blocks until one of them releases
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      --depth;
      s.release();
    });
    gather(a, b, c);
    if (max_depth != 2) throw std::logic_error("case2: three tasks entered with only two permits");
  });

  // case 3: FIFO fairness — waiters acquire permits in acquire() call order.
  // The queue order is fixed by chained events ("the previous waiter queues before the next one
  // starts"): r1.set() happens before acquire, so when r1.wait() returns t1 has necessarily
  // queued.
  run_case(__func__, "case3", [&] {
    Semaphore s(1);
    s.acquire();  // The main task holds the only permit to create a queue
    std::vector<int> order;
    Event r1, r2, r3;
    auto t1 = create_task("c3_1", [&] {
      r1.set();
      s.acquire();
      order.push_back(1);
      s.release();
    });
    auto t2 = create_task("c3_2", [&] {
      r1.wait();
      r2.set();
      s.acquire();
      order.push_back(2);
      s.release();
    });
    auto t3 = create_task("c3_3", [&] {
      r2.wait();
      r3.set();
      s.acquire();
      order.push_back(3);
      s.release();
    });
    r1.wait();    // t1 is at the head of the queue
    r2.wait();    // t2 is right behind it
    r3.wait();    // t3 is at the tail
    s.release();  // Release one by one
    gather(t1, t2, t3);
    if (order != std::vector<int>({1, 2, 3}))
      throw std::logic_error("case3: waiters must be served in FIFO order");
    if (s.locked()) throw std::logic_error("case3: semaphore must be free after last release");
  });

  // case 4: every release wakes exactly one waiter — after being released and before it releases
  // on its own, the next waiter is not woken (the proceed event pins t1 in the "acquired but not
  // released" interval, making the check deterministic under event-driven synchronisation)
  run_case(__func__, "case4", [&] {
    Semaphore s(1);
    s.acquire();  // The main task holds the only permit
    Event r1, r2, got1, got2, proceed;
    auto t1 = create_task("c4_1", [&] {
      r1.set();
      s.acquire();  // Released by the main task's release
      got1.set();
      proceed.wait();  // Pinned here: the permit is acquired but not released yet
      s.release();
    });
    auto t2 = create_task("c4_2", [&] {
      r1.wait();
      r2.set();
      s.acquire();  // Queues behind t1
      got2.set();
      s.release();
    });
    r1.wait();  // t1 has queued
    r2.wait();  // t2 has queued behind t1
    s.release();
    got1.wait();  // t1 has acquired the permit
    if (got2.is_set()) throw std::logic_error("case4: one release must wake exactly one waiter");
    proceed.set();  // Let t1 release → which releases t2 in turn
    got2.wait();
    gather(t1, t2);
  });

  // case 5: a queued waiter is cancelled → it throws Cancelled and holds no permit; the permit is
  // still handed to the remaining waiters by the main task's release
  run_case(__func__, "case5", [&] {
    Semaphore s(1);
    s.acquire();  // The main task holds the only permit
    Event r1, r2, caught, got;
    auto victim = create_task("c5_victim", [&] {
      r1.set();
      try {
        s.acquire();
        throw std::logic_error("case5: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c5_peer", [&] {
      r1.wait();
      r2.set();
      s.acquire();  // Queues behind victim
      got.set();
      s.release();
    });
    r1.wait();        // victim has queued
    r2.wait();        // peer has queued behind victim
    victim.cancel();  // Cancel only victim, before any release has happened
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    if (got.is_set()) throw std::logic_error("case5: cancel must not release a permit");
    s.release();  // With victim unlinked, peer is at the head of the queue
    got.wait();
    peer.await();
    if (s.locked()) throw std::logic_error("case5: peer must have released the permit");
  });

  // case 6: cancellation and release in the same slot — release() has already handed the permit to
  // victim, and the immediately following cancel() makes it receive Cancelled at the blocking point
  // (the backend cancel leg wins). The permit must not be lost: it has to be handed to the next
  // waiter.
  run_case(__func__, "case6", [&] {
    Semaphore s(1);
    s.acquire();
    Event r1, r2, caught, got;
    auto victim = create_task("c6_victim", [&] {
      r1.set();
      try {
        s.acquire();
        throw std::logic_error("case6: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c6_peer", [&] {
      r1.wait();
      r2.set();
      s.acquire();
      got.set();
      s.release();
    });
    r1.wait();
    r2.wait();
    // There is no suspension point between release() and cancel(), so victim cannot be scheduled
    // between them: it is necessarily in the "already released but receiving a cancellation" race
    // shape (both backends give the cancel leg precedence).
    s.release();
    victim.cancel();
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    got.wait();  // The point: the permit was transferred to peer instead of vanishing with the
                 // cancellation
    peer.await();
    if (s.locked()) throw std::logic_error("case6: peer must have released after handoff");
  });

  // case 7: handle semantics — copies share the same semaphore (as with Event)
  run_case(__func__, "case7", [&] {
    Semaphore a(1);
    Semaphore b = a;
    a.acquire();
    if (!b.locked()) throw std::logic_error("case7: copies must share the same semaphore");
    b.release();  // A semaphore records no owner, so any handle may release
    if (a.locked()) throw std::logic_error("case7: release through a copy must free it");
  });

  // case 8: SemaphoreGuard — acquires on construction, releases on destruction (the scope is the
  // critical section)
  run_case(__func__, "case8", [&] {
    Semaphore s(1);
    {
      SemaphoreGuard g(s);
      if (!s.locked()) throw std::logic_error("case8: guard must hold the permit");
    }
    if (s.locked()) throw std::logic_error("case8: guard must release at scope exit");
  });

  // case 9: SemaphoreGuard releases while unwinding a cancellation (≡ async with's __aexit__)
  run_case(__func__, "case9", [&] {
    Semaphore s(1);
    Event in_crit;
    auto holder = create_task("c9_holder", [&] {
      SemaphoreGuard g(s);
      in_crit.set();
      Event never;
      never.wait();  // Blocks while holding the permit, then gets cancelled
    });
    in_crit.wait();
    if (!s.locked()) throw std::logic_error("case9: guard must hold the permit");
    holder.cancel();
    try {
      holder.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    if (s.locked()) throw std::logic_error("case9: guard must release while unwinding");
    s.acquire();  // The permit really is available: acquiring again does not block
    s.release();
  });

  // case 10: bound check — releasing again after the release count reaches the initial value throws
  // std::logic_error (≡ asyncio's ValueError); the throw does not change state and the bookkeeping
  // stays consistent
  run_case(__func__, "case10", [&] {
    BoundedSemaphore d;  // Bound of 1 by default
    if (d.locked()) throw std::logic_error("case10: fresh BoundedSemaphore must be unlocked");
    BoundedSemaphore b(2);
    b.acquire();
    b.acquire();
    if (!b.locked()) throw std::logic_error("case10: locked() must reflect exhausted permits");
    b.release();
    b.release();
    if (b.locked()) throw std::logic_error("case10: locked() must reflect released permits");
    bool threw = false;
    try {
      b.release();  // The third release goes past the initial value 2
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case10: release beyond bound must throw");
    b.acquire();  // The throw did not pollute the state: the bookkeeping is still consistent
    b.release();
  });

  // case 11: the bound bookkeeping does not drift after a queued waiter is cancelled — the
  // cancellation path neither consumes nor returns a permit, so after the main task returns its
  // permit the count is exactly back at the bound and one more release must throw
  run_case(__func__, "case11", [&] {
    BoundedSemaphore b(1);
    b.acquire();  // The main task holds the only permit
    Event r1, caught;
    auto victim = create_task("c11_victim", [&] {
      r1.set();
      try {
        b.acquire();
        throw std::logic_error("case11: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    r1.wait();        // victim has queued
    victim.cancel();  // Cancelled in the queue: unlinked, no permit was ever issued
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    b.release();  // The main task returns its permit: the count is back at the bound
    bool threw = false;
    try {
      b.release();  // If the cancellation path dirtied the bookkeeping, this check would fail
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case11: permits taken must equal permits released");
    b.acquire();  // Exactly one permit is available
    b.release();
  });

  // case 12: the bound check must read the real count — a permit that flowed back via "release →
  // same-slot cancellation → handoff transfer" is released by a non-holder, so the count has
  // quietly returned to the bound and one more release must throw.
  // An outer mirror count (tracking only paired calls) would miss this path and diverge from
  // asyncio.
  run_case(__func__, "case12", [&] {
    BoundedSemaphore b(1);
    b.acquire();  // The main task holds the only permit
    Event r1, r2, caught, got;
    auto victim = create_task("c12_victim", [&] {
      r1.set();
      try {
        b.acquire();
        throw std::logic_error("case12: cancelled waiter must not acquire");
      } catch (const Cancelled&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("c12_peer", [&] {
      r1.wait();
      r2.set();
      b.acquire();  // Receives victim's permit through the handoff
      got.set();
      b.release();  // A non-holder's release: the count returns to the bound
    });
    r1.wait();
    r2.wait();
    // There is no suspension point between release() and cancel(): victim is necessarily in the
    // same-slot race shape of "already released but receiving a cancellation", and its permit is
    // transferred to peer by the handoff.
    b.release();
    victim.cancel();
    caught.wait();
    try {
      victim.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    got.wait();
    peer.await();
    bool threw = false;
    try {
      b.release();  // peer has already put the count back at the bound, so this must throw
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case12: over-release through handoff must be caught");
    if (b.locked()) throw std::logic_error("case12: one permit must be available");
    b.acquire();  // The bookkeeping is correct: exactly one permit
    b.release();
  });

  // case 13: BoundedSemaphoreGuard — acquires on construction, releases on destruction (the scope
  // is the critical section)
  run_case(__func__, "case13", [&] {
    BoundedSemaphore b(1);
    {
      BoundedSemaphoreGuard g(b);
      if (!b.locked()) throw std::logic_error("case13: guard must hold the permit");
    }
    if (b.locked()) throw std::logic_error("case13: guard must release at scope exit");
  });

  report(__func__);
}
