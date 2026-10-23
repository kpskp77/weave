// Synchronization primitives: Semaphore / BoundedSemaphore
//
// Both share one piece of core bookkeeping (SemaphoreCore in the anonymous namespace below): the
// count, the FIFO waiter queue, handoff, and queueing/waiting-cancellation-transfer at the single
// blocking point. They differ in exactly one place — BoundedSemaphore::release() has an extra bound
// check, so release() stays in each class (Semaphore never throws, BoundedSemaphore throws when
// value >= bound), and everything else is reused.
//
// Public inheritance is deliberately not used for unification: Semaphore is a value-semantics
// handle, so `Semaphore s = bounded;` would slice it into an ordinary handle sharing the same Impl,
// and SemaphoreGuard only accepts Semaphore& — both can bypass the bound check. Plugging those
// holes would mean making release virtual, which costs far more than it gains.
// Why share rather than write two copies: the cancellation/same-slot transfer path in acquire()
// below is the most error-prone code in the repository (see "Pitfalls in this environment" in
// AGENTS.md); once two implementations drift, the difference only shows up as backend-dependent
// undefined behaviour under some delta race.

#include <cstddef>
#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <weave/event.hpp>
#include <weave/semaphore.hpp>

namespace weave {
namespace {

// ── Shared core bookkeeping ──────────────────────────────────────────────────────
//
// Two invariants (check each one when editing this):
//   1. value > 0 ⇒ waiters is empty. acquire takes synchronously only while the count is > 0, and
//      handoff only increments the count when the queue is empty, so there is no idle window where
//      "a permit is available but someone is queued", and a newly arriving acquire cannot jump the
//      queue (barging protection, matching asyncio 3.10+'s fair semantics).
//   2. Permits are not lost on the cancellation path: when cancellation and release land in the
//      same delta (the backend cancel leg wins, so the waiter receives CancelledError while already
//      granted), the permit is transferred to the next waiter; if the queue is already empty the
//      permit is returned to the count.
struct SemaphoreCore {
  struct Waiter {
    Event ev;              // Release signal: one-shot, being set() is what acquires the permit
    bool granted = false;  // Already released (the acquirer may not have consumed it, e.g. due to
                           // cancellation)
  };

  std::size_t value = 0;
  std::deque<std::shared_ptr<Waiter>> waiters;

  // Hands the permit to the queue head; returns it to the count (value + 1) when the queue is
  // empty.
  void handoff() {
    if (waiters.empty()) {
      ++value;
      return;
    }
    const std::shared_ptr<Waiter> next = std::move(waiters.front());
    waiters.pop_front();
    next->granted = true;
    next->ev.set();
  }

  // Acquires one permit: taken synchronously while the count is > 0 (no suspension, no yield);
  // queues in FIFO order when the count is 0.
  void acquire() {
    if (value > 0) {
      --value;
      return;  // Available count taken synchronously ≡ asyncio's fast path
    }
    const std::shared_ptr<Waiter> me = std::make_shared<Waiter>();
    waiters.push_back(me);
    try {
      me->ev.wait();
    } catch (...) {
      // CancelledError while queued (Event::wait only throws CancelledError; catch (...) is still
      // used to protect invariant 2). The two branches are mutually exclusive — granted is only set
      // after dequeueing:
      //   · not released yet → remove ourselves from the queue (the permit was never issued)
      //   · already released → transfer to the queue head / return to the count, the permit must
      //     not vanish with the cancellation
      if (me->granted) {
        handoff();
      } else {
        for (auto it = waiters.begin(); it != waiters.end(); ++it) {
          if (it->get() == me.get()) {
            waiters.erase(it);
            break;
          }
        }
      }
      throw;
    }
  }
};

}  // namespace

// ── Semaphore ─────────────────────────────────────────────────────────
//
// Impl only serves type distinction (each handle holds its own Impl; copies share it, no
// conversion across types); all state and logic live in SemaphoreCore.

struct Semaphore::Impl : SemaphoreCore {};

Semaphore::Semaphore(std::size_t value) : impl_(std::make_shared<Impl>()) {
  impl_->value = value;
}

bool Semaphore::locked() const noexcept {
  // Invariant 1: a non-empty queue ⇒ value == 0, so the count alone is enough.
  return impl_->value == 0;
}

void Semaphore::acquire() {
  impl_->acquire();
}

void Semaphore::release() {
  impl_->handoff();  // No bound: releasing always succeeds (≡ asyncio.Semaphore)
}

SemaphoreGuard::SemaphoreGuard(Semaphore& sem) : sem_(sem) {
  sem_.acquire();
}

SemaphoreGuard::~SemaphoreGuard() {
  sem_.release();
}

// ── BoundedSemaphore ──────────────────────────────────────────────────

// bound exists only in BoundedSemaphore's Impl: the upper bound is a concept unique to it and is
// not written into the shared core. Invariant 1 is strengthened here to value <= bound — the check
// in release guarantees value < bound on entry to handoff, so the empty-queue +1 cannot exceed the
// bound; the handoff on the cancellation path returns a permit that was already released, so it
// cannot exceed it either.

struct BoundedSemaphore::Impl : SemaphoreCore {
  std::size_t bound = 0;  // The upper bound = the initial permit count
};

BoundedSemaphore::BoundedSemaphore(std::size_t value) : impl_(std::make_shared<Impl>()) {
  impl_->value = value;
  impl_->bound = value;
}

bool BoundedSemaphore::locked() const noexcept {
  return impl_->value == 0;
}

void BoundedSemaphore::acquire() {
  impl_->acquire();  // Same implementation as Semaphore (including cancellation transfer)
}

void BoundedSemaphore::release() {
  // value >= bound ⟺ CPython's self._value >= self._bound_value: releasing again would push the
  // count past its initial value, which is API misuse (≡ asyncio's ValueError).
  // The check must read the real count: a permit can flow back through handoff and be released by
  // a non-holder, and an outer mirror count (tracking paired calls only) would miss the
  // over-release on those paths.
  if (impl_->value >= impl_->bound)
    throw std::logic_error("weave: BoundedSemaphore.release() released too many times");
  impl_->handoff();
}

BoundedSemaphoreGuard::BoundedSemaphoreGuard(BoundedSemaphore& sem) : sem_(sem) {
  sem_.acquire();
}

BoundedSemaphoreGuard::~BoundedSemaphoreGuard() {
  sem_.release();
}

}  // namespace weave
