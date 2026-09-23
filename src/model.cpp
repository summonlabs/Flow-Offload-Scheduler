// Flow Offload Scheduler - domain model rendering and canonicalization.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/model.hpp"

#include <algorithm>
#include <array>

#include "flow_offload/canonical.hpp"

namespace flow_offload {
namespace {

struct CapabilityEntry {
  Capability capability;
  std::string_view name;
};

// Sorted by name so parsing can binary-search deterministically.
constexpr std::array<CapabilityEntry, kCapabilityCount> kCapabilityTable = {{
    {Capability::AtomicCounterUpdate, std::string_view{"atomic-counter-update"}},
    {Capability::ConnectionTracking, std::string_view{"connection-tracking"}},
    {Capability::CryptoAesGcm, std::string_view{"crypto-aes-gcm"}},
    {Capability::IpsecEspProcessing, std::string_view{"ipsec-esp-processing"}},
    {Capability::LoadBalancingHash, std::string_view{"load-balancing-hash"}},
    {Capability::NatTranslation, std::string_view{"nat-translation"}},
    {Capability::OrderingGuarantee, std::string_view{"ordering-guarantee"}},
    {Capability::PacketClassification, std::string_view{"packet-classification"}},
    {Capability::PreciseTimestamping, std::string_view{"precise-timestamping"}},
    {Capability::ProgrammableParser, std::string_view{"programmable-parser"}},
    {Capability::RateMetering, std::string_view{"rate-metering"}},
    {Capability::SharedStateReplication, std::string_view{"shared-state-replication"}},
    {Capability::StatefulAcl, std::string_view{"stateful-acl"}},
    {Capability::TlsRecordInspection, std::string_view{"tls-record-inspection"}},
    {Capability::VxlanDecap, std::string_view{"vxlan-decap"}},
    {Capability::VxlanEncap, std::string_view{"vxlan-encap"}},
}};

constexpr bool capability_table_sorted() noexcept {
  for (std::size_t i = 1; i < kCapabilityTable.size(); ++i) {
    if (kCapabilityTable[i - 1].name >= kCapabilityTable[i].name) {
      return false;
    }
  }
  return true;
}

static_assert(capability_table_sorted(), "capability table must be sorted by name");

}  // namespace

std::string_view to_string(ExecutionDomain value) noexcept {
  switch (value) {
    case ExecutionDomain::Host:
      return std::string_view{"host"};
    case ExecutionDomain::OffloadDevice:
      return std::string_view{"offload-device"};
    case ExecutionDomain::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(TargetKind value) noexcept {
  switch (value) {
    case TargetKind::HostKernelPath:
      return std::string_view{"host-kernel-path"};
    case TargetKind::HostUserspacePath:
      return std::string_view{"host-userspace-path"};
    case TargetKind::NicHardwareOffload:
      return std::string_view{"nic-hardware-offload"};
    case TargetKind::SmartNic:
      return std::string_view{"smartnic"};
    case TargetKind::Dpu:
      return std::string_view{"dpu"};
    case TargetKind::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(TargetLifecycle value) noexcept {
  switch (value) {
    case TargetLifecycle::Discovered:
      return std::string_view{"discovered"};
    case TargetLifecycle::Ready:
      return std::string_view{"ready"};
    case TargetLifecycle::Degraded:
      return std::string_view{"degraded"};
    case TargetLifecycle::Draining:
      return std::string_view{"draining"};
    case TargetLifecycle::Quiesced:
      return std::string_view{"quiesced"};
    case TargetLifecycle::Removed:
      return std::string_view{"removed"};
    case TargetLifecycle::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(Statefulness value) noexcept {
  switch (value) {
    case Statefulness::Stateless:
      return std::string_view{"stateless"};
    case Statefulness::Stateful:
      return std::string_view{"stateful"};
    case Statefulness::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(CostClass value) noexcept {
  switch (value) {
    case CostClass::Economy:
      return std::string_view{"economy"};
    case CostClass::Standard:
      return std::string_view{"standard"};
    case CostClass::Premium:
      return std::string_view{"premium"};
    case CostClass::Scarce:
      return std::string_view{"scarce"};
    case CostClass::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(LoadModel value) noexcept {
  switch (value) {
    case LoadModel::ObservedOnly:
      return std::string_view{"observed-only"};
    case LoadModel::ScheduledOnly:
      return std::string_view{"scheduled-only"};
    case LoadModel::SumConservative:
      return std::string_view{"sum-conservative"};
    case LoadModel::MaximumOfBoth:
      return std::string_view{"maximum-of-both"};
    case LoadModel::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(EvidenceState value) noexcept {
  switch (value) {
    case EvidenceState::Known:
      return std::string_view{"known"};
    case EvidenceState::Unsupported:
      return std::string_view{"unsupported"};
    case EvidenceState::Conflicting:
      return std::string_view{"conflicting"};
    case EvidenceState::Stale:
      return std::string_view{"stale"};
    case EvidenceState::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(DomainPreference value) noexcept {
  switch (value) {
    case DomainPreference::HostFirst:
      return std::string_view{"host-first"};
    case DomainPreference::OffloadFirst:
      return std::string_view{"offload-first"};
    case DomainPreference::HostOnly:
      return std::string_view{"host-only"};
    case DomainPreference::OffloadOnly:
      return std::string_view{"offload-only"};
    case DomainPreference::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(FallbackPolicy value) noexcept {
  switch (value) {
    case FallbackPolicy::Forbid:
      return std::string_view{"forbid"};
    case FallbackPolicy::AllowHostFallback:
      return std::string_view{"allow-host-fallback"};
    case FallbackPolicy::AllowOffloadFallback:
      return std::string_view{"allow-offload-fallback"};
    case FallbackPolicy::AllowAnyFallback:
      return std::string_view{"allow-any-fallback"};
    case FallbackPolicy::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(LocalityKind value) noexcept {
  switch (value) {
    case LocalityKind::SameHostAsFlow:
      return std::string_view{"same-host-as-flow"};
    case LocalityKind::SameDeviceAsFlow:
      return std::string_view{"same-device-as-flow"};
    case LocalityKind::None:
    default:
      return std::string_view{"none"};
  }
}

std::string_view to_string(EffectState value) noexcept {
  switch (value) {
    case EffectState::IntentIssued:
      return std::string_view{"intent-issued"};
    case EffectState::Acknowledged:
      return std::string_view{"acknowledged"};
    case EffectState::Applied:
      return std::string_view{"applied"};
    case EffectState::Verified:
      return std::string_view{"verified"};
    case EffectState::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(MigrationPhase value) noexcept {
  switch (value) {
    case MigrationPhase::Planned:
      return std::string_view{"planned"};
    case MigrationPhase::IntentIssued:
      return std::string_view{"intent-issued"};
    case MigrationPhase::SourceAcknowledged:
      return std::string_view{"source-acknowledged"};
    case MigrationPhase::DestinationAcknowledged:
      return std::string_view{"destination-acknowledged"};
    case MigrationPhase::EffectApplied:
      return std::string_view{"effect-applied"};
    case MigrationPhase::EffectVerified:
      return std::string_view{"effect-verified"};
    case MigrationPhase::Completed:
      return std::string_view{"completed"};
    case MigrationPhase::Aborted:
      return std::string_view{"aborted"};
    case MigrationPhase::Fenced:
      return std::string_view{"fenced"};
    case MigrationPhase::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(MigrationActor value) noexcept {
  switch (value) {
    case MigrationActor::Source:
      return std::string_view{"source"};
    case MigrationActor::Destination:
      return std::string_view{"destination"};
    case MigrationActor::Unknown:
    default:
      return std::string_view{"unknown"};
  }
}

std::string_view to_string(Capability capability) noexcept {
  for (const CapabilityEntry& entry : kCapabilityTable) {
    if (entry.capability == capability) {
      return entry.name;
    }
  }
  return std::string_view{"unknown"};
}

bool capability_from_string(std::string_view text, Capability& out) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto it = std::lower_bound(
      kCapabilityTable.begin(), kCapabilityTable.end(), text,
      [](const CapabilityEntry& entry, std::string_view key) { return entry.name < key; });
  if (it == kCapabilityTable.end() || it->name != text) {
    return false;
  }
  out = it->capability;
  return true;
}

std::string render_capabilities(CapabilityMask mask) {
  std::string out;
  for (std::uint8_t bit = 0; bit < kCapabilityCount; ++bit) {
    const auto capability = static_cast<Capability>(bit);
    if (!has_capability(mask, capability)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(to_string(capability));
  }
  if (out.empty()) {
    out = "none";
  }
  return out;
}

bool parse_capabilities(std::string_view text, CapabilityMask& out) noexcept {
  out = 0;
  if (text.empty() || text == std::string_view{"none"}) {
    return true;
  }
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    const std::string_view token = text.substr(start, end - start);
    if (token.empty()) {
      return false;
    }
    Capability capability = Capability::ConnectionTracking;
    if (!capability_from_string(token, capability)) {
      out = 0;
      return false;
    }
    out |= capability_bit(capability);
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return true;
}

void canonicalize(Policy& policy) {
  canonical_sort(policy.allowed_domains);
  canonical_sort(policy.forbidden_hosts);
  canonical_sort(policy.forbidden_devices);
  canonical_sort(policy.allowed_functions);
}

bool contract_is_safe_for_stateful(const MigrationContract& contract) noexcept {
  if (contract.statefulness != Statefulness::Stateful) {
    return contract.state_transfer_defined && contract.ordering_preserved;
  }
  return contract.state_transfer_defined && contract.ordering_preserved &&
         contract.rollback_defined && contract.exclusive_handoff;
}

std::string render(const Explanation& explanation) {
  std::string out;
  out.reserve(512);
  out.append("explanation primary=");
  out.append(to_string(explanation.primary));
  out.append(" flow=");
  out.append(to_string(explanation.flow));
  out.append(" flow_generation=");
  out.append(to_string(explanation.flow_generation));
  out.append(" target=");
  out.append(to_string(explanation.target));
  out.append(" incarnation=");
  out.append(to_string(explanation.incarnation));
  out.append(" policy_generation=");
  out.append(to_string(explanation.policy_generation));
  out.append(" schedule_generation=");
  out.append(to_string(explanation.schedule_generation));
  out.append(" topology_generation=");
  out.append(to_string(explanation.topology_generation));
  out.append(" epoch=");
  out.append(to_string(explanation.epoch));
  out.append(" boot=");
  out.append(to_string(explanation.boot));
  out.append(" instant_ns=");
  out.append(std::to_string(explanation.evaluation_instant.nanos()));
  for (const ExplanationStep& step : explanation.accepted) {
    out.append("\n  accept ");
    out.append(to_string(step.code));
    out.append(" a=");
    out.append(std::to_string(step.value_a));
    out.append(" b=");
    out.append(std::to_string(step.value_b));
    out.push_back(' ');
    out.append(step.detail);
  }
  for (const ExplanationStep& step : explanation.refused) {
    out.append("\n  refuse ");
    out.append(to_string(step.code));
    out.append(" a=");
    out.append(std::to_string(step.value_a));
    out.append(" b=");
    out.append(std::to_string(step.value_b));
    out.push_back(' ');
    out.append(step.detail);
  }
  return out;
}

Sha256Digest digest_of(const Explanation& explanation) noexcept {
  const std::string text = render(explanation);
  return Sha256::of(std::string_view{text});
}

}  // namespace flow_offload
