// Time conversion: femtoseconds → backend ticks (rounded up, guaranteeing at least the requested
// duration; non-positive durations stay at 0 ticks). The tick rate comes from the port contract
// and is backend-agnostic.

#include <cstdint>
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

}  // namespace detail
}  // namespace weave
