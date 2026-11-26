// Communication-primitive self-check: the asyncio.Barrier semantics — the fixed-party rendezvous
// (parties-1 arrivals block, the parties-th releases the whole generation at once with a unique
// arrival index each), n_waiting/parties/broken, reuse across generations (including an arrival
// that has to wait for the previous generation to drain), abort()/reset() (BrokenBarrierError for
// the parties inside and for later wait() calls, reset reviving a broken barrier), cancellation of
// a party (asyncio: the barrier is neither broken nor corrupted, the remaining parties keep
// waiting until abort()/reset()), and handle copies sharing state.
// Synchronisation is fully event-driven (pure delta cycles) and does not depend on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp); it shares the assertion bodies with the other *_semantics.cpp files and is
// regressed by both the weave_smoke_sc and weave_smoke_svdpi backends.

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(barrier_semantics) {
  // case 1: construction and accessors — parties >= 1 is required (0 throws
  // std::invalid_argument, ≡ asyncio's ValueError), a fresh barrier reports open/filling
  run_case(__func__, "case1", [&] {
    bool threw = false;
    try {
      Barrier invalid(0);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case1: parties == 0 must throw std::invalid_argument");
    Barrier b(3);
    if (b.parties() != 3) throw std::logic_error("case1: parties() must echo the constructor");
    if (b.n_waiting() != 0) throw std::logic_error("case1: a fresh barrier has no waiting party");
    if (b.broken()) throw std::logic_error("case1: a fresh barrier is not broken");
    Barrier single(1);  // parties == 1 is legal: every wait() trips it on its own
    if (single.wait() != 0) throw std::logic_error("case1: a single party must get index 0");
    if (single.n_waiting() != 0) throw std::logic_error("case1: a tripped barrier has no waiter");
  });

  // case 2: rendezvous — the first parties-1 arrivals block, the parties-th releases the whole
  // generation at once, and every party gets back its arrival index (0..parties-1)
  run_case(__func__, "case2", [&] {
    Barrier b(3);
    Event t1_in, t2_in;
    std::size_t i1 = 99;
    std::size_t i2 = 99;
    bool t1_released = false;
    bool t2_released = false;
    auto t1 = create_task("c2_t1", [&] {
      t1_in.set();
      i1 = b.wait();
      t1_released = true;
    });
    auto t2 = create_task("c2_t2", [&] {
      t1_in.wait();
      t2_in.set();
      i2 = b.wait();
      t2_released = true;
    });
    t1_in.wait();  // t1 is inside the barrier
    t2_in.wait();  // t2 is inside the barrier (the events are set before wait(), so the main task
                   // can only resume after both have blocked)
    if (b.n_waiting() != 2)
      throw std::logic_error("case2: n_waiting must count the parties inside");
    if (t1_released || t2_released)
      throw std::logic_error("case2: a bare majority must not pass the barrier");
    const std::size_t im = b.wait();  // The third party trips it
    if (im != 2) throw std::logic_error("case2: the index must be the arrival order");
    if (b.n_waiting() != 0) throw std::logic_error("case2: a tripped barrier has no waiting party");
    if (b.broken()) throw std::logic_error("case2: tripping must not break the barrier");
    gather(t1, t2);
    if (!t1_released || !t2_released)
      throw std::logic_error("case2: the parties-th arrival must release everyone");
    if (i1 != 0 || i2 != 1) throw std::logic_error("case2: indexes must be the arrival order");
  });

  // case 3: reuse across generations — after the parties leave, the barrier trips again, and an
  // arrival during the drain window of the previous generation waits for it to finish before it
  // can take a slot
  run_case(__func__, "case3", [&] {
    Barrier b(2);
    std::size_t i1 = 99;
    bool t1_left = false;
    Event t1_in;
    auto t1 = create_task("c3_t1", [&] {
      t1_in.set();
      i1 = b.wait();
      t1_left = true;  // Only true once wait() returned, i.e. once the barrier was left
    });
    t1_in.wait();  // t1 is inside
    const std::size_t im = b.wait();
    if (im != 1) throw std::logic_error("case3: the second arrival must get index 1");
    if (b.n_waiting() != 0)
      throw std::logic_error("case3: released parties that have not left yet are not waiting");
    if (t1_left) throw std::logic_error("case3: the released party cannot have left yet");
    // The next generation's first arrival: it arrives while the previous generation is still
    // draining, so it has to wait for that to finish instead of running off with a leftover slot.
    std::size_t im2 = 99;
    std::size_t i2 = 99;
    auto t2 = create_task("c3_t2", [&] { i2 = b.wait(); });
    im2 = b.wait();
    if (!t1_left) throw std::logic_error("case3: the next generation must wait for the drain");
    gather(t1, t2);
    if (i1 != 0) throw std::logic_error("case3: the first arrival of the first round got index 0");
    if (im2 == i2 || im2 > 1 || i2 > 1)
      throw std::logic_error("case3: the next generation must hand out indexes 0..parties-1");
    if (b.broken()) throw std::logic_error("case3: reuse must not break the barrier");
  });

  // case 4: abort() — the parties waiting inside and every later wait() throw BrokenBarrierError,
  // n_waiting drops to 0, and the broken state is sticky (no automatic recovery)
  run_case(__func__, "case4", [&] {
    Barrier b(3);
    Event t1_in, t2_in, caught1, caught2;
    auto t1 = create_task("c4_t1", [&] {
      t1_in.set();
      try {
        b.wait();
        throw std::logic_error("case4: an aborted party must not pass the barrier");
      } catch (const BrokenBarrierError& e) {
        // A party that was already inside when the barrier broke gets asyncio's _wait() message;
        // "Barrier aborted" is what a party arriving afterwards gets (see case4's last check).
        if (std::string(e.what()) != "Abort or reset of barrier")
          throw std::logic_error("case4: unexpected message for an aborted party");
        caught1.set();
      }
    });
    auto t2 = create_task("c4_t2", [&] {
      t1_in.wait();
      t2_in.set();
      try {
        b.wait();
        throw std::logic_error("case4: an aborted party must not pass the barrier");
      } catch (const BrokenBarrierError&) {
        caught2.set();
      }
    });
    t1_in.wait();  // t1 is inside
    t2_in.wait();  // t2 is inside
    if (b.n_waiting() != 2)
      throw std::logic_error("case4: n_waiting must count the parties inside");
    b.abort();
    if (!b.broken()) throw std::logic_error("case4: abort() must break the barrier");
    if (b.n_waiting() != 0) throw std::logic_error("case4: a broken barrier has no waiter");
    caught1.wait();
    caught2.wait();
    gather(t1, t2);
    bool threw = false;
    try {
      b.wait();  // An arrival after the abort fails immediately
    } catch (const BrokenBarrierError& e) {
      threw = std::string(e.what()) == "Barrier aborted";
    }
    if (!threw) throw std::logic_error("case4: wait() on a broken barrier must throw");
    if (!b.broken()) throw std::logic_error("case4: the broken state must be sticky");
  });

  // case 5: reset() while parties wait — they throw BrokenBarrierError, and once the last one has
  // left the barrier is filling again and reusable
  run_case(__func__, "case5", [&] {
    Barrier b(2);
    Event t1_in, caught;
    auto t1 = create_task("c5_t1", [&] {
      t1_in.set();
      try {
        b.wait();
        throw std::logic_error("case5: a reset party must not pass the barrier");
      } catch (const BrokenBarrierError& e) {
        if (std::string(e.what()) != "Abort or reset of barrier")
          throw std::logic_error("case5: unexpected message for a reset party");
        caught.set();
      }
    });
    t1_in.wait();  // t1 is inside
    b.reset();
    caught.wait();
    t1.await();
    if (b.broken()) throw std::logic_error("case5: reset() must not break the barrier");
    if (b.n_waiting() != 0) throw std::logic_error("case5: reset() must clear the waiting parties");
    auto t2 = create_task("c5_t2", [&] { b.wait(); });
    b.wait();  // The barrier is filling again: this trips together with t2
    t2.await();
  });

  // case 6: reset() on a broken barrier revives it; reset() on an idle barrier is a no-op
  run_case(__func__, "case6", [&] {
    Barrier b(2);
    b.abort();
    if (!b.broken()) throw std::logic_error("case6: abort() must break the barrier");
    b.reset();
    if (b.broken()) throw std::logic_error("case6: reset() must clear the broken state");
    if (b.n_waiting() != 0) throw std::logic_error("case6: an idle reset must not create waiters");
    auto t = create_task("c6_t", [&] { b.wait(); });
    b.wait();  // Revived: it trips normally again
    t.await();
    b.reset();  // Idle: still usable
    auto t2 = create_task("c6_t2", [&] { b.wait(); });
    b.wait();
    t2.await();
  });

  // case 7: a cancelled party leaves the barrier and throws CancelledError — as in asyncio the
  // barrier is neither broken nor corrupted (unlike threading.Barrier), and it can trip again
  // afterwards
  run_case(__func__, "case7", [&] {
    Barrier b(2);
    Event t1_in, caught;
    auto t1 = create_task("c7_t1", [&] {
      t1_in.set();
      try {
        b.wait();
        throw std::logic_error("case7: a cancelled party must not pass the barrier");
      } catch (const CancelledError&) {
        caught.set();
        throw;
      }
    });
    t1_in.wait();  // t1 is inside
    if (b.n_waiting() != 1) throw std::logic_error("case7: n_waiting must count the party inside");
    t1.cancel();
    caught.wait();
    try {
      t1.await();
    } catch (const CancelledError&) {
      // Expected path
    }
    if (b.broken()) throw std::logic_error("case7: cancellation must not break the barrier");
    if (b.n_waiting() != 0)
      throw std::logic_error("case7: the cancelled party must leave the barrier");
    auto t2 = create_task("c7_t2", [&] { b.wait(); });
    b.wait();  // The bookkeeping is intact: a fresh generation trips
    t2.await();
  });

  // case 8: a cancellation with other parties still waiting — the barrier stays filling, so the
  // remaining parties keep waiting (asyncio semantics); abort() is what releases them
  run_case(__func__, "case8", [&] {
    Barrier b(3);
    Event t1_in, t2_in, caught_cancel, caught_broken;
    auto t1 = create_task("c8_t1", [&] {
      t1_in.set();
      try {
        b.wait();
        throw std::logic_error("case8: the remaining party must not pass the barrier");
      } catch (const BrokenBarrierError&) {
        caught_broken.set();
      }
    });
    auto t2 = create_task("c8_t2", [&] {
      t1_in.wait();
      t2_in.set();
      try {
        b.wait();
        throw std::logic_error("case8: a cancelled party must not pass the barrier");
      } catch (const CancelledError&) {
        caught_cancel.set();
        throw;
      }
    });
    t1_in.wait();  // t1 is inside
    t2_in.wait();  // t2 is inside
    t2.cancel();
    caught_cancel.wait();
    try {
      t2.await();
    } catch (const CancelledError&) {
      // Expected path
    }
    if (b.broken()) throw std::logic_error("case8: cancellation must not break the barrier");
    if (b.n_waiting() != 1)
      throw std::logic_error("case8: only the cancelled party may leave the barrier");
    if (caught_broken.is_set())
      throw std::logic_error("case8: the remaining party must keep waiting");
    b.abort();  // Releases the party that is still waiting
    caught_broken.wait();
    t1.await();
  });

  // case 9: handle semantics — copies share the same barrier (as with Lock/Event)
  run_case(__func__, "case9", [&] {
    Barrier a(2);
    Barrier b = a;
    if (b.parties() != 2) throw std::logic_error("case9: copies must share the same barrier");
    a.abort();
    if (!b.broken()) throw std::logic_error("case9: abort through a copy must be visible");
    b.reset();
    if (a.broken()) throw std::logic_error("case9: reset through a copy must be visible");
    auto t = create_task("c9_t", [&] { b.wait(); });
    a.wait();  // Trips the same barrier
    t.await();
  });

  report(__func__);
}
