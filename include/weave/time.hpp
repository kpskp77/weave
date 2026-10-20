#pragma once

// 时间表示。内部固定为 int64 飞秒（10^-15 s，对齐 SV 最大精度），
// 提交给后端时按 tick 换算（向上取整，保证至少睡够；换算见 src/core/time.cpp）。
//
// 表示上限：int64 飞秒 ≈ 9223 s ≈ 2.56 h 仿真时间。越界的字面量在编译期报错
// （见 detail::fs_scaled），不会静默回绕成负数。
//
// 算术（+ - * / 与整数缩放）用于把时延算成变量，例如 `rounds * 100_ns`：
//   · 结果同样不得越界：越界抛 std::overflow_error、除零抛 std::domain_error。
//     与字面量越界的编译期守卫同一原则——不静默回绕（回绕会变成负 tick，
//     在 port 线格式里恰好意味着"无超时腿"，即睡死）。
//   · duration 是**有符号**量：d1 - d2 可为负，表示差值/偏置。非正时长交给
//     sleep / wait_for 时按 0 tick 处理（≡ asyncio：delay <= 0 只让出一次），
//     不会退化成"无超时"或负延迟。
//   · 只提供**整数**缩放，不提供浮点乘除：仿真时间必须可复现，浮点比例会在两端
//     引入舍入差异（需要比例时先在整数域换算，如 `n * d / 8`）。

#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace weave {

namespace detail {

// 字面量换算 + 范围检查。越界时在常量求值路径上 throw → 不是常量表达式，
// 因此 `sleep(10000_s)` 这类超过 int64 飞秒上限的写法直接编译失败，
// 而不会乘出负数静默交给后端。
constexpr std::int64_t fs_scaled(unsigned long long v, unsigned long long fs_per_unit) {
  if (v > static_cast<unsigned long long>(INT64_MAX) / fs_per_unit)
    throw std::overflow_error("weave: duration literal exceeds int64 femtoseconds (~2.56 h)");
  return static_cast<std::int64_t>(v * fs_per_unit);
}

// 运行期算术的越界检查，与 fs_scaled 的编译期守卫同一原则。
// 判界一律用除法完成（不经 __int128），公共头对编译器扩展零依赖；
// 各分支里作为除数的操作数都严格为正或为 -1 之外的安全值，故不产生新的溢出。
constexpr std::int64_t checked_add(std::int64_t a, std::int64_t b) {
  if (b > 0 ? a > INT64_MAX - b : a < INT64_MIN - b)
    throw std::overflow_error("weave: duration addition exceeds int64 femtoseconds (~2.56 h)");
  return a + b;
}

constexpr std::int64_t checked_sub(std::int64_t a, std::int64_t b) {
  if (b > 0 ? a < INT64_MIN + b : a > INT64_MAX + b)
    throw std::overflow_error("weave: duration subtraction exceeds int64 femtoseconds (~2.56 h)");
  return a - b;
}

constexpr std::int64_t checked_mul(std::int64_t a, std::int64_t b) {
  if (a == 0 || b == 0) return 0;
  const bool overflow = a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)
                              : (b > 0 ? a < INT64_MIN / b : b < INT64_MAX / a);
  if (overflow)
    throw std::overflow_error("weave: duration scaling exceeds int64 femtoseconds (~2.56 h)");
  return a * b;
}

constexpr std::int64_t checked_div(std::int64_t a, std::int64_t b) {
  if (b == 0) throw std::domain_error("weave: duration division by zero");
  if (a == INT64_MIN && b == -1)
    throw std::overflow_error("weave: duration division exceeds int64 femtoseconds (~2.56 h)");
  return a / b;
}

}  // namespace detail

class duration {
public:
  constexpr explicit duration(int64_t fs = 0) : fs_(fs) {}
  constexpr int64_t count() const { return fs_; }  // 飞秒

  // 复合赋值：便于累加变量时延（total += chunk）。直接走 detail::checked_*，
  // 不写 *this = *this + rhs —— 类体内看不到类后声明的自由 operator。
  constexpr duration& operator+=(duration rhs) {
    fs_ = detail::checked_add(fs_, rhs.fs_);
    return *this;
  }
  constexpr duration& operator-=(duration rhs) {
    fs_ = detail::checked_sub(fs_, rhs.fs_);
    return *this;
  }
  constexpr duration& operator*=(int64_t k) {
    fs_ = detail::checked_mul(fs_, k);
    return *this;
  }
  constexpr duration& operator/=(int64_t k) {
    fs_ = detail::checked_div(fs_, k);
    return *this;
  }

  // 浮点操作数显式删除（见下方自由 operator* 的说明）。
  template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
  duration& operator*=(T) = delete;
  template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
  duration& operator/=(T) = delete;

private:
  int64_t fs_;
};

// 加减：两段时长拼接/相减，结果可负（差值）。
constexpr duration operator+(duration lhs, duration rhs) {
  return duration(detail::checked_add(lhs.count(), rhs.count()));
}

constexpr duration operator-(duration lhs, duration rhs) {
  return duration(detail::checked_sub(lhs.count(), rhs.count()));
}

// 取负：便于写偏置（d + (-offset)）与差值翻转。
constexpr duration operator-(duration d) {
  return duration(detail::checked_sub(0, d.count()));
}

// 整数缩放：k * d 与 d * k 等价（变量时延的主要写法，如 rounds * 100_ns）。
constexpr duration operator*(duration d, int64_t k) {
  return duration(detail::checked_mul(d.count(), k));
}

constexpr duration operator*(int64_t k, duration d) {
  return d * k;
}

// 整除：向零截断（C++ 整数除法语义），除零抛 std::domain_error。
constexpr duration operator/(duration d, int64_t k) {
  return duration(detail::checked_div(d.count(), k));
}

// 时长之比：整数商，向零截断（如 1_us / 100_ns == 10）。
// 返回 int64_t 而非 duration——量纲是"倍数"，不是时长。
constexpr int64_t operator/(duration lhs, duration rhs) {
  return detail::checked_div(lhs.count(), rhs.count());
}

// 浮点操作数一律**编译期拒绝**：`d * 1.5` 若靠隐式转换走整数重载会静默变成
// `d * 1`（与内建类型 int64_t * 1.5 的结果不同，且毫无提示）。删除的模板只对浮点
// 启用，因此不影响 `d * 2`（int 仍落到 int64_t 重载）；需要比例时先在整数域
// 换算（`n * d / 8`）或显式 `duration(static_cast<int64_t>(...))` 明确取整口径。
template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
duration operator*(duration, T) = delete;
template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
duration operator*(T, duration) = delete;
template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
duration operator/(duration, T) = delete;

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
