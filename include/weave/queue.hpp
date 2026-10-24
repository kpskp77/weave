#pragma once

// Communication primitive: Queue<T> / QueueEmpty / QueueFull / QueueShutDown, whose semantics
// match asyncio.Queue (including shutdown() from Python 3.13). The algorithm is one-to-one with
// CPython's: the std::deque is the single source of truth, and the FIFO waiter queues hold pure
// "go recheck the deque" signals (each waiter owns a one-shot Event), so barging is allowed
// exactly as in asyncio.
// Everything is built on Event + the single blocking point, with zero backend changes.
// Handle semantics match Lock/Event: copies share the same queue, they are not deep copies.
//
// Queue<T> is a template and therefore header-only; the three exception types below are ordinary
// classes whose key function (the destructor) is defined out-of-line in src/core/queue.cpp, so
// their vtable/typeinfo is emitted exactly once in libweave.so and a cross-DSO catch matches (see
// the ABI notes in docs/backends.md, same as BrokenBarrierError).

#include <cstddef>
#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <weave/event.hpp>

namespace weave {

// ── QueueEmpty (asyncio.QueueEmpty) ─────────────────────────────────────────
//
// Thrown by get_nowait() on an empty queue. Unlike CancelledError it derives from
// std::exception: asyncio's QueueEmpty is an ordinary Exception, i.e. a normal failure path that a
// generic catch (const std::exception&) backstop may handle.
//
// The destructor is declared here and defined **out-of-line** (src/core/queue.cpp): it is the key
// function of this type, so the vtable/typeinfo is emitted exactly once in libweave.so and
// consumer TUs keep only undefined references. If it degraded into a per-TU weak definition,
// libc++abi's pointer comparison would make a cross-DSO catch silently mismatch (not even the
// linker would complain). Same ABI requirement as BrokenBarrierError::~BrokenBarrierError.
class QueueEmpty : public std::runtime_error {
public:
  explicit QueueEmpty(const char* what_arg) : std::runtime_error(what_arg) {}
  ~QueueEmpty() override;
};

// ── QueueFull (asyncio.QueueFull) ───────────────────────────────────────────
//
// Thrown by put_nowait() on a full queue. Same hierarchy and key-function rationale as
// QueueEmpty above.
class QueueFull : public std::runtime_error {
public:
  explicit QueueFull(const char* what_arg) : std::runtime_error(what_arg) {}
  ~QueueFull() override;
};

// ── QueueShutDown (asyncio.QueueShutDown, Python 3.13+) ─────────────────────
//
// Thrown by put()/put_nowait() on a shut-down queue, and by get()/get_nowait() once a shut-down
// queue has been drained. Unlike CancelledError it derives from std::exception: asyncio's
// QueueShutDown is an ordinary Exception (the shutdown signal is a normal failure path, not a
// cancellation). Same key-function rationale as QueueEmpty/QueueFull above.
class QueueShutDown : public std::runtime_error {
public:
  explicit QueueShutDown(const char* what_arg) : std::runtime_error(what_arg) {}
  ~QueueShutDown() override;
};

// ── Queue (asyncio.Queue) ───────────────────────────────────────────────────
//
// FIFO queue:
//   · construction takes the capacity bound (0 by default = unbounded, ≡ asyncio maxsize <= 0;
//     a positive bound makes put() wait once qsize() reaches it);
//   · put(item): appends synchronously while there is room (no suspension, no yield); when the
//     queue is full, queues in FIFO order and waits — the only blocking point is the queueing;
//   · get(): pops synchronously while the queue is non-empty (no suspension, no yield); when it is
//     empty, queues in FIFO order and waits;
//   · put_nowait()/get_nowait(): the non-blocking forms; throw QueueFull/QueueEmpty instead of
//     waiting;
//   · qsize()/empty()/full()/maxsize(): state queries (full() is always false on an unbounded
//     queue);
//   · join(): blocks until every item that has been put has been both gotten and task_done()'d
//     (asyncio's "all work is processed" barrier); returns immediately when nothing is pending;
//   · task_done(): marks one gotten item as fully processed; when the last outstanding item is
//     marked, join() is released. Throws std::logic_error when called more times than items were
//     put (≡ asyncio's ValueError('task_done() called too many times'));
//   · shutdown(immediate=false) (Python 3.13+): closes the queue to further puts — every
//     subsequent put()/put_nowait() raises QueueShutDown (whether or not there is room), and
//     every getter raises it once the queue is empty. get()/get_nowait() keep draining the items
//     that are already in the queue (a shut-down queue is wound down, not discarded). All blocked
//     getters and putters are woken and raise QueueShutDown at their blocking call; with
//     immediate=true the queue is drained right away and the join accounting is settled (each
//     drained item cancels one unfinished task; reaching zero releases join()). shutdown() is
//     idempotent and never suspends. task_done() remains valid after shutdown (to settle the
//     accounting of items already delivered), and join() itself does not raise QueueShutDown.
//
// Cancellation (one-to-one with CPython's get/put except clauses):
//   · cancelled while still queued → unlinked, nothing was ever assigned to the waiter;
//   · cancellation and a wakeup in the same slot (already dequeued by a put()/get(), the backend
//     cancel leg wins) → the deque itself is untouched, so no item or slot is lost; the cancelled
//     waiter re-wakes the next waiter of its side instead (matching CPython's
//     "if not self.empty() and not getter.cancelled(): self._wakeup_next(...)"), so a stranded
//     item or freed slot always finds a taker.
//
// Two structural notes versus CPython:
//   · there is no internal lock: the port contract's single-active-process guarantee makes every
//     check-then-act between suspension points atomic, so the deque cannot be raced (same design
//     basis as Semaphore);
//   · each blocking call creates a fresh one-shot Event per queueing round and re-checks the deque
//     after waking (the while loop), so barging — a woken waiter finding the queue full/empty
//     again because another task moved first — simply queues another round, exactly as in asyncio.
// join() carries its own per-joiner waiter queue (rather than one shared flag Event) so that
// concurrent join() calls are each released individually.
template <typename T>
class Queue {
public:
  explicit Queue(std::size_t maxsize = 0);

  // Appends item; suspends in FIFO order while the queue is full. CancelledError while queued →
  // throws CancelledError, and in that case the item was not appended (the cancellation is
  // injected at the blocking point, which is the normal shape of cooperative cancellation).
  // Throws QueueShutDown if the queue has been shut down.
  void put(T item);

  // Appends item synchronously; throws QueueFull when the queue is full (no waiting), or
  // QueueShutDown when the queue has been shut down (shutdown takes precedence over fullness).
  void put_nowait(T item);

  // Pops the head item; suspends in FIFO order while the queue is empty. CancelledError while
  // queued → throws CancelledError, and in that case no item was taken. Throws QueueShutDown when
  // the queue is empty and has been shut down.
  T get();

  // Pops the head item synchronously; throws QueueEmpty when the queue is empty (no waiting), or
  // QueueShutDown when the queue is empty and has been shut down (shutdown takes precedence).
  T get_nowait();

  [[nodiscard]] std::size_t qsize() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] bool full() const noexcept;            // Always false on an unbounded queue
  [[nodiscard]] std::size_t maxsize() const noexcept;  // 0 = unbounded

  // Marks one gotten item as fully processed; releases join() when the last one is marked.
  // Throws std::logic_error when there is no outstanding item (≡ asyncio's ValueError).
  // Remains valid after shutdown(), to settle the accounting of already-delivered items.
  void task_done();

  // Blocks until every put item has been gotten and task_done()'d.
  void join();

  // Shuts the queue down (Python 3.13+); see the class comment for the full semantics.
  // No suspension; idempotent.
  void shutdown(bool immediate = false);

private:
  struct Impl;
  // Handle semantics match Lock/Event: copies share the same queue, they are not deep copies.
  std::shared_ptr<Impl> impl_;
};

template <typename T>
struct Queue<T>::Impl {
  struct Waiter {
    Event ev;              // Wake signal: one-shot; being set() is what dequeues the waiter
    bool granted = false;  // Already woken (the owner may not have consumed it, e.g. due to
                           // cancellation in the same slot)
  };

  std::deque<T> items;         // The single source of truth; waiters are pure recheck signals
  std::size_t bound = 0;       // Capacity bound; 0 = unbounded (≡ asyncio maxsize <= 0)
  std::size_t unfinished = 0;  // Puts not yet matched by task_done() (join bookkeeping)
  bool is_shutdown = false;    // Set by shutdown(); puts close immediately, gets once drained
  std::deque<std::shared_ptr<Waiter>> getters, putters, joiners;

  [[nodiscard]] bool full() const noexcept { return bound > 0 && items.size() >= bound; }

  // ≡ CPython's `if self._is_shutdown: raise QueueShutDown` guards: called at the top of every
  // queueing loop iteration and first in both *_nowait forms, so a shut-down queue raises before
  // anything is queued or taken.
  void check_shutdown() const {
    if (is_shutdown) throw QueueShutDown("weave: Queue is shut down");
  }

  // Releases every joiner of the current round (≡ CPython's self._finished.set() under the
  // edge-semantics Event: each joiner's one-shot Event fires exactly once).
  void release_joiners() {
    while (!joiners.empty()) {
      const std::shared_ptr<Waiter> j = std::move(joiners.front());
      joiners.pop_front();
      j->granted = true;
      j->ev.set();
    }
  }

  // Removes a not-yet-woken waiter from a queue (cancellation before the wakeup; a granted waiter
  // was already popped by wake_next and is simply not found).
  static void unlink(std::deque<std::shared_ptr<Waiter>>& q, const std::shared_ptr<Waiter>& me) {
    for (auto it = q.begin(); it != q.end(); ++it) {
      if (it->get() == me.get()) {
        q.erase(it);
        return;
      }
    }
  }

  // Pops the head waiter and wakes it (≡ CPython's _wakeup_next; the deque is left to the woken
  // waiter to recheck, which is what allows barging).
  void wake_next(std::deque<std::shared_ptr<Waiter>>& q) {
    if (q.empty()) return;
    const std::shared_ptr<Waiter> next = std::move(q.front());
    q.pop_front();
    next->granted = true;
    next->ev.set();
  }
};

template <typename T>
Queue<T>::Queue(std::size_t maxsize) : impl_(std::make_shared<Impl>()) {
  impl_->bound = maxsize;
}

template <typename T>
void Queue<T>::put(T item) {
  Impl& imp = *impl_;
  while (imp.full()) {
    imp.check_shutdown();
    const std::shared_ptr<typename Impl::Waiter> me = std::make_shared<typename Impl::Waiter>();
    imp.putters.push_back(me);
    try {
      me->ev.wait();
    } catch (...) {
      // CancelledError while queued (Event::wait only throws CancelledError; catch (...) is still
      // used to protect the bookkeeping invariants). The two branches are mutually exclusive —
      // granted is only set after dequeueing:
      //   · not woken yet → remove ourselves from the queue (no slot was ever assigned to us)
      //   · already woken → a get() freed a slot and woke us; the slot is still free (we never
      //     appended), so wake the next putter instead of letting the slot strand
      if (me->granted) {
        if (!imp.full()) imp.wake_next(imp.putters);
      } else {
        Impl::unlink(imp.putters, me);
      }
      throw;
    }
  }
  put_nowait(std::move(item));
}

template <typename T>
void Queue<T>::put_nowait(T item) {
  Impl& imp = *impl_;
  imp.check_shutdown();  // Shutdown takes precedence over fullness (≡ CPython's check order)
  if (imp.full()) throw QueueFull("weave: Queue.put_nowait() on a full queue");
  imp.items.push_back(std::move(item));
  ++imp.unfinished;
  imp.wake_next(imp.getters);
}

template <typename T>
T Queue<T>::get() {
  Impl& imp = *impl_;
  while (imp.items.empty()) {
    imp.check_shutdown();  // Empty + shut down → raise (≡ CPython's in-loop guard)
    const std::shared_ptr<typename Impl::Waiter> me = std::make_shared<typename Impl::Waiter>();
    imp.getters.push_back(me);
    try {
      me->ev.wait();
    } catch (...) {
      // CancelledError while queued (see put() for the branch rationale):
      //   · not woken yet → remove ourselves from the queue (no item was ever assigned to us)
      //   · already woken → a put() appended an item and woke us; the item is still in the deque
      //     (we never popped), so wake the next getter instead of letting it strand
      if (me->granted) {
        if (!imp.items.empty()) imp.wake_next(imp.getters);
      } else {
        Impl::unlink(imp.getters, me);
      }
      throw;
    }
  }
  T item = std::move(imp.items.front());
  imp.items.pop_front();
  imp.wake_next(imp.putters);
  return item;
}

template <typename T>
T Queue<T>::get_nowait() {
  Impl& imp = *impl_;
  if (imp.items.empty()) {
    // Shutdown takes precedence over emptiness (≡ CPython's check order)
    imp.check_shutdown();
    throw QueueEmpty("weave: Queue.get_nowait() on an empty queue");
  }
  T item = std::move(imp.items.front());
  imp.items.pop_front();
  imp.wake_next(imp.putters);
  return item;
}

template <typename T>
std::size_t Queue<T>::qsize() const noexcept {
  return impl_->items.size();
}

template <typename T>
bool Queue<T>::empty() const noexcept {
  return impl_->items.empty();
}

template <typename T>
bool Queue<T>::full() const noexcept {
  return impl_->full();
}

template <typename T>
std::size_t Queue<T>::maxsize() const noexcept {
  return impl_->bound;
}

template <typename T>
void Queue<T>::task_done() {
  Impl& imp = *impl_;
  if (imp.unfinished == 0) throw std::logic_error("weave: Queue.task_done() called too many times");
  --imp.unfinished;
  if (imp.unfinished == 0) imp.release_joiners();
}

template <typename T>
void Queue<T>::join() {
  Impl& imp = *impl_;
  while (imp.unfinished > 0) {
    const std::shared_ptr<typename Impl::Waiter> me = std::make_shared<typename Impl::Waiter>();
    imp.joiners.push_back(me);
    try {
      me->ev.wait();
    } catch (...) {
      // CancelledError while queued: nothing was consumed (task_done releases all joiners of a
      // round together), so unlinking is enough; a granted joiner was already popped above.
      Impl::unlink(imp.joiners, me);
      throw;
    }
  }
}

template <typename T>
void Queue<T>::shutdown(bool immediate) {
  Impl& imp = *impl_;
  imp.is_shutdown = true;
  if (immediate) {
    // Drain the deque and settle the join accounting (≡ CPython's immediate branch): each drained
    // item cancels one unfinished task, and reaching zero releases the joiners.
    while (!imp.items.empty()) {
      imp.items.pop_front();
      if (imp.unfinished > 0) --imp.unfinished;
    }
    if (imp.unfinished == 0) imp.release_joiners();
  }
  // Wake every blocked getter and putter, exactly like CPython's set_result(None) loop: the
  // woken waiters raise QueueShutDown themselves by re-checking the loop conditions (get: empty +
  // shut down; put: the loop-top guard / put_nowait's first check) — the exception is never
  // injected at the blocking point. Waiters already granted earlier were popped by wake_next and
  // are not found here; a never-resumed cancelled waiter is popped like everyone else and its
  // already-consumed Event is harmlessly set.
  while (!imp.getters.empty()) {
    const std::shared_ptr<typename Impl::Waiter> next = std::move(imp.getters.front());
    imp.getters.pop_front();
    next->granted = true;
    next->ev.set();
  }
  while (!imp.putters.empty()) {
    const std::shared_ptr<typename Impl::Waiter> next = std::move(imp.putters.front());
    imp.putters.pop_front();
    next->granted = true;
    next->ev.set();
  }
}

}  // namespace weave
