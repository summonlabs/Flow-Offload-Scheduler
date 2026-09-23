// Flow Offload Scheduler - canonical serialization of model and state records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/serialize.hpp"

#include <algorithm>

namespace flow_offload {
namespace {

template <class T>
void write_list(CanonicalWriter& writer, const std::vector<T>& items) noexcept {
  writer.u32(static_cast<std::uint32_t>(items.size()));
  for (const T& item : items) {
    encode(writer, item);
  }
}

template <class T>
[[nodiscard]] bool read_list(CanonicalReader& reader, std::vector<T>& items,
                             std::size_t max_items) noexcept {
  std::uint32_t count = 0;
  if (!reader.u32(count)) {
    return false;
  }
  if (count > max_items) {
    return reader.reject(ReasonCode::RejectedOversizedInput);
  }
  try {
    items.clear();
    items.resize(count);
  } catch (...) {
    return reader.reject(ReasonCode::RejectedResourceExhausted);
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!decode(reader, items[i])) {
      return false;
    }
  }
  return true;
}

void write_mask(CanonicalWriter& writer, CapabilityMask mask) noexcept { writer.u64(mask); }

[[nodiscard]] bool read_mask(CanonicalReader& reader, CapabilityMask& out) noexcept {
  std::uint64_t raw = 0;
  if (!reader.u64(raw)) {
    return false;
  }
  constexpr std::uint64_t kKnownBits =
      (static_cast<std::uint64_t>(1U) << static_cast<unsigned>(kCapabilityCount)) - 1U;
  if ((raw & ~kKnownBits) != 0U) {
    return reader.reject(ReasonCode::RejectedUnknownCapability);
  }
  out = raw;
  return true;
}

}  // namespace

void encode(CanonicalWriter& writer, ReasonCode value) noexcept {
  writer.u32(reason_value(value));
}

bool decode(CanonicalReader& reader, ReasonCode& out) noexcept {
  std::uint32_t raw = 0;
  if (!reader.u32(raw)) {
    return false;
  }
  if (raw > 0xFFFFU) {
    // No defined reason code is anywhere near this value; refusing early keeps a
    // corrupted field from being probed against the table at all.
    return reader.reject(ReasonCode::RejectedMalformedInput);
  }
  const auto code = static_cast<ReasonCode>(raw);
  if (to_string(code) == std::string_view{"UnknownReasonCode"}) {
    return reader.reject(ReasonCode::RejectedMalformedInput);
  }
  out = code;
  return true;
}

void encode(CanonicalWriter& writer, const BootId& value) noexcept {
  writer.u64(value.high);
  writer.u64(value.low);
}

bool decode(CanonicalReader& reader, BootId& out) noexcept {
  BootId value{};
  if (!reader.u64(value.high) || !reader.u64(value.low)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const Sha256Digest& value) noexcept { writer.digest(value); }

bool decode(CanonicalReader& reader, Sha256Digest& out) noexcept { return reader.digest(out); }

void encode(CanonicalWriter& writer, const Timestamp& value) noexcept { writer.u64(value.nanos()); }

bool decode(CanonicalReader& reader, Timestamp& out) noexcept {
  std::uint64_t raw = 0;
  if (!reader.u64(raw)) {
    return false;
  }
  out = Timestamp{raw};
  return true;
}

void encode(CanonicalWriter& writer, const CapacityVector& value) noexcept {
  writer.u64(value.packets_per_second);
  writer.u64(value.bytes_per_second);
  writer.u64(value.state_bytes);
  writer.u64(value.table_entries);
}

bool decode(CanonicalReader& reader, CapacityVector& out) noexcept {
  CapacityVector value{};
  if (!reader.u64(value.packets_per_second) || !reader.u64(value.bytes_per_second) ||
      !reader.u64(value.state_bytes) || !reader.u64(value.table_entries)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const FlowDescriptor& value) noexcept {
  encode(writer, value.flow);
  encode(writer, value.generation);
  encode(writer, value.function);
  encode_enum(writer, value.statefulness);
  encode(writer, value.exclusive_state_key);
  write_mask(writer, value.required_capabilities);
  encode_enum(writer, value.max_cost_class);
  encode(writer, value.demand);
  encode_enum(writer, value.locality);
  encode(writer, value.locality_anchor);
  writer.boolean(value.allow_migration);
  writer.boolean(value.pinned);
}

bool decode(CanonicalReader& reader, FlowDescriptor& out) noexcept {
  FlowDescriptor value{};
  if (!decode(reader, value.flow) || !decode(reader, value.generation) ||
      !decode(reader, value.function) || !decode_enum(reader, value.statefulness) ||
      !decode(reader, value.exclusive_state_key) ||
      !read_mask(reader, value.required_capabilities) ||
      !decode_enum(reader, value.max_cost_class) || !decode(reader, value.demand) ||
      !decode_enum(reader, value.locality) || !decode(reader, value.locality_anchor) ||
      !reader.boolean(value.allow_migration) || !reader.boolean(value.pinned)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const TargetDescriptor& value) noexcept {
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode(writer, value.topology_generation);
  encode_enum(writer, value.domain);
  encode_enum(writer, value.kind);
  encode(writer, value.host);
  encode(writer, value.device);
  write_mask(writer, value.capabilities);
  encode(writer, value.capacity);
  encode(writer, value.reserved);
  encode_enum(writer, value.cost_class);
  encode_enum(writer, value.lifecycle);
  writer.boolean(value.supports_stateful);
  writer.boolean(value.supports_migration);
  encode(writer, value.source);
  encode(writer, value.source_sequence);
  encode(writer, value.issued_at);
}

bool decode(CanonicalReader& reader, TargetDescriptor& out) noexcept {
  TargetDescriptor value{};
  if (!decode(reader, value.target) || !decode(reader, value.incarnation) ||
      !decode(reader, value.capability_generation) ||
      !decode(reader, value.topology_generation) || !decode_enum(reader, value.domain) ||
      !decode_enum(reader, value.kind) || !decode(reader, value.host) ||
      !decode(reader, value.device) || !read_mask(reader, value.capabilities) ||
      !decode(reader, value.capacity) || !decode(reader, value.reserved) ||
      !decode_enum(reader, value.cost_class) || !decode_enum(reader, value.lifecycle) ||
      !reader.boolean(value.supports_stateful) || !reader.boolean(value.supports_migration) ||
      !decode(reader, value.source) || !decode(reader, value.source_sequence) ||
      !decode(reader, value.issued_at)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const LoadEvidence& value) noexcept {
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode(writer, value.source);
  encode(writer, value.source_sequence);
  encode(writer, value.observed_at);
  encode_enum(writer, value.state);
  encode(writer, value.utilized);
}

bool decode(CanonicalReader& reader, LoadEvidence& out) noexcept {
  LoadEvidence value{};
  if (!decode(reader, value.target) || !decode(reader, value.incarnation) ||
      !decode(reader, value.capability_generation) || !decode(reader, value.source) ||
      !decode(reader, value.source_sequence) || !decode(reader, value.observed_at) ||
      !decode_enum(reader, value.state) || !decode(reader, value.utilized)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const Policy& value) noexcept {
  encode(writer, value.generation);
  encode_enum(writer, value.domain_preference);
  encode_enum(writer, value.fallback);
  write_mask(writer, value.required_capabilities);
  encode_enum(writer, value.max_cost_class);
  writer.u32(static_cast<std::uint32_t>(value.allowed_domains.size()));
  for (const ExecutionDomain domain : value.allowed_domains) {
    encode_enum(writer, domain);
  }
  write_list(writer, value.forbidden_hosts);
  write_list(writer, value.forbidden_devices);
  encode(writer, value.reserve);
  encode_enum(writer, value.load_model);
  writer.boolean(value.allow_placement_without_load_evidence);
  writer.boolean(value.sticky);
  writer.boolean(value.allow_migration);
  writer.u32(value.min_improvement_ppm);
  writer.u32(value.max_rebalance_migrations);
  write_list(writer, value.allowed_functions);
}

bool decode(CanonicalReader& reader, Policy& out) noexcept {
  Policy value{};
  std::uint32_t domain_count = 0;
  if (!decode(reader, value.generation) || !decode_enum(reader, value.domain_preference) ||
      !decode_enum(reader, value.fallback) ||
      !read_mask(reader, value.required_capabilities) ||
      !decode_enum(reader, value.max_cost_class) || !reader.u32(domain_count)) {
    return false;
  }
  if (domain_count > 8U) {
    return reader.reject(ReasonCode::RejectedOversizedInput);
  }
  try {
    value.allowed_domains.resize(domain_count);
  } catch (...) {
    return reader.reject(ReasonCode::RejectedResourceExhausted);
  }
  for (std::uint32_t i = 0; i < domain_count; ++i) {
    if (!decode_enum(reader, value.allowed_domains[i])) {
      return false;
    }
  }
  if (!read_list(reader, value.forbidden_hosts, kMaxTargets) ||
      !read_list(reader, value.forbidden_devices, kMaxTargets) ||
      !decode(reader, value.reserve) || !decode_enum(reader, value.load_model) ||
      !reader.boolean(value.allow_placement_without_load_evidence) ||
      !reader.boolean(value.sticky) || !reader.boolean(value.allow_migration) ||
      !reader.u32(value.min_improvement_ppm) || !reader.u32(value.max_rebalance_migrations) ||
      !read_list(reader, value.allowed_functions, kMaxFlows)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const MigrationContract& value) noexcept {
  encode(writer, value.id);
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
  encode_enum(writer, value.statefulness);
  writer.boolean(value.state_transfer_defined);
  writer.boolean(value.ordering_preserved);
  writer.boolean(value.rollback_defined);
  writer.boolean(value.exclusive_handoff);
  encode(writer, value.destination_target);
  encode(writer, value.destination_incarnation);
  encode(writer, value.destination_capability_generation);
  encode(writer, value.policy_generation);
  writer.u64(value.max_state_bytes);
}

bool decode(CanonicalReader& reader, MigrationContract& out) noexcept {
  MigrationContract value{};
  if (!decode(reader, value.id) || !decode(reader, value.flow) ||
      !decode(reader, value.flow_generation) || !decode_enum(reader, value.statefulness) ||
      !reader.boolean(value.state_transfer_defined) ||
      !reader.boolean(value.ordering_preserved) || !reader.boolean(value.rollback_defined) ||
      !reader.boolean(value.exclusive_handoff) || !decode(reader, value.destination_target) ||
      !decode(reader, value.destination_incarnation) ||
      !decode(reader, value.destination_capability_generation) ||
      !decode(reader, value.policy_generation) || !reader.u64(value.max_state_bytes)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const Assignment& value) noexcept {
  encode(writer, value.assignment);
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode(writer, value.policy_generation);
  encode(writer, value.schedule_generation);
  encode_enum(writer, value.effect);
  encode(writer, value.reason);
  encode(writer, value.committed_at);
  encode(writer, value.supersedes);
}

bool decode(CanonicalReader& reader, Assignment& out) noexcept {
  Assignment value{};
  if (!decode(reader, value.assignment) || !decode(reader, value.flow) ||
      !decode(reader, value.flow_generation) || !decode(reader, value.target) ||
      !decode(reader, value.incarnation) || !decode(reader, value.capability_generation) ||
      !decode(reader, value.policy_generation) || !decode(reader, value.schedule_generation) ||
      !decode_enum(reader, value.effect) || !decode(reader, value.reason) ||
      !decode(reader, value.committed_at) || !decode(reader, value.supersedes)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const Recommendation& value) noexcept {
  encode(writer, value.id);
  encode(writer, value.request);
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode(writer, value.policy_generation);
  encode(writer, value.schedule_generation);
  encode(writer, value.topology_generation);
  encode(writer, value.epoch);
  encode(writer, value.boot);
  encode_enum(writer, value.domain);
  encode(writer, value.outcome);
  writer.boolean(value.fallback);
  encode(writer, value.issued_at);
  encode(writer, value.digest);
}

bool decode(CanonicalReader& reader, Recommendation& out) noexcept {
  Recommendation value{};
  if (!decode(reader, value.id) || !decode(reader, value.request) || !decode(reader, value.flow) ||
      !decode(reader, value.flow_generation) || !decode(reader, value.target) ||
      !decode(reader, value.incarnation) || !decode(reader, value.capability_generation) ||
      !decode(reader, value.policy_generation) || !decode(reader, value.schedule_generation) ||
      !decode(reader, value.topology_generation) || !decode(reader, value.epoch) ||
      !decode(reader, value.boot) || !decode_enum(reader, value.domain) ||
      !decode(reader, value.outcome) || !reader.boolean(value.fallback) ||
      !decode(reader, value.issued_at) || !decode(reader, value.digest)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const Authorization& value) noexcept {
  encode(writer, value.id);
  encode(writer, value.recommendation);
  encode(writer, value.request);
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode(writer, value.policy_generation);
  encode(writer, value.schedule_generation);
  encode(writer, value.epoch);
  encode(writer, value.boot);
  encode(writer, value.fence);
  encode(writer, value.outcome);
  encode(writer, value.granted_at);
  encode(writer, value.digest);
}

bool decode(CanonicalReader& reader, Authorization& out) noexcept {
  Authorization value{};
  if (!decode(reader, value.id) || !decode(reader, value.recommendation) ||
      !decode(reader, value.request) || !decode(reader, value.flow) ||
      !decode(reader, value.flow_generation) || !decode(reader, value.target) ||
      !decode(reader, value.incarnation) || !decode(reader, value.capability_generation) ||
      !decode(reader, value.policy_generation) || !decode(reader, value.schedule_generation) ||
      !decode(reader, value.epoch) || !decode(reader, value.boot) ||
      !decode(reader, value.fence) || !decode(reader, value.outcome) ||
      !decode(reader, value.granted_at) || !decode(reader, value.digest)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const MigrationIntent& value) noexcept {
  encode(writer, value.attempt);
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
  encode(writer, value.source_assignment);
  encode(writer, value.source_target);
  encode(writer, value.source_incarnation);
  encode(writer, value.source_capability_generation);
  encode(writer, value.destination_target);
  encode(writer, value.destination_incarnation);
  encode(writer, value.destination_capability_generation);
  encode(writer, value.contract);
  encode(writer, value.policy_generation);
  encode(writer, value.schedule_generation);
  encode(writer, value.epoch);
  encode(writer, value.boot);
  encode(writer, value.fence);
  encode_enum(writer, value.phase);
  encode(writer, value.issued_at);
}

bool decode(CanonicalReader& reader, MigrationIntent& out) noexcept {
  MigrationIntent value{};
  if (!decode(reader, value.attempt) || !decode(reader, value.flow) ||
      !decode(reader, value.flow_generation) || !decode(reader, value.source_assignment) ||
      !decode(reader, value.source_target) || !decode(reader, value.source_incarnation) ||
      !decode(reader, value.source_capability_generation) ||
      !decode(reader, value.destination_target) ||
      !decode(reader, value.destination_incarnation) ||
      !decode(reader, value.destination_capability_generation) ||
      !decode(reader, value.contract) || !decode(reader, value.policy_generation) ||
      !decode(reader, value.schedule_generation) || !decode(reader, value.epoch) ||
      !decode(reader, value.boot) || !decode(reader, value.fence) ||
      !decode_enum(reader, value.phase) || !decode(reader, value.issued_at)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const MigrationAcknowledgement& value) noexcept {
  encode(writer, value.attempt);
  encode_enum(writer, value.actor);
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode(writer, value.epoch);
  encode(writer, value.boot);
  writer.boolean(value.accepted);
  encode(writer, value.reason);
  encode(writer, value.acknowledged_at);
}

bool decode(CanonicalReader& reader, MigrationAcknowledgement& out) noexcept {
  MigrationAcknowledgement value{};
  if (!decode(reader, value.attempt) || !decode_enum(reader, value.actor) ||
      !decode(reader, value.target) || !decode(reader, value.incarnation) ||
      !decode(reader, value.capability_generation) || !decode(reader, value.epoch) ||
      !decode(reader, value.boot) || !reader.boolean(value.accepted) ||
      !decode(reader, value.reason) || !decode(reader, value.acknowledged_at)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const AppliedEffect& value) noexcept {
  encode(writer, value.attempt);
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode(writer, value.epoch);
  encode(writer, value.boot);
  encode(writer, value.effect_digest);
  writer.u64(value.state_bytes);
  encode(writer, value.applied_at);
}

bool decode(CanonicalReader& reader, AppliedEffect& out) noexcept {
  AppliedEffect value{};
  if (!decode(reader, value.attempt) || !decode(reader, value.target) ||
      !decode(reader, value.incarnation) || !decode(reader, value.capability_generation) ||
      !decode(reader, value.epoch) || !decode(reader, value.boot) ||
      !decode(reader, value.effect_digest) || !reader.u64(value.state_bytes) ||
      !decode(reader, value.applied_at)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const EffectObservation& value) noexcept {
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode(writer, value.source);
  encode(writer, value.source_sequence);
  encode_enum(writer, value.state);
  encode_enum(writer, value.lifecycle);
  encode(writer, value.effect_digest);
  encode(writer, value.observed_at);
}

bool decode(CanonicalReader& reader, EffectObservation& out) noexcept {
  EffectObservation value{};
  if (!decode(reader, value.flow) || !decode(reader, value.flow_generation) ||
      !decode(reader, value.target) || !decode(reader, value.incarnation) ||
      !decode(reader, value.capability_generation) || !decode(reader, value.source) ||
      !decode(reader, value.source_sequence) || !decode_enum(reader, value.state) ||
      !decode_enum(reader, value.lifecycle) || !decode(reader, value.effect_digest) ||
      !decode(reader, value.observed_at)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const FenceRecord& value) noexcept {
  encode(writer, value.id);
  encode(writer, value.epoch);
  encode(writer, value.boot);
  encode(writer, value.flow);
  encode(writer, value.target);
  encode(writer, value.reason);
  writer.boolean(value.active);
  encode(writer, value.issued_at);
}

bool decode(CanonicalReader& reader, FenceRecord& out) noexcept {
  FenceRecord value{};
  if (!decode(reader, value.id) || !decode(reader, value.epoch) || !decode(reader, value.boot) ||
      !decode(reader, value.flow) || !decode(reader, value.target) ||
      !decode(reader, value.reason) || !reader.boolean(value.active) ||
      !decode(reader, value.issued_at)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const ExplanationStep& value) noexcept {
  encode(writer, value.code);
  writer.u64(value.value_a);
  writer.u64(value.value_b);
  writer.text(value.detail);
}

bool decode(CanonicalReader& reader, ExplanationStep& out) noexcept {
  ExplanationStep value{};
  if (!decode(reader, value.code) || !reader.u64(value.value_a) ||
      !reader.u64(value.value_b) || !reader.text(value.detail, kMaxStringBytes)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const Explanation& value) noexcept {
  encode(writer, value.primary);
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.policy_generation);
  encode(writer, value.schedule_generation);
  encode(writer, value.topology_generation);
  encode(writer, value.epoch);
  encode(writer, value.boot);
  encode(writer, value.evaluation_instant);
  write_list(writer, value.accepted);
  write_list(writer, value.refused);
}

bool decode(CanonicalReader& reader, Explanation& out) noexcept {
  Explanation value{};
  if (!decode(reader, value.primary) || !decode(reader, value.flow) ||
      !decode(reader, value.flow_generation) || !decode(reader, value.target) ||
      !decode(reader, value.incarnation) || !decode(reader, value.policy_generation) ||
      !decode(reader, value.schedule_generation) || !decode(reader, value.topology_generation) ||
      !decode(reader, value.epoch) || !decode(reader, value.boot) ||
      !decode(reader, value.evaluation_instant) ||
      !read_list(reader, value.accepted, kMaxExplanationSteps) ||
      !read_list(reader, value.refused, kMaxExplanationSteps)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const MigrationRecord& value) noexcept {
  encode(writer, value.intent);
  encode(writer, value.source_acknowledgement);
  encode(writer, value.destination_acknowledgement);
  writer.boolean(value.source_acknowledged);
  writer.boolean(value.destination_acknowledged);
  encode(writer, value.applied);
  writer.boolean(value.effect_applied);
  writer.boolean(value.effect_verified);
  encode(writer, value.verified_digest);
  encode(writer, value.abort_reason);
}

bool decode(CanonicalReader& reader, MigrationRecord& out) noexcept {
  MigrationRecord value{};
  if (!decode(reader, value.intent) || !decode(reader, value.source_acknowledgement) ||
      !decode(reader, value.destination_acknowledgement) ||
      !reader.boolean(value.source_acknowledged) ||
      !reader.boolean(value.destination_acknowledged) || !decode(reader, value.applied) ||
      !reader.boolean(value.effect_applied) || !reader.boolean(value.effect_verified) ||
      !decode(reader, value.verified_digest) || !decode(reader, value.abort_reason)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const SourceWatermark& value) noexcept {
  encode(writer, value.source);
  encode(writer, value.sequence);
  encode(writer, value.capability_generation);
  encode(writer, value.target);
  encode(writer, value.incarnation);
}

bool decode(CanonicalReader& reader, SourceWatermark& out) noexcept {
  SourceWatermark value{};
  if (!decode(reader, value.source) || !decode(reader, value.sequence) ||
      !decode(reader, value.capability_generation) || !decode(reader, value.target) ||
      !decode(reader, value.incarnation)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const RequestRecord& value) noexcept {
  encode(writer, value.request);
  encode(writer, value.request_digest);
  encode(writer, value.result_digest);
  encode(writer, value.schedule_generation);
  encode(writer, value.outcome);
}

bool decode(CanonicalReader& reader, RequestRecord& out) noexcept {
  RequestRecord value{};
  if (!decode(reader, value.request) || !decode(reader, value.request_digest) ||
      !decode(reader, value.result_digest) || !decode(reader, value.schedule_generation) ||
      !decode(reader, value.outcome)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const ReasonCount& value) noexcept {
  encode(writer, value.code);
  writer.u64(value.count);
}

bool decode(CanonicalReader& reader, ReasonCount& out) noexcept {
  ReasonCount value{};
  if (!decode(reader, value.code) || !reader.u64(value.count)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const PlacementItem& value) noexcept {
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
}

bool decode(CanonicalReader& reader, PlacementItem& out) noexcept {
  PlacementItem value{};
  if (!decode(reader, value.flow) || !decode(reader, value.flow_generation)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const PlacementRequest& value) noexcept {
  encode(writer, value.request);
  write_list(writer, value.items);
  encode(writer, value.evaluation_instant);
  encode(writer, value.policy_generation);
  encode(writer, value.base_schedule_generation);
  writer.u32(value.max_alternatives);
  writer.boolean(value.dry_run);
}

bool decode(CanonicalReader& reader, PlacementRequest& out) noexcept {
  PlacementRequest value{};
  if (!decode(reader, value.request) || !read_list(reader, value.items, kMaxFlows) ||
      !decode(reader, value.evaluation_instant) || !decode(reader, value.policy_generation) ||
      !decode(reader, value.base_schedule_generation) || !reader.u32(value.max_alternatives) ||
      !reader.boolean(value.dry_run)) {
    return false;
  }
  if (value.max_alternatives > kMaxAlternatives) {
    return reader.reject(ReasonCode::RejectedOversizedInput);
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const RebalanceRequest& value) noexcept {
  encode(writer, value.request);
  encode(writer, value.evaluation_instant);
  writer.u32(value.max_migrations);
  writer.boolean(value.dry_run);
}

bool decode(CanonicalReader& reader, RebalanceRequest& out) noexcept {
  RebalanceRequest value{};
  if (!decode(reader, value.request) || !decode(reader, value.evaluation_instant) ||
      !reader.u32(value.max_migrations) || !reader.boolean(value.dry_run)) {
    return false;
  }
  if (value.max_migrations > kMaxRebalanceMigrations) {
    return reader.reject(ReasonCode::RejectedOversizedInput);
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const RebalanceReport& value) noexcept {
  encode(writer, value.request);
  encode(writer, value.status);
  encode(writer, value.schedule_generation);
  writer.u32(value.considered);
  writer.u32(value.eligible);
  writer.u32(value.selected);
  writer.u32(value.truncated);
  writer.boolean(value.dry_run);
  write_list(writer, value.intents);
  write_list(writer, value.refusal_summary);
  encode(writer, value.explanation);
}

bool decode(CanonicalReader& reader, RebalanceReport& out) noexcept {
  RebalanceReport value{};
  if (!decode(reader, value.request) || !decode(reader, value.status) ||
      !decode(reader, value.schedule_generation) || !reader.u32(value.considered) ||
      !reader.u32(value.eligible) || !reader.u32(value.selected) ||
      !reader.u32(value.truncated) || !reader.boolean(value.dry_run) ||
      !read_list(reader, value.intents, kMaxRebalanceMigrations) ||
      !read_list(reader, value.refusal_summary, reason_code_count()) ||
      !decode(reader, value.explanation)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const CandidateView& value) noexcept {
  encode(writer, value.target);
  encode_enum(writer, value.domain);
  encode(writer, value.host);
  encode(writer, value.device);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode_enum(writer, value.cost_class);
  encode(writer, value.capacity);
  encode(writer, value.effective_used);
  encode(writer, value.available);
  encode(writer, value.demand);
  writer.u32(value.utilisation_ppm);
  writer.boolean(value.utilisation_saturated);
  writer.boolean(value.load_evidence_present);
  writer.boolean(value.load_evidence_fresh);
  writer.boolean(value.eligible);
  encode(writer, value.reason);
  writer.u32(value.rank);
  writer.boolean(value.fallback_domain);
}

bool decode(CanonicalReader& reader, CandidateView& out) noexcept {
  CandidateView value{};
  if (!decode(reader, value.target) || !decode_enum(reader, value.domain) ||
      !decode(reader, value.host) || !decode(reader, value.device) ||
      !decode(reader, value.incarnation) || !decode(reader, value.capability_generation) ||
      !decode_enum(reader, value.cost_class) || !decode(reader, value.capacity) ||
      !decode(reader, value.effective_used) || !decode(reader, value.available) ||
      !decode(reader, value.demand) || !reader.u32(value.utilisation_ppm) ||
      !reader.boolean(value.utilisation_saturated) ||
      !reader.boolean(value.load_evidence_present) ||
      !reader.boolean(value.load_evidence_fresh) || !reader.boolean(value.eligible) ||
      !decode(reader, value.reason) || !reader.u32(value.rank) ||
      !reader.boolean(value.fallback_domain)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const FlowPlacement& value) noexcept {
  encode(writer, value.flow);
  encode(writer, value.flow_generation);
  encode(writer, value.function);
  encode(writer, value.outcome);
  writer.boolean(value.accepted);
  writer.boolean(value.fallback);
  writer.boolean(value.retained);
  writer.boolean(value.changed);
  encode(writer, value.target);
  encode(writer, value.incarnation);
  encode(writer, value.capability_generation);
  encode_enum(writer, value.domain);
  encode(writer, value.assignment);
  encode(writer, value.recommendation);
  writer.u32(value.alternatives);
  write_list(writer, value.ranked);
  write_list(writer, value.refused);
  encode(writer, value.explanation);
}

bool decode(CanonicalReader& reader, FlowPlacement& out) noexcept {
  FlowPlacement value{};
  if (!decode(reader, value.flow) || !decode(reader, value.flow_generation) ||
      !decode(reader, value.function) || !decode(reader, value.outcome) ||
      !reader.boolean(value.accepted) || !reader.boolean(value.fallback) ||
      !reader.boolean(value.retained) || !reader.boolean(value.changed) ||
      !decode(reader, value.target) || !decode(reader, value.incarnation) ||
      !decode(reader, value.capability_generation) || !decode_enum(reader, value.domain) ||
      !decode(reader, value.assignment) || !decode(reader, value.recommendation) ||
      !reader.u32(value.alternatives) ||
      !read_list(reader, value.ranked, kMaxRankedCandidates) ||
      !read_list(reader, value.refused, kMaxExplanationSteps) ||
      !decode(reader, value.explanation)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const PlacementResult& value) noexcept {
  encode(writer, value.request);
  encode(writer, value.status);
  writer.boolean(value.accepted);
  writer.boolean(value.duplicate);
  writer.boolean(value.dry_run);
  encode(writer, value.schedule_generation);
  write_list(writer, value.placements);
  write_list(writer, value.refusal_summary);
  encode(writer, value.digest);
}

bool decode(CanonicalReader& reader, PlacementResult& out) noexcept {
  PlacementResult value{};
  if (!decode(reader, value.request) || !decode(reader, value.status) ||
      !reader.boolean(value.accepted) || !reader.boolean(value.duplicate) ||
      !reader.boolean(value.dry_run) || !decode(reader, value.schedule_generation) ||
      !read_list(reader, value.placements, kMaxFlows) ||
      !read_list(reader, value.refusal_summary, reason_code_count()) ||
      !decode(reader, value.digest)) {
    return false;
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const Counters& value) noexcept {
  const std::uint64_t fields[] = {
      value.flows_registered,
      value.targets_registered,
      value.targets_removed,
      value.evidence_accepted,
      value.evidence_rejected,
      value.evidence_idempotent,
      value.load_records_rejected_stale,
      value.placements_accepted,
      value.placements_refused,
      value.placements_fallback,
      value.placements_retained,
      value.recommendations_issued,
      value.authorizations_granted,
      value.authorizations_refused,
      value.migrations_issued,
      value.migrations_completed,
      value.migrations_aborted,
      value.migrations_fenced,
      value.acknowledgements_accepted,
      value.acknowledgements_rejected,
      value.effects_applied,
      value.effects_verified,
      value.effects_rejected,
      value.rebalances_executed,
      value.rebalances_truncated,
      value.rebalances_selected,
      value.requests_deduplicated,
      value.restarts,
      value.persist_commits,
      value.persist_failures,
      value.compactions,
      value.refusals_by_reason_total,
      value.history_evictions,
      value.observations_rejected,
      value.authorizations_revoked,
      value.operations_submitted,
      value.operations_cancelled,
      value.operations_backpressured,
      value.evidence_conflicts,
      value.load_evidence_stale_at_decision};
  for (const std::uint64_t field : fields) {
    writer.u64(field);
  }
}

bool decode(CanonicalReader& reader, Counters& out) noexcept {
  Counters value{};
  std::uint64_t* fields[] = {
      &value.flows_registered,
      &value.targets_registered,
      &value.targets_removed,
      &value.evidence_accepted,
      &value.evidence_rejected,
      &value.evidence_idempotent,
      &value.load_records_rejected_stale,
      &value.placements_accepted,
      &value.placements_refused,
      &value.placements_fallback,
      &value.placements_retained,
      &value.recommendations_issued,
      &value.authorizations_granted,
      &value.authorizations_refused,
      &value.migrations_issued,
      &value.migrations_completed,
      &value.migrations_aborted,
      &value.migrations_fenced,
      &value.acknowledgements_accepted,
      &value.acknowledgements_rejected,
      &value.effects_applied,
      &value.effects_verified,
      &value.effects_rejected,
      &value.rebalances_executed,
      &value.rebalances_truncated,
      &value.rebalances_selected,
      &value.requests_deduplicated,
      &value.restarts,
      &value.persist_commits,
      &value.persist_failures,
      &value.compactions,
      &value.refusals_by_reason_total,
      &value.history_evictions,
      &value.observations_rejected,
      &value.authorizations_revoked,
      &value.operations_submitted,
      &value.operations_cancelled,
      &value.operations_backpressured,
      &value.evidence_conflicts,
      &value.load_evidence_stale_at_decision};
  for (std::uint64_t* field : fields) {
    if (!reader.u64(*field)) {
      return false;
    }
  }
  out = value;
  return true;
}

void encode(CanonicalWriter& writer, const DurableState& value) noexcept {
  writer.u32(value.semantics_version);
  encode(writer, value.epoch);
  encode(writer, value.boot);
  encode(writer, value.schedule_generation);
  encode(writer, value.topology_generation);
  writer.boolean(value.policy_present);
  encode(writer, value.policy);
  write_list(writer, value.flows);
  write_list(writer, value.targets);
  write_list(writer, value.assignments);
  write_list(writer, value.contracts);
  write_list(writer, value.migrations);
  write_list(writer, value.fences);
  write_list(writer, value.watermarks);
  write_list(writer, value.requests);
  write_list(writer, value.refusal_counts);
  encode(writer, value.counters);
}

bool decode(CanonicalReader& reader, DurableState& out) noexcept {
  DurableState value{};
  if (!reader.u32(value.semantics_version)) {
    return false;
  }
  if (value.semantics_version != kStateSemanticsVersion) {
    return reader.reject(ReasonCode::RecoverySemanticsMismatch);
  }
  if (!decode(reader, value.epoch) || !decode(reader, value.boot) ||
      !decode(reader, value.schedule_generation) || !decode(reader, value.topology_generation) ||
      !reader.boolean(value.policy_present) || !decode(reader, value.policy) ||
      !read_list(reader, value.flows, kMaxFlows) ||
      !read_list(reader, value.targets, kMaxTargets) ||
      !read_list(reader, value.assignments, kMaxFlows) ||
      !read_list(reader, value.contracts, kMaxMigrationContracts) ||
      !read_list(reader, value.migrations, kMaxMigrationRecords) ||
      !read_list(reader, value.fences, kMaxHistoryRecords) ||
      !read_list(reader, value.watermarks, kMaxEvidenceSources) ||
      !read_list(reader, value.requests, kMaxRequestRecords) ||
      !read_list(reader, value.refusal_counts, reason_code_count()) ||
      !decode(reader, value.counters)) {
    return false;
  }
  out = value;
  return true;
}

void DurableState::canonicalize_order() {
  canonicalize(policy);
  std::sort(flows.begin(), flows.end(),
            [](const FlowDescriptor& a, const FlowDescriptor& b) { return a.flow < b.flow; });
  std::sort(targets.begin(), targets.end(), [](const TargetDescriptor& a, const TargetDescriptor& b) {
    return a.target < b.target;
  });
  std::sort(assignments.begin(), assignments.end(),
            [](const Assignment& a, const Assignment& b) { return a.flow < b.flow; });
  std::sort(contracts.begin(), contracts.end(),
            [](const MigrationContract& a, const MigrationContract& b) { return a.id < b.id; });
  std::sort(migrations.begin(), migrations.end(),
            [](const MigrationRecord& a, const MigrationRecord& b) {
              return a.intent.attempt < b.intent.attempt;
            });
  std::sort(fences.begin(), fences.end(),
            [](const FenceRecord& a, const FenceRecord& b) { return a.id < b.id; });
  std::sort(watermarks.begin(), watermarks.end(),
            [](const SourceWatermark& a, const SourceWatermark& b) { return a.source < b.source; });
  std::sort(requests.begin(), requests.end(),
            [](const RequestRecord& a, const RequestRecord& b) { return a.request < b.request; });
  std::sort(refusal_counts.begin(), refusal_counts.end(),
            [](const ReasonCount& a, const ReasonCount& b) {
              return reason_value(a.code) < reason_value(b.code);
            });
}

}  // namespace flow_offload
