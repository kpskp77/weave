// Communication primitive: Barrier / BrokenBarrierError
//
// Built on Condition (which is itself built on Lock + Event + the single blocking point), so the
// backend needs no changes. The state machine is one-to-one with asyncio.Barrier (CPython 3.12+):
//
//   FILLING    accepting parties; `count` parties are inside the barrier
//   DRAINING   the barrier tripped: the released parties are still leaving; new arrivals wait
//              until the generation has fully drained before joining the next one
//   RESETTING  reset() was called while parties were inside: they fail with BrokenBarrierError and
//              the barrier returns to FILLING once the last of them has left
//   BROKEN     abort() was called: waiting and all later parties fail with BrokenBarrierError
//
// Three invariants (check each one when editing this):
//   1. Every state transition happens inside one critical section (the Condition's lock), and no
//      critical section here suspends except through Condition::wait() — which drops the lock
//      across the suspension and re-acquires it before returning. The barrier's internal lock is
//      therefore never held across a suspension, so abort()/reset() (which merely take it) can
//      never block and stay plain member functions.
//   2. A party that enters the barrier always leaves it exactly once, on every path (trip,
//      BrokenBarrierError, CancelledError). Leaving is what decrements `count` and, when it reaches
//      0, returns DRAINING/RESETTING to FILLING and wakes the arrivals that are waiting on the
//      drain gate — forgetting it would wedge the barrier forever.
//   3. Cancellation neither breaks nor corrupts the barrier (matching asyncio, and unlike
//      threading.Barrier): the cancelled party unwinds through invariant 2, and the remaining
//      parties keep waiting; only abort()/reset() releases them.

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <weave/barrier.hpp>
#include <weave/locks.hpp>

#include "core_internal.hpp"

namespace weave {

// Key function (see barrier.hpp): emitted here, exactly once, so every consumer TU keeps only an
// undefined reference and a cross-DSO catch (the -sv_lib boundary) matches by typeinfo pointer.
BrokenBarrierError::~BrokenBarrierError() = default;

namespace {

enum class State { FILLING, DRAINING, RESETTING, BROKEN };

}  // namespace

struct Barrier::Impl {
  Condition cond;  // Guards state/count and carries the release/recheck wakeup
  std::size_t parties = 1;
  State state = State::FILLING;
  std::size_t count = 0;  // Parties currently inside the barrier (waiting, or released and leaving)

  // Joins the current generation and blocks until it trips; returns this party's arrival index.
  // Must be called with the lock held; throws BrokenBarrierError if the barrier is aborted or reset
  // while this party waits.
  std::size_t fill() {
    const std::size_t index = count;
    ++count;
    if (index + 1 == parties) {
      // The parties-th arrival: trip the barrier and release the whole generation at once. The
      // state moves to DRAINING, so arrivals belonging to the next generation wait until this one
      // has left (generations never overlap).
      state = State::DRAINING;
      cond.notify_all();
      return index;
    }
    // Wait for the trip; wakeups are re-checked in a loop, because notify_all also serves the
    // drain gate and abort()/reset(), and spurious wakeups must not be mistaken for a release.
    while (state == State::FILLING) cond.wait();
    // The message is asyncio's _wait() wording (a party that was inside when the barrier broke or
    // was reset); "Barrier aborted" in wait() below is asyncio's _block() wording (a party
    // arriving after the abort).
    if (state == State::BROKEN || state == State::RESETTING)
      throw BrokenBarrierError("Abort or reset of barrier");
    return index;
  }

  // Leaves the barrier (must be called with the lock held, exactly once per entered party): the
  // last party out returns DRAINING/RESETTING to FILLING and wakes the parties that arrived while
  // the previous generation was draining.
  void leave() {
    --count;
    if (count == 0) {
      if (state == State::DRAINING || state == State::RESETTING) state = State::FILLING;
      cond.notify_all();
    }
  }
};

Barrier::Barrier(std::size_t parties) : impl_(std::make_shared<Impl>()) {
  // ≡ asyncio's ValueError("parties must be >= 1")
  if (parties == 0) throw std::invalid_argument("weave: Barrier requires parties >= 1");
  impl_->parties = parties;
}

std::size_t Barrier::wait() {
  Impl& s = *impl_;
  s.cond.acquire();
  // Drain gate: a party arriving while the previous generation is still leaving, or while a reset()
  // is being applied to the parties inside, waits first and then joins the (new) filling
  // generation. CancelledError here leaves the barrier untouched — no slot was taken yet.
  try {
    while (s.state == State::DRAINING || s.state == State::RESETTING) s.cond.wait();
    if (s.state == State::BROKEN) throw BrokenBarrierError("Barrier aborted");
  } catch (...) {
    s.cond.release();
    throw;
  }
  // fill() may throw (BrokenBarrierError / CancelledError) only after it has taken a slot, so the
  // leaving bookkeeping must run on that path too — that is invariant 2 above.
  auto leave_and_release = detail::on_scope_exit([&s] {
    s.leave();
    s.cond.release();
  });
  return s.fill();
}

void Barrier::abort() {
  Impl& s = *impl_;
  s.cond.acquire();
  s.state = State::BROKEN;  // Sticky: no automatic recovery, only reset() clears it
  s.cond.notify_all();      // The parties inside fail with BrokenBarrierError
  s.cond.release();
}

void Barrier::reset() {
  Impl& s = *impl_;
  s.cond.acquire();
  if (s.count > 0) {
    // Parties are inside: they fail, and the last one to leave returns the state to FILLING (see
    // leave()). Keeping RESETTING sticky until then is what stops the failed generation's parties
    // from being confused with the next one's.
    if (s.state != State::RESETTING) s.state = State::RESETTING;
  } else {
    // Idle (including a broken barrier): back to the empty filling state immediately.
    s.state = State::FILLING;
  }
  s.cond.notify_all();  // Wake the parties inside (BrokenBarrierError) / waiters on the drain gate
  s.cond.release();
}

std::size_t Barrier::parties() const noexcept {
  return impl_->parties;
}

std::size_t Barrier::n_waiting() const noexcept {
  // Matching asyncio: parties that were released but have not left the barrier yet are not
  // "waiting".
  return impl_->state == State::FILLING ? impl_->count : 0;
}

bool Barrier::broken() const noexcept {
  return impl_->state == State::BROKEN;
}

}  // namespace weave
