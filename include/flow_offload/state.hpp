// Flow Offload Scheduler - durable and derived state records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_STATE_HPP
#define FLOW_OFFLOAD_STATE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "flow_offload/digest.hpp"
#include "flow_offload/model.hpp"
#include "flow_offload/version.hpp"

namespace flow_offload {

/// A count of refusals attributed to one reason code. Kept as a sorted vector so
/// the export is canonical without depending on hash iteration order.
struct ReasonCount {
  ReasonCode code{ReasonCode::Ok};
  std::uint64_t count{0};

  friend bool operator==(const ReasonCount&, const ReasonCount&) noexcept = default;
};

/// Monotonic operational counters. They are part of the durable state so that
/// accounting closure survives restart and cannot be reset by a crash.
struct Counters {
  std::uint64_t flows_registered{0};
  std::uint64_t targets_registered{0};
  std::uint64_t targets_removed{0};
  std::uint64_t evidence_accepted{0};
  std::uint64_t evidence_rejected{0};
  std::uint64_t evidence_idempotent{0};
  std::uint64_t load_records_rejected_stale{0};
  std::uint64_t placements_accepted{0};
  std::uint64_t placements_refused{0};
  std::uint64_t placements_fallback{0};
  std::uint64_t placements_retained{0};
  std::uint64_t recommendations_issued{0};
  std::uint64_t authorizations_granted{0};
  std::uint64_t authorizations_refused{0};
  std::uint64_t migrations_issued{0};
  std::uint64_t migrations_completed{0};
  std::uint64_t migrations_aborted{0};
  std::uint64_t migrations_fenced{0};
  std::uint64_t acknowledgements_accepted{0};
  std::uint64_t acknowledgements_rejected{0};
  std::uint64_t effects_applied{0};
  std::uint64_t effects_verified{0};
  std::uint64_t effects_rejected{0};
  std::uint64_t rebalances_executed{0};
  std::uint64_t rebalances_truncated{0};
  std::uint64_t rebalances_selected{0};
  std::uint64_t requests_deduplicated{0};
  std::uint64_t restarts{0};
  std::uint64_t persist_commits{0};
  std::uint64_t persist_failures{0};
  std::uint64_t compactions{0};
  std::uint64_t refusals_by_reason_total{0};
  std::uint64_t history_evictions{0};
  std::uint64_t observations_rejected{0};
  std::uint64_t authorizations_revoked{0};
  std::uint64_t operations_submitted{0};
  std::uint64_t operations_cancelled{0};
  std::uint64_t operations_backpressured{0};
  std::uint64_t evidence_conflicts{0};
  std::uint64_t load_evidence_stale_at_decision{0};

  friend bool operator==(const Counters&, const Counters&) noexcept = default;
};

/// Idempotency record for a placement request. A repeated request with the same
/// digest returns the recorded result; a repeated request with a different digest
/// is a conflicting duplicate and is refused.
struct RequestRecord {
  RequestId request{};
  Sha256Digest request_digest{};
  Sha256Digest result_digest{};
  ScheduleGeneration schedule_generation{};
  ReasonCode outcome{ReasonCode::Ok};

  friend bool operator==(const RequestRecord&, const RequestRecord&) noexcept = default;
};

/// Highest accepted evidence sequence per source. Persisted so that a replayed
/// or out-of-order delivery after restart is still detected.
struct SourceWatermark {
  EvidenceSourceId source{};
  EvidenceSequence sequence{};
  CapabilityGeneration capability_generation{};
  TargetId target{};
  TargetIncarnation incarnation{};

  friend bool operator==(const SourceWatermark&, const SourceWatermark&) noexcept = default;
};

/// Full record of one migration attempt, including every acknowledgement,
/// application and verification, so the phase can be recomputed from evidence.
struct MigrationRecord {
  MigrationIntent intent{};
  MigrationAcknowledgement source_acknowledgement{};
  MigrationAcknowledgement destination_acknowledgement{};
  bool source_acknowledged{false};
  bool destination_acknowledged{false};
  AppliedEffect applied{};
  bool effect_applied{false};
  bool effect_verified{false};
  Sha256Digest verified_digest{};
  ReasonCode abort_reason{ReasonCode::Ok};

  friend bool operator==(const MigrationRecord&, const MigrationRecord&) noexcept = default;
};

/// The complete durable state of the scheduler. Everything that must survive a
/// restart is here; everything derived is recomputed. Live evidence, target
/// liveness and authorizations are deliberately absent.
struct DurableState {
  std::uint32_t semantics_version{kStateSemanticsVersion};
  CoordinatorEpoch epoch{};
  BootId boot{};
  ScheduleGeneration schedule_generation{};
  TopologyGeneration topology_generation{};
  Policy policy{};
  bool policy_present{false};

  std::vector<FlowDescriptor> flows;
  std::vector<TargetDescriptor> targets;
  std::vector<Assignment> assignments;
  std::vector<MigrationContract> contracts;
  std::vector<MigrationRecord> migrations;
  std::vector<FenceRecord> fences;
  std::vector<SourceWatermark> watermarks;
  std::vector<RequestRecord> requests;
  std::vector<ReasonCount> refusal_counts;
  Counters counters{};

  void canonicalize_order();
  friend bool operator==(const DurableState&, const DurableState&) noexcept = default;
};

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_STATE_HPP
