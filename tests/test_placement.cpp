// Flow Offload Scheduler - eligibility, ranking, capacity and policy tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "flow_offload/engine.hpp"
#include "flow_offload/export.hpp"
#include "harness.hpp"

using namespace flow_offload;

namespace {

constexpr std::uint64_t kFunction = 1;

Policy base_policy(std::uint64_t generation = 1) {
  Policy policy{};
  policy.generation = PolicyGeneration{generation};
  policy.domain_preference = DomainPreference::OffloadFirst;
  policy.fallback = FallbackPolicy::AllowHostFallback;
  policy.load_model = LoadModel::SumConservative;
  policy.max_cost_class = CostClass::Standard;
  policy.sticky = true;
  policy.max_rebalance_migrations = 8;
  canonicalize(policy);
  return policy;
}

TargetDescriptor make_target(std::uint64_t id, ExecutionDomain domain, CapacityVector capacity,
                             CostClass cost = CostClass::Standard,
                             TargetLifecycle lifecycle = TargetLifecycle::Ready) {
  TargetDescriptor target{};
  target.target = TargetId{id};
  target.incarnation = TargetIncarnation{1};
  target.capability_generation = CapabilityGeneration{1};
  target.topology_generation = TopologyGeneration{1};
  target.domain = domain;
  target.kind = domain == ExecutionDomain::Host ? TargetKind::HostKernelPath : TargetKind::SmartNic;
  target.host = HostId{1};
  target.device = domain == ExecutionDomain::Host ? DeviceId{0} : DeviceId{id};
  target.capabilities = capability_bit(Capability::ConnectionTracking) |
                        capability_bit(Capability::StatefulAcl);
  target.capacity = capacity;
  target.cost_class = cost;
  target.lifecycle = lifecycle;
  target.supports_stateful = true;
  target.supports_migration = true;
  target.source = EvidenceSourceId{1};
  target.source_sequence = EvidenceSequence{id};
  target.issued_at = Timestamp{100};
  return target;
}

FlowDescriptor make_flow(std::uint64_t id, CapacityVector demand,
                         Statefulness statefulness = Statefulness::Stateless) {
  FlowDescriptor flow{};
  flow.flow = FlowId{id};
  flow.generation = FlowGeneration{1};
  flow.function = ProcessingFunctionId{kFunction};
  flow.statefulness = statefulness;
  flow.exclusive_state_key = statefulness == Statefulness::Stateful ? ExclusiveStateKeyId{id + 1000}
                                                                   : ExclusiveStateKeyId{0};
  flow.required_capabilities = capability_bit(Capability::ConnectionTracking);
  flow.max_cost_class = CostClass::Standard;
  flow.demand = demand;
  flow.allow_migration = true;
  return flow;
}

LoadEvidence make_load(std::uint64_t target_id, CapacityVector utilized, std::uint64_t sequence,
                       std::uint64_t observed_at = 1000) {
  LoadEvidence evidence{};
  evidence.target = TargetId{target_id};
  evidence.incarnation = TargetIncarnation{1};
  evidence.capability_generation = CapabilityGeneration{1};
  // One source per target, so that sequences from different targets cannot be
  // mistaken for a replay of each other.
  evidence.source = EvidenceSourceId{1000 + target_id};
  evidence.source_sequence = EvidenceSequence{sequence};
  evidence.observed_at = Timestamp{observed_at};
  evidence.state = EvidenceState::Known;
  evidence.utilized = utilized;
  return evidence;
}

PlacementRequest one_flow(std::uint64_t request, std::uint64_t flow, std::uint64_t instant = 2000) {
  PlacementRequest placement{};
  placement.request = RequestId{request};
  placement.evaluation_instant = Timestamp{instant};
  placement.items.push_back(PlacementItem{FlowId{flow}, FlowGeneration{}});
  return placement;
}

}  // namespace

FOS_TEST(placement_prefers_offload_and_records_capacity) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::Host,
                                                CapacityVector{1000000, 1000000, 1000000, 1000},
                                                CostClass::Economy))
                  .ok());
  FOS_REQUIRE(engine.declare_target(make_target(2, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000000, 1000000, 1000000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{1000, 1000, 0, 10})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{1000, 1000, 0, 1}, 1), Timestamp{1000}).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(2, CapacityVector{1000, 1000, 0, 1}, 2), Timestamp{1000}).ok());

  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::AcceptedOffloadPlacement);
  FOS_CHECK_EQ(result.placements[0].target, TargetId{2});
  FOS_CHECK(result.placements[0].accepted);
  FOS_CHECK(!result.placements[0].fallback);
  FOS_CHECK_EQ(engine.snapshot().counters.placements_accepted, static_cast<std::uint64_t>(1));

  const std::map<TargetId, CapacityVector> committed = engine.committed_demand();
  FOS_CHECK_EQ(committed.at(TargetId{2}).packets_per_second, static_cast<std::uint64_t>(1000));
  FOS_CHECK_EQ(committed.at(TargetId{1}).packets_per_second, static_cast<std::uint64_t>(0));
}

FOS_TEST(placement_refuses_without_load_evidence_and_never_assumes_zero) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000000, 1000000, 1000000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{1000, 1000, 0, 10})).ok());
  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK(!result.placements[0].accepted);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::RejectedMissingEvidence);
  FOS_CHECK_EQ(engine.snapshot().counters.placements_refused, static_cast<std::uint64_t>(1));
  Assignment assignment{};
  FOS_CHECK(!engine.lookup_assignment(FlowId{1}, assignment));
}

FOS_TEST(placement_accepts_without_load_evidence_only_with_explicit_opt_in) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  Policy policy = base_policy();
  policy.allow_placement_without_load_evidence = true;
  FOS_REQUIRE(engine.set_policy(policy).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000000, 1000000, 1000000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{1000, 1000, 0, 10})).ok());
  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK(result.placements[0].accepted);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::AcceptedWithoutLoadEvidence);
}

FOS_TEST(placement_falls_back_to_host_only_when_policy_permits) {
  Engine host_only_policy_engine;
  FOS_REQUIRE(host_only_policy_engine.open().ok());
  Policy forbid = base_policy();
  forbid.fallback = FallbackPolicy::Forbid;
  FOS_REQUIRE(host_only_policy_engine.set_policy(forbid).ok());
  FOS_REQUIRE(host_only_policy_engine
                  .declare_target(make_target(1, ExecutionDomain::Host,
                                              CapacityVector{1000000, 1000000, 1000000, 1000},
                                              CostClass::Economy))
                  .ok());
  FOS_REQUIRE(host_only_policy_engine.register_flow(make_flow(1, CapacityVector{10, 10, 0, 1})).ok());
  FOS_REQUIRE(host_only_policy_engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult refused = host_only_policy_engine.place(one_flow(1, 1));
  FOS_REQUIRE(refused.placements.size() == 1U);
  FOS_CHECK(!refused.placements[0].accepted);
  FOS_CHECK_EQ(refused.placements[0].outcome, ReasonCode::RejectedNoEligibleTarget);

  Engine fallback_engine;
  FOS_REQUIRE(fallback_engine.open().ok());
  FOS_REQUIRE(fallback_engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(fallback_engine
                  .declare_target(make_target(1, ExecutionDomain::Host,
                                              CapacityVector{1000000, 1000000, 1000000, 1000},
                                              CostClass::Economy))
                  .ok());
  FOS_REQUIRE(fallback_engine.register_flow(make_flow(1, CapacityVector{10, 10, 0, 1})).ok());
  FOS_REQUIRE(fallback_engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult allowed = fallback_engine.place(one_flow(1, 1));
  FOS_REQUIRE(allowed.placements.size() == 1U);
  FOS_CHECK(allowed.placements[0].accepted);
  FOS_CHECK(allowed.placements[0].fallback);
  FOS_CHECK_EQ(allowed.placements[0].outcome, ReasonCode::AcceptedFallbackToHost);
  FOS_CHECK(allowed.placements[0].target == TargetId{1});
}

FOS_TEST(placement_refuses_unsupported_targets_and_unknown_capabilities) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  TargetDescriptor weak = make_target(1, ExecutionDomain::OffloadDevice,
                                      CapacityVector{1000000, 1000000, 1000000, 1000});
  weak.capabilities = capability_bit(Capability::NatTranslation);
  FOS_REQUIRE(engine.declare_target(weak).ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 0, 1})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK(!result.placements[0].accepted);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::RejectedUnsupportedCapability);
  FOS_CHECK_EQ(result.placements[0].refused.size(), static_cast<std::size_t>(1));
  FOS_CHECK_EQ(result.placements[0].refused[0].reason, ReasonCode::RejectedUnsupportedCapability);
}

FOS_TEST(placement_refuses_targets_that_are_not_ready) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine
                  .declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                              CapacityVector{1000000, 1000000, 1000000, 1000},
                                              CostClass::Standard, TargetLifecycle::Draining))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 0, 1})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::RejectedTargetDraining);
}

FOS_TEST(placement_respects_capacity_and_safety_reserve) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  Policy policy = base_policy();
  policy.reserve = CapacityVector{100, 0, 0, 0};
  FOS_REQUIRE(engine.set_policy(policy).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000000, 1000000, 100}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{950, 0, 0, 0})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::RejectedCapacityInsufficient);

  Engine second;
  FOS_REQUIRE(second.open().ok());
  Policy relaxed = base_policy();
  relaxed.reserve = CapacityVector{10, 0, 0, 0};
  FOS_REQUIRE(second.set_policy(relaxed).ok());
  FOS_REQUIRE(second.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000000, 1000000, 100}))
                  .ok());
  FOS_REQUIRE(second.register_flow(make_flow(1, CapacityVector{950, 0, 0, 0})).ok());
  FOS_REQUIRE(second.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult accepted = second.place(one_flow(1, 1));
  FOS_REQUIRE(accepted.placements.size() == 1U);
  FOS_CHECK(accepted.placements[0].accepted);
}

FOS_TEST(capacity_is_conserved_across_a_batch_and_overflow_is_refused) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{2500, 1000000, 1000000, 100}))
                  .ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  for (std::uint64_t index = 1; index <= 3; ++index) {
    FOS_REQUIRE(engine.register_flow(make_flow(index, CapacityVector{1000, 0, 0, 0})).ok());
  }
  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{2000};
  for (std::uint64_t index = 1; index <= 3; ++index) {
    request.items.push_back(PlacementItem{FlowId{index}, FlowGeneration{}});
  }
  const PlacementResult result = engine.place(request);
  FOS_REQUIRE(result.placements.size() == 3U);
  std::uint64_t accepted = 0;
  std::uint64_t refused = 0;
  for (const FlowPlacement& placement : result.placements) {
    if (placement.accepted) {
      ++accepted;
    } else {
      ++refused;
      FOS_CHECK_EQ(placement.outcome, ReasonCode::RejectedCapacityInsufficient);
    }
  }
  FOS_CHECK_EQ(accepted, static_cast<std::uint64_t>(2));
  FOS_CHECK_EQ(refused, static_cast<std::uint64_t>(1));
  const std::map<TargetId, CapacityVector> committed = engine.committed_demand();
  FOS_CHECK(committed.at(TargetId{1}).packets_per_second <= 2500U);
}

FOS_TEST(ranking_is_stable_and_independent_of_target_declaration_order) {
  auto run = [](bool reverse, std::uint64_t& chosen) {
    Engine engine;
    (void)engine.open();
    (void)engine.set_policy(base_policy());
    const std::uint64_t order[3] = {1, 2, 3};
    for (std::uint64_t index = 0; index < 3; ++index) {
      const std::uint64_t id = reverse ? order[2 - index] : order[index];
      (void)engine.declare_target(make_target(id, ExecutionDomain::OffloadDevice,
                                             CapacityVector{1000000, 1000000, 1000000, 1000}));
      (void)engine.ingest_load(make_load(id, CapacityVector{0, 0, 0, 0}, id), Timestamp{1000});
    }
    (void)engine.register_flow(make_flow(1, CapacityVector{10, 10, 0, 1}));
    const PlacementResult result = engine.place(one_flow(1, 1));
    chosen = result.placements.empty() ? 0U : result.placements[0].target.value();
  };
  std::uint64_t forward = 0;
  std::uint64_t backward = 0;
  run(false, forward);
  run(true, backward);
  FOS_CHECK_EQ(forward, static_cast<std::uint64_t>(1));
  FOS_CHECK_EQ(backward, forward);
}

FOS_TEST(ranking_prefers_lower_utilisation_after_placement) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.declare_target(make_target(2, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{900, 900, 900, 900}, 1), Timestamp{1000}).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(2, CapacityVector{100, 100, 100, 100}, 2), Timestamp{1000}).ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK_EQ(result.placements[0].target, TargetId{2});
  FOS_REQUIRE(result.placements[0].ranked.size() >= 2U);
  FOS_CHECK_EQ(result.placements[0].ranked[0].target, TargetId{2});
  FOS_CHECK(result.placements[0].ranked[0].utilisation_ppm <
            result.placements[0].ranked[1].utilisation_ppm);
  FOS_CHECK_EQ(result.placements[0].ranked[0].rank, static_cast<std::uint32_t>(0));
  FOS_CHECK_EQ(result.placements[0].ranked[1].rank, static_cast<std::uint32_t>(1));
}

FOS_TEST(sticky_policy_retains_a_healthy_target) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.declare_target(make_target(2, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{900, 900, 900, 900}, 1), Timestamp{1000}).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(2, CapacityVector{100, 100, 100, 100}, 2), Timestamp{1000}).ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  const PlacementResult first = engine.place(one_flow(1, 1));
  FOS_REQUIRE(first.placements.size() == 1U);
  FOS_CHECK_EQ(first.placements[0].target, TargetId{2});
  const ScheduleGeneration after_first = engine.snapshot().schedule_generation;

  // Target 1 becomes idle, so a fresh decision would move the flow. Sticky
  // policy keeps it where it is.
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 3), Timestamp{1000}).ok());
  const PlacementResult second = engine.place(one_flow(2, 1));
  FOS_REQUIRE(second.placements.size() == 1U);
  FOS_CHECK(second.placements[0].accepted);
  FOS_CHECK(second.placements[0].retained);
  FOS_CHECK_EQ(second.placements[0].outcome, ReasonCode::AcceptedStickyRetention);
  FOS_CHECK_EQ(second.placements[0].target, TargetId{2});
  FOS_CHECK_EQ(engine.snapshot().schedule_generation, after_first);

  // With sticky off, the same inputs move the flow.
  Policy non_sticky = base_policy(2);
  non_sticky.sticky = false;
  FOS_REQUIRE(engine.set_policy(non_sticky).ok());
  const PlacementResult third = engine.place(one_flow(3, 1));
  FOS_REQUIRE(third.placements.size() == 1U);
  FOS_CHECK(third.placements[0].changed);
  FOS_CHECK_EQ(third.placements[0].target, TargetId{1});
}

FOS_TEST(policy_forbids_domains_hosts_devices_functions_and_cost) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  Policy policy = base_policy();
  policy.forbidden_hosts.push_back(HostId{1});
  canonicalize(policy);
  FOS_REQUIRE(engine.set_policy(policy).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::RejectedPolicyForbidsHost);

  Engine cost_engine;
  FOS_REQUIRE(cost_engine.open().ok());
  Policy cheap = base_policy();
  cheap.max_cost_class = CostClass::Economy;
  FOS_REQUIRE(cost_engine.set_policy(cheap).ok());
  FOS_REQUIRE(cost_engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                    CapacityVector{1000, 1000, 1000, 1000},
                                                    CostClass::Premium))
                  .ok());
  FOS_REQUIRE(cost_engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(cost_engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult cost_result = cost_engine.place(one_flow(1, 1));
  FOS_REQUIRE(cost_result.placements.size() == 1U);
  FOS_CHECK_EQ(cost_result.placements[0].outcome, ReasonCode::RejectedPolicyCostClass);

  Engine only_engine;
  FOS_REQUIRE(only_engine.open().ok());
  Policy host_only = base_policy();
  host_only.domain_preference = DomainPreference::HostOnly;
  FOS_REQUIRE(only_engine.set_policy(host_only).ok());
  FOS_REQUIRE(only_engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                    CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(only_engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(only_engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult only_result = only_engine.place(one_flow(1, 1));
  FOS_REQUIRE(only_result.placements.size() == 1U);
  FOS_CHECK_EQ(only_result.placements[0].outcome, ReasonCode::RejectedPolicyForbidsDomain);
}

FOS_TEST(locality_is_a_hard_constraint) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  TargetDescriptor second = make_target(2, ExecutionDomain::OffloadDevice,
                                        CapacityVector{1000, 1000, 1000, 1000});
  second.host = HostId{2};
  second.device = DeviceId{2};
  FOS_REQUIRE(engine.declare_target(second).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(2, CapacityVector{0, 0, 0, 0}, 2), Timestamp{1000}).ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(engine.place(one_flow(1, 1)).accepted);

  FlowDescriptor anchored = make_flow(2, CapacityVector{10, 10, 10, 10});
  anchored.locality = LocalityKind::SameHostAsFlow;
  anchored.locality_anchor = FlowId{1};
  FOS_REQUIRE(engine.register_flow(anchored).ok());
  const PlacementResult result = engine.place(one_flow(2, 2));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK(result.placements[0].accepted);
  FOS_CHECK_EQ(result.placements[0].target, TargetId{1});

  FlowDescriptor unsatisfiable = make_flow(3, CapacityVector{10, 10, 10, 10});
  unsatisfiable.locality = LocalityKind::SameHostAsFlow;
  unsatisfiable.locality_anchor = FlowId{99};
  FOS_REQUIRE(engine.register_flow(unsatisfiable).ok());
  const PlacementResult missing_anchor = engine.place(one_flow(3, 3));
  FOS_REQUIRE(missing_anchor.placements.size() == 1U);
  FOS_CHECK_EQ(missing_anchor.placements[0].outcome, ReasonCode::RejectedAffinityUnsatisfiable);
}

FOS_TEST(placement_requests_are_idempotent_and_conflicts_are_refused) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult first = engine.place(one_flow(7, 1));
  FOS_CHECK(first.accepted);
  const ScheduleGeneration generation = first.schedule_generation;
  const PlacementResult replay = engine.place(one_flow(7, 1));
  FOS_CHECK(replay.duplicate);
  FOS_CHECK_EQ(replay.status, ReasonCode::AcceptedIdempotentReplay);
  FOS_CHECK_EQ(replay.schedule_generation, generation);
  FOS_CHECK_EQ(engine.snapshot().counters.requests_deduplicated, static_cast<std::uint64_t>(1));

  PlacementRequest conflicting = one_flow(7, 1);
  conflicting.dry_run = true;
  const PlacementResult conflict = engine.place(conflicting);
  FOS_CHECK_EQ(conflict.status, ReasonCode::RejectedConflictingDuplicate);
}

FOS_TEST(malformed_placement_requests_are_refused_with_stable_codes) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  PlacementRequest empty{};
  empty.request = RequestId{1};
  empty.evaluation_instant = Timestamp{1};
  FOS_CHECK_EQ(engine.place(empty).status, ReasonCode::RejectedEmptyRequest);

  PlacementRequest no_policy = one_flow(2, 1);
  FOS_CHECK_EQ(engine.place(no_policy).status, ReasonCode::RejectedMissingEvidence);

  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  PlacementRequest no_instant{};
  no_instant.request = RequestId{3};
  no_instant.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  FOS_CHECK_EQ(engine.place(no_instant).status, ReasonCode::RejectedMissingEvidence);

  PlacementRequest bad_request = one_flow(0, 1);
  FOS_CHECK_EQ(engine.place(bad_request).status, ReasonCode::RejectedMalformedInput);

  PlacementRequest stale_policy = one_flow(4, 1);
  stale_policy.policy_generation = PolicyGeneration{9};
  FOS_CHECK_EQ(engine.place(stale_policy).status, ReasonCode::RejectedStalePolicyGeneration);

  PlacementRequest stale_schedule = one_flow(5, 1);
  stale_schedule.base_schedule_generation = ScheduleGeneration{9};
  FOS_CHECK_EQ(engine.place(stale_schedule).status, ReasonCode::RejectedStaleScheduleGeneration);

  PlacementRequest duplicate = one_flow(6, 1);
  duplicate.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{5}});
  duplicate.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{6}});
  FOS_CHECK_EQ(engine.place(duplicate).status, ReasonCode::RejectedConflictingDuplicate);

  PlacementRequest unknown_flow = one_flow(8, 4242);
  const PlacementResult unknown = engine.place(unknown_flow);
  FOS_CHECK_EQ(unknown.status, ReasonCode::AcceptedNoChangeRequired);
  FOS_REQUIRE(unknown.placements.size() == 1U);
  FOS_CHECK_EQ(unknown.placements[0].outcome, ReasonCode::RejectedUnknownFlow);

  PlacementRequest oversized = one_flow(9, 1);
  oversized.items.assign(kMaxFlows + 1U, PlacementItem{FlowId{1}, FlowGeneration{}});
  FOS_CHECK_EQ(engine.place(oversized).status, ReasonCode::RejectedOversizedInput);
}

FOS_TEST(evidence_ordering_duplicates_and_conflicts_are_detected) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  const LoadEvidence first = make_load(1, CapacityVector{10, 10, 10, 10}, 5);
  FOS_CHECK_EQ(engine.ingest_load(first, Timestamp{1000}).code(), ReasonCode::AcceptedEvidenceRecorded);
  FOS_CHECK_EQ(engine.ingest_load(first, Timestamp{1000}).code(), ReasonCode::AcceptedIdempotentReplay);

  LoadEvidence conflicting = first;
  conflicting.utilized = CapacityVector{20, 20, 20, 20};
  FOS_CHECK_EQ(engine.ingest_load(conflicting, Timestamp{1000}).code(),
               ReasonCode::RejectedConflictingDuplicate);

  LoadEvidence replayed = first;
  replayed.source_sequence = EvidenceSequence{4};
  replayed.observed_at = Timestamp{1200};
  FOS_CHECK_EQ(engine.ingest_load(replayed, Timestamp{1000}).code(),
               ReasonCode::RejectedReplayedEvidence);

  LoadEvidence older = first;
  older.source_sequence = EvidenceSequence{6};
  older.observed_at = Timestamp{900};
  older.utilized = CapacityVector{30, 30, 30, 30};
  FOS_CHECK_EQ(engine.ingest_load(older, Timestamp{1000}).code(),
               ReasonCode::RejectedOutOfOrderEvidence);

  LoadEvidence newer = first;
  newer.source_sequence = EvidenceSequence{7};
  newer.observed_at = Timestamp{1100};
  newer.utilized = CapacityVector{40, 40, 40, 40};
  FOS_CHECK_EQ(engine.ingest_load(newer, Timestamp{1100}).code(), ReasonCode::AcceptedEvidenceRecorded);

  LoadEvidence other_source = first;
  other_source.source = EvidenceSourceId{4};
  other_source.source_sequence = EvidenceSequence{1};
  other_source.observed_at = Timestamp{1100};
  other_source.utilized = CapacityVector{50, 50, 50, 50};
  FOS_CHECK_EQ(engine.ingest_load(other_source, Timestamp{1100}).code(),
               ReasonCode::AcceptedEvidenceRecorded);
  FOS_CHECK_EQ(engine.snapshot().counters.evidence_conflicts, static_cast<std::uint64_t>(1));

  // Conflicting evidence cannot justify a placement.
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  const PlacementResult result = engine.place(one_flow(1, 1, 1100));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::RejectedConflictingEvidence);

  // Evidence about an undeclared target is refused rather than inventing one.
  LoadEvidence unknown_target = make_load(77, CapacityVector{0, 0, 0, 0}, 1);
  unknown_target.source = EvidenceSourceId{5};
  FOS_CHECK_EQ(engine.ingest_load(unknown_target, Timestamp{1000}).code(),
               ReasonCode::RejectedUnknownTarget);

  // A stale capability generation can never be used.
  LoadEvidence stale = make_load(1, CapacityVector{0, 0, 0, 0}, 100);
  stale.capability_generation = CapabilityGeneration{9};
  stale.source = EvidenceSourceId{6};
  FOS_CHECK_EQ(engine.ingest_load(stale, Timestamp{1000}).code(),
               ReasonCode::RejectedStaleCapabilityGeneration);
}

FOS_TEST(stale_evidence_cannot_justify_a_current_decision) {
  EngineConfig config{};
  config.evidence_freshness = Duration{100};
  Engine engine(config);
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1, 1000), Timestamp{1000}).ok());
  FOS_CHECK(engine.place(one_flow(1, 1, 1050)).accepted);
  const PlacementResult stale = engine.place(one_flow(2, 1, 5000));
  FOS_REQUIRE(stale.placements.size() == 1U);
  FOS_CHECK_EQ(stale.placements[0].outcome, ReasonCode::RejectedStaleEvidence);
}

FOS_TEST(target_redeclaration_rejects_stale_generations_and_reregisters) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  TargetDescriptor target = make_target(1, ExecutionDomain::OffloadDevice,
                                        CapacityVector{1000, 1000, 1000, 1000});
  FOS_REQUIRE(engine.declare_target(target).ok());
  FOS_CHECK_EQ(engine.declare_target(target).code(), ReasonCode::AcceptedNoChangeRequired);

  TargetDescriptor zero_incarnation = target;
  zero_incarnation.incarnation = TargetIncarnation{0};
  FOS_CHECK_EQ(engine.declare_target(zero_incarnation).code(), ReasonCode::RejectedMalformedInput);

  TargetDescriptor stale_capability = target;
  stale_capability.capability_generation = CapabilityGeneration{0};
  FOS_CHECK_EQ(engine.declare_target(stale_capability).code(),
               ReasonCode::RejectedStaleCapabilityGeneration);

  TargetDescriptor conflicting = target;
  conflicting.capacity = CapacityVector{5, 5, 5, 5};
  FOS_CHECK_EQ(engine.declare_target(conflicting).code(), ReasonCode::RejectedConflictingDuplicate);

  TargetDescriptor newer_capability = target;
  newer_capability.capability_generation = CapabilityGeneration{2};
  newer_capability.capabilities |= capability_bit(Capability::VxlanEncap);
  FOS_CHECK_EQ(engine.declare_target(newer_capability).code(), ReasonCode::AcceptedTargetUpdated);

  TargetDescriptor newer_incarnation = target;
  newer_incarnation.incarnation = TargetIncarnation{2};
  newer_incarnation.source_sequence = EvidenceSequence{2};
  FOS_CHECK_EQ(engine.declare_target(newer_incarnation).code(), ReasonCode::AcceptedTargetUpdated);

  TargetDescriptor stale_topology = target;
  stale_topology.incarnation = TargetIncarnation{3};
  stale_topology.topology_generation = TopologyGeneration{0};
  FOS_CHECK_EQ(engine.declare_target(stale_topology).code(),
               ReasonCode::RejectedStaleTopologyGeneration);
}

FOS_TEST(a_newer_topology_generation_invalidates_older_declarations) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  FOS_CHECK(engine.place(one_flow(1, 1)).accepted);

  TargetDescriptor second = make_target(2, ExecutionDomain::OffloadDevice,
                                        CapacityVector{1000, 1000, 1000, 1000});
  second.topology_generation = TopologyGeneration{2};
  FOS_REQUIRE(engine.declare_target(second).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(2, CapacityVector{0, 0, 0, 0}, 2), Timestamp{1000}).ok());
  FOS_CHECK_EQ(engine.snapshot().topology_generation, TopologyGeneration{2});
  // Target 1 was declared under topology generation 1, so it is no longer live.
  FOS_CHECK(engine.snapshot().live_targets == 1U);
  const PlacementResult result = engine.place(one_flow(2, 1, 1100));
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK(result.placements[0].accepted);
  FOS_CHECK_EQ(result.placements[0].target, TargetId{2});
}

FOS_TEST(rebalance_is_bounded_and_accounts_for_truncation) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  Policy policy = base_policy();
  policy.allow_migration = true;
  policy.min_improvement_ppm = 0;
  policy.max_rebalance_migrations = 2;
  FOS_REQUIRE(engine.set_policy(policy).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{900, 900, 900, 900}, 1), Timestamp{1000}).ok());
  for (std::uint64_t index = 1; index <= 4; ++index) {
    FOS_REQUIRE(engine.register_flow(make_flow(index, CapacityVector{10, 10, 10, 10})).ok());
  }
  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{1000};
  for (std::uint64_t index = 1; index <= 4; ++index) {
    request.items.push_back(PlacementItem{FlowId{index}, FlowGeneration{}});
  }
  FOS_REQUIRE(engine.place(request).accepted);
  // A fresh, idle target appears. Every flow now has somewhere better to be.
  FOS_REQUIRE(engine.declare_target(make_target(2, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.ingest_load(make_load(2, CapacityVector{0, 0, 0, 0}, 2), Timestamp{1000}).ok());

  RebalanceRequest rebalance{};
  rebalance.request = RequestId{2};
  rebalance.evaluation_instant = Timestamp{1000};
  const RebalanceReport report = engine.rebalance(rebalance);
  FOS_CHECK_EQ(report.considered, static_cast<std::uint32_t>(4));
  FOS_CHECK_EQ(report.eligible, static_cast<std::uint32_t>(4));
  FOS_CHECK_EQ(report.selected, static_cast<std::uint32_t>(2));
  FOS_CHECK_EQ(report.truncated, static_cast<std::uint32_t>(2));
  FOS_CHECK_EQ(report.intents.size(), static_cast<std::size_t>(2));
  for (const MigrationIntent& intent : report.intents) {
    FOS_CHECK_EQ(intent.destination_target, TargetId{2});
    FOS_CHECK_EQ(intent.phase, MigrationPhase::Planned);
  }
  FOS_CHECK_EQ(engine.snapshot().counters.rebalances_truncated, static_cast<std::uint64_t>(2));

  // Rebalance plans only; it never issues authority by itself.
  MigrationRecord record{};
  FOS_CHECK(!engine.lookup_migration(MigrationAttemptId{1}, record));
}

FOS_TEST(explanations_expose_evidence_and_generations) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  const PlacementResult result = engine.place(one_flow(1, 1));
  FOS_REQUIRE(result.placements.size() == 1U);
  const Explanation& explanation = result.placements[0].explanation;
  FOS_CHECK_EQ(explanation.primary, ReasonCode::AcceptedOffloadPlacement);
  FOS_CHECK_EQ(explanation.policy_generation, PolicyGeneration{1});
  FOS_CHECK(!explanation.accepted.empty());
  FOS_CHECK(!render(explanation).empty());

  Explanation stored{};
  FOS_REQUIRE(engine.explain_flow(FlowId{1}, stored).ok());
  FOS_CHECK_EQ(stored.primary, ReasonCode::AcceptedOffloadPlacement);
  FOS_CHECK_EQ(stored.target, TargetId{1});
  FOS_CHECK_EQ(stored.flow_generation, FlowGeneration{1});
  Explanation missing{};
  FOS_CHECK_EQ(engine.explain_flow(FlowId{99}, missing).code(), ReasonCode::RejectedUnknownFlow);
}

FOS_TEST(export_is_canonical_and_content_addressed) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(base_policy()).ok());
  FOS_REQUIRE(engine.declare_target(make_target(1, ExecutionDomain::OffloadDevice,
                                                CapacityVector{1000, 1000, 1000, 1000}))
                  .ok());
  FOS_REQUIRE(engine.register_flow(make_flow(1, CapacityVector{10, 10, 10, 10})).ok());
  FOS_REQUIRE(engine.ingest_load(make_load(1, CapacityVector{0, 0, 0, 0}, 1), Timestamp{1000}).ok());
  FOS_REQUIRE(engine.place(one_flow(1, 1)).accepted);

  const EngineExport first = engine.export_state();
  const EngineExport second = engine.export_state();
  FOS_CHECK_EQ(export_json(first), export_json(second));
  FOS_CHECK_EQ(export_digest(first), export_digest(second));
  FOS_CHECK_EQ(export_binary(first), export_binary(second));
  FOS_CHECK(!export_json(first).empty());
  FOS_CHECK(!export_text(first).empty());
  FOS_CHECK(export_json(first).find("\"schema\"") != std::string::npos);
  FOS_CHECK(export_json(first).find("offload-device") != std::string::npos);

  // A different state produces a different digest.
  FOS_REQUIRE(engine.register_flow(make_flow(2, CapacityVector{10, 10, 10, 10})).ok());
  FOS_CHECK(export_digest(engine.export_state()) != export_digest(first));
}
