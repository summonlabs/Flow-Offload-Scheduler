// Flow Offload Scheduler - typed time and checked capacity arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_UNITS_HPP
#define FLOW_OFFLOAD_UNITS_HPP

#include <compare>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace flow_offload {

/// Nanoseconds since an epoch chosen by the caller. The runtime never reads a
/// wall clock: every decision is evaluated at an instant supplied by the caller,
/// which is what makes a decision reproducible from its inputs.
class Timestamp {
 public:
  constexpr Timestamp() noexcept = default;
  constexpr explicit Timestamp(std::uint64_t nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] constexpr std::uint64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return nanos_ != 0U; }

  friend constexpr bool operator==(Timestamp a, Timestamp b) noexcept { return a.nanos_ == b.nanos_; }
  friend constexpr bool operator!=(Timestamp a, Timestamp b) noexcept { return !(a == b); }
  friend constexpr std::strong_ordering operator<=>(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ <=> b.nanos_;
  }

 private:
  std::uint64_t nanos_{};
};

class Duration {
 public:
  constexpr Duration() noexcept = default;
  constexpr explicit Duration(std::uint64_t nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] constexpr std::uint64_t nanos() const noexcept { return nanos_; }

  friend constexpr bool operator==(Duration a, Duration b) noexcept { return a.nanos_ == b.nanos_; }
  friend constexpr std::strong_ordering operator<=>(Duration a, Duration b) noexcept {
    return a.nanos_ <=> b.nanos_;
  }

 private:
  std::uint64_t nanos_{};
};

/// Saturating elapsed time. An inverted pair yields zero rather than wrapping, so
/// arithmetic on externally supplied timestamps can never fabricate freshness.
[[nodiscard]] constexpr Duration saturating_elapsed(Timestamp later, Timestamp earlier) noexcept {
  if (later.nanos() < earlier.nanos()) {
    return Duration{0};
  }
  return Duration{later.nanos() - earlier.nanos()};
}

template <class T>
[[nodiscard]] constexpr bool checked_add(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_add requires an unsigned type");
  if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
    return false;
  }
  out = static_cast<T>(a + b);
  return true;
}

template <class T>
[[nodiscard]] constexpr bool checked_mul(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_mul requires an unsigned type");
  if (a == 0U || b == 0U) {
    out = 0;
    return true;
  }
  if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
    return false;
  }
  out = static_cast<T>(a * b);
  return true;
}

/// Upper bound on the reported utilisation ratio. Values above this are clamped
/// and flagged saturated; the sentinel is deliberately larger than 100% so an
/// oversubscribed resource never sorts as if it were idle.
inline constexpr std::uint32_t kUtilisationPpmSaturated = 2000000U;

/// Utilisation of a resource in parts per million, floor-rounded.
///
/// Returns false only when p capacity is zero, because utilisation of a
/// zero-capacity resource is undefined and must never be reported as zero.
/// When p used exceeds p capacity the result is clamped to
/// kUtilisationPpmSaturated and p saturated is set.
///
/// Both operands are normalised by an exact common right shift before the
/// division, so the arithmetic cannot overflow and the result is bit-identical
/// on every platform.
[[nodiscard]] constexpr bool utilisation_ppm(std::uint64_t used, std::uint64_t capacity,
                                             std::uint32_t& out, bool& saturated) noexcept {
  constexpr std::uint64_t kMaxOperand = 0xFFFFFFFFULL;
  saturated = false;
  if (capacity == 0U) {
    return false;
  }
  std::uint64_t scaled_used = used;
  std::uint64_t scaled_capacity = capacity;
  while (scaled_used > kMaxOperand || scaled_capacity > kMaxOperand) {
    scaled_used >>= 1U;
    scaled_capacity >>= 1U;
  }
  if (scaled_capacity == 0U) {
    // The capacity was annihilated by the normalisation shift, so the ratio is
    // effectively unbounded. Report the saturated sentinel rather than zero.
    saturated = true;
    out = kUtilisationPpmSaturated;
    return true;
  }
  const std::uint64_t product = scaled_used * 1000000ULL;
  std::uint64_t ppm = product / scaled_capacity;
  if (ppm > static_cast<std::uint64_t>(kUtilisationPpmSaturated)) {
    ppm = static_cast<std::uint64_t>(kUtilisationPpmSaturated);
    saturated = true;
  }
  out = static_cast<std::uint32_t>(ppm);
  return true;
}

/// A four-dimensional capacity/utilisation vector. Dimensions are independent;
/// dominance is componentwise. There is deliberately no defaulted ordering,
/// because lexicographic order would silently mislead a capacity comparison.
struct CapacityVector {
  std::uint64_t packets_per_second{};
  std::uint64_t bytes_per_second{};
  std::uint64_t state_bytes{};
  std::uint64_t table_entries{};

  [[nodiscard]] static constexpr CapacityVector zero() noexcept { return CapacityVector{}; }

  [[nodiscard]] static constexpr CapacityVector saturated() noexcept {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    return CapacityVector{kMax, kMax, kMax, kMax};
  }

  [[nodiscard]] constexpr bool is_zero() const noexcept {
    return packets_per_second == 0U && bytes_per_second == 0U && state_bytes == 0U &&
           table_entries == 0U;
  }

  /// True when every component of *this is greater than or equal to *other.
  [[nodiscard]] constexpr bool dominates(const CapacityVector& other) const noexcept {
    return packets_per_second >= other.packets_per_second &&
           bytes_per_second >= other.bytes_per_second && state_bytes >= other.state_bytes &&
           table_entries >= other.table_entries;
  }

  /// True when any component of *this exceeds the corresponding component of *other.
  [[nodiscard]] constexpr bool exceeds_any(const CapacityVector& other) const noexcept {
    return packets_per_second > other.packets_per_second ||
           bytes_per_second > other.bytes_per_second || state_bytes > other.state_bytes ||
           table_entries > other.table_entries;
  }

  /// Deterministic total order used for canonical serialization and hashing only.
  [[nodiscard]] constexpr std::strong_ordering lexicographic(const CapacityVector& other) const noexcept {
    if (packets_per_second != other.packets_per_second) {
      return packets_per_second <=> other.packets_per_second;
    }
    if (bytes_per_second != other.bytes_per_second) {
      return bytes_per_second <=> other.bytes_per_second;
    }
    if (state_bytes != other.state_bytes) {
      return state_bytes <=> other.state_bytes;
    }
    return table_entries <=> other.table_entries;
  }

  friend constexpr bool operator==(const CapacityVector& a, const CapacityVector& b) noexcept {
    return a.packets_per_second == b.packets_per_second && a.bytes_per_second == b.bytes_per_second &&
           a.state_bytes == b.state_bytes && a.table_entries == b.table_entries;
  }
  friend constexpr bool operator!=(const CapacityVector& a, const CapacityVector& b) noexcept {
    return !(a == b);
  }
};

[[nodiscard]] constexpr bool checked_add(const CapacityVector& a, const CapacityVector& b,
                                         CapacityVector& out) noexcept {
  CapacityVector result{};
  if (!checked_add(a.packets_per_second, b.packets_per_second, result.packets_per_second) ||
      !checked_add(a.bytes_per_second, b.bytes_per_second, result.bytes_per_second) ||
      !checked_add(a.state_bytes, b.state_bytes, result.state_bytes) ||
      !checked_add(a.table_entries, b.table_entries, result.table_entries)) {
    return false;
  }
  out = result;
  return true;
}

[[nodiscard]] constexpr bool checked_sub(const CapacityVector& a, const CapacityVector& b,
                                         CapacityVector& out) noexcept {
  if (b.exceeds_any(a)) {
    return false;
  }
  out = CapacityVector{a.packets_per_second - b.packets_per_second,
                       a.bytes_per_second - b.bytes_per_second, a.state_bytes - b.state_bytes,
                       a.table_entries - b.table_entries};
  return true;
}

[[nodiscard]] constexpr CapacityVector saturating_sub(const CapacityVector& a,
                                                      const CapacityVector& b) noexcept {
  CapacityVector result{};
  result.packets_per_second =
      a.packets_per_second > b.packets_per_second ? a.packets_per_second - b.packets_per_second : 0U;
  result.bytes_per_second =
      a.bytes_per_second > b.bytes_per_second ? a.bytes_per_second - b.bytes_per_second : 0U;
  result.state_bytes = a.state_bytes > b.state_bytes ? a.state_bytes - b.state_bytes : 0U;
  result.table_entries = a.table_entries > b.table_entries ? a.table_entries - b.table_entries : 0U;
  return result;
}

/// Aggregate utilisation score: the maximum per-dimension utilisation in ppm.
/// Returns false when the capacity is zero in any dimension, in which case the
/// score is undefined rather than zero.
[[nodiscard]] constexpr bool aggregate_utilisation_ppm(const CapacityVector& used,
                                                       const CapacityVector& capacity,
                                                       std::uint32_t& out, bool& saturated) noexcept {
  const std::uint64_t used_parts[4] = {used.packets_per_second, used.bytes_per_second,
                                       used.state_bytes, used.table_entries};
  const std::uint64_t capacity_parts[4] = {capacity.packets_per_second, capacity.bytes_per_second,
                                           capacity.state_bytes, capacity.table_entries};
  std::uint32_t worst = 0;
  bool any_saturated = false;
  for (std::size_t i = 0; i < 4; ++i) {
    std::uint32_t ppm = 0;
    bool part_saturated = false;
    if (!utilisation_ppm(used_parts[i], capacity_parts[i], ppm, part_saturated)) {
      return false;
    }
    if (part_saturated) {
      any_saturated = true;
    }
    if (ppm > worst) {
      worst = ppm;
    }
  }
  out = worst;
  saturated = any_saturated;
  return true;
}

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_UNITS_HPP
