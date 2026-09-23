// Flow Offload Scheduler - concurrency, cancellation and lifecycle tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "flow_offload/engine.hpp"
#include "flow_offload/export.hpp"
#include "harness.hpp"

using namespace flow_offload;

namespace {

Policy concurrent_policy() {
  Policy policy{};
  policy.generation = PolicyGeneration{1};
  policy.domain_preference = DomainPreference::OffloadFirst;
  policy.fallback = FallbackPolicy::AllowHostFallback;
  policy.load_model = LoadModel::ScheduledOnly;
  policy.max_cost_class = CostClass::Standard;
  policy.sticky = true;
  policy.allow_migration = true;
  canonicalize(policy);
  return policy;
}

TargetDescriptor concurrent_target(std::uint64_t id) {
  TargetDescriptor target{};
  target.target = TargetId{id};
  target.incarnation = TargetIncarnation{1};
  target.capability_generation = CapabilityGeneration{1};
  target.topology_generation = TopologyGeneration{1};
  target.domain = ExecutionDomain::OffloadDevice;
  target.kind = TargetKind::SmartNic;
  target.host = HostId{1};
  target.device = DeviceId{id};
  target.cost_class = CostClass::Standard;
  target.capacity = CapacityVector{100000, 1000000, 1000000, 100000};
  target.lifecycle = TargetLifecycle::Ready;
  target.supports_stateful = true;
  target.supports_migration = true;
  target.source = EvidenceSourceId{id};
  target.source_sequence = EvidenceSequence{1};
  target.issued_at = Timestamp{1};
  return target;
}

FlowDescriptor concurrent_flow(std::uint64_t id) {
  FlowDescriptor flow{};
  flow.flow = FlowId{id};
  flow.generation = FlowGeneration{1};
  flow.function = ProcessingFunctionId{1};
  flow.statefulness = Statefulness::Stateless;
  flow.max_cost_class = CostClass::Standard;
  flow.demand = CapacityVector{10, 10, 10, 10};
  flow.allow_migration = true;
  return flow;
}

/// The semantic result of a schedule: which flow runs where, at which
/// generations, with what committed demand. Identifier allocation order follows
/// commit order under concurrency, so identifiers are deliberately excluded -
/// the decisions are what must be reproducible.
std::string schedule_fingerprint(const Engine& engine) {
  const EngineExport state = engine.export_state();
  std::string out;
  for (const Assignment& assignment : state.assignments) {
    out += std::to_string(assignment.flow.value());
    out += ":";
    out += std::to_string(assignment.target.value());
    out += ":";
    out += std::to_string(assignment.incarnation.value());
    out += ":";
    out += std::to_string(assignment.capability_generation.value());
    out += ":";
    out += std::to_string(assignment.flow_generation.value());
    out += ":";
    out += std::to_string(reason_value(assignment.reason));
    out += ";";
  }
  out += "|schedule_generation=";
  out += std::to_string(state.schedule_generation.value());
  for (const TargetExport& target : state.targets) {
    out += "|target";
    out += std::to_string(target.descriptor.target.value());
    out += "=";
    out += std::to_string(target.committed.packets_per_second);
    out += ",";
    out += std::to_string(target.committed.bytes_per_second);
    out += ",";
    out += std::to_string(target.committed.state_bytes);
    out += ",";
    out += std::to_string(target.committed.table_entries);
  }
  return out;
}

/// Builds the same engine state twice: once serially and once with the ingests
/// spread across threads. The resulting exported state must be identical.
std::string run_serial() {
  Engine engine;
  (void)engine.open();
  (void)engine.set_policy(concurrent_policy());
  for (std::uint64_t id = 1; id <= 4; ++id) {
    (void)engine.declare_target(concurrent_target(id));
  }
  for (std::uint64_t id = 1; id <= 16; ++id) {
    (void)engine.register_flow(concurrent_flow(id));
  }
  for (std::uint64_t id = 1; id <= 4; ++id) {
    LoadEvidence evidence{};
    evidence.target = TargetId{id};
    evidence.incarnation = TargetIncarnation{1};
    evidence.capability_generation = CapabilityGeneration{1};
    evidence.source = EvidenceSourceId{100 + id};
    evidence.source_sequence = EvidenceSequence{1};
    evidence.observed_at = Timestamp{50};
    evidence.state = EvidenceState::Known;
    evidence.utilized = CapacityVector{0, 0, 0, 0};
    (void)engine.ingest_load(evidence, Timestamp{50});
  }
  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{100};
  for (std::uint64_t id = 1; id <= 16; ++id) {
    request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
  }
  (void)engine.place(request);
  return export_json(engine.export_state());
}

std::string run_concurrent() {
  Engine engine;
  (void)engine.open();
  (void)engine.set_policy(concurrent_policy());
  std::vector<std::thread> threads;
  for (std::uint64_t id = 1; id <= 4; ++id) {
    threads.emplace_back([&engine, id] { (void)engine.declare_target(concurrent_target(id)); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  threads.clear();
  for (std::uint64_t id = 1; id <= 16; ++id) {
    threads.emplace_back([&engine, id] { (void)engine.register_flow(concurrent_flow(id)); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  threads.clear();
  for (std::uint64_t id = 1; id <= 4; ++id) {
    threads.emplace_back([&engine, id] {
      LoadEvidence evidence{};
      evidence.target = TargetId{id};
      evidence.incarnation = TargetIncarnation{1};
      evidence.capability_generation = CapabilityGeneration{1};
      evidence.source = EvidenceSourceId{100 + id};
      evidence.source_sequence = EvidenceSequence{1};
      evidence.observed_at = Timestamp{50};
      evidence.state = EvidenceState::Known;
      evidence.utilized = CapacityVector{0, 0, 0, 0};
      (void)engine.ingest_load(evidence, Timestamp{50});
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{100};
  for (std::uint64_t id = 1; id <= 16; ++id) {
    request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
  }
  (void)engine.place(request);
  return export_json(engine.export_state());
}

}  // namespace

FOS_TEST(concurrent_ingest_matches_serial_ingest) {
  const std::string serial = run_serial();
  for (int repeat = 0; repeat < 4; ++repeat) {
    FOS_CHECK_EQ(run_concurrent(), serial);
  }
}

FOS_TEST(concurrent_readers_observe_consistent_state) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(concurrent_policy()).ok());
  FOS_REQUIRE(engine.declare_target(concurrent_target(1)).ok());
  FOS_REQUIRE(engine.register_flow(concurrent_flow(1)).ok());
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 3; ++index) {
    readers.emplace_back([&engine, &stop, &reads] {
      while (!stop.load()) {
        FlowDescriptor flow{};
        (void)engine.lookup_flow(FlowId{1}, flow);
        (void)engine.snapshot();
        (void)engine.export_state();
        reads.fetch_add(1);
      }
    });
  }
  for (std::uint64_t round = 1; round <= 200; ++round) {
    PlacementRequest request{};
    request.request = RequestId{round};
    request.evaluation_instant = Timestamp{100 + round};
    request.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
    (void)engine.place(request);
  }
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  FOS_CHECK(reads.load() > 0U);
  FOS_CHECK(engine.snapshot().assignments == 1U);
}

FOS_TEST(asynchronous_operations_complete_and_cancel_correctly) {
  EngineConfig config{};
  config.workers = 4;
  config.max_queue_depth = 8;
  Engine engine(config);
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(concurrent_policy()).ok());
  FOS_REQUIRE(engine.declare_target(concurrent_target(1)).ok());
  for (std::uint64_t id = 1; id <= 8; ++id) {
    FOS_REQUIRE(engine.register_flow(concurrent_flow(id)).ok());
  }
  std::vector<OperationId> operations;
  for (std::uint64_t id = 1; id <= 8; ++id) {
    PlacementRequest request{};
    request.request = RequestId{id};
    request.evaluation_instant = Timestamp{100};
    request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
    const OperationId operation = engine.submit(request);
    FOS_REQUIRE(operation.is_valid());
    operations.push_back(operation);
  }
  std::size_t completed = 0;
  for (const OperationId id : operations) {
    OperationStatus status{};
    FOS_REQUIRE(engine.wait_for(id, status));
    FOS_CHECK(status.state == OperationState::Completed || status.state == OperationState::Rejected);
    FOS_CHECK(status.has_result);
    if (status.state == OperationState::Completed) {
      ++completed;
    }
  }
  FOS_CHECK_EQ(completed, static_cast<std::size_t>(8));
  FOS_CHECK(engine.snapshot().counters.operations_submitted == 8U);
}

FOS_TEST(cancelling_a_pending_operation_prevents_publication) {
  EngineConfig config{};
  config.workers = 1;
  config.max_queue_depth = 64;
  Engine engine(config);
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(concurrent_policy()).ok());
  FOS_REQUIRE(engine.declare_target(concurrent_target(1)).ok());
  for (std::uint64_t id = 1; id <= 64; ++id) {
    FOS_REQUIRE(engine.register_flow(concurrent_flow(id)).ok());
  }
  // Fill the queue so that later submissions cannot start immediately.
  std::vector<OperationId> operations;
  std::size_t cancelled = 0;
  for (std::uint64_t id = 1; id <= 64; ++id) {
    PlacementRequest request{};
    request.request = RequestId{id};
    request.evaluation_instant = Timestamp{100};
    request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
    const OperationId operation = engine.submit(request);
    if (!operation.is_valid()) {
      break;
    }
    operations.push_back(operation);
    const Status status = engine.cancel(operation);
    if (status.ok()) {
      ++cancelled;
    }
  }
  for (const OperationId id : operations) {
    OperationStatus status{};
    FOS_REQUIRE(engine.wait_for(id, status));
    if (status.state == OperationState::Cancelled) {
      FOS_CHECK_EQ(status.reason, ReasonCode::RejectedCancelled);
      FOS_CHECK(!status.has_result);
    }
  }
  FOS_CHECK(cancelled > 0U);
  FOS_CHECK(engine.snapshot().counters.operations_cancelled == cancelled);
}

FOS_TEST(repeated_open_and_shutdown_returns_resources_to_baseline) {
  EngineConfig config{};
  config.workers = 2;
  Engine engine(config);
  for (int round = 0; round < 8; ++round) {
    FOS_REQUIRE(engine.open().ok());
    FOS_REQUIRE(engine.set_policy(concurrent_policy()).ok());
    FOS_REQUIRE(engine.declare_target(concurrent_target(1)).ok());
    FOS_REQUIRE(engine.register_flow(concurrent_flow(1)).ok());
    PlacementRequest request{};
    request.request = RequestId{static_cast<std::uint64_t>(round + 1)};
    request.evaluation_instant = Timestamp{100};
    request.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
    FOS_REQUIRE(engine.place(request).accepted);
    engine.shutdown();
    FOS_CHECK(!engine.is_open());
  }
  FOS_CHECK_EQ(engine.snapshot().counters.restarts, static_cast<std::uint64_t>(8));
  FOS_CHECK_EQ(engine.snapshot().migrations_in_flight, static_cast<std::size_t>(0));
}

FOS_TEST(shutdown_with_work_in_flight_completes_cleanly) {
  EngineConfig config{};
  config.workers = 4;
  config.max_queue_depth = 128;
  Engine engine(config);
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(concurrent_policy()).ok());
  FOS_REQUIRE(engine.declare_target(concurrent_target(1)).ok());
  for (std::uint64_t id = 1; id <= 32; ++id) {
    FOS_REQUIRE(engine.register_flow(concurrent_flow(id)).ok());
  }
  std::vector<OperationId> operations;
  for (std::uint64_t id = 1; id <= 32; ++id) {
    PlacementRequest request{};
    request.request = RequestId{id};
    request.evaluation_instant = Timestamp{100};
    request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
    const OperationId operation = engine.submit(request);
    if (operation.is_valid()) {
      operations.push_back(operation);
    }
  }
  engine.shutdown();
  for (const OperationId id : operations) {
    const OperationStatus status = engine.poll(id);
    FOS_CHECK(status.state != OperationState::Pending && status.state != OperationState::Running);
  }
  FOS_CHECK(!engine.is_open());
}

FOS_TEST(sequential_submissions_are_byte_identical) {
  std::vector<std::string> digests;
  for (int repeat = 0; repeat < 3; ++repeat) {
    Engine engine;
    FOS_REQUIRE(engine.open().ok());
    FOS_REQUIRE(engine.set_policy(concurrent_policy()).ok());
    FOS_REQUIRE(engine.declare_target(concurrent_target(1)).ok());
    FOS_REQUIRE(engine.declare_target(concurrent_target(2)).ok());
    for (std::uint64_t id = 1; id <= 12; ++id) {
      FOS_REQUIRE(engine.register_flow(concurrent_flow(id)).ok());
    }
    for (std::uint64_t id = 1; id <= 12; ++id) {
      PlacementRequest request{};
      request.request = RequestId{id};
      request.evaluation_instant = Timestamp{100};
      request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
      FOS_REQUIRE(engine.place(request).accepted);
    }
    digests.push_back(export_json(engine.export_state()));
    engine.shutdown();
  }
  FOS_CHECK_EQ(digests.size(), static_cast<std::size_t>(3));
  FOS_CHECK_EQ(digests[0], digests[1]);
  FOS_CHECK_EQ(digests[1], digests[2]);
}

FOS_TEST(concurrent_submissions_produce_a_valid_schedule) {
  // Concurrency changes the order in which independent requests observe the
  // state, so byte-identity is not claimed here. What is claimed - and what is
  // checked - is validity: every operation completes, every flow is placed
  // exactly once, every placement matches a declared target's current
  // generations, and demand is conserved.
  for (int repeat = 0; repeat < 4; ++repeat) {
    EngineConfig config{};
    config.workers = 4;
    config.max_queue_depth = 64;
    Engine engine(config);
    FOS_REQUIRE(engine.open().ok());
    FOS_REQUIRE(engine.set_policy(concurrent_policy()).ok());
    FOS_REQUIRE(engine.declare_target(concurrent_target(1)).ok());
    FOS_REQUIRE(engine.declare_target(concurrent_target(2)).ok());
    constexpr std::uint64_t kFlows = 12;
    for (std::uint64_t id = 1; id <= kFlows; ++id) {
      FOS_REQUIRE(engine.register_flow(concurrent_flow(id)).ok());
    }
    std::vector<OperationId> operations;
    for (std::uint64_t id = 1; id <= kFlows; ++id) {
      PlacementRequest request{};
      request.request = RequestId{id};
      request.evaluation_instant = Timestamp{100};
      request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
      const OperationId operation = engine.submit(request);
      FOS_REQUIRE(operation.is_valid());
      operations.push_back(operation);
    }
    for (const OperationId id : operations) {
      OperationStatus status{};
      FOS_REQUIRE(engine.wait_for(id, status));
      FOS_CHECK_EQ(status.state, OperationState::Completed);
    }
    const EngineExport state = engine.export_state();
    FOS_CHECK_EQ(state.assignments.size(), static_cast<std::size_t>(kFlows));
    std::map<std::uint64_t, std::uint64_t> committed;
    std::set<std::uint64_t> seen_flows;
    for (const Assignment& assignment : state.assignments) {
      FOS_CHECK(seen_flows.insert(assignment.flow.value()).second);
      bool matched = false;
      for (const TargetExport& target : state.targets) {
        if (target.descriptor.target != assignment.target) {
          continue;
        }
        matched = target.live;
        FOS_CHECK_EQ(target.descriptor.incarnation, assignment.incarnation);
        FOS_CHECK_EQ(target.descriptor.capability_generation, assignment.capability_generation);
      }
      FOS_CHECK(matched);
      committed[assignment.target.value()] += 10U;
    }
    std::uint64_t total = 0;
    for (const TargetExport& target : state.targets) {
      total += target.committed.packets_per_second;
      FOS_CHECK_EQ(target.committed.packets_per_second, committed[target.descriptor.target.value()]);
    }
    FOS_CHECK_EQ(total, kFlows * 10U);
    engine.shutdown();
  }
}

FOS_TEST(concurrent_submission_allocates_identifiers_without_duplication) {
  EngineConfig config{};
  config.workers = 4;
  config.max_queue_depth = 64;
  Engine engine(config);
  FOS_REQUIRE(engine.open().ok());
  FOS_REQUIRE(engine.set_policy(concurrent_policy()).ok());
  FOS_REQUIRE(engine.declare_target(concurrent_target(1)).ok());
  FOS_REQUIRE(engine.declare_target(concurrent_target(2)).ok());
  constexpr std::uint64_t kFlows = 24;
  for (std::uint64_t id = 1; id <= kFlows; ++id) {
    FOS_REQUIRE(engine.register_flow(concurrent_flow(id)).ok());
  }
  std::vector<OperationId> operations;
  for (std::uint64_t id = 1; id <= kFlows; ++id) {
    PlacementRequest request{};
    request.request = RequestId{id};
    request.evaluation_instant = Timestamp{100};
    request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
    const OperationId operation = engine.submit(request);
    FOS_REQUIRE(operation.is_valid());
    operations.push_back(operation);
  }
  for (const OperationId id : operations) {
    OperationStatus status{};
    FOS_REQUIRE(engine.wait_for(id, status));
    FOS_CHECK_EQ(status.state, OperationState::Completed);
  }
  const EngineExport state = engine.export_state();
  FOS_CHECK_EQ(state.assignments.size(), static_cast<std::size_t>(kFlows));
  std::set<std::uint64_t> assignment_ids;
  std::set<std::uint64_t> flow_ids;
  for (const Assignment& assignment : state.assignments) {
    FOS_CHECK(assignment_ids.insert(assignment.assignment.value()).second);
    FOS_CHECK(flow_ids.insert(assignment.flow.value()).second);
    FOS_CHECK(assignment.assignment.value() >= 1U);
    FOS_CHECK(assignment.assignment.value() <= kFlows);
  }
  FOS_CHECK_EQ(assignment_ids.size(), static_cast<std::size_t>(kFlows));
  // Every flow's demand is committed exactly once.
  std::uint64_t total_packets = 0;
  for (const TargetExport& target : state.targets) {
    total_packets += target.committed.packets_per_second;
  }
  FOS_CHECK_EQ(total_packets, kFlows * 10U);
}
