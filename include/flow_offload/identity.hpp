// Flow Offload Scheduler - strongly typed identities and generations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_IDENTITY_HPP
#define FLOW_OFFLOAD_IDENTITY_HPP

#include <compare>
#include <cstdint>
#include <string>

namespace flow_offload {

/// A strongly typed 64-bit identifier. Distinct tag types make the identities
/// mutually incompatible at compile time, so a flow id can never be passed where
/// a target id is expected and a generation can never be compared with an
/// incarnation.
template <class Tag, class Rep = std::uint64_t>
class Id {
 public:
  using rep_type = Rep;

  constexpr Id() noexcept = default;
  constexpr explicit Id(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  /// A default-constructed identifier is invalid and never denotes a real entity.
  [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != Rep{}; }
  [[nodiscard]] static constexpr Id invalid() noexcept { return Id{}; }

  friend constexpr bool operator==(const Id& a, const Id& b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(const Id& a, const Id& b) noexcept { return !(a == b); }
  friend constexpr std::strong_ordering operator<=>(const Id& a, const Id& b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  Rep value_{};
};

// --- entity identities -------------------------------------------------------
struct FlowTag;
struct HostTag;
struct DeviceTag;
struct TargetTag;
struct ProcessingFunctionTag;
struct EvidenceSourceTag;
struct MigrationContractTag;
struct ExclusiveStateKeyTag;

using FlowId = Id<FlowTag>;
using HostId = Id<HostTag>;
using DeviceId = Id<DeviceTag>;
using TargetId = Id<TargetTag>;
using ProcessingFunctionId = Id<ProcessingFunctionTag>;
using EvidenceSourceId = Id<EvidenceSourceTag>;
using MigrationContractId = Id<MigrationContractTag>;
using ExclusiveStateKeyId = Id<ExclusiveStateKeyTag>;

// --- generations, incarnations, epochs ---------------------------------------
struct FlowGenerationTag;
struct TargetIncarnationTag;
struct CapabilityGenerationTag;
struct TopologyGenerationTag;
struct PolicyGenerationTag;
struct ScheduleGenerationTag;
struct EvidenceSequenceTag;
struct CoordinatorEpochTag;
struct MigrationAttemptIdTag;
struct FenceIdTag;
struct AssignmentIdTag;
struct RecommendationIdTag;
struct AuthorizationIdTag;
struct RequestIdTag;
struct OperationIdTag;

using FlowGeneration = Id<FlowGenerationTag>;
using TargetIncarnation = Id<TargetIncarnationTag>;
using CapabilityGeneration = Id<CapabilityGenerationTag>;
using TopologyGeneration = Id<TopologyGenerationTag>;
using PolicyGeneration = Id<PolicyGenerationTag>;
using ScheduleGeneration = Id<ScheduleGenerationTag>;
using EvidenceSequence = Id<EvidenceSequenceTag>;
using CoordinatorEpoch = Id<CoordinatorEpochTag>;
using MigrationAttemptId = Id<MigrationAttemptIdTag>;
using FenceId = Id<FenceIdTag>;
using AssignmentId = Id<AssignmentIdTag>;
using RecommendationId = Id<RecommendationIdTag>;
using AuthorizationId = Id<AuthorizationIdTag>;
using RequestId = Id<RequestIdTag>;
using OperationId = Id<OperationIdTag>;

/// Identity of a coordinator process incarnation. A boot id distinguishes two
/// runs that happen to observe the same coordinator epoch, which is what makes
/// pre-restart authority fencible.
struct BootId {
  std::uint64_t high{};
  std::uint64_t low{};

  [[nodiscard]] constexpr bool is_valid() const noexcept { return high != 0U || low != 0U; }

  friend constexpr bool operator==(const BootId& a, const BootId& b) noexcept {
    return a.high == b.high && a.low == b.low;
  }
  friend constexpr bool operator!=(const BootId& a, const BootId& b) noexcept { return !(a == b); }
  friend constexpr std::strong_ordering operator<=>(const BootId& a, const BootId& b) noexcept {
    if (a.high != b.high) {
      return a.high <=> b.high;
    }
    return a.low <=> b.low;
  }
};

/// Canonical decimal rendering, used by explanations, CLI output and exports.
[[nodiscard]] std::string to_string(const BootId& boot);

template <class Tag, class Rep>
[[nodiscard]] std::string to_string(const Id<Tag, Rep>& id) {
  return std::to_string(id.value());
}

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_IDENTITY_HPP
