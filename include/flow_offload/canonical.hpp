// Flow Offload Scheduler - canonical deterministic encoding.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_CANONICAL_HPP
#define FLOW_OFFLOAD_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "flow_offload/limits.hpp"
#include "flow_offload/reason.hpp"

namespace flow_offload {

struct Sha256Digest;

/// Canonical byte writer. Integers are big-endian fixed width, variable-length
/// fields carry a 32-bit length prefix, and collections are always written in a
/// canonical order chosen by the caller. Two runs that observe the same state
/// therefore produce byte-identical output.
///
/// The writer is bounded: once the configured limit would be exceeded the writer
/// latches a failure and appends nothing further, so an oversized value is an
/// observable failure rather than an unbounded allocation.
class CanonicalWriter {
 public:
  explicit CanonicalWriter(std::size_t max_bytes = kMaxCanonicalBytes) noexcept;

  void u8(std::uint8_t value) noexcept;
  void u16(std::uint16_t value) noexcept;
  void u32(std::uint32_t value) noexcept;
  void u64(std::uint64_t value) noexcept;
  void i64(std::int64_t value) noexcept;
  void boolean(bool value) noexcept;

  /// Raw bytes with no length prefix.
  void fixed(std::span<const std::byte> data) noexcept;
  /// 32-byte digest.
  void digest(const Sha256Digest& value) noexcept;
  /// 32-bit length prefix followed by the bytes.
  void blob(std::span<const std::byte> data) noexcept;
  /// 32-bit length prefix followed by the UTF-8 bytes.
  void text(std::string_view value) noexcept;

  [[nodiscard]] bool ok() const noexcept { return !overflow_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return std::span<const std::byte>(buffer_.data(), buffer_.size());
  }
  [[nodiscard]] const std::vector<std::byte>& storage() const noexcept { return buffer_; }

 private:
  void put(const void* data, std::size_t size) noexcept;

  std::vector<std::byte> buffer_;
  std::size_t limit_;
  bool overflow_{false};
};

/// Canonical byte reader. Every read is bounds checked; the first failure latches
/// a stable reason code and all subsequent reads become no-ops, so a decode can
/// never half-succeed silently.
class CanonicalReader {
 public:
  explicit CanonicalReader(std::span<const std::byte> data) noexcept;

  [[nodiscard]] bool u8(std::uint8_t& out) noexcept;
  [[nodiscard]] bool u16(std::uint16_t& out) noexcept;
  [[nodiscard]] bool u32(std::uint32_t& out) noexcept;
  [[nodiscard]] bool u64(std::uint64_t& out) noexcept;
  [[nodiscard]] bool i64(std::int64_t& out) noexcept;
  [[nodiscard]] bool boolean(bool& out) noexcept;

  [[nodiscard]] bool fixed(std::span<std::byte> out) noexcept;
  [[nodiscard]] bool digest(Sha256Digest& out) noexcept;
  [[nodiscard]] bool blob(std::vector<std::byte>& out, std::size_t max_bytes) noexcept;
  [[nodiscard]] bool text(std::string& out, std::size_t max_bytes) noexcept;
  [[nodiscard]] bool skip(std::size_t count) noexcept;

  /// Latches a failure with an explicit reason code and returns false, so a
  /// caller can write c return reader.reject(ReasonCode::X);
  bool reject(ReasonCode code) noexcept {
    fail(code);
    return false;
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] ReasonCode reason() const noexcept { return reason_; }
  [[nodiscard]] std::size_t consumed() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept;
  [[nodiscard]] bool at_end() const noexcept { return position_ == data_.size(); }

 private:
  [[nodiscard]] bool take(std::size_t count, const std::byte*& out) noexcept;
  void fail(ReasonCode code) noexcept;

  std::span<const std::byte> data_;
  std::size_t position_{0};
  bool failed_{false};
  ReasonCode reason_{ReasonCode::Ok};
};

/// Deterministic ordering helper used to canonicalize collections of unsigned ids.
template <class T>
void canonical_sort(std::vector<T>& values) {
  for (std::size_t i = 1; i < values.size(); ++i) {
    T key = values[i];
    std::size_t j = i;
    while (j > 0 && values[j - 1] > key) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = key;
  }
}

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_CANONICAL_HPP
