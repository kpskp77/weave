// Common communication-primitive usage: Barrier (a fixed-party rendezvous point).
//   · rendezvous: three parties meet at the barrier; the first two block, the third one releases
//     all of them at once, and each gets back its arrival index (≡ asyncio.Barrier.wait())
//   · reuse: the barrier trips again for the next generation
//   · abort(): a party still waiting fails with BrokenBarrierError, and every later wait() fails
//     immediately (≡ asyncio.Barrier.abort)
// Synchronisation relies entirely on event passing, and nothing assumes "what should happen after
// sleeping this long".

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(barrier_demo) {
  // ── rendezvous: three parties, released together ──────────────────────
  {
    Barrier b(3);
    std::printf("[barrier_demo] barrier created with %zu parties\n", b.parties());
    auto a = create_task("barrier_a", [&] {
      const std::size_t index = b.wait();
      std::printf("[barrier_demo] a released, arrival index %zu\n", index);
    });
    auto c = create_task("barrier_c", [&] {
      const std::size_t index = b.wait();
      std::printf("[barrier_demo] c released, arrival index %zu\n", index);
    });
    const std::size_t mine = b.wait();  // The main task is the third party and trips the barrier
    if (b.n_waiting() != 0)
      throw std::logic_error("barrier_demo: a tripped barrier has no waiting party");
    std::printf("[barrier_demo] main released, arrival index %zu\n", mine);
    gather(a, c);
    if (b.broken())
      throw std::logic_error("barrier_demo: a normal trip does not break the barrier");
  }

  // ── the barrier is reusable: the next generation trips as well ──────
  {
    Barrier b(2);
    auto peer = create_task("barrier_peer", [&] { b.wait(); });
    b.wait();
    peer.await();
    std::printf("[barrier_demo] second generation passed (n_waiting=%zu)\n", b.n_waiting());
  }

  // ── abort(): the party still waiting fails instead of passing ────────
  {
    Barrier b(2);
    Event inside;
    auto waiter = create_task("barrier_waiter", [&] {
      inside.set();
      try {
        b.wait();
        throw std::logic_error("barrier_demo: an aborted party must not pass the barrier");
      } catch (const BrokenBarrierError& e) {
        std::printf("[barrier_demo] waiter released with an error: %s\n", e.what());
      }
    });
    inside.wait();  // waiter is inside the barrier
    b.abort();
    waiter.await();
    try {
      b.wait();  // Every later arrival fails immediately too
      throw std::logic_error("barrier_demo: wait() on a broken barrier must throw");
    } catch (const BrokenBarrierError&) {
      std::printf("[barrier_demo] broken=%d, later wait() rejected\n", b.broken() ? 1 : 0);
    }
    b.reset();  // Back to a usable, empty barrier
    if (b.broken()) throw std::logic_error("barrier_demo: reset() must clear the broken state");
  }

  std::printf("[barrier_demo] done\n");
}
