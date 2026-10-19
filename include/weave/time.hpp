#pragma once

// 时间表示。内部固定为 int64 飞秒（10^-15 s，对齐 SV 最大精度），
// 提交给后端时按 tick 换算（向上取整，保证至少睡够；换算见 src/core/time.cpp）。
//
// 表示上限：int64 飞秒 ≈ 9223 s ≈ 2.56 h 仿真时间。越界的字面量在编译期报错
// （见 detail::fs_scaled），不会静默回绕成负数。

#include <cstdint>
#include <stdexcept>

namespace weave {

class duration {
public:
  constexpr explicit duration(int64_t fs = 0) : fs_(fs) {}
  constexpr int64_t count() const { return fs_; }  // 飞秒
private:
  int64_t fs_;
};

namespace detail {

// 字面量换算 + 范围检查。越界时在常量求值路径上 throw → 不是常量表达式，
// 因此 `sleep(10000_s)` 这类超过 int64 飞秒上限的写法直接编译失败，
// 而不会乘出负数静默交给后端。
constexpr std::int64_t fs_scaled(unsigned long long v, unsigned long long fs_per_unit) {
  if (v > static_cast<unsigned long long>(INT64_MAX) / fs_per_unit)
    throw std::overflow_error("weave: duration literal exceeds int64 femtoseconds (~2.56 h)");
  return static_cast<std::int64_t>(v * fs_per_unit);
}

}  // namespace detail

namespace literals {
constexpr duration operator""_fs(unsigned long long v) {
  return duration(detail::fs_scaled(v, 1));
}
constexpr duration operator""_ps(unsigned long long v) {
  return duration(detail::fs_scaled(v, 1000ULL));
}
constexpr duration operator""_ns(unsigned long long v) {
  return duration(detail::fs_scaled(v, 1000000ULL));
}
constexpr duration operator""_us(unsigned long long v) {
  return duration(detail::fs_scaled(v, 1000000000ULL));
}
constexpr duration operator""_ms(unsigned long long v) {
  return duration(detail::fs_scaled(v, 1000000000000ULL));
}
constexpr duration operator""_s(unsigned long long v) {
  return duration(detail::fs_scaled(v, 1000000000000000ULL));
}
}  // namespace literals

}  // namespace weave
