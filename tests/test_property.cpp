// Flow Offload Scheduler - seeded randomized property and invariant tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "flow_offload/engine.hpp"
#include "flow_offload/export.hpp"
#include "flow_offload/serialize.hpp"
#include "harness.hpp"
#include "support/process.hpp"

using namespace flow_offload;

namespace {

Policy random_policy(fostest::Rng& rng) {
  Policy policy{};
  policy.generation = PolicyGeneration{1};
  policy.domain_preference = rng.chance(50) ? DomainPreference::OffloadFirst
                                            : DomainPreference::HostFirst;
  policy.fallback = static_cast<FallbackPolicy>(1U + static_cast<std::uint8_t>(rng.below(4)));
  policy.load_model = static_cast<LoadModel>(1U + static_cast<std::uint8_t>(rng.below(4)));
  policy.max_cost_class = CostClass::Scarce;
  policy.sticky = rng.chance(50);
  policy.allow_migration = rng.chance(50);
  policy.min_improvement_ppm = static_cast<std::uint32_t>(rng.below(500000));
  policy.max_rebalance_migrations = static_cast<std::uint32_t>(1U + rng.below(8));
  policy.allow_placement_without_load_evidence = rng.chance(25);
  canonicalize(policy);
  return policy;
}

TargetDescriptor random_target(fostest::Rng& rng, std::uint64_t id) {
  TargetDescriptor target{};
  target.target = TargetId{id};
  target.incarnation = TargetIncarnation{1};
  target.capability_generation = CapabilityGeneration{1};
  target.topology_generation = TopologyGeneration{1};
  target.domain = rng.chance(50) ? ExecutionDomain::Host : ExecutionDomain::OffloadDevice;
  target.kind = target.domain == ExecutionDomain::Host ? TargetKind::HostKernelPath
                                                       : TargetKind::SmartNic;
  target.host = HostId{1 + rng.below(2)};
  target.device = target.domain == ExecutionDomain::Host ? DeviceId{0}
                                                         : DeviceId{1 + rng.below(3)};
  CapabilityMask mask = capability_bit(Capability::ConnectionTracking);
  if (rng.chance(50)) {
    mask |= capability_bit(Capability::StatefulAcl);
  }
  target.capabilities = mask;
  target.capacity = CapacityVector{500 + rng.below(2000), 1000000, 1U << 20U,
                                   1000 + rng.below(1000)};
  target.reserved = CapacityVector{rng.below(50), 0, 0, 0};
  target.cost_class = static_cast<CostClass>(1U + static_cast<std::uint8_t>(rng.below(3)));
  target.lifecycle = rng.chance(90) ? TargetLifecycle::Ready : TargetLifecycle::Draining;
  target.supports_stateful = rng.chance(80);
  target.supports_migration = rng.chance(80);
  target.source = EvidenceSourceId{id};
  target.source_sequence = EvidenceSequence{1};
  target.issued_at = Timestamp{1};
  return target;
}

FlowDescriptor random_flow(fostest::Rng& rng, std::uint64_t id, bool stateful) {
  FlowDescriptor flow{};
  flow.flow = FlowId{id};
  flow.generation = FlowGeneration{1};
  flow.function = ProcessingFunctionId{1 + rng.below(2)};
  flow.statefulness = stateful ? Statefulness::Stateful : Statefulness::Stateless;
  flow.exclusive_state_key = stateful ? ExclusiveStateKeyId{id} : ExclusiveStateKeyId{0};
  flow.required_capabilities = capability_bit(Capability::ConnectionTracking);
  if (stateful && rng.chance(50)) {
    flow.required_capabilities |= capability_bit(Capability::StatefulAcl);
  }
  flow.max_cost_class = CostClass::Scarce;
  flow.demand = CapacityVector{10 + rng.below(50), 1000, stateful ? 4096U : 0U, 10};
  flow.allow_migration = rng.chance(70);
  flow.pinned = rng.chance(10);
  return flow;
}

struct Scenario {
  Policy policy{};
  std::vector<TargetDescriptor> targets;
  std::vector<FlowDescriptor> flows;
};

Scenario make_scenario(std::uint64_t seed) {
  fostest::Rng rng(seed);
  Scenario scenario{};
  scenario.policy = random_policy(rng);
  const std::uint64_t target_count = 1 + rng.below(5);
  for (std::uint64_t id = 1; id <= target_count; ++id) {
    scenario.targets.push_back(random_target(rng, id));
  }
  const std::uint64_t flow_count = 1 + rng.below(8);
  for (std::uint64_t id = 1; id <= flow_count; ++id) {
    scenario.flows.push_back(random_flow(rng, id, rng.chance(40)));
  }
  return scenario;
}

void seed(Engine& engine, const Scenario& scenario, bool reverse) {
  (void)engine.set_policy(scenario.policy);
  std::vector<TargetDescriptor> targets = scenario.targets;
  if (reverse) {
    std::reverse(targets.begin(), targets.end());
  }
  for (const TargetDescriptor& target : targets) {
    (void)engine.declare_target(target);
  }
  std::vector<FlowDescriptor> flows = scenario.flows;
  if (reverse) {
    std::reverse(flows.begin(), flows.end());
  }
  for (const FlowDescriptor& flow : flows) {
    (void)engine.register_flow(flow);
  }
  for (const TargetDescriptor& target : scenario.targets) {
    LoadEvidence evidence{};
    evidence.target = target.target;
    evidence.incarnation = target.incarnation;
    evidence.capability_generation = target.capability_generation;
    evidence.source = EvidenceSourceId{100 + target.target.value()};
    evidence.source_sequence = EvidenceSequence{1};
    evidence.observed_at = Timestamp{50};
    evidence.state = EvidenceState::Known;
    evidence.utilized = CapacityVector{target.capacity.packets_per_second / 4U, 0, 0, 0};
    (void)engine.ingest_load(evidence, Timestamp{50});
  }
}

PlacementRequest batch_request(const Scenario& scenario, std::uint64_t request_id,
                               std::uint64_t instant) {
  PlacementRequest request{};
  request.request = RequestId{request_id};
  request.evaluation_instant = Timestamp{instant};
  for (const FlowDescriptor& flow : scenario.flows) {
    request.items.push_back(PlacementItem{flow.flow, FlowGeneration{}});
  }
  return request;
}

/// Every accepted placement must reference a live target whose declared
/// capabilities, cost class and capacity actually admit the flow.
void check_placement_invariants(const Engine& engine) {
  const EngineExport state = engine.export_state();
  std::map<TargetId, const TargetExport*> targets;
  for (const TargetExport& target : state.targets) {
    targets[target.descriptor.target] = &target;
  }
  std::map<FlowId, const FlowDescriptor*> flows;
  for (const FlowDescriptor& flow : state.flows) {
    flows[flow.flow] = &flow;
  }
  CapacityVector committed_total{};
  for (const Assignment& assignment : state.assignments) {
    const auto flow_it = flows.find(assignment.flow);
    FOS_REQUIRE(flow_it != flows.end());
    const auto target_it = targets.find(assignment.target);
    FOS_REQUIRE(target_it != targets.end());
    const TargetDescriptor& descriptor = target_it->second->descriptor;
    const bool current_generations =
        descriptor.incarnation == assignment.incarnation &&
        descriptor.capability_generation == assignment.capability_generation;
    if (!current_generations) {
      // A placement decided against an older target generation may still
      // describe where the flow runs, but it can never be authorised or
      // verified again, so it must not claim a verified effect.
      FOS_CHECK_EQ(assignment.effect, EffectState::Unknown);
      CapacityVector stale_sum{};
      FOS_CHECK(checked_add(committed_total, flow_it->second->demand, stale_sum));
      committed_total = stale_sum;
      FOS_CHECK(!target_it->second->committed.exceeds_any(descriptor.capacity));
      continue;
    }
    FOS_CHECK(satisfies(descriptor.capabilities, flow_it->second->required_capabilities));
    FOS_CHECK(descriptor.cost_class != CostClass::Unknown);
    FOS_CHECK(descriptor.cost_class <= flow_it->second->max_cost_class);
    if (flow_it->second->statefulness == Statefulness::Stateful) {
      FOS_CHECK(descriptor.supports_stateful);
    }
    FOS_CHECK(!target_it->second->committed.exceeds_any(descriptor.capacity));
    CapacityVector sum{};
    FOS_CHECK(checked_add(committed_total, flow_it->second->demand, sum));
    committed_total = sum;
  }
  // Conservation: the demand committed to targets equals the demand of every
  // placed flow, exactly once each.
  CapacityVector observed_total{};
  for (const TargetExport& target : state.targets) {
    CapacityVector sum{};
    FOS_CHECK(checked_add(observed_total, target.committed, sum));
    observed_total = sum;
  }
  FOS_CHECK_EQ(observed_total, committed_total);
}

/// At most one live assignment may exist for any exclusive state key.
void check_exclusive_state(const Engine& engine) {
  const EngineExport state = engine.export_state();
  std::map<FlowId, const FlowDescriptor*> flows;
  for (const FlowDescriptor& flow : state.flows) {
    flows[flow.flow] = &flow;
  }
  std::map<std::uint64_t, TargetId> holders;
  for (const Assignment& assignment : state.assignments) {
    const auto flow_it = flows.find(assignment.flow);
    if (flow_it == flows.end() ||
        flow_it->second->statefulness != Statefulness::Stateful) {
      continue;
    }
    const std::uint64_t key = flow_it->second->exclusive_state_key.value();
    const auto existing = holders.find(key);
    if (existing != holders.end()) {
      FOS_CHECK(existing->second == assignment.target);
    }
    holders[key] = assignment.target;
  }
}

}  // namespace

FOS_TEST(placement_is_order_independent_across_many_scenarios) {
  for (std::uint64_t seed_value = 1; seed_value <= 60; ++seed_value) {
    const Scenario scenario = make_scenario(seed_value);
    std::uint64_t forward_digest = 0;
    std::uint64_t reverse_digest = 0;
    ScheduleGeneration forward_generation{};
    ScheduleGeneration reverse_generation{};
    for (const bool reverse : {false, true}) {
      Engine engine;
      FOS_REQUIRE(engine.open().ok());
      seed(engine, scenario, reverse);
      const PlacementResult result = batch_request(scenario, 1, 1000).items.empty()
                                         ? PlacementResult{}
                                         : engine.place(batch_request(scenario, 1, 1000));
      const EngineExport state = engine.export_state();
      const auto bytes = [](const std::string& text) {
        return Sha256::of(std::string_view{text});
      };
      const std::uint64_t digest = [&state] {
        std::uint64_t value = 0;
        for (const std::uint8_t byte : export_json(state)) {
          value = (value * 1099511628211ULL) ^ byte;
        }
        return value;
      }();
      (void)bytes;
      if (reverse) {
        reverse_digest = digest;
        reverse_generation = result.schedule_generation;
      } else {
        forward_digest = digest;
        forward_generation = result.schedule_generation;
      }
      check_placement_invariants(engine);
      check_exclusive_state(engine);
    }
    FOS_CHECK_EQ(forward_digest, reverse_digest);
    FOS_CHECK_EQ(forward_generation, reverse_generation);
  }
}

FOS_TEST(capacity_bounds_hold_under_random_batches) {
  for (std::uint64_t seed_value = 1; seed_value <= 60; ++seed_value) {
    const Scenario scenario = make_scenario(seed_value);
    Engine engine;
    FOS_REQUIRE(engine.open().ok());
    seed(engine, scenario, false);
    for (std::uint64_t round = 1; round <= 3; ++round) {
      (void)engine.place(batch_request(scenario, round, 1000 + round));
      check_placement_invariants(engine);
      check_exclusive_state(engine);
    }
    // Duplicate delivery of the same request is idempotent: no placement,
    // assignment, generation or recommendation changes. The only difference is
    // the counter that records the duplicate, which is deliberately observable.
    ExportOptions without_counters{};
    without_counters.include_counters = false;
    const PlacementResult first = engine.place(batch_request(scenario, 99, 2000));
    const EngineExport after_first = engine.export_state();
    const PlacementResult replay = engine.place(batch_request(scenario, 99, 2000));
    const EngineExport after_replay = engine.export_state();
    FOS_CHECK(replay.duplicate);
    FOS_CHECK_EQ(replay.schedule_generation, first.schedule_generation);
    FOS_CHECK_EQ(export_json(after_first, without_counters),
                 export_json(after_replay, without_counters));
    FOS_CHECK_EQ(after_replay.counters.requests_deduplicated,
                 after_first.counters.requests_deduplicated + 1U);
    FOS_CHECK_EQ(after_replay.schedule_generation, after_first.schedule_generation);
  }
}

FOS_TEST(deterministic_fallback_across_random_scenarios) {
  for (std::uint64_t seed_value = 1; seed_value <= 40; ++seed_value) {
    const Scenario scenario = make_scenario(seed_value);
    std::vector<ReasonCode> outcomes;
    for (int repeat = 0; repeat < 3; ++repeat) {
      Engine engine;
      FOS_REQUIRE(engine.open().ok());
      seed(engine, scenario, false);
      const PlacementResult result = engine.place(batch_request(scenario, 1, 1000));
      std::vector<ReasonCode> run;
      for (const FlowPlacement& placement : result.placements) {
        run.push_back(placement.outcome);
      }
      if (repeat == 0) {
        outcomes = run;
      } else {
        FOS_CHECK(outcomes == run);
      }
      check_placement_invariants(engine);
    }
  }
}

FOS_TEST(stale_generations_are_never_reused_after_redeclaration) {
  for (std::uint64_t seed_value = 1; seed_value <= 30; ++seed_value) {
    const Scenario scenario = make_scenario(seed_value);
    Engine engine;
    FOS_REQUIRE(engine.open().ok());
    seed(engine, scenario, false);
    (void)engine.place(batch_request(scenario, 1, 1000));

    fostest::Rng rng(seed_value * 31U);
    for (const TargetDescriptor& target : scenario.targets) {
      if (!rng.chance(60)) {
        continue;
      }
      TargetDescriptor replacement = target;
      replacement.incarnation = TargetIncarnation{1 + rng.below(3)};
      replacement.capability_generation = CapabilityGeneration{1 + rng.below(3)};
      if (rng.chance(30)) {
        replacement.lifecycle = TargetLifecycle::Draining;
      }
      (void)engine.declare_target(replacement);
    }
    (void)engine.place(batch_request(scenario, 2, 2000));
    check_placement_invariants(engine);
    check_exclusive_state(engine);
  }
}

FOS_TEST(restart_preserves_invariants_across_random_stores) {
  for (std::uint64_t seed_value = 1; seed_value <= 20; ++seed_value) {
    const Scenario scenario = make_scenario(seed_value);
    const std::string directory = fostest::make_temp_directory("fos-property-restart");
    FOS_REQUIRE(!directory.empty());
    EngineConfig config{};
    config.persistence_enabled = true;
    config.store.directory = directory;
    {
      Engine engine(config);
      FOS_REQUIRE(engine.open().ok());
      seed(engine, scenario, false);
      (void)engine.place(batch_request(scenario, 1, 1000));
      check_placement_invariants(engine);
      engine.shutdown();
    }
    {
      Engine engine(config);
      const Status opened = engine.open();
      FOS_REQUIRE(opened.ok());
      check_placement_invariants(engine);
      check_exclusive_state(engine);
      // Liveness is not resurrected, so nothing may be placed without the owner
      // re-declaring its targets.
      for (const TargetExport& target : engine.export_state().targets) {
        FOS_CHECK(!target.live);
      }
      engine.shutdown();
    }
    fostest::remove_directory(directory);
  }
}

FOS_TEST(canonical_decoding_never_accepts_malformed_bytes_as_success) {
  fostest::Rng rng(0xC0FFEEULL);
  std::size_t accepted = 0;
  for (std::uint64_t round = 0; round < 4000; ++round) {
    const std::size_t length = static_cast<std::size_t>(rng.below(256));
    std::vector<std::byte> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
      bytes[i] = static_cast<std::byte>(rng.below(256));
    }
    CanonicalReader reader(std::span<const std::byte>(bytes.data(), bytes.size()));
    DurableState state{};
    if (decode(reader, state)) {
      ++accepted;
      // Anything that decodes must be canonical: re-encoding produces exactly
      // the bytes that were consumed.
      CanonicalWriter writer;
      encode(writer, state);
      FOS_REQUIRE(writer.ok());
      FOS_CHECK_EQ(writer.size(), reader.consumed());
      FOS_CHECK(std::equal(writer.bytes().begin(), writer.bytes().end(), bytes.begin()));
    } else {
      FOS_CHECK(reader.failed());
      FOS_CHECK(is_refusal(reader.reason()));
    }
  }
  // Random bytes essentially never form a valid state record.
  FOS_CHECK(accepted <= 1U);
}

FOS_TEST(truncated_valid_encodings_are_never_accepted) {
  DurableState state{};
  state.epoch = CoordinatorEpoch{7};
  state.boot = BootId{3, 4};
  state.policy_present = true;
  state.policy.generation = PolicyGeneration{2};
  state.policy.domain_preference = DomainPreference::HostFirst;
  state.policy.fallback = FallbackPolicy::Forbid;
  state.policy.load_model = LoadModel::ScheduledOnly;
  state.policy.max_cost_class = CostClass::Standard;
  state.canonicalize_order();
  CanonicalWriter writer;
  encode(writer, state);
  FOS_REQUIRE(writer.ok());
  const std::vector<std::byte> full(writer.bytes().begin(), writer.bytes().end());
  for (std::size_t length = 0; length < full.size(); ++length) {
    CanonicalReader reader(std::span<const std::byte>(full.data(), length));
    DurableState decoded{};
    if (decode(reader, decoded)) {
      FOS_CHECK(false);
    }
  }
  CanonicalReader reader(std::span<const std::byte>(full.data(), full.size()));
  DurableState decoded{};
  FOS_CHECK(decode(reader, decoded));
}

FOS_TEST(random_rebalance_plans_are_bounded_and_deterministic) {
  for (std::uint64_t seed_value = 1; seed_value <= 30; ++seed_value) {
    const Scenario scenario = make_scenario(seed_value);
    std::vector<std::uint32_t> first_selection;
    for (int repeat = 0; repeat < 2; ++repeat) {
      Engine engine;
      FOS_REQUIRE(engine.open().ok());
      seed(engine, scenario, false);
      (void)engine.place(batch_request(scenario, 1, 1000));
      RebalanceRequest request{};
      request.request = RequestId{2};
      request.evaluation_instant = Timestamp{2000};
      const RebalanceReport report = engine.rebalance(request);
      FOS_CHECK(report.selected <= report.eligible);
      FOS_CHECK_EQ(report.selected + report.truncated, report.eligible);
      FOS_CHECK(report.intents.size() == report.selected);
      FOS_CHECK(report.selected <= engine.export_state().policy.max_rebalance_migrations);
      std::vector<std::uint32_t> selection;
      for (const MigrationIntent& intent : report.intents) {
        selection.push_back(static_cast<std::uint32_t>(intent.flow.value()));
      }
      if (repeat == 0) {
        first_selection = selection;
      } else {
        FOS_CHECK(first_selection == selection);
      }
      // Planning is not issuing: no attempt exists yet.
      FOS_CHECK_EQ(engine.export_state().migrations.size(), static_cast<std::size_t>(0));
    }
  }
}

FOS_TEST(random_evidence_ordering_never_advances_a_watermark_backwards) {
  for (std::uint64_t seed_value = 1; seed_value <= 40; ++seed_value) {
    fostest::Rng rng(seed_value);
    Engine engine;
    FOS_REQUIRE(engine.open().ok());
    TargetDescriptor target = random_target(rng, 1);
    target.lifecycle = TargetLifecycle::Ready;
    FOS_REQUIRE(engine.declare_target(target).ok());
    std::uint64_t highest = 0;
    for (int step = 0; step < 40; ++step) {
      LoadEvidence evidence{};
      evidence.target = TargetId{1};
      evidence.incarnation = target.incarnation;
      evidence.capability_generation = target.capability_generation;
      evidence.source = EvidenceSourceId{1};
      evidence.source_sequence = EvidenceSequence{1 + rng.below(20)};
      evidence.observed_at = Timestamp{100 + rng.below(50)};
      evidence.state = EvidenceState::Known;
      evidence.utilized = CapacityVector{rng.below(100), 0, 0, 0};
      const Status status = engine.ingest_load(evidence, Timestamp{1000});
      if (status.ok()) {
        highest = std::max(highest, evidence.source_sequence.value());
      }
      const EngineExport state = engine.export_state();
      std::uint64_t observed = 0;
      for (const SourceWatermark& mark : state.watermarks) {
        observed = std::max(observed, mark.sequence.value());
      }
      FOS_CHECK(observed <= highest);
    }
  }
}
