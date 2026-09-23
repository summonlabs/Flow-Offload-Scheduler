// Flow Offload Scheduler - migration authority and effect lifecycle tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "flow_offload/engine.hpp"
#include "harness.hpp"

using namespace flow_offload;

namespace {

Policy migration_policy(std::uint64_t generation = 1) {
  Policy policy{};
  policy.generation = PolicyGeneration{generation};
  policy.domain_preference = DomainPreference::OffloadFirst;
  policy.fallback = FallbackPolicy::AllowHostFallback;
  policy.load_model = LoadModel::SumConservative;
  policy.max_cost_class = CostClass::Standard;
  policy.allow_migration = true;
  policy.min_improvement_ppm = 0;
  canonicalize(policy);
  return policy;
}

TargetDescriptor target(std::uint64_t id, ExecutionDomain domain) {
  TargetDescriptor descriptor{};
  descriptor.target = TargetId{id};
  descriptor.incarnation = TargetIncarnation{1};
  descriptor.capability_generation = CapabilityGeneration{1};
  descriptor.topology_generation = TopologyGeneration{1};
  descriptor.domain = domain;
  descriptor.kind = domain == ExecutionDomain::Host ? TargetKind::HostKernelPath : TargetKind::SmartNic;
  descriptor.host = HostId{1};
  descriptor.device = domain == ExecutionDomain::Host ? DeviceId{0} : DeviceId{id};
  descriptor.capabilities = capability_bit(Capability::ConnectionTracking) |
                            capability_bit(Capability::StatefulAcl);
  descriptor.capacity = CapacityVector{1000000, 1000000, 1000000, 10000};
  descriptor.cost_class = CostClass::Standard;
  descriptor.lifecycle = TargetLifecycle::Ready;
  descriptor.supports_stateful = true;
  descriptor.supports_migration = true;
  descriptor.source = EvidenceSourceId{1};
  descriptor.source_sequence = EvidenceSequence{id};
  descriptor.issued_at = Timestamp{100};
  return descriptor;
}

FlowDescriptor flow(std::uint64_t id, Statefulness statefulness) {
  FlowDescriptor descriptor{};
  descriptor.flow = FlowId{id};
  descriptor.generation = FlowGeneration{1};
  descriptor.function = ProcessingFunctionId{1};
  descriptor.statefulness = statefulness;
  descriptor.exclusive_state_key = statefulness == Statefulness::Stateful ? ExclusiveStateKeyId{id}
                                                                         : ExclusiveStateKeyId{0};
  descriptor.required_capabilities = capability_bit(Capability::ConnectionTracking);
  descriptor.max_cost_class = CostClass::Standard;
  descriptor.demand = CapacityVector{10, 10, 1024, 10};
  descriptor.allow_migration = true;
  return descriptor;
}

LoadEvidence load(std::uint64_t id) {
  LoadEvidence evidence{};
  evidence.target = TargetId{id};
  evidence.incarnation = TargetIncarnation{1};
  evidence.capability_generation = CapabilityGeneration{1};
  evidence.source = EvidenceSourceId{2};
  evidence.source_sequence = EvidenceSequence{id};
  evidence.observed_at = Timestamp{1000};
  evidence.state = EvidenceState::Known;
  evidence.utilized = CapacityVector{0, 0, 0, 0};
  return evidence;
}

/// Drives a migration to the point where both executors have acknowledged.
struct MigrationUnderway {
  Engine* engine{nullptr};
  MigrationIntent intent{};
  AppliedEffect effect{};
};

bool establish(MigrationUnderway& underway, Engine& engine, FlowId flow_id, TargetId destination) {
  PlacementRequest placement{};
  placement.request = RequestId{1};
  placement.evaluation_instant = Timestamp{1000};
  placement.items.push_back(PlacementItem{flow_id, FlowGeneration{}});
  const PlacementResult placed = engine.place(placement);
  if (placed.placements.empty() || !placed.placements.front().accepted) {
    return false;
  }
  Authorization authorization{};
  if (engine.authorize(placed.placements.front().recommendation, Timestamp{1000}, authorization)
          .failed()) {
    return false;
  }
  MigrationIntent intent{};
  intent.flow = flow_id;
  intent.destination_target = destination;
  intent.destination_incarnation = TargetIncarnation{1};
  intent.destination_capability_generation = CapabilityGeneration{1};
  intent.issued_at = Timestamp{1000};
  if (engine.issue_intent(authorization, intent).failed()) {
    return false;
  }
  underway.engine = &engine;
  underway.intent = intent;
  return true;
}

bool acknowledge_both(MigrationUnderway& underway) {
  MigrationAcknowledgement source{};
  source.attempt = underway.intent.attempt;
  source.actor = MigrationActor::Source;
  source.target = underway.intent.source_target;
  source.incarnation = underway.intent.source_incarnation;
  source.capability_generation = underway.intent.source_capability_generation;
  source.epoch = underway.intent.epoch;
  source.boot = underway.intent.boot;
  source.accepted = true;
  source.acknowledged_at = Timestamp{1000};
  if (underway.engine->acknowledge(source).failed()) {
    return false;
  }
  MigrationAcknowledgement destination = source;
  destination.actor = MigrationActor::Destination;
  destination.target = underway.intent.destination_target;
  destination.incarnation = underway.intent.destination_incarnation;
  destination.capability_generation = underway.intent.destination_capability_generation;
  return underway.engine->acknowledge(destination).ok();
}

AppliedEffect make_effect(const MigrationIntent& intent, std::string_view tag) {
  AppliedEffect effect{};
  effect.attempt = intent.attempt;
  effect.target = intent.destination_target;
  effect.incarnation = intent.destination_incarnation;
  effect.capability_generation = intent.destination_capability_generation;
  effect.epoch = intent.epoch;
  effect.boot = intent.boot;
  effect.effect_digest = Sha256::of(tag);
  effect.state_bytes = 4096;
  effect.applied_at = Timestamp{1000};
  return effect;
}

EffectObservation make_observation(const MigrationRecord& record, const Sha256Digest& digest,
                                   EvidenceState state = EvidenceState::Known,
                                   TargetLifecycle lifecycle = TargetLifecycle::Ready) {
  EffectObservation observation{};
  observation.flow = record.intent.flow;
  observation.flow_generation = record.intent.flow_generation;
  observation.target = record.intent.destination_target;
  observation.incarnation = record.intent.destination_incarnation;
  observation.capability_generation = record.intent.destination_capability_generation;
  observation.source = EvidenceSourceId{9};
  observation.source_sequence = EvidenceSequence{1};
  observation.state = state;
  observation.lifecycle = lifecycle;
  observation.effect_digest = digest;
  observation.observed_at = Timestamp{1000};
  return observation;
}

/// A complete, safe migration contract for flow 1 to the given destination.
bool register_safe_contract(Engine& engine, MigrationContractId id, TargetId destination,
                            std::uint64_t flow_id = 1) {
  MigrationContract contract{};
  contract.id = id;
  contract.flow = FlowId{flow_id};
  contract.flow_generation = FlowGeneration{1};
  contract.statefulness = Statefulness::Stateful;
  contract.state_transfer_defined = true;
  contract.ordering_preserved = true;
  contract.rollback_defined = true;
  contract.exclusive_handoff = true;
  contract.destination_target = destination;
  contract.destination_incarnation = TargetIncarnation{1};
  contract.destination_capability_generation = CapabilityGeneration{1};
  contract.policy_generation = PolicyGeneration{1};
  return engine.register_migration_contract(contract).ok();
}

void seed_stateful(Engine& engine) {
  (void)engine.set_policy(migration_policy());
  (void)engine.declare_target(target(1, ExecutionDomain::OffloadDevice));
  (void)engine.declare_target(target(2, ExecutionDomain::OffloadDevice));
  (void)engine.ingest_load(load(1), Timestamp{1000});
  (void)engine.ingest_load(load(2), Timestamp{1000});
  (void)engine.register_flow(flow(1, Statefulness::Stateful));
}

}  // namespace

FOS_TEST(stateful_migration_requires_a_safe_contract) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  FOS_REQUIRE(register_safe_contract(engine, MigrationContractId{1}, TargetId{2}));
  MigrationUnderway underway{};
  FOS_REQUIRE(establish(underway, engine, FlowId{1}, TargetId{2}));
  FOS_CHECK_EQ(underway.intent.contract, MigrationContractId{1});
  FOS_CHECK(underway.intent.attempt.is_valid());
  FOS_CHECK(underway.intent.fence.is_valid());
  FOS_CHECK_EQ(underway.intent.phase, MigrationPhase::IntentIssued);
  FOS_CHECK(underway.intent.boot.is_valid());
  Assignment assignment{};
  FOS_REQUIRE(engine.lookup_assignment(FlowId{1}, assignment));
  FOS_CHECK_EQ(assignment.effect, EffectState::IntentIssued);
}

FOS_TEST(stateful_migration_is_refused_without_a_contract) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);

  PlacementRequest placement{};
  placement.request = RequestId{1};
  placement.evaluation_instant = Timestamp{1000};
  placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  const PlacementResult placed = engine.place(placement);
  FOS_REQUIRE(!placed.placements.empty());
  FOS_REQUIRE(placed.placements.front().accepted);
  Authorization authorization{};
  FOS_REQUIRE(engine.authorize(placed.placements.front().recommendation, Timestamp{1000}, authorization).ok());
  const ScheduleGeneration schedule_before = engine.snapshot().schedule_generation;

  MigrationIntent intent{};
  intent.flow = FlowId{1};
  intent.destination_target = TargetId{2};
  intent.destination_incarnation = TargetIncarnation{1};
  intent.destination_capability_generation = CapabilityGeneration{1};
  intent.issued_at = Timestamp{1000};
  FOS_CHECK_EQ(engine.issue_intent(authorization, intent).code(),
               ReasonCode::RejectedMigrationContractMissing);
  FOS_CHECK_EQ(engine.snapshot().schedule_generation, schedule_before);
  FOS_CHECK(!intent.attempt.is_valid());
}

FOS_TEST(an_incomplete_contract_is_not_safe_for_stateful_migration) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  MigrationContract contract{};
  contract.id = MigrationContractId{1};
  contract.flow = FlowId{1};
  contract.flow_generation = FlowGeneration{1};
  contract.statefulness = Statefulness::Stateful;
  contract.state_transfer_defined = true;
  contract.ordering_preserved = true;
  contract.exclusive_handoff = false;   // missing
  contract.rollback_defined = false;    // missing
  contract.destination_target = TargetId{2};
  contract.destination_incarnation = TargetIncarnation{1};
  contract.destination_capability_generation = CapabilityGeneration{1};
  contract.policy_generation = PolicyGeneration{1};
  FOS_REQUIRE(engine.register_migration_contract(contract).ok());

  PlacementRequest placement{};
  placement.request = RequestId{1};
  placement.evaluation_instant = Timestamp{1000};
  placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  const PlacementResult placed = engine.place(placement);
  FOS_REQUIRE(!placed.placements.empty() && placed.placements.front().accepted);
  Authorization authorization{};
  FOS_REQUIRE(engine.authorize(placed.placements.front().recommendation, Timestamp{1000}, authorization).ok());
  MigrationIntent intent{};
  intent.flow = FlowId{1};
  intent.destination_target = TargetId{2};
  intent.destination_incarnation = TargetIncarnation{1};
  intent.destination_capability_generation = CapabilityGeneration{1};
  intent.issued_at = Timestamp{1000};
  FOS_CHECK_EQ(engine.issue_intent(authorization, intent).code(),
               ReasonCode::RejectedStatefulMigrationUnsafe);
}

FOS_TEST(migration_requires_acknowledgement_before_effect_and_effect_before_verification) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  MigrationContract contract{};
  contract.id = MigrationContractId{1};
  contract.flow = FlowId{1};
  contract.flow_generation = FlowGeneration{1};
  contract.statefulness = Statefulness::Stateful;
  contract.state_transfer_defined = true;
  contract.ordering_preserved = true;
  contract.rollback_defined = true;
  contract.exclusive_handoff = true;
  contract.destination_target = TargetId{2};
  contract.destination_incarnation = TargetIncarnation{1};
  contract.destination_capability_generation = CapabilityGeneration{1};
  contract.policy_generation = PolicyGeneration{1};
  FOS_REQUIRE(engine.register_migration_contract(contract).ok());

  MigrationUnderway underway{};
  FOS_REQUIRE(establish(underway, engine, FlowId{1}, TargetId{2}));
  FOS_CHECK_EQ(underway.intent.contract, MigrationContractId{1});

  // Effect before acknowledgement is refused.
  AppliedEffect effect = make_effect(underway.intent, "state-1");
  FOS_CHECK_EQ(engine.report_applied(effect).code(), ReasonCode::RejectedMigrationEffectBeforeAck);

  // Destination acknowledgement before the source acknowledgement is out of order.
  MigrationAcknowledgement destination{};
  destination.attempt = underway.intent.attempt;
  destination.actor = MigrationActor::Destination;
  destination.target = underway.intent.destination_target;
  destination.incarnation = underway.intent.destination_incarnation;
  destination.capability_generation = underway.intent.destination_capability_generation;
  destination.epoch = underway.intent.epoch;
  destination.boot = underway.intent.boot;
  destination.accepted = true;
  destination.acknowledged_at = Timestamp{1000};
  FOS_CHECK_EQ(engine.acknowledge(destination).code(), ReasonCode::RejectedMigrationAckOutOfOrder);

  FOS_REQUIRE(acknowledge_both(underway));
  MigrationRecord record{};
  FOS_REQUIRE(engine.lookup_migration(underway.intent.attempt, record));
  FOS_CHECK_EQ(record.intent.phase, MigrationPhase::DestinationAcknowledged);
  FOS_CHECK(record.source_acknowledged);
  FOS_CHECK(record.destination_acknowledged);

  // Observation before application is refused: acknowledgement is not effect.
  const EffectObservation premature = make_observation(record, effect.effect_digest);
  FOS_CHECK_EQ(engine.observe(premature).code(), ReasonCode::RejectedMigrationEffectBeforeAck);

  FOS_REQUIRE(engine.report_applied(effect).ok());
  FOS_REQUIRE(engine.lookup_migration(underway.intent.attempt, record));
  FOS_CHECK_EQ(record.intent.phase, MigrationPhase::EffectApplied);

  // A mismatched observation digest cannot verify the effect.
  EffectObservation mismatched = make_observation(record, Sha256::of(std::string_view{"other"}));
  FOS_CHECK_EQ(engine.observe(mismatched).code(), ReasonCode::RejectedMigrationVerificationMismatch);

  // An observation that is not Known cannot stand in for evidence.
  EffectObservation unknown = make_observation(record, effect.effect_digest, EvidenceState::Unknown);
  FOS_CHECK_EQ(engine.observe(unknown).code(), ReasonCode::RejectedMissingEvidence);
  EffectObservation unsupported =
      make_observation(record, effect.effect_digest, EvidenceState::Unsupported);
  FOS_CHECK_EQ(engine.observe(unsupported).code(), ReasonCode::RejectedUnsupportedTarget);
  EffectObservation conflicting =
      make_observation(record, effect.effect_digest, EvidenceState::Conflicting);
  FOS_CHECK_EQ(engine.observe(conflicting).code(), ReasonCode::RejectedConflictingEvidence);
  EffectObservation stale = make_observation(record, effect.effect_digest, EvidenceState::Stale);
  FOS_CHECK_EQ(engine.observe(stale).code(), ReasonCode::RejectedStaleEvidence);

  // A target that is not Ready cannot verify an effect either.
  EffectObservation draining = make_observation(record, effect.effect_digest, EvidenceState::Known,
                                                TargetLifecycle::Draining);
  FOS_CHECK_EQ(engine.observe(draining).code(), ReasonCode::RejectedTargetDraining);

  const EffectObservation good = make_observation(record, effect.effect_digest);
  FOS_REQUIRE(engine.observe(good).ok());
  FOS_REQUIRE(engine.lookup_migration(underway.intent.attempt, record));
  FOS_CHECK_EQ(record.intent.phase, MigrationPhase::Completed);
  FOS_CHECK(record.effect_verified);
  Assignment assignment{};
  FOS_REQUIRE(engine.lookup_assignment(FlowId{1}, assignment));
  FOS_CHECK_EQ(assignment.target, TargetId{2});
  FOS_CHECK_EQ(assignment.effect, EffectState::Verified);
  FOS_CHECK(assignment.supersedes.is_valid());
  FOS_CHECK_EQ(engine.snapshot().counters.effects_verified, static_cast<std::uint64_t>(1));
}

FOS_TEST(exclusive_state_is_never_double_assigned) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  // A second stateful flow claiming the same exclusive state key conflicts.
  FlowDescriptor second = flow(2, Statefulness::Stateful);
  second.exclusive_state_key = ExclusiveStateKeyId{1};
  FOS_REQUIRE(engine.register_flow(second).ok());
  PlacementRequest placement{};
  placement.request = RequestId{1};
  placement.evaluation_instant = Timestamp{1000};
  placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  const PlacementResult first = engine.place(placement);
  FOS_REQUIRE(first.placements.front().accepted);

  placement.request = RequestId{2};
  placement.items.clear();
  placement.items.push_back(PlacementItem{FlowId{2}, FlowGeneration{}});
  const PlacementResult conflicting = engine.place(placement);
  FOS_REQUIRE(!conflicting.placements.empty());
  FOS_CHECK_EQ(conflicting.placements.front().outcome, ReasonCode::RejectedExclusiveStateConflict);
  FOS_CHECK(!conflicting.placements.front().accepted);
}

FOS_TEST(a_second_migration_for_one_flow_is_refused) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  MigrationContract contract{};
  contract.id = MigrationContractId{1};
  contract.flow = FlowId{1};
  contract.flow_generation = FlowGeneration{1};
  contract.statefulness = Statefulness::Stateful;
  contract.state_transfer_defined = true;
  contract.ordering_preserved = true;
  contract.rollback_defined = true;
  contract.exclusive_handoff = true;
  contract.destination_target = TargetId{2};
  contract.destination_incarnation = TargetIncarnation{1};
  contract.destination_capability_generation = CapabilityGeneration{1};
  contract.policy_generation = PolicyGeneration{1};
  FOS_REQUIRE(engine.register_migration_contract(contract).ok());
  MigrationUnderway underway{};
  FOS_REQUIRE(establish(underway, engine, FlowId{1}, TargetId{2}));

  PlacementRequest placement{};
  placement.request = RequestId{2};
  placement.evaluation_instant = Timestamp{1000};
  placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  const PlacementResult placed = engine.place(placement);
  FOS_REQUIRE(!placed.placements.empty());
  Authorization authorization{};
  FOS_REQUIRE(engine.authorize(placed.placements.front().recommendation, Timestamp{1000}, authorization).ok());
  MigrationIntent second{};
  second.flow = FlowId{1};
  second.destination_target = TargetId{2};
  second.destination_incarnation = TargetIncarnation{1};
  second.destination_capability_generation = CapabilityGeneration{1};
  second.issued_at = Timestamp{1000};
  FOS_CHECK_EQ(engine.issue_intent(authorization, second).code(),
               ReasonCode::RejectedMigrationAlreadyInFlight);
}

FOS_TEST(target_disappearance_mid_migration_aborts_the_attempt) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  MigrationContract contract{};
  contract.id = MigrationContractId{1};
  contract.flow = FlowId{1};
  contract.flow_generation = FlowGeneration{1};
  contract.statefulness = Statefulness::Stateful;
  contract.state_transfer_defined = true;
  contract.ordering_preserved = true;
  contract.rollback_defined = true;
  contract.exclusive_handoff = true;
  contract.destination_target = TargetId{2};
  contract.destination_incarnation = TargetIncarnation{1};
  contract.destination_capability_generation = CapabilityGeneration{1};
  contract.policy_generation = PolicyGeneration{1};
  FOS_REQUIRE(engine.register_migration_contract(contract).ok());
  MigrationUnderway underway{};
  FOS_REQUIRE(establish(underway, engine, FlowId{1}, TargetId{2}));
  FOS_REQUIRE(acknowledge_both(underway));

  FOS_REQUIRE(engine.remove_target(TargetId{2}, TargetIncarnation{1}, Timestamp{1000}).ok());
  MigrationRecord record{};
  FOS_REQUIRE(engine.lookup_migration(underway.intent.attempt, record));
  FOS_CHECK_EQ(record.intent.phase, MigrationPhase::Aborted);
  FOS_CHECK_EQ(record.abort_reason, ReasonCode::RejectedTargetRemoved);
  FOS_CHECK(engine.active_fences().size() >= 1U);

  const AppliedEffect effect = make_effect(underway.intent, "state-1");
  FOS_CHECK_EQ(engine.report_applied(effect).code(), ReasonCode::RejectedMigrationAttemptTerminal);
}

FOS_TEST(a_new_incarnation_aborts_in_flight_attempts) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  FOS_REQUIRE(register_safe_contract(engine, MigrationContractId{1}, TargetId{2}));
  MigrationUnderway underway{};
  FOS_REQUIRE(establish(underway, engine, FlowId{1}, TargetId{2}));
  TargetDescriptor replacement = target(2, ExecutionDomain::OffloadDevice);
  replacement.incarnation = TargetIncarnation{2};
  replacement.source_sequence = EvidenceSequence{5};
  FOS_REQUIRE(engine.declare_target(replacement).ok());
  MigrationRecord record{};
  FOS_REQUIRE(engine.lookup_migration(underway.intent.attempt, record));
  FOS_CHECK_EQ(record.intent.phase, MigrationPhase::Aborted);
  FOS_CHECK_EQ(record.abort_reason, ReasonCode::RejectedStaleIncarnation);
}

FOS_TEST(a_stale_authorization_can_never_issue_an_intent) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  PlacementRequest placement{};
  placement.request = RequestId{1};
  placement.evaluation_instant = Timestamp{1000};
  placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  const PlacementResult placed = engine.place(placement);
  FOS_REQUIRE(!placed.placements.empty() && placed.placements.front().accepted);
  Authorization authorization{};
  FOS_REQUIRE(engine.authorize(placed.placements.front().recommendation, Timestamp{1000}, authorization).ok());

  // A newer schedule generation supersedes the authorization. Placing a second
  // flow publishes a new generation, which invalidates the earlier authority.
  FOS_REQUIRE(engine.register_flow(flow(2, Statefulness::Stateful)).ok());
  PlacementRequest second{};
  second.request = RequestId{2};
  second.evaluation_instant = Timestamp{1000};
  second.items.push_back(PlacementItem{FlowId{2}, FlowGeneration{}});
  FOS_REQUIRE(engine.place(second).accepted);

  MigrationIntent intent{};
  intent.flow = FlowId{1};
  intent.destination_target = TargetId{2};
  intent.destination_incarnation = TargetIncarnation{1};
  intent.destination_capability_generation = CapabilityGeneration{1};
  intent.issued_at = Timestamp{1000};
  FOS_CHECK_EQ(engine.issue_intent(authorization, intent).code(),
               ReasonCode::RejectedStaleScheduleGeneration);

  // Revocation is permanent.
  PlacementRequest third{};
  third.request = RequestId{3};
  third.evaluation_instant = Timestamp{1000};
  third.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  const PlacementResult third_result = engine.place(third);
  FOS_REQUIRE(!third_result.placements.empty() && third_result.placements.front().accepted);
  Authorization fresh{};
  FOS_REQUIRE(engine.authorize(third_result.placements.front().recommendation, Timestamp{1000}, fresh).ok());
  FOS_REQUIRE(engine.revoke_authorization(fresh.id).ok());
  FOS_CHECK_EQ(engine.issue_intent(fresh, intent).code(), ReasonCode::RejectedAuthorizationSuperseded);
  FOS_CHECK_EQ(engine.snapshot().counters.authorizations_revoked, static_cast<std::uint64_t>(1));
}

FOS_TEST(a_recommendation_is_not_authority) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  PlacementRequest placement{};
  placement.request = RequestId{1};
  placement.evaluation_instant = Timestamp{1000};
  placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  const PlacementResult placed = engine.place(placement);
  FOS_REQUIRE(!placed.placements.empty() && placed.placements.front().accepted);
  Recommendation recommendation{};
  FOS_REQUIRE(engine.lookup_recommendation(placed.placements.front().recommendation, recommendation));
  FOS_CHECK_EQ(recommendation.outcome, ReasonCode::AcceptedOffloadPlacement);
  FOS_CHECK(!recommendation.digest.is_zero());

  // A recommendation alone cannot issue an intent.
  MigrationIntent intent{};
  intent.flow = FlowId{1};
  intent.destination_target = TargetId{2};
  intent.destination_incarnation = TargetIncarnation{1};
  intent.destination_capability_generation = CapabilityGeneration{1};
  intent.issued_at = Timestamp{1000};
  Authorization forged{};
  forged.id = AuthorizationId{999};
  FOS_CHECK_EQ(engine.issue_intent(forged, intent).code(), ReasonCode::RejectedAuthorizationMissing);

  // An unknown recommendation cannot be authorised.
  Authorization authorization{};
  FOS_CHECK_EQ(engine.authorize(RecommendationId{999}, Timestamp{1000}, authorization).code(),
               ReasonCode::RejectedRecommendationMismatch);
}

FOS_TEST(stateless_migration_needs_no_contract_but_still_needs_verification) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  (void)engine.set_policy(migration_policy());
  (void)engine.declare_target(target(1, ExecutionDomain::OffloadDevice));
  (void)engine.declare_target(target(2, ExecutionDomain::OffloadDevice));
  (void)engine.ingest_load(load(1), Timestamp{1000});
  (void)engine.ingest_load(load(2), Timestamp{1000});
  FOS_REQUIRE(engine.register_flow(flow(1, Statefulness::Stateless)).ok());
  MigrationUnderway underway{};
  FOS_REQUIRE(establish(underway, engine, FlowId{1}, TargetId{2}));
  FOS_CHECK_EQ(underway.intent.contract, MigrationContractId{});
  FOS_REQUIRE(acknowledge_both(underway));
  const AppliedEffect effect = make_effect(underway.intent, "stateless-effect");
  FOS_REQUIRE(engine.report_applied(effect).ok());
  MigrationRecord record{};
  FOS_REQUIRE(engine.lookup_migration(underway.intent.attempt, record));
  FOS_REQUIRE(engine.observe(make_observation(record, effect.effect_digest)).ok());
  Assignment assignment{};
  FOS_REQUIRE(engine.lookup_assignment(FlowId{1}, assignment));
  FOS_CHECK_EQ(assignment.target, TargetId{2});
  FOS_CHECK_EQ(assignment.effect, EffectState::Verified);
}

FOS_TEST(an_applied_effect_without_a_digest_is_refused) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  FOS_REQUIRE(register_safe_contract(engine, MigrationContractId{1}, TargetId{2}));
  MigrationUnderway underway{};
  FOS_REQUIRE(establish(underway, engine, FlowId{1}, TargetId{2}));
  FOS_REQUIRE(acknowledge_both(underway));
  AppliedEffect effect = make_effect(underway.intent, "state");
  effect.effect_digest = Sha256Digest{};
  FOS_CHECK_EQ(engine.report_applied(effect).code(), ReasonCode::RejectedMissingEvidence);
}

FOS_TEST(an_attempt_from_a_previous_boot_is_fenced) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  FOS_REQUIRE(register_safe_contract(engine, MigrationContractId{1}, TargetId{2}));
  MigrationUnderway underway{};
  FOS_REQUIRE(establish(underway, engine, FlowId{1}, TargetId{2}));

  // A duplicate acknowledgement that is byte-identical is idempotent.
  MigrationAcknowledgement source{};
  source.attempt = underway.intent.attempt;
  source.actor = MigrationActor::Source;
  source.target = underway.intent.source_target;
  source.incarnation = underway.intent.source_incarnation;
  source.capability_generation = underway.intent.source_capability_generation;
  source.epoch = underway.intent.epoch;
  source.boot = underway.intent.boot;
  source.accepted = true;
  source.acknowledged_at = Timestamp{1000};
  FOS_REQUIRE(engine.acknowledge(source).ok());
  FOS_CHECK_EQ(engine.acknowledge(source).code(), ReasonCode::AcceptedIdempotentReplay);

  // A conflicting duplicate is refused.
  MigrationAcknowledgement changed = source;
  changed.acknowledged_at = Timestamp{2000};
  FOS_CHECK_EQ(engine.acknowledge(changed).code(), ReasonCode::RejectedMigrationAckDuplicate);

  // An acknowledgement carrying the wrong boot can never be applied.
  MigrationAcknowledgement wrong_boot = source;
  wrong_boot.actor = MigrationActor::Destination;
  wrong_boot.target = underway.intent.destination_target;
  wrong_boot.incarnation = underway.intent.destination_incarnation;
  wrong_boot.capability_generation = underway.intent.destination_capability_generation;
  wrong_boot.boot = BootId{0xDEADBEEFULL, 0xFEEDFACEULL};
  FOS_CHECK_EQ(engine.acknowledge(wrong_boot).code(), ReasonCode::RejectedFencedByRestart);

  // An acknowledgement from a stale incarnation is refused.
  MigrationAcknowledgement stale_incarnation = wrong_boot;
  stale_incarnation.boot = underway.intent.boot;
  stale_incarnation.incarnation = TargetIncarnation{9};
  FOS_CHECK_EQ(engine.acknowledge(stale_incarnation).code(), ReasonCode::RejectedStaleIncarnation);

  FOS_REQUIRE(engine.abort_migration(underway.intent.attempt, ReasonCode::RejectedCancelled,
                                     Timestamp{1000})
                  .ok());
  MigrationRecord record{};
  FOS_REQUIRE(engine.lookup_migration(underway.intent.attempt, record));
  FOS_CHECK_EQ(record.intent.phase, MigrationPhase::Aborted);
  FOS_CHECK_EQ(record.abort_reason, ReasonCode::RejectedCancelled);
}

FOS_TEST(registering_a_new_flow_generation_releases_stale_state) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  seed_stateful(engine);
  PlacementRequest placement{};
  placement.request = RequestId{1};
  placement.evaluation_instant = Timestamp{1000};
  placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  FOS_REQUIRE(engine.place(placement).accepted);
  Assignment assignment{};
  FOS_REQUIRE(engine.lookup_assignment(FlowId{1}, assignment));

  FlowDescriptor replacement = flow(1, Statefulness::Stateful);
  replacement.generation = FlowGeneration{2};
  FOS_REQUIRE(engine.register_flow(replacement).ok());
  FOS_CHECK(!engine.lookup_assignment(FlowId{1}, assignment));

  FlowDescriptor stale = flow(1, Statefulness::Stateful);
  stale.generation = FlowGeneration{1};
  FOS_CHECK_EQ(engine.register_flow(stale).code(), ReasonCode::RejectedStaleFlowGeneration);

  FlowDescriptor conflicting = flow(1, Statefulness::Stateful);
  conflicting.generation = FlowGeneration{2};
  conflicting.demand = CapacityVector{99, 99, 99, 99};
  FOS_CHECK_EQ(engine.register_flow(conflicting).code(), ReasonCode::RejectedConflictingDuplicate);

  FlowDescriptor malformed = flow(2, Statefulness::Stateful);
  malformed.exclusive_state_key = ExclusiveStateKeyId{};
  FOS_CHECK_EQ(engine.register_flow(malformed).code(), ReasonCode::RejectedMalformedInput);
}
