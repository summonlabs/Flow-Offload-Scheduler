// Downstream consumer of the installed Flow Offload Scheduler package.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program uses only installed headers and the exported CMake target. It
// exercises the public surface end to end and returns non-zero if any step does
// not behave as documented.

#include <cstdio>
#include <string>
#include <thread>

#include "flow_offload/engine.hpp"
#include "flow_offload/export.hpp"
#include "flow_offload/service.hpp"

namespace {

int fail(const char* what, flow_offload::ReasonCode code) {
  std::fprintf(stderr, "consumer: %s failed: %s\n", what,
               std::string{flow_offload::to_string(code)}.c_str());
  return 1;
}

}  // namespace

int main() {
  using namespace flow_offload;

  std::printf("flow-offload-scheduler version %s\n", std::string{version_string()}.c_str());

  Engine engine;
  if (engine.open().failed()) {
    return fail("open", engine.open().code());
  }

  Policy policy{};
  policy.generation = PolicyGeneration{1};
  policy.domain_preference = DomainPreference::OffloadFirst;
  policy.fallback = FallbackPolicy::AllowHostFallback;
  policy.load_model = LoadModel::ScheduledOnly;
  policy.max_cost_class = CostClass::Standard;
  canonicalize(policy);
  const Status policy_status = engine.set_policy(policy);
  if (policy_status.failed()) {
    return fail("set_policy", policy_status.code());
  }

  TargetDescriptor host{};
  host.target = TargetId{1};
  host.incarnation = TargetIncarnation{1};
  host.capability_generation = CapabilityGeneration{1};
  host.topology_generation = TopologyGeneration{1};
  host.domain = ExecutionDomain::Host;
  host.kind = TargetKind::HostKernelPath;
  host.host = HostId{1};
  host.cost_class = CostClass::Economy;
  host.capacity = CapacityVector{1000000, 1000000000, 1048576, 1000000};
  host.lifecycle = TargetLifecycle::Ready;

  TargetDescriptor nic = host;
  nic.target = TargetId{2};
  nic.domain = ExecutionDomain::OffloadDevice;
  nic.kind = TargetKind::SmartNic;
  nic.device = DeviceId{7};
  nic.cost_class = CostClass::Standard;
  nic.capabilities = capability_bit(Capability::ConnectionTracking);
  nic.supports_stateful = true;
  nic.supports_migration = true;

  if (engine.declare_target(host).failed() || engine.declare_target(nic).failed()) {
    return fail("declare_target", ReasonCode::RejectedInternalInvariant);
  }

  FlowDescriptor flow{};
  flow.flow = FlowId{1};
  flow.generation = FlowGeneration{1};
  flow.function = ProcessingFunctionId{1};
  flow.statefulness = Statefulness::Stateless;
  flow.required_capabilities = capability_bit(Capability::ConnectionTracking);
  flow.max_cost_class = CostClass::Standard;
  flow.demand = CapacityVector{1000, 1000000, 0, 100};
  if (engine.register_flow(flow).failed()) {
    return fail("register_flow", ReasonCode::RejectedInternalInvariant);
  }

  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{1000};
  request.items.push_back(PlacementItem{FlowId{1}, FlowGeneration{}});
  const PlacementResult result = engine.place(request);
  if (!result.accepted || result.placements.empty() || !result.placements.front().accepted) {
    return fail("place", result.status);
  }
  if (result.placements.front().target != TargetId{2}) {
    std::fprintf(stderr, "consumer: expected the offload target to be chosen\n");
    return 1;
  }
  std::printf("placed flow 1 on target %s via %s\n",
              std::to_string(result.placements.front().target.value()).c_str(),
              std::string{to_string(result.placements.front().outcome)}.c_str());

  const std::string exported = export_json(engine.export_state());
  if (exported.find("summon.flow-offload.scheduler.export") == std::string::npos) {
    std::fprintf(stderr, "consumer: export did not carry its schema marker\n");
    return 1;
  }
  std::printf("exported %zu canonical bytes\n", exported.size());

  Explanation explanation{};
  if (engine.explain_flow(FlowId{1}, explanation).failed()) {
    return fail("explain_flow", ReasonCode::RejectedUnknownFlow);
  }
  std::printf("explanation: %s\n", render(explanation).c_str());

  // The transport surface is part of the same installed package.
  Service service(engine);
  if (service.start().failed()) {
    return fail("service start", ReasonCode::ProtocolIoError);
  }
  // The accept loop must be running before a client can be served.
  std::thread runner([&service] { service.run(); });
  Client client;
  if (client.connect("127.0.0.1", service.port()).failed()) {
    return fail("client connect", ReasonCode::ProtocolIoError);
  }
  HelloResponse hello{};
  if (client.hello("downstream-consumer", hello).failed() ||
      hello.protocol_version != kProtocolVersion) {
    return fail("client hello", ReasonCode::ProtocolMalformedFrame);
  }
  std::printf("service spoke protocol %u over port %u\n", hello.protocol_version, service.port());
  client.close();
  service.request_stop();
  runner.join();
  service.shutdown();
  engine.shutdown();
  std::printf("consumer completed successfully\n");
  return 0;
}
