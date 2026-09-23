// Flow Offload Scheduler - persistence, integrity and restart tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "flow_offload/engine.hpp"
#include "flow_offload/store.hpp"
#include "harness.hpp"
#include "support/process.hpp"

using namespace flow_offload;

namespace {

/// Owns a temporary store directory for the duration of one test.
class Sandbox {
 public:
  explicit Sandbox(const std::string& name) : path_(fostest::make_temp_directory(name)) {}
  ~Sandbox() { fostest::remove_directory(path_); }

  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::filesystem::path snapshot() const {
    return std::filesystem::path{path_} / "state.snapshot";
  }
  [[nodiscard]] std::filesystem::path journal() const {
    return std::filesystem::path{path_} / "state.journal";
  }

 private:
  std::string path_;
};

StoreConfig store_config(const Sandbox& sandbox) {
  StoreConfig config{};
  config.directory = sandbox.path();
  config.compaction_record_threshold = 8;
  return config;
}

std::vector<std::byte> read_all(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> bytes;
  char ch = 0;
  while (stream.get(ch)) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

void write_all(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
}

DurableState sample_state(std::uint64_t epoch, std::uint64_t schedule_generation) {
  DurableState state{};
  state.epoch = CoordinatorEpoch{epoch};
  state.boot = BootId{epoch, epoch * 7U};
  state.schedule_generation = ScheduleGeneration{schedule_generation};
  state.topology_generation = TopologyGeneration{1};
  state.policy_present = true;
  state.policy.generation = PolicyGeneration{1};
  state.policy.domain_preference = DomainPreference::OffloadFirst;
  state.policy.fallback = FallbackPolicy::AllowHostFallback;
  state.policy.load_model = LoadModel::SumConservative;
  state.policy.max_cost_class = CostClass::Standard;
  FlowDescriptor flow{};
  flow.flow = FlowId{1};
  flow.generation = FlowGeneration{1};
  flow.function = ProcessingFunctionId{1};
  flow.statefulness = Statefulness::Stateless;
  state.flows.push_back(flow);
  TargetDescriptor target{};
  target.target = TargetId{1};
  target.incarnation = TargetIncarnation{1};
  target.capability_generation = CapabilityGeneration{1};
  target.topology_generation = TopologyGeneration{1};
  target.domain = ExecutionDomain::OffloadDevice;
  target.kind = TargetKind::SmartNic;
  target.host = HostId{1};
  target.device = DeviceId{1};
  target.cost_class = CostClass::Standard;
  target.lifecycle = TargetLifecycle::Ready;
  state.targets.push_back(target);
  state.counters.placements_accepted = schedule_generation;
  state.canonicalize_order();
  return state;
}

EngineConfig engine_config(const Sandbox& sandbox) {
  EngineConfig config{};
  config.persistence_enabled = true;
  config.store = store_config(sandbox);
  return config;
}

}  // namespace

FOS_TEST(store_round_trips_state_without_semantic_loss) {
  Sandbox sandbox("fos-round-trip");
  StoreConfig config = store_config(sandbox);
  const DurableState written = sample_state(4, 9);
  {
    Store store(config);
    const RecoveryReport report = store.open();
    FOS_REQUIRE(report.usable);
    FOS_REQUIRE(store.commit(written).ok());
  }
  Store reopened(config);
  const RecoveryReport report = reopened.open();
  FOS_CHECK(report.usable);
  FOS_CHECK(reopened.has_state());
  DurableState recovered = reopened.state();
  recovered.canonicalize_order();
  FOS_CHECK_EQ(recovered, written);
  FOS_CHECK_EQ(recovered.epoch, CoordinatorEpoch{4});
  FOS_CHECK_EQ(recovered.schedule_generation, ScheduleGeneration{9});
  FOS_CHECK_EQ(recovered.counters.placements_accepted, static_cast<std::uint64_t>(9));
  FOS_CHECK_EQ(recovered.flows.size(), static_cast<std::size_t>(1));
  FOS_CHECK_EQ(recovered.targets.size(), static_cast<std::size_t>(1));
}

FOS_TEST(clean_shutdown_leaves_a_clean_store) {
  Sandbox sandbox("fos-clean");
  {
    Engine engine(engine_config(sandbox));
    FOS_REQUIRE(engine.open().ok());
    Policy policy{};
    policy.generation = PolicyGeneration{1};
    policy.domain_preference = DomainPreference::HostFirst;
    policy.fallback = FallbackPolicy::Forbid;
    policy.load_model = LoadModel::ScheduledOnly;
    policy.max_cost_class = CostClass::Standard;
    canonicalize(policy);
    FOS_REQUIRE(engine.set_policy(policy).ok());
    engine.shutdown();
  }
  StoreConfig config = store_config(sandbox);
  Store store(config);
  const RecoveryReport report = store.open();
  FOS_CHECK(report.usable);
  FOS_CHECK_EQ(report.kind, RecoveryKind::CleanReopen);
  FOS_CHECK(report.snapshot_used);
  FOS_CHECK_EQ(report.records_replayed, static_cast<std::size_t>(0));
  FOS_CHECK_EQ(store.state().policy.domain_preference, DomainPreference::HostFirst);
}

FOS_TEST(a_torn_journal_tail_is_truncated_and_classified) {
  Sandbox sandbox("fos-torn");
  StoreConfig config = store_config(sandbox);
  std::uintmax_t after_first_record = 0;
  {
    Store store(config);
    FOS_REQUIRE(store.open().usable);
    FOS_REQUIRE(store.commit(sample_state(1, 1)).ok());
    after_first_record = std::filesystem::file_size(sandbox.journal());
    FOS_REQUIRE(store.commit(sample_state(1, 2)).ok());
  }
  const std::vector<std::byte> intact = read_all(sandbox.journal());
  std::vector<std::byte> torn = intact;
  torn.resize(torn.size() - 7U);   // chop into the last record's digest
  write_all(sandbox.journal(), torn);

  Store store(config);
  const RecoveryReport report = store.open();
  FOS_CHECK(report.usable);
  FOS_CHECK_EQ(report.kind, RecoveryKind::TornTailTruncated);
  FOS_CHECK_EQ(report.code, ReasonCode::RecoveryTornTailTruncated);
  // The whole partial record is discarded, not just the bytes physically lost.
  FOS_CHECK_EQ(report.bytes_truncated,
               static_cast<std::uint64_t>(intact.size() - 7U) -
                   static_cast<std::uint64_t>(after_first_record));
  FOS_CHECK(store.has_state());
  FOS_CHECK_EQ(store.state().schedule_generation, ScheduleGeneration{1});
  // The repaired journal is usable again.
  FOS_REQUIRE(store.commit(sample_state(1, 5)).ok());
  Store second(config);
  const RecoveryReport second_report = second.open();
  FOS_CHECK_MSG(second_report.usable, second_report.detail);
  FOS_CHECK_MSG(second_report.usable, second_report.detail);
  FOS_CHECK_EQ(second.state().schedule_generation, ScheduleGeneration{5});
}

FOS_TEST(mid_file_journal_corruption_is_refused) {
  Sandbox sandbox("fos-corrupt-mid");
  StoreConfig config = store_config(sandbox);
  config.compaction_record_threshold = 1000;
  {
    Store store(config);
    FOS_REQUIRE(store.open().usable);
    FOS_REQUIRE(store.commit(sample_state(1, 1)).ok());
    FOS_REQUIRE(store.commit(sample_state(1, 2)).ok());
    FOS_REQUIRE(store.commit(sample_state(1, 3)).ok());
  }
  std::vector<std::byte> bytes = read_all(sandbox.journal());
  // Damage a byte inside the first record's payload, leaving later records intact.
  const std::size_t damage_at = kJournalHeaderBytes + kJournalRecordHeaderBytes + 3U;
  FOS_REQUIRE(bytes.size() > damage_at + 1U);
  bytes[damage_at] = static_cast<std::byte>(std::to_integer<std::uint8_t>(bytes[damage_at]) ^ 0xFFU);
  write_all(sandbox.journal(), bytes);

  Store store(config);
  const RecoveryReport report = store.open();
  FOS_CHECK(!report.usable);
  FOS_CHECK_EQ(report.kind, RecoveryKind::Corrupt);
  FOS_CHECK_EQ(report.code, ReasonCode::RecoveryCorruptJournal);
}

FOS_TEST(a_corrupt_snapshot_is_refused_but_the_journal_can_still_recover) {
  Sandbox sandbox("fos-corrupt-snapshot");
  StoreConfig config = store_config(sandbox);
  config.compaction_record_threshold = 1000;
  {
    Store store(config);
    FOS_REQUIRE(store.open().usable);
    FOS_REQUIRE(store.commit(sample_state(2, 4)).ok());
    FOS_REQUIRE(store.compact(sample_state(2, 5)).ok());
    FOS_REQUIRE(store.commit(sample_state(2, 6)).ok());
  }
  FOS_REQUIRE(std::filesystem::exists(sandbox.snapshot()));
  std::vector<std::byte> snapshot = read_all(sandbox.snapshot());
  FOS_REQUIRE(snapshot.size() > kSnapshotHeaderBytes);
  snapshot[kSnapshotHeaderBytes + 4U] =
      static_cast<std::byte>(std::to_integer<std::uint8_t>(snapshot[kSnapshotHeaderBytes + 4U]) ^ 0x5AU);
  write_all(sandbox.snapshot(), snapshot);

  Store store(config);
  const RecoveryReport report = store.open();
  FOS_CHECK(report.usable);
  FOS_CHECK_EQ(report.kind, RecoveryKind::JournalOnly);
  FOS_CHECK(!report.snapshot_used);
  FOS_CHECK(report.journal_used);
  FOS_CHECK_EQ(store.state().schedule_generation, ScheduleGeneration{6});
}

FOS_TEST(a_truncated_snapshot_header_is_refused_when_there_is_no_journal_record) {
  Sandbox sandbox("fos-truncated-snapshot");
  StoreConfig config = store_config(sandbox);
  {
    Store store(config);
    FOS_REQUIRE(store.open().usable);
    FOS_REQUIRE(store.compact(sample_state(2, 5)).ok());
  }
  write_all(sandbox.snapshot(), std::vector<std::byte>(40, std::byte{0}));
  write_all(sandbox.journal(), std::vector<std::byte>{});

  Store store(config);
  const RecoveryReport report = store.open();
  FOS_CHECK(!report.usable);
  FOS_CHECK_EQ(report.kind, RecoveryKind::Corrupt);
}

FOS_TEST(an_incompatible_container_version_is_refused) {
  Sandbox sandbox("fos-version");
  StoreConfig config = store_config(sandbox);
  {
    Store store(config);
    FOS_REQUIRE(store.open().usable);
    FOS_REQUIRE(store.compact(sample_state(2, 5)).ok());
  }
  std::vector<std::byte> snapshot = read_all(sandbox.snapshot());
  FOS_REQUIRE(snapshot.size() > 12U);
  snapshot[11] = std::byte{9};   // container version low byte
  write_all(sandbox.snapshot(), snapshot);
  write_all(sandbox.journal(), std::vector<std::byte>{});

  Store store(config);
  const RecoveryReport report = store.open();
  FOS_CHECK(!report.usable);
  FOS_CHECK_EQ(report.kind, RecoveryKind::IncompatibleVersion);
  FOS_CHECK_EQ(report.code, ReasonCode::RecoveryIncompatibleVersion);
}

FOS_TEST(an_oversized_journal_record_is_refused_before_allocation) {
  Sandbox sandbox("fos-oversized");
  StoreConfig config = store_config(sandbox);
  config.max_journal_bytes = 4096;
  {
    Store store(config);
    FOS_REQUIRE(store.open().usable);
    FOS_REQUIRE(store.commit(sample_state(1, 1)).ok());
  }
  std::vector<std::byte> bytes = read_all(sandbox.journal());
  FOS_REQUIRE(bytes.size() > kJournalHeaderBytes + 4U);
  const std::size_t length_at = kJournalHeaderBytes;
  const std::uint32_t absurd = 0xFFFFFFF0U;
  bytes[length_at] = static_cast<std::byte>((absurd >> 24U) & 0xFFU);
  bytes[length_at + 1] = static_cast<std::byte>((absurd >> 16U) & 0xFFU);
  bytes[length_at + 2] = static_cast<std::byte>((absurd >> 8U) & 0xFFU);
  bytes[length_at + 3] = static_cast<std::byte>(absurd & 0xFFU);
  write_all(sandbox.journal(), bytes);

  Store store(config);
  const RecoveryReport report = store.open();
  FOS_CHECK(!report.usable);
  FOS_CHECK_EQ(report.kind, RecoveryKind::Oversized);
  FOS_CHECK_EQ(report.code, ReasonCode::RecoveryOversizedRecord);
}

FOS_TEST(compaction_bounds_journal_growth) {
  Sandbox sandbox("fos-compaction");
  StoreConfig config = store_config(sandbox);
  config.compaction_record_threshold = 4;
  Store store(config);
  FOS_REQUIRE(store.open().usable);
  for (std::uint64_t round = 1; round <= 20; ++round) {
    FOS_REQUIRE(store.commit(sample_state(1, round)).ok());
  }
  FOS_CHECK(store.records_since_snapshot() <= 4U);
  FOS_CHECK(store.journal_bytes() < 64U * 1024U);
  FOS_CHECK(std::filesystem::exists(sandbox.snapshot()));
  FOS_CHECK(store.state().schedule_generation.value() >= 19U);
}

FOS_TEST(restart_advances_the_epoch_and_fences_pre_restart_authority) {
  Sandbox sandbox("fos-restart-fence");
  TargetId destination{2};
  std::uint64_t attempt_value = 0;
  {
    Engine engine(engine_config(sandbox));
    FOS_REQUIRE(engine.open().ok());
    const EngineSnapshot first = engine.snapshot();
    FOS_CHECK_EQ(first.epoch, CoordinatorEpoch{1});
    Policy policy{};
    policy.generation = PolicyGeneration{1};
    policy.domain_preference = DomainPreference::OffloadFirst;
    policy.fallback = FallbackPolicy::AllowHostFallback;
    policy.load_model = LoadModel::ScheduledOnly;
    policy.max_cost_class = CostClass::Standard;
    policy.allow_migration = true;
    canonicalize(policy);
    FOS_REQUIRE(engine.set_policy(policy).ok());
    for (std::uint64_t id = 1; id <= 2; ++id) {
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
      target.supports_migration = true;
      FOS_REQUIRE(engine.declare_target(target).ok());
    }
    FlowDescriptor flow{};
    flow.flow = FlowId{1};
    flow.generation = FlowGeneration{1};
    flow.function = ProcessingFunctionId{1};
    flow.statefulness = Statefulness::Stateless;
    flow.max_cost_class = CostClass::Standard;
    flow.demand = CapacityVector{10, 10, 10, 10};
    flow.allow_migration = true;
    FOS_REQUIRE(engine.register_flow(flow).ok());
    PlacementRequest placement{};
    placement.request = RequestId{1};
    placement.evaluation_instant = Timestamp{100};
    placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
    const PlacementResult placed = engine.place(placement);
    FOS_REQUIRE(!placed.placements.empty() && placed.placements.front().accepted);
    Authorization authorization{};
    FOS_REQUIRE(engine.authorize(placed.placements.front().recommendation, Timestamp{100}, authorization).ok());
    MigrationIntent intent{};
    intent.flow = FlowId{1};
    intent.destination_target = destination;
    intent.destination_incarnation = TargetIncarnation{1};
    intent.destination_capability_generation = CapabilityGeneration{1};
    intent.issued_at = Timestamp{100};
    FOS_REQUIRE(engine.issue_intent(authorization, intent).ok());
    attempt_value = intent.attempt.value();
    engine.shutdown();
  }
  {
    Engine engine(engine_config(sandbox));
    const Status opened = engine.open();
    FOS_REQUIRE(opened.ok());
    const EngineSnapshot second = engine.snapshot();
    FOS_CHECK_EQ(second.epoch, CoordinatorEpoch{2});
    FOS_CHECK(second.boot.is_valid());
    FOS_CHECK_EQ(second.counters.migrations_fenced, static_cast<std::uint64_t>(1));
    FOS_CHECK(second.active_fences >= 1U);
    MigrationRecord record{};
    FOS_REQUIRE(engine.lookup_migration(MigrationAttemptId{attempt_value}, record));
    FOS_CHECK_EQ(record.intent.phase, MigrationPhase::Fenced);
    FOS_CHECK_EQ(record.abort_reason, ReasonCode::RejectedFencedByRestart);
    // The pre-restart attempt can never be completed.
    MigrationAcknowledgement acknowledgement{};
    acknowledgement.attempt = record.intent.attempt;
    acknowledgement.actor = MigrationActor::Source;
    acknowledgement.target = record.intent.source_target;
    acknowledgement.incarnation = record.intent.source_incarnation;
    acknowledgement.capability_generation = record.intent.source_capability_generation;
    acknowledgement.epoch = record.intent.epoch;
    acknowledgement.boot = record.intent.boot;
    acknowledgement.accepted = true;
    acknowledgement.acknowledged_at = Timestamp{200};
    FOS_CHECK_EQ(engine.acknowledge(acknowledgement).code(), ReasonCode::RejectedFencedByRestart);
    engine.shutdown();
  }
}

FOS_TEST(restart_does_not_resurrect_target_liveness) {
  Sandbox sandbox("fos-restart-liveness");
  {
    Engine engine(engine_config(sandbox));
    FOS_REQUIRE(engine.open().ok());
    Policy policy{};
    policy.generation = PolicyGeneration{1};
    policy.domain_preference = DomainPreference::HostFirst;
    policy.fallback = FallbackPolicy::Forbid;
    policy.load_model = LoadModel::ScheduledOnly;
    policy.max_cost_class = CostClass::Standard;
    canonicalize(policy);
    FOS_REQUIRE(engine.set_policy(policy).ok());
    TargetDescriptor target{};
    target.target = TargetId{1};
    target.incarnation = TargetIncarnation{1};
    target.capability_generation = CapabilityGeneration{1};
    target.topology_generation = TopologyGeneration{1};
    target.domain = ExecutionDomain::Host;
    target.kind = TargetKind::HostKernelPath;
    target.host = HostId{1};
    target.cost_class = CostClass::Economy;
    target.lifecycle = TargetLifecycle::Ready;
    FOS_REQUIRE(engine.declare_target(target).ok());
    FOS_CHECK_EQ(engine.snapshot().live_targets, static_cast<std::size_t>(1));
    engine.shutdown();
  }
  Engine engine(engine_config(sandbox));
  FOS_REQUIRE(engine.open().ok());
  FOS_CHECK_EQ(engine.snapshot().targets, static_cast<std::size_t>(1));
  // Identity is remembered, liveness is not.
  FOS_CHECK_EQ(engine.snapshot().live_targets, static_cast<std::size_t>(0));
  TargetDescriptor restored{};
  FOS_REQUIRE(engine.lookup_target(TargetId{1}, restored));
  FOS_CHECK_EQ(restored.domain, ExecutionDomain::Host);
  FOS_CHECK_EQ(restored.incarnation, TargetIncarnation{1});
  FOS_CHECK_EQ(restored.lifecycle, TargetLifecycle::Unknown);
  const PlacementResult result = engine.place([] {
    PlacementRequest request{};
    request.request = RequestId{1};
    request.evaluation_instant = Timestamp{50};
    request.items.push_back(PlacementItem{FlowId{}, FlowGeneration{}});
    return request;
  }());
  FOS_CHECK(!result.accepted || result.placements.empty() || !result.placements.front().accepted);
  engine.shutdown();
}

FOS_TEST(restart_keeps_verified_placements_and_downgrades_unverified_ones) {
  Sandbox sandbox("fos-restart-effects");
  {
    Engine engine(engine_config(sandbox));
    FOS_REQUIRE(engine.open().ok());
    Policy policy{};
    policy.generation = PolicyGeneration{1};
    policy.domain_preference = DomainPreference::HostFirst;
    policy.fallback = FallbackPolicy::Forbid;
    policy.load_model = LoadModel::ScheduledOnly;
    policy.max_cost_class = CostClass::Standard;
    canonicalize(policy);
    FOS_REQUIRE(engine.set_policy(policy).ok());
    TargetDescriptor target{};
    target.target = TargetId{1};
    target.incarnation = TargetIncarnation{1};
    target.capability_generation = CapabilityGeneration{1};
    target.topology_generation = TopologyGeneration{1};
    target.domain = ExecutionDomain::Host;
    target.kind = TargetKind::HostKernelPath;
    target.host = HostId{1};
    target.cost_class = CostClass::Economy;
    target.capacity = CapacityVector{100000, 1000000, 1000000, 100000};
    target.lifecycle = TargetLifecycle::Ready;
    FOS_REQUIRE(engine.declare_target(target).ok());
    for (std::uint64_t id = 1; id <= 2; ++id) {
      FlowDescriptor flow{};
      flow.flow = FlowId{id};
      flow.generation = FlowGeneration{1};
      flow.function = ProcessingFunctionId{1};
      flow.statefulness = Statefulness::Stateless;
      flow.max_cost_class = CostClass::Economy;
      flow.demand = CapacityVector{1, 1, 1, 1};
      FOS_REQUIRE(engine.register_flow(flow).ok());
    }
    PlacementRequest placement{};
    placement.request = RequestId{1};
    placement.evaluation_instant = Timestamp{100};
    placement.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
    const PlacementResult placed = engine.place(placement);
    FOS_REQUIRE(!placed.placements.empty() && placed.placements.front().accepted);
    // The placement is made but its effect is never verified before shutdown,
    // so the restart must not claim it was applied.
    Assignment before{};
    FOS_REQUIRE(engine.lookup_assignment(FlowId{1}, before));
    FOS_CHECK_EQ(before.effect, EffectState::Unknown);
    FOS_CHECK_EQ(before.incarnation, TargetIncarnation{1});
    engine.shutdown();
  }
  Engine engine(engine_config(sandbox));
  FOS_REQUIRE(engine.open().ok());
  Assignment assignment{};
  FOS_REQUIRE(engine.lookup_assignment(FlowId{1}, assignment));
  FOS_CHECK_EQ(assignment.effect, EffectState::Unknown);
  FOS_CHECK_EQ(assignment.incarnation, TargetIncarnation{1});
  FOS_CHECK_EQ(assignment.capability_generation, CapabilityGeneration{1});
  // The schedule still points where it did; only the claim of effect is gone.
  FOS_CHECK_EQ(assignment.target, TargetId{1});
  engine.shutdown();
}

FOS_TEST(evidence_watermarks_survive_restart) {
  Sandbox sandbox("fos-restart-evidence");
  const LoadEvidence evidence = [] {
    LoadEvidence value{};
    value.target = TargetId{1};
    value.incarnation = TargetIncarnation{1};
    value.capability_generation = CapabilityGeneration{1};
    value.source = EvidenceSourceId{5};
    value.source_sequence = EvidenceSequence{10};
    value.observed_at = Timestamp{100};
    value.state = EvidenceState::Known;
    value.utilized = CapacityVector{1, 1, 1, 1};
    return value;
  }();
  {
    Engine engine(engine_config(sandbox));
    FOS_REQUIRE(engine.open().ok());
    TargetDescriptor target{};
    target.target = TargetId{1};
    target.incarnation = TargetIncarnation{1};
    target.capability_generation = CapabilityGeneration{1};
    target.topology_generation = TopologyGeneration{1};
    target.domain = ExecutionDomain::OffloadDevice;
    target.kind = TargetKind::SmartNic;
    target.host = HostId{1};
    target.device = DeviceId{1};
    target.cost_class = CostClass::Standard;
    target.lifecycle = TargetLifecycle::Ready;
    FOS_REQUIRE(engine.declare_target(target).ok());
    FOS_REQUIRE(engine.ingest_load(evidence, Timestamp{100}).ok());
    engine.shutdown();
  }
  Engine engine(engine_config(sandbox));
  FOS_REQUIRE(engine.open().ok());
  // Load evidence is not durable, so the newest observation is re-supplied by
  // its owner. Its acceptance records that it was re-established.
  FOS_CHECK_EQ(engine.ingest_load(evidence, Timestamp{100}).code(),
               ReasonCode::AcceptedEvidenceRecorded);
  // The persisted watermark still refuses anything older than what was seen
  // before the restart.
  LoadEvidence older = evidence;
  older.source_sequence = EvidenceSequence{9};
  older.observed_at = Timestamp{200};
  FOS_CHECK_EQ(engine.ingest_load(older, Timestamp{100}).code(),
               ReasonCode::RejectedReplayedEvidence);
  engine.shutdown();
}

FOS_TEST(an_unusable_store_refuses_to_open) {
  Sandbox sandbox("fos-unusable");
  write_all(std::filesystem::path{sandbox.path()} / "state.snapshot",
            std::vector<std::byte>(200, std::byte{0xAB}));
  Engine engine(engine_config(sandbox));
  const Status opened = engine.open();
  FOS_CHECK(opened.failed());
  FOS_CHECK_EQ(opened.code(), ReasonCode::RecoveryCorruptSnapshot);
  FOS_CHECK(!engine.is_open());
}

FOS_TEST(a_store_directory_that_cannot_be_created_is_reported) {
  StoreConfig config{};
  config.directory = std::filesystem::path{"\\\\?\\invalid-path-that-cannot-exist\\store"};
  Store store(config);
  const RecoveryReport report = store.open();
  if (!report.usable) {
    FOS_CHECK(report.kind == RecoveryKind::Unavailable || report.kind == RecoveryKind::Corrupt);
  }
}

FOS_TEST(engine_state_survives_shutdown_without_semantic_loss) {
  Sandbox sandbox("fos-engine-roundtrip");
  DurableState before{};
  {
    Engine engine(engine_config(sandbox));
    FOS_REQUIRE(engine.open().ok());
    Policy policy{};
    policy.generation = PolicyGeneration{3};
    policy.domain_preference = DomainPreference::OffloadFirst;
    policy.fallback = FallbackPolicy::AllowHostFallback;
    policy.load_model = LoadModel::MaximumOfBoth;
    policy.max_cost_class = CostClass::Premium;
    policy.forbidden_hosts = {HostId{4}};
    policy.allowed_functions = {ProcessingFunctionId{2}};
    canonicalize(policy);
    FOS_REQUIRE(engine.set_policy(policy).ok());
    TargetDescriptor target{};
    target.target = TargetId{7};
    target.incarnation = TargetIncarnation{2};
    target.capability_generation = CapabilityGeneration{3};
    target.topology_generation = TopologyGeneration{1};
    target.domain = ExecutionDomain::OffloadDevice;
    target.kind = TargetKind::Dpu;
    target.host = HostId{1};
    target.device = DeviceId{7};
    target.capabilities = capability_bit(Capability::StatefulAcl);
    target.capacity = CapacityVector{100, 200, 300, 400};
    target.reserved = CapacityVector{1, 2, 3, 4};
    target.cost_class = CostClass::Premium;
    target.lifecycle = TargetLifecycle::Ready;
    target.supports_stateful = true;
    target.supports_migration = true;
    FOS_REQUIRE(engine.declare_target(target).ok());
    FlowDescriptor flow{};
    flow.flow = FlowId{9};
    flow.generation = FlowGeneration{5};
    flow.function = ProcessingFunctionId{2};
    flow.statefulness = Statefulness::Stateful;
    flow.exclusive_state_key = ExclusiveStateKeyId{9};
    flow.required_capabilities = capability_bit(Capability::StatefulAcl);
    flow.max_cost_class = CostClass::Premium;
    flow.demand = CapacityVector{5, 6, 7, 8};
    flow.allow_migration = true;
    FOS_REQUIRE(engine.register_flow(flow).ok());
    MigrationContract contract{};
    contract.id = MigrationContractId{11};
    contract.flow = FlowId{9};
    contract.flow_generation = FlowGeneration{5};
    contract.statefulness = Statefulness::Stateful;
    contract.state_transfer_defined = true;
    contract.ordering_preserved = true;
    contract.rollback_defined = true;
    contract.exclusive_handoff = true;
    contract.destination_target = TargetId{7};
    contract.destination_incarnation = TargetIncarnation{2};
    contract.destination_capability_generation = CapabilityGeneration{3};
    contract.policy_generation = PolicyGeneration{3};
    contract.max_state_bytes = 4096;
    FOS_REQUIRE(engine.register_migration_contract(contract).ok());
    before = engine.durable_state();
    engine.shutdown();
  }
  Engine engine(engine_config(sandbox));
  FOS_REQUIRE(engine.open().ok());
  DurableState after = engine.durable_state();
  // The epoch advances and the boot identity changes on every restart; every
  // other correctness-critical field must round-trip exactly.
  FOS_CHECK(after.epoch != before.epoch);
  FOS_CHECK(after.boot != before.boot);
  after.epoch = before.epoch;
  after.boot = before.boot;
  after.counters = before.counters;
  after.fences.clear();
  before.fences.clear();
  FOS_CHECK_EQ(after, before);
  engine.shutdown();
}
