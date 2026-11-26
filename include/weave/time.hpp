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
//   · A duration is also a *difference* of time points: `later - earlier` yields the elapsed
//     duration (see time_point below).

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

// ── Absolute simulation time ──────────────────────────────────────────────────────────────────
// A time point is the elapsed simulation time since the epoch (simulation start, t = 0). The
// backend is asked for its current time in ticks and converted at the same tick rate as the
// fs→tick conversion, so `wait_until(now() + d)` and `sleep(d)` land on the same tick. The
// resolution is the backend tick (1 ps for both shipped backends), i.e. sub-tick time does not
// exist: consecutive statements between two suspension points observe the same `now()` (the
// simulators' own time does not advance either until someone suspends).
//
// Reading the clock is the port's 7th primitive — the only non-suspending one. Every
// deadline-bearing primitive above the core (periodic scheduling, timeout diagnostics, deadline
// arithmetic) is built on it, which is why it belongs to the contract even though nothing suspends.

// Current backend time in ticks. Never throws and never decreases: a backend query failure is
// frozen at the last observed value (0 before the first successful query), so callers may rely
// on monotonicity.
std::int64_t now_ticks() noexcept;

// Current simulation time as a duration since the epoch. Saturates at INT64_MAX instead of
// throwing — this is a clock query, not arithmetic on caller-supplied values.
duration now() noexcept;

// An absolute point in time, stored as a duration since the epoch. The converting constructor is
// deliberately implicit so a deadline reads naturally: `wait_until(now() + 1_ms)` (a duration
// used where a deadline is expected is always "time since t = 0", never a delay).
class time_point {
public:
  constexpr time_point() = default;
  constexpr time_point(duration since_epoch) : since_epoch_(since_epoch) {}

  constexpr duration time_since_epoch() const { return since_epoch_; }

  constexpr time_point& operator+=(duration d) {
    since_epoch_ += d;
    return *this;
  }
  constexpr time_point& operator-=(duration d) {
    since_epoch_ -= d;
    return *this;
  }

private:
  duration since_epoch_{};
};

constexpr time_point operator+(time_point tp, duration d) {
  return time_point(tp.time_since_epoch() + d);
}
constexpr time_point operator+(duration d, time_point tp) {
  return tp + d;
}
constexpr time_point operator-(time_point tp, duration d) {
  return time_point(tp.time_since_epoch() - d);
}
// Difference of two time points: the elapsed duration (may be negative).
constexpr duration operator-(time_point lhs, time_point rhs) {
  return lhs.time_since_epoch() - rhs.time_since_epoch();
}

constexpr bool operator==(time_point lhs, time_point rhs) {
  return lhs.time_since_epoch().count() == rhs.time_since_epoch().count();
}
constexpr bool operator!=(time_point lhs, time_point rhs) {
  return !(lhs == rhs);
}
constexpr bool operator<(time_point lhs, time_point rhs) {
  return lhs.time_since_epoch().count() < rhs.time_since_epoch().count();
}
constexpr bool operator>(time_point lhs, time_point rhs) {
  return rhs < lhs;
}
constexpr bool operator<=(time_point lhs, time_point rhs) {
  return !(rhs < lhs);
}
constexpr bool operator>=(time_point lhs, time_point rhs) {
  return !(lhs < rhs);
}

// Suspends until backend time reaches `deadline` (≡ asyncio `loop.call_at`; the equivalent delay
// form is `sleep(max(0, deadline - now()))`). A deadline at or before now() returns **without
// suspending** — not even a 0-tick yield: there is nothing to wait for, and a caller that wants to
// yield must say `sleep(0)`. Like every suspension point it must be called from a task context
// (outside one it throws std::logic_error); `now()`/`now_ticks()` are the parts callable from
// anywhere.
void wait_until(time_point deadline);

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
