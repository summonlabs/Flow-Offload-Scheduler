// Flow Offload Scheduler - benchmark of completed placements.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The benchmark measures work that has actually completed. Every asynchronous
// submission is waited on to a terminal state before any throughput is
// reported, and the run fails if any operation did not complete or if the
// conservation invariant is violated. Enqueue latency is never reported as a
// result.
//
// The wall clock is used for measurement only. It is never a synchronisation
// mechanism: completion is observed through the runtime's own completion signal.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "flow_offload/engine.hpp"

namespace {

using namespace flow_offload;

struct Options {
  std::uint64_t targets = 16;
  std::uint64_t flows = 2048;
  std::uint64_t rounds = 8;
  std::uint32_t workers = 4;
};

bool parse(int argc, char** argv, Options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    const bool has_value = i + 1 < argc;
    if (token == "--targets" && has_value) {
      out.targets = std::strtoull(argv[++i], nullptr, 10);
    } else if (token == "--flows" && has_value) {
      out.flows = std::strtoull(argv[++i], nullptr, 10);
    } else if (token == "--rounds" && has_value) {
      out.rounds = std::strtoull(argv[++i], nullptr, 10);
    } else if (token == "--workers" && has_value) {
      out.workers = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      return false;
    }
  }
  return out.targets > 0U && out.flows > 0U && out.rounds > 0U && out.workers <= kMaxWorkers;
}

TargetDescriptor make_target(std::uint64_t id) {
  TargetDescriptor target{};
  target.target = TargetId{id};
  target.incarnation = TargetIncarnation{1};
  target.capability_generation = CapabilityGeneration{1};
  target.topology_generation = TopologyGeneration{1};
  target.domain = (id % 4U == 0U) ? ExecutionDomain::Host : ExecutionDomain::OffloadDevice;
  target.kind = target.domain == ExecutionDomain::Host ? TargetKind::HostKernelPath
                                                       : TargetKind::SmartNic;
  target.host = HostId{1 + (id % 4U)};
  target.device = target.domain == ExecutionDomain::Host ? DeviceId{0} : DeviceId{id};
  target.capabilities = capability_bit(Capability::ConnectionTracking) |
                        capability_bit(Capability::StatefulAcl);
  target.capacity = CapacityVector{100000000ULL, 100000000000ULL, 1ULL << 30U, 10000000ULL};
  target.cost_class = CostClass::Standard;
  target.lifecycle = TargetLifecycle::Ready;
  target.supports_stateful = true;
  target.supports_migration = true;
  target.source = EvidenceSourceId{id};
  target.source_sequence = EvidenceSequence{1};
  target.issued_at = Timestamp{1};
  return target;
}

FlowDescriptor make_flow(std::uint64_t id) {
  FlowDescriptor flow{};
  flow.flow = FlowId{id};
  flow.generation = FlowGeneration{1};
  flow.function = ProcessingFunctionId{1};
  flow.statefulness = (id % 5U == 0U) ? Statefulness::Stateful : Statefulness::Stateless;
  flow.exclusive_state_key = flow.statefulness == Statefulness::Stateful ? ExclusiveStateKeyId{id}
                                                                        : ExclusiveStateKeyId{0};
  flow.required_capabilities = capability_bit(Capability::ConnectionTracking);
  flow.max_cost_class = CostClass::Standard;
  flow.demand = CapacityVector{100, 10000, flow.statefulness == Statefulness::Stateful ? 4096U : 0U,
                               10};
  flow.allow_migration = true;
  return flow;
}

Policy make_policy() {
  Policy policy{};
  policy.generation = PolicyGeneration{1};
  policy.domain_preference = DomainPreference::OffloadFirst;
  policy.fallback = FallbackPolicy::AllowHostFallback;
  policy.load_model = LoadModel::SumConservative;
  policy.max_cost_class = CostClass::Standard;
  policy.sticky = true;
  policy.allow_migration = true;
  canonicalize(policy);
  return policy;
}

/// Verifies conservation over the committed schedule and returns the number of
/// assignments that were checked.
bool check_conservation(const Engine& engine, std::uint64_t& assignments) {
  const std::map<TargetId, CapacityVector> committed = engine.committed_demand();
  const EngineExport state = engine.export_state();
  assignments = state.assignments.size();
  std::map<FlowId, const FlowDescriptor*> flows;
  for (const FlowDescriptor& flow : state.flows) {
    flows[flow.flow] = &flow;
  }
  std::map<TargetId, CapacityVector> expected;
  for (const Assignment& assignment : state.assignments) {
    const auto flow_it = flows.find(assignment.flow);
    if (flow_it == flows.end()) {
      return false;
    }
    CapacityVector sum{};
    if (!checked_add(expected[assignment.target], flow_it->second->demand, sum)) {
      return false;
    }
    expected[assignment.target] = sum;
  }
  for (const TargetExport& target : state.targets) {
    const auto observed = committed.find(target.descriptor.target);
    const CapacityVector value = observed == committed.end() ? CapacityVector{} : observed->second;
    const auto want = expected.find(target.descriptor.target);
    const CapacityVector want_value = want == expected.end() ? CapacityVector{} : want->second;
    if (!(value == want_value)) {
      return false;
    }
    if (target.committed.exceeds_any(target.descriptor.capacity)) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) {
    std::fprintf(stderr,
                 "usage: fos_bench [--targets N] [--flows N] [--rounds N] [--workers N]\n");
    return 2;
  }
  EngineConfig config{};
  config.workers = options.workers;
  config.max_queue_depth = kMaxQueueDepth;
  Engine engine(config);
  if (engine.open().failed()) {
    std::fprintf(stderr, "bench: engine could not open\n");
    return 1;
  }
  if (engine.set_policy(make_policy()).failed()) {
    std::fprintf(stderr, "bench: policy refused\n");
    return 1;
  }
  for (std::uint64_t id = 1; id <= options.targets; ++id) {
    const TargetDescriptor target = make_target(id);
    if (engine.declare_target(target).failed()) {
      std::fprintf(stderr, "bench: target %llu refused\n",
                   static_cast<unsigned long long>(id));
      return 1;
    }
    // A target with no load evidence is not placeable, and a benchmark that
    // measured refusals would measure nothing. Supply an observation.
    LoadEvidence evidence{};
    evidence.target = target.target;
    evidence.incarnation = target.incarnation;
    evidence.capability_generation = target.capability_generation;
    evidence.source = EvidenceSourceId{1000 + id};
    evidence.source_sequence = EvidenceSequence{1};
    evidence.observed_at = Timestamp{1};
    evidence.state = EvidenceState::Known;
    evidence.utilized =
        CapacityVector{target.capacity.packets_per_second / 10U, 0, 0, 0};
    if (engine.ingest_load(evidence, Timestamp{1}).failed()) {
      std::fprintf(stderr, "bench: load evidence for target %llu refused\n",
                   static_cast<unsigned long long>(id));
      return 1;
    }
  }
  for (std::uint64_t id = 1; id <= options.flows; ++id) {
    if (engine.register_flow(make_flow(id)).failed()) {
      std::fprintf(stderr, "bench: flow %llu refused\n", static_cast<unsigned long long>(id));
      return 1;
    }
  }

  // ---- synchronous placement -------------------------------------------
  std::uint64_t accepted = 0;
  std::uint64_t requested = 0;
  const auto sync_start = std::chrono::steady_clock::now();
  for (std::uint64_t round = 1; round <= options.rounds; ++round) {
    PlacementRequest request{};
    request.request = RequestId{round};
    request.evaluation_instant = Timestamp{1000 + round};
    for (std::uint64_t id = 1; id <= options.flows; ++id) {
      request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
    }
    const PlacementResult result = engine.place(request);
    requested += result.placements.size();
    for (const FlowPlacement& placement : result.placements) {
      if (placement.accepted) {
        ++accepted;
      }
    }
  }
  const auto sync_end = std::chrono::steady_clock::now();
  const double sync_seconds = std::chrono::duration<double>(sync_end - sync_start).count();
  if (requested == 0U) {
    std::fprintf(stderr, "bench: no placement decisions were requested\n");
    return 1;
  }
  if (accepted == 0U) {
    std::fprintf(stderr, "bench: no placement was accepted; the measurement would be empty\n");
    return 1;
  }
  std::uint64_t assignments = 0;
  if (!check_conservation(engine, assignments)) {
    std::fprintf(stderr, "bench: conservation invariant violated\n");
    return 1;
  }
  if (assignments != options.flows) {
    std::fprintf(stderr, "bench: expected %llu assignments but found %llu\n",
                 static_cast<unsigned long long>(options.flows),
                 static_cast<unsigned long long>(assignments));
    return 1;
  }

  // ---- asynchronous placement ------------------------------------------
  // Every submission is waited on to a terminal state, so the reported figure
  // counts completed decisions rather than accepted queue entries.
  std::vector<OperationId> operations;
  operations.reserve(options.rounds);
  std::uint64_t completed = 0;
  std::uint64_t async_decisions = 0;
  const auto async_start = std::chrono::steady_clock::now();
  for (std::uint64_t round = 1; round <= options.rounds; ++round) {
    PlacementRequest request{};
    request.request = RequestId{1000 + round};
    request.evaluation_instant = Timestamp{1000 + round};
    for (std::uint64_t id = 1; id <= options.flows; ++id) {
      request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
    }
    const OperationId operation = engine.submit(request);
    if (!operation.is_valid()) {
      std::fprintf(stderr, "bench: submission was refused by backpressure\n");
      return 1;
    }
    operations.push_back(operation);
  }
  for (const OperationId id : operations) {
    OperationStatus status{};
    if (!engine.wait_for(id, status)) {
      std::fprintf(stderr, "bench: operation vanished before completion\n");
      return 1;
    }
    if (status.state != OperationState::Completed) {
      std::fprintf(stderr, "bench: operation did not complete: %s\n",
                   std::string{to_string(status.reason)}.c_str());
      return 1;
    }
    if (!status.has_result) {
      std::fprintf(stderr, "bench: completed operation reported no result\n");
      return 1;
    }
    ++completed;
    async_decisions += status.result.placements.size();
  }
  const auto async_end = std::chrono::steady_clock::now();
  const double async_seconds = std::chrono::duration<double>(async_end - async_start).count();
  if (completed != operations.size()) {
    std::fprintf(stderr, "bench: only %llu of %llu operations completed\n",
                 static_cast<unsigned long long>(completed),
                 static_cast<unsigned long long>(operations.size()));
    return 1;
  }
  if (!check_conservation(engine, assignments)) {
    std::fprintf(stderr, "bench: conservation invariant violated after async run\n");
    return 1;
  }
  if (assignments != options.flows) {
    std::fprintf(stderr, "bench: assignment count changed during the async run\n");
    return 1;
  }

  const EngineSnapshot snapshot = engine.snapshot();
  std::printf("flow-offload-scheduler benchmark\n");
  std::printf("  targets=%llu flows=%llu rounds=%llu workers=%u\n",
              static_cast<unsigned long long>(options.targets),
              static_cast<unsigned long long>(options.flows),
              static_cast<unsigned long long>(options.rounds), options.workers);
  std::printf("  synchronous: decisions=%llu accepted=%llu elapsed=%.3fs rate=%.0f decisions/s\n",
              static_cast<unsigned long long>(requested),
              static_cast<unsigned long long>(accepted), sync_seconds,
              sync_seconds > 0.0 ? static_cast<double>(requested) / sync_seconds : 0.0);
  std::printf("  asynchronous: operations_completed=%llu decisions=%llu elapsed=%.3fs "
              "rate=%.0f decisions/s\n",
              static_cast<unsigned long long>(completed),
              static_cast<unsigned long long>(async_decisions), async_seconds,
              async_seconds > 0.0 ? static_cast<double>(async_decisions) / async_seconds : 0.0);
  std::printf("  assignments=%llu schedule_generation=%llu recommendations=%llu\n",
              static_cast<unsigned long long>(assignments),
              static_cast<unsigned long long>(snapshot.schedule_generation.value()),
              static_cast<unsigned long long>(snapshot.counters.recommendations_issued));
  std::printf("  conservation: OK (committed demand equals the sum of placed flow demand)\n");
  engine.shutdown();
  return 0;
}
