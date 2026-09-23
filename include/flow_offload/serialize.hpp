// Flow Offload Scheduler - canonical serialization of model and state records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_SERIALIZE_HPP
#define FLOW_OFFLOAD_SERIALIZE_HPP

#include <cstdint>
#include <type_traits>

#include "flow_offload/canonical.hpp"
#include "flow_offload/engine.hpp"
#include "flow_offload/state.hpp"

namespace flow_offload {

/// Per-enumeration set of admissible wire values. A value that is not listed can
/// never be produced by a decode, so a corrupted byte cannot smuggle in an
/// out-of-range enumerator.
template <class E>
struct EnumTraits;

#define FLOW_OFFLOAD_ENUM_TRAITS(EnumType, ...)                        \
  template <>                                                          \
  struct EnumTraits<EnumType> {                                        \
    static constexpr std::uint8_t values[] = {__VA_ARGS__};            \
    static constexpr bool valid(std::uint8_t raw) noexcept {           \
      for (const std::uint8_t candidate : values) {                    \
        if (candidate == raw) {                                        \
          return true;                                                 \
        }                                                              \
      }                                                                \
      return false;                                                    \
    }                                                                  \
  }

FLOW_OFFLOAD_ENUM_TRAITS(ExecutionDomain, 0, 1, 2);
FLOW_OFFLOAD_ENUM_TRAITS(TargetKind, 0, 1, 2, 3, 4, 5);
FLOW_OFFLOAD_ENUM_TRAITS(TargetLifecycle, 0, 1, 2, 3, 4, 5, 6);
FLOW_OFFLOAD_ENUM_TRAITS(Statefulness, 0, 1, 2);
FLOW_OFFLOAD_ENUM_TRAITS(CostClass, 0, 1, 2, 3, 4);
FLOW_OFFLOAD_ENUM_TRAITS(EvidenceState, 0, 1, 2, 3, 4);
FLOW_OFFLOAD_ENUM_TRAITS(LoadModel, 0, 1, 2, 3, 4);
FLOW_OFFLOAD_ENUM_TRAITS(DomainPreference, 0, 1, 2, 3, 4);
FLOW_OFFLOAD_ENUM_TRAITS(FallbackPolicy, 0, 1, 2, 3, 4);
FLOW_OFFLOAD_ENUM_TRAITS(LocalityKind, 0, 1, 2);
FLOW_OFFLOAD_ENUM_TRAITS(EffectState, 0, 1, 2, 3, 4);
FLOW_OFFLOAD_ENUM_TRAITS(MigrationPhase, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
FLOW_OFFLOAD_ENUM_TRAITS(MigrationActor, 0, 1, 2);
FLOW_OFFLOAD_ENUM_TRAITS(Capability, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);

#undef FLOW_OFFLOAD_ENUM_TRAITS

template <class E>
void encode_enum(CanonicalWriter& writer, E value) noexcept {
  static_assert(std::is_enum_v<E>, "encode_enum requires an enumeration");
  writer.u8(static_cast<std::uint8_t>(value));
}

template <class E>
[[nodiscard]] bool decode_enum(CanonicalReader& reader, E& out) noexcept {
  static_assert(std::is_enum_v<E>, "decode_enum requires an enumeration");
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) {
    return false;
  }
  if (!EnumTraits<E>::valid(raw)) {
    return reader.reject(ReasonCode::RejectedMalformedInput);
  }
  out = static_cast<E>(raw);
  return true;
}

template <class Tag, class Rep>
void encode(CanonicalWriter& writer, const Id<Tag, Rep>& value) noexcept {
  writer.u64(static_cast<std::uint64_t>(value.value()));
}

template <class Tag, class Rep>
[[nodiscard]] bool decode(CanonicalReader& reader, Id<Tag, Rep>& out) noexcept {
  std::uint64_t raw = 0;
  if (!reader.u64(raw)) {
    return false;
  }
  out = Id<Tag, Rep>(static_cast<Rep>(raw));
  return true;
}

/// Reason codes are four-byte values with a validated set defined by the reason
/// table, so they get their own encoding rather than the byte-sized enum traits.
void encode(CanonicalWriter& writer, ReasonCode value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, ReasonCode& out) noexcept;

void encode(CanonicalWriter& writer, const BootId& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, BootId& out) noexcept;

void encode(CanonicalWriter& writer, const Sha256Digest& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, Sha256Digest& out) noexcept;

void encode(CanonicalWriter& writer, const Timestamp& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, Timestamp& out) noexcept;

void encode(CanonicalWriter& writer, const CapacityVector& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, CapacityVector& out) noexcept;

void encode(CanonicalWriter& writer, const FlowDescriptor& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, FlowDescriptor& out) noexcept;

void encode(CanonicalWriter& writer, const TargetDescriptor& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, TargetDescriptor& out) noexcept;

void encode(CanonicalWriter& writer, const LoadEvidence& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, LoadEvidence& out) noexcept;

void encode(CanonicalWriter& writer, const Policy& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, Policy& out) noexcept;

void encode(CanonicalWriter& writer, const MigrationContract& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, MigrationContract& out) noexcept;

void encode(CanonicalWriter& writer, const Assignment& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, Assignment& out) noexcept;

void encode(CanonicalWriter& writer, const Recommendation& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, Recommendation& out) noexcept;

void encode(CanonicalWriter& writer, const Authorization& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, Authorization& out) noexcept;

void encode(CanonicalWriter& writer, const MigrationIntent& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, MigrationIntent& out) noexcept;

void encode(CanonicalWriter& writer, const MigrationAcknowledgement& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, MigrationAcknowledgement& out) noexcept;

void encode(CanonicalWriter& writer, const AppliedEffect& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, AppliedEffect& out) noexcept;

void encode(CanonicalWriter& writer, const EffectObservation& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, EffectObservation& out) noexcept;

void encode(CanonicalWriter& writer, const FenceRecord& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, FenceRecord& out) noexcept;

void encode(CanonicalWriter& writer, const PlacementItem& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, PlacementItem& out) noexcept;

void encode(CanonicalWriter& writer, const PlacementRequest& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, PlacementRequest& out) noexcept;

void encode(CanonicalWriter& writer, const RebalanceRequest& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, RebalanceRequest& out) noexcept;

void encode(CanonicalWriter& writer, const RebalanceReport& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, RebalanceReport& out) noexcept;

void encode(CanonicalWriter& writer, const CandidateView& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, CandidateView& out) noexcept;

void encode(CanonicalWriter& writer, const FlowPlacement& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, FlowPlacement& out) noexcept;

void encode(CanonicalWriter& writer, const PlacementResult& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, PlacementResult& out) noexcept;

void encode(CanonicalWriter& writer, const ReasonCount& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, ReasonCount& out) noexcept;

void encode(CanonicalWriter& writer, const ExplanationStep& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, ExplanationStep& out) noexcept;

void encode(CanonicalWriter& writer, const Explanation& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, Explanation& out) noexcept;

void encode(CanonicalWriter& writer, const MigrationRecord& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, MigrationRecord& out) noexcept;

void encode(CanonicalWriter& writer, const SourceWatermark& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, SourceWatermark& out) noexcept;

void encode(CanonicalWriter& writer, const RequestRecord& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, RequestRecord& out) noexcept;

void encode(CanonicalWriter& writer, const ReasonCount& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, ReasonCount& out) noexcept;

void encode(CanonicalWriter& writer, const Counters& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, Counters& out) noexcept;

void encode(CanonicalWriter& writer, const DurableState& value) noexcept;
[[nodiscard]] bool decode(CanonicalReader& reader, DurableState& out) noexcept;

/// Canonical digest of an arbitrary serializable record.
template <class T>
[[nodiscard]] Sha256Digest digest_of(const T& value) noexcept {
  CanonicalWriter writer;
  encode(writer, value);
  if (!writer.ok()) {
    return Sha256Digest{};
  }
  return Sha256::of(writer.bytes());
}

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_SERIALIZE_HPP
