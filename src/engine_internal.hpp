// Flow Offload Scheduler - internal engine representation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Locking contract
// ----------------
//  * Impl::state_mutex guards every field of Impl except the async operation
//    table, which has its own Impl::operation_mutex.
//  * The two locks are never held at the same time. A worker computes a
//    placement holding only state_mutex, releases it, and only then takes
//    operation_mutex to publish the result. There is therefore no lock order to
//    invert.
//  * No callback, no event emission and no join ever happens under state_mutex
//    except the durable commit, which is a leaf operation that acquires no
//    lock owned by this runtime.
//  * ThreadPool::shutdown() joins workers that themselves take state_mutex, so
//    it is always called with state_mutex released.

#ifndef FLOW_OFFLOAD_SRC_ENGINE_INTERNAL_HPP
#define FLOW_OFFLOAD_SRC_ENGINE_INTERNAL_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "flow_offload/engine.hpp"

namespace flow_offload {

/// Deterministic rank tuple. Comparison is lexicographic over these fields; the
/// final field is the target identity, which is unique, so the order is total
/// and cannot depend on the order in which targets were supplied.
struct RankKey {
  std::uint8_t sticky{1};
  std::uint8_t domain_rank{1};
  std::uint8_t cost_rank{255};
  std::uint32_t utilisation_ppm{0};
  std::uint64_t target{0};
};

[[nodiscard]] int compare_rank(const RankKey& a, const RankKey& b) noexcept;

/// Saturating addition for counters, so accounting can never wrap to a small
/// value that looks like a fresh baseline.
[[nodiscard]] constexpr std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  return a > (std::numeric_limits<std::uint64_t>::max() - b) ? std::numeric_limits<std::uint64_t>::max()
                                                             : a + b;
}

/// The load actually offered to a target, and whether it is known at all.
struct EffectiveLoad {
  bool known{false};
  CapacityVector used{};
  ReasonCode reason{ReasonCode::RejectedMissingEvidence};
};

struct TargetState {
  TargetDescriptor descriptor{};
  /// True only when the target's descriptor is trustworthy in this coordinator
  /// incarnation, which requires an explicit declaration. A restored or
  /// invalidated target is not declared, so it can never accept a flow no matter
  /// what lifecycle value was persisted.
  bool declared{false};
  bool has_load{false};
  LoadEvidence load{};
  bool conflicting{false};
  LoadEvidence conflict_peer{};
  CapacityVector scheduled{};
};

struct StateHolder {
  FlowId flow{};
  FlowGeneration flow_generation{};
  TargetId target{};
  TargetIncarnation incarnation{};
  AssignmentId assignment{};
  bool handoff_pending{false};
  MigrationAttemptId pending_attempt{};
};

/// State of one asynchronous placement. Every field is guarded by
/// Impl::operation_mutex.
struct AsyncOperation {
  OperationId id{};
  OperationState state{OperationState::Pending};
  ReasonCode reason{ReasonCode::Ok};
  bool has_result{false};
  bool cancel_requested{false};
  PlacementResult result{};
  std::condition_variable ready{};
};

/// Priority used to pick the single most informative refusal reason for a flow
/// when every candidate was rejected. Deterministic: higher wins, ties go to the
/// numerically lower reason code.
[[nodiscard]] int refusal_priority(ReasonCode code) noexcept;

struct Engine::Impl {
  explicit Impl(const EngineConfig& config);
  ~Impl();

  EngineConfig config{};
  mutable std::mutex state_mutex;
  bool open{false};
  bool shutting_down{false};

  CoordinatorEpoch epoch{};
  BootId boot{};
  PolicyGeneration policy_generation{};
  ScheduleGeneration schedule_generation{};
  TopologyGeneration topology_generation{};
  Policy policy{};
  bool policy_present{false};

  std::map<FlowId, FlowDescriptor> flows;
  std::map<TargetId, TargetState> targets;
  std::map<FlowId, Assignment> assignments;
  std::map<ExclusiveStateKeyId, StateHolder> holders;
  std::map<MigrationAttemptId, MigrationRecord> migrations;
  std::map<MigrationContractId, MigrationContract> contracts;
  std::map<FenceId, FenceRecord> fences;
  std::map<EvidenceSourceId, SourceWatermark> watermarks;
  std::map<RequestId, RequestRecord> requests;
  std::map<std::uint32_t, std::uint64_t> refusal_counts;
  std::map<AuthorizationId, Authorization> authorizations;
  std::map<RecommendationId, Recommendation> recommendations;

  Counters counters{};
  std::uint64_t next_assignment{1};
  std::uint64_t next_recommendation{1};
  std::uint64_t next_authorization{1};
  std::uint64_t next_attempt{1};
  std::uint64_t next_fence{1};
  std::uint64_t next_operation{1};

  Store store;
  RecoveryReport recovery{};
  ThreadPool pool{};

  mutable std::mutex operation_mutex;
  std::map<OperationId, std::shared_ptr<AsyncOperation>> operations;

  // --- helpers; state_mutex must be held -----------------------------------
  [[nodiscard]] Status persist_locked();
  void record_refusal_locked(ReasonCode code);
  [[nodiscard]] DurableState build_durable_locked() const;
  [[nodiscard]] EffectiveLoad effective_load_locked(const TargetState& state,
                                                    Timestamp instant) const;
  [[nodiscard]] FenceId issue_fence_locked(FlowId flow, TargetId target, ReasonCode reason,
                                           Timestamp instant);
  void fence_all_active_locked(Timestamp instant);
  void rebuild_holders_locked();
  [[nodiscard]] const StateHolder* find_holder_locked(ExclusiveStateKeyId key) const;
  [[nodiscard]] std::size_t migrations_in_flight_locked() const;
  void evict_history_locked();
  void restore_locked(const DurableState& state);
  [[nodiscard]] ReasonCode dominant_refusal_locked(const std::vector<ReasonCode>& reasons) const;
  [[nodiscard]] CapacityVector committed_demand_for_locked(TargetId target) const;
  void recompute_scheduled_locked();
  [[nodiscard]] Policy effective_policy_locked() const;
  [[nodiscard]] ReasonCode evaluate_target_locked(const FlowDescriptor& flow,
                                                  const TargetState& target, Timestamp instant,
                                                  CandidateView& out) const;
};

/// Runs a placement request with state_mutex already held. Implemented in
/// placement.cpp so the decision procedure is readable on its own.
[[nodiscard]] PlacementResult place_locked(Engine::Impl& state, const PlacementRequest& request);

/// Runs a bounded rebalance with state_mutex already held.
[[nodiscard]] RebalanceReport rebalance_locked(Engine::Impl& state, const RebalanceRequest& request);

/// Canonical digest of a placement result's decision-relevant content.
[[nodiscard]] Sha256Digest placement_result_digest(const PlacementResult& result) noexcept;

/// Canonical digest of a placement request.
[[nodiscard]] Sha256Digest placement_request_digest(const PlacementRequest& request) noexcept;

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_SRC_ENGINE_INTERNAL_HPP
