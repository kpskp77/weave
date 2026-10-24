// Common communication-primitive usage: Queue<T> (a FIFO queue, asyncio.Queue).
//   · producer/consumer with join/task_done: join() returns only after the consumer has
//     task_done()'d every item
//   · bounded queue: put() suspends while the queue is full and is released by a get
//   · a queued getter is cancelled: the item stays in the queue for the next getter
// Synchronisation relies entirely on event passing, and nothing assumes "what should happen after
// sleeping this long".

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;

WEAVE_TEST(queue_demo) {
  // ── producer/consumer + join/task_done ────────────────────────────────
  {
    Queue<int> q;
    Event produced;
    auto producer = create_task("queue_producer", [&] {
      for (int i = 1; i <= 3; ++i) q.put(i);
      produced.set();
      q.join();  // Blocks until the consumer has task_done()'d all three items
      std::printf("[queue_demo] join returned: all items processed\n");
    });
    auto consumer = create_task("queue_consumer", [&] {
      produced.wait();  // All three items are in the queue
      for (int i = 1; i <= 3; ++i) {
        const int v = q.get();
        if (v != i) throw std::logic_error("queue_demo: FIFO order violated");
        q.task_done();
      }
    });
    gather(producer, consumer);
    std::printf("[queue_demo] producer/consumer ok: FIFO order preserved\n");
  }

  // ── bounded queue: the second put blocks until a get frees the slot ───
  {
    Queue<int> q(1);
    Event first_in;
    auto producer = create_task("queue_bounded_producer", [&] {
      q.put(1);
      first_in.set();  // The queue is full from here on
      q.put(2);        // Blocks until the main task's get frees the slot
      std::printf("[queue_demo] blocked put released after a get\n");
    });
    first_in.wait();
    if (!q.full()) throw std::logic_error("queue_demo: queue must be full");
    if (q.get_nowait() != 1) throw std::logic_error("queue_demo: first item must leave first");
    producer.await();
    if (q.get_nowait() != 2) throw std::logic_error("queue_demo: blocked put must land after get");
    std::printf("[queue_demo] bounded queue ok\n");
  }

  // ── a queued getter is cancelled: the item stays in the queue ─────────
  {
    Queue<int> q;
    Event queued;
    auto waiter = create_task("queue_waiter", [&] {
      queued.set();
      try {
        q.get();
        throw std::logic_error("queue_demo: cancelled getter must not receive an item");
      } catch (const CancelledError&) {
        std::printf("[queue_demo] getter cancelled while queued\n");
        throw;
      }
    });
    queued.wait();
    waiter.cancel();
    try {
      waiter.await();
    } catch (const CancelledError&) {
      // Expected path
    }
    q.put_nowait(7);
    auto peer = create_task("queue_peer", [&] {
      const int v = q.get();
      if (v != 7) throw std::logic_error("queue_demo: item must survive the cancelled getter");
    });
    peer.await();
    std::printf("[queue_demo] item stays in the queue after a cancelled getter\n");
  }

  // ── shutdown (Python 3.13+): blocked getters are released with
  //    QueueShutDown, and the queue closes to further puts ──────────────
  {
    Queue<int> q;
    Event waiting;
    auto consumer = create_task("queue_shutdown_consumer", [&] {
      waiting.set();
      try {
        q.get();
        throw std::logic_error("queue_demo: getter must not pass after shutdown");
      } catch (const QueueShutDown&) {
        std::printf("[queue_demo] blocked getter released by shutdown\n");
        throw;
      }
    });
    waiting.wait();
    q.shutdown();
    try {
      consumer.await();
    } catch (const QueueShutDown&) {
      // Expected path
    }
    bool threw = false;
    try {
      q.put_nowait(1);
    } catch (const QueueShutDown&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("queue_demo: put after shutdown must throw");
    std::printf("[queue_demo] shutdown ok: queue is closed to further puts\n");
  }

  std::printf("[queue_demo] done\n");
}
