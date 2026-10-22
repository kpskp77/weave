#pragma once

// Communication primitives: Lock / LockGuard / Condition, whose semantics match asyncio.Lock,
// `async with lock:`, and asyncio.Condition respectively.
// All of them are built on Event + the single blocking point (each queued waiter holds a one-shot
// Event), with zero backend changes.
// Handle semantics match Event: copies share the same lock/condition variable, they are not deep
// copies.

#include <cstddef>
#include <memory>

namespace weave {

// ── Lock (asyncio.Lock) ───────────────────────────────────────────────────
//
// A mutex:
//   · acquire(): if the lock is free it is taken immediately (no suspension, no yield); if it is
//     already held, queue and wait — the only blocking point is the queueing;
//   · release(): hand the lock to the queue head (FIFO fairness), or unlock if nobody is queued;
//     no suspension;
//   · locked(): whether it is currently held.
// As in asyncio, the lock does not record an owner and is not reentrant: acquiring it twice from
// the same task waits for itself to release (self-deadlock), and any task may call release().
//
// The hand-off happens inside release() with held staying true throughout: there is no
// "released but not yet taken over" idle window (in asyncio locked() briefly returns False in
// that window, and newcomers do not jump the queue either). The lock is not lost when a waiter is
// cancelled: if cancellation and hand-off land in the same slot (the lock was already handed to
// it but what it receives is Cancelled), ownership is transferred to the next waiter — see
// src/core/locks.cpp.
class Lock {
public:
  Lock();

  // Acquires the lock; if already held, suspends until it is handed over. Cancelled while queued →
  // throws Cancelled, and in that case the lock is not held (the cancellation is injected at the
  // blocking point, which is the normal shape of cooperative cancellation).
  void acquire();

  // Releases and hands off: wakes the queue head, or actually unlocks when the queue is empty.
  // Throws std::logic_error when not held (corresponding to asyncio's
  // RuntimeError("Lock is not acquired.")).
  void release();

  [[nodiscard]] bool locked() const noexcept;

private:
  struct Impl;
  // Handle semantics match Event: copies share the same lock, they are not deep copies.
  std::shared_ptr<Impl> impl_;
};

// ── LockGuard ─────────────────────────────────────────────────────────────
//
// The C++ counterpart of asyncio's `async with lock:`: the scope is the held interval.
// Construction calls acquire() (it may suspend; if it throws Cancelled the construction fails and
// nothing is held), destruction calls release() (no suspension). Cancellation during stack
// unwinding therefore releases too, equivalent to releasing through __aexit__.
// It holds its own copy of the handle (Lock is copyable and shares the same lock) and does not
// depend on the lifetime of the caller's handle.
// Misuse (e.g. a manual release() followed by destruction) terminates per noexcept semantics.
class LockGuard {
public:
  explicit LockGuard(Lock& lock);
  ~LockGuard();

  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

private:
  Lock lock_;
};

// ── Condition (asyncio.Condition) ─────────────────────────────────────────

// Condition variable whose semantics match asyncio.Condition:
//   · construction takes an underlying Lock (an existing Lock may be passed in for reuse,
//     matching asyncio.Condition(lock));
//   · acquire()/release()/locked() delegate to the underlying lock (≡ entering/leaving
//     `async with cond:`; the held interval can also be expressed as LockGuard
//     guard(cond.mutex()));
//   · wait(): requires the lock to be held. Atomically "enqueue → drop the lock → block", and after
//     being woken by notify it **re-acquires the lock** before returning (as in asyncio: when wait
//     returns, the critical section the predicate check relies on holds again). Cancelled while
//     waiting → first unlink from the queue, re-acquire the lock, then throw Cancelled (the lock
//     does not leak with the cancellation, matching asyncio 3.13+'s "re-acquire first, then
//     propagate the cancellation");
//   · wait_for(pred): calls wait() repeatedly until pred() is true (matching asyncio's
//     `while not predicate(): await cond.wait()`);
//   · notify(n=1)/notify_all(): wake the first n/all waiters, requires the lock to be held
//     (≡ asyncio's RuntimeError("cannot notify on un-acquired lock")). Waking is only a release:
//     the woken waiters still re-acquire the lock through the Lock's FIFO queue, notify does not
//     hand over the lock.
//
// Everything is built on Lock + Event + the single blocking point, with zero backend changes.
// Handle semantics match Lock/Event: copies share the same condition variable.
class Condition {
public:
  Condition();
  explicit Condition(Lock lock);  // Reuses an existing lock, matching asyncio.Condition(lock)

  void acquire();  // Acquires the underlying lock (may suspend)
  void release();  // Releases the underlying lock
  [[nodiscard]] bool locked() const noexcept;
  [[nodiscard]] Lock& mutex() noexcept;  // Underlying lock handle (asyncio's .lock attribute)

  // Requires the lock to be held; the lock is re-acquired by the time it returns. See the class
  // comment for the cancellation semantics.
  void wait();

  // Calls wait() repeatedly until pred() is true; the lock is held when it returns.
  template <typename Pred>
  void wait_for(Pred pred) {
    while (!pred()) wait();
  }

  void notify(std::size_t n = 1);  // Wakes at most n waiters from the head; requires the lock
  void notify_all();               // Wakes all waiters; requires the lock

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace weave
