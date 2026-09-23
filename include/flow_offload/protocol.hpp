// Flow Offload Scheduler - framed bounded wire protocol.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_PROTOCOL_HPP
#define FLOW_OFFLOAD_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "flow_offload/engine.hpp"

namespace flow_offload {

/// Frame layout (all integers big-endian):
///    0  4  payload length (excludes this header and the trailing digest)
///    4  2  protocol version
///    6  1  operation
///    7  1  flags
///    8  8  correlation identifier
///   16  4  status reason code
///   20  4  reserved, must be zero
///   24  N  payload
/// 24+N 32  SHA-256 over bytes [0, 24+N)
inline constexpr std::size_t kFrameHeaderBytes = 24;
inline constexpr std::size_t kFrameDigestBytes = 32;
inline constexpr std::size_t kFrameOverheadBytes = kFrameHeaderBytes + kFrameDigestBytes;

enum class Operation : std::uint8_t {
  Hello = 1,
  DeclareTarget = 2,
  RegisterFlow = 3,
  IngestLoad = 4,
  SetPolicy = 5,
  RegisterContract = 6,
  Place = 7,
  Rebalance = 8,
  Explain = 9,
  Export = 10,
  Shutdown = 11,
};

[[nodiscard]] std::string_view to_string(Operation operation) noexcept;
[[nodiscard]] bool operation_from_string(std::string_view text, Operation& out) noexcept;

enum class ExportFormat : std::uint8_t {
  Json = 0,
  Binary = 1,
  Text = 2,
};

[[nodiscard]] std::string_view to_string(ExportFormat format) noexcept;

struct Frame {
  std::uint16_t version{kProtocolVersion};
  Operation operation{Operation::Hello};
  std::uint8_t flags{0};
  std::uint64_t correlation{0};
  ReasonCode status{ReasonCode::ProtocolOk};
  std::vector<std::byte> payload;
};

/// Encodes a frame. Fails - without allocating beyond the bound - when the
/// payload or the resulting frame would exceed \p max_frame_bytes.
[[nodiscard]] bool encode_frame(const Frame& frame, std::vector<std::byte>& out,
                                std::size_t max_frame_bytes) noexcept;

/// Decodes and validates a complete frame, including its digest. Truncated,
/// oversized, mis-versioned, non-zero-reserved and digest-mismatched frames are
/// refused with distinct reason codes.
[[nodiscard]] bool decode_frame(std::span<const std::byte> bytes, Frame& out,
                                std::size_t max_frame_bytes) noexcept;

/// Payload codecs. Each returns false rather than producing a partial record.
[[nodiscard]] bool encode_hello_request(std::string_view client_name,
                                        std::uint32_t client_version,
                                        std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_hello_request(std::span<const std::byte> payload, std::string& client_name,
                                        std::uint32_t& client_version) noexcept;

struct HelloResponse {
  std::uint32_t protocol_version{kProtocolVersion};
  std::uint32_t semantics_version{kStateSemanticsVersion};
  std::string library_version;
  CoordinatorEpoch epoch{};
  BootId boot{};
};

[[nodiscard]] bool encode_hello_response(const HelloResponse& response,
                                         std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_hello_response(std::span<const std::byte> payload,
                                         HelloResponse& out) noexcept;

[[nodiscard]] bool encode_target_declaration(const TargetDescriptor& descriptor,
                                             std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_target_declaration(std::span<const std::byte> payload,
                                             TargetDescriptor& out) noexcept;

[[nodiscard]] bool encode_flow_registration(const FlowDescriptor& descriptor,
                                            std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_flow_registration(std::span<const std::byte> payload,
                                            FlowDescriptor& out) noexcept;

[[nodiscard]] bool encode_load_evidence(const LoadEvidence& evidence, Timestamp instant,
                                        std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_load_evidence(std::span<const std::byte> payload, LoadEvidence& out,
                                        Timestamp& instant) noexcept;

[[nodiscard]] bool encode_policy(const Policy& policy, std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_policy(std::span<const std::byte> payload, Policy& out) noexcept;

[[nodiscard]] bool encode_contract(const MigrationContract& contract,
                                   std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_contract(std::span<const std::byte> payload,
                                   MigrationContract& out) noexcept;

[[nodiscard]] bool encode_placement_request(const PlacementRequest& request,
                                            std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_placement_request(std::span<const std::byte> payload,
                                            PlacementRequest& out) noexcept;

[[nodiscard]] bool encode_placement_result(const PlacementResult& result,
                                           std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_placement_result(std::span<const std::byte> payload,
                                           PlacementResult& out) noexcept;

[[nodiscard]] bool encode_rebalance_request(const RebalanceRequest& request,
                                            std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_rebalance_request(std::span<const std::byte> payload,
                                            RebalanceRequest& out) noexcept;

[[nodiscard]] bool encode_rebalance_report(const RebalanceReport& report,
                                           std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_rebalance_report(std::span<const std::byte> payload,
                                           RebalanceReport& out) noexcept;

[[nodiscard]] bool encode_explain_request(FlowId flow, std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_explain_request(std::span<const std::byte> payload, FlowId& out) noexcept;

[[nodiscard]] bool encode_explanation(const Explanation& explanation,
                                      std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_explanation(std::span<const std::byte> payload,
                                      Explanation& out) noexcept;

[[nodiscard]] bool encode_export_request(ExportFormat format, std::vector<std::byte>& out) noexcept;
[[nodiscard]] bool decode_export_request(std::span<const std::byte> payload,
                                         ExportFormat& out) noexcept;

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_PROTOCOL_HPP
