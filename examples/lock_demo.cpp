// Common communication-primitive usage: Lock (a mutex).
//   · LockGuard: the scope is the critical section (≡ Python's `async with lock:`)
//   · mutual exclusion + FIFO hand-off: queued waiters enter the critical section in acquire()
//     order
//   · a queued waiter is cancelled: the lock stays with its holder, and the cancelled task holds
//     nothing and does not count as a failure
// Synchronisation relies entirely on event passing, and nothing assumes "what should happen after
// sleeping this long".

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(locks_demo) {
  // ── LockGuard: releases automatically on scope exit ───────────────────
  {
    Lock l;
    {
      LockGuard g(l);
      if (!l.locked()) throw std::logic_error("locks_demo: guard must hold the lock");
      std::printf("[locks_demo] inside critical section\n");
    }
    if (l.locked()) throw std::logic_error("locks_demo: guard must release at scope exit");
    std::printf("[locks_demo] LockGuard released at scope exit\n");
  }

  // ── mutual exclusion + FIFO: a queues first, b takes over after a releases ──
  {
    Lock l;
    l.acquire();  // The main task grabs it first so the two tasks have to queue
    Event queued_a, queued_b, done_b;
    auto a = create_task("lock_a", [&] {
      queued_a.set();
      l.acquire();
      std::printf("[locks_demo] a in critical section\n");
      l.release();
    });
    auto b = create_task("lock_b", [&] {
      queued_a.wait();  // b only sets off after a queued (event-driven, no sleep durations)
      queued_b.set();
      l.acquire();
      std::printf("[locks_demo] b in critical section\n");
      done_b.set();
      l.release();
    });
    queued_a.wait();
    queued_b.wait();  // Both a and b are now queued in order
    l.release();      // Hands off to a; after a releases, b takes over
    gather(a, b);
    if (!done_b.is_set()) throw std::logic_error("locks_demo: b must run after a");
    std::printf("[locks_demo] FIFO handoff ok\n");
  }

  // ── a queued waiter is cancelled: the lock does not go with it ────────
  {
    Lock l;
    l.acquire();  // Held by the main task
    Event queued;
    auto waiter = create_task("lock_waiter", [&] {
      queued.set();
      try {
        l.acquire();
      } catch (const CancelledError&) {
        std::printf("[locks_demo] waiter cancelled while queued\n");
        throw;  // Keep propagating (swallowing it would wrongly end the task as DONE)
      }
    });
    queued.wait();    // waiter is now queued
    waiter.cancel();  // Cancel a waiter that is sitting in the queue
    try {
      waiter.await();
    } catch (const CancelledError&) {
      // Expected path
    }
    if (!l.locked()) throw std::logic_error("locks_demo: lock must stay with its owner");
    l.release();
    std::printf("[locks_demo] cancel while queued keeps the lock with its owner\n");
  }
}
