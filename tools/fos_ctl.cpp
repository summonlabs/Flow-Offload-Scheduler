// Flow Offload Scheduler - inspection and control CLI.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "flow_offload/engine.hpp"
#include "flow_offload/export.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

using namespace flow_offload;

/// Hard-kills the process at a named durable boundary. It exists so that crash
/// behaviour is proved against a real process death rather than a simulated
/// exception. The tool documents it as a fault-injection option.
void crash_at_boundary(CrashPoint point, void* /*context*/) {
  std::fprintf(stdout, "CRASH %s\n", std::string{to_string(point)}.c_str());
  std::fflush(stdout);
  std::_Exit(137);
}

bool crash_point_from_name(std::string_view text, CrashPoint& out) {
  for (std::uint8_t value = 0; value <= 5U; ++value) {
    const auto point = static_cast<CrashPoint>(value);
    if (to_string(point) == text) {
      out = point;
      return true;
    }
  }
  return false;
}

void print_usage() {
  std::cout <<
      "fos_ctl - Flow Offload Scheduler inspection and control\n"
      "\n"
      "usage: fos_ctl [--store <dir>] <command> [options]\n"
      "\n"
      "commands:\n"
      "  status                 show coordinator identity, generations and counters\n"
      "  verify                 re-read the durable store and classify its integrity\n"
      "  export                 emit a canonical machine-readable export\n"
      "  declare-target         declare or re-declare an execution target\n"
      "  register-flow          register a flow processing request\n"
      "  ingest-load            supply target load evidence\n"
      "  set-policy             install a policy generation\n"
      "  register-contract      register a safe migration contract\n"
      "  place                  compute a placement decision\n"
      "  explain                explain the committed placement of a flow\n"
      "  rebalance              plan bounded rebalance migrations\n"
      "  begin-migration        authorise and issue one migration intent\n"
      "  complete-migration     acknowledge, apply and verify a migration attempt\n"
      "  seed-demo              build a deterministic demonstration scenario\n"
      "  demo-migration         seed the scenario and leave a migration in flight\n"
      "\n"
      "global options:\n"
      "  --store <dir>          enable durable persistence in <dir>\n"
      "  --format json|text|binary   export format (default json)\n"
      "  --help                 show this message\n";
}

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string> options;

  [[nodiscard]] bool has(const std::string& key) const { return options.find(key) != options.end(); }

  [[nodiscard]] std::string get(const std::string& key, const std::string& fallback) const {
    const auto it = options.find(key);
    return it == options.end() ? fallback : it->second;
  }

  [[nodiscard]] bool get_u64(const std::string& key, std::uint64_t& out) const {
    const auto it = options.find(key);
    if (it == options.end()) {
      return false;
    }
    try {
      std::size_t consumed = 0;
      const unsigned long long value = std::stoull(it->second, &consumed, 10);
      if (consumed != it->second.size()) {
        return false;
      }
      out = static_cast<std::uint64_t>(value);
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] std::uint64_t u64_or(const std::string& key, std::uint64_t fallback) const {
    std::uint64_t value = 0;
    return get_u64(key, value) ? value : fallback;
  }

  [[nodiscard]] std::uint32_t u32_or(const std::string& key, std::uint32_t fallback) const {
    std::uint64_t value = 0;
    if (!get_u64(key, value) || value > 0xFFFFFFFFULL) {
      return fallback;
    }
    return static_cast<std::uint32_t>(value);
  }
};

bool parse_args(int argc, char** argv, Args& out, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token.rfind("--", 0) == 0) {
      const std::string key = token.substr(2);
      if (key == "help" || key == "supports-stateful" || key == "supports-migration" ||
          key == "allow-migration" || key == "pinned" || key == "dry-run") {
        out.options[key] = "true";
        continue;
      }
      if (i + 1 >= argc) {
        error = "option --" + key + " requires a value";
        return false;
      }
      out.options[key] = argv[++i];
      continue;
    }
    out.positional.push_back(token);
  }
  return true;
}

int fail(const Status& status, const char* what) {
  std::cerr << "fos_ctl: " << what << " failed: " << to_string(status.code()) << "\n";
  return kExitFailure;
}

bool enum_from_name(std::string_view text, ExecutionDomain& out) {
  if (text == "host") {
    out = ExecutionDomain::Host;
    return true;
  }
  if (text == "offload-device" || text == "offload") {
    out = ExecutionDomain::OffloadDevice;
    return true;
  }
  return false;
}

bool kind_from_name(std::string_view text, TargetKind& out) {
  const std::pair<std::string_view, TargetKind> table[] = {
      {"host-kernel-path", TargetKind::HostKernelPath},
      {"host-userspace-path", TargetKind::HostUserspacePath},
      {"nic-hardware-offload", TargetKind::NicHardwareOffload},
      {"smartnic", TargetKind::SmartNic},
      {"dpu", TargetKind::Dpu}};
  for (const auto& entry : table) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

bool cost_from_name(std::string_view text, CostClass& out) {
  const std::pair<std::string_view, CostClass> table[] = {{"economy", CostClass::Economy},
                                                          {"standard", CostClass::Standard},
                                                          {"premium", CostClass::Premium},
                                                          {"scarce", CostClass::Scarce}};
  for (const auto& entry : table) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

bool lifecycle_from_name(std::string_view text, TargetLifecycle& out) {
  const std::pair<std::string_view, TargetLifecycle> table[] = {
      {"discovered", TargetLifecycle::Discovered}, {"ready", TargetLifecycle::Ready},
      {"degraded", TargetLifecycle::Degraded},     {"draining", TargetLifecycle::Draining},
      {"quiesced", TargetLifecycle::Quiesced},     {"removed", TargetLifecycle::Removed}};
  for (const auto& entry : table) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

bool preference_from_name(std::string_view text, DomainPreference& out) {
  const std::pair<std::string_view, DomainPreference> table[] = {
      {"host-first", DomainPreference::HostFirst},
      {"offload-first", DomainPreference::OffloadFirst},
      {"host-only", DomainPreference::HostOnly},
      {"offload-only", DomainPreference::OffloadOnly}};
  for (const auto& entry : table) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

bool fallback_from_name(std::string_view text, FallbackPolicy& out) {
  const std::pair<std::string_view, FallbackPolicy> table[] = {
      {"forbid", FallbackPolicy::Forbid},
      {"allow-host-fallback", FallbackPolicy::AllowHostFallback},
      {"allow-offload-fallback", FallbackPolicy::AllowOffloadFallback},
      {"allow-any-fallback", FallbackPolicy::AllowAnyFallback}};
  for (const auto& entry : table) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

bool load_model_from_name(std::string_view text, LoadModel& out) {
  const std::pair<std::string_view, LoadModel> table[] = {
      {"observed-only", LoadModel::ObservedOnly},
      {"scheduled-only", LoadModel::ScheduledOnly},
      {"sum-conservative", LoadModel::SumConservative},
      {"maximum-of-both", LoadModel::MaximumOfBoth}};
  for (const auto& entry : table) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

bool evidence_state_from_name(std::string_view text, EvidenceState& out) {
  const std::pair<std::string_view, EvidenceState> table[] = {
      {"known", EvidenceState::Known},
      {"unsupported", EvidenceState::Unsupported},
      {"conflicting", EvidenceState::Conflicting},
      {"stale", EvidenceState::Stale}};
  for (const auto& entry : table) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

TargetDescriptor build_target(const Args& args) {
  TargetDescriptor target{};
  target.target = TargetId{args.u64_or("target", 0)};
  target.incarnation = TargetIncarnation{args.u64_or("incarnation", 1)};
  target.capability_generation = CapabilityGeneration{args.u64_or("capability-generation", 1)};
  target.topology_generation = TopologyGeneration{args.u64_or("topology-generation", 1)};
  ExecutionDomain domain = ExecutionDomain::Host;
  (void)enum_from_name(args.get("domain", "host"), domain);
  target.domain = domain;
  TargetKind kind = domain == ExecutionDomain::Host ? TargetKind::HostKernelPath : TargetKind::SmartNic;
  (void)kind_from_name(args.get("kind", domain == ExecutionDomain::Host ? "host-kernel-path" : "smartnic"),
                       kind);
  target.kind = kind;
  target.host = HostId{args.u64_or("host", 1)};
  target.device = DeviceId{args.u64_or("device", 0)};
  CapabilityMask caps = 0;
  (void)parse_capabilities(args.get("caps", "none"), caps);
  target.capabilities = caps;
  CostClass cost = CostClass::Standard;
  (void)cost_from_name(args.get("cost", "standard"), cost);
  target.cost_class = cost;
  target.capacity = CapacityVector{args.u64_or("pps", 1000000), args.u64_or("bytes", 1000000000),
                                   args.u64_or("state-bytes", 1U << 20U),
                                   args.u64_or("entries", 1000000)};
  target.reserved = CapacityVector{args.u64_or("reserve-pps", 0), args.u64_or("reserve-bytes", 0),
                                   args.u64_or("reserve-state-bytes", 0),
                                   args.u64_or("reserve-entries", 0)};
  TargetLifecycle lifecycle = TargetLifecycle::Ready;
  (void)lifecycle_from_name(args.get("lifecycle", "ready"), lifecycle);
  target.lifecycle = lifecycle;
  target.supports_stateful = args.has("supports-stateful");
  target.supports_migration = args.has("supports-migration");
  target.source = EvidenceSourceId{args.u64_or("source", 1)};
  target.source_sequence = EvidenceSequence{args.u64_or("sequence", 1)};
  target.issued_at = Timestamp{args.u64_or("instant", 1)};
  return target;
}

FlowDescriptor build_flow(const Args& args) {
  FlowDescriptor flow{};
  flow.flow = FlowId{args.u64_or("flow", 0)};
  flow.generation = FlowGeneration{args.u64_or("generation", 1)};
  flow.function = ProcessingFunctionId{args.u64_or("function", 1)};
  flow.statefulness = args.get("statefulness", "stateless") == "stateful" ? Statefulness::Stateful
                                                                         : Statefulness::Stateless;
  flow.exclusive_state_key = ExclusiveStateKeyId{args.u64_or("state-key", 0)};
  CapabilityMask caps = 0;
  (void)parse_capabilities(args.get("caps", "none"), caps);
  flow.required_capabilities = caps;
  CostClass cost = CostClass::Scarce;
  (void)cost_from_name(args.get("cost", "scarce"), cost);
  flow.max_cost_class = cost;
  flow.demand = CapacityVector{args.u64_or("demand-pps", 1000), args.u64_or("demand-bytes", 1000000),
                               args.u64_or("demand-state-bytes", 0),
                               args.u64_or("demand-entries", 100)};
  const std::string locality = args.get("locality", "none");
  if (locality == "same-host-as-flow") {
    flow.locality = LocalityKind::SameHostAsFlow;
  } else if (locality == "same-device-as-flow") {
    flow.locality = LocalityKind::SameDeviceAsFlow;
  }
  flow.locality_anchor = FlowId{args.u64_or("anchor", 0)};
  flow.allow_migration = args.has("allow-migration");
  flow.pinned = args.has("pinned");
  return flow;
}

Policy build_policy(const Args& args) {
  Policy policy{};
  policy.generation = PolicyGeneration{args.u64_or("generation", 1)};
  DomainPreference preference = DomainPreference::OffloadFirst;
  (void)preference_from_name(args.get("preference", "offload-first"), preference);
  policy.domain_preference = preference;
  FallbackPolicy fallback = FallbackPolicy::AllowHostFallback;
  (void)fallback_from_name(args.get("fallback", "allow-host-fallback"), fallback);
  policy.fallback = fallback;
  LoadModel model = LoadModel::SumConservative;
  (void)load_model_from_name(args.get("load-model", "sum-conservative"), model);
  policy.load_model = model;
  CostClass cost = CostClass::Scarce;
  (void)cost_from_name(args.get("cost", "scarce"), cost);
  policy.max_cost_class = cost;
  CapabilityMask caps = 0;
  (void)parse_capabilities(args.get("caps", "none"), caps);
  policy.required_capabilities = caps;
  policy.reserve = CapacityVector{args.u64_or("reserve-pps", 0), args.u64_or("reserve-bytes", 0),
                                  args.u64_or("reserve-state-bytes", 0),
                                  args.u64_or("reserve-entries", 0)};
  policy.allow_placement_without_load_evidence = args.has("allow-missing-evidence");
  policy.sticky = args.get("sticky", "true") != "false";
  policy.allow_migration = args.has("allow-migration");
  policy.min_improvement_ppm = args.u32_or("min-improvement-ppm", 0);
  policy.max_rebalance_migrations = args.u32_or("max-rebalance", 64);
  canonicalize(policy);
  return policy;
}

MigrationContract build_contract(const Args& args) {
  MigrationContract contract{};
  contract.id = MigrationContractId{args.u64_or("contract", 1)};
  contract.flow = FlowId{args.u64_or("flow", 0)};
  contract.flow_generation = FlowGeneration{args.u64_or("flow-generation", 1)};
  contract.statefulness = args.get("statefulness", "stateful") == "stateless"
                              ? Statefulness::Stateless
                              : Statefulness::Stateful;
  contract.state_transfer_defined = true;
  contract.ordering_preserved = true;
  contract.rollback_defined = true;
  contract.exclusive_handoff = true;
  contract.destination_target = TargetId{args.u64_or("target", 0)};
  contract.destination_incarnation = TargetIncarnation{args.u64_or("incarnation", 1)};
  contract.destination_capability_generation =
      CapabilityGeneration{args.u64_or("capability-generation", 1)};
  contract.policy_generation = PolicyGeneration{args.u64_or("policy-generation", 1)};
  contract.max_state_bytes = args.u64_or("max-state-bytes", 0);
  return contract;
}

void print_status(const Engine& engine) {
  const EngineSnapshot snapshot = engine.snapshot();
  std::cout << "coordinator epoch=" << snapshot.epoch.value() << " boot=" << to_string(snapshot.boot)
            << "\n";
  std::cout << "generations policy=" << snapshot.policy_generation.value()
            << " schedule=" << snapshot.schedule_generation.value()
            << " topology=" << snapshot.topology_generation.value() << "\n";
  std::cout << "recovery kind=" << to_string(snapshot.recovery.kind)
            << " usable=" << (snapshot.recovery.usable ? "true" : "false")
            << " records_replayed=" << snapshot.recovery.records_replayed
            << " snapshot_used=" << (snapshot.recovery.snapshot_used ? "true" : "false")
            << " journal_used=" << (snapshot.recovery.journal_used ? "true" : "false") << "\n";
  std::cout << "entities flows=" << snapshot.flows << " targets=" << snapshot.targets
            << " live_targets=" << snapshot.live_targets << " assignments=" << snapshot.assignments
            << " migrations_in_flight=" << snapshot.migrations_in_flight
            << " active_fences=" << snapshot.active_fences << "\n";
  std::cout << "counters restarts=" << snapshot.counters.restarts
            << " placements_accepted=" << snapshot.counters.placements_accepted
            << " placements_refused=" << snapshot.counters.placements_refused
            << " migrations_issued=" << snapshot.counters.migrations_issued
            << " migrations_completed=" << snapshot.counters.migrations_completed
            << " migrations_fenced=" << snapshot.counters.migrations_fenced
            << " effects_verified=" << snapshot.counters.effects_verified
            << " persist_commits=" << snapshot.counters.persist_commits
            << " history_evictions=" << snapshot.counters.history_evictions << "\n";
}

int command_seed_demo(Engine& engine) {
  Policy policy{};
  policy.generation = PolicyGeneration{1};
  policy.domain_preference = DomainPreference::OffloadFirst;
  policy.fallback = FallbackPolicy::AllowHostFallback;
  policy.load_model = LoadModel::SumConservative;
  policy.max_cost_class = CostClass::Premium;
  policy.sticky = true;
  policy.allow_migration = true;
  policy.min_improvement_ppm = 100000;
  policy.max_rebalance_migrations = 8;
  canonicalize(policy);
  if (engine.set_policy(policy).failed()) {
    return kExitFailure;
  }
  for (std::uint64_t index = 1; index <= 2; ++index) {
    TargetDescriptor target{};
    target.target = TargetId{index};
    target.incarnation = TargetIncarnation{1};
    target.capability_generation = CapabilityGeneration{1};
    target.topology_generation = TopologyGeneration{1};
    target.domain = index == 1 ? ExecutionDomain::Host : ExecutionDomain::OffloadDevice;
    target.kind = index == 1 ? TargetKind::HostKernelPath : TargetKind::SmartNic;
    target.host = HostId{1};
    target.device = index == 1 ? DeviceId{0} : DeviceId{7};
    target.capabilities = capability_bit(Capability::ConnectionTracking) |
                          capability_bit(Capability::StatefulAcl) |
                          capability_bit(Capability::NatTranslation);
    target.capacity = CapacityVector{1000000, 1000000000, 1U << 20U, 1000000};
    target.cost_class = index == 1 ? CostClass::Economy : CostClass::Standard;
    target.lifecycle = TargetLifecycle::Ready;
    target.supports_stateful = true;
    target.supports_migration = true;
    target.source = EvidenceSourceId{1};
    target.source_sequence = EvidenceSequence{index};
    target.issued_at = Timestamp{1000};
    if (engine.declare_target(target).failed()) {
      return kExitFailure;
    }
    LoadEvidence load{};
    load.target = TargetId{index};
    load.incarnation = TargetIncarnation{1};
    load.capability_generation = CapabilityGeneration{1};
    load.source = EvidenceSourceId{2};
    load.source_sequence = EvidenceSequence{index};
    load.observed_at = Timestamp{1000};
    load.state = EvidenceState::Known;
    load.utilized = CapacityVector{100000, 100000000, 4096, 1000};
    if (engine.ingest_load(load, Timestamp{1000}).failed()) {
      return kExitFailure;
    }
  }
  for (std::uint64_t index = 1; index <= 3; ++index) {
    FlowDescriptor flow{};
    flow.flow = FlowId{index};
    flow.generation = FlowGeneration{1};
    flow.function = ProcessingFunctionId{1};
    flow.statefulness = index == 3 ? Statefulness::Stateful : Statefulness::Stateless;
    flow.exclusive_state_key = index == 3 ? ExclusiveStateKeyId{100} : ExclusiveStateKeyId{0};
    flow.required_capabilities = capability_bit(Capability::ConnectionTracking);
    flow.max_cost_class = CostClass::Standard;
    flow.demand = CapacityVector{1000, 1000000, index == 3 ? 65536U : 0U, 100};
    flow.allow_migration = true;
    if (engine.register_flow(flow).failed()) {
      return kExitFailure;
    }
  }
  PlacementRequest request{};
  request.request = RequestId{1};
  request.evaluation_instant = Timestamp{2000};
  for (std::uint64_t index = 1; index <= 3; ++index) {
    request.items.push_back(PlacementItem{FlowId{index}, FlowGeneration{1}});
  }
  const PlacementResult result = engine.place(request);
  std::cout << "seed-demo status=" << to_string(result.status)
            << " schedule_generation=" << result.schedule_generation.value() << "\n";
  for (const FlowPlacement& placement : result.placements) {
    std::cout << "  flow " << to_string(placement.flow) << " outcome=" << to_string(placement.outcome)
              << " target=" << to_string(placement.target) << "\n";
  }
  return result.accepted ? kExitOk : kExitFailure;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string error;
  if (!parse_args(argc, argv, args, error)) {
    std::cerr << "fos_ctl: " << error << "\n";
    print_usage();
    return kExitUsage;
  }
  if (args.positional.empty()) {
    print_usage();
    return args.has("help") ? kExitOk : kExitUsage;
  }
  const std::string command = args.positional.front();

  EngineConfig config{};
  if (args.has("store")) {
    config.persistence_enabled = true;
    config.store.directory = args.get("store", "");
  }
  if (command == "verify") {
    // Integrity inspection deliberately runs before any engine is opened: a
    // store that cannot be opened is exactly what this command must report.
    Store store(config.store);
    const RecoveryReport report = store.verify_integrity();
    std::cout << "integrity kind=" << to_string(report.kind) << " reason=" << to_string(report.code)
              << " usable=" << (report.usable ? "true" : "false")
              << " records_replayed=" << report.records_replayed
              << " bytes_truncated=" << report.bytes_truncated
              << " commit_sequence=" << report.commit_sequence << "\n";
    if (!report.detail.empty()) {
      std::cout << "detail: " << report.detail << "\n";
    }
    return report.usable ? kExitOk : kExitFailure;
  }
  if (args.has("crash-at")) {
    CrashPoint point = CrashPoint::BeforeJournalAppend;
    if (!crash_point_from_name(args.get("crash-at", ""), point)) {
      std::cerr << "fos_ctl: unknown crash point '" << args.get("crash-at", "") << "'\n";
      return kExitUsage;
    }
    config.crash_hook = &crash_at_boundary;
    config.crash_context = nullptr;
  }
  Engine engine(config);
  const Status opened = engine.open();
  if (opened.failed()) {
    std::cerr << "fos_ctl: engine could not open: " << to_string(opened.code()) << "\n";
    return kExitFailure;
  }

  if (command == "help") {
    print_usage();
    return kExitOk;
  }
  if (command == "status") {
    print_status(engine);
    return kExitOk;
  }
  if (command == "export") {
    const std::string format = args.get("format", "json");
    const EngineExport state = engine.export_state();
    if (format == "binary") {
      const std::string bytes = export_binary(state);
      std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      std::cout << "\n";
    } else if (format == "text") {
      std::cout << export_text(state);
    } else {
      std::cout << export_json(state);
    }
    return kExitOk;
  }
  if (command == "seed-demo") {
    return command_seed_demo(engine);
  }
  if (command == "demo-migration") {
    // Seeds the scenario and leaves one migration in flight, all inside this
    // process, so that a later process can prove what a restart does to it.
    if (command_seed_demo(engine) != kExitOk) {
      return kExitFailure;
    }
    const std::uint64_t flow = args.u64_or("flow", 1);
    const Timestamp instant{args.u64_or("instant", 2000)};
    PlacementRequest request{};
    request.request = RequestId{args.u64_or("request", 2)};
    request.evaluation_instant = instant;
    request.items.push_back(PlacementItem{FlowId{flow}, FlowGeneration{0}});
    const PlacementResult placed = engine.place(request);
    if (!placed.accepted || placed.placements.empty() || !placed.placements.front().accepted) {
      std::cerr << "fos_ctl: demo-migration placement refused\n";
      return kExitFailure;
    }
    Authorization authorization{};
    const Status authorized =
        engine.authorize(placed.placements.front().recommendation, instant, authorization);
    if (authorized.failed()) {
      return fail(authorized, "authorize");
    }
    MigrationIntent intent{};
    intent.flow = FlowId{flow};
    intent.destination_target = TargetId{args.u64_or("target", 1)};
    intent.destination_incarnation = TargetIncarnation{args.u64_or("incarnation", 1)};
    intent.destination_capability_generation =
        CapabilityGeneration{args.u64_or("capability-generation", 1)};
    intent.issued_at = instant;
    const Status issued = engine.issue_intent(authorization, intent);
    if (issued.failed()) {
      return fail(issued, "issue-intent");
    }
    std::cout << "attempt=" << intent.attempt.value() << " flow=" << to_string(intent.flow)
              << " from=" << to_string(intent.source_target)
              << " to=" << to_string(intent.destination_target)
              << " phase=" << to_string(intent.phase) << "\n";
    return kExitOk;
  }
  if (command == "declare-target") {
    return fail(engine.declare_target(build_target(args)), "declare-target");
  }
  if (command == "register-flow") {
    return fail(engine.register_flow(build_flow(args)), "register-flow");
  }
  if (command == "set-policy") {
    return fail(engine.set_policy(build_policy(args)), "set-policy");
  }
  if (command == "register-contract") {
    return fail(engine.register_migration_contract(build_contract(args)), "register-contract");
  }
  if (command == "ingest-load") {
    LoadEvidence evidence{};
    evidence.target = TargetId{args.u64_or("target", 0)};
    evidence.incarnation = TargetIncarnation{args.u64_or("incarnation", 1)};
    evidence.capability_generation = CapabilityGeneration{args.u64_or("capability-generation", 1)};
    evidence.source = EvidenceSourceId{args.u64_or("source", 1)};
    evidence.source_sequence = EvidenceSequence{args.u64_or("sequence", 1)};
    evidence.observed_at = Timestamp{args.u64_or("observed-at", 1)};
    EvidenceState state = EvidenceState::Known;
    (void)evidence_state_from_name(args.get("state", "known"), state);
    evidence.state = state;
    evidence.utilized = CapacityVector{args.u64_or("pps", 0), args.u64_or("bytes", 0),
                                       args.u64_or("state-bytes", 0), args.u64_or("entries", 0)};
    return fail(engine.ingest_load(evidence, Timestamp{args.u64_or("instant", 1)}), "ingest-load");
  }
  if (command == "place") {
    PlacementRequest request{};
    request.request = RequestId{args.u64_or("request", 1)};
    request.evaluation_instant = Timestamp{args.u64_or("instant", 1)};
    const std::uint64_t flow = args.u64_or("flow", 0);
    request.items.push_back(PlacementItem{FlowId{flow}, FlowGeneration{args.u64_or("generation", 0)}});
    request.dry_run = args.has("dry-run");
    const PlacementResult result = engine.place(request);
    for (const FlowPlacement& placement : result.placements) {
      std::cout << "flow " << to_string(placement.flow) << " outcome=" << to_string(placement.outcome)
                << " target=" << to_string(placement.target)
                << " incarnation=" << to_string(placement.incarnation)
                << " retained=" << (placement.retained ? "true" : "false")
                << " fallback=" << (placement.fallback ? "true" : "false") << "\n";
    }
    std::cout << "status=" << to_string(result.status)
              << " schedule_generation=" << result.schedule_generation.value() << "\n";
    return result.accepted ? kExitOk : kExitFailure;
  }
  if (command == "rebalance") {
    RebalanceRequest request{};
    request.request = RequestId{args.u64_or("request", 1)};
    request.evaluation_instant = Timestamp{args.u64_or("instant", 1)};
    request.max_migrations = args.u32_or("max", 0);
    request.dry_run = args.has("dry-run");
    const RebalanceReport report = engine.rebalance(request);
    std::cout << "bounded rebalance considered=" << report.considered << " eligible=" << report.eligible
              << " selected=" << report.selected << " truncated=" << report.truncated << "\n";
    for (const MigrationIntent& intent : report.intents) {
      std::cout << "  flow=" << to_string(intent.flow)
                << " from=" << to_string(intent.source_target)
                << " to=" << to_string(intent.destination_target) << "\n";
    }
    return kExitOk;
  }
  if (command == "explain") {
    Explanation explanation{};
    const Status status = engine.explain_flow(FlowId{args.u64_or("flow", 0)}, explanation);
    std::cout << render(explanation) << "\n";
    return status.ok() ? kExitOk : kExitFailure;
  }
  if (command == "begin-migration") {
    const std::uint64_t flow = args.u64_or("flow", 0);
    const Timestamp instant{args.u64_or("instant", 1)};
    PlacementRequest request{};
    request.request = RequestId{args.u64_or("request", 1)};
    request.evaluation_instant = instant;
    request.items.push_back(PlacementItem{FlowId{flow}, FlowGeneration{0}});
    const PlacementResult placed = engine.place(request);
    if (!placed.accepted || placed.placements.empty() || !placed.placements.front().accepted) {
      std::cerr << "fos_ctl: placement refused: "
                << (placed.placements.empty() ? std::string{"no placement"}
                                              : std::string{to_string(placed.placements.front().outcome)})
                << "\n";
      return kExitFailure;
    }
    Authorization authorization{};
    const Status authorized = engine.authorize(placed.placements.front().recommendation, instant, authorization);
    if (authorized.failed()) {
      return fail(authorized, "authorize");
    }
    MigrationIntent intent{};
    intent.flow = FlowId{flow};
    intent.destination_target = TargetId{args.u64_or("target", 0)};
    intent.destination_incarnation = TargetIncarnation{args.u64_or("incarnation", 1)};
    intent.destination_capability_generation =
        CapabilityGeneration{args.u64_or("capability-generation", 1)};
    intent.issued_at = instant;
    const Status issued = engine.issue_intent(authorization, intent);
    if (issued.failed()) {
      return fail(issued, "issue-intent");
    }
    std::cout << "attempt=" << intent.attempt.value() << " flow=" << to_string(intent.flow)
              << " from=" << to_string(intent.source_target)
              << " to=" << to_string(intent.destination_target)
              << " phase=" << to_string(intent.phase) << "\n";
    return kExitOk;
  }
  if (command == "complete-migration") {
    const MigrationAttemptId attempt{args.u64_or("attempt", 0)};
    MigrationRecord record{};
    if (!engine.lookup_migration(attempt, record)) {
      std::cerr << "fos_ctl: unknown attempt\n";
      return kExitFailure;
    }
    const Timestamp instant{args.u64_or("instant", 2)};
    MigrationAcknowledgement source{};
    source.attempt = attempt;
    source.actor = MigrationActor::Source;
    source.target = record.intent.source_target;
    source.incarnation = record.intent.source_incarnation;
    source.capability_generation = record.intent.source_capability_generation;
    source.epoch = record.intent.epoch;
    source.boot = record.intent.boot;
    source.accepted = true;
    source.reason = ReasonCode::Ok;
    source.acknowledged_at = instant;
    if (engine.acknowledge(source).failed()) {
      return kExitFailure;
    }
    MigrationAcknowledgement destination = source;
    destination.actor = MigrationActor::Destination;
    destination.target = record.intent.destination_target;
    destination.incarnation = record.intent.destination_incarnation;
    destination.capability_generation = record.intent.destination_capability_generation;
    if (engine.acknowledge(destination).failed()) {
      return kExitFailure;
    }
    AppliedEffect effect{};
    effect.attempt = attempt;
    effect.target = record.intent.destination_target;
    effect.incarnation = record.intent.destination_incarnation;
    effect.capability_generation = record.intent.destination_capability_generation;
    effect.epoch = record.intent.epoch;
    effect.boot = record.intent.boot;
    const std::string digest_text = "effect:" + std::to_string(attempt.value());
    effect.effect_digest = Sha256::of(std::string_view{digest_text});
    effect.state_bytes = args.u64_or("state-bytes", 4096);
    effect.applied_at = instant;
    if (engine.report_applied(effect).failed()) {
      return kExitFailure;
    }
    EffectObservation observation{};
    observation.flow = record.intent.flow;
    observation.flow_generation = record.intent.flow_generation;
    observation.target = effect.target;
    observation.incarnation = effect.incarnation;
    observation.capability_generation = effect.capability_generation;
    observation.source = EvidenceSourceId{9};
    observation.source_sequence = EvidenceSequence{attempt.value()};
    observation.state = EvidenceState::Known;
    observation.lifecycle = TargetLifecycle::Ready;
    observation.effect_digest = effect.effect_digest;
    observation.observed_at = instant;
    const Status verified = engine.observe(observation);
    if (verified.failed()) {
      return fail(verified, "observe");
    }
    std::cout << "attempt=" << attempt.value() << " verified phase="
              << to_string(MigrationPhase::Completed) << "\n";
    return kExitOk;
  }

  std::cerr << "fos_ctl: unknown command '" << command << "'\n";
  print_usage();
  return kExitUsage;
}
