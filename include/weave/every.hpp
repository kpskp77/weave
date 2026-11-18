#pragma once

// Every: a periodic cadence that stays on a fixed time grid instead of drifting with the work it
// paces. Each wait() targets the next multiple of the period measured from the anchor, so a
// consumer that takes a little less than a period per turn wakes exactly on the grid, and a
// consumer that overruns by a whole period gives up on the ticks it missed instead of firing them
// back to back.
//
//   weave::Every tick(1_ms);
//   for (;;) {
//     tick.wait();        // suspend until the next grid point
//     poll_inputs();      // takes ~200us: the next wait still lands on the grid
//   }
//
// No asyncio counterpart — asyncio has no periodic timer primitive; the closest Python idiom is
// rebuilding loop.call_later per tick, which is exactly the "drifting" behaviour this type avoids
// by re-anchoring instead of accumulating error.
//
// Every is a value, not a handle: copying it copies the anchor and the drift counter, so two copies
// pace independently (an Event/Pulse copy shares state; a cadence must not).
//
// Cancellation: wait() is an ordinary blocking point, so a cancelled task throws CancelledError
// out of it. The anchor has already been advanced, so a caller that catches the cancellation and
// waits again resumes on the same grid (not one period later).

#include <cstdint>
#include <weave/time.hpp>

namespace weave {

class Every {
public:
  // period must be positive: a zero or negative cadence is a bug in the caller, so it has no
  // run-time meaning and is rejected at construction (std::invalid_argument).
  explicit Every(duration period);

  // The configured cadence.
  [[nodiscard]] duration period() const noexcept { return period_; }

  // How many times wait()/next() found itself a whole period or more late and re-anchored onto the
  // current time, i.e. how many grid ticks were skipped (not merely jittered). Zero means the
  // consumer kept up with every tick. Diagnostics only — the cadence does not depend on it.
  [[nodiscard]] std::uint64_t drift() const noexcept { return drift_; }

  // Advance the schedule by one period and return the deadline it now points at; never suspends.
  // The first call anchors at now() + period, so the first tick is one full period away.
  [[nodiscard]] time_point next();

  // wait_until(next()) — suspend until the next grid point. Requires task context.
  void wait();

private:
  duration period_;
  time_point deadline_;
  std::uint64_t drift_ = 0;
  bool anchored_ = false;
};

}  // namespace weave
