// Flow Offload Scheduler - engine core: state, ingest, authority, effects.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/engine.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "flow_offload/serialize.hpp"
#include "engine_internal.hpp"

namespace flow_offload {
namespace {

[[nodiscard]] std::uint64_t saturating_increment(std::uint64_t value) noexcept {
  return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

[[nodiscard]] bool is_terminal(OperationState state) noexcept {
  return state == OperationState::Completed || state == OperationState::Cancelled ||
         state == OperationState::Rejected;
}

/// Identity of a declaration. The lifecycle is deliberately excluded: it is a
/// dynamic property that changes as a target is drained, quiesced or removed,
/// not part of what the target is.
[[nodiscard]] bool same_target_identity(const TargetDescriptor& a, const TargetDescriptor& b) noexcept {
  return a.target == b.target && a.incarnation == b.incarnation &&
         a.capability_generation == b.capability_generation &&
         a.topology_generation == b.topology_generation && a.domain == b.domain &&
         a.kind == b.kind && a.host == b.host && a.device == b.device &&
         a.capabilities == b.capabilities && a.capacity == b.capacity && a.reserved == b.reserved &&
         a.cost_class == b.cost_class && a.supports_stateful == b.supports_stateful &&
         a.supports_migration == b.supports_migration;
}

}  // namespace

std::string_view to_string(OperationState state) noexcept {
  switch (state) {
    case OperationState::Pending:
      return std::string_view{"pending"};
    case OperationState::Running:
      return std::string_view{"running"};
    case OperationState::Completed:
      return std::string_view{"completed"};
    case OperationState::Cancelled:
      return std::string_view{"cancelled"};
    case OperationState::Rejected:
      return std::string_view{"rejected"};
  }
  return std::string_view{"pending"};
}

int compare_rank(const RankKey& a, const RankKey& b) noexcept {
  if (a.sticky != b.sticky) {
    return a.sticky < b.sticky ? -1 : 1;
  }
  if (a.domain_rank != b.domain_rank) {
    return a.domain_rank < b.domain_rank ? -1 : 1;
  }
  if (a.cost_rank != b.cost_rank) {
    return a.cost_rank < b.cost_rank ? -1 : 1;
  }
  if (a.utilisation_ppm != b.utilisation_ppm) {
    return a.utilisation_ppm < b.utilisation_ppm ? -1 : 1;
  }
  if (a.target != b.target) {
    return a.target < b.target ? -1 : 1;
  }
  return 0;
}

int refusal_priority(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::RejectedMissingEvidence:
    case ReasonCode::RejectedConflictingEvidence:
    case ReasonCode::RejectedCapacityUnknown:
    case ReasonCode::RejectedUnknownEvidenceSource:
      return 100;
    case ReasonCode::RejectedStaleCapabilityGeneration:
    case ReasonCode::RejectedStaleTopologyGeneration:
    case ReasonCode::RejectedStaleIncarnation:
    case ReasonCode::RejectedStaleEvidence:
      return 90;
    case ReasonCode::RejectedTargetLifecycleUnknown:
    case ReasonCode::RejectedTargetNotReady:
    case ReasonCode::RejectedTargetDraining:
    case ReasonCode::RejectedTargetDegraded:
    case ReasonCode::RejectedTargetQuiesced:
    case ReasonCode::RejectedTargetRemoved:
      return 85;
    case ReasonCode::RejectedUnsupportedTarget:
    case ReasonCode::RejectedUnsupportedFunction:
    case ReasonCode::RejectedUnsupportedCapability:
      return 80;
    case ReasonCode::RejectedPolicyForbidsDomain:
    case ReasonCode::RejectedPolicyForbidsHost:
    case ReasonCode::RejectedPolicyForbidsDevice:
    case ReasonCode::RejectedPolicyFunctionNotAllowed:
    case ReasonCode::RejectedPolicyCostClass:
      return 70;
    case ReasonCode::RejectedAffinityUnsatisfiable:
      return 65;
    case ReasonCode::RejectedCapacityInsufficient:
    case ReasonCode::RejectedCapacityOverSubscribed:
    case ReasonCode::RejectedCapacityOverflow:
      return 60;
    case ReasonCode::RejectedExclusiveStateConflict:
      return 55;
    default:
      return 10;
  }
}

Engine::Impl::Impl(const EngineConfig& engine_config)
    : config(engine_config), store(engine_config.store) {
  store.set_crash_hook(engine_config.crash_hook, engine_config.crash_context);
}

Engine::Impl::~Impl() = default;

void Engine::Impl::record_refusal_locked(ReasonCode code) {
  const std::uint32_t key = reason_value(code);
  auto it = refusal_counts.find(key);
  if (it == refusal_counts.end()) {
    refusal_counts.emplace(key, 1U);
  } else {
    it->second = saturating_increment(it->second);
  }
  counters.refusals_by_reason_total = saturating_increment(counters.refusals_by_reason_total);
}

DurableState Engine::Impl::build_durable_locked() const {
  DurableState state{};
  state.semantics_version = kStateSemanticsVersion;
  state.epoch = epoch;
  state.boot = boot;
  state.schedule_generation = schedule_generation;
  state.topology_generation = topology_generation;
  state.policy = policy;
  state.policy_present = policy_present;
  state.counters = counters;
  state.flows.reserve(flows.size());
  for (const auto& entry : flows) {
    state.flows.push_back(entry.second);
  }
  state.targets.reserve(targets.size());
  for (const auto& entry : targets) {
    TargetDescriptor descriptor = entry.second.descriptor;
    // Liveness is never durable: a restart must re-establish it.
    descriptor.lifecycle = TargetLifecycle::Unknown;
    state.targets.push_back(descriptor);
  }
  state.assignments.reserve(assignments.size());
  for (const auto& entry : assignments) {
    Assignment assignment = entry.second;
    if (assignment.effect != EffectState::Verified) {
      // An unverified application is not a durable fact.
      assignment.effect = EffectState::Unknown;
    }
    state.assignments.push_back(assignment);
  }
  state.contracts.reserve(contracts.size());
  for (const auto& entry : contracts) {
    state.contracts.push_back(entry.second);
  }
  state.migrations.reserve(migrations.size());
  for (const auto& entry : migrations) {
    state.migrations.push_back(entry.second);
  }
  state.fences.reserve(fences.size());
  for (const auto& entry : fences) {
    state.fences.push_back(entry.second);
  }
  state.watermarks.reserve(watermarks.size());
  for (const auto& entry : watermarks) {
    state.watermarks.push_back(entry.second);
  }
  state.requests.reserve(requests.size());
  for (const auto& entry : requests) {
    state.requests.push_back(entry.second);
  }
  state.refusal_counts.reserve(refusal_counts.size());
  for (const auto& entry : refusal_counts) {
    state.refusal_counts.push_back(ReasonCount{static_cast<ReasonCode>(entry.first), entry.second});
  }
  state.canonicalize_order();
  return state;
}

Status Engine::Impl::persist_locked() {
  if (!config.persistence_enabled) {
    return Status::accepted(ReasonCode::PersistNotConfigured);
  }
  // Persistence is configured: the commit outcome is the operation outcome.
  counters.persist_commits = saturating_increment(counters.persist_commits);
  const DurableState state = build_durable_locked();
  const Status status = store.commit(state);
  if (status.failed()) {
    if (counters.persist_commits > 0U) {
      counters.persist_commits -= 1U;
    }
    counters.persist_failures = saturating_increment(counters.persist_failures);
  }
  return status;
}

CapacityVector Engine::Impl::committed_demand_for_locked(TargetId target) const {
  CapacityVector total{};
  for (const auto& entry : assignments) {
    if (entry.second.target != target) {
      continue;
    }
    const auto flow_it = flows.find(entry.first);
    if (flow_it == flows.end()) {
      continue;
    }
    CapacityVector sum{};
    if (!checked_add(total, flow_it->second.demand, sum)) {
      return CapacityVector::saturated();
    }
    total = sum;
  }
  return total;
}

void Engine::Impl::recompute_scheduled_locked() {
  for (auto& entry : targets) {
    entry.second.scheduled = CapacityVector::zero();
  }
  for (const auto& entry : assignments) {
    const auto target_it = targets.find(entry.second.target);
    if (target_it == targets.end()) {
      continue;
    }
    const auto flow_it = flows.find(entry.first);
    if (flow_it == flows.end()) {
      continue;
    }
    CapacityVector sum{};
    if (!checked_add(target_it->second.scheduled, flow_it->second.demand, sum)) {
      target_it->second.scheduled = CapacityVector::saturated();
    } else {
      target_it->second.scheduled = sum;
    }
  }
}

EffectiveLoad Engine::Impl::effective_load_locked(const TargetState& state,
                                                  Timestamp instant) const {
  EffectiveLoad load{};
  const bool observed_needed = policy.load_model != LoadModel::ScheduledOnly;
  bool observed_known = state.has_load && !state.conflicting &&
                        state.load.state == EvidenceState::Known;
  if (state.conflicting) {
    // Two sources disagree about this target at the same instant. Neither may be
    // preferred, so the load is unknown and the conflict is reported as such.
    load.known = false;
    load.reason = ReasonCode::RejectedConflictingEvidence;
    return load;
  }
  if (observed_known && config.evidence_freshness.nanos() > 0U) {
    const Duration age = saturating_elapsed(instant, state.load.observed_at);
    if (age.nanos() > config.evidence_freshness.nanos()) {
      observed_known = false;
    }
  }
  if (observed_needed && !observed_known && !state.conflicting) {
    load.known = false;
    load.reason = state.has_load ? (state.load.state == EvidenceState::Unsupported
                                        ? ReasonCode::RejectedUnsupportedTarget
                                        : ReasonCode::RejectedStaleEvidence)
                                 : ReasonCode::RejectedMissingEvidence;
    return load;
  }
  const CapacityVector observed = observed_known ? state.load.utilized : CapacityVector::zero();
  switch (policy.load_model) {
    case LoadModel::ObservedOnly:
      load.used = observed;
      break;
    case LoadModel::ScheduledOnly:
      load.used = state.scheduled;
      break;
    case LoadModel::MaximumOfBoth: {
      CapacityVector combined{};
      combined.packets_per_second = std::max(observed.packets_per_second, state.scheduled.packets_per_second);
      combined.bytes_per_second = std::max(observed.bytes_per_second, state.scheduled.bytes_per_second);
      combined.state_bytes = std::max(observed.state_bytes, state.scheduled.state_bytes);
      combined.table_entries = std::max(observed.table_entries, state.scheduled.table_entries);
      load.used = combined;
      break;
    }
    case LoadModel::SumConservative:
    case LoadModel::Unknown:
    default: {
      CapacityVector sum{};
      if (!checked_add(observed, state.scheduled, sum)) {
        load.known = false;
        load.reason = ReasonCode::RejectedCapacityOverflow;
        return load;
      }
      load.used = sum;
      break;
    }
  }
  load.known = true;
  load.reason = ReasonCode::Ok;
  return load;
}

FenceId Engine::Impl::issue_fence_locked(FlowId flow, TargetId target, ReasonCode reason,
                                         Timestamp instant) {
  FenceRecord record{};
  record.id = FenceId{next_fence};
  next_fence = saturating_increment(next_fence);
  record.epoch = epoch;
  record.boot = boot;
  record.flow = flow;
  record.target = target;
  record.reason = reason;
  record.active = true;
  record.issued_at = instant;
  const FenceId id = record.id;
  fences.emplace(id, record);
  // Fence history is bounded. An inactive fence is evicted first; an active one
  // is only evicted once every inactive fence is gone, and the eviction is
  // counted so a truncation is never silent.
  while (fences.size() > config.max_history) {
    auto victim = fences.end();
    for (auto it = fences.begin(); it != fences.end(); ++it) {
      if (!it->second.active) {
        victim = it;
        break;
      }
    }
    if (victim == fences.end()) {
      victim = fences.begin();
    }
    fences.erase(victim);
    counters.history_evictions = saturating_increment(counters.history_evictions);
  }
  return id;
}

void Engine::Impl::fence_all_active_locked(Timestamp instant) {
  for (auto& entry : fences) {
    if (entry.second.active) {
      entry.second.active = false;
      entry.second.reason = ReasonCode::RejectedFencedBySupersession;
    }
  }
  [[maybe_unused]] const FenceId restart_fence =
      issue_fence_locked(FlowId{}, TargetId{}, ReasonCode::RejectedFencedByRestart, instant);
}

const StateHolder* Engine::Impl::find_holder_locked(ExclusiveStateKeyId key) const {
  const auto it = holders.find(key);
  return it == holders.end() ? nullptr : &it->second;
}

std::size_t Engine::Impl::migrations_in_flight_locked() const {
  std::size_t count = 0;
  for (const auto& entry : migrations) {
    const MigrationPhase phase = entry.second.intent.phase;
    if (phase != MigrationPhase::Completed && phase != MigrationPhase::Aborted &&
        phase != MigrationPhase::Fenced) {
      ++count;
    }
  }
  return count;
}

void Engine::Impl::evict_history_locked() {
  while (recommendations.size() > config.max_history) {
    recommendations.erase(recommendations.begin());
    counters.history_evictions = saturating_increment(counters.history_evictions);
  }
  while (authorizations.size() > config.max_history) {
    authorizations.erase(authorizations.begin());
    counters.history_evictions = saturating_increment(counters.history_evictions);
  }
  while (requests.size() > config.max_requests) {
    requests.erase(requests.begin());
    counters.history_evictions = saturating_increment(counters.history_evictions);
  }
}

void Engine::Impl::rebuild_holders_locked() {
  holders.clear();
  for (const auto& entry : assignments) {
    const auto flow_it = flows.find(entry.first);
    if (flow_it == flows.end()) {
      continue;
    }
    if (flow_it->second.statefulness != Statefulness::Stateful) {
      continue;
    }
    const ExclusiveStateKeyId key = flow_it->second.exclusive_state_key;
    if (!key.is_valid()) {
      continue;
    }
    StateHolder holder{};
    holder.flow = entry.first;
    holder.flow_generation = entry.second.flow_generation;
    holder.target = entry.second.target;
    holder.incarnation = entry.second.incarnation;
    holder.assignment = entry.second.assignment;
    holders[key] = holder;
  }
}

ReasonCode Engine::Impl::dominant_refusal_locked(const std::vector<ReasonCode>& reasons) const {
  ReasonCode best = ReasonCode::RejectedNoEligibleTarget;
  int best_priority = -1;
  for (const ReasonCode code : reasons) {
    const int priority = refusal_priority(code);
    if (priority > best_priority ||
        (priority == best_priority && reason_value(code) < reason_value(best))) {
      best_priority = priority;
      best = code;
    }
  }
  return best;
}

Policy Engine::Impl::effective_policy_locked() const { return policy; }

void Engine::Impl::restore_locked(const DurableState& state) {
  epoch = state.epoch;
  boot = state.boot;
  policy = state.policy;
  policy_present = state.policy_present;
  policy_generation = state.policy.generation;
  schedule_generation = state.schedule_generation;
  topology_generation = state.topology_generation;
  counters = state.counters;

  flows.clear();
  for (const FlowDescriptor& descriptor : state.flows) {
    flows[descriptor.flow] = descriptor;
  }
  targets.clear();
  for (const TargetDescriptor& descriptor : state.targets) {
    TargetState target{};
    target.descriptor = descriptor;
    // A restored target is not declared: the owner must re-establish it in this
    // coordinator incarnation before it can accept a flow.
    target.declared = false;
    targets[descriptor.target] = target;
  }
  assignments.clear();
  for (const Assignment& assignment : state.assignments) {
    Assignment restored = assignment;
    if (restored.effect != EffectState::Verified) {
      restored.effect = EffectState::Unknown;
    }
    assignments[restored.flow] = restored;
  }
  contracts.clear();
  for (const MigrationContract& contract : state.contracts) {
    contracts[contract.id] = contract;
  }
  migrations.clear();
  for (const MigrationRecord& record : state.migrations) {
    migrations[record.intent.attempt] = record;
  }
  fences.clear();
  for (const FenceRecord& fence : state.fences) {
    FenceRecord restored = fence;
    restored.active = false;
    fences[restored.id] = restored;
  }
  watermarks.clear();
  for (const SourceWatermark& mark : state.watermarks) {
    watermarks[mark.source] = mark;
  }
  requests.clear();
  for (const RequestRecord& record : state.requests) {
    requests[record.request] = record;
  }
  refusal_counts.clear();
  for (const ReasonCount& count : state.refusal_counts) {
    refusal_counts[reason_value(count.code)] = count.count;
  }
  recommendations.clear();
  authorizations.clear();

  std::uint64_t max_assignment = 0;
  for (const auto& entry : assignments) {
    max_assignment = std::max(max_assignment, entry.second.assignment.value());
  }
  std::uint64_t max_attempt = 0;
  for (const auto& entry : migrations) {
    max_attempt = std::max(max_attempt, entry.second.intent.attempt.value());
  }
  std::uint64_t max_fence = 0;
  for (const auto& entry : fences) {
    max_fence = std::max(max_fence, entry.second.id.value());
  }
  next_assignment = saturating_increment(max_assignment);
  next_attempt = saturating_increment(max_attempt);
  next_fence = saturating_increment(max_fence);
  next_recommendation = 1;
  next_authorization = 1;

  rebuild_holders_locked();
  recompute_scheduled_locked();

  // Fence everything that was in flight when the previous incarnation ended.
  for (auto& entry : migrations) {
    MigrationPhase& phase = entry.second.intent.phase;
    if (phase == MigrationPhase::Completed || phase == MigrationPhase::Aborted ||
        phase == MigrationPhase::Fenced) {
      continue;
    }
    phase = MigrationPhase::Fenced;
    entry.second.abort_reason = ReasonCode::RejectedFencedByRestart;
    counters.migrations_fenced = saturating_increment(counters.migrations_fenced);
  }
  for (auto& entry : holders) {
    entry.second.handoff_pending = false;
    entry.second.pending_attempt = MigrationAttemptId{};
  }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

Engine::Engine(const EngineConfig& config) : impl_(std::make_unique<Impl>(config)) {}

Engine::~Engine() { shutdown(); }

Status Engine::open() {
  Impl& s = *impl_;
  Status result{ReasonCode::Ok};
  {
    std::lock_guard<std::mutex> lock(s.state_mutex);
    if (s.open) {
      return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
    }
    s.shutting_down = false;
    if (!s.config.persistence_enabled) {
      s.recovery = RecoveryReport{};
      s.recovery.kind = RecoveryKind::EmptyStore;
      s.recovery.code = ReasonCode::RecoveryEmptyStore;
      s.recovery.usable = true;
      s.recovery.detail = "persistence is disabled; state is in memory only";
    }
    CoordinatorEpoch previous_epoch{};
    BootId previous_boot{};
    if (s.config.persistence_enabled) {
      s.recovery = s.store.open();
      if (!s.recovery.usable) {
        return Status::refused(s.recovery.code);
      }
      if (s.store.has_state()) {
        const DurableState& state = s.store.state();
        previous_epoch = state.epoch;
        previous_boot = state.boot;
        s.restore_locked(state);
      }
    }
    if (previous_epoch.value() == std::numeric_limits<std::uint64_t>::max()) {
      return Status::refused(ReasonCode::RejectedInternalInvariant);
    }
    s.epoch = CoordinatorEpoch{previous_epoch.value() + 1U};
    if (s.config.boot_id_override.is_valid()) {
      s.boot = s.config.boot_id_override;
    } else {
      s.boot = derive_boot_id(previous_boot, s.epoch);
    }
    s.counters.restarts = saturating_increment(s.counters.restarts);
    if (previous_boot.is_valid()) {
      // Everything the previous incarnation could still have authorised is
      // fenced before any new authority can be granted.
      s.fence_all_active_locked(Timestamp{});
      if (s.recovery.usable) {
        s.recovery.detail += "; pre-restart authority fenced";
      }
    }
    s.open = true;
    result = s.persist_locked();
    if (result.failed()) {
      s.open = false;
      return result;
    }
  }
  s.pool.start(s.config.workers, s.config.max_queue_depth);
  return result;
}

void Engine::shutdown() {
  Impl& s = *impl_;
  {
    std::lock_guard<std::mutex> lock(s.state_mutex);
    if (s.shutting_down) {
      return;
    }
    s.shutting_down = true;
  }
  // Joining must never happen while holding state_mutex: workers take that lock.
  s.pool.shutdown();
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (s.open && s.config.persistence_enabled) {
    [[maybe_unused]] const Status persisted = s.persist_locked();
    [[maybe_unused]] const Status compacted = s.store.compact(s.build_durable_locked());
  }
  s.store.close();
  s.open = false;
  s.shutting_down = false;
}

bool Engine::is_open() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  return s.open;
}

const RecoveryReport& Engine::recovery() const { return impl_->recovery; }

EngineSnapshot Engine::snapshot() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  EngineSnapshot out{};
  out.epoch = s.epoch;
  out.boot = s.boot;
  out.policy_generation = s.policy_generation;
  out.schedule_generation = s.schedule_generation;
  out.topology_generation = s.topology_generation;
  out.recovery = s.recovery;
  out.counters = s.counters;
  out.flows = s.flows.size();
  out.targets = s.targets.size();
  for (const auto& entry : s.targets) {
    if (entry.second.declared && entry.second.descriptor.lifecycle == TargetLifecycle::Ready) {
      ++out.live_targets;
    }
  }
  out.assignments = s.assignments.size();
  out.migrations_in_flight = s.migrations_in_flight_locked();
  for (const auto& entry : s.fences) {
    if (entry.second.active) {
      ++out.active_fences;
    }
  }
  return out;
}

// ===========================================================================
// Ingest
// ===========================================================================

Status Engine::register_flow(const FlowDescriptor& flow) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  if (!flow.flow.is_valid() || !flow.generation.is_valid() || !flow.function.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (flow.statefulness == Statefulness::Unknown) {
    return Status::refused(ReasonCode::RejectedMissingEvidence);
  }
  if (flow.statefulness == Statefulness::Stateful && !flow.exclusive_state_key.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (flow.locality != LocalityKind::None && !flow.locality_anchor.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  ReasonCode outcome = ReasonCode::AcceptedFlowRegistered;
  const auto it = s.flows.find(flow.flow);
  if (it == s.flows.end()) {
    if (s.flows.size() >= s.config.max_flows) {
      return Status::refused(ReasonCode::RejectedResourceExhausted);
    }
    s.flows.emplace(flow.flow, flow);
    s.counters.flows_registered = saturating_increment(s.counters.flows_registered);
  } else if (it->second == flow) {
    return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
  } else if (flow.generation < it->second.generation) {
    return Status::refused(ReasonCode::RejectedStaleFlowGeneration);
  } else if (flow.generation == it->second.generation) {
    return Status::refused(ReasonCode::RejectedConflictingDuplicate);
  } else {
    // A new generation supersedes the previous processing incarnation: the old
    // exclusive state and any placement derived from it are released, and every
    // in-flight attempt for the old generation is fenced.
    if (it->second.statefulness == Statefulness::Stateful) {
      const auto holder_it = s.holders.find(it->second.exclusive_state_key);
      if (holder_it != s.holders.end() && holder_it->second.flow == flow.flow) {
        s.holders.erase(holder_it);
      }
    }
    const auto assignment_it = s.assignments.find(flow.flow);
    if (assignment_it != s.assignments.end()) {
      s.assignments.erase(assignment_it);
      s.recompute_scheduled_locked();
    }
    for (auto& entry : s.migrations) {
      MigrationRecord& record = entry.second;
      if (record.intent.flow != flow.flow) {
        continue;
      }
      if (record.intent.phase == MigrationPhase::Completed ||
          record.intent.phase == MigrationPhase::Aborted ||
          record.intent.phase == MigrationPhase::Fenced) {
        continue;
      }
      record.intent.phase = MigrationPhase::Fenced;
      record.abort_reason = ReasonCode::RejectedStaleFlowGeneration;
      s.counters.migrations_fenced = saturating_increment(s.counters.migrations_fenced);
    }
    it->second = flow;
    outcome = ReasonCode::AcceptedFlowUpdated;
  }
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(outcome);
}

Status Engine::remove_flow(FlowId flow, FlowGeneration generation) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  const auto it = s.flows.find(flow);
  if (it == s.flows.end()) {
    return Status::refused(ReasonCode::RejectedUnknownFlow);
  }
  if (generation.is_valid() && generation != it->second.generation) {
    return Status::refused(ReasonCode::RejectedStaleFlowGeneration);
  }
  if (it->second.statefulness == Statefulness::Stateful) {
    const auto holder_it = s.holders.find(it->second.exclusive_state_key);
    if (holder_it != s.holders.end() && holder_it->second.flow == flow) {
      s.holders.erase(holder_it);
    }
  }
  s.flows.erase(it);
  if (s.assignments.erase(flow) > 0U) {
    s.recompute_scheduled_locked();
  }
  for (auto& entry : s.migrations) {
    MigrationRecord& record = entry.second;
    if (record.intent.flow != flow) {
      continue;
    }
    if (record.intent.phase == MigrationPhase::Completed ||
        record.intent.phase == MigrationPhase::Aborted ||
        record.intent.phase == MigrationPhase::Fenced) {
      continue;
    }
    record.intent.phase = MigrationPhase::Fenced;
    record.abort_reason = ReasonCode::RejectedUnknownFlow;
    s.counters.migrations_fenced = saturating_increment(s.counters.migrations_fenced);
  }
  (void)s.issue_fence_locked(flow, TargetId{}, ReasonCode::RejectedFencedBySupersession, Timestamp{});
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
}

Status Engine::declare_target(const TargetDescriptor& target) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  if (!target.target.is_valid() || !target.incarnation.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (target.domain == ExecutionDomain::Unknown || target.kind == TargetKind::Unknown) {
    return Status::refused(ReasonCode::RejectedUnsupportedTarget);
  }
  if (target.cost_class == CostClass::Unknown) {
    return Status::refused(ReasonCode::RejectedPolicyCostClass);
  }
  if (target.domain == ExecutionDomain::OffloadDevice && !target.device.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (!target.host.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (target.reserved.exceeds_any(target.capacity)) {
    return Status::refused(ReasonCode::RejectedCapacityInsufficient);
  }
  if (target.topology_generation < s.topology_generation) {
    return Status::refused(ReasonCode::RejectedStaleTopologyGeneration);
  }

  ReasonCode outcome = ReasonCode::AcceptedTargetRegistered;
  const auto it = s.targets.find(target.target);
  if (it == s.targets.end()) {
    if (s.targets.size() >= s.config.max_targets) {
      return Status::refused(ReasonCode::RejectedResourceExhausted);
    }
    TargetState state{};
    state.descriptor = target;
    state.declared = true;
    s.targets.emplace(target.target, state);
    s.counters.targets_registered = saturating_increment(s.counters.targets_registered);
  } else {
    TargetState& existing = it->second;
    if (target.incarnation < existing.descriptor.incarnation) {
      return Status::refused(ReasonCode::RejectedStaleIncarnation);
    }
    if (target.incarnation == existing.descriptor.incarnation) {
      if (target.capability_generation < existing.descriptor.capability_generation) {
        return Status::refused(ReasonCode::RejectedStaleCapabilityGeneration);
      }
      if (target.capability_generation == existing.descriptor.capability_generation) {
        if (!same_target_identity(existing.descriptor, target)) {
          return Status::refused(ReasonCode::RejectedConflictingDuplicate);
        }
        const bool changed = !existing.declared ||
                             existing.descriptor.lifecycle != target.lifecycle;
        existing.descriptor = target;
        existing.declared = true;
        if (!changed) {
          return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
        }
        outcome = ReasonCode::AcceptedTargetUpdated;
      } else {
        // Capability generation advanced on the same incarnation. Every
        // decision taken against the previous capability set is stale: it can
        // no longer be authorised or verified, so its effect is demoted. The
        // recorded generations are left as the historical fact of what was
        // decided rather than being rewritten to look current.
        for (auto& entry : s.assignments) {
          if (entry.second.target == target.target) {
            entry.second.effect = EffectState::Unknown;
          }
        }
        existing.descriptor = target;
        existing.declared = true;
        existing.has_load = false;
        existing.conflicting = false;
        outcome = ReasonCode::AcceptedTargetUpdated;
      }
    } else {
      // New incarnation: everything derived from the previous incarnation is
      // stale and must not be reused.
      for (auto& entry : s.assignments) {
        if (entry.second.target == target.target) {
          entry.second.effect = EffectState::Unknown;
        }
      }
      for (auto& entry : s.migrations) {
        MigrationRecord& record = entry.second;
        const bool touches = record.intent.source_target == target.target ||
                             record.intent.destination_target == target.target;
        if (!touches || record.intent.phase == MigrationPhase::Completed ||
            record.intent.phase == MigrationPhase::Aborted ||
            record.intent.phase == MigrationPhase::Fenced) {
          continue;
        }
        record.intent.phase = MigrationPhase::Aborted;
        record.abort_reason = ReasonCode::RejectedStaleIncarnation;
        s.counters.migrations_aborted = saturating_increment(s.counters.migrations_aborted);
      }
      for (auto& entry : s.holders) {
        if (entry.second.target == target.target) {
          entry.second.incarnation = target.incarnation;
          entry.second.handoff_pending = false;
        }
      }
      existing = TargetState{};
      existing.descriptor = target;
      existing.declared = true;
      outcome = ReasonCode::AcceptedTargetUpdated;
    }
  }

  if (target.topology_generation > s.topology_generation) {
    s.topology_generation = target.topology_generation;
    // A newer topology generation invalidates every declaration derived from an
    // older one; those targets must be re-declared before they can host flows.
    for (auto& entry : s.targets) {
      if (entry.first == target.target) {
        continue;
      }
      if (entry.second.descriptor.topology_generation < s.topology_generation) {
        entry.second.declared = false;
        entry.second.has_load = false;
        entry.second.conflicting = false;
      }
    }
  }
  s.recompute_scheduled_locked();
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(outcome);
}

Status Engine::remove_target(TargetId target, TargetIncarnation incarnation, Timestamp instant) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  const auto it = s.targets.find(target);
  if (it == s.targets.end()) {
    return Status::refused(ReasonCode::RejectedUnknownTarget);
  }
  if (incarnation.is_valid() && incarnation != it->second.descriptor.incarnation) {
    return Status::refused(ReasonCode::RejectedStaleIncarnation);
  }
  // The descriptor stays trustworthy - the owner has told us exactly what
  // happened - but the lifecycle makes it unusable for new flows.
  it->second.declared = true;
  it->second.descriptor.lifecycle = TargetLifecycle::Removed;
  it->second.has_load = false;
  it->second.conflicting = false;

  for (auto& entry : s.assignments) {
    if (entry.second.target == target) {
      entry.second.effect = EffectState::Unknown;
    }
  }
  // Target disappearance mid-migration: every attempt that touches it is
  // aborted rather than left pending against a target that no longer exists.
  for (auto& entry : s.migrations) {
    MigrationRecord& record = entry.second;
    if (record.intent.phase == MigrationPhase::Completed ||
        record.intent.phase == MigrationPhase::Aborted ||
        record.intent.phase == MigrationPhase::Fenced) {
      continue;
    }
    if (record.intent.source_target != target && record.intent.destination_target != target) {
      continue;
    }
    record.intent.phase = MigrationPhase::Aborted;
    record.abort_reason = ReasonCode::RejectedTargetRemoved;
    s.counters.migrations_aborted = saturating_increment(s.counters.migrations_aborted);
    for (auto& holder : s.holders) {
      if (holder.second.pending_attempt == record.intent.attempt) {
        holder.second.handoff_pending = false;
        holder.second.pending_attempt = MigrationAttemptId{};
      }
    }
  }
  (void)s.issue_fence_locked(FlowId{}, target, ReasonCode::RejectedTargetRemoved, instant);
  s.counters.targets_removed = saturating_increment(s.counters.targets_removed);
  s.recompute_scheduled_locked();
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(ReasonCode::AcceptedTargetRemoved);
}

Status Engine::ingest_load(const LoadEvidence& evidence, Timestamp evaluation_instant) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  if (!evidence.target.is_valid() || !evidence.source.is_valid() ||
      !evidence.source_sequence.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (evidence.state == EvidenceState::Unknown) {
    return Status::refused(ReasonCode::RejectedMissingEvidence);
  }
  const auto it = s.targets.find(evidence.target);
  if (it == s.targets.end()) {
    return Status::refused(ReasonCode::RejectedUnknownTarget);
  }
  if (evidence.incarnation != it->second.descriptor.incarnation) {
    return Status::refused(ReasonCode::RejectedStaleIncarnation);
  }
  if (evidence.capability_generation != it->second.descriptor.capability_generation) {
    return Status::refused(ReasonCode::RejectedStaleCapabilityGeneration);
  }

  const auto mark_it = s.watermarks.find(evidence.source);
  if (mark_it != s.watermarks.end()) {
    const SourceWatermark& mark = mark_it->second;
    if (evidence.source_sequence < mark.sequence) {
      s.counters.evidence_rejected = saturating_increment(s.counters.evidence_rejected);
      return Status::refused(ReasonCode::RejectedReplayedEvidence);
    }
  }

  const bool same_source_as_current = it->second.has_load && it->second.load.source == evidence.source;
  if (it->second.has_load && same_source_as_current &&
      evidence.source_sequence == it->second.load.source_sequence) {
    if (evidence == it->second.load) {
      s.counters.evidence_idempotent = saturating_increment(s.counters.evidence_idempotent);
      return Status::accepted(ReasonCode::AcceptedIdempotentReplay);
    }
    s.counters.evidence_rejected = saturating_increment(s.counters.evidence_rejected);
    return Status::refused(ReasonCode::RejectedConflictingDuplicate);
  }

  if (it->second.has_load && evidence.observed_at < it->second.load.observed_at) {
    // Out-of-order delivery. It is accepted only when it agrees with what is
    // already known, so an older observation can never overwrite a newer one.
    if (evidence.state == it->second.load.state && evidence.utilized == it->second.load.utilized) {
      SourceWatermark& mark = s.watermarks[evidence.source];
      mark.source = evidence.source;
      if (evidence.source_sequence > mark.sequence) {
        mark.sequence = evidence.source_sequence;
      }
      s.counters.evidence_accepted = saturating_increment(s.counters.evidence_accepted);
      return Status::accepted(ReasonCode::AcceptedIdempotentReplay);
    }
    s.counters.evidence_rejected = saturating_increment(s.counters.evidence_rejected);
    return Status::refused(ReasonCode::RejectedOutOfOrderEvidence);
  }

  if (s.watermarks.size() >= s.config.max_evidence_sources &&
      s.watermarks.find(evidence.source) == s.watermarks.end()) {
    return Status::refused(ReasonCode::RejectedResourceExhausted);
  }

  TargetState& target = it->second;
  if (target.has_load && !same_source_as_current) {
    if (evidence.observed_at == target.load.observed_at &&
        evidence.utilized != target.load.utilized) {
      target.conflicting = true;
      target.conflict_peer = evidence;
      s.counters.evidence_conflicts = saturating_increment(s.counters.evidence_conflicts);
    } else if (evidence.observed_at == target.load.observed_at) {
      target.conflict_peer = evidence;
    } else {
      // A strictly newer observation from another source resolves any conflict.
      target.conflicting = false;
      target.conflict_peer = LoadEvidence{};
    }
  } else if (!target.has_load) {
    target.conflicting = false;
  } else if (same_source_as_current) {
    target.conflicting = false;
    target.conflict_peer = LoadEvidence{};
  }
  target.load = evidence;
  target.has_load = true;

  SourceWatermark& mark = s.watermarks[evidence.source];
  mark.source = evidence.source;
  if (evidence.source_sequence > mark.sequence) {
    mark.sequence = evidence.source_sequence;
  }
  mark.capability_generation = evidence.capability_generation;
  mark.target = evidence.target;
  mark.incarnation = evidence.incarnation;
  s.counters.evidence_accepted = saturating_increment(s.counters.evidence_accepted);
  (void)evaluation_instant;
  return Status::accepted(ReasonCode::AcceptedEvidenceRecorded);
}

Status Engine::set_policy(const Policy& policy) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  if (!policy.generation.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (policy.domain_preference == DomainPreference::Unknown ||
      policy.fallback == FallbackPolicy::Unknown ||
      policy.max_cost_class == CostClass::Unknown || policy.load_model == LoadModel::Unknown) {
    return Status::refused(ReasonCode::RejectedMissingEvidence);
  }
  Policy canonical = policy;
  canonicalize(canonical);
  if (s.policy_present) {
    if (canonical.generation < s.policy_generation) {
      return Status::refused(ReasonCode::RejectedStalePolicyGeneration);
    }
    if (canonical.generation == s.policy_generation) {
      if (canonical == s.policy) {
        return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
      }
      return Status::refused(ReasonCode::RejectedConflictingDuplicate);
    }
  }
  s.policy = canonical;
  s.policy_present = true;
  s.policy_generation = canonical.generation;
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(ReasonCode::AcceptedPolicyApplied);
}

Status Engine::set_topology_generation(TopologyGeneration generation) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  if (!generation.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (generation < s.topology_generation) {
    return Status::refused(ReasonCode::RejectedStaleTopologyGeneration);
  }
  if (generation == s.topology_generation) {
    return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
  }
  s.topology_generation = generation;
  for (auto& entry : s.targets) {
    if (entry.second.descriptor.topology_generation < generation) {
      entry.second.declared = false;
      entry.second.has_load = false;
      entry.second.conflicting = false;
    }
  }
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
}

Status Engine::register_migration_contract(const MigrationContract& contract) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  if (!contract.id.is_valid() || !contract.flow.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  const auto it = s.contracts.find(contract.id);
  if (it != s.contracts.end()) {
    if (it->second == contract) {
      return Status::accepted(ReasonCode::AcceptedIdempotentReplay);
    }
    return Status::refused(ReasonCode::RejectedConflictingDuplicate);
  }
  if (s.contracts.size() >= s.config.max_contracts) {
    return Status::refused(ReasonCode::RejectedResourceExhausted);
  }
  s.contracts.emplace(contract.id, contract);
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
}

// ===========================================================================
// Authority
// ===========================================================================

Status Engine::authorize(RecommendationId recommendation, Timestamp instant, Authorization& out) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  const auto it = s.recommendations.find(recommendation);
  if (it == s.recommendations.end()) {
    s.counters.authorizations_refused = saturating_increment(s.counters.authorizations_refused);
    return Status::refused(ReasonCode::RejectedRecommendationMismatch);
  }
  const Recommendation& rec = it->second;
  ReasonCode refusal = ReasonCode::Ok;
  if (rec.outcome != ReasonCode::AcceptedHostPlacement &&
      rec.outcome != ReasonCode::AcceptedOffloadPlacement &&
      rec.outcome != ReasonCode::AcceptedStickyRetention &&
      rec.outcome != ReasonCode::AcceptedFallbackToHost &&
      rec.outcome != ReasonCode::AcceptedFallbackToOffload &&
      rec.outcome != ReasonCode::AcceptedWithoutLoadEvidence) {
    refusal = ReasonCode::RejectedRecommendationMismatch;
  } else if (rec.epoch != s.epoch || rec.boot != s.boot) {
    refusal = ReasonCode::RejectedFencedByRestart;
  } else if (rec.schedule_generation != s.schedule_generation) {
    refusal = ReasonCode::RejectedStaleScheduleGeneration;
  } else if (rec.policy_generation != s.policy_generation) {
    refusal = ReasonCode::RejectedStalePolicyGeneration;
  } else if (rec.topology_generation != s.topology_generation) {
    refusal = ReasonCode::RejectedStaleTopologyGeneration;
  } else {
    const auto target_it = s.targets.find(rec.target);
    if (target_it == s.targets.end() || !target_it->second.declared) {
      refusal = ReasonCode::RejectedTargetLifecycleUnknown;
    } else if (target_it->second.descriptor.lifecycle != TargetLifecycle::Ready) {
      refusal = ReasonCode::RejectedTargetNotReady;
    } else if (target_it->second.descriptor.incarnation != rec.incarnation) {
      refusal = ReasonCode::RejectedStaleIncarnation;
    } else if (target_it->second.descriptor.capability_generation != rec.capability_generation) {
      refusal = ReasonCode::RejectedStaleCapabilityGeneration;
    } else {
      const auto flow_it = s.flows.find(rec.flow);
      if (flow_it == s.flows.end()) {
        refusal = ReasonCode::RejectedUnknownFlow;
      } else if (flow_it->second.generation != rec.flow_generation) {
        refusal = ReasonCode::RejectedStaleFlowGeneration;
      } else {
        const auto assignment_it = s.assignments.find(rec.flow);
        if (assignment_it == s.assignments.end() || assignment_it->second.target != rec.target) {
          refusal = ReasonCode::RejectedRecommendationMismatch;
        }
      }
    }
  }
  if (refusal != ReasonCode::Ok) {
    s.counters.authorizations_refused = saturating_increment(s.counters.authorizations_refused);
    s.record_refusal_locked(refusal);
    return Status::refused(refusal);
  }
  Authorization authorization{};
  authorization.id = AuthorizationId{s.next_authorization};
  s.next_authorization = saturating_increment(s.next_authorization);
  authorization.recommendation = rec.id;
  authorization.request = rec.request;
  authorization.flow = rec.flow;
  authorization.flow_generation = rec.flow_generation;
  authorization.target = rec.target;
  authorization.incarnation = rec.incarnation;
  authorization.capability_generation = rec.capability_generation;
  authorization.policy_generation = s.policy_generation;
  authorization.schedule_generation = s.schedule_generation;
  authorization.epoch = s.epoch;
  authorization.boot = s.boot;
  authorization.fence = s.issue_fence_locked(rec.flow, rec.target,
                                             ReasonCode::AcceptedAuthorizationGranted, instant);
  authorization.outcome = ReasonCode::AcceptedAuthorizationGranted;
  authorization.granted_at = instant;
  authorization.digest = digest_of(authorization);
  s.authorizations.emplace(authorization.id, authorization);
  s.counters.authorizations_granted = saturating_increment(s.counters.authorizations_granted);
  s.evict_history_locked();
  out = authorization;
  return Status::accepted(ReasonCode::AcceptedAuthorizationGranted);
}

Status Engine::revoke_authorization(AuthorizationId authorization) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  const auto it = s.authorizations.find(authorization);
  if (it == s.authorizations.end()) {
    return Status::refused(ReasonCode::RejectedAuthorizationMissing);
  }
  if (it->second.outcome != ReasonCode::AcceptedAuthorizationGranted) {
    return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
  }
  it->second.outcome = ReasonCode::RejectedAuthorizationSuperseded;
  s.counters.authorizations_revoked = saturating_increment(s.counters.authorizations_revoked);
  const auto fence_it = s.fences.find(it->second.fence);
  if (fence_it != s.fences.end()) {
    fence_it->second.active = false;
    fence_it->second.reason = ReasonCode::RejectedAuthorizationSuperseded;
  }
  return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
}

// ===========================================================================
// Effect lifecycle
// ===========================================================================

Status Engine::issue_intent(const Authorization& authorization, MigrationIntent& inout) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  const auto auth_it = s.authorizations.find(authorization.id);
  if (auth_it == s.authorizations.end()) {
    return Status::refused(ReasonCode::RejectedAuthorizationMissing);
  }
  const Authorization& stored = auth_it->second;
  if (stored.outcome != ReasonCode::AcceptedAuthorizationGranted) {
    return Status::refused(ReasonCode::RejectedAuthorizationSuperseded);
  }
  if (stored.epoch != s.epoch || stored.boot != s.boot) {
    return Status::refused(ReasonCode::RejectedFencedByRestart);
  }
  if (stored.policy_generation != s.policy_generation) {
    return Status::refused(ReasonCode::RejectedStalePolicyGeneration);
  }
  if (stored.schedule_generation != s.schedule_generation) {
    return Status::refused(ReasonCode::RejectedStaleScheduleGeneration);
  }
  const auto flow_it = s.flows.find(stored.flow);
  if (flow_it == s.flows.end()) {
    return Status::refused(ReasonCode::RejectedUnknownFlow);
  }
  const FlowDescriptor& flow = flow_it->second;
  if (flow.generation != stored.flow_generation) {
    return Status::refused(ReasonCode::RejectedStaleFlowGeneration);
  }
  const auto assignment_it = s.assignments.find(stored.flow);
  if (assignment_it == s.assignments.end()) {
    return Status::refused(ReasonCode::RejectedRecommendationMismatch);
  }
  const Assignment& source = assignment_it->second;
  if (!inout.destination_target.is_valid() || !inout.destination_incarnation.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  if (inout.destination_target == source.target &&
      inout.destination_incarnation == source.incarnation) {
    return Status::refused(ReasonCode::RejectedMigrationSameTarget);
  }
  const auto destination_it = s.targets.find(inout.destination_target);
  if (destination_it == s.targets.end() || !destination_it->second.declared) {
    return Status::refused(ReasonCode::RejectedTargetLifecycleUnknown);
  }
  if (destination_it->second.descriptor.lifecycle != TargetLifecycle::Ready) {
    return Status::refused(ReasonCode::RejectedTargetNotReady);
  }
  if (destination_it->second.descriptor.incarnation != inout.destination_incarnation) {
    return Status::refused(ReasonCode::RejectedStaleIncarnation);
  }
  if (destination_it->second.descriptor.capability_generation !=
      inout.destination_capability_generation) {
    return Status::refused(ReasonCode::RejectedStaleCapabilityGeneration);
  }
  const auto source_target_it = s.targets.find(source.target);
  if (source_target_it != s.targets.end() && !source_target_it->second.descriptor.supports_migration) {
    return Status::refused(ReasonCode::RejectedMigrationNotPermitted);
  }
  if (!flow.allow_migration && !s.policy.allow_migration) {
    return Status::refused(ReasonCode::RejectedMigrationNotPermitted);
  }
  if (s.migrations_in_flight_locked() >= s.config.max_migrations) {
    return Status::refused(ReasonCode::RejectedMigrationLimitReached);
  }

  MigrationContractId contract_id{};
  if (flow.statefulness == Statefulness::Stateful) {
    const MigrationContract* match = nullptr;
    for (const auto& entry : s.contracts) {
      const MigrationContract& candidate = entry.second;
      if (candidate.flow != stored.flow || candidate.flow_generation != flow.generation) {
        continue;
      }
      if (candidate.destination_target != inout.destination_target ||
          candidate.destination_incarnation != inout.destination_incarnation ||
          candidate.destination_capability_generation != inout.destination_capability_generation) {
        continue;
      }
      if (candidate.policy_generation != s.policy_generation) {
        continue;
      }
      match = &candidate;
      break;
    }
    if (match == nullptr) {
      return Status::refused(ReasonCode::RejectedMigrationContractMissing);
    }
    if (match->statefulness != Statefulness::Stateful) {
      return Status::refused(ReasonCode::RejectedMigrationContractMismatch);
    }
    if (!contract_is_safe_for_stateful(*match)) {
      return Status::refused(ReasonCode::RejectedStatefulMigrationUnsafe);
    }
    if (match->max_state_bytes != 0U && flow.demand.state_bytes > match->max_state_bytes) {
      return Status::refused(ReasonCode::RejectedMigrationContractMismatch);
    }
    contract_id = match->id;
    const StateHolder* holder = s.find_holder_locked(flow.exclusive_state_key);
    if (holder != nullptr && holder->handoff_pending) {
      return Status::refused(ReasonCode::RejectedMigrationAlreadyInFlight);
    }
    if (holder != nullptr && holder->flow != stored.flow) {
      return Status::refused(ReasonCode::RejectedExclusiveStateConflict);
    }
  }

  MigrationIntent intent{};
  intent.attempt = MigrationAttemptId{s.next_attempt};
  s.next_attempt = saturating_increment(s.next_attempt);
  intent.flow = stored.flow;
  intent.flow_generation = flow.generation;
  intent.source_assignment = source.assignment;
  intent.source_target = source.target;
  intent.source_incarnation = source.incarnation;
  intent.source_capability_generation = source.capability_generation;
  intent.destination_target = inout.destination_target;
  intent.destination_incarnation = inout.destination_incarnation;
  intent.destination_capability_generation = inout.destination_capability_generation;
  intent.contract = contract_id;
  intent.policy_generation = s.policy_generation;
  intent.schedule_generation = s.schedule_generation;
  intent.epoch = s.epoch;
  intent.boot = s.boot;
  intent.fence = s.issue_fence_locked(stored.flow, stored.target,
                                      ReasonCode::AcceptedMigrationIntentIssued, inout.issued_at);
  intent.phase = MigrationPhase::IntentIssued;
  intent.issued_at = inout.issued_at;

  MigrationRecord record{};
  record.intent = intent;
  s.migrations.emplace(intent.attempt, record);
  assignment_it->second.effect = EffectState::IntentIssued;
  if (flow.statefulness == Statefulness::Stateful) {
    StateHolder& holder = s.holders[flow.exclusive_state_key];
    holder.flow = stored.flow;
    holder.flow_generation = flow.generation;
    holder.target = source.target;
    holder.incarnation = source.incarnation;
    holder.assignment = source.assignment;
    holder.handoff_pending = true;
    holder.pending_attempt = intent.attempt;
  }
  s.counters.migrations_issued = saturating_increment(s.counters.migrations_issued);
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  inout = intent;
  return Status::accepted(ReasonCode::AcceptedMigrationIntentIssued);
}

Status Engine::acknowledge(const MigrationAcknowledgement& acknowledgement) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  const auto it = s.migrations.find(acknowledgement.attempt);
  if (it == s.migrations.end()) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedMigrationAttemptUnknown);
  }
  MigrationRecord& record = it->second;
  if (record.intent.phase == MigrationPhase::Fenced) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedFencedByRestart);
  }
  if (record.intent.phase == MigrationPhase::Completed ||
      record.intent.phase == MigrationPhase::Aborted) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedMigrationAttemptTerminal);
  }
  if (record.intent.boot != s.boot || acknowledgement.boot != s.boot) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedFencedByRestart);
  }
  if (record.intent.epoch != s.epoch || acknowledgement.epoch != s.epoch) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedStaleCoordinatorEpoch);
  }
  if (acknowledgement.actor == MigrationActor::Unknown) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  const bool is_source = acknowledgement.actor == MigrationActor::Source;
  const TargetId expected_target = is_source ? record.intent.source_target : record.intent.destination_target;
  const TargetIncarnation expected_incarnation =
      is_source ? record.intent.source_incarnation : record.intent.destination_incarnation;
  const CapabilityGeneration expected_capability =
      is_source ? record.intent.source_capability_generation
                : record.intent.destination_capability_generation;
  if (acknowledgement.target != expected_target) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedConflictingAuthority);
  }
  if (acknowledgement.incarnation != expected_incarnation) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedStaleIncarnation);
  }
  if (acknowledgement.capability_generation != expected_capability) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedStaleCapabilityGeneration);
  }
  MigrationAcknowledgement* slot =
      is_source ? &record.source_acknowledgement : &record.destination_acknowledgement;
  bool* flag = is_source ? &record.source_acknowledged : &record.destination_acknowledged;
  if (*flag) {
    if (*slot == acknowledgement) {
      s.counters.evidence_idempotent = saturating_increment(s.counters.evidence_idempotent);
      return Status::accepted(ReasonCode::AcceptedIdempotentReplay);
    }
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedMigrationAckDuplicate);
  }
  if (is_source && record.intent.phase != MigrationPhase::IntentIssued) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedMigrationAckOutOfOrder);
  }
  if (!is_source && record.intent.phase != MigrationPhase::SourceAcknowledged) {
    s.counters.acknowledgements_rejected = saturating_increment(s.counters.acknowledgements_rejected);
    return Status::refused(ReasonCode::RejectedMigrationAckOutOfOrder);
  }
  *slot = acknowledgement;
  *flag = true;
  s.counters.acknowledgements_accepted = saturating_increment(s.counters.acknowledgements_accepted);
  if (!acknowledgement.accepted) {
    record.intent.phase = MigrationPhase::Aborted;
    record.abort_reason = acknowledgement.reason == ReasonCode::Ok ? ReasonCode::RejectedMigrationFenced
                                                                  : acknowledgement.reason;
    s.counters.migrations_aborted = saturating_increment(s.counters.migrations_aborted);
    for (auto& holder : s.holders) {
      if (holder.second.pending_attempt == record.intent.attempt) {
        holder.second.handoff_pending = false;
        holder.second.pending_attempt = MigrationAttemptId{};
      }
    }
    const Status persisted = s.persist_locked();
    if (persisted.failed() && persisted.code() != ReasonCode::PersistNotConfigured &&
        persisted.code() != ReasonCode::RecoveryCompacted) {
      return persisted;
    }
    return Status::accepted(ReasonCode::AcceptedMigrationAborted);
  }
  record.intent.phase = is_source ? MigrationPhase::SourceAcknowledged
                                  : MigrationPhase::DestinationAcknowledged;
  const auto assignment_it = s.assignments.find(record.intent.flow);
  if (assignment_it != s.assignments.end()) {
    assignment_it->second.effect = EffectState::Acknowledged;
  }
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(is_source ? ReasonCode::AcceptedSourceAckRecorded
                                    : ReasonCode::AcceptedDestinationAckRecorded);
}

Status Engine::report_applied(const AppliedEffect& effect) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  const auto it = s.migrations.find(effect.attempt);
  if (it == s.migrations.end()) {
    return Status::refused(ReasonCode::RejectedMigrationAttemptUnknown);
  }
  MigrationRecord& record = it->second;
  if (record.intent.phase == MigrationPhase::Fenced) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedFencedByRestart);
  }
  if (record.intent.phase == MigrationPhase::Completed ||
      record.intent.phase == MigrationPhase::Aborted) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedMigrationAttemptTerminal);
  }
  if (record.effect_applied) {
    if (record.applied == effect) {
      return Status::accepted(ReasonCode::AcceptedIdempotentReplay);
    }
    return Status::refused(ReasonCode::RejectedConflictingDuplicate);
  }
  if (record.intent.phase != MigrationPhase::DestinationAcknowledged) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedMigrationEffectBeforeAck);
  }
  if (effect.boot != s.boot || record.intent.boot != s.boot) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedFencedByRestart);
  }
  if (effect.epoch != s.epoch) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedStaleCoordinatorEpoch);
  }
  if (effect.target != record.intent.destination_target) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedConflictingAuthority);
  }
  if (effect.incarnation != record.intent.destination_incarnation) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedStaleIncarnation);
  }
  if (effect.capability_generation != record.intent.destination_capability_generation) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedStaleCapabilityGeneration);
  }
  if (effect.effect_digest.is_zero()) {
    s.counters.effects_rejected = saturating_increment(s.counters.effects_rejected);
    return Status::refused(ReasonCode::RejectedMissingEvidence);
  }
  record.applied = effect;
  record.effect_applied = true;
  record.intent.phase = MigrationPhase::EffectApplied;
  const auto assignment_it = s.assignments.find(record.intent.flow);
  if (assignment_it != s.assignments.end()) {
    assignment_it->second.effect = EffectState::Applied;
  }
  s.counters.effects_applied = saturating_increment(s.counters.effects_applied);
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(ReasonCode::AcceptedAppliedEffectRecorded);
}

Status Engine::observe(const EffectObservation& observation) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  if (!observation.flow.is_valid() || !observation.source.is_valid() ||
      !observation.source_sequence.is_valid()) {
    return Status::refused(ReasonCode::RejectedMalformedInput);
  }
  // Find the single in-flight attempt for this flow. Two concurrent attempts for
  // one flow would be a double assignment of its exclusive state.
  MigrationAttemptId attempt{};
  std::size_t matches = 0;
  for (const auto& entry : s.migrations) {
    const MigrationRecord& candidate = entry.second;
    if (candidate.intent.flow != observation.flow) {
      continue;
    }
    if (candidate.intent.phase == MigrationPhase::Completed ||
        candidate.intent.phase == MigrationPhase::Aborted ||
        candidate.intent.phase == MigrationPhase::Fenced) {
      continue;
    }
    attempt = candidate.intent.attempt;
    ++matches;
  }
  if (matches == 0) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedMigrationAttemptUnknown);
  }
  if (matches > 1) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedExclusiveStateConflict);
  }
  MigrationRecord& record = s.migrations[attempt];
  if (observation.state != EvidenceState::Known) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    switch (observation.state) {
      case EvidenceState::Conflicting:
        return Status::refused(ReasonCode::RejectedConflictingEvidence);
      case EvidenceState::Stale:
        return Status::refused(ReasonCode::RejectedStaleEvidence);
      case EvidenceState::Unsupported:
        return Status::refused(ReasonCode::RejectedUnsupportedTarget);
      case EvidenceState::Unknown:
      default:
        return Status::refused(ReasonCode::RejectedMissingEvidence);
    }
  }
  if (observation.lifecycle != TargetLifecycle::Ready) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    switch (observation.lifecycle) {
      case TargetLifecycle::Removed:
        return Status::refused(ReasonCode::RejectedTargetRemoved);
      case TargetLifecycle::Draining:
        return Status::refused(ReasonCode::RejectedTargetDraining);
      case TargetLifecycle::Degraded:
        return Status::refused(ReasonCode::RejectedTargetDegraded);
      case TargetLifecycle::Quiesced:
        return Status::refused(ReasonCode::RejectedTargetQuiesced);
      case TargetLifecycle::Discovered:
        return Status::refused(ReasonCode::RejectedTargetNotReady);
      case TargetLifecycle::Unknown:
      default:
        return Status::refused(ReasonCode::RejectedTargetLifecycleUnknown);
    }
  }
  if (record.intent.phase == MigrationPhase::Fenced) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedFencedByRestart);
  }
  if (record.intent.phase == MigrationPhase::Completed ||
      record.intent.phase == MigrationPhase::Aborted) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedMigrationAttemptTerminal);
  }
  if (!record.effect_applied) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedMigrationEffectBeforeAck);
  }
  if (observation.target != record.intent.destination_target) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedConflictingAuthority);
  }
  if (observation.incarnation != record.intent.destination_incarnation) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedStaleIncarnation);
  }
  if (observation.capability_generation != record.intent.destination_capability_generation) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedStaleCapabilityGeneration);
  }
  if (observation.effect_digest != record.applied.effect_digest) {
    s.counters.observations_rejected = saturating_increment(s.counters.observations_rejected);
    return Status::refused(ReasonCode::RejectedMigrationVerificationMismatch);
  }

  record.effect_verified = true;
  record.verified_digest = observation.effect_digest;
  record.intent.phase = MigrationPhase::Completed;
  auto assignment_it = s.assignments.find(record.intent.flow);
  if (assignment_it != s.assignments.end()) {
    const Assignment previous = assignment_it->second;
    Assignment updated = previous;
    updated.assignment = AssignmentId{s.next_assignment};
    s.next_assignment = saturating_increment(s.next_assignment);
    updated.target = record.intent.destination_target;
    updated.incarnation = record.intent.destination_incarnation;
    updated.capability_generation = record.intent.destination_capability_generation;
    updated.policy_generation = record.intent.policy_generation;
    updated.schedule_generation = record.intent.schedule_generation;
    updated.effect = EffectState::Verified;
    updated.reason = ReasonCode::AcceptedVerifiedEffectRecorded;
    updated.committed_at = observation.observed_at;
    updated.supersedes = previous.assignment;
    assignment_it->second = updated;

    const auto flow_it = s.flows.find(record.intent.flow);
    if (flow_it != s.flows.end() && flow_it->second.statefulness == Statefulness::Stateful) {
      const ExclusiveStateKeyId key = flow_it->second.exclusive_state_key;
      StateHolder& holder = s.holders[key];
      holder.flow = record.intent.flow;
      holder.flow_generation = record.intent.flow_generation;
      holder.target = updated.target;
      holder.incarnation = updated.incarnation;
      holder.assignment = updated.assignment;
      holder.handoff_pending = false;
      holder.pending_attempt = MigrationAttemptId{};
    }
  }
  // The fence that guarded this attempt is released only now, after the effect
  // has been verified rather than merely acknowledged.
  const auto fence_it = s.fences.find(record.intent.fence);
  if (fence_it != s.fences.end()) {
    fence_it->second.active = false;
    fence_it->second.reason = ReasonCode::AcceptedVerifiedEffectRecorded;
  }
  s.counters.effects_verified = saturating_increment(s.counters.effects_verified);
  s.counters.migrations_completed = saturating_increment(s.counters.migrations_completed);
  s.recompute_scheduled_locked();
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(ReasonCode::AcceptedVerifiedEffectRecorded);
}

Status Engine::abort_migration(MigrationAttemptId attempt, ReasonCode reason, Timestamp instant) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  if (!s.open) {
    return Status::refused(ReasonCode::RejectedShuttingDown);
  }
  const auto it = s.migrations.find(attempt);
  if (it == s.migrations.end()) {
    return Status::refused(ReasonCode::RejectedMigrationAttemptUnknown);
  }
  MigrationRecord& record = it->second;
  if (record.intent.phase == MigrationPhase::Completed) {
    return Status::refused(ReasonCode::RejectedMigrationAttemptTerminal);
  }
  if (record.intent.phase == MigrationPhase::Aborted || record.intent.phase == MigrationPhase::Fenced) {
    return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
  }
  record.intent.phase = MigrationPhase::Aborted;
  record.abort_reason = reason == ReasonCode::Ok ? ReasonCode::RejectedMigrationFenced : reason;
  s.counters.migrations_aborted = saturating_increment(s.counters.migrations_aborted);
  for (auto& holder : s.holders) {
    if (holder.second.pending_attempt == attempt) {
      holder.second.handoff_pending = false;
      holder.second.pending_attempt = MigrationAttemptId{};
    }
  }
  const auto fence_it = s.fences.find(record.intent.fence);
  if (fence_it != s.fences.end()) {
    fence_it->second.active = false;
    fence_it->second.reason = record.abort_reason;
  }
  const auto assignment_it = s.assignments.find(record.intent.flow);
  if (assignment_it != s.assignments.end()) {
    assignment_it->second.effect = EffectState::Unknown;
  }
  (void)instant;
  const Status persisted = s.persist_locked();
  if (persisted.failed()) {
    return persisted;
  }
  return Status::accepted(ReasonCode::AcceptedMigrationAborted);
}

// ===========================================================================
// Asynchronous operations
// ===========================================================================

OperationId Engine::submit(const PlacementRequest& request) {
  Impl& s = *impl_;
  {
    std::lock_guard<std::mutex> lock(s.state_mutex);
    if (!s.open) {
      return OperationId{};
    }
  }
  std::shared_ptr<AsyncOperation> operation;
  bool backpressured = false;
  {
    std::lock_guard<std::mutex> lock(s.operation_mutex);
    std::size_t terminal = 0;
    for (const auto& entry : s.operations) {
      if (is_terminal(entry.second->state)) {
        ++terminal;
      }
    }
    while (s.operations.size() >= s.config.max_requests && terminal > 0) {
      bool erased = false;
      for (auto it = s.operations.begin(); it != s.operations.end(); ++it) {
        if (is_terminal(it->second->state)) {
          s.operations.erase(it);
          --terminal;
          erased = true;
          break;
        }
      }
      if (!erased) {
        break;
      }
    }
    if (s.operations.size() >= s.config.max_requests) {
      backpressured = true;
    } else {
      operation = std::make_shared<AsyncOperation>();
      operation->id = OperationId{s.next_operation};
      s.next_operation = saturating_increment(s.next_operation);
      s.operations.emplace(operation->id, operation);
    }
  }
  // Counters live under state_mutex, so they are updated outside the operation
  // lock rather than by nesting the two.
  {
    std::lock_guard<std::mutex> lock(s.state_mutex);
    if (backpressured) {
      s.counters.operations_backpressured = saturating_increment(s.counters.operations_backpressured);
      return OperationId{};
    }
    s.counters.operations_submitted = saturating_increment(s.counters.operations_submitted);
  }
  const OperationId id = operation->id;
  const bool queued = s.pool.try_submit([this, operation, request] {
    bool start = false;
    {
      std::lock_guard<std::mutex> lock(impl_->operation_mutex);
      if (operation->cancel_requested) {
        operation->state = OperationState::Cancelled;
        operation->reason = ReasonCode::RejectedCancelled;
      } else {
        operation->state = OperationState::Running;
        start = true;
      }
    }
    if (!start) {
      operation->ready.notify_all();
      return;
    }
    PlacementResult result = this->place(request);
    {
      std::lock_guard<std::mutex> lock(impl_->operation_mutex);
      operation->result = std::move(result);
      operation->has_result = true;
      if (is_acceptance(operation->result.status)) {
        operation->state = OperationState::Completed;
      } else {
        operation->state = OperationState::Rejected;
        operation->reason = operation->result.status;
      }
    }
    operation->ready.notify_all();
  });
  if (!queued) {
    {
      std::lock_guard<std::mutex> lock(s.operation_mutex);
      const auto it = s.operations.find(id);
      if (it != s.operations.end()) {
        it->second->state = OperationState::Rejected;
        it->second->reason = ReasonCode::RejectedQueueFull;
      }
    }
    std::lock_guard<std::mutex> lock(s.state_mutex);
    s.counters.operations_backpressured = saturating_increment(s.counters.operations_backpressured);
  }
  return id;
}

bool Engine::wait_for(OperationId id, OperationStatus& out) {
  Impl& s = *impl_;
  std::shared_ptr<AsyncOperation> operation;
  {
    std::lock_guard<std::mutex> lock(s.operation_mutex);
    const auto it = s.operations.find(id);
    if (it == s.operations.end()) {
      return false;
    }
    operation = it->second;
  }
  std::unique_lock<std::mutex> lock(s.operation_mutex);
  operation->ready.wait(lock, [&operation] { return is_terminal(operation->state); });
  out.id = operation->id;
  out.state = operation->state;
  out.reason = operation->reason;
  out.has_result = operation->has_result;
  out.result = operation->result;
  return true;
}

OperationStatus Engine::poll(OperationId id) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.operation_mutex);
  OperationStatus out{};
  const auto it = s.operations.find(id);
  if (it == s.operations.end()) {
    out.state = OperationState::Rejected;
    out.reason = ReasonCode::RejectedResourceExhausted;
    return out;
  }
  out.id = it->second->id;
  out.state = it->second->state;
  out.reason = it->second->reason;
  out.has_result = it->second->has_result;
  out.result = it->second->result;
  return out;
}

Status Engine::cancel(OperationId id) {
  Impl& s = *impl_;
  {
    std::lock_guard<std::mutex> lock(s.operation_mutex);
    const auto it = s.operations.find(id);
    if (it == s.operations.end()) {
      return Status::refused(ReasonCode::RejectedResourceExhausted);
    }
    AsyncOperation& operation = *it->second;
    if (is_terminal(operation.state)) {
      return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
    }
    if (operation.state == OperationState::Running) {
      // The placement may already have been committed. Reporting success here
      // would claim a cancellation that cannot be honoured.
      return Status::refused(ReasonCode::RejectedCancelled);
    }
    operation.cancel_requested = true;
  }
  // state_mutex is taken only after operation_mutex has been released, so the
  // two locks are never held together and there is no order to invert.
  {
    std::lock_guard<std::mutex> state_lock(s.state_mutex);
    s.counters.operations_cancelled = saturating_increment(s.counters.operations_cancelled);
  }
  return Status::accepted(ReasonCode::AcceptedCancelled);
}

// ===========================================================================
// Inspection
// ===========================================================================

bool Engine::lookup_flow(FlowId flow, FlowDescriptor& out) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  const auto it = s.flows.find(flow);
  if (it == s.flows.end()) {
    return false;
  }
  out = it->second;
  return true;
}

bool Engine::lookup_target(TargetId target, TargetDescriptor& out) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  const auto it = s.targets.find(target);
  if (it == s.targets.end()) {
    return false;
  }
  out = it->second.descriptor;
  return true;
}

bool Engine::lookup_assignment(FlowId flow, Assignment& out) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  const auto it = s.assignments.find(flow);
  if (it == s.assignments.end()) {
    return false;
  }
  out = it->second;
  return true;
}

bool Engine::lookup_migration(MigrationAttemptId attempt, MigrationRecord& out) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  const auto it = s.migrations.find(attempt);
  if (it == s.migrations.end()) {
    return false;
  }
  out = it->second;
  return true;
}

bool Engine::lookup_authorization(AuthorizationId authorization, Authorization& out) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  const auto it = s.authorizations.find(authorization);
  if (it == s.authorizations.end()) {
    return false;
  }
  out = it->second;
  return true;
}

bool Engine::lookup_recommendation(RecommendationId recommendation, Recommendation& out) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  const auto it = s.recommendations.find(recommendation);
  if (it == s.recommendations.end()) {
    return false;
  }
  out = it->second;
  return true;
}

DurableState Engine::durable_state() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  return s.build_durable_locked();
}

std::vector<FenceRecord> Engine::active_fences() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  std::vector<FenceRecord> out;
  for (const auto& entry : s.fences) {
    if (entry.second.active) {
      out.push_back(entry.second);
    }
  }
  return out;
}

std::vector<ReasonCount> Engine::refusal_counts() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  std::vector<ReasonCount> out;
  out.reserve(s.refusal_counts.size());
  for (const auto& entry : s.refusal_counts) {
    out.push_back(ReasonCount{static_cast<ReasonCode>(entry.first), entry.second});
  }
  return out;
}

std::size_t Engine::target_count() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  return s.targets.size();
}

std::size_t Engine::flow_count() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  return s.flows.size();
}

EngineExport Engine::export_state() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  EngineExport out{};
  out.epoch = s.epoch;
  out.boot = s.boot;
  out.policy_generation = s.policy_generation;
  out.schedule_generation = s.schedule_generation;
  out.topology_generation = s.topology_generation;
  out.policy_present = s.policy_present;
  out.policy = s.policy;
  out.recovery = s.recovery;
  out.counters = s.counters;
  for (const auto& entry : s.targets) {
    TargetExport target{};
    target.descriptor = entry.second.descriptor;
    target.live = entry.second.declared &&
                  entry.second.descriptor.lifecycle == TargetLifecycle::Ready;
    target.has_load = entry.second.has_load;
    target.load_conflicting = entry.second.conflicting;
    target.load = entry.second.load;
    target.committed = s.committed_demand_for_locked(entry.first);
    out.targets.push_back(target);
  }
  for (const auto& entry : s.flows) {
    out.flows.push_back(entry.second);
  }
  for (const auto& entry : s.assignments) {
    out.assignments.push_back(entry.second);
  }
  for (const auto& entry : s.migrations) {
    out.migrations.push_back(entry.second);
  }
  for (const auto& entry : s.fences) {
    out.fences.push_back(entry.second);
  }
  for (const auto& entry : s.refusal_counts) {
    out.refusal_counts.push_back(ReasonCount{static_cast<ReasonCode>(entry.first), entry.second});
  }
  for (const auto& entry : s.requests) {
    out.requests.push_back(entry.second);
  }
  for (const auto& entry : s.watermarks) {
    out.watermarks.push_back(entry.second);
  }
  for (const auto& entry : s.contracts) {
    out.contracts.push_back(entry.second);
  }
  return out;
}

std::map<TargetId, CapacityVector> Engine::committed_demand() const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  std::map<TargetId, CapacityVector> out;
  for (const auto& entry : s.targets) {
    out[entry.first] = s.committed_demand_for_locked(entry.first);
  }
  return out;
}

}  // namespace flow_offload
