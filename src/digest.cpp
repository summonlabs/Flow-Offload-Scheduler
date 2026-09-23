// Flow Offload Scheduler - SHA-256 implementation (FIPS 180-4).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/digest.hpp"

#include <cstring>

namespace flow_offload {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t amount) noexcept {
  return (value >> amount) | (value << (32U - amount));
}

[[nodiscard]] constexpr std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) | static_cast<std::uint32_t>(p[3]);
}

constexpr void store_be32(std::uint8_t* p, std::uint32_t value) noexcept {
  p[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
  p[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  p[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  p[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

}  // namespace

Sha256::Sha256() noexcept {
  state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  buffer_.fill(0);
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t w[64] = {};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = load_be32(block + (i * 4));
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  update(std::span<const std::byte>(static_cast<const std::byte*>(data), size));
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  if (finalized_ || data.empty()) {
    return;
  }
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t remaining = data.size();

  if (buffered_ > 0) {
    const std::size_t need = 64 - buffered_;
    const std::size_t take = remaining < need ? remaining : need;
    std::memcpy(buffer_.data() + buffered_, p, take);
    buffered_ += take;
    p += take;
    remaining -= take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  while (remaining >= 64) {
    compress(p);
    p += 64;
    remaining -= 64;
  }

  if (remaining > 0) {
    std::memcpy(buffer_.data() + buffered_, p, remaining);
    buffered_ += remaining;
  }
}

Sha256Digest Sha256::finish() noexcept {
  Sha256Digest out{};
  if (finalized_) {
    return out;
  }

  const std::uint64_t bit_length = total_bytes_ * 8U;
  std::uint8_t padding[72] = {};
  padding[0] = 0x80U;
  const std::size_t pad_len = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
  update(padding, pad_len);

  std::uint8_t length_bytes[8];
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bit_length >> ((7U - i) * 8U)) & 0xFFU);
  }
  update(length_bytes, 8);

  for (std::size_t i = 0; i < 8; ++i) {
    store_be32(out.bytes.data() + (i * 4), state_[i]);
  }
  finalized_ = true;
  return out;
}

Sha256Digest Sha256::of(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

Sha256Digest Sha256::of(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

std::string Sha256Digest::hex() const {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(64);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[i * 2] = kHexDigits[(bytes[i] >> 4U) & 0x0FU];
    out[(i * 2) + 1] = kHexDigits[bytes[i] & 0x0FU];
  }
  return out;
}

bool Sha256Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes) {
    if (byte != 0U) {
      return false;
    }
  }
  return true;
}

}  // namespace flow_offload
