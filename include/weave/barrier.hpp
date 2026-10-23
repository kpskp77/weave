#pragma once

// Communication primitive: Barrier / BrokenBarrierError, whose semantics match asyncio.Barrier
// (the four-state machine used from CPython 3.12 on: filling / draining / resetting / broken).
// A barrier is a fixed-party rendezvous point: every party calls wait(), the call blocks until the
// `parties`-th arrival, and then all of them are released at the same time — each one receiving a
// distinct index that tells it which arrival it was.
// Everything is built on Condition + Event + the single blocking point, so the backend needs no
// changes.
// Handle semantics match Lock/Event: copies share the same barrier, they are not deep copies.

#include <cstddef>
#include <memory>
#include <stdexcept>

namespace weave {

// ── BrokenBarrierError (asyncio.BrokenBarrierError) ──────────────────────────
//
// The barrier-broken signal: thrown by wait() on a barrier that was abort()ed, and injected into
// the parties that were already inside a barrier which was then abort()ed or reset().
// Unlike CancelledError it derives from std::exception — asyncio's BrokenBarrierError is a
// RuntimeError (an ordinary Exception), i.e. a normal failure path that a generic
// catch (const std::exception&) backstop may handle.
//
// The destructor is declared here and defined **out-of-line** (src/core/barrier.cpp): it is the
// key function of this type, so the vtable/typeinfo is emitted exactly once in libweave.so and
// consumer TUs keep only undefined references. This is the same ABI requirement as
// CancelledError::what() / TimeoutError::what() (see the "ABI notes" section of docs/backends.md):
// if it degraded into a per-TU weak definition, libc++abi's pointer comparison would make cross-DSO
// catch silently mismatch (not even the linker would complain).
class BrokenBarrierError : public std::runtime_error {
public:
  explicit BrokenBarrierError(const char* what_arg) : std::runtime_error(what_arg) {}
  ~BrokenBarrierError() override;
};

// ── Barrier (asyncio.Barrier) ───────────────────────────────────────────────
//
// Fixed-party rendezvous point:
//   · Barrier(parties) requires parties >= 1, otherwise throws std::invalid_argument
//     (≡ asyncio's ValueError("parties must be >= 1"));
//   · wait(): increments the number of parties inside the barrier; the parties-1 first arrivals
//     block (the only suspension point), the parties-th arrival releases the whole generation at
//     once (≡ asyncio's notify_all). Every party gets back a unique index in [0, parties) equal to
//     its arrival order within the generation — matching asyncio.wait()'s return value, which a
//     party can e.g. use to pick a leader;
//   · the barrier is reusable: the generation that just tripped returns to "filling" as soon as its
//     last party has left the barrier. An arrival during that drain window waits first and then
//     joins the next generation (so a new generation never overlaps the old one);
//   · abort(): puts the barrier into the broken state. Parties currently waiting — and every later
//     wait(), no matter when — throw BrokenBarrierError, and n_waiting() drops to 0. The broken
//     state is sticky: the barrier is not revived automatically, only reset() clears it;
//   · reset(): returns the barrier to the empty filling state. Parties that are inside at that
//     moment throw BrokenBarrierError (≡ asyncio's "Abort or reset of barrier"); once the last of
//     them has left, the barrier is filling again and reusable. On an idle (or broken) barrier it
//     is a no-op that clears the broken state;
//   · parties(): the number of parties required to trip the barrier (immutable);
//   · n_waiting(): the number of parties currently waiting at the barrier — 0 unless the barrier is
//     filling, matching asyncio (the parties that are released but have not left yet are not
//     "waiting");
//   · broken(): whether the barrier is in the broken state.
//
// Cancellation follows asyncio rather than threading.Barrier: a cancelled party leaves the barrier
// (CancelledError is injected at the blocking point; the generation bookkeeping and the drain gate
// stay consistent, so the barrier is neither broken nor corrupted), and the remaining parties keep
// waiting. Releasing them is the caller's job — abort() or reset() — exactly as in asyncio.
//
// abort()/reset() are plain non-suspending member functions even though asyncio spells them
// `async def`: the internal critical sections of a barrier never suspend (the only suspension point
// is the wait for the release, and Condition::wait drops the lock across it), and with a single
// active process per instant the barrier's internal lock is never contended, so acquiring it cannot
// block. asyncio's `async with barrier:` has no C++ counterpart: a guard cannot hand back wait()'s
// index, so call wait() explicitly.
class Barrier {
public:
  explicit Barrier(std::size_t parties);

  // Blocks until `parties` parties have arrived; returns this party's unique arrival index in
  // [0, parties). Throws BrokenBarrierError if the barrier is (or becomes) broken or was reset
  // while this party waited, and CancelledError if the waiting party is cancelled.
  std::size_t wait();

  // Breaks the barrier: waiting parties and all later wait() calls throw BrokenBarrierError.
  // No suspension.
  void abort();

  // Returns the barrier to the empty filling state; parties inside right now throw
  // BrokenBarrierError. Also clears the broken state. No suspension.
  void reset();

  [[nodiscard]] std::size_t parties() const noexcept;    // Parties required to trip the barrier
  [[nodiscard]] std::size_t n_waiting() const noexcept;  // Parties currently waiting (0 unless
                                                         // filling)
  [[nodiscard]] bool broken() const noexcept;

private:
  struct Impl;
  // Handle semantics match Lock/Event: copies share the same barrier, they are not deep copies.
  std::shared_ptr<Impl> impl_;
};

}  // namespace weave
