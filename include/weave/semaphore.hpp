#pragma once

// Communication primitives: Semaphore / SemaphoreGuard / BoundedSemaphore /
// BoundedSemaphoreGuard, whose semantics match asyncio.Semaphore,
// `async with semaphore:`, and asyncio.BoundedSemaphore respectively.
// All of them are built on Event + the single blocking point (each queued waiter holds a one-shot
// Event), with zero backend changes.
// Handle semantics match Lock/Event: copies share the same semaphore, they are not deep copies.

#include <cstddef>
#include <memory>

namespace weave {

// ── Semaphore (asyncio.Semaphore) ────────────────────────────────────────────
//
// Counting semaphore:
//   · construction takes the number of permits (1 by default, ≡ asyncio.Semaphore(value=1);
//     0 is legal and means the first acquire must queue);
//   · acquire(): taken immediately while the count is > 0 (no suspension, no yield); when the
//     count is 0, queue in FIFO order and wait to be released — the only blocking point is the
//     queueing;
//   · release(): releases one permit — handed straight to the queue head if there is a waiter,
//     otherwise the count is incremented. Unlike Lock, release always succeeds and does not
//     require a prior acquire (a semaphore records no owner, as in asyncio);
//   · locked(): whether acquire() would block right now.
//
// It shares Lock's two invariants (see src/core/semaphore.cpp for details):
//   1. waiters can only exist while the count is 0 — there is no idle window where "a permit is
//      available but someone is queued", and a newly arriving acquire cannot jump the queue
//      (barging protection, matching asyncio 3.10+'s fair semantics);
//   2. permits are never lost on the cancellation path: when cancellation and release land in the
//      same slot (already granted but what it receives is CancelledError), the permit is
//      transferred to the next waiter, or returned to the count if the queue is empty.
class Semaphore {
public:
  explicit Semaphore(std::size_t value = 1);

  // Acquires one permit; suspends until released if the count is 0. CancelledError while queued →
  // throws CancelledError, and in that case no permit is held (the cancellation is injected at the
  // blocking point, which is the normal shape of cooperative cancellation).
  void acquire();

  // Releases one permit: handed straight to the queue head if a waiter exists, otherwise the count
  // is incremented. No suspension, always succeeds.
  void release();

  [[nodiscard]] bool locked() const noexcept;  // Whether acquire() would block right now

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// ── SemaphoreGuard ───────────────────────────────────────────────────────────
//
// The C++ counterpart of asyncio's `async with semaphore:`: the scope is the held-permit interval.
// Construction calls acquire() (it may suspend; if it throws CancelledError the construction fails
// and no permit is held), destruction calls release() (no suspension). Cancellation during stack
// unwinding therefore releases too, equivalent to releasing through __aexit__.
// It holds its own copy of the handle (Semaphore is copyable and shares the same semaphore) and
// does not depend on the lifetime of the caller's handle.
// Consistent with Semaphore itself, the guard's release in its destructor always succeeds
// (semaphores record no owner).
class SemaphoreGuard {
public:
  explicit SemaphoreGuard(Semaphore& sem);
  ~SemaphoreGuard();

  SemaphoreGuard(const SemaphoreGuard&) = delete;
  SemaphoreGuard& operator=(const SemaphoreGuard&) = delete;

private:
  Semaphore sem_;
};

// ── BoundedSemaphore (asyncio.BoundedSemaphore) ──────────────────────────────
//
// Bounded semaphore: structurally identical to Semaphore (same permit acquisition, FIFO queueing,
// barging protection, and no permit loss on cancellation; see the Semaphore section above), with
// the single difference that release() performs a bound check — releasing after the internal count
// has reached the bound passed to the constructor (= the initial permit count) throws
// std::logic_error (corresponding to asyncio's
// ValueError("BoundedSemaphore released too many times")).
// Same origin as Lock's "releasing while not held throws": the bound is the semaphore version of a
// binary constraint, and releasing past it makes the count bookkeeping meaningless. The check reads
// the real count directly (value >= bound), one-to-one with CPython's
// `self._value >= self._bound_value` — an outer mirror count cannot substitute for it, because a
// permit can flow back through a handoff and be released by a non-holder, and the mirror would
// miss the over-release on those paths.
//
// The implementation shares one core bookkeeping struct with Semaphore (SemaphoreCore in
// src/core/semaphore.cpp), adding only the bound member and this one check in release; public
// inheritance is not used, because a value-semantics handle would get sliced or let
// SemaphoreGuard bypass the bound check.
class BoundedSemaphore {
public:
  explicit BoundedSemaphore(std::size_t value = 1);

  // Acquires one permit; suspends until released if the count is 0. CancelledError while queued →
  // throws CancelledError, and in that case no permit is held. Exactly like Semaphore::acquire.
  void acquire();

  // Releases one permit; throws std::logic_error if the count has already reached the bound (the
  // number of released permits equals the initial value). No suspension. Otherwise identical to
  // Semaphore::release.
  void release();

  [[nodiscard]] bool locked() const noexcept;  // Whether acquire() would block right now

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// ── BoundedSemaphoreGuard ────────────────────────────────────────────────────
//
// The C++ counterpart of asyncio's `async with bounded_semaphore:`: the scope is the held-permit
// interval. Construction calls acquire() (it may suspend; if it throws CancelledError the
// construction fails and no permit is held), destruction calls release() (no suspension).
// Cancellation during stack unwinding therefore releases too. It holds its own copy of the handle
// (BoundedSemaphore is copyable and shares the same semaphore) and does not depend on the lifetime
// of the caller's handle. The guard pairs strictly with acquire — misuse (an over-bound release
// caused by more destructions than constructions) terminates per noexcept semantics.
class BoundedSemaphoreGuard {
public:
  explicit BoundedSemaphoreGuard(BoundedSemaphore& sem);
  ~BoundedSemaphoreGuard();

  BoundedSemaphoreGuard(const BoundedSemaphoreGuard&) = delete;
  BoundedSemaphoreGuard& operator=(const BoundedSemaphoreGuard&) = delete;

private:
  BoundedSemaphore sem_;
};

}  // namespace weave
