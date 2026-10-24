// Communication-primitive self-check: the asyncio semantics of Queue — the non-blocking fast
// paths (no suspension, no yield), QueueEmpty/QueueFull signals, the full() boundary at maxsize,
// blocking get/put with FIFO service of the waiter queues, waiter cancellation (unlink, and the
// same-slot "already woken" race where the item/slot must not strand but be re-woken to the next
// waiter), the join/task_done accounting (including its misuse checks), and handle copies sharing
// the same queue.
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

WEAVE_TEST(queue_semantics) {
  // case 1: state queries and the nowait fast paths — a fresh queue is empty/not full; put/get
  // with room pass synchronously (no suspension, no yield); get_nowait on empty throws QueueEmpty;
  // put_nowait past maxsize throws QueueFull; full() flips exactly at the bound; the default
  // maxsize 0 is unbounded
  run_case(__func__, "case1", [&] {
    Queue<int> q;
    if (!q.empty() || q.qsize() != 0 || q.full() || q.maxsize() != 0)
      throw std::logic_error("case1: fresh queue must be empty, not full, unbounded");
    Event started;
    auto probe = create_task("q1_probe", [&] { started.set(); });
    q.put_nowait(7);
    if (started.is_set()) throw std::logic_error("case1: put_nowait with room must not yield");
    if (q.get_nowait() != 7) throw std::logic_error("case1: get_nowait must return the item");
    if (started.is_set()) throw std::logic_error("case1: get_nowait with items must not yield");
    probe.await();
    bool threw_empty = false;
    try {
      q.get_nowait();
    } catch (const QueueEmpty&) {
      threw_empty = true;
    }
    if (!threw_empty) throw std::logic_error("case1: get_nowait on empty must throw QueueEmpty");

    Queue<int> b(1);
    if (b.maxsize() != 1 || b.full()) throw std::logic_error("case1: Queue(1) must start not full");
    b.put_nowait(1);
    if (!b.full() || b.qsize() != 1) throw std::logic_error("case1: full() must hold at the bound");
    bool threw_full = false;
    try {
      b.put_nowait(2);
    } catch (const QueueFull&) {
      threw_full = true;
    }
    if (!threw_full) throw std::logic_error("case1: put_nowait past maxsize must throw QueueFull");
    if (b.get_nowait() != 1 || !b.empty() || b.full())
      throw std::logic_error("case1: full() must clear after the item is taken");

    q.put_nowait(1);
    q.put_nowait(2);
    q.put_nowait(3);  // More than any typical bound: unbounded must never be full
    if (q.full() || q.qsize() != 3)
      throw std::logic_error("case1: unbounded queue must never fill");
    q.get_nowait();
    q.get_nowait();
    q.get_nowait();
  });

  // case 2: blocking get — the consumer suspends on an empty queue and is released by a later
  // put; the received value crosses the hand-off correctly
  run_case(__func__, "case2", [&] {
    Queue<int> q;
    Event in_get, got;
    auto consumer = create_task("q2_consumer", [&] {
      in_get.set();  // Set before get: when in_get.wait() returns, consumer is at the latest
                     // about to block — and it cannot run past get until a put arrives
      const int v = q.get();
      if (v != 42) throw std::logic_error("case2: consumer must receive the put value");
      got.set();
    });
    in_get.wait();
    q.put_nowait(42);  // The only put: if get were not really blocked, nothing could release it
    got.wait();
    consumer.await();
    if (!q.empty()) throw std::logic_error("case2: queue must be drained");
  });

  // case 3: blocking put with a bound — the producer suspends while the queue is full and is
  // released by a get; the item lands only after the slot is free
  run_case(__func__, "case3", [&] {
    Queue<int> q(1);
    Event in_q, blocked;
    auto producer = create_task("q3_producer", [&] {
      q.put(1);
      in_q.set();  // 1 is in the queue (full), producer is at the latest about to block in put(2)
      blocked.set();
      q.put(2);
    });
    in_q.wait();
    blocked.wait();
    if (q.qsize() != 1 || !q.full())
      throw std::logic_error("case3: queue must be full while the producer is blocked");
    if (q.get_nowait() != 1)
      throw std::logic_error("case3: the first item must leave before the second lands");
    producer.await();  // Released by the get above
    if (q.qsize() != 1 || q.get_nowait() != 2)
      throw std::logic_error("case3: the blocked put must land after the slot frees");
  });

  // case 4: FIFO fairness of the getter queue — waiters are served in get() call order.
  // The queue order is fixed by chained events ("the previous waiter queues before the next one
  // starts"): r1.set() happens before get, so when r1.wait() returns g1 has necessarily queued.
  run_case(__func__, "case4", [&] {
    Queue<int> q;
    std::vector<int> order;
    Event r1, r2, g1, g2;
    auto t1 = create_task("q4_1", [&] {
      r1.set();
      order.push_back(q.get());
      g1.set();
    });
    auto t2 = create_task("q4_2", [&] {
      r1.wait();
      r2.set();
      order.push_back(q.get());
      g2.set();
    });
    r1.wait();  // t1 at the head of the getter queue
    r2.wait();  // t2 behind it
    q.put_nowait(1);
    g1.wait();  // t1 has taken its item
    if (g2.is_set()) throw std::logic_error("case4: one put must wake exactly one getter");
    if (!q.empty()) throw std::logic_error("case4: the item must be gone after t1 took it");
    q.put_nowait(2);
    g2.wait();
    gather(t1, t2);
    if (order != std::vector<int>({1, 2}))
      throw std::logic_error("case4: getters must be served in FIFO order");
  });

  // case 5: a queued getter is cancelled → it throws CancelledError and takes nothing; it is
  // unlinked, so the next put wakes a live getter and the item is delivered normally
  run_case(__func__, "case5", [&] {
    Queue<int> q;
    Event r, caught, got;
    auto victim = create_task("q5_victim", [&] {
      r.set();
      try {
        q.get();
        throw std::logic_error("case5: cancelled getter must not receive an item");
      } catch (const CancelledError&) {
        caught.set();
        throw;
      }
    });
    r.wait();         // victim is queued (the queue is empty, so it cannot pass get)
    victim.cancel();  // Cancel before any put has happened
    caught.wait();
    try {
      victim.await();
      throw std::logic_error("case5: victim must end CANCELLED");
    } catch (const CancelledError&) {
      // Expected path
    }
    q.put_nowait(9);
    auto peer = create_task("q5_peer", [&] {
      const int v = q.get();  // Receives the item: the unlink really happened (a still-linked
      if (v != 9) throw std::logic_error("case5: peer must receive the put value");
      got.set();
    });
    got.wait();
    peer.await();
    if (!q.empty()) throw std::logic_error("case5: the item must be consumed exactly once");
  });

  // case 6: cancellation and a put in the same slot — put_nowait() has already woken victim
  // (dequeued + event set), and the immediately following cancel() makes it receive
  // CancelledError at the blocking point (the backend cancel leg wins). The item must not strand:
  // victim's cancellation path re-wakes the next getter, which receives the item.
  run_case(__func__, "case6", [&] {
    Queue<int> q;
    Event r1, r2, caught, got;
    auto victim = create_task("q6_victim", [&] {
      r1.set();
      try {
        q.get();
        throw std::logic_error("case6: cancelled getter must not receive an item");
      } catch (const CancelledError&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("q6_peer", [&] {
      r1.wait();
      r2.set();
      const int v = q.get();
      if (v != 5) throw std::logic_error("case6: peer must receive the re-woken item");
      got.set();
    });
    r1.wait();
    r2.wait();  // The getter queue is victim,peer
    // There is no suspension point between put_nowait() and cancel(), so victim cannot be
    // scheduled between them: it is necessarily in the "already woken but receiving a
    // cancellation" race shape (both backends give the cancel leg precedence).
    q.put_nowait(5);
    victim.cancel();
    caught.wait();
    try {
      victim.await();
      throw std::logic_error("case6: victim must end CANCELLED");
    } catch (const CancelledError&) {
      // Expected path
    }
    got.wait();  // The point: the item went to peer through the re-wake instead of stranding
    peer.await();
    if (!q.empty()) throw std::logic_error("case6: the item must be consumed exactly once");
  });

  // case 7: a queued putter is cancelled → it throws CancelledError and its item is never
  // appended; the queue keeps exactly its previous contents and later puts proceed normally
  run_case(__func__, "case7", [&] {
    Queue<int> q(1);
    q.put_nowait(1);  // Full
    Event r, caught;
    auto victim = create_task("q7_victim", [&] {
      r.set();
      try {
        q.put(99);
        throw std::logic_error("case7: cancelled putter must not append");
      } catch (const CancelledError&) {
        caught.set();
        throw;
      }
    });
    r.wait();         // victim is queued (the queue is full, so it cannot pass put)
    victim.cancel();  // Cancel before any get has happened
    caught.wait();
    try {
      victim.await();
      throw std::logic_error("case7: victim must end CANCELLED");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (q.qsize() != 1 || q.get_nowait() != 1)
      throw std::logic_error("case7: the cancelled put must not have landed");
    q.put_nowait(2);  // The bookkeeping is intact: a later put works
    if (q.get_nowait() != 2) throw std::logic_error("case7: later puts must proceed normally");
  });

  // case 8: cancellation and a get in the same slot — get_nowait() has already woken the head
  // putter (a slot is free but its item was never appended), and the immediately following
  // cancel() makes it receive CancelledError. The slot must not strand: the cancelled putter
  // re-wakes the next putter, whose item lands.
  run_case(__func__, "case8", [&] {
    Queue<int> q(1);
    q.put_nowait(1);  // Full
    Event r1, r2, caught, done;
    auto victim = create_task("q8_victim", [&] {
      r1.set();
      try {
        q.put(2);
        throw std::logic_error("case8: cancelled putter must not append");
      } catch (const CancelledError&) {
        caught.set();
        throw;
      }
    });
    auto peer = create_task("q8_peer", [&] {
      r1.wait();
      r2.set();
      q.put(3);
      done.set();
    });
    r1.wait();
    r2.wait();  // The putter queue is victim,peer
    // No suspension point between get_nowait() and cancel(): victim is necessarily in the
    // same-slot race shape, and its freed slot is re-woken to peer.
    if (q.get_nowait() != 1) throw std::logic_error("case8: the first item must be taken");
    victim.cancel();
    caught.wait();
    try {
      victim.await();
      throw std::logic_error("case8: victim must end CANCELLED");
    } catch (const CancelledError&) {
      // Expected path
    }
    done.wait();  // peer's put landed through the re-wake
    peer.await();
    if (q.qsize() != 1 || q.get_nowait() != 3)
      throw std::logic_error("case8: peer's item must be the one in the queue");
  });

  // case 9: join/task_done accounting — join passes immediately on an idle queue; join blocks
  // until every put item is both gotten and task_done()'d; task_done beyond the outstanding count
  // (or with none outstanding) throws std::logic_error (≡ asyncio's ValueError); a joiner
  // cancelled inside join() is unlinked and does not disturb the accounting
  run_case(__func__, "case9", [&] {
    Queue<int> idle;
    idle.join();  // Nothing outstanding: must return immediately (a hang here fails the case)

    Queue<int> q;
    q.put_nowait(1);
    q.put_nowait(2);
    Event d1, d2, joined;
    auto consumer = create_task("q9_consumer", [&] {
      if (q.get() != 1) throw std::logic_error("case9: wrong first item");
      d1.set();
      q.task_done();
      if (q.get() != 2) throw std::logic_error("case9: wrong second item");
      d2.set();
      q.task_done();
    });
    auto joiner = create_task("q9_joiner", [&] {
      q.join();
      joined.set();
    });
    joined.wait();  // join returns only after the last task_done
    if (!d1.is_set() || !d2.is_set())
      throw std::logic_error("case9: join must release only after all task_done calls");
    gather(consumer, joiner);

    bool threw_extra = false;
    try {
      q.task_done();  // Both items are already done: one more is misuse
    } catch (const std::logic_error&) {
      threw_extra = true;
    }
    if (!threw_extra) throw std::logic_error("case9: extra task_done must throw");

    bool threw_bare = false;
    try {
      idle.task_done();  // Nothing was ever put
    } catch (const std::logic_error&) {
      threw_bare = true;
    }
    if (!threw_bare) throw std::logic_error("case9: task_done without puts must throw");

    Queue<int> q2;
    q2.put_nowait(8);
    Event r;
    auto join_victim = create_task("q9_join_victim", [&] {
      r.set();
      q2.join();  // Blocks: one item is outstanding
    });
    r.wait();
    join_victim.cancel();
    try {
      join_victim.await();
      throw std::logic_error("case9: cancelled joiner must end CANCELLED");
    } catch (const CancelledError&) {
      // Expected path
    }
    if (q2.get_nowait() != 8) throw std::logic_error("case9: item must survive the joiner cancel");
    q2.task_done();
    q2.join();  // The cancelled joiner left no residue: join passes immediately
  });

  // case 10: handle semantics — copies share the same queue (as with Event/Lock/Semaphore)
  run_case(__func__, "case10", [&] {
    Queue<int> a(1);
    Queue<int> b = a;
    if (a.maxsize() != 1 || b.maxsize() != 1)
      throw std::logic_error("case10: maxsize must be visible through every handle");
    b.put_nowait(1);
    if (!a.full()) throw std::logic_error("case10: copies must share the same queue");
    if (a.get_nowait() != 1) throw std::logic_error("case10: the item must cross the copy");
    if (!b.empty()) throw std::logic_error("case10: emptiness must be shared");
  });

  // case 11: shutdown() releases every blocked getter and putter with QueueShutDown, closes the
  // queue to further puts (even with room), and turns empty gets into QueueShutDown — shutdown
  // takes precedence over QueueFull/QueueEmpty, and a second shutdown() is a harmless no-op
  run_case(__func__, "case11", [&] {
    Queue<int> q1;  // Unbounded: two getters block on the empty queue
    Event g1r, g2r, g1c, g2c;
    auto g1 = create_task("q11_g1", [&] {
      g1r.set();
      try {
        q1.get();
        throw std::logic_error("case11: getter must not pass after shutdown");
      } catch (const QueueShutDown&) {
        g1c.set();
        throw;
      }
    });
    auto g2 = create_task("q11_g2", [&] {
      g1r.wait();
      g2r.set();
      try {
        q1.get();
        throw std::logic_error("case11: getter must not pass after shutdown");
      } catch (const QueueShutDown&) {
        g2c.set();
        throw;
      }
    });
    Queue<int> q2(1);  // Bounded: two putters block on the full queue
    q2.put_nowait(1);
    Event p1r, p2r, p1c, p2c;
    auto p1 = create_task("q11_p1", [&] {
      p1r.set();
      try {
        q2.put(2);
        throw std::logic_error("case11: putter must not pass after shutdown");
      } catch (const QueueShutDown&) {
        p1c.set();
        throw;
      }
    });
    auto p2 = create_task("q11_p2", [&] {
      p1r.wait();
      p2r.set();
      try {
        q2.put(3);
        throw std::logic_error("case11: putter must not pass after shutdown");
      } catch (const QueueShutDown&) {
        p2c.set();
        throw;
      }
    });
    g1r.wait();
    g2r.wait();  // Both getters are queued
    p1r.wait();
    p2r.wait();  // Both putters are queued
    q1.shutdown();
    q2.shutdown();
    // Every blocked caller is released with QueueShutDown (all getters, all putters)
    g1c.wait();
    g2c.wait();
    p1c.wait();
    p2c.wait();
    for (Task* t : {&g1, &g2, &p1, &p2}) {
      try {
        t->await();
        throw std::logic_error("case11: shut-down waiter must end FAILED");
      } catch (const QueueShutDown&) {
        // Expected path
      }
    }
    // Closed to further puts — even where there is room (q1 is empty and unbounded)
    bool threw_put = false;
    try {
      q1.put_nowait(1);
    } catch (const QueueShutDown&) {
      threw_put = true;
    }
    if (!threw_put) throw std::logic_error("case11: put_nowait after shutdown must throw");
    bool threw_put_block = false;
    try {
      q1.put(1);  // The blocking form must raise without queueing either
    } catch (const QueueShutDown&) {
      threw_put_block = true;
    }
    if (!threw_put_block) throw std::logic_error("case11: put after shutdown must throw");
    // Empty gets raise QueueShutDown, not QueueEmpty
    bool threw_get = false;
    try {
      q1.get_nowait();
    } catch (const QueueShutDown&) {
      threw_get = true;
    }
    if (!threw_get) throw std::logic_error("case11: get_nowait on shut-down empty must throw");
    // Items already in the queue survive: q2 still holds its one item
    if (q2.get_nowait() != 1) throw std::logic_error("case11: queued items must survive shutdown");
    // Idempotent
    q1.shutdown();
    q2.shutdown();
  });

  // case 12: a non-immediate shutdown is a wind-down, not a discard — the items already in the
  // queue are still delivered, task_done() keeps settling the accounting, and join() returns once
  // the last outstanding item is done; only then do gets start raising QueueShutDown
  run_case(__func__, "case12", [&] {
    Queue<int> q;
    q.put_nowait(1);
    q.put_nowait(2);
    q.shutdown();  // Non-immediate: the two items stay queued
    if (q.qsize() != 2) throw std::logic_error("case12: items must survive a wind-down shutdown");
    if (q.get_nowait() != 1) throw std::logic_error("case12: first item must still be delivered");
    q.task_done();  // Still valid after shutdown
    if (q.get() != 2) throw std::logic_error("case12: second item must still be delivered");
    q.task_done();
    q.join();  // unfinished == 0: returns even though the queue is shut down
    bool threw = false;
    try {
      q.get();  // Now empty + shut down: raises (and must not queue)
    } catch (const QueueShutDown&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case12: drained shut-down get must throw");
  });

  // case 13: join() interaction — a non-immediate shutdown does NOT release a blocked joiner (the
  // remaining work must still be task_done()'d), while an immediate shutdown drains the queue,
  // settles the unfinished count, and releases the joiner
  run_case(__func__, "case13", [&] {
    // Non-immediate: the joiner stays blocked until the item is gotten and task_done()'d
    Queue<int> q(1);
    q.put_nowait(1);
    Event rj, go, d, joined;
    auto joiner = create_task("q13_joiner", [&] {
      rj.set();
      q.join();
      joined.set();
    });
    auto consumer = create_task("q13_consumer", [&] {
      go.wait();
      if (q.get() != 1) throw std::logic_error("case13: item must be delivered after shutdown");
      q.task_done();
      d.set();
    });
    rj.wait();  // The joiner is blocked in join (unfinished == 1)
    q.shutdown();
    if (joiner.done()) throw std::logic_error("case13: a plain shutdown must not release join()");
    go.set();  // Let the consumer settle the accounting
    joined.wait();
    if (!d.is_set()) throw std::logic_error("case13: join must release only after the task_done");
    gather(joiner, consumer);

    // Immediate: the queue is drained and the unfinished count settled, releasing join()
    Queue<int> q2(2);
    q2.put_nowait(1);
    q2.put_nowait(2);
    Event rj2, joined2;
    auto joiner2 = create_task("q13_joiner2", [&] {
      rj2.set();
      q2.join();
      joined2.set();
    });
    rj2.wait();  // Blocked in join (unfinished == 2)
    q2.shutdown(true);
    joined2.wait();
    joiner2.await();
    if (!q2.empty()) throw std::logic_error("case13: immediate shutdown must drain the queue");
    bool threw = false;
    try {
      q2.get_nowait();
    } catch (const QueueShutDown&) {
      threw = true;
    }
    if (!threw) throw std::logic_error("case13: drained immediate shutdown must close gets");
  });

  report(__func__);
}
