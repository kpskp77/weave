// Synchronization primitives: Lock / LockGuard / Condition

#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <weave/event.hpp>
#include <weave/locks.hpp>

#include "core_internal.hpp"

namespace weave {

// ── Lock ─────────────────────────────────────────────────────────
//
// Lock implementation: every queued waiter holds a one-shot Event (the hand-off signal), and
// release() only set()s the queue head; the other waiters keep sleeping. Everything is built on
// Event + the single blocking point, so the backend needs no changes.
//
// Two invariants:
//   1. held only turns false when "the queue is empty" — it stays true throughout a hand-off, so
//      there is no "released but nobody took over" window, and a newcomer (acquire) cannot jump
//      the queue.
//   2. Ownership is not lost on the cancellation path: when cancellation and hand-off land in the
//      same delta (the backend cancel leg wins, so the waiter receives CancelledError while already
//      granted), it is handed over to the next waiter.

struct Lock::Impl {
  struct Waiter {
    Event ev;              // Hand-off signal: one-shot, released as soon as it is set()
    bool granted = false;  // Already handed over (the holder may not have consumed it, e.g. it
                           // was cancelled)
  };

  bool held = false;
  std::deque<std::shared_ptr<Waiter>> waiters;

  // Hands ownership to the queue head; actually unlocks when the queue is empty.
  void handoff() {
    if (waiters.empty()) {
      held = false;
      return;
    }
    const std::shared_ptr<Waiter> next = std::move(waiters.front());
    waiters.pop_front();
    next->granted = true;
    next->ev.set();  // held stays true: ownership transfers directly, never passing through idle
  }
};

Lock::Lock() : impl_(std::make_shared<Impl>()) {}

bool Lock::locked() const noexcept {
  return impl_->held;
}

void Lock::acquire() {
  if (!impl_->held) {
    impl_->held = true;
    return;  // Free lock taken synchronously: no suspension, no yield (≡ asyncio's fast path)
  }
  const std::shared_ptr<Impl::Waiter> me = std::make_shared<Impl::Waiter>();
  impl_->waiters.push_back(me);
  try {
    me->ev.wait();
  } catch (...) {
    // CancelledError while queued (Event::wait only throws CancelledError; catch (...) is still
    // used to protect the "ownership is never lost" invariant). The two branches are mutually
    // exclusive — granted is only set after dequeueing:
    //   · not handed over yet → remove ourselves from the queue, the lock still belongs to the
    //     current holder
    //   · already handed over → hand it on to the queue head, the lock must not vanish with the
    //     cancellation
    if (me->granted) {
      impl_->handoff();
    } else {
      auto& q = impl_->waiters;
      for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->get() == me.get()) {
          q.erase(it);
          break;
        }
      }
    }
    throw;
  }
}

void Lock::release() {
  if (!impl_->held) throw std::logic_error("weave: Lock.release() on unlocked lock");
  impl_->handoff();
}

LockGuard::LockGuard(Lock& lock) : lock_(lock) {
  lock_.acquire();
}

LockGuard::~LockGuard() {
  lock_.release();
}

// ── Condition ─────────────────────────────────────────────────────────
//
// Every member of the waiter queue holds a one-shot Event (the release signal). notify only set()s
// at most the first n waiters and does not hand over the lock: before returning, the woken waiters
// re-acquire the lock via Lock::acquire() in FIFO order, and the lock belongs to the notifier in
// the meantime.
//
// The cancellation path upholds two invariants:
//   1. Both unlinking from the queue and re-acquiring the lock happen inside the catch, so the lock
//      has been restored by the time CancelledError propagates (nothing leaks);
//   2. if cancellation and notify land in the same slot (already dequeued and what it receives is
//      CancelledError), the failed unlink is the expected shape — the release was already consumed,
//      so re-acquire the lock and then propagate.

struct Condition::Impl {
  struct Waiter {
    Event ev;  // Release signal: one-shot; set() is what dequeues it (notify dequeues first)
  };

  Lock mutex;
  std::deque<std::shared_ptr<Waiter>> waiters;
};

Condition::Condition() : impl_(std::make_shared<Impl>()) {}

Condition::Condition(Lock lock) : impl_(std::make_shared<Impl>()) {
  impl_->mutex = lock;
}

void Condition::acquire() {
  impl_->mutex.acquire();
}

void Condition::release() {
  impl_->mutex.release();
}

bool Condition::locked() const noexcept {
  return impl_->mutex.locked();
}

Lock& Condition::mutex() noexcept {
  return impl_->mutex;
}

void Condition::wait() {
  if (!impl_->mutex.locked()) throw std::logic_error("weave: Condition.wait() on un-acquired lock");
  const std::shared_ptr<Impl::Waiter> me = std::make_shared<Impl::Waiter>();
  impl_->waiters.push_back(me);
  impl_->mutex.release();  // First step of the atomic semantics: no suspension between enqueue
                           // and unlocking
  try {
    me->ev.wait();
  } catch (...) {
    // Cancellation (Event::wait only throws CancelledError; catch (...) is still used to protect
    // the invariant). Not dequeued by notify yet → unlink; already dequeued (same slot as notify) →
    // the failed unlink is the expected shape.
    auto& q = impl_->waiters;
    for (auto it = q.begin(); it != q.end(); ++it) {
      if (it->get() == me.get()) {
        q.erase(it);
        break;
      }
    }
    impl_->mutex.acquire();  // Restore the lock before propagating (matching asyncio 3.13+)
    throw;
  }
  impl_->mutex.acquire();  // Re-acquire after waking up, so the critical section still holds when
                           // wait returns
}

void Condition::notify(std::size_t n) {
  if (!impl_->mutex.locked())
    throw std::logic_error("weave: Condition.notify() on un-acquired lock");
  auto& q = impl_->waiters;
  while (n > 0 && !q.empty()) {
    const std::shared_ptr<Impl::Waiter> next = std::move(q.front());
    q.pop_front();  // Dequeue before releasing: the woken waiter is no longer in the queue when
                    // it returns
    next->ev.set();
    --n;
  }
}

void Condition::notify_all() {
  notify(impl_->waiters.size());
}

}  // namespace weave
