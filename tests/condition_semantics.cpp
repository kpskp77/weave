// Communication-primitive self-check: Condition's asyncio.Condition semantics — the held-lock
// requirement (wait/notify throw std::logic_error without it), wait's atomic unlock and
// re-acquisition after waking, notify(n) releasing only the first n waiters, notify_all releasing
// everyone, predicate polling (wait_for = repeated wait), waiter cancellation (unlink, re-acquire,
// then propagate, without leaking the lock), reusing an external lock, and handle copies sharing
// the same condition variable.
// Synchronisation is fully event-driven (pure delta cycles) and does not depend on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp); it shares the assertion bodies with the other *_semantics.cpp files and is
// regressed by both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <stdexcept>
#include <vector>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(condition_semantics) {
  // case 1: construction owns a separate, unlocked lock; wait()/notify() without it →
  // std::logic_error (≡ asyncio's RuntimeError("cannot wait/notify on un-acquired lock"))
  run_case(__func__, "case1", [&] {
    Condition c;
    if (c.locked()) throw std::logic_error("case1: fresh Condition must be unlocked");
    bool threw_wait = false;
    try {
      c.wait();
    } catch (const std::logic_error&) {
      threw_wait = true;
    }
    if (!threw_wait) throw std::logic_error("case1: wait on un-acquired lock must throw");
    bool threw_notify = false;
    try {
      c.notify();
    } catch (const std::logic_error&) {
      threw_notify = true;
    }
    if (!threw_notify) throw std::logic_error("case1: notify on un-acquired lock must throw");
    c.acquire();
    c.notify();  // Empty waiter queue + lock held: a legal no-op
    c.release();
  });

  // case 2: wait unlocks atomically — once the waiter is blocked the lock is released (a third
  // party's acquire succeeds at once); after notify releases it, the lock is re-acquired before
  // wait returns (the critical section holds when wait returns)
  run_case(__func__, "case2", [&] {
    Condition c;
    Event r, done;
    auto w = create_task("c2_waiter", [&] {
      c.acquire();
      r.set();   // Holding-lock marker: when r.wait() returns, waiter has certainly not waited yet
      c.wait();  // Atomic: enqueue + unlock + block
      if (!c.locked()) throw std::logic_error("case2: wait must re-acquire before return");
      c.release();
      done.set();
    });
    r.wait();
    c.acquire();  // Blocks until waiter unlocks inside wait() → "the lock is released while
                  // waiting" is proven
    c.notify();   // Releases the only waiter; the lock stays with the main task
    c.release();  // waiter re-acquires, checks, releases
    done.wait();
    w.await();
    if (c.locked()) throw std::logic_error("case2: waiter must have released");
  });

  // case 3: notify(n) releases only the first n waiters (FIFO), the rest keep waiting;
  // notify_all releases the remainder.
  // The queue order is fixed by chained events ("the previous waiter sets its queued marker before
  // the next one starts").
  run_case(__func__, "case3", [&] {
    Condition c;
    Event r1, r2, r3, d1, d2, d3;
    auto w1 = create_task("c3_1", [&] {
      c.acquire();
      r1.set();
      c.wait();
      d1.set();
      c.release();
    });
    auto w2 = create_task("c3_2", [&] {
      r1.wait();
      c.acquire();
      r2.set();
      c.wait();
      d2.set();
      c.release();
    });
    auto w3 = create_task("c3_3", [&] {
      r2.wait();
      c.acquire();
      r3.set();
      c.wait();
      d3.set();
      c.release();
    });
    r1.wait();
    r2.wait();
    r3.wait();  // All three waiters have queued as w1,w2,w3, are blocked inside wait(), and the
                // lock is free
    c.acquire();
    c.notify(2);  // Releases only w1,w2
    c.release();  // Hands over the lock: w1 takes it first via the Lock FIFO, then each finishes
    d2.wait();    // d2 finished ⟹ w1 finished before it (lock FIFO)
    if (!d1.is_set()) throw std::logic_error("case3: w1 must finish before w2");
    if (d3.is_set()) throw std::logic_error("case3: w3 must still be waiting after notify(2)");
    c.acquire();  // w1/w2 have released, the lock is free
    c.notify_all();
    c.release();
    d3.wait();
    gather(w1, w2, w3);
  });

  // case 4: predicate polling — when wait is released but the predicate is still false, it must
  // re-enqueue for the next round (wait_for's loop semantics); every wait round unlocks first and
  // re-acquires after waking
  run_case(__func__, "case4", [&] {
    Condition c;
    int wakes = 0;
    Event r1, r2;
    auto w = create_task("c4_waiter", [&] {
      c.acquire();
      r1.set();
      c.wait();
      ++wakes;
      r2.set();  // About to wait a second time (holding the lock right now)
      c.wait();
      ++wakes;
      c.release();
    });
    r1.wait();
    c.acquire();  // Blocks until the first unlock
    c.notify_all();
    c.release();  // First release
    r2.wait();
    c.acquire();  // Blocks until the second unlock
    if (wakes != 1) throw std::logic_error("case4: waiter must be back in wait after false wake");
    c.notify_all();
    c.release();  // Second release
    w.await();
    if (wakes != 2) throw std::logic_error("case4: waiter must wake exactly twice");
  });

  // case 5: waiter cancellation → unlink from the queue, re-acquire the lock, then propagate
  // CancelledError; after the unlink a notify must not release it again (that would resume on a
  // destroyed stack frame), and the lock does not leak with the cancellation
  run_case(__func__, "case5", [&] {
    Condition c;
    Event r, cancelled, ack;
    auto victim = create_task("c5_victim", [&] {
      c.acquire();
      r.set();
      try {
        c.wait();
        throw std::logic_error("case5: cancelled waiter must not pass wait");
      } catch (const CancelledError&) {
        if (!c.locked())
          throw std::logic_error("case5: lock must be re-acquired before CancelledError");
        cancelled.set();
        ack.wait();  // Wait for the main task to finish verifying before unlocking (avoids a
                     // deadlock while the main task waits)
        c.release();
        throw;
      }
    });
    r.wait();
    c.acquire();  // Blocks until victim unlocks inside wait(); after that the lock belongs to the
                  // main task and victim is in the queue
    victim.cancel();
    // victim's catch must re-acquire the lock first, so the main task has to yield it
    c.release();
    cancelled.wait();  // victim has re-acquired the lock and passed the locked() check
    if (!c.locked()) throw std::logic_error("case5: re-acquired lock must belong to victim");
    ack.set();
    try {
      victim.await();
      throw std::logic_error("case5: victim must end CANCELLED");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (c.locked()) throw std::logic_error("case5: victim must have released in its catch");
    // Key invariant: victim was unlinked — notify must not set its one-shot event again
    c.acquire();
    c.notify_all();
    c.release();
  });

  // case 6: cancellation and notify in the same slot — there is no suspension point between
  // notify_all() and cancel(), so victim is in the "already dequeued but receiving CancelledError"
  // race shape (the backend cancel leg wins): the release was already consumed, the lock is
  // re-acquired and then the cancellation propagates, without affecting the remaining waiters
  run_case(__func__, "case6", [&] {
    Condition c;
    Event r1, r2, cancelled, ack, got;
    auto victim = create_task("c6_victim", [&] {
      c.acquire();
      r1.set();
      try {
        c.wait();
        throw std::logic_error("case6: cancelled waiter must not pass wait");
      } catch (const CancelledError&) {
        if (!c.locked())
          throw std::logic_error("case6: lock must be re-acquired before CancelledError");
        cancelled.set();
        ack.wait();
        c.release();
        throw;
      }
    });
    auto peer = create_task("c6_peer", [&] {
      r1.wait();
      c.acquire();
      r2.set();
      c.wait();
      got.set();
      c.release();
    });
    r1.wait();
    r2.wait();  // The condition queue is victim,peer; the lock is free
    c.acquire();
    c.notify_all();  // Both are dequeued and released
    // Same slot as the release: victim gets CancelledError, peer proceeds normally
    victim.cancel();
    c.release();
    cancelled.wait();
    ack.set();
    got.wait();  // peer is unaffected by victim's cancellation and runs through normally
    peer.await();
    try {
      victim.await();
      throw std::logic_error("case6: victim must end CANCELLED");
    } catch (const CancelledError&) {
      // Expected path
    }
  });

  // case 7: with an already-true predicate wait_for neither blocks nor yields (passes
  // synchronously, ≡ asyncio's fast path)
  run_case(__func__, "case7", [&] {
    Condition c;
    int items = 0;
    c.acquire();
    ++items;
    c.release();
    Event ran;
    auto consumer = create_task("c7_consumer", [&] {
      c.acquire();
      if (c.locked() == false) throw std::logic_error("case7: consumer must hold the lock");
      c.wait_for([&] { return items > 0; });  // Predicate already true: must not suspend
      if (!c.locked()) throw std::logic_error("case7: wait_for must hold the lock on exit");
      --items;
      c.release();
      ran.set();
    });
    consumer.await();
    if (!ran.is_set()) throw std::logic_error("case7: consumer must have run");
    if (items != 0) throw std::logic_error("case7: item must be consumed");
  });

  // case 8: reusing an external lock + handle copies sharing the same condition variable (matching
  // Lock/Event handle semantics)
  run_case(__func__, "case8", [&] {
    Lock l;
    Condition c(l);  // Matching asyncio.Condition(lock): the underlying lock is the one passed in
    c.acquire();
    if (!l.locked()) throw std::logic_error("case8: condition must hold the given lock");
    Condition d = c;  // Copies share
    d.release();
    if (l.locked()) throw std::logic_error("case8: copies must share state with the given lock");
  });

  report(__func__);
}
