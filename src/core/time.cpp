// Time conversion: femtoseconds → backend ticks (rounded up, guaranteeing at least the requested
// duration; non-positive durations stay at 0 ticks) and the backend→femtosecond direction of the
// clock query. The tick rate comes from the port contract and is backend-agnostic.

#include <cstdint>
#include <weave/task.hpp>  // sleep (wait_until) — time.hpp itself stays below task.hpp
#include <weave/time.hpp>

#include "port.hpp"

namespace weave {
namespace detail {

std::int64_t ticks_per_sec() {
  static const std::int64_t v = port::ticks_per_sec();
  return v;
}

std::int64_t to_ticks(duration d) {
  // Non-positive duration → 0 ticks (≡ asyncio: delay <= 0 merely yields once). This must be
  // clamped explicitly: a negative tick means "no timeout leg" in the port wire format, and
  // letting it through turns sleep into a permanent block.
  if (d.count() <= 0) return 0;
  constexpr __int128 kFsPerSec = static_cast<__int128>(1000000000000000LL);
  const __int128 num = static_cast<__int128>(d.count()) * ticks_per_sec();
  return static_cast<std::int64_t>((num + kFsPerSec - 1) / kFsPerSec);
}

duration from_ticks(std::int64_t ticks) noexcept {
  try {
    const std::int64_t tps = ticks_per_sec();
    if (tps <= 0) return duration{};
    // Multiply before dividing (__int128, same family as to_ticks): tick * 1e15 may exceed int64
    // for a far-future time point, and the intermediate must not wrap.
    constexpr __int128 kFsPerSec = static_cast<__int128>(1000000000000000LL);
    __int128 fs = static_cast<__int128>(ticks) * kFsPerSec / tps;
    if (fs > INT64_MAX) return duration(INT64_MAX);  // saturate, never throw from a clock query
    if (fs < INT64_MIN) return duration(INT64_MIN);
    return duration(static_cast<std::int64_t>(fs));
  } catch (...) {
    return duration{};
  }
}

}  // namespace detail

std::int64_t now_ticks() noexcept {
  // Monotonic clamp: the contract only promises a query, not a perfectly monotonic one, and every
  // consumer of the clock (deadline arithmetic, elapsed-time measurement) would misbehave on a
  // value going backwards. A backend that fails the query leaves the clock frozen at the last
  // value it reported.
  static std::int64_t last = 0;
  try {
    const std::int64_t v = port::now_ticks();
    if (v > last) last = v;
  } catch (...) {
    // Query unavailable: keep the last observed time (no exception may cross a DPI boundary).
  }
  return last;
}

duration now() noexcept {
  return detail::from_ticks(now_ticks());
}

void wait_until(time_point deadline) {
  const duration remaining = deadline.time_since_epoch() - now();
  // Explicit early return rather than relying on to_ticks' 0-clamp: sleep(0) is a real yield, while
  // an expired deadline must not suspend at all.
  if (remaining.count() <= 0) return;
  sleep(remaining);
}

}  // namespace weave
