// Common communication-primitive usage: Condition (a condition variable).
//   · producer/consumer: the consumer suspends while the queue is empty, and notify releases it
//     after produce
//   · wait_for: predicate polling; when it returns the lock has been re-acquired and the critical
//     section holds
//   · a waiter is cancelled: unlink from the queue, re-acquire the lock, then propagate — the lock
//     does not leak
// Synchronisation relies entirely on event passing, and nothing assumes "what should happen after
// sleeping this long".

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;

WEAVE_TEST(condition_demo) {
  // ── producer/consumer: suspend on an empty queue, released after produce ──
  {
    Condition c;
    int items = 0;
    Event consumer_in;
    auto consumer = create_task("cond_consumer", [&] {
      c.acquire();
      consumer_in.set();  // About to check the predicate for the first time
      c.wait_for([&] { return items > 0; });
      --items;  // Consume only because the predicate held inside the critical section
      c.release();
      std::printf("[condition_demo] consumer got an item\n");
    });
    consumer_in.wait();
    c.acquire();  // Blocks until consumer drops the lock inside wait() (queue empty, really
                  // suspended)
    if (items != 0) throw std::logic_error("condition_demo: queue must start empty");
    ++items;     // produce (mutating the predicate inside the critical section)
    c.notify();  // Release the waiter
    c.release();
    consumer.await();
    if (items != 0) throw std::logic_error("condition_demo: item must be consumed");
    std::printf("[condition_demo] produce/notify/consume handshake ok\n");
  }

  // ── wait_for with an already-true predicate: passes synchronously, no suspension ──
  {
    Condition c;
    int items = 1;
    c.acquire();
    ++items;
    c.release();
    auto consumer = create_task("cond_consumer2", [&] {
      c.acquire();
      c.wait_for([&] { return items > 0; });  // Returns immediately
      --items;
      c.release();
    });
    consumer.await();
    if (items != 1) throw std::logic_error("condition_demo: must consume exactly one");
    std::printf("[condition_demo] wait_for with true predicate passes without blocking\n");
  }

  // ── a waiter is cancelled: the lock returns to the available state via
  // the re-acquire/release inside the catch ────────────────────────────
  {
    Condition c;
    Event waiting;
    auto waiter = create_task("cond_waiter", [&] {
      c.acquire();
      waiting.set();
      try {
        c.wait();
        throw std::logic_error("condition_demo: cancelled waiter must not pass wait");
      } catch (const Cancelled&) {
        c.release();  // wait already re-acquired the lock; release, then propagate
        throw;
      }
    });
    waiting.wait();
    c.acquire();  // Blocks until waiter drops the lock inside wait()
    waiter.cancel();
    c.release();  // Yields the lock: wait's cancellation path re-acquires it first
    try {
      waiter.await();
    } catch (const Cancelled&) {
      // Expected path
    }
    if (c.locked()) throw std::logic_error("condition_demo: lock must be free after cancel");
    std::printf("[condition_demo] cancelled waiter re-acquired then released the lock\n");
  }
}
