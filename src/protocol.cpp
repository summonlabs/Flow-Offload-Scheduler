// Flow Offload Scheduler - framed bounded wire protocol.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/protocol.hpp"

#include <cstring>

#include "flow_offload/canonical.hpp"
#include "flow_offload/digest.hpp"
#include "flow_offload/serialize.hpp"

namespace flow_offload {
namespace {

void put_u16(std::byte* p, std::uint16_t value) noexcept {
  p[0] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  p[1] = static_cast<std::byte>(value & 0xFFU);
}

void put_u32(std::byte* p, std::uint32_t value) noexcept {
  p[0] = static_cast<std::byte>((value >> 24U) & 0xFFU);
  p[1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  p[2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  p[3] = static_cast<std::byte>(value & 0xFFU);
}

void put_u64(std::byte* p, std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    p[i] = static_cast<std::byte>((value >> ((7U - i) * 8U)) & 0xFFU);
  }
}

std::uint16_t get_u16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) << 8U) |
      static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1])));
}

std::uint32_t get_u32(const std::byte* p) noexcept {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
}

std::uint64_t get_u64(const std::byte* p) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
  }
  return value;
}

/// Wraps a payload produced by a canonical encoder into the caller's buffer.
template <class EncodeFn>
[[nodiscard]] bool encode_payload(std::vector<std::byte>& out, EncodeFn encode_fn) noexcept {
  CanonicalWriter writer(kMaxFramePayloadBytes);
  encode_fn(writer);
  if (!writer.ok()) {
    return false;
  }
  const std::span<const std::byte> bytes = writer.bytes();
  try {
    out.assign(bytes.begin(), bytes.end());
  } catch (...) {
    return false;
  }
  return true;
}

}  // namespace

std::string_view to_string(Operation operation) noexcept {
  switch (operation) {
    case Operation::Hello:
      return std::string_view{"hello"};
    case Operation::DeclareTarget:
      return std::string_view{"declare-target"};
    case Operation::RegisterFlow:
      return std::string_view{"register-flow"};
    case Operation::IngestLoad:
      return std::string_view{"ingest-load"};
    case Operation::SetPolicy:
      return std::string_view{"set-policy"};
    case Operation::RegisterContract:
      return std::string_view{"register-contract"};
    case Operation::Place:
      return std::string_view{"place"};
    case Operation::Rebalance:
      return std::string_view{"rebalance"};
    case Operation::Explain:
      return std::string_view{"explain"};
    case Operation::Export:
      return std::string_view{"export"};
    case Operation::Shutdown:
      return std::string_view{"shutdown"};
  }
  return std::string_view{"unknown"};
}

bool operation_from_string(std::string_view text, Operation& out) noexcept {
  for (std::uint8_t value = 1; value <= 11; ++value) {
    const auto candidate = static_cast<Operation>(value);
    if (to_string(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::string_view to_string(ExportFormat format) noexcept {
  switch (format) {
    case ExportFormat::Json:
      return std::string_view{"json"};
    case ExportFormat::Binary:
      return std::string_view{"binary"};
    case ExportFormat::Text:
      return std::string_view{"text"};
  }
  return std::string_view{"unknown"};
}

bool encode_frame(const Frame& frame, std::vector<std::byte>& out, std::size_t max_frame_bytes) noexcept {
  if (max_frame_bytes < kFrameOverheadBytes) {
    return false;
  }
  const std::size_t payload_limit = max_frame_bytes - kFrameOverheadBytes;
  if (frame.payload.size() > payload_limit || frame.payload.size() > kMaxFramePayloadBytes) {
    return false;
  }
  const std::size_t total = kFrameHeaderBytes + frame.payload.size() + kFrameDigestBytes;
  try {
    out.assign(total, std::byte{0});
  } catch (...) {
    return false;
  }
  put_u32(out.data(), static_cast<std::uint32_t>(frame.payload.size()));
  put_u16(out.data() + 4, frame.version);
  out[6] = static_cast<std::byte>(static_cast<std::uint8_t>(frame.operation));
  out[7] = static_cast<std::byte>(frame.flags);
  put_u64(out.data() + 8, frame.correlation);
  put_u32(out.data() + 16, reason_value(frame.status));
  put_u32(out.data() + 20, 0U);
  if (!frame.payload.empty()) {
    std::memcpy(out.data() + kFrameHeaderBytes, frame.payload.data(), frame.payload.size());
  }
  const Sha256Digest digest =
      Sha256::of(std::span<const std::byte>(out.data(), kFrameHeaderBytes + frame.payload.size()));
  std::memcpy(out.data() + kFrameHeaderBytes + frame.payload.size(), digest.bytes.data(),
              kFrameDigestBytes);
  return true;
}

bool decode_frame(std::span<const std::byte> bytes, Frame& out, std::size_t max_frame_bytes) noexcept {
  out = Frame{};
  if (max_frame_bytes < kFrameOverheadBytes) {
    return false;
  }
  if (bytes.size() < kFrameHeaderBytes) {
    out.status = ReasonCode::ProtocolTruncatedFrame;
    return false;
  }
  if (bytes.size() > max_frame_bytes) {
    out.status = ReasonCode::ProtocolOversizedFrame;
    return false;
  }
  const std::uint32_t payload_length = get_u32(bytes.data());
  if (payload_length > kMaxFramePayloadBytes ||
      static_cast<std::size_t>(payload_length) + kFrameOverheadBytes != bytes.size()) {
    out.status = static_cast<std::size_t>(payload_length) + kFrameOverheadBytes > bytes.size()
                     ? ReasonCode::ProtocolTruncatedFrame
                     : ReasonCode::ProtocolMalformedFrame;
    return false;
  }
  const std::uint16_t version = get_u16(bytes.data() + 4);
  if (version != static_cast<std::uint16_t>(kProtocolVersion)) {
    out.status = ReasonCode::ProtocolUnsupportedVersion;
    return false;
  }
  if (get_u32(bytes.data() + 20) != 0U) {
    out.status = ReasonCode::ProtocolMalformedFrame;
    return false;
  }
  const std::uint8_t raw_operation = std::to_integer<std::uint8_t>(bytes[6]);
  if (raw_operation < 1U || raw_operation > 11U) {
    out.status = ReasonCode::ProtocolUnknownOperation;
    return false;
  }
  const std::uint32_t raw_status = get_u32(bytes.data() + 16);
  const auto status = static_cast<ReasonCode>(raw_status);
  if (raw_status > 0xFFFFU || to_string(status) == std::string_view{"UnknownReasonCode"}) {
    out.status = ReasonCode::ProtocolMalformedFrame;
    return false;
  }
  Sha256Digest stored{};
  std::memcpy(stored.bytes.data(), bytes.data() + kFrameHeaderBytes + payload_length,
              kFrameDigestBytes);
  const Sha256Digest computed =
      Sha256::of(bytes.subspan(0, kFrameHeaderBytes + payload_length));
  if (computed != stored) {
    out.status = ReasonCode::ProtocolMalformedFrame;
    return false;
  }
  out.version = version;
  out.operation = static_cast<Operation>(raw_operation);
  out.flags = std::to_integer<std::uint8_t>(bytes[7]);
  out.correlation = get_u64(bytes.data() + 8);
  out.status = status;
  try {
    out.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes),
                       bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes + payload_length));
  } catch (...) {
    out.status = ReasonCode::ProtocolBackpressure;
    return false;
  }
  return true;
}

bool encode_hello_request(std::string_view client_name, std::uint32_t client_version,
                          std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) {
    writer.text(client_name);
    writer.u32(client_version);
  });
}

bool decode_hello_request(std::span<const std::byte> payload, std::string& client_name,
                          std::uint32_t& client_version) noexcept {
  CanonicalReader reader(payload);
  if (!reader.text(client_name, kMaxNameBytes) || !reader.u32(client_version) || !reader.at_end() ||
      reader.failed()) {
    return false;
  }
  return true;
}

bool encode_hello_response(const HelloResponse& response, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) {
    writer.u32(response.protocol_version);
    writer.u32(response.semantics_version);
    writer.text(response.library_version);
    encode(writer, response.epoch);
    encode(writer, response.boot);
  });
}

bool decode_hello_response(std::span<const std::byte> payload, HelloResponse& out) noexcept {
  CanonicalReader reader(payload);
  if (!reader.u32(out.protocol_version) || !reader.u32(out.semantics_version) ||
      !reader.text(out.library_version, kMaxStringBytes) || !decode(reader, out.epoch) ||
      !decode(reader, out.boot) || !reader.at_end() || reader.failed()) {
    return false;
  }
  return true;
}

bool encode_target_declaration(const TargetDescriptor& descriptor, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, descriptor); });
}

bool decode_target_declaration(std::span<const std::byte> payload, TargetDescriptor& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_flow_registration(const FlowDescriptor& descriptor, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, descriptor); });
}

bool decode_flow_registration(std::span<const std::byte> payload, FlowDescriptor& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_load_evidence(const LoadEvidence& evidence, Timestamp instant,
                          std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) {
    encode(writer, evidence);
    encode(writer, instant);
  });
}

bool decode_load_evidence(std::span<const std::byte> payload, LoadEvidence& out,
                          Timestamp& instant) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && decode(reader, instant) && reader.at_end() && !reader.failed();
}

bool encode_policy(const Policy& policy, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, policy); });
}

bool decode_policy(std::span<const std::byte> payload, Policy& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_contract(const MigrationContract& contract, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, contract); });
}

bool decode_contract(std::span<const std::byte> payload, MigrationContract& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_placement_request(const PlacementRequest& request, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, request); });
}

bool decode_placement_request(std::span<const std::byte> payload, PlacementRequest& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_placement_result(const PlacementResult& result, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, result); });
}

bool decode_placement_result(std::span<const std::byte> payload, PlacementResult& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_rebalance_request(const RebalanceRequest& request, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, request); });
}

bool decode_rebalance_request(std::span<const std::byte> payload, RebalanceRequest& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_rebalance_report(const RebalanceReport& report, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, report); });
}

bool decode_rebalance_report(std::span<const std::byte> payload, RebalanceReport& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_explain_request(FlowId flow, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, flow); });
}

bool decode_explain_request(std::span<const std::byte> payload, FlowId& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_explanation(const Explanation& explanation, std::vector<std::byte>& out) noexcept {
  return encode_payload(out, [&](CanonicalWriter& writer) { encode(writer, explanation); });
}

bool decode_explanation(std::span<const std::byte> payload, Explanation& out) noexcept {
  CanonicalReader reader(payload);
  return decode(reader, out) && reader.at_end() && !reader.failed();
}

bool encode_export_request(ExportFormat format, std::vector<std::byte>& out) noexcept {
  return encode_payload(out,
                        [&](CanonicalWriter& writer) { writer.u8(static_cast<std::uint8_t>(format)); });
}

bool decode_export_request(std::span<const std::byte> payload, ExportFormat& out) noexcept {
  CanonicalReader reader(payload);
  std::uint8_t raw = 0;
  if (!reader.u8(raw) || raw > 2U || !reader.at_end() || reader.failed()) {
    return false;
  }
  out = static_cast<ExportFormat>(raw);
  return true;
}

}  // namespace flow_offload
