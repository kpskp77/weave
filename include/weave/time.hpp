#pragma once

// 时间表示。内部固定为 int64 飞秒（10^-15 s，对齐 SV 最大精度），
// 提交给后端时按 tick 换算（向上取整，保证至少睡够；换算见 src/core/time.cpp）。

#include <cstdint>

namespace weave {

class duration {
public:
  constexpr explicit duration(int64_t fs = 0) : fs_(fs) {}
  constexpr int64_t count() const { return fs_; }  // 飞秒
private:
  int64_t fs_;
};

namespace literals {
constexpr duration operator""_fs(unsigned long long v) {
  return duration(static_cast<int64_t>(v));
}
constexpr duration operator""_ps(unsigned long long v) {
  return duration(static_cast<int64_t>(v) * 1000LL);
}
constexpr duration operator""_ns(unsigned long long v) {
  return duration(static_cast<int64_t>(v) * 1000000LL);
}
constexpr duration operator""_us(unsigned long long v) {
  return duration(static_cast<int64_t>(v) * 1000000000LL);
}
constexpr duration operator""_ms(unsigned long long v) {
  return duration(static_cast<int64_t>(v) * 1000000000000LL);
}
constexpr duration operator""_s(unsigned long long v) {
  return duration(static_cast<int64_t>(v) * 1000000000000000LL);
}
}  // namespace literals

}  // namespace weave
