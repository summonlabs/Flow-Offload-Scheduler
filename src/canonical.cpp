// Flow Offload Scheduler - canonical deterministic encoding.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/canonical.hpp"

#include <cstring>

#include "flow_offload/digest.hpp"

namespace flow_offload {
namespace {

constexpr std::size_t kInitialReserve = 4096U;

}  // namespace

CanonicalWriter::CanonicalWriter(std::size_t max_bytes) noexcept : limit_(max_bytes) {
  const std::size_t reserve = limit_ < kInitialReserve ? limit_ : kInitialReserve;
  try {
    buffer_.reserve(reserve);
  } catch (...) {
    overflow_ = true;
  }
}

void CanonicalWriter::put(const void* data, std::size_t size) noexcept {
  if (overflow_) {
    return;
  }
  if (size > limit_ - (buffer_.size() < limit_ ? buffer_.size() : limit_)) {
    overflow_ = true;
    return;
  }
  const auto* first = static_cast<const std::byte*>(data);
  try {
    buffer_.insert(buffer_.end(), first, first + size);
  } catch (...) {
    overflow_ = true;
  }
}

void CanonicalWriter::u8(std::uint8_t value) noexcept {
  const std::uint8_t bytes[1] = {value};
  put(bytes, 1);
}

void CanonicalWriter::u16(std::uint16_t value) noexcept {
  const std::uint8_t bytes[2] = {static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                                 static_cast<std::uint8_t>(value & 0xFFU)};
  put(bytes, 2);
}

void CanonicalWriter::u32(std::uint32_t value) noexcept {
  const std::uint8_t bytes[4] = {static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                                 static_cast<std::uint8_t>(value & 0xFFU)};
  put(bytes, 4);
}

void CanonicalWriter::u64(std::uint64_t value) noexcept {
  const std::uint8_t bytes[8] = {static_cast<std::uint8_t>((value >> 56U) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 48U) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 40U) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 32U) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                                 static_cast<std::uint8_t>(value & 0xFFU)};
  put(bytes, 8);
}

void CanonicalWriter::i64(std::int64_t value) noexcept {
  u64(static_cast<std::uint64_t>(value));
}

void CanonicalWriter::boolean(bool value) noexcept { u8(value ? 1U : 0U); }

void CanonicalWriter::fixed(std::span<const std::byte> data) noexcept {
  if (!data.empty()) {
    put(data.data(), data.size());
  }
}

void CanonicalWriter::digest(const Sha256Digest& value) noexcept {
  put(value.bytes.data(), value.bytes.size());
}

void CanonicalWriter::blob(std::span<const std::byte> data) noexcept {
  if (data.size() > 0xFFFFFFFFULL) {
    overflow_ = true;
    return;
  }
  u32(static_cast<std::uint32_t>(data.size()));
  fixed(data);
}

void CanonicalWriter::text(std::string_view value) noexcept {
  blob(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

CanonicalReader::CanonicalReader(std::span<const std::byte> data) noexcept : data_(data) {}

void CanonicalReader::fail(ReasonCode code) noexcept {
  if (!failed_) {
    failed_ = true;
    reason_ = code;
  }
}

bool CanonicalReader::take(std::size_t count, const std::byte*& out) noexcept {
  if (failed_) {
    return false;
  }
  if (count > data_.size() - position_) {
    fail(ReasonCode::RejectedTruncatedInput);
    return false;
  }
  out = data_.data() + position_;
  position_ += count;
  return true;
}

std::size_t CanonicalReader::remaining() const noexcept {
  if (failed_ || position_ > data_.size()) {
    return 0;
  }
  return data_.size() - position_;
}

bool CanonicalReader::u8(std::uint8_t& out) noexcept {
  const std::byte* p = nullptr;
  if (!take(1, p)) {
    return false;
  }
  out = std::to_integer<std::uint8_t>(p[0]);
  return true;
}

bool CanonicalReader::u16(std::uint16_t& out) noexcept {
  const std::byte* p = nullptr;
  if (!take(2, p)) {
    return false;
  }
  out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) << 8U) |
                                   static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1])));
  return true;
}

bool CanonicalReader::u32(std::uint32_t& out) noexcept {
  const std::byte* p = nullptr;
  if (!take(4, p)) {
    return false;
  }
  out = (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
  return true;
}

bool CanonicalReader::u64(std::uint64_t& out) noexcept {
  const std::byte* p = nullptr;
  if (!take(8, p)) {
    return false;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
  }
  out = value;
  return true;
}

bool CanonicalReader::i64(std::int64_t& out) noexcept {
  std::uint64_t raw = 0;
  if (!u64(raw)) {
    return false;
  }
  out = static_cast<std::int64_t>(raw);
  return true;
}

bool CanonicalReader::boolean(bool& out) noexcept {
  std::uint8_t raw = 0;
  if (!u8(raw)) {
    return false;
  }
  if (raw > 1U) {
    fail(ReasonCode::RejectedMalformedInput);
    return false;
  }
  out = raw == 1U;
  return true;
}

bool CanonicalReader::fixed(std::span<std::byte> out) noexcept {
  const std::byte* p = nullptr;
  if (!take(out.size(), p)) {
    return false;
  }
  if (!out.empty()) {
    std::memcpy(out.data(), p, out.size());
  }
  return true;
}

bool CanonicalReader::digest(Sha256Digest& out) noexcept {
  const std::byte* p = nullptr;
  if (!take(out.bytes.size(), p)) {
    return false;
  }
  for (std::size_t i = 0; i < out.bytes.size(); ++i) {
    out.bytes[i] = std::to_integer<std::uint8_t>(p[i]);
  }
  return true;
}

bool CanonicalReader::blob(std::vector<std::byte>& out, std::size_t max_bytes) noexcept {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (length > max_bytes) {
    fail(ReasonCode::RejectedOversizedInput);
    return false;
  }
  const std::byte* p = nullptr;
  if (!take(length, p)) {
    return false;
  }
  try {
    out.assign(p, p + length);
  } catch (...) {
    fail(ReasonCode::RejectedResourceExhausted);
    return false;
  }
  return true;
}

bool CanonicalReader::text(std::string& out, std::size_t max_bytes) noexcept {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (length > max_bytes) {
    fail(ReasonCode::RejectedOversizedInput);
    return false;
  }
  const std::byte* p = nullptr;
  if (!take(length, p)) {
    return false;
  }
  try {
    out.assign(reinterpret_cast<const char*>(p), length);
  } catch (...) {
    fail(ReasonCode::RejectedResourceExhausted);
    return false;
  }
  return true;
}

bool CanonicalReader::skip(std::size_t count) noexcept {
  const std::byte* p = nullptr;
  return take(count, p);
}

}  // namespace flow_offload
