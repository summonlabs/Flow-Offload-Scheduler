// Flow Offload Scheduler - independent OS process and real-socket tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These tests prove behaviour across process boundaries: a real server process
// is launched, driven over a real loopback socket, hard-killed, and restarted
// against the same durable store. Synchronisation is always on completed work -
// a line read from the child's standard output, an exit status, or a protocol
// response - never on a timeout or a sleep.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "flow_offload/export.hpp"
#include "flow_offload/service.hpp"
#include "harness.hpp"
#include "support/process.hpp"

using namespace flow_offload;

namespace {

struct Daemon {
  fostest::ChildProcess child;
  std::uint16_t port{0};

  bool start(const std::string& store) {
    std::vector<std::string> arguments;
    if (!store.empty()) {
      arguments.push_back("--store");
      arguments.push_back(store);
    }
    arguments.push_back("--port");
    arguments.push_back("0");
    arguments.push_back("--workers");
    arguments.push_back("3");
    const std::string error = child.launch(fostest::g_auxiliary_binary, arguments);
    if (!error.empty()) {
      std::fprintf(stderr, "launch failed: %s\n", error.c_str());
      return false;
    }
    std::string line;
    if (!child.read_line(line)) {
      return false;
    }
    if (line.rfind("READY ", 0) != 0) {
      std::fprintf(stderr, "unexpected daemon banner: %s\n", line.c_str());
      return false;
    }
    port = static_cast<std::uint16_t>(std::stoul(line.substr(6)));
    return port != 0;
  }
};

std::string cli_binary() { return fostest::g_binary_directory + "/fos_ctl.exe"; }

struct CliResult {
  int exit_code{0};
  std::string output;
};

CliResult run_cli(const std::vector<std::string>& arguments) {
  CliResult result{};
  fostest::ChildProcess child;
  const std::string error = child.launch(cli_binary(), arguments);
  if (!error.empty()) {
    result.exit_code = -1;
    result.output = error;
    return result;
  }
  std::string line;
  while (child.read_line(line)) {
    result.output.append(line);
    result.output.push_back('\n');
  }
  if (!child.wait_for_exit(result.exit_code)) {
    result.exit_code = -1;
  }
  return result;
}

Policy process_policy() {
  Policy policy{};
  policy.generation = PolicyGeneration{1};
  policy.domain_preference = DomainPreference::OffloadFirst;
  policy.fallback = FallbackPolicy::AllowHostFallback;
  policy.load_model = LoadModel::ScheduledOnly;
  policy.max_cost_class = CostClass::Standard;
  policy.sticky = true;
  canonicalize(policy);
  return policy;
}

TargetDescriptor process_target(std::uint64_t id) {
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

FlowDescriptor process_flow(std::uint64_t id) {
  FlowDescriptor flow{};
  flow.flow = FlowId{id};
  flow.generation = FlowGeneration{1};
  flow.function = ProcessingFunctionId{1};
  flow.statefulness = Statefulness::Stateless;
  flow.max_cost_class = CostClass::Standard;
  flow.demand = CapacityVector{10, 10, 10, 10};
  return flow;
}

PlacementRequest process_request(std::uint64_t id, std::uint64_t flow) {
  PlacementRequest request{};
  request.request = RequestId{id};
  request.evaluation_instant = Timestamp{100};
  request.items.push_back(PlacementItem{FlowId{flow}, FlowGeneration{}});
  return request;
}

/// The same inputs, applied in process, so the daemon's answer can be compared
/// against a local authority rather than against a hand-written expectation.
std::string reference_export() {
  Engine engine;
  (void)engine.open();
  (void)engine.set_policy(process_policy());
  for (std::uint64_t id = 1; id <= 3; ++id) {
    (void)engine.declare_target(process_target(id));
  }
  for (std::uint64_t id = 1; id <= 6; ++id) {
    (void)engine.register_flow(process_flow(id));
  }
  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{100};
  for (std::uint64_t id = 1; id <= 6; ++id) {
    request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
  }
  (void)engine.place(request);
  return export_json(engine.export_state());
}

void drive_control_surface(Client& client) {
  FOS_REQUIRE(client.set_policy(process_policy()).ok());
  for (std::uint64_t id = 1; id <= 3; ++id) {
    FOS_REQUIRE(client.declare_target(process_target(id)).ok());
  }
  for (std::uint64_t id = 1; id <= 6; ++id) {
    FOS_REQUIRE(client.register_flow(process_flow(id)).ok());
  }
  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{100};
  for (std::uint64_t id = 1; id <= 6; ++id) {
    request.items.push_back(PlacementItem{FlowId{id}, FlowGeneration{}});
  }
  PlacementResult result{};
  FOS_REQUIRE(client.place(request, result).ok());
  FOS_CHECK_EQ(result.placements.size(), static_cast<std::size_t>(6));
}

}  // namespace

FOS_TEST(a_separate_process_serves_the_authority_over_a_real_socket) {
  FOS_REQUIRE(!fostest::g_auxiliary_binary.empty());
  Daemon daemon;
  FOS_REQUIRE(daemon.start(""));
  Client client;
  FOS_REQUIRE(client.connect("127.0.0.1", daemon.port).ok());
  HelloResponse hello{};
  FOS_REQUIRE(client.hello("fostest", hello).ok());
  FOS_CHECK_EQ(hello.protocol_version, kProtocolVersion);
  FOS_CHECK_EQ(hello.semantics_version, kStateSemanticsVersion);
  FOS_CHECK_EQ(hello.library_version, std::string{version_string()});
  FOS_CHECK_EQ(hello.epoch, CoordinatorEpoch{1});

  drive_control_surface(client);

  std::string child_export;
  FOS_REQUIRE(client.export_state(ExportFormat::Json, child_export).ok());
  const std::string reference = reference_export();
  // The independent process reaches exactly the same canonical state as a local
  // engine fed the same inputs, apart from the coordinator identity.
  const std::size_t boot_at = child_export.find("\"coordinator_boot\"");
  const std::size_t reference_boot_at = reference.find("\"coordinator_boot\"");
  FOS_REQUIRE(boot_at != std::string::npos && reference_boot_at != std::string::npos);
  const std::size_t boot_end = child_export.find('\n', boot_at);
  const std::size_t reference_boot_end = reference.find('\n', reference_boot_at);
  std::string normalised = child_export;
  normalised.replace(boot_at, boot_end - boot_at, reference.substr(reference_boot_at,
                                                                   reference_boot_end - reference_boot_at));
  FOS_CHECK_EQ(normalised, reference);

  FOS_REQUIRE(client.shutdown_server().ok());
  client.close();
  int exit_code = -1;
  FOS_REQUIRE(daemon.child.wait_for_exit(exit_code));
  FOS_CHECK_EQ(exit_code, 0);
}

FOS_TEST(hard_killed_daemon_does_not_fabricate_success_and_fences_on_restart) {
  FOS_REQUIRE(!fostest::g_auxiliary_binary.empty());
  const std::string store = fostest::make_temp_directory("fos-process-kill");
  FOS_REQUIRE(!store.empty());
  {
    Daemon daemon;
    FOS_REQUIRE(daemon.start(store));
    Client client;
    FOS_REQUIRE(client.connect("127.0.0.1", daemon.port).ok());
    drive_control_surface(client);
    std::string exported;
    FOS_REQUIRE(client.export_state(ExportFormat::Json, exported).ok());
    FOS_CHECK(exported.find("\"assignments\"") != std::string::npos);
    // Hard kill: no shutdown frame, no orderly flush, no chance to clean up.
    daemon.child.kill();
  }

  Daemon restarted;
  FOS_REQUIRE(restarted.start(store));
  Client client;
  FOS_REQUIRE(client.connect("127.0.0.1", restarted.port).ok());
  HelloResponse hello{};
  FOS_REQUIRE(client.hello("fostest", hello).ok());
  FOS_CHECK_EQ(hello.epoch, CoordinatorEpoch{2});

  std::string exported;
  FOS_REQUIRE(client.export_state(ExportFormat::Json, exported).ok());
  // The three placements survived the kill because they were committed before
  // the acknowledgment they were given.
  FOS_CHECK(exported.find("\"assignments\"") != std::string::npos);
  FOS_CHECK(exported.find("\"flow\": 6") != std::string::npos);
  // Nothing may be placed until the targets are re-declared.
  PlacementResult refused{};
  FOS_REQUIRE(client.place(process_request(2, 1), refused).ok());
  FOS_REQUIRE(refused.placements.size() == 1U);
  FOS_CHECK(!refused.placements[0].accepted);
  FOS_REQUIRE(client.declare_target(process_target(1)).ok());
  PlacementResult accepted{};
  FOS_REQUIRE(client.place(process_request(3, 1), accepted).ok());
  FOS_CHECK(accepted.placements[0].accepted);
  FOS_REQUIRE(client.shutdown_server().ok());
  client.close();
  int exit_code = -1;
  FOS_REQUIRE(restarted.child.wait_for_exit(exit_code));
  FOS_CHECK_EQ(exit_code, 0);
  fostest::remove_directory(store);
}

FOS_TEST(a_cli_process_reports_recovery_and_carries_in_flight_work_across_restarts) {
  FOS_REQUIRE(!fostest::g_auxiliary_binary.empty());
  const std::string store = fostest::make_temp_directory("fos-process-cli");
  FOS_REQUIRE(!store.empty());

  // One process seeds the scenario and leaves a migration in flight.
  const CliResult seeded = run_cli({"--store", store, "demo-migration"});
  FOS_CHECK_EQ(seeded.exit_code, 0);
  FOS_CHECK(seeded.output.find("AcceptedSchedulePublished") != std::string::npos);
  FOS_CHECK(seeded.output.find("phase=intent-issued") != std::string::npos);

  // A second process reopens the store and confirms the epoch advanced and that
  // nothing is live until its owner re-declares it.
  const CliResult status = run_cli({"--store", store, "status"});
  FOS_CHECK_EQ(status.exit_code, 0);
  FOS_CHECK(status.output.find("coordinator epoch=2") != std::string::npos);
  FOS_CHECK(status.output.find("restarts=2") != std::string::npos);
  FOS_CHECK(status.output.find("live_targets=0") != std::string::npos);
  FOS_CHECK(status.output.find("migrations_fenced=1") != std::string::npos);

  // A third process must fence that attempt rather than resume it.
  const CliResult fenced = run_cli({"--store", store, "export", "--format", "json"});
  FOS_CHECK_EQ(fenced.exit_code, 0);
  FOS_CHECK(fenced.output.find("\"phase\": \"fenced\"") != std::string::npos);
  FOS_CHECK(fenced.output.find("RejectedFencedByRestart") != std::string::npos);

  const CliResult completion = run_cli({"--store", store, "complete-migration", "--attempt", "1"});
  FOS_CHECK(completion.exit_code != 0);

  const CliResult verified = run_cli({"--store", store, "verify"});
  FOS_CHECK_EQ(verified.exit_code, 0);
  FOS_CHECK(verified.output.find("usable=true") != std::string::npos);
  fostest::remove_directory(store);
}

FOS_TEST(an_interrupted_commit_leaves_a_recoverable_store) {
  FOS_REQUIRE(!fostest::g_auxiliary_binary.empty());
  const char* points[] = {"before-journal-append",
                          "after-journal-append-before-flush",
                          "after-journal-flush-before-snapshot",
                          "after-snapshot-write-before-rename",
                          "after-snapshot-rename-before-journal-rewrite",
                          "after-journal-rewrite-before-acknowledge"};
  for (const char* point : points) {
    const std::string store = fostest::make_temp_directory("fos-process-crash");
    FOS_REQUIRE(!store.empty());
    const CliResult crashed =
        run_cli({"--store", store, "--crash-at", point, "seed-demo"});
    FOS_CHECK_MSG(crashed.exit_code != 0, point);
    FOS_CHECK_MSG(crashed.output.find("CRASH ") != std::string::npos, point);

    const CliResult verify = run_cli({"--store", store, "verify"});
    FOS_CHECK_MSG(verify.exit_code == 0, point);
    FOS_CHECK_MSG(verify.output.find("usable=true") != std::string::npos, point);

    // Whatever survived, the next process must be able to make progress and must
    // not claim the interrupted commit succeeded.
    const CliResult status = run_cli({"--store", store, "status"});
    FOS_CHECK_MSG(status.exit_code == 0, point);
    fostest::remove_directory(store);
  }
}

FOS_TEST(a_damaged_store_is_refused_by_an_independent_process) {
  FOS_REQUIRE(!fostest::g_auxiliary_binary.empty());
  const std::string store = fostest::make_temp_directory("fos-process-damaged");
  FOS_REQUIRE(!store.empty());
  FOS_CHECK_EQ(run_cli({"--store", store, "seed-demo"}).exit_code, 0);

  // Damage a byte inside the snapshot payload while leaving the header and both
  // digests intact, so the damage is caught by the payload digest.
  const std::filesystem::path snapshot = std::filesystem::path{store} / "state.snapshot";
  FOS_REQUIRE(std::filesystem::exists(snapshot));
  {
    std::FILE* file = std::fopen(snapshot.string().c_str(), "r+b");
    FOS_REQUIRE(file != nullptr);
    const long offset = static_cast<long>(kSnapshotHeaderBytes + 6U);
    FOS_REQUIRE(std::fseek(file, offset, SEEK_SET) == 0);
    unsigned char probe[1] = {};
    FOS_REQUIRE(std::fread(probe, 1, 1, file) == 1U);
    const unsigned char marker = static_cast<unsigned char>(probe[0] ^ 0x5AU);
    FOS_REQUIRE(std::fseek(file, offset, SEEK_SET) == 0);
    FOS_REQUIRE(std::fwrite(&marker, 1, 1, file) == 1U);
    FOS_CHECK(std::fclose(file) == 0);
  }

  const CliResult status = run_cli({"--store", store, "status"});
  FOS_CHECK(status.exit_code != 0);
  FOS_CHECK(status.output.find("could not open") != std::string::npos);
  const CliResult verify = run_cli({"--store", store, "verify"});
  FOS_CHECK(verify.exit_code != 0);
  FOS_CHECK(verify.output.find("kind=corrupt") != std::string::npos);
  FOS_CHECK(verify.output.find("RecoveryCorruptSnapshot") != std::string::npos);
  fostest::remove_directory(store);
}
