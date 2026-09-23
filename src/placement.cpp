// Flow Offload Scheduler - deterministic placement and bounded rebalance.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The decision procedure has no hidden inputs: no wall clock, no pointer or hash
// iteration order, no thread scheduling. Two runs that observe the same flows,
// targets, evidence, policy and evaluation instant produce byte-identical
// results, in any input order.

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "flow_offload/serialize.hpp"
#include "engine_internal.hpp"

namespace flow_offload {
namespace {

[[nodiscard]] std::uint64_t saturating_increment(std::uint64_t value) noexcept {
  return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}

[[nodiscard]] ExecutionDomain preferred_domain(const Policy& policy) noexcept {
  switch (policy.domain_preference) {
    case DomainPreference::HostFirst:
    case DomainPreference::HostOnly:
      return ExecutionDomain::Host;
    case DomainPreference::OffloadFirst:
    case DomainPreference::OffloadOnly:
      return ExecutionDomain::OffloadDevice;
    case DomainPreference::Unknown:
    default:
      return ExecutionDomain::Unknown;
  }
}

[[nodiscard]] bool fallback_allowed(const Policy& policy, ExecutionDomain preferred,
                                    ExecutionDomain chosen) noexcept {
  if (preferred == ExecutionDomain::Unknown || chosen == preferred) {
    return true;
  }
  if (chosen == ExecutionDomain::Host && preferred == ExecutionDomain::OffloadDevice) {
    return policy.fallback == FallbackPolicy::AllowHostFallback ||
           policy.fallback == FallbackPolicy::AllowAnyFallback;
  }
  if (chosen == ExecutionDomain::OffloadDevice && preferred == ExecutionDomain::Host) {
    return policy.fallback == FallbackPolicy::AllowOffloadFallback ||
           policy.fallback == FallbackPolicy::AllowAnyFallback;
  }
  return false;
}

[[nodiscard]] ReasonCode domain_outcome(ExecutionDomain domain, bool fallback) noexcept {
  if (fallback) {
    return domain == ExecutionDomain::Host ? ReasonCode::AcceptedFallbackToHost
                                           : ReasonCode::AcceptedFallbackToOffload;
  }
  return domain == ExecutionDomain::Host ? ReasonCode::AcceptedHostPlacement
                                         : ReasonCode::AcceptedOffloadPlacement;
}

struct RankedCandidate {
  RankKey key{};
  CandidateView view{};
};

[[nodiscard]] ReasonCode key_for(const CandidateView& view, bool sticky, ExecutionDomain preferred,
                                 RankKey& out) noexcept {
  out = RankKey{};
  out.sticky = sticky ? 0U : 1U;
  out.domain_rank = (view.domain == preferred) ? 0U : 1U;
  if (view.cost_class == CostClass::Unknown) {
    return ReasonCode::RejectedPolicyCostClass;
  }
  out.cost_rank = static_cast<std::uint8_t>(view.cost_class);
  out.utilisation_ppm = view.utilisation_ppm;
  out.target = view.target.value();
  return ReasonCode::Ok;
}

void add_step(std::vector<ExplanationStep>& steps, ReasonCode code, std::uint64_t a,
              std::uint64_t b, std::string detail) {
  if (steps.size() >= kMaxExplanationSteps) {
    return;
  }
  ExplanationStep step{};
  step.code = code;
  step.value_a = a;
  step.value_b = b;
  step.detail = std::move(detail);
  steps.push_back(std::move(step));
}

void append_refusal_summary(std::vector<ReasonCount>& summary,
                            const std::map<std::uint32_t, std::uint64_t>& counts) {
  summary.clear();
  summary.reserve(counts.size());
  for (const auto& entry : counts) {
    summary.push_back(ReasonCount{static_cast<ReasonCode>(entry.first), entry.second});
  }
}

}  // namespace

Sha256Digest placement_request_digest(const PlacementRequest& request) noexcept {
  CanonicalWriter writer;
  encode(writer, request.request);
  encode(writer, request.evaluation_instant);
  encode(writer, request.policy_generation);
  encode(writer, request.base_schedule_generation);
  writer.u32(request.max_alternatives);
  writer.boolean(request.dry_run);
  std::vector<PlacementItem> items = request.items;
  std::sort(items.begin(), items.end(), [](const PlacementItem& a, const PlacementItem& b) {
    if (a.flow != b.flow) {
      return a.flow < b.flow;
    }
    return a.flow_generation < b.flow_generation;
  });
  writer.u32(static_cast<std::uint32_t>(items.size()));
  for (const PlacementItem& item : items) {
    encode(writer, item.flow);
    encode(writer, item.flow_generation);
  }
  if (!writer.ok()) {
    return Sha256Digest{};
  }
  return Sha256::of(writer.bytes());
}

Sha256Digest placement_result_digest(const PlacementResult& result) noexcept {
  CanonicalWriter writer;
  encode(writer, result.request);
  encode(writer, result.status);
  writer.boolean(result.accepted);
  writer.boolean(result.duplicate);
  writer.boolean(result.dry_run);
  encode(writer, result.schedule_generation);
  writer.u32(static_cast<std::uint32_t>(result.placements.size()));
  for (const FlowPlacement& placement : result.placements) {
    encode(writer, placement.flow);
    encode(writer, placement.flow_generation);
    encode(writer, placement.function);
    encode(writer, placement.outcome);
    writer.boolean(placement.accepted);
    writer.boolean(placement.fallback);
    writer.boolean(placement.retained);
    writer.boolean(placement.changed);
    encode(writer, placement.target);
    encode(writer, placement.incarnation);
    encode(writer, placement.capability_generation);
    encode_enum(writer, placement.domain);
    encode(writer, placement.assignment);
    encode(writer, placement.recommendation);
    writer.u32(placement.alternatives);
  }
  writer.u32(static_cast<std::uint32_t>(result.refusal_summary.size()));
  for (const ReasonCount& count : result.refusal_summary) {
    encode(writer, count.code);
    writer.u64(count.count);
  }
  if (!writer.ok()) {
    return Sha256Digest{};
  }
  return Sha256::of(writer.bytes());
}

ReasonCode Engine::Impl::evaluate_target_locked(const FlowDescriptor& flow, const TargetState& target,
                                                Timestamp instant, CandidateView& out) const {
  out = CandidateView{};
  const TargetDescriptor& descriptor = target.descriptor;
  out.target = descriptor.target;
  out.domain = descriptor.domain;
  out.host = descriptor.host;
  out.device = descriptor.device;
  out.incarnation = descriptor.incarnation;
  out.capability_generation = descriptor.capability_generation;
  out.cost_class = descriptor.cost_class;
  out.capacity = descriptor.capacity;
  out.demand = flow.demand;

  if (!target.declared) {
    // Nothing the previous incarnation persisted about this target is
    // trustworthy here, so its lifecycle is unknown rather than inherited.
    return ReasonCode::RejectedTargetLifecycleUnknown;
  }
  switch (descriptor.lifecycle) {
    case TargetLifecycle::Ready:
      break;
    case TargetLifecycle::Removed:
      return ReasonCode::RejectedTargetRemoved;
    case TargetLifecycle::Draining:
      return ReasonCode::RejectedTargetDraining;
    case TargetLifecycle::Degraded:
      return ReasonCode::RejectedTargetDegraded;
    case TargetLifecycle::Quiesced:
      return ReasonCode::RejectedTargetQuiesced;
    case TargetLifecycle::Discovered:
      return ReasonCode::RejectedTargetNotReady;
    case TargetLifecycle::Unknown:
    default:
      return ReasonCode::RejectedTargetLifecycleUnknown;
  }

  const ExecutionDomain preferred = preferred_domain(policy);
  if (policy.domain_preference == DomainPreference::HostOnly &&
      descriptor.domain != ExecutionDomain::Host) {
    return ReasonCode::RejectedPolicyForbidsDomain;
  }
  if (policy.domain_preference == DomainPreference::OffloadOnly &&
      descriptor.domain != ExecutionDomain::OffloadDevice) {
    return ReasonCode::RejectedPolicyForbidsDomain;
  }
  if (!policy.allowed_domains.empty() &&
      !std::binary_search(policy.allowed_domains.begin(), policy.allowed_domains.end(),
                          descriptor.domain)) {
    return ReasonCode::RejectedPolicyForbidsDomain;
  }
  if (std::binary_search(policy.forbidden_hosts.begin(), policy.forbidden_hosts.end(),
                         descriptor.host)) {
    return ReasonCode::RejectedPolicyForbidsHost;
  }
  if (descriptor.device.is_valid() &&
      std::binary_search(policy.forbidden_devices.begin(), policy.forbidden_devices.end(),
                         descriptor.device)) {
    return ReasonCode::RejectedPolicyForbidsDevice;
  }
  if (!policy.allowed_functions.empty() &&
      !std::binary_search(policy.allowed_functions.begin(), policy.allowed_functions.end(),
                          flow.function)) {
    return ReasonCode::RejectedPolicyFunctionNotAllowed;
  }

  const CapabilityMask required = flow.required_capabilities | policy.required_capabilities;
  if (!satisfies(descriptor.capabilities, required)) {
    return ReasonCode::RejectedUnsupportedCapability;
  }
  if (flow.statefulness == Statefulness::Stateful && !descriptor.supports_stateful) {
    return ReasonCode::RejectedUnsupportedTarget;
  }
  if (descriptor.cost_class == CostClass::Unknown || descriptor.cost_class > flow.max_cost_class ||
      descriptor.cost_class > policy.max_cost_class) {
    return ReasonCode::RejectedPolicyCostClass;
  }

  if (flow.locality != LocalityKind::None) {
    if (flow.locality_anchor == flow.flow) {
      return ReasonCode::RejectedAffinityUnsatisfiable;
    }
    const auto anchor_it = assignments.find(flow.locality_anchor);
    if (anchor_it == assignments.end()) {
      // An unplaced anchor cannot justify locality: absent evidence never
      // becomes a satisfied constraint.
      return ReasonCode::RejectedAffinityUnsatisfiable;
    }
    const auto anchor_target_it = targets.find(anchor_it->second.target);
    if (anchor_target_it == targets.end()) {
      return ReasonCode::RejectedAffinityUnsatisfiable;
    }
    if (flow.locality == LocalityKind::SameHostAsFlow &&
        anchor_target_it->second.descriptor.host != descriptor.host) {
      return ReasonCode::RejectedAffinityUnsatisfiable;
    }
    if (flow.locality == LocalityKind::SameDeviceAsFlow &&
        anchor_target_it->second.descriptor.device != descriptor.device) {
      return ReasonCode::RejectedAffinityUnsatisfiable;
    }
  }

  out.load_evidence_present = target.has_load && !target.conflicting;
  out.load_evidence_fresh = false;
  EffectiveLoad load = effective_load_locked(target, instant);
  if (load.known && out.load_evidence_present && config.evidence_freshness.nanos() > 0U) {
    out.load_evidence_fresh =
        saturating_elapsed(instant, target.load.observed_at).nanos() <=
        config.evidence_freshness.nanos();
  }
  if (!load.known) {
    if (!policy.allow_placement_without_load_evidence) {
      return load.reason;
    }
    // Explicit, auditable opt-in. The decision is recorded as
    // AcceptedWithoutLoadEvidence so the missing evidence stays visible.
    load.known = true;
    load.used = CapacityVector::zero();
  }
  out.effective_used = load.used;
  if (load.used.exceeds_any(descriptor.capacity)) {
    out.available = CapacityVector::zero();
    return ReasonCode::RejectedCapacityOverSubscribed;
  }
  const CapacityVector after_reserve = saturating_sub(descriptor.capacity, descriptor.reserved);
  out.available = saturating_sub(after_reserve, load.used);
  CapacityVector required_total{};
  if (!checked_add(flow.demand, policy.reserve, required_total)) {
    return ReasonCode::RejectedCapacityOverflow;
  }
  if (!out.available.dominates(required_total)) {
    return ReasonCode::RejectedCapacityInsufficient;
  }
  CapacityVector after{};
  if (!checked_add(load.used, flow.demand, after)) {
    return ReasonCode::RejectedCapacityOverflow;
  }
  std::uint32_t ppm = 0;
  bool saturated = false;
  if (!aggregate_utilisation_ppm(after, descriptor.capacity, ppm, saturated)) {
    return ReasonCode::RejectedCapacityUnknown;
  }
  out.utilisation_ppm = ppm;
  out.utilisation_saturated = saturated;
  out.fallback_domain = preferred != ExecutionDomain::Unknown && descriptor.domain != preferred;
  (void)instant;
  return ReasonCode::Ok;
}

PlacementResult place_locked(Engine::Impl& s, const PlacementRequest& request) {
  PlacementResult result{};
  result.request = request.request;
  result.dry_run = request.dry_run;

  if (!s.open) {
    result.status = ReasonCode::RejectedShuttingDown;
    return result;
  }
  if (!request.request.is_valid()) {
    result.status = ReasonCode::RejectedMalformedInput;
    s.record_refusal_locked(result.status);
    return result;
  }
  if (request.items.empty()) {
    result.status = ReasonCode::RejectedEmptyRequest;
    s.record_refusal_locked(result.status);
    return result;
  }
  if (request.items.size() > s.config.max_flows) {
    result.status = ReasonCode::RejectedOversizedInput;
    s.record_refusal_locked(result.status);
    return result;
  }
  if (!request.evaluation_instant.is_set()) {
    result.status = ReasonCode::RejectedMissingEvidence;
    s.record_refusal_locked(result.status);
    return result;
  }
  if (!s.policy_present) {
    result.status = ReasonCode::RejectedMissingEvidence;
    s.record_refusal_locked(result.status);
    return result;
  }
  if (request.policy_generation.is_valid() && request.policy_generation != s.policy_generation) {
    result.status = ReasonCode::RejectedStalePolicyGeneration;
    s.record_refusal_locked(result.status);
    return result;
  }
  if (request.base_schedule_generation.is_valid() &&
      request.base_schedule_generation != s.schedule_generation) {
    result.status = ReasonCode::RejectedStaleScheduleGeneration;
    s.record_refusal_locked(result.status);
    return result;
  }

  const Sha256Digest request_digest = placement_request_digest(request);
  if (request_digest.is_zero()) {
    result.status = ReasonCode::RejectedOversizedInput;
    s.record_refusal_locked(result.status);
    return result;
  }
  const auto existing = s.requests.find(request.request);
  if (existing != s.requests.end()) {
    if (existing->second.request_digest != request_digest) {
      result.status = ReasonCode::RejectedConflictingDuplicate;
      s.record_refusal_locked(result.status);
      return result;
    }
    // Idempotent duplicate. No state changes; the reported placements are
    // re-derived from the committed schedule so they always describe reality.
    s.counters.requests_deduplicated = saturating_increment(s.counters.requests_deduplicated);
    result.duplicate = true;
    result.status = ReasonCode::AcceptedIdempotentReplay;
    result.accepted = true;
    result.schedule_generation = existing->second.schedule_generation;
    for (const PlacementItem& item : request.items) {
      const auto flow_it = s.flows.find(item.flow);
      if (flow_it == s.flows.end()) {
        continue;
      }
      FlowPlacement placement{};
      placement.flow = item.flow;
      placement.flow_generation = flow_it->second.generation;
      placement.function = flow_it->second.function;
      const auto assignment_it = s.assignments.find(item.flow);
      if (assignment_it == s.assignments.end()) {
        placement.outcome = ReasonCode::RejectedUnknownFlow;
      } else {
        placement.accepted = true;
        placement.retained = true;
        placement.target = assignment_it->second.target;
        placement.incarnation = assignment_it->second.incarnation;
        placement.capability_generation = assignment_it->second.capability_generation;
        placement.assignment = assignment_it->second.assignment;
        placement.outcome = ReasonCode::AcceptedIdempotentReplay;
        const auto target_it = s.targets.find(placement.target);
        if (target_it != s.targets.end()) {
          placement.domain = target_it->second.descriptor.domain;
        }
      }
      result.placements.push_back(std::move(placement));
    }
    result.digest = placement_result_digest(result);
    return result;
  }

  // Canonical item order makes the result independent of input order.
  std::vector<PlacementItem> items;
  items.reserve(request.items.size());
  for (const PlacementItem& item : request.items) {
    items.push_back(item);
  }
  std::sort(items.begin(), items.end(), [](const PlacementItem& a, const PlacementItem& b) {
    if (a.flow != b.flow) {
      return a.flow < b.flow;
    }
    return a.flow_generation < b.flow_generation;
  });
  for (std::size_t i = 1; i < items.size(); ++i) {
    if (items[i].flow == items[i - 1].flow && items[i].flow_generation != items[i - 1].flow_generation) {
      result.status = ReasonCode::RejectedConflictingDuplicate;
      s.record_refusal_locked(result.status);
      return result;
    }
  }
  items.erase(std::unique(items.begin(), items.end(),
                          [](const PlacementItem& a, const PlacementItem& b) {
                            return a.flow == b.flow;
                          }),
              items.end());

  const std::uint32_t max_alternatives =
      request.max_alternatives == 0U
          ? static_cast<std::uint32_t>(s.config.max_alternatives)
          : std::min(request.max_alternatives, static_cast<std::uint32_t>(kMaxAlternatives));

  // Demand planned by this batch, so capacity is conserved across the batch
  // before anything is committed.
  std::map<TargetId, CapacityVector> planned_add;
  std::map<TargetId, CapacityVector> planned_release;
  std::map<std::uint32_t, std::uint64_t> batch_refusals;

  bool any_change = false;
  const ExecutionDomain preferred = preferred_domain(s.policy);

  // Decisions are computed first and committed afterwards, so every record that
  // is published carries the schedule generation that the request actually
  // produces rather than the one that preceded it.
  struct PendingCommit {
    std::size_t index{0};
    FlowId flow{};
    FlowGeneration generation{};
    TargetId target{};
    TargetIncarnation incarnation{};
    CapabilityGeneration capability{};
    ExecutionDomain domain{ExecutionDomain::Unknown};
    ReasonCode outcome{ReasonCode::Ok};
    bool fallback{false};
    bool retained{false};
  };
  std::vector<PendingCommit> commits;

  for (const PlacementItem& item : items) {
    FlowPlacement placement{};
    placement.flow = item.flow;
    const std::size_t placement_index = result.placements.size();

    const auto flow_it = s.flows.find(item.flow);
    if (flow_it == s.flows.end()) {
      placement.outcome = ReasonCode::RejectedUnknownFlow;
      ++batch_refusals[reason_value(placement.outcome)];
      s.counters.placements_refused = saturating_increment(s.counters.placements_refused);
      s.record_refusal_locked(placement.outcome);
      result.placements.push_back(std::move(placement));
      continue;
    }
    const FlowDescriptor& flow = flow_it->second;
    if (item.flow_generation.is_valid() && item.flow_generation != flow.generation) {
      placement.outcome = ReasonCode::RejectedStaleFlowGeneration;
      ++batch_refusals[reason_value(placement.outcome)];
      s.counters.placements_refused = saturating_increment(s.counters.placements_refused);
      s.record_refusal_locked(placement.outcome);
      result.placements.push_back(std::move(placement));
      continue;
    }
    placement.flow_generation = flow.generation;
    placement.function = flow.function;
    placement.explanation.flow = flow.flow;
    placement.explanation.flow_generation = flow.generation;
    placement.explanation.policy_generation = s.policy_generation;
    placement.explanation.topology_generation = s.topology_generation;
    placement.explanation.epoch = s.epoch;
    placement.explanation.boot = s.boot;
    placement.explanation.evaluation_instant = request.evaluation_instant;

    if (flow.statefulness == Statefulness::Stateful) {
      const StateHolder* holder = s.find_holder_locked(flow.exclusive_state_key);
      if (holder != nullptr &&
          (holder->flow != flow.flow || holder->flow_generation != flow.generation)) {
        // Exclusive state is live elsewhere. It is never double-assigned.
        placement.outcome = ReasonCode::RejectedExclusiveStateConflict;
        ++batch_refusals[reason_value(placement.outcome)];
        s.counters.placements_refused = saturating_increment(s.counters.placements_refused);
        s.record_refusal_locked(placement.outcome);
        add_step(placement.explanation.refused, placement.outcome, holder->flow.value(),
                 holder->flow_generation.value(), "exclusive state is held by another flow");
        placement.explanation.primary = placement.outcome;
        result.placements.push_back(std::move(placement));
        continue;
      }
    }

    const Assignment* current = nullptr;
    const auto current_it = s.assignments.find(flow.flow);
    if (current_it != s.assignments.end()) {
      current = &current_it->second;
    }

    std::vector<RankedCandidate> ranked;
    std::vector<CandidateView> refused;
    for (const auto& target_entry : s.targets) {
      TargetState local = target_entry.second;
      const auto add_it = planned_add.find(target_entry.first);
      const auto release_it = planned_release.find(target_entry.first);
      if (add_it != planned_add.end()) {
        CapacityVector sum{};
        if (checked_add(local.scheduled, add_it->second, sum)) {
          local.scheduled = sum;
        } else {
          local.scheduled = CapacityVector::saturated();
        }
      }
      if (release_it != planned_release.end()) {
        local.scheduled = saturating_sub(local.scheduled, release_it->second);
      }
      CandidateView view{};
      const ReasonCode verdict =
          s.evaluate_target_locked(flow, local, request.evaluation_instant, view);
      view.eligible = verdict == ReasonCode::Ok;
      view.reason = verdict;
      if (view.eligible) {
        RankKey key{};
        const bool sticky = s.policy.sticky && current != nullptr &&
                            current->target == view.target &&
                            current->incarnation == view.incarnation;
        const ReasonCode keyed = key_for(view, sticky, preferred, key);
        if (keyed != ReasonCode::Ok) {
          view.eligible = false;
          view.reason = keyed;
          if (refused.size() < kMaxRankedCandidates) {
            refused.push_back(view);
          }
          continue;
        }
        if (ranked.size() < kMaxRankedCandidates) {
          RankedCandidate entry{};
          entry.key = key;
          entry.view = view;
          ranked.push_back(entry);
        }
      } else if (refused.size() < kMaxRankedCandidates) {
        refused.push_back(view);
      }
    }

    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const RankedCandidate& a, const RankedCandidate& b) {
                       return compare_rank(a.key, b.key) < 0;
                     });
    for (std::size_t i = 0; i < ranked.size(); ++i) {
      ranked[i].view.rank = static_cast<std::uint32_t>(i);
    }

    const RankedCandidate* chosen = nullptr;
    bool fallback = false;
    if (!ranked.empty()) {
      chosen = &ranked.front();
      fallback = ranked.front().key.domain_rank != 0U;
      if (fallback && !fallback_allowed(s.policy, preferred, chosen->view.domain)) {
        chosen = nullptr;
      }
    }

    for (std::size_t i = 0; i < ranked.size() && i < max_alternatives; ++i) {
      placement.ranked.push_back(ranked[i].view);
    }
    for (const CandidateView& view : refused) {
      if (placement.refused.size() >= kMaxExplanationSteps) {
        break;
      }
      placement.refused.push_back(view);
      add_step(placement.explanation.refused, view.reason, view.target.value(), view.utilisation_ppm,
               std::string{"target refused"});
    }
    placement.alternatives = static_cast<std::uint32_t>(ranked.size());

    if (chosen == nullptr) {
      std::vector<ReasonCode> reasons;
      reasons.reserve(refused.size() + 1);
      for (const CandidateView& view : refused) {
        reasons.push_back(view.reason);
      }
      const ReasonCode dominant =
          reasons.empty() ? ReasonCode::RejectedNoEligibleTarget : s.dominant_refusal_locked(reasons);
      placement.outcome = dominant;
      placement.explanation.primary = dominant;
      ++batch_refusals[reason_value(dominant)];
      s.counters.placements_refused = saturating_increment(s.counters.placements_refused);
      s.record_refusal_locked(dominant);
      if (reasons.empty()) {
        add_step(placement.explanation.refused, dominant, 0, 0,
                 "no execution target is declared for this coordinator incarnation");
      }
      result.placements.push_back(std::move(placement));
      continue;
    }

    const bool retained = s.policy.sticky && current != nullptr &&
                          current->target == chosen->view.target &&
                          current->incarnation == chosen->view.incarnation;
    ReasonCode outcome = ReasonCode::Ok;
    if (retained) {
      outcome = ReasonCode::AcceptedStickyRetention;
    } else {
      const bool load_missing = !chosen->view.load_evidence_fresh;
      if (fallback) {
        outcome = domain_outcome(chosen->view.domain, true);
      } else if (load_missing && s.policy.allow_placement_without_load_evidence) {
        outcome = ReasonCode::AcceptedWithoutLoadEvidence;
      } else {
        outcome = domain_outcome(chosen->view.domain, false);
      }
    }

    placement.accepted = true;
    placement.fallback = fallback;
    placement.retained = retained;
    placement.outcome = outcome;
    placement.target = chosen->view.target;
    placement.incarnation = chosen->view.incarnation;
    placement.capability_generation = chosen->view.capability_generation;
    placement.domain = chosen->view.domain;
    placement.changed = !retained;
    placement.explanation.primary = outcome;
    placement.explanation.target = chosen->view.target;
    placement.explanation.incarnation = chosen->view.incarnation;
    add_step(placement.explanation.accepted, ReasonCode::AcceptedHostPlacement,
             chosen->view.target.value(), chosen->view.rank,
             std::string{"selected target "} + to_string(chosen->view.target) + " rank " +
                 std::to_string(chosen->view.rank));
    add_step(placement.explanation.accepted, ReasonCode::AcceptedEvidenceRecorded,
             chosen->view.utilisation_ppm,
             chosen->view.load_evidence_fresh ? 1U : 0U,
             chosen->view.load_evidence_fresh ? std::string{"fresh load evidence"}
                                              : std::string{"no fresh load evidence"});
    add_step(placement.explanation.accepted, ReasonCode::AcceptedNoChangeRequired,
             chosen->view.available.packets_per_second, chosen->view.demand.packets_per_second,
             std::string{"capacity available vs required (packets per second)"});

    if (retained) {
      placement.assignment = current->assignment;
      s.counters.placements_retained = saturating_increment(s.counters.placements_retained);
    } else {
      if (current != nullptr) {
        const auto old_flow_it = s.flows.find(flow.flow);
        if (old_flow_it != s.flows.end()) {
          const auto release_it = planned_release.find(current->target);
          CapacityVector sum{};
          if (release_it != planned_release.end()) {
            if (!checked_add(release_it->second, old_flow_it->second.demand, sum)) {
              sum = CapacityVector::saturated();
            }
          } else {
            sum = old_flow_it->second.demand;
          }
          planned_release[current->target] = sum;
        }
      }
      const auto add_it = planned_add.find(chosen->view.target);
      CapacityVector sum{};
      if (add_it != planned_add.end()) {
        if (!checked_add(add_it->second, flow.demand, sum)) {
          sum = CapacityVector::saturated();
        }
      } else {
        sum = flow.demand;
      }
      planned_add[chosen->view.target] = sum;
      any_change = true;
    }

    PendingCommit pending{};
    pending.index = placement_index;
    pending.flow = flow.flow;
    pending.generation = flow.generation;
    pending.target = chosen->view.target;
    pending.incarnation = chosen->view.incarnation;
    pending.capability = chosen->view.capability_generation;
    pending.domain = chosen->view.domain;
    pending.outcome = outcome;
    pending.fallback = fallback;
    pending.retained = retained;
    commits.push_back(pending);

    s.counters.placements_accepted = saturating_increment(s.counters.placements_accepted);
    if (fallback) {
      s.counters.placements_fallback = saturating_increment(s.counters.placements_fallback);
    }
    result.placements.push_back(std::move(placement));
  }

  ScheduleGeneration published = s.schedule_generation;
  if (any_change && !request.dry_run) {
    published = ScheduleGeneration{s.schedule_generation.value() + 1U};
    if (!published.is_valid()) {
      // A generation counter that wrapped would allow a stale generation to
      // compare equal to a fresh one, so it is refused rather than reused.
      result.status = ReasonCode::RejectedInternalInvariant;
      s.record_refusal_locked(result.status);
      return result;
    }
    s.schedule_generation = published;
    s.recompute_scheduled_locked();
  }

  if (!request.dry_run) {
    for (const PendingCommit& pending : commits) {
      Recommendation recommendation{};
      recommendation.id = RecommendationId{s.next_recommendation};
      s.next_recommendation = saturating_increment(s.next_recommendation);
      recommendation.request = request.request;
      recommendation.flow = pending.flow;
      recommendation.flow_generation = pending.generation;
      recommendation.target = pending.target;
      recommendation.incarnation = pending.incarnation;
      recommendation.capability_generation = pending.capability;
      recommendation.policy_generation = s.policy_generation;
      recommendation.schedule_generation = published;
      recommendation.topology_generation = s.topology_generation;
      recommendation.epoch = s.epoch;
      recommendation.boot = s.boot;
      recommendation.domain = pending.domain;
      recommendation.outcome = pending.outcome;
      recommendation.fallback = pending.fallback;
      recommendation.issued_at = request.evaluation_instant;
      recommendation.digest = digest_of(recommendation);

      AssignmentId assignment_id{};
      if (!pending.retained) {
        Assignment assignment{};
        assignment.assignment = AssignmentId{s.next_assignment};
        s.next_assignment = saturating_increment(s.next_assignment);
        assignment.flow = pending.flow;
        assignment.flow_generation = pending.generation;
        assignment.target = pending.target;
        assignment.incarnation = pending.incarnation;
        assignment.capability_generation = pending.capability;
        assignment.policy_generation = s.policy_generation;
        assignment.schedule_generation = published;
        assignment.effect = EffectState::Unknown;
        assignment.reason = pending.outcome;
        assignment.committed_at = request.evaluation_instant;
        const auto previous_it = s.assignments.find(pending.flow);
        if (previous_it != s.assignments.end()) {
          assignment.supersedes = previous_it->second.assignment;
        }
        s.assignments[pending.flow] = assignment;
        assignment_id = assignment.assignment;
        const auto flow_it = s.flows.find(pending.flow);
        if (flow_it != s.flows.end() && flow_it->second.statefulness == Statefulness::Stateful) {
          StateHolder& holder = s.holders[flow_it->second.exclusive_state_key];
          holder.flow = pending.flow;
          holder.flow_generation = pending.generation;
          holder.target = assignment.target;
          holder.incarnation = assignment.incarnation;
          holder.assignment = assignment.assignment;
          holder.handoff_pending = false;
          holder.pending_attempt = MigrationAttemptId{};
        }
      } else {
        const auto existing_it = s.assignments.find(pending.flow);
        if (existing_it != s.assignments.end()) {
          assignment_id = existing_it->second.assignment;
        }
      }
      s.recommendations.emplace(recommendation.id, recommendation);
      result.placements[pending.index].recommendation = recommendation.id;
      result.placements[pending.index].assignment = assignment_id;
      s.counters.recommendations_issued = saturating_increment(s.counters.recommendations_issued);
    }
  }
  result.schedule_generation = published;
  result.accepted = true;
  if (any_change && !request.dry_run) {
    result.status = ReasonCode::AcceptedSchedulePublished;
  } else if (result.duplicate) {
    result.status = ReasonCode::AcceptedIdempotentReplay;
  } else {
    result.status = ReasonCode::AcceptedNoChangeRequired;
  }

  for (const auto& entry : batch_refusals) {
    result.refusal_summary.push_back(
        ReasonCount{static_cast<ReasonCode>(entry.first), entry.second});
  }
  result.digest = placement_result_digest(result);

  if (!request.dry_run) {
    RequestRecord record{};
    record.request = request.request;
    record.request_digest = request_digest;
    record.result_digest = result.digest;
    record.schedule_generation = result.schedule_generation;
    record.outcome = result.status;
    s.requests[request.request] = record;
    s.evict_history_locked();
    (void)s.persist_locked();
  }
  return result;
}

RebalanceReport rebalance_locked(Engine::Impl& s, const RebalanceRequest& request) {
  RebalanceReport report{};
  report.request = request.request;
  report.dry_run = request.dry_run;
  if (!s.open) {
    report.status = ReasonCode::RejectedShuttingDown;
    return report;
  }
  if (!request.evaluation_instant.is_set()) {
    report.status = ReasonCode::RejectedMissingEvidence;
    return report;
  }
  if (!s.policy_present) {
    report.status = ReasonCode::RejectedMissingEvidence;
    return report;
  }

  struct Proposal {
    FlowId flow{};
    FlowGeneration generation{};
    std::uint32_t improvement{0};
    TargetId target{};
    TargetIncarnation incarnation{};
    CapabilityGeneration capability{};
    ExecutionDomain domain{ExecutionDomain::Unknown};
  };
  std::vector<Proposal> proposals;
  std::map<std::uint32_t, std::uint64_t> refusals;
  const ExecutionDomain preferred = preferred_domain(s.policy);

  for (const auto& assignment_entry : s.assignments) {
    ++report.considered;
    const auto flow_it = s.flows.find(assignment_entry.first);
    if (flow_it == s.flows.end()) {
      continue;
    }
    const FlowDescriptor& flow = flow_it->second;
    const Assignment& assignment = assignment_entry.second;
    if (flow.pinned) {
      ++refusals[reason_value(ReasonCode::RejectedMigrationNotPermitted)];
      continue;
    }
    if (!flow.allow_migration && !s.policy.allow_migration) {
      ++refusals[reason_value(ReasonCode::RejectedMigrationNotPermitted)];
      continue;
    }
    const auto current_it = s.targets.find(assignment.target);
    if (current_it == s.targets.end()) {
      ++refusals[reason_value(ReasonCode::RejectedUnknownTarget)];
      continue;
    }
    CapacityVector current_used{};
    {
      TargetState local = current_it->second;
      const EffectiveLoad load = s.effective_load_locked(local, request.evaluation_instant);
      if (!load.known) {
        ++refusals[reason_value(load.reason)];
        continue;
      }
      current_used = load.used;
    }
    std::uint32_t current_ppm = 0;
    bool current_saturated = false;
    if (!aggregate_utilisation_ppm(current_used, current_it->second.descriptor.capacity, current_ppm,
                                   current_saturated)) {
      ++refusals[reason_value(ReasonCode::RejectedCapacityUnknown)];
      continue;
    }

    std::vector<RankedCandidate> ranked;
    for (const auto& target_entry : s.targets) {
      if (target_entry.first == assignment.target) {
        continue;
      }
      TargetState local = target_entry.second;
      CandidateView view{};
      const ReasonCode verdict =
          s.evaluate_target_locked(flow, local, request.evaluation_instant, view);
      if (verdict != ReasonCode::Ok) {
        ++refusals[reason_value(verdict)];
        continue;
      }
      RankKey key{};
      const ReasonCode keyed = key_for(view, false, preferred, key);
      if (keyed != ReasonCode::Ok) {
        ++refusals[reason_value(keyed)];
        continue;
      }
      RankedCandidate entry{};
      entry.key = key;
      entry.view = view;
      ranked.push_back(entry);
    }
    if (ranked.empty()) {
      continue;
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const RankedCandidate& a, const RankedCandidate& b) {
                       return compare_rank(a.key, b.key) < 0;
                     });
    const RankedCandidate& best = ranked.front();
    if (best.key.domain_rank != 0U && !fallback_allowed(s.policy, preferred, best.view.domain)) {
      ++refusals[reason_value(ReasonCode::RejectedPolicyForbidsDomain)];
      continue;
    }
    if (best.view.utilisation_ppm >= current_ppm) {
      continue;
    }
    const std::uint32_t improvement = current_ppm - best.view.utilisation_ppm;
    if (improvement < s.policy.min_improvement_ppm) {
      continue;
    }
    Proposal proposal{};
    proposal.flow = flow.flow;
    proposal.generation = flow.generation;
    proposal.improvement = improvement;
    proposal.target = best.view.target;
    proposal.incarnation = best.view.incarnation;
    proposal.capability = best.view.capability_generation;
    proposal.domain = best.view.domain;
    proposals.push_back(proposal);
  }

  report.eligible = static_cast<std::uint32_t>(proposals.size());
  std::stable_sort(proposals.begin(), proposals.end(),
                   [](const Proposal& a, const Proposal& b) {
                     if (a.improvement != b.improvement) {
                       return a.improvement > b.improvement;
                     }
                     return a.flow < b.flow;
                   });

  std::size_t limit = request.max_migrations == 0U
                          ? s.policy.max_rebalance_migrations
                          : std::min<std::size_t>(request.max_migrations, s.policy.max_rebalance_migrations);
  limit = std::min<std::size_t>(limit, s.config.max_rebalance_migrations);
  const std::size_t in_flight = s.migrations_in_flight_locked();
  const std::size_t capacity =
      s.config.max_migrations > in_flight ? s.config.max_migrations - in_flight : 0U;
  limit = std::min(limit, capacity);

  const std::size_t selected = std::min(limit, proposals.size());
  for (std::size_t i = 0; i < selected; ++i) {
    MigrationIntent intent{};
    intent.flow = proposals[i].flow;
    intent.flow_generation = proposals[i].generation;
    intent.destination_target = proposals[i].target;
    intent.destination_incarnation = proposals[i].incarnation;
    intent.destination_capability_generation = proposals[i].capability;
    intent.policy_generation = s.policy_generation;
    intent.schedule_generation = s.schedule_generation;
    intent.epoch = s.epoch;
    intent.boot = s.boot;
    intent.phase = MigrationPhase::Planned;
    intent.issued_at = request.evaluation_instant;
    const auto assignment_it = s.assignments.find(proposals[i].flow);
    if (assignment_it != s.assignments.end()) {
      intent.source_assignment = assignment_it->second.assignment;
      intent.source_target = assignment_it->second.target;
      intent.source_incarnation = assignment_it->second.incarnation;
      intent.source_capability_generation = assignment_it->second.capability_generation;
    }
    report.intents.push_back(intent);
  }
  report.selected = static_cast<std::uint32_t>(selected);
  report.truncated = report.eligible - report.selected;
  report.schedule_generation = s.schedule_generation;
  report.status = selected > 0U ? ReasonCode::AcceptedRebalanceBounded
                                : ReasonCode::AcceptedNoChangeRequired;
  for (const auto& entry : refusals) {
    report.refusal_summary.push_back(ReasonCount{static_cast<ReasonCode>(entry.first), entry.second});
  }
  add_step(report.explanation.accepted, ReasonCode::AcceptedRebalanceBounded, report.considered,
           report.selected, "considered/selected");
  add_step(report.explanation.refused, ReasonCode::RejectedMigrationLimitReached, report.truncated,
           limit, "truncated/limit");
  report.explanation.primary = report.status;
  report.explanation.policy_generation = s.policy_generation;
  report.explanation.schedule_generation = s.schedule_generation;
  report.explanation.topology_generation = s.topology_generation;
  report.explanation.epoch = s.epoch;
  report.explanation.boot = s.boot;
  report.explanation.evaluation_instant = request.evaluation_instant;
  s.counters.rebalances_executed = saturating_increment(s.counters.rebalances_executed);
  s.counters.rebalances_selected =
      saturating_add(s.counters.rebalances_selected, static_cast<std::uint64_t>(report.selected));
  if (report.truncated > 0U) {
    s.counters.rebalances_truncated =
        saturating_add(s.counters.rebalances_truncated, static_cast<std::uint64_t>(report.truncated));
  }
  if (!request.dry_run) {
    (void)s.persist_locked();
  }
  return report;
}

PlacementResult Engine::place(const PlacementRequest& request) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  return place_locked(s, request);
}

RebalanceReport Engine::rebalance(const RebalanceRequest& request) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  return rebalance_locked(s, request);
}

Status Engine::explain_flow(FlowId flow, Explanation& out) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.state_mutex);
  const auto flow_it = s.flows.find(flow);
  if (flow_it == s.flows.end()) {
    return Status::refused(ReasonCode::RejectedUnknownFlow);
  }
  Explanation explanation{};
  explanation.flow = flow;
  explanation.flow_generation = flow_it->second.generation;
  explanation.policy_generation = s.policy_generation;
  explanation.schedule_generation = s.schedule_generation;
  explanation.topology_generation = s.topology_generation;
  explanation.epoch = s.epoch;
  explanation.boot = s.boot;
  const auto assignment_it = s.assignments.find(flow);
  if (assignment_it == s.assignments.end()) {
    explanation.primary = ReasonCode::RejectedNoEligibleTarget;
    add_step(explanation.refused, ReasonCode::RejectedNoEligibleTarget, 0, 0,
             "the flow has no committed placement");
    out = explanation;
    return Status::accepted(ReasonCode::RejectedNoEligibleTarget);
  }
  const Assignment& assignment = assignment_it->second;
  explanation.target = assignment.target;
  explanation.incarnation = assignment.incarnation;
  explanation.primary = assignment.reason;
  add_step(explanation.accepted, assignment.reason, assignment.target.value(),
           assignment.incarnation.value(), "committed placement");
  add_step(explanation.accepted, ReasonCode::AcceptedEvidenceRecorded,
           assignment.capability_generation.value(), assignment.flow_generation.value(),
           "capability and flow generation of record");
  const auto target_it = s.targets.find(assignment.target);
  if (target_it == s.targets.end()) {
    add_step(explanation.refused, ReasonCode::RejectedUnknownTarget, assignment.target.value(), 0,
             "the committed target is no longer declared");
  } else {
    add_step(explanation.accepted, ReasonCode::AcceptedTargetRegistered,
             target_it->second.descriptor.capability_generation.value(),
             target_it->second.declared ? 1U : 0U,
             "declared capability generation and declaration state");
    if (!target_it->second.declared) {
      add_step(explanation.refused, ReasonCode::RejectedTargetLifecycleUnknown,
               assignment.target.value(), 0,
               "the target is not declared in this coordinator incarnation");
    }
  }
  add_step(explanation.accepted, ReasonCode::AcceptedAppliedEffectRecorded,
           static_cast<std::uint64_t>(assignment.effect), 0,
           std::string{"effect state "} + std::string{to_string(assignment.effect)});
  if (assignment.effect != EffectState::Verified) {
    add_step(explanation.refused, ReasonCode::RejectedMissingEvidence, 0, 0,
             "no verified effect observation has been recorded for this placement");
  }
  out = explanation;
  return Status::accepted(assignment.reason);
}

}  // namespace flow_offload
