// Every: a periodic cadence on a fixed time grid (include/weave/every.hpp). Core-level: it is
// built entirely from time queries plus the blocking point, so it needs no port change.

#include <stdexcept>
#include <weave/every.hpp>

#include "core_internal.hpp"

namespace weave {

Every::Every(duration period) : period_(period) {
  if (period.count() <= 0) {
    throw std::invalid_argument("weave::Every: period must be positive");
  }
  deadline_ = now();  // Placeholder: the real anchor is taken on the first next()/wait()
}

time_point Every::next() {
  const time_point current = now();

  if (!anchored_) {
    anchored_ = true;
    deadline_ = current + period_;
    return deadline_;
  }

  deadline_ = deadline_ + period_;

  // A consumer that overran by a whole period or more would otherwise fire the missed ticks back to
  // back, which is the one thing a cadence must not do: give up on them and re-anchor instead.
  if (current - deadline_ >= period_) {
    ++drift_;
    deadline_ = current + period_;
  }
  return deadline_;
}

void Every::wait() {
  wait_until(next());
}

}  // namespace weave
