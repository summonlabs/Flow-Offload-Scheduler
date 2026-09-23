// Flow Offload Scheduler - placement authority engine.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_ENGINE_HPP
#define FLOW_OFFLOAD_ENGINE_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "flow_offload/state.hpp"
#include "flow_offload/store.hpp"
#include "flow_offload/thread_pool.hpp"

namespace flow_offload {

struct EngineConfig {
  /// When false the engine runs entirely in memory and never touches the disk.
  bool persistence_enabled{false};
  StoreConfig store{};

  /// Evidence older than this, measured against the caller supplied evaluation
  /// instant, is Stale and cannot justify a decision. Zero disables the check.
  Duration evidence_freshness{1000000000ULL};

  /// Bounds. Every one of them is enforced before allocation.
  std::size_t max_flows{kMaxFlows};
  std::size_t max_targets{kMaxTargets};
  std::size_t max_contracts{kMaxMigrationContracts};
  std::size_t max_migrations{kMaxMigrationRecords};
  std::size_t max_requests{kMaxRequestRecords};
  std::size_t max_history{kMaxHistoryRecords};
  std::size_t max_evidence_sources{kMaxEvidenceSources};
  std::size_t max_alternatives{4};
  std::size_t max_rebalance_migrations{kMaxRebalanceMigrations};

  /// Concurrency. Zero workers means synchronous execution on the calling thread.
  std::uint32_t workers{0};
  std::size_t max_queue_depth{kMaxQueueDepth};

  /// Test seam: forces the coordinator boot identity. An invalid value derives
  /// the identity deterministically from the previous boot and the new epoch.
  BootId boot_id_override{};

  /// Test seam: invoked at each durable commit boundary. It exists so that crash
  /// behaviour is proved against a real process death rather than a simulated
  /// exception. A null hook disables it, which is the production setting.
  Store::CrashHook crash_hook{nullptr};
  void* crash_context{nullptr};
};

/// One flow to decide about in a placement request.
struct PlacementItem {
  FlowId flow{};
  /// Invalid means "whatever generation is currently registered".
  FlowGeneration flow_generation{};
};

struct PlacementRequest {
  RequestId request{};
  std::vector<PlacementItem> items;
  Timestamp evaluation_instant{};
  /// Invalid means "the current policy generation".
  PolicyGeneration policy_generation{};
  /// Invalid means "the current schedule generation".
  ScheduleGeneration base_schedule_generation{};
  std::uint32_t max_alternatives{0};
  /// A dry run produces the same deterministic answer without publishing a new
  /// schedule generation or changing any assignment.
  bool dry_run{false};
};

/// One evaluated target, with the exact numbers that produced its rank.
struct CandidateView {
  TargetId target{};
  ExecutionDomain domain{ExecutionDomain::Unknown};
  HostId host{};
  DeviceId device{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  CostClass cost_class{CostClass::Unknown};
  CapacityVector capacity{};
  CapacityVector effective_used{};
  CapacityVector available{};
  CapacityVector demand{};
  std::uint32_t utilisation_ppm{0};
  bool utilisation_saturated{false};
  /// True when observed load evidence was available and fresh for this target.
  bool load_evidence_present{false};
  bool load_evidence_fresh{false};
  bool eligible{false};
  ReasonCode reason{ReasonCode::Ok};
  std::uint32_t rank{0};
  bool fallback_domain{false};

  friend bool operator==(const CandidateView&, const CandidateView&) noexcept = default;
};

struct FlowPlacement {
  FlowId flow{};
  FlowGeneration flow_generation{};
  ProcessingFunctionId function{};
  ReasonCode outcome{ReasonCode::Ok};
  bool accepted{false};
  bool fallback{false};
  bool retained{false};
  bool changed{false};
  TargetId target{};
  TargetIncarnation incarnation{};
  CapabilityGeneration capability_generation{};
  ExecutionDomain domain{ExecutionDomain::Unknown};
  AssignmentId assignment{};
  RecommendationId recommendation{};
  std::uint32_t alternatives{0};
  std::vector<CandidateView> ranked;
  std::vector<CandidateView> refused;
  Explanation explanation{};

  friend bool operator==(const FlowPlacement&, const FlowPlacement&) noexcept = default;
};

struct PlacementResult {
  RequestId request{};
  ReasonCode status{ReasonCode::Ok};
  bool accepted{false};
  bool duplicate{false};
  bool dry_run{false};
  ScheduleGeneration schedule_generation{};
  std::vector<FlowPlacement> placements;
  std::vector<ReasonCount> refusal_summary;
  Sha256Digest digest{};

  friend bool operator==(const PlacementResult&, const PlacementResult&) noexcept = default;
};

struct RebalanceRequest {
  RequestId request{};
  Timestamp evaluation_instant{};
  /// Zero means "the policy bound".
  std::uint32_t max_migrations{0};
  bool dry_run{false};
};

struct RebalanceReport {
  RequestId request{};
  ReasonCode status{ReasonCode::Ok};
  ScheduleGeneration schedule_generation{};
  std::uint32_t considered{0};
  std::uint32_t eligible{0};
  std::uint32_t selected{0};
  std::uint32_t truncated{0};
  bool dry_run{false};
  std::vector<MigrationIntent> intents;
  std::vector<ReasonCount> refusal_summary;
  Explanation explanation{};

  friend bool operator==(const RebalanceReport&, const RebalanceReport&) noexcept = default;
};

/// One target as the runtime currently understands it, including the live
/// evidence and the demand this runtime has committed to it.
struct TargetExport {
  TargetDescriptor descriptor{};
  bool live{false};
  bool has_load{false};
  bool load_conflicting{false};
  LoadEvidence load{};
  CapacityVector committed{};
};

/// The complete externally visible state of the runtime. Produced under one lock
/// so an export is a consistent snapshot rather than a torn read.
struct EngineExport {
  std::uint32_t semantics_version{kStateSemanticsVersion};
  std::uint32_t snapshot_container_version{kSnapshotContainerVersion};
  std::uint32_t protocol_version{kProtocolVersion};
  CoordinatorEpoch epoch{};
  BootId boot{};
  PolicyGeneration policy_generation{};
  ScheduleGeneration schedule_generation{};
  TopologyGeneration topology_generation{};
  bool policy_present{false};
  Policy policy{};
  RecoveryReport recovery{};
  Counters counters{};
  std::vector<TargetExport> targets;
  std::vector<FlowDescriptor> flows;
  std::vector<Assignment> assignments;
  std::vector<MigrationRecord> migrations;
  std::vector<FenceRecord> fences;
  std::vector<ReasonCount> refusal_counts;
  std::vector<RequestRecord> requests;
  std::vector<SourceWatermark> watermarks;
  std::vector<MigrationContract> contracts;
};

struct EngineSnapshot {
  CoordinatorEpoch epoch{};
  BootId boot{};
  PolicyGeneration policy_generation{};
  ScheduleGeneration schedule_generation{};
  TopologyGeneration topology_generation{};
  RecoveryReport recovery{};
  Counters counters{};
  std::size_t flows{0};
  std::size_t targets{0};
  std::size_t live_targets{0};
  std::size_t assignments{0};
  std::size_t migrations_in_flight{0};
  std::size_t active_fences{0};
};

/// Asynchronous operation state. Submission is never completion.
enum class OperationState : std::uint8_t {
  Pending = 0,
  Running = 1,
  Completed = 2,
  Cancelled = 3,
  Rejected = 4,
};

[[nodiscard]] std::string_view to_string(OperationState state) noexcept;

struct OperationStatus {
  OperationId id{};
  OperationState state{OperationState::Pending};
  ReasonCode reason{ReasonCode::Ok};
  bool has_result{false};
  PlacementResult result{};
};

/// The placement authority. Owns every decision about where flow processing
/// executes, and nothing else: it never forwards a packet, programs a device,
/// discovers topology or collects telemetry.
class Engine {
 public:
  /// Opaque implementation. Declared here only so that the internal translation
  /// units can name it; every member is private and no definition is exposed.
  struct Impl;

  explicit Engine(const EngineConfig& config = EngineConfig{});
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;

  /// Recovers durable state, advances the coordinator epoch, derives a new boot
  /// identity, fences every pre-restart attempt and marks every target as not
  /// live. Refuses to open on an unusable or incompatible store.
  Status open();

  /// Stops accepting work, waits for in-flight work, compacts the store and
  /// releases every resource. Idempotent.
  void shutdown();

  [[nodiscard]] bool is_open() const;
  [[nodiscard]] EngineSnapshot snapshot() const;
  [[nodiscard]] const RecoveryReport& recovery() const;

  // ---- ingest -------------------------------------------------------------
  Status register_flow(const FlowDescriptor& flow);
  Status remove_flow(FlowId flow, FlowGeneration generation);
  Status declare_target(const TargetDescriptor& target);
  Status remove_target(TargetId target, TargetIncarnation incarnation, Timestamp instant);
  Status ingest_load(const LoadEvidence& evidence, Timestamp evaluation_instant);
  Status set_policy(const Policy& policy);
  Status set_topology_generation(TopologyGeneration generation);
  Status register_migration_contract(const MigrationContract& contract);

  // ---- synchronous decisions ---------------------------------------------
  PlacementResult place(const PlacementRequest& request);
  RebalanceReport rebalance(const RebalanceRequest& request);

  // ---- authority ----------------------------------------------------------
  /// Grants authority for exactly one recommendation. Refuses when the
  /// recommendation has been superseded by a newer schedule, policy, topology
  /// generation, incarnation or coordinator incarnation.
  Status authorize(RecommendationId recommendation, Timestamp instant, Authorization& out);

  /// Revokes authority. A revoked authorization can never be used again.
  Status revoke_authorization(AuthorizationId authorization);

  // ---- effect lifecycle ---------------------------------------------------
  /// Creates a migration attempt from an authorization. An invalid source
  /// assignment denotes an initial installation rather than a migration.
  Status issue_intent(const Authorization& authorization, MigrationIntent& inout);

  Status acknowledge(const MigrationAcknowledgement& acknowledgement);
  Status report_applied(const AppliedEffect& effect);
  Status observe(const EffectObservation& observation);
  Status abort_migration(MigrationAttemptId attempt, ReasonCode reason, Timestamp instant);

  // ---- asynchronous -------------------------------------------------------
  OperationId submit(const PlacementRequest& request);
  /// Blocks until the operation reaches a terminal state. Synchronises on the
  /// operation's own completion, never on a timeout.
  bool wait_for(OperationId id, OperationStatus& out);
  [[nodiscard]] OperationStatus poll(OperationId id) const;
  Status cancel(OperationId id);

  // ---- inspection ---------------------------------------------------------
  [[nodiscard]] bool lookup_flow(FlowId flow, FlowDescriptor& out) const;
  [[nodiscard]] bool lookup_target(TargetId target, TargetDescriptor& out) const;
  [[nodiscard]] bool lookup_assignment(FlowId flow, Assignment& out) const;
  [[nodiscard]] bool lookup_migration(MigrationAttemptId attempt, MigrationRecord& out) const;
  [[nodiscard]] bool lookup_authorization(AuthorizationId authorization, Authorization& out) const;
  [[nodiscard]] bool lookup_recommendation(RecommendationId recommendation,
                                           Recommendation& out) const;
  [[nodiscard]] DurableState durable_state() const;
  /// Consistent, canonical-order view of everything the runtime knows.
  [[nodiscard]] EngineExport export_state() const;
  [[nodiscard]] std::vector<FenceRecord> active_fences() const;
  [[nodiscard]] std::vector<ReasonCount> refusal_counts() const;
  [[nodiscard]] std::size_t target_count() const;
  [[nodiscard]] std::size_t flow_count() const;

  /// Recomputes the aggregate demand committed to each target from the durable
  /// assignments. Used by conservation checks and by the CLI.
  [[nodiscard]] std::map<TargetId, CapacityVector> committed_demand() const;

  /// Canonical explanation for the current placement of p flow. Refuses with a
  /// stable reason when the flow is unknown or unplaced.
  Status explain_flow(FlowId flow, Explanation& out) const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_ENGINE_HPP
