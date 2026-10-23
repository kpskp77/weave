// Common communication-primitive usage: Semaphore (a counting semaphore).
//   · SemaphoreGuard: the scope is the held-permit interval (≡ Python's `async with semaphore:`)
//   · counting exclusion: 2 permits let two tasks enter the critical section at once, and the
//     third queues
//   · a queued waiter is cancelled: the permit does not go with it, it stays in the semaphore
// Synchronisation relies entirely on event passing, and nothing assumes "what should happen after
// sleeping this long".

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(semaphore_demo) {
  // ── SemaphoreGuard: returns the permit automatically on scope exit ────
  {
    Semaphore s;
    {
      SemaphoreGuard g(s);
      if (!s.locked()) throw std::logic_error("semaphore_demo: guard must hold the permit");
      std::printf("[semaphore_demo] inside guarded section\n");
    }
    if (s.locked()) throw std::logic_error("semaphore_demo: guard must release at scope exit");
    std::printf("[semaphore_demo] SemaphoreGuard released at scope exit\n");
  }

  // ── counting exclusion: a and b hold permits simultaneously (2 permits),
  // while c queues until one is returned ───────────────────────────────
  {
    Semaphore s(2);
    int depth = 0;
    int max_depth = 0;
    Event a_in;
    auto worker = [&](const char* name, Event* wake) {
      if (wake) wake->wait();
      s.acquire();
      ++depth;
      max_depth = depth > max_depth ? depth : max_depth;
      std::printf("[semaphore_demo] %s in critical section (depth=%d)\n", name, depth);
      sleep(0_fs);  // Yield: give other tasks a chance to compete
      --depth;
      s.release();
    };
    auto a = create_task("sem_a", [&] {
      worker("a", nullptr);
      a_in.set();
    });
    auto b = create_task("sem_b", [&] { worker("b", &a_in); });
    auto c = create_task("sem_c", [&] { worker("c", &a_in); });
    gather(a, b, c);
    if (max_depth != 2) throw std::logic_error("semaphore_demo: depth exceeded permit count");
    std::printf("[semaphore_demo] counting ok: max depth == permit count\n");
  }

  // ── BoundedSemaphore: an over-bound release throws (≡ asyncio's ValueError) ──
  {
    BoundedSemaphore b(1);
    b.acquire();
    b.release();  // The count is back at the bound
    bool threw = false;
    try {
      b.release();  // Past the bound: a second release without holding anything
    } catch (const std::logic_error&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("semaphore_demo: over-release must throw");
    std::printf("[semaphore_demo] BoundedSemaphore over-release rejected\n");
  }

  // ── a queued waiter is cancelled: the permit stays in the semaphore ───
  {
    Semaphore s(1);
    s.acquire();  // The main task holds the only permit
    Event queued;
    auto waiter = create_task("sem_waiter", [&] {
      queued.set();
      try {
        s.acquire();
      } catch (const CancelledError&) {
        std::printf("[semaphore_demo] waiter cancelled while queued\n");
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
    if (!s.locked())
      throw std::logic_error("semaphore_demo: permit must not leak with the cancelled waiter");
    s.release();  // The permit is still there: releasing makes the semaphore idle again
    std::printf("[semaphore_demo] cancel while queued keeps the permit in the semaphore\n");
  }
}
