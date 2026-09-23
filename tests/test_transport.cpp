// Flow Offload Scheduler - framed protocol and in-process service tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "flow_offload/export.hpp"
#include "flow_offload/service.hpp"
#include "harness.hpp"

using namespace flow_offload;

namespace {

Policy service_policy() {
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

TargetDescriptor service_target(std::uint64_t id) {
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

FlowDescriptor service_flow(std::uint64_t id) {
  FlowDescriptor flow{};
  flow.flow = FlowId{id};
  flow.generation = FlowGeneration{1};
  flow.function = ProcessingFunctionId{1};
  flow.statefulness = Statefulness::Stateless;
  flow.max_cost_class = CostClass::Standard;
  flow.demand = CapacityVector{10, 10, 10, 10};
  return flow;
}

/// Runs a service on a real loopback socket and returns a connected client.
class ServiceFixture {
 public:
  explicit ServiceFixture(Engine& engine) : service_(engine), engine_(&engine) {}

  bool start() {
    if (service_.start().failed()) {
      return false;
    }
    // The accept loop must be running before any client can be served.
    runner_ = std::thread([this] { service_.run(); });
    if (client_.connect("127.0.0.1", service_.port()).failed()) {
      return false;
    }
    HelloResponse hello{};
    return client_.hello("fostest", hello).ok() && hello.protocol_version == kProtocolVersion;
  }

  ~ServiceFixture() {
    service_.request_stop();
    if (runner_.joinable()) {
      runner_.join();
    }
    service_.shutdown();
  }

  Service& service() { return service_; }
  Client& client() { return client_; }

 private:
  Service service_;
  Client client_;
  std::thread runner_;
  Engine* engine_;
};

}  // namespace

FOS_TEST(frame_codec_rejects_every_malformed_shape) {
  Frame frame{};
  frame.operation = Operation::Place;
  frame.correlation = 42;
  frame.status = ReasonCode::ProtocolOk;
  frame.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  std::vector<std::byte> encoded;
  FOS_REQUIRE(encode_frame(frame, encoded, kMaxFrameBytes));
  Frame decoded{};
  FOS_REQUIRE(decode_frame(std::span<const std::byte>(encoded.data(), encoded.size()), decoded,
                           kMaxFrameBytes));
  FOS_CHECK_EQ(decoded.operation, Operation::Place);
  FOS_CHECK_EQ(decoded.correlation, static_cast<std::uint64_t>(42));
  FOS_CHECK_EQ(decoded.payload, frame.payload);

  // Truncated.
  Frame ignored{};
  FOS_CHECK(!decode_frame(std::span<const std::byte>(encoded.data(), encoded.size() - 1U), ignored,
                          kMaxFrameBytes));
  FOS_CHECK_EQ(ignored.status, ReasonCode::ProtocolTruncatedFrame);

  // Payload length larger than the frame.
  std::vector<std::byte> lying = encoded;
  lying[0] = std::byte{0};
  lying[1] = std::byte{0};
  lying[2] = std::byte{0};
  lying[3] = std::byte{99};
  FOS_CHECK(!decode_frame(std::span<const std::byte>(lying.data(), lying.size()), ignored,
                          kMaxFrameBytes));

  // Unsupported version.
  std::vector<std::byte> wrong_version = encoded;
  wrong_version[4] = std::byte{0};
  wrong_version[5] = std::byte{9};
  FOS_CHECK(!decode_frame(std::span<const std::byte>(wrong_version.data(), wrong_version.size()),
                          ignored, kMaxFrameBytes));
  FOS_CHECK_EQ(ignored.status, ReasonCode::ProtocolUnsupportedVersion);

  // Non-zero reserved field.
  std::vector<std::byte> reserved = encoded;
  reserved[20] = std::byte{1};
  FOS_CHECK(!decode_frame(std::span<const std::byte>(reserved.data(), reserved.size()), ignored,
                          kMaxFrameBytes));
  FOS_CHECK_EQ(ignored.status, ReasonCode::ProtocolMalformedFrame);

  // Unknown operation.
  std::vector<std::byte> unknown = encoded;
  unknown[6] = std::byte{200};
  FOS_CHECK(!decode_frame(std::span<const std::byte>(unknown.data(), unknown.size()), ignored,
                          kMaxFrameBytes));
  FOS_CHECK_EQ(ignored.status, ReasonCode::ProtocolUnknownOperation);

  // Damaged digest.
  std::vector<std::byte> damaged = encoded;
  damaged[encoded.size() - 1U] =
      static_cast<std::byte>(std::to_integer<std::uint8_t>(damaged[encoded.size() - 1U]) ^ 0xFFU);
  FOS_CHECK(!decode_frame(std::span<const std::byte>(damaged.data(), damaged.size()), ignored,
                          kMaxFrameBytes));
  FOS_CHECK_EQ(ignored.status, ReasonCode::ProtocolMalformedFrame);

  // Undefined reason code.
  std::vector<std::byte> bad_status = encoded;
  bad_status[16] = std::byte{0};
  bad_status[17] = std::byte{0};
  bad_status[18] = std::byte{0x7F};
  bad_status[19] = std::byte{0xFF};
  FOS_CHECK(!decode_frame(std::span<const std::byte>(bad_status.data(), bad_status.size()), ignored,
                          kMaxFrameBytes));

  // Oversized payload is refused before allocation.
  Frame huge{};
  huge.operation = Operation::Export;
  huge.payload.assign(kMaxFrameBytes, std::byte{0});
  FOS_CHECK(!encode_frame(huge, encoded, kMaxFrameBytes));
}

FOS_TEST(service_round_trips_the_full_control_surface_over_a_socket) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  ServiceFixture fixture(engine);
  FOS_REQUIRE(fixture.start());

  FOS_REQUIRE(fixture.client().set_policy(service_policy()).ok());
  FOS_REQUIRE(fixture.client().declare_target(service_target(1)).ok());
  FOS_REQUIRE(fixture.client().register_flow(service_flow(1)).ok());
  LoadEvidence evidence{};
  evidence.target = TargetId{1};
  evidence.incarnation = TargetIncarnation{1};
  evidence.capability_generation = CapabilityGeneration{1};
  evidence.source = EvidenceSourceId{9};
  evidence.source_sequence = EvidenceSequence{1};
  evidence.observed_at = Timestamp{50};
  evidence.state = EvidenceState::Known;
  evidence.utilized = CapacityVector{0, 0, 0, 0};
  FOS_REQUIRE(fixture.client().ingest_load(evidence, Timestamp{50}).ok());

  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{100};
  request.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  PlacementResult result{};
  FOS_REQUIRE(fixture.client().place(request, result).ok());
  FOS_CHECK(result.accepted);
  FOS_REQUIRE(result.placements.size() == 1U);
  FOS_CHECK_EQ(result.placements[0].outcome, ReasonCode::AcceptedOffloadPlacement);
  FOS_CHECK_EQ(result.placements[0].target, TargetId{1});

  Explanation explanation{};
  FOS_REQUIRE(fixture.client().explain(FlowId{1}, explanation).ok());
  FOS_CHECK_EQ(explanation.target, TargetId{1});

  std::string exported;
  FOS_REQUIRE(fixture.client().export_state(ExportFormat::Json, exported).ok());
  FOS_CHECK(exported.find("summon.flow-offload.scheduler.export") != std::string::npos);
  // The export produced over the wire is identical to the in-process export.
  FOS_CHECK_EQ(exported, export_json(engine.export_state()));

  // A second client sees the same authority: the service owns no state of its own.
  Client second;
  FOS_REQUIRE(second.connect("127.0.0.1", fixture.service().port()).ok());
  Explanation remote{};
  FOS_REQUIRE(second.explain(FlowId{1}, remote).ok());
  FOS_CHECK_EQ(remote.target, TargetId{1});
  second.close();

  const ServiceStats stats = fixture.service().stats();
  FOS_CHECK(stats.frames_handled >= 7U);
  FOS_CHECK_EQ(stats.frames_rejected, static_cast<std::uint64_t>(0));
}

FOS_TEST(adversarial_frames_do_not_break_the_service) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  ServiceFixture fixture(engine);
  FOS_REQUIRE(fixture.start());
  FOS_REQUIRE(fixture.client().set_policy(service_policy()).ok());

  // A frame whose digest does not match its contents is refused with an explicit
  // error frame rather than being silently dropped or accepted.
  Client attacker;
  FOS_REQUIRE(attacker.connect("127.0.0.1", fixture.service().port()).ok());
  Frame frame{};
  frame.operation = Operation::DeclareTarget;
  frame.correlation = 7;
  std::vector<std::byte> encoded;
  FOS_REQUIRE(encode_frame(frame, encoded, kMaxFrameBytes));
  encoded[encoded.size() - 1U] =
      static_cast<std::byte>(std::to_integer<std::uint8_t>(encoded[encoded.size() - 1U]) ^ 0x01U);
  std::vector<std::byte> response;
  FOS_REQUIRE(attacker.call_raw(std::span<const std::byte>(encoded.data(), encoded.size()), response)
                  .ok());
  Frame refusal{};
  FOS_REQUIRE(decode_frame(std::span<const std::byte>(response.data(), response.size()), refusal,
                           kMaxFrameBytes));
  FOS_CHECK_EQ(refusal.status, ReasonCode::ProtocolMalformedFrame);
  attacker.close();

  // An oversized declared length is refused before the payload is read.
  Client oversized;
  FOS_REQUIRE(oversized.connect("127.0.0.1", fixture.service().port()).ok());
  std::vector<std::byte> bogus(kFrameHeaderBytes, std::byte{0});
  bogus[0] = std::byte{0x7F};
  bogus[1] = std::byte{0xFF};
  bogus[2] = std::byte{0xFF};
  bogus[3] = std::byte{0xFF};
  bogus[4] = std::byte{0x00};
  bogus[5] = std::byte{0x01};
  bogus[6] = std::byte{0x0B};
  bogus[8] = std::byte{0x00};
  bogus[9] = std::byte{0x00};
  bogus[10] = std::byte{0x00};
  bogus[11] = std::byte{0x00};
  bogus[12] = std::byte{0x00};
  bogus[13] = std::byte{0x00};
  bogus[14] = std::byte{0x00};
  bogus[15] = std::byte{0x2A};
  FOS_REQUIRE(oversized.call_raw(std::span<const std::byte>(bogus.data(), bogus.size()), response)
                  .ok());
  Frame oversized_refusal{};
  FOS_REQUIRE(decode_frame(std::span<const std::byte>(response.data(), response.size()),
                           oversized_refusal, kMaxFrameBytes));
  FOS_CHECK_EQ(oversized_refusal.status, ReasonCode::ProtocolOversizedFrame);
  FOS_CHECK_EQ(oversized_refusal.correlation, static_cast<std::uint64_t>(42));
  oversized.close();

  // A partial frame is completed only by the client closing: the server treats
  // the truncated stream as a protocol error rather than waiting forever.
  Client truncated;
  FOS_REQUIRE(truncated.connect("127.0.0.1", fixture.service().port()).ok());
  Frame partial{};
  partial.operation = Operation::Explain;
  partial.correlation = 3;
  std::vector<std::byte> partial_bytes;
  FOS_REQUIRE(encode_frame(partial, partial_bytes, kMaxFrameBytes));
  partial_bytes.resize(kFrameHeaderBytes + 2U);
  FOS_REQUIRE(truncated.send_raw(
                  std::span<const std::byte>(partial_bytes.data(), partial_bytes.size()))
                  .ok());
  truncated.close();

  // The service is still healthy and still serving correct answers.
  Explanation explanation{};
  FOS_CHECK_EQ(fixture.client().explain(FlowId{1}, explanation).code(),
               ReasonCode::RejectedUnknownFlow);
  FOS_REQUIRE(fixture.client().declare_target(service_target(1)).ok());
  FOS_CHECK(fixture.service().stats().frames_rejected >= 2U);
  FOS_CHECK(fixture.client().connected());
}

FOS_TEST(concurrent_clients_see_a_consistent_authority) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  ServiceFixture fixture(engine);
  FOS_REQUIRE(fixture.start());
  FOS_REQUIRE(fixture.client().set_policy(service_policy()).ok());
  for (std::uint64_t id = 1; id <= 3; ++id) {
    FOS_REQUIRE(fixture.client().declare_target(service_target(id)).ok());
  }
  for (std::uint64_t id = 1; id <= 12; ++id) {
    FOS_REQUIRE(fixture.client().register_flow(service_flow(id)).ok());
  }
  for (std::uint64_t id = 1; id <= 3; ++id) {
    LoadEvidence evidence{};
    evidence.target = TargetId{id};
    evidence.incarnation = TargetIncarnation{1};
    evidence.capability_generation = CapabilityGeneration{1};
    evidence.source = EvidenceSourceId{100 + id};
    evidence.source_sequence = EvidenceSequence{1};
    evidence.observed_at = Timestamp{50};
    evidence.state = EvidenceState::Known;
    evidence.utilized = CapacityVector{0, 0, 0, 0};
    FOS_REQUIRE(fixture.client().ingest_load(evidence, Timestamp{50}).ok());
  }

  std::vector<std::string> exports(4);
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < exports.size(); ++index) {
    threads.emplace_back([&exports, index, &fixture] {
      Client client;
      if (client.connect("127.0.0.1", fixture.service().port()).failed()) {
        return;
      }
      PlacementRequest request{};
      request.request = RequestId{index + 1};
      request.evaluation_instant = Timestamp{100};
      request.items.push_back(PlacementItem{FlowId{1 + index}, FlowGeneration{}});
      PlacementResult result{};
      (void)client.place(request, result);
      (void)client.export_state(ExportFormat::Json, exports[index]);
      client.close();
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (const std::string& text : exports) {
    FOS_CHECK(!text.empty());
    FOS_CHECK(text.find("\"targets\"") != std::string::npos);
  }
}

FOS_TEST(a_shutdown_frame_stops_the_service) {
  Engine engine;
  FOS_REQUIRE(engine.open().ok());
  Service service(engine);
  FOS_REQUIRE(service.start().ok());
  const std::uint16_t port = service.port();
  // The accept loop must already be running: a shutdown frame is answered by a
  // connection handler, so a client that sent one before the loop started would
  // wait for a reply that only the loop can produce.
  std::thread runner([&service] { service.run(); });
  {
    Client client;
    const Status connected = client.connect("127.0.0.1", port);
    FOS_CHECK_MSG(connected.ok(), to_string(connected.code()));
    if (connected.ok()) {
      const Status shutdown_status = client.shutdown_server();
      FOS_CHECK_MSG(shutdown_status.ok(), to_string(shutdown_status.code()));
    }
  }
  // The thread must be joined before any early return: a joinable std::thread
  // destroyed without a join terminates the process.
  runner.join();
  FOS_CHECK(service.stopped());
  service.shutdown();
  FOS_CHECK(!service.started());

  // Starting again after shutdown works.
  const Status restarted = service.start();
  FOS_CHECK_MSG(restarted.ok(), to_string(restarted.code()));
  if (restarted.ok()) {
    std::thread second_runner([&service] { service.run(); });
    Client second;
    const Status connected = second.connect("127.0.0.1", service.port());
    FOS_CHECK_MSG(connected.ok(), to_string(connected.code()));
    if (connected.ok()) {
      HelloResponse hello{};
      FOS_CHECK(second.hello("fostest", hello).ok());
    }
    second.close();
    service.request_stop();
    second_runner.join();
  }
  service.shutdown();
}
