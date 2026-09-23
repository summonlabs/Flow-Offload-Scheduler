// Flow Offload Scheduler - domain model.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_MODEL_HPP
#define FLOW_OFFLOAD_MODEL_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "flow_offload/digest.hpp"
#include "flow_offload/identity.hpp"
#include "flow_offload/limits.hpp"
#include "flow_offload/reason.hpp"
#include "flow_offload/units.hpp"

namespace flow_offload {

// ===========================================================================
// Enumerations. Every enumeration has an explicit Unknown member that never
// compares equal to a known value and is never silently coerced.
// ===========================================================================

enum class ExecutionDomain : std::uint8_t {
  Unknown = 0,
  Host = 1,
  OffloadDevice = 2,
};

enum class TargetKind : std::uint8_t {
  Unknown = 0,
  HostKernelPath = 1,
  HostUserspacePath = 2,
  NicHardwareOffload = 3,
  SmartNic = 4,
  Dpu = 5,
};

enum class TargetLifecycle : std::uint8_t {
  Unknown = 0,  ///< never observed in this coordinator incarnation
  Discovered = 1,
  Ready = 2,
  Degraded = 3,
  Draining = 4,
  Quiesced = 5,
  Removed = 6,
};

enum class Statefulness : std::uint8_t {
  Unknown = 0,
  Stateless = 1,
  Stateful = 2,
};

enum class CostClass : std::uint8_t {
  Unknown = 0,
  Economy = 1,
  Standard = 2,
  Premium = 3,
  Scarce = 4,
};

/// How observed and scheduled consumption are combined when computing the load
/// offered to a target. An unknown model is never assumed; the conservative
/// sum is the default so a target can never be over-committed by optimism.
enum class LoadModel : std::uint8_t {
  Unknown = 0,
  ObservedOnly = 1,
  ScheduledOnly = 2,
  SumConservative = 3,
  MaximumOfBoth = 4,
};

enum class EvidenceState : std::uint8_t {
  Unknown = 0,
  Known = 1,
  Unsupported = 2,
  Conflicting = 3,
  Stale = 4,
};

enum class DomainPreference : std::uint8_t {
  Unknown = 0,
  HostFirst = 1,
  OffloadFirst = 2,
  HostOnly = 3,
  OffloadOnly = 4,
};

enum class FallbackPolicy : std::uint8_t {
  Unknown = 0,
  Forbid = 1,
  AllowHostFallback = 2,
  AllowOffloadFallback = 3,
  AllowAnyFallback = 4,
};

enum class LocalityKind : std::uint8_t {
  None = 0,
  SameHostAsFlow = 1,
  SameDeviceAsFlow = 2,
};

/// How far an accepted decision has actually progressed. Recommendation,
/// authorization, application and verification are distinct states and are never
/// conflated.
enum class EffectState : std::uint8_t {
  Unknown = 0,
  IntentIssued = 1,
  Acknowledged = 2,
  Applied = 3,
  Verified = 4,
};

enum class MigrationPhase : std::uint8_t {
  Unknown = 0,
  Planned = 1,
  IntentIssued = 2,
  SourceAcknowledged = 3,
  DestinationAcknowledged = 4,
  EffectApplied = 5,
  EffectVerified = 6,
  Completed = 7,
  Aborted = 8,
  Fenced = 9,
};

enum class MigrationActor : std::uint8_t {
  Unknown = 0,
  Source = 1,
  Destination = 2,
};

[[nodiscard]] std::string_view to_string(LoadModel value) noexcept;
[[nodiscard]] std::string_view to_string(ExecutionDomain value) noexcept;
[[nodiscard]] std::string_view to_string(TargetKind value) noexcept;
[[nodiscard]] std::string_view to_string(TargetLifecycle value) noexcept;
[[nodiscard]] std::string_view to_string(Statefulness value) noexcept;
[[nodiscard]] std::string_view to_string(CostClass value) noexcept;
[[nodiscard]] std::string_view to_string(EvidenceState value) noexcept;
[[nodiscard]] std::string_view to_string(DomainPreference value) noexcept;
[[nodiscard]] std::string_view to_string(FallbackPolicy value) noexcept;
[[nodiscard]] std::string_view to_string(LocalityKind value) noexcept;
[[nodiscard]] std::string_view to_string(EffectState value) noexcept;
[[nodiscard]] std::string_view to_string(MigrationPhase value) noexcept;
[[nodiscard]] std::string_view to_string(MigrationActor value) noexcept;

/// True when the lifecycle value denotes a target that may accept new flows.
[[nodiscard]] constexpr bool lifecycle_accepts_new_flows(TargetLifecycle value) noexcept {
  return value == TargetLifecycle::Ready;
}

/// True for terminal lifecycles that can never be reactivated in place.
[[nodiscard]] constexpr bool lifecycle_is_terminal(TargetLifecycle value) noexcept {
  return value == TargetLifecycle::Removed;
}

// ===========================================================================
// Capabilities. A capability mask is a set of independently meaningful bits;
// it is never interpreted as an integer quantity.
// ===========================================================================

enum class Capability : std::uint8_t {
  ConnectionTracking = 0,
  NatTranslation = 1,
  TlsRecordInspection = 2,
  IpsecEspProcessing = 3,
  VxlanEncap = 4,
  VxlanDecap = 5,
  PacketClassification = 6,
  StatefulAcl = 7,
  LoadBalancingHash = 8,
  RateMetering = 9,
  PreciseTimestamping = 10,
  ProgrammableParser = 11,
  AtomicCounterUpdate = 12,
  CryptoAesGcm = 13,
  SharedStateReplication = 14,
  OrderingGuarantee = 15,
};

inline constexpr std::uint8_t kCapabilityCount = 16;

using CapabilityMask = std::uint64_t;

[[nodiscard]] constexpr CapabilityMask capability_bit(Capability capability) noexcept {
  return static_cast<CapabilityMask>(1U) << static_cast<std::uint8_t>(capability);
}

[[nodiscard]] constexpr bool has_capability(CapabilityMask mask, Capability capability) noexcept {
  return (mask & capability_bit(capability)) != 0U;
}

/// True when every bit of p required is present in p available.
[[nodiscard]] constexpr bool satisfies(CapabilityMask available, CapabilityMask required) noexcept {
  return (available & required) == required;
}

[[nodiscard]] std::string_view to_string(Capability capability) noexcept;
[[nodiscard]] bool capability_from_string(std::string_view text, Capability& out) noexcept;

/// Canonical capability rendering: names in ascending bit order, comma separated.
[[nodiscard]] std::string render_capabilities(CapabilityMask mask);

/// Parses a canonical capability list. Any unknown name fails the whole parse so
/// an unknown capability can never be silently dropped.
[[nodiscard]] bool parse_capabilities(std::string_view text, CapabilityMask& out) noexcept;

// ===========================================================================
// Flow descriptors
// ===========================================================================

/// A flow processing request: which processing function must run, how much
/// capacity it needs, and what constrains where it may run.
struct FlowDescriptor {
  FlowId flow{};
  FlowGeneration generation{};
  ProcessingFunctionId function{};
  Statefulness statefulness{Statefulness::Unknown};
  /// Key of the exclusive flow state. Two flows sharing a key share exclusive
  /// state and may never hold live state on two different targets at once.
  /// An invalid key means the flow has no exclusive state of its own.
  ExclusiveStateKeyId exclusive_state_key{};
  CapabilityMask required_capabilities{0};
  CostClass max_cost_class{CostClass::Scarce};
  CapacityVector demand{};
  LocalityKind locality{LocalityKind::None};
  /// Anchor flow for locality. Invalid when locality is None.
  FlowId locality_anchor{};
  bool allow_migration{false};
  /// A pinned flow is never moved by rebalance; it may still be placed for the
  /// first time.
  bool pinned{false};

  friend bool operator==(const FlowDescriptor&, const FlowDescriptor&) noexcept = default;
};

// ===========================================================================
// Targets
// ===========================================================================

/// A place flow processing can execute. Declared by the target's owner and
/// supplied to this runtime as evidence; this runtime never discovers targets.
struct TargetDescriptor {
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  TopologyGeneration topology_generation{};
  ExecutionDomain domain{ExecutionDomain::Unknown};
  TargetKind kind{TargetKind::Unknown};
  HostId host{};
  DeviceId device{};
  CapabilityMask capabilities{0};
  CapacityVector capacity{};
  /// Capacity withheld from new placements (control plane, other tenants).
  CapacityVector reserved{};
  CostClass cost_class{CostClass::Unknown};
  TargetLifecycle lifecycle{TargetLifecycle::Unknown};
  bool supports_stateful{false};
  bool supports_migration{false};

  // Provenance of this declaration.
  EvidenceSourceId source{};
  EvidenceSequence source_sequence{};
  Timestamp issued_at{};

  friend bool operator==(const TargetDescriptor&, const TargetDescriptor&) noexcept = default;
};

/// Observed consumption of a target. Absent evidence is Unknown, never zero.
struct LoadEvidence {
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  EvidenceSourceId source{};
  EvidenceSequence source_sequence{};
  Timestamp observed_at{};
  EvidenceState state{EvidenceState::Unknown};
  CapacityVector utilized{};

  friend bool operator==(const LoadEvidence&, const LoadEvidence&) noexcept = default;
};

// ===========================================================================
// Policy
// ===========================================================================

struct Policy {
  PolicyGeneration generation{};
  DomainPreference domain_preference{DomainPreference::Unknown};
  FallbackPolicy fallback{FallbackPolicy::Unknown};
  CapabilityMask required_capabilities{0};
  CostClass max_cost_class{CostClass::Scarce};
  /// Allowed execution domains in canonical (ascending) order. Empty means the
  /// domain preference alone decides.
  std::vector<ExecutionDomain> allowed_domains;
  std::vector<HostId> forbidden_hosts;
  std::vector<DeviceId> forbidden_devices;
  /// Headroom that must remain free on a target after a placement.
  CapacityVector reserve{};
  /// Explicit, auditable opt-in: place without load evidence. The resulting
  /// decision is recorded with AcceptedWithoutLoadEvidence so the missing
  /// evidence is never invisible.
  bool allow_placement_without_load_evidence{false};
  /// How observed and scheduled consumption are combined.
  LoadModel load_model{LoadModel::SumConservative};
  /// Prefer the current target when it is still eligible.
  bool sticky{true};
  bool allow_migration{false};
  /// Minimum improvement in aggregate utilisation (ppm) required to justify a
  /// rebalance migration.
  std::uint32_t min_improvement_ppm{0};
  std::uint32_t max_rebalance_migrations{64};
  /// Empty means every function is allowed.
  std::vector<ProcessingFunctionId> allowed_functions;

  friend bool operator==(const Policy&, const Policy&) noexcept = default;
};

/// Canonicalizes the order of every collection inside a policy so that two
/// policies that differ only in input order compare and hash identically.
void canonicalize(Policy& policy);

// ===========================================================================
// Migration contract - the only way stateful processing may move
// ===========================================================================

struct MigrationContract {
  MigrationContractId id{};
  FlowId flow{};
  FlowGeneration flow_generation{};
  Statefulness statefulness{Statefulness::Unknown};
  bool state_transfer_defined{false};
  bool ordering_preserved{false};
  bool rollback_defined{false};
  bool exclusive_handoff{false};
  TargetId destination_target{};
  TargetIncarnation destination_incarnation{};
  CapabilityGeneration destination_capability_generation{};
  PolicyGeneration policy_generation{};
  std::uint64_t max_state_bytes{0};

  friend bool operator==(const MigrationContract&, const MigrationContract&) noexcept = default;
};

/// True when the contract is complete enough to move exclusive flow state.
[[nodiscard]] bool contract_is_safe_for_stateful(const MigrationContract& contract) noexcept;

// ===========================================================================
// Committed placement and decision surfaces
// ===========================================================================

struct Assignment {
  AssignmentId assignment{};
  FlowId flow{};
  FlowGeneration flow_generation{};
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  PolicyGeneration policy_generation{};
  ScheduleGeneration schedule_generation{};
  EffectState effect{EffectState::Unknown};
  ReasonCode reason{ReasonCode::Ok};
  Timestamp committed_at{};
  AssignmentId supersedes{};

  friend bool operator==(const Assignment&, const Assignment&) noexcept = default;
};

/// A recommendation is advice. It carries no authority and cannot be applied.
struct Recommendation {
  RecommendationId id{};
  RequestId request{};
  FlowId flow{};
  FlowGeneration flow_generation{};
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  PolicyGeneration policy_generation{};
  ScheduleGeneration schedule_generation{};
  TopologyGeneration topology_generation{};
  CoordinatorEpoch epoch{};
  BootId boot{};
  ExecutionDomain domain{ExecutionDomain::Unknown};
  ReasonCode outcome{ReasonCode::Ok};
  bool fallback{false};
  Timestamp issued_at{};
  Sha256Digest digest{};
};

/// An authorization permits application of exactly one recommendation, for one
/// coordinator epoch and boot. It is never durable and never transferable.
struct Authorization {
  AuthorizationId id{};
  RecommendationId recommendation{};
  RequestId request{};
  FlowId flow{};
  FlowGeneration flow_generation{};
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  PolicyGeneration policy_generation{};
  ScheduleGeneration schedule_generation{};
  CoordinatorEpoch epoch{};
  BootId boot{};
  FenceId fence{};
  ReasonCode outcome{ReasonCode::Ok};
  Timestamp granted_at{};
  Sha256Digest digest{};
};

struct MigrationIntent {
  MigrationAttemptId attempt{};
  FlowId flow{};
  FlowGeneration flow_generation{};
  AssignmentId source_assignment{};
  TargetId source_target{};
  TargetIncarnation source_incarnation{};
  CapabilityGeneration source_capability_generation{};
  TargetId destination_target{};
  TargetIncarnation destination_incarnation{};
  CapabilityGeneration destination_capability_generation{};
  MigrationContractId contract{};
  PolicyGeneration policy_generation{};
  ScheduleGeneration schedule_generation{};
  CoordinatorEpoch epoch{};
  BootId boot{};
  FenceId fence{};
  MigrationPhase phase{MigrationPhase::Unknown};
  Timestamp issued_at{};

  friend bool operator==(const MigrationIntent&, const MigrationIntent&) noexcept = default;
};

struct MigrationAcknowledgement {
  MigrationAttemptId attempt{};
  MigrationActor actor{MigrationActor::Unknown};
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  CoordinatorEpoch epoch{};
  BootId boot{};
  bool accepted{false};
  ReasonCode reason{ReasonCode::Ok};
  Timestamp acknowledged_at{};

  friend bool operator==(const MigrationAcknowledgement&, const MigrationAcknowledgement&) noexcept =
      default;
};

struct AppliedEffect {
  MigrationAttemptId attempt{};
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  CoordinatorEpoch epoch{};
  BootId boot{};
  Sha256Digest effect_digest{};
  std::uint64_t state_bytes{0};
  Timestamp applied_at{};

  friend bool operator==(const AppliedEffect&, const AppliedEffect&) noexcept = default;
};

/// Independently observed execution location. Only an observation that matches
/// the applied effect converts an application into a verified effect.
struct EffectObservation {
  FlowId flow{};
  FlowGeneration flow_generation{};
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  EvidenceSourceId source{};
  EvidenceSequence source_sequence{};
  EvidenceState state{EvidenceState::Unknown};
  TargetLifecycle lifecycle{TargetLifecycle::Unknown};
  Sha256Digest effect_digest{};
  Timestamp observed_at{};

  friend bool operator==(const EffectObservation&, const EffectObservation&) noexcept = default;
};

struct FenceRecord {
  FenceId id{};
  CoordinatorEpoch epoch{};
  BootId boot{};
  /// Invalid flow means the fence covers every flow on the target.
  FlowId flow{};
  TargetId target{};
  ReasonCode reason{ReasonCode::Ok};
  bool active{false};
  Timestamp issued_at{};

  friend bool operator==(const FenceRecord&, const FenceRecord&) noexcept = default;
};

// ===========================================================================
// Explanations
// ===========================================================================

struct ExplanationStep {
  ReasonCode code{ReasonCode::Ok};
  std::uint64_t value_a{0};
  std::uint64_t value_b{0};
  std::string detail;

  friend bool operator==(const ExplanationStep&, const ExplanationStep&) noexcept = default;
};

/// Why a decision was accepted or refused, and which evidence, generation and
/// policy made it legal. Steps are recorded in evaluation order and both lists
/// are bounded.
struct Explanation {
  ReasonCode primary{ReasonCode::Ok};
  FlowId flow{};
  FlowGeneration flow_generation{};
  TargetId target{};
  TargetIncarnation incarnation{};
  PolicyGeneration policy_generation{};
  ScheduleGeneration schedule_generation{};
  TopologyGeneration topology_generation{};
  CoordinatorEpoch epoch{};
  BootId boot{};
  Timestamp evaluation_instant{};
  std::vector<ExplanationStep> accepted;
  std::vector<ExplanationStep> refused;

  friend bool operator==(const Explanation&, const Explanation&) noexcept = default;
};

/// Canonical multi-line rendering. Stable across runs and platforms.
[[nodiscard]] std::string render(const Explanation& explanation);

/// Canonical digest over the evaluation-relevant content of an explanation.
[[nodiscard]] Sha256Digest digest_of(const Explanation& explanation) noexcept;

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_MODEL_HPP
