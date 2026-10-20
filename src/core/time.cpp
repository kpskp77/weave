// 时间换算：飞秒 → 后端 tick（向上取整，保证至少睡够；非正时长保持 0 tick）。
// tick 率来自 port 契约，后端无关。

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
  // 非正时长 → 0 tick（≡ asyncio：delay <= 0 只是让出一次）。必须显式夹住：
  // port 线格式里负 tick 表示"无超时腿"，漏下去会让 sleep 变成永久阻塞。
  if (d.count() <= 0) return 0;
  constexpr __int128 kFsPerSec = static_cast<__int128>(1000000000000000LL);
  const __int128 num = static_cast<__int128>(d.count()) * ticks_per_sec();
  return static_cast<std::int64_t>((num + kFsPerSec - 1) / kFsPerSec);
}

}  // namespace detail
}  // namespace weave
