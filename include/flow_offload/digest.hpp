// Flow Offload Scheduler - SHA-256 digest used for integrity and canonical identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_DIGEST_HPP
#define FLOW_OFFLOAD_DIGEST_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace flow_offload {

/// A 256-bit digest. Comparison is bytewise on the canonical big-endian order.
struct Sha256Digest {
  std::array<std::uint8_t, 32> bytes{};

  [[nodiscard]] std::string hex() const;
  [[nodiscard]] bool is_zero() const noexcept;

  friend constexpr bool operator==(const Sha256Digest& a, const Sha256Digest& b) noexcept {
    return a.bytes == b.bytes;
  }
  friend constexpr bool operator!=(const Sha256Digest& a, const Sha256Digest& b) noexcept {
    return !(a == b);
  }
  friend constexpr std::strong_ordering operator<=>(const Sha256Digest& a,
                                                    const Sha256Digest& b) noexcept {
    for (std::size_t i = 0; i < a.bytes.size(); ++i) {
      if (a.bytes[i] != b.bytes[i]) {
        return a.bytes[i] <=> b.bytes[i];
      }
    }
    return std::strong_ordering::equal;
  }
};

/// Incremental SHA-256. Self-contained: no third-party dependency.
class Sha256 {
 public:
  Sha256() noexcept;

  void update(std::span<const std::byte> data) noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept;

  /// Finalizes and returns the digest. The object must not be reused afterwards.
  [[nodiscard]] Sha256Digest finish() noexcept;

  [[nodiscard]] static Sha256Digest of(std::span<const std::byte> data) noexcept;
  [[nodiscard]] static Sha256Digest of(std::string_view text) noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{};
  std::uint64_t total_bytes_{};
  bool finalized_{false};
};

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_DIGEST_HPP
