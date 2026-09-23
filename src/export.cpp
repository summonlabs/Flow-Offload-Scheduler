// Flow Offload Scheduler - canonical machine-readable export.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/export.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

#include "flow_offload/canonical.hpp"
#include "flow_offload/serialize.hpp"

namespace flow_offload {
namespace {

/// Deterministic JSON writer. Keys are written by the caller in a fixed order;
/// the writer escapes exactly the characters JSON requires and nothing else, so
/// the byte sequence is stable.
class JsonWriter {
 public:
  explicit JsonWriter(std::size_t max_bytes) : limit_(max_bytes) {}

  void begin_object() { open('{'); }
  void end_object() { close('}'); }
  void begin_array() { open('['); }
  void end_array() { close(']'); }

  void key(std::string_view name) {
    separate();
    string(name);
    append(": ");
    pending_value_ = true;
  }

  void value_string(std::string_view text) {
    separate();
    string(text);
  }

  void value_uint(std::uint64_t value) {
    separate();
    append(std::to_string(value));
  }

  void value_bool(bool value) {
    separate();
    append(value ? "true" : "false");
  }

  void value_null() {
    separate();
    append("null");
  }

  void value_digest(const Sha256Digest& digest) {
    separate();
    string(digest.hex());
  }

  void value_capacity(const CapacityVector& capacity) {
    begin_object();
    key("packets_per_second");
    value_uint(capacity.packets_per_second);
    key("bytes_per_second");
    value_uint(capacity.bytes_per_second);
    key("state_bytes");
    value_uint(capacity.state_bytes);
    key("table_entries");
    value_uint(capacity.table_entries);
    end_object();
  }

  [[nodiscard]] bool ok() const noexcept { return !overflow_; }
  [[nodiscard]] const std::string& text() const noexcept { return out_; }

 private:
  void open(char bracket) {
    separate();
    out_.push_back(bracket);
    stack_.push_back(true);
  }

  void close(char bracket) {
    if (!stack_.empty()) {
      stack_.pop_back();
    }
    out_.push_back(bracket);
    pending_value_ = false;
  }

  /// Emits the separator before a value or a key. A key sets pending_value_ so
  /// that the value belonging to it is not preceded by a second separator.
  void separate() {
    if (pending_value_) {
      pending_value_ = false;
      return;
    }
    if (stack_.empty()) {
      return;
    }
    if (stack_.back()) {
      stack_.back() = false;
    } else {
      out_.push_back(',');
    }
  }

  void append(std::string_view text) {
    if (overflow_) {
      return;
    }
    if (out_.size() + text.size() > limit_) {
      overflow_ = true;
      return;
    }
    out_.append(text);
  }

  void string(std::string_view text) {
    if (overflow_) {
      return;
    }
    if (out_.size() + text.size() + 2 > limit_) {
      overflow_ = true;
      return;
    }
    out_.push_back('"');
    for (const char ch : text) {
      const unsigned char byte = static_cast<unsigned char>(ch);
      switch (ch) {
        case '"':
          out_.append("\\\"");
          break;
        case '\\':
          out_.append("\\\\");
          break;
        case '\n':
          out_.append("\\n");
          break;
        case '\r':
          out_.append("\\r");
          break;
        case '\t':
          out_.append("\\t");
          break;
        default:
          if (byte < 0x20U) {
            static constexpr char kHex[] = "0123456789abcdef";
            out_.append("\\u00");
            out_.push_back(kHex[(byte >> 4U) & 0x0FU]);
            out_.push_back(kHex[byte & 0x0FU]);
          } else {
            out_.push_back(ch);
          }
          break;
      }
    }
    out_.push_back('"');
  }

  std::string out_;
  std::vector<bool> stack_;
  std::size_t limit_;
  bool overflow_{false};
  bool pending_value_{false};
};

void write_capability_list(JsonWriter& json, CapabilityMask mask) {
  json.begin_array();
  for (std::uint8_t bit = 0; bit < kCapabilityCount; ++bit) {
    const auto capability = static_cast<Capability>(bit);
    if (has_capability(mask, capability)) {
      json.value_string(to_string(capability));
    }
  }
  json.end_array();
}

void write_reason_counts(JsonWriter& json, const std::vector<ReasonCount>& counts) {
  json.begin_array();
  for (const ReasonCount& count : counts) {
    json.begin_object();
    json.key("code");
    json.value_string(to_string(count.code));
    json.key("numeric");
    json.value_uint(reason_value(count.code));
    json.key("count");
    json.value_uint(count.count);
    json.end_object();
  }
  json.end_array();
}

void write_counters(JsonWriter& json, const Counters& counters) {
  json.begin_object();
#define FLOW_OFFLOAD_COUNTER(name)      \
  json.key(#name);                      \
  json.value_uint(counters.name);
  FLOW_OFFLOAD_COUNTER(flows_registered)
  FLOW_OFFLOAD_COUNTER(targets_registered)
  FLOW_OFFLOAD_COUNTER(targets_removed)
  FLOW_OFFLOAD_COUNTER(evidence_accepted)
  FLOW_OFFLOAD_COUNTER(evidence_rejected)
  FLOW_OFFLOAD_COUNTER(evidence_idempotent)
  FLOW_OFFLOAD_COUNTER(load_records_rejected_stale)
  FLOW_OFFLOAD_COUNTER(placements_accepted)
  FLOW_OFFLOAD_COUNTER(placements_refused)
  FLOW_OFFLOAD_COUNTER(placements_fallback)
  FLOW_OFFLOAD_COUNTER(placements_retained)
  FLOW_OFFLOAD_COUNTER(recommendations_issued)
  FLOW_OFFLOAD_COUNTER(authorizations_granted)
  FLOW_OFFLOAD_COUNTER(authorizations_refused)
  FLOW_OFFLOAD_COUNTER(migrations_issued)
  FLOW_OFFLOAD_COUNTER(migrations_completed)
  FLOW_OFFLOAD_COUNTER(migrations_aborted)
  FLOW_OFFLOAD_COUNTER(migrations_fenced)
  FLOW_OFFLOAD_COUNTER(acknowledgements_accepted)
  FLOW_OFFLOAD_COUNTER(acknowledgements_rejected)
  FLOW_OFFLOAD_COUNTER(effects_applied)
  FLOW_OFFLOAD_COUNTER(effects_verified)
  FLOW_OFFLOAD_COUNTER(effects_rejected)
  FLOW_OFFLOAD_COUNTER(rebalances_executed)
  FLOW_OFFLOAD_COUNTER(rebalances_truncated)
  FLOW_OFFLOAD_COUNTER(rebalances_selected)
  FLOW_OFFLOAD_COUNTER(requests_deduplicated)
  FLOW_OFFLOAD_COUNTER(restarts)
  FLOW_OFFLOAD_COUNTER(persist_commits)
  FLOW_OFFLOAD_COUNTER(persist_failures)
  FLOW_OFFLOAD_COUNTER(compactions)
  FLOW_OFFLOAD_COUNTER(refusals_by_reason_total)
  FLOW_OFFLOAD_COUNTER(history_evictions)
  FLOW_OFFLOAD_COUNTER(observations_rejected)
  FLOW_OFFLOAD_COUNTER(authorizations_revoked)
  FLOW_OFFLOAD_COUNTER(operations_submitted)
  FLOW_OFFLOAD_COUNTER(operations_cancelled)
  FLOW_OFFLOAD_COUNTER(operations_backpressured)
  FLOW_OFFLOAD_COUNTER(evidence_conflicts)
  FLOW_OFFLOAD_COUNTER(load_evidence_stale_at_decision)
#undef FLOW_OFFLOAD_COUNTER
  json.end_object();
}

void write_target(JsonWriter& json, const TargetExport& target) {
  const TargetDescriptor& descriptor = target.descriptor;
  json.begin_object();
  json.key("target");
  json.value_uint(descriptor.target.value());
  json.key("incarnation");
  json.value_uint(descriptor.incarnation.value());
  json.key("capability_generation");
  json.value_uint(descriptor.capability_generation.value());
  json.key("topology_generation");
  json.value_uint(descriptor.topology_generation.value());
  json.key("domain");
  json.value_string(to_string(descriptor.domain));
  json.key("kind");
  json.value_string(to_string(descriptor.kind));
  json.key("host");
  json.value_uint(descriptor.host.value());
  json.key("device");
  json.value_uint(descriptor.device.value());
  json.key("cost_class");
  json.value_string(to_string(descriptor.cost_class));
  json.key("declared_lifecycle");
  json.value_string(to_string(descriptor.lifecycle));
  json.key("live");
  json.value_bool(target.live);
  json.key("supports_stateful");
  json.value_bool(descriptor.supports_stateful);
  json.key("supports_migration");
  json.value_bool(descriptor.supports_migration);
  json.key("capabilities");
  write_capability_list(json, descriptor.capabilities);
  json.key("capacity");
  json.value_capacity(descriptor.capacity);
  json.key("reserved");
  json.value_capacity(descriptor.reserved);
  json.key("committed_demand");
  json.value_capacity(target.committed);
  json.key("load_evidence_present");
  json.value_bool(target.has_load);
  json.key("load_evidence_state");
  json.value_string(target.load_conflicting ? std::string_view{"conflicting"}
                                            : to_string(target.load.state));
  json.key("load_evidence_observed_at");
  json.value_uint(target.load.observed_at.nanos());
  json.key("load_evidence_utilized");
  json.value_capacity(target.load.utilized);
  json.end_object();
}

void write_flow(JsonWriter& json, const FlowDescriptor& flow) {
  json.begin_object();
  json.key("flow");
  json.value_uint(flow.flow.value());
  json.key("generation");
  json.value_uint(flow.generation.value());
  json.key("function");
  json.value_uint(flow.function.value());
  json.key("statefulness");
  json.value_string(to_string(flow.statefulness));
  json.key("exclusive_state_key");
  json.value_uint(flow.exclusive_state_key.value());
  json.key("max_cost_class");
  json.value_string(to_string(flow.max_cost_class));
  json.key("locality");
  json.value_string(to_string(flow.locality));
  json.key("locality_anchor");
  json.value_uint(flow.locality_anchor.value());
  json.key("allow_migration");
  json.value_bool(flow.allow_migration);
  json.key("pinned");
  json.value_bool(flow.pinned);
  json.key("required_capabilities");
  write_capability_list(json, flow.required_capabilities);
  json.key("demand");
  json.value_capacity(flow.demand);
  json.end_object();
}

void write_assignment(JsonWriter& json, const Assignment& assignment) {
  json.begin_object();
  json.key("assignment");
  json.value_uint(assignment.assignment.value());
  json.key("flow");
  json.value_uint(assignment.flow.value());
  json.key("flow_generation");
  json.value_uint(assignment.flow_generation.value());
  json.key("target");
  json.value_uint(assignment.target.value());
  json.key("incarnation");
  json.value_uint(assignment.incarnation.value());
  json.key("capability_generation");
  json.value_uint(assignment.capability_generation.value());
  json.key("policy_generation");
  json.value_uint(assignment.policy_generation.value());
  json.key("schedule_generation");
  json.value_uint(assignment.schedule_generation.value());
  json.key("effect_state");
  json.value_string(to_string(assignment.effect));
  json.key("reason");
  json.value_string(to_string(assignment.reason));
  json.key("reason_numeric");
  json.value_uint(reason_value(assignment.reason));
  json.key("committed_at");
  json.value_uint(assignment.committed_at.nanos());
  json.key("supersedes");
  json.value_uint(assignment.supersedes.value());
  json.end_object();
}

void write_migration(JsonWriter& json, const MigrationRecord& record) {
  json.begin_object();
  json.key("attempt");
  json.value_uint(record.intent.attempt.value());
  json.key("flow");
  json.value_uint(record.intent.flow.value());
  json.key("flow_generation");
  json.value_uint(record.intent.flow_generation.value());
  json.key("phase");
  json.value_string(to_string(record.intent.phase));
  json.key("epoch");
  json.value_uint(record.intent.epoch.value());
  json.key("boot");
  json.value_string(to_string(record.intent.boot));
  json.key("source_target");
  json.value_uint(record.intent.source_target.value());
  json.key("source_incarnation");
  json.value_uint(record.intent.source_incarnation.value());
  json.key("destination_target");
  json.value_uint(record.intent.destination_target.value());
  json.key("destination_incarnation");
  json.value_uint(record.intent.destination_incarnation.value());
  json.key("contract");
  json.value_uint(record.intent.contract.value());
  json.key("source_acknowledged");
  json.value_bool(record.source_acknowledged);
  json.key("destination_acknowledged");
  json.value_bool(record.destination_acknowledged);
  json.key("effect_applied");
  json.value_bool(record.effect_applied);
  json.key("effect_verified");
  json.value_bool(record.effect_verified);
  json.key("abort_reason");
  json.value_string(to_string(record.abort_reason));
  json.end_object();
}

void write_fence(JsonWriter& json, const FenceRecord& fence) {
  json.begin_object();
  json.key("fence");
  json.value_uint(fence.id.value());
  json.key("epoch");
  json.value_uint(fence.epoch.value());
  json.key("boot");
  json.value_string(to_string(fence.boot));
  json.key("flow");
  json.value_uint(fence.flow.value());
  json.key("target");
  json.value_uint(fence.target.value());
  json.key("reason");
  json.value_string(to_string(fence.reason));
  json.key("active");
  json.value_bool(fence.active);
  json.end_object();
}

}  // namespace

std::string export_json(const EngineExport& state, const ExportOptions& options) {
  JsonWriter json(options.max_bytes);
  json.begin_object();
  json.key("schema");
  json.value_string("summon.flow-offload.scheduler.export");
  json.key("schema_version");
  json.value_uint(kExportSchemaVersion);
  json.key("library_version");
  json.value_string(version_string());
  json.key("semantics_version");
  json.value_uint(state.semantics_version);
  json.key("snapshot_container_version");
  json.value_uint(state.snapshot_container_version);
  json.key("protocol_version");
  json.value_uint(state.protocol_version);
  json.key("coordinator_epoch");
  json.value_uint(state.epoch.value());
  json.key("coordinator_boot");
  json.value_string(to_string(state.boot));
  json.key("policy_generation");
  json.value_uint(state.policy_generation.value());
  json.key("schedule_generation");
  json.value_uint(state.schedule_generation.value());
  json.key("topology_generation");
  json.value_uint(state.topology_generation.value());
  json.key("policy_present");
  json.value_bool(state.policy_present);
  if (state.policy_present) {
    json.key("policy");
    json.begin_object();
    json.key("generation");
    json.value_uint(state.policy.generation.value());
    json.key("domain_preference");
    json.value_string(to_string(state.policy.domain_preference));
    json.key("fallback");
    json.value_string(to_string(state.policy.fallback));
    json.key("load_model");
    json.value_string(to_string(state.policy.load_model));
    json.key("max_cost_class");
    json.value_string(to_string(state.policy.max_cost_class));
    json.key("sticky");
    json.value_bool(state.policy.sticky);
    json.key("allow_migration");
    json.value_bool(state.policy.allow_migration);
    json.key("allow_placement_without_load_evidence");
    json.value_bool(state.policy.allow_placement_without_load_evidence);
    json.key("min_improvement_ppm");
    json.value_uint(state.policy.min_improvement_ppm);
    json.key("max_rebalance_migrations");
    json.value_uint(state.policy.max_rebalance_migrations);
    json.key("required_capabilities");
    write_capability_list(json, state.policy.required_capabilities);
    json.key("reserve");
    json.value_capacity(state.policy.reserve);
    json.key("allowed_domains");
    json.begin_array();
    for (const ExecutionDomain domain : state.policy.allowed_domains) {
      json.value_string(to_string(domain));
    }
    json.end_array();
    json.key("forbidden_hosts");
    json.begin_array();
    for (const HostId host : state.policy.forbidden_hosts) {
      json.value_uint(host.value());
    }
    json.end_array();
    json.key("forbidden_devices");
    json.begin_array();
    for (const DeviceId device : state.policy.forbidden_devices) {
      json.value_uint(device.value());
    }
    json.end_array();
    json.key("allowed_functions");
    json.begin_array();
    for (const ProcessingFunctionId function : state.policy.allowed_functions) {
      json.value_uint(function.value());
    }
    json.end_array();
    json.end_object();
  }
  if (options.include_recovery) {
    json.key("recovery");
    json.begin_object();
    json.key("kind");
    json.value_string(to_string(state.recovery.kind));
    json.key("reason");
    json.value_string(to_string(state.recovery.code));
    json.key("usable");
    json.value_bool(state.recovery.usable);
    json.key("snapshot_used");
    json.value_bool(state.recovery.snapshot_used);
    json.key("journal_used");
    json.value_bool(state.recovery.journal_used);
    json.key("records_replayed");
    json.value_uint(state.recovery.records_replayed);
    json.key("bytes_truncated");
    json.value_uint(state.recovery.bytes_truncated);
    json.key("commit_sequence");
    json.value_uint(state.recovery.commit_sequence);
    json.key("state_digest");
    json.value_digest(state.recovery.state_digest);
    json.key("detail");
    json.value_string(state.recovery.detail);
    json.end_object();
  }
  json.key("targets");
  json.begin_array();
  for (const TargetExport& target : state.targets) {
    write_target(json, target);
  }
  json.end_array();
  if (options.include_flows) {
    json.key("flows");
    json.begin_array();
    for (const FlowDescriptor& flow : state.flows) {
      write_flow(json, flow);
    }
    json.end_array();
  }
  if (options.include_assignments) {
    json.key("assignments");
    json.begin_array();
    for (const Assignment& assignment : state.assignments) {
      write_assignment(json, assignment);
    }
    json.end_array();
  }
  if (options.include_migrations) {
    json.key("migrations");
    json.begin_array();
    for (const MigrationRecord& record : state.migrations) {
      write_migration(json, record);
    }
    json.end_array();
  }
  json.key("fences");
  json.begin_array();
  for (const FenceRecord& fence : state.fences) {
    write_fence(json, fence);
  }
  json.end_array();
  json.key("contracts");
  json.begin_array();
  for (const MigrationContract& contract : state.contracts) {
    json.begin_object();
    json.key("contract");
    json.value_uint(contract.id.value());
    json.key("flow");
    json.value_uint(contract.flow.value());
    json.key("flow_generation");
    json.value_uint(contract.flow_generation.value());
    json.key("statefulness");
    json.value_string(to_string(contract.statefulness));
    json.key("state_transfer_defined");
    json.value_bool(contract.state_transfer_defined);
    json.key("ordering_preserved");
    json.value_bool(contract.ordering_preserved);
    json.key("rollback_defined");
    json.value_bool(contract.rollback_defined);
    json.key("exclusive_handoff");
    json.value_bool(contract.exclusive_handoff);
    json.key("destination_target");
    json.value_uint(contract.destination_target.value());
    json.key("destination_incarnation");
    json.value_uint(contract.destination_incarnation.value());
    json.key("destination_capability_generation");
    json.value_uint(contract.destination_capability_generation.value());
    json.key("policy_generation");
    json.value_uint(contract.policy_generation.value());
    json.key("max_state_bytes");
    json.value_uint(contract.max_state_bytes);
    json.end_object();
  }
  json.end_array();
  json.key("watermarks");
  json.begin_array();
  for (const SourceWatermark& mark : state.watermarks) {
    json.begin_object();
    json.key("source");
    json.value_uint(mark.source.value());
    json.key("sequence");
    json.value_uint(mark.sequence.value());
    json.key("target");
    json.value_uint(mark.target.value());
    json.key("incarnation");
    json.value_uint(mark.incarnation.value());
    json.key("capability_generation");
    json.value_uint(mark.capability_generation.value());
    json.end_object();
  }
  json.end_array();
  json.key("refusals_by_reason");
  write_reason_counts(json, state.refusal_counts);
  if (options.include_counters) {
    json.key("counters");
    write_counters(json, state.counters);
  }
  json.end_object();
  if (!json.ok()) {
    return std::string{};
  }
  std::string out = json.text();
  out.push_back('\n');
  return out;
}

std::string export_binary(const EngineExport& state) {
  CanonicalWriter writer(kMaxExportBytes);
  writer.u32(kExportSchemaVersion);
  writer.u32(state.semantics_version);
  writer.u32(state.snapshot_container_version);
  writer.u32(state.protocol_version);
  encode(writer, state.epoch);
  encode(writer, state.boot);
  encode(writer, state.policy_generation);
  encode(writer, state.schedule_generation);
  encode(writer, state.topology_generation);
  writer.boolean(state.policy_present);
  encode(writer, state.policy);
  encode(writer, state.counters);
  writer.u32(static_cast<std::uint32_t>(state.targets.size()));
  for (const TargetExport& target : state.targets) {
    encode(writer, target.descriptor);
    writer.boolean(target.live);
    writer.boolean(target.has_load);
    writer.boolean(target.load_conflicting);
    encode(writer, target.load);
    encode(writer, target.committed);
  }
  writer.u32(static_cast<std::uint32_t>(state.flows.size()));
  for (const FlowDescriptor& flow : state.flows) {
    encode(writer, flow);
  }
  writer.u32(static_cast<std::uint32_t>(state.assignments.size()));
  for (const Assignment& assignment : state.assignments) {
    encode(writer, assignment);
  }
  writer.u32(static_cast<std::uint32_t>(state.migrations.size()));
  for (const MigrationRecord& record : state.migrations) {
    encode(writer, record);
  }
  writer.u32(static_cast<std::uint32_t>(state.fences.size()));
  for (const FenceRecord& fence : state.fences) {
    encode(writer, fence);
  }
  writer.u32(static_cast<std::uint32_t>(state.refusal_counts.size()));
  for (const ReasonCount& count : state.refusal_counts) {
    encode(writer, count);
  }
  if (!writer.ok()) {
    return std::string{};
  }
  const std::span<const std::byte> bytes = writer.bytes();
  return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string export_text(const EngineExport& state) {
  std::string out;
  out.reserve(1024);
  out.append("flow-offload-scheduler export schema=");
  out.append(std::to_string(kExportSchemaVersion));
  out.append(" library=");
  out.append(version_string());
  out.append("\n  coordinator epoch=");
  out.append(to_string(state.epoch));
  out.append(" boot=");
  out.append(to_string(state.boot));
  out.append("\n  generations policy=");
  out.append(to_string(state.policy_generation));
  out.append(" schedule=");
  out.append(to_string(state.schedule_generation));
  out.append(" topology=");
  out.append(to_string(state.topology_generation));
  out.append("\n  recovery kind=");
  out.append(to_string(state.recovery.kind));
  out.append(" usable=");
  out.append(state.recovery.usable ? "true" : "false");
  out.append(" records_replayed=");
  out.append(std::to_string(state.recovery.records_replayed));
  out.append("\n  targets=");
  out.append(std::to_string(state.targets.size()));
  out.append(" flows=");
  out.append(std::to_string(state.flows.size()));
  out.append(" assignments=");
  out.append(std::to_string(state.assignments.size()));
  out.append(" migrations=");
  out.append(std::to_string(state.migrations.size()));
  out.append("\n");
  for (const TargetExport& target : state.targets) {
    out.append("  target ");
    out.append(to_string(target.descriptor.target));
    out.append(" domain=");
    out.append(to_string(target.descriptor.domain));
    out.append(" kind=");
    out.append(to_string(target.descriptor.kind));
    out.append(" incarnation=");
    out.append(to_string(target.descriptor.incarnation));
    out.append(" capability_generation=");
    out.append(to_string(target.descriptor.capability_generation));
    out.append(" live=");
    out.append(target.live ? "true" : "false");
    out.append(" cost=");
    out.append(to_string(target.descriptor.cost_class));
    out.append("\n");
  }
  for (const Assignment& assignment : state.assignments) {
    out.append("  placement flow ");
    out.append(to_string(assignment.flow));
    out.append(" generation=");
    out.append(to_string(assignment.flow_generation));
    out.append(" -> target ");
    out.append(to_string(assignment.target));
    out.append(" incarnation=");
    out.append(to_string(assignment.incarnation));
    out.append(" effect=");
    out.append(to_string(assignment.effect));
    out.append(" reason=");
    out.append(to_string(assignment.reason));
    out.append("\n");
  }
  for (const MigrationRecord& record : state.migrations) {
    out.append("  migration attempt ");
    out.append(to_string(record.intent.attempt));
    out.append(" flow ");
    out.append(to_string(record.intent.flow));
    out.append(" phase=");
    out.append(to_string(record.intent.phase));
    out.append(" abort_reason=");
    out.append(to_string(record.abort_reason));
    out.append("\n");
  }
  return out;
}

std::string export_digest(const EngineExport& state) {
  const std::string json = export_json(state);
  return Sha256::of(std::string_view{json}).hex();
}

}  // namespace flow_offload
