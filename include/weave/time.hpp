#pragma once

// Time representation. Internally always int64 femtoseconds (10^-15 s, matching SV's maximum
// precision), converted to ticks when submitted to the backend (rounded up, guaranteeing at least
// the requested duration; conversion in src/core/time.cpp).
//
// Representable ceiling: int64 femtoseconds ≈ 9223 s ≈ 2.56 h of simulation time. An
// out-of-range literal is a compile error (see detail::fs_scaled) and never wraps silently into
// a negative value.
//
// Arithmetic (+ - * / and integer scaling) exists so delays can be computed from variables, e.g.
// `rounds * 100_ns`:
//   · Results must not overflow either: overflow throws std::overflow_error and division by zero
//     throws std::domain_error. Same principle as the compile-time guard on literals — no silent
//     wrapping (a wrap becomes a negative tick, which in the port wire format means exactly
//     "no timeout leg", i.e. sleeping forever).
//   · duration is a **signed** quantity: d1 - d2 may be negative, expressing a difference/offset.
//     A non-positive duration handed to sleep / wait_for is treated as 0 ticks (≡ asyncio:
//     delay <= 0 yields once) and never degenerates into "no timeout" or a negative delay.
//   · Only **integer** scaling is provided, no floating-point multiply/divide: simulation time
//     must be reproducible, and a floating-point ratio introduces rounding differences on both
//     ends (when a ratio is needed, convert in the integer domain first, e.g. `n * d / 8`).

#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace weave {

namespace detail {

// Literal conversion + range check. On overflow it throws on the constant-evaluation path → not
// a constant expression, so something like `sleep(10000_s)` that exceeds the int64 femtosecond
// ceiling fails to compile instead of multiplying into a negative value handed to the backend
// silently.
constexpr std::int64_t fs_scaled(unsigned long long v, unsigned long long fs_per_unit) {
  if (v > static_cast<unsigned long long>(INT64_MAX) / fs_per_unit)
    throw std::overflow_error("weave: duration literal exceeds int64 femtoseconds (~2.56 h)");
  return static_cast<std::int64_t>(v * fs_per_unit);
}

// Range checks for runtime arithmetic, same principle as fs_scaled's compile-time guard.
// Bounds are always tested by division (never via __int128), keeping the public header free of
// compiler extensions; the divisors in each branch are strictly positive or safe values other
// than -1, so no new overflow is introduced.
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
  constexpr int64_t count() const { return fs_; }  // femtoseconds

  // Compound assignment: convenient for accumulating a delay (total += chunk). Goes straight
  // through detail::checked_*, without writing *this = *this + rhs — the free operators declared
  // after the class are not visible inside the class body.
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

  // Floating-point operands are explicitly deleted (see the free operator* below).
  template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
  duration& operator*=(T) = delete;
  template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
  duration& operator/=(T) = delete;

private:
  int64_t fs_;
};

// Addition/subtraction: concatenating/differencing two spans, the result may be negative (a
// difference).
constexpr duration operator+(duration lhs, duration rhs) {
  return duration(detail::checked_add(lhs.count(), rhs.count()));
}

constexpr duration operator-(duration lhs, duration rhs) {
  return duration(detail::checked_sub(lhs.count(), rhs.count()));
}

// Negation: makes offsets easy to write (d + (-offset)) and flips differences.
constexpr duration operator-(duration d) {
  return duration(detail::checked_sub(0, d.count()));
}

// Integer scaling: k * d and d * k are equivalent (the main way to build variable delays, e.g.
// rounds * 100_ns).
constexpr duration operator*(duration d, int64_t k) {
  return duration(detail::checked_mul(d.count(), k));
}

constexpr duration operator*(int64_t k, duration d) {
  return d * k;
}

// Integer division: truncates toward zero (C++ integer division semantics); division by zero
// throws std::domain_error.
constexpr duration operator/(duration d, int64_t k) {
  return duration(detail::checked_div(d.count(), k));
}

// Ratio of durations: the integer quotient, truncating toward zero (e.g. 1_us / 100_ns == 10).
// Returns int64_t rather than duration — the quantity is a "multiple", not a duration.
constexpr int64_t operator/(duration lhs, duration rhs) {
  return detail::checked_div(lhs.count(), rhs.count());
}

// Floating-point operands are always **rejected at compile time**: if `d * 1.5` went through an
// implicit conversion to the integer overload it would silently become `d * 1` (a different
// result from the built-in int64_t * 1.5, with no hint whatsoever). The deleted templates are
// enabled only for floating-point types, so they do not affect `d * 2` (int still picks the
// int64_t overload); when a ratio is needed, convert in the integer domain (`n * d / 8`) or write
// an explicit `duration(static_cast<int64_t>(...))` stating the rounding convention.
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
