// Flow Offload Scheduler - core primitive tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "flow_offload/canonical.hpp"
#include "flow_offload/digest.hpp"
#include "flow_offload/serialize.hpp"
#include "harness.hpp"

using namespace flow_offload;

namespace {

std::string hex_of(std::string_view text) { return Sha256::of(text).hex(); }

/// Builds a byte vector from an explicit length so that embedded NUL bytes are
/// preserved; a string literal would stop at the first one.
std::vector<std::byte> bytes_of(const char* data, std::size_t size) {
  return std::vector<std::byte>(reinterpret_cast<const std::byte*>(data),
                                reinterpret_cast<const std::byte*>(data) + size);
}

}  // namespace

FOS_TEST(sha256_matches_published_vectors) {
  FOS_CHECK_EQ(hex_of(""),
               std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
  FOS_CHECK_EQ(hex_of("abc"),
               std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
  FOS_CHECK_EQ(
      hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"});
  std::string million(1000000, 'a');
  FOS_CHECK_EQ(hex_of(million),
               std::string{"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"});
}

FOS_TEST(sha256_incremental_matches_one_shot) {
  const std::string text = "flow offload scheduler incremental digest coverage";
  Sha256 hasher;
  for (std::size_t chunk = 1; chunk <= 8; ++chunk) {
    hasher = Sha256{};
    for (std::size_t offset = 0; offset < text.size(); offset += chunk) {
      const std::size_t take = std::min(chunk, text.size() - offset);
      hasher.update(std::string_view{text}.substr(offset, take));
    }
    FOS_CHECK_EQ(hasher.finish().hex(), Sha256::of(std::string_view{text}).hex());
  }
}

FOS_TEST(canonical_writer_is_big_endian_and_bounded) {
  CanonicalWriter writer(64);
  writer.u8(0x01U);
  writer.u16(0x0203U);
  writer.u32(0x04050607U);
  writer.u64(0x08090A0B0C0D0E0FU);
  FOS_CHECK(writer.ok());
  FOS_CHECK_EQ(writer.size(), static_cast<std::size_t>(15));
  const auto bytes = writer.bytes();
  FOS_CHECK_EQ(std::to_integer<std::uint8_t>(bytes[0]), static_cast<std::uint8_t>(0x01));
  FOS_CHECK_EQ(std::to_integer<std::uint8_t>(bytes[1]), static_cast<std::uint8_t>(0x02));
  FOS_CHECK_EQ(std::to_integer<std::uint8_t>(bytes[14]), static_cast<std::uint8_t>(0x0F));

  CanonicalWriter bounded(8);
  bounded.u64(1);
  bounded.u64(2);
  FOS_CHECK(!bounded.ok());
  FOS_CHECK_EQ(bounded.size(), static_cast<std::size_t>(8));
}

FOS_TEST(canonical_reader_rejects_truncated_oversized_and_malformed) {
  const char raw[] = {'\0', '\0', '\0', '\5', 'h', 'e', 'l', 'l', 'o'};
  const std::vector<std::byte> payload = bytes_of(raw, sizeof(raw));
  CanonicalReader good(std::span<const std::byte>(payload.data(), payload.size()));
  std::string text;
  FOS_CHECK(good.text(text, 16));
  FOS_CHECK_EQ(text, std::string{"hello"});
  FOS_CHECK(good.at_end());

  CanonicalReader truncated(std::span<const std::byte>(payload.data(), 6));
  std::string short_text;
  FOS_CHECK(!truncated.text(short_text, 16));
  FOS_CHECK(truncated.failed());
  FOS_CHECK_EQ(truncated.reason(), ReasonCode::RejectedTruncatedInput);

  CanonicalReader oversized(std::span<const std::byte>(payload.data(), payload.size()));
  std::string limited;
  FOS_CHECK(!oversized.text(limited, 4));
  FOS_CHECK_EQ(oversized.reason(), ReasonCode::RejectedOversizedInput);

  const char bool_raw[] = {'\2'};
  const std::vector<std::byte> bad_bool = bytes_of(bool_raw, sizeof(bool_raw));
  CanonicalReader boolean_reader(std::span<const std::byte>(bad_bool.data(), bad_bool.size()));
  bool flag = false;
  FOS_CHECK(!boolean_reader.boolean(flag));
  FOS_CHECK_EQ(boolean_reader.reason(), ReasonCode::RejectedMalformedInput);
}

FOS_TEST(reason_codes_round_trip_and_split_correctly) {
  FOS_CHECK(reason_code_count() > 100U);
  const ReasonCode samples[] = {ReasonCode::Ok,
                                ReasonCode::RejectedStaleCapabilityGeneration,
                                ReasonCode::AcceptedOffloadPlacement,
                                ReasonCode::RecoveryTornTailTruncated,
                                ReasonCode::ProtocolOversizedFrame,
                                ReasonCode::RejectedCancelled,
                                ReasonCode::RejectedInternalInvariant};
  for (const ReasonCode code : samples) {
    const std::string_view name = to_string(code);
    FOS_CHECK(name != std::string_view{"UnknownReasonCode"});
    ReasonCode parsed = ReasonCode::Ok;
    FOS_CHECK(reason_from_string(name, parsed));
    FOS_CHECK_EQ(parsed, code);
  }
  ReasonCode unknown = ReasonCode::Ok;
  FOS_CHECK(!reason_from_string("NotAReasonCode", unknown));
  FOS_CHECK(is_acceptance(ReasonCode::Ok));
  FOS_CHECK(is_acceptance(ReasonCode::AcceptedHostPlacement));
  FOS_CHECK(is_acceptance(ReasonCode::PersistCommitted));
  FOS_CHECK(is_acceptance(ReasonCode::RecoveryCleanReopen));
  FOS_CHECK(!is_acceptance(ReasonCode::RecoveryCorruptSnapshot));
  FOS_CHECK(!is_acceptance(ReasonCode::RejectedStalePolicyGeneration));
  FOS_CHECK(is_refusal(ReasonCode::RecoveryCorruptJournal));
  FOS_CHECK(is_refusal(ReasonCode::ProtocolMalformedFrame));
  // An undefined numeric value must never classify as success.
  FOS_CHECK(!is_acceptance(static_cast<ReasonCode>(7777)));
}

FOS_TEST(status_distinguishes_success_from_refusal) {
  FOS_CHECK(Status{}.ok());
  FOS_CHECK(Status::accepted(ReasonCode::AcceptedSchedulePublished).ok());
  FOS_CHECK(Status::refused(ReasonCode::RejectedStaleEvidence).failed());
  FOS_CHECK_EQ(Status::refused(ReasonCode::RejectedCancelled).to_string(),
               std::string{"failed:RejectedCancelled"});
}

FOS_TEST(checked_arithmetic_saturates_and_reports_overflow) {
  std::uint64_t sum = 0;
  FOS_CHECK(checked_add<std::uint64_t>(1, 2, sum));
  FOS_CHECK_EQ(sum, static_cast<std::uint64_t>(3));
  FOS_CHECK(!checked_add<std::uint64_t>(~0ULL, 1, sum));
  std::uint64_t product = 0;
  FOS_CHECK(checked_mul<std::uint64_t>(1ULL << 32U, 1ULL << 31U, product));
  FOS_CHECK_EQ(product, 1ULL << 63U);
  FOS_CHECK(!checked_mul<std::uint64_t>(~0ULL, 2, product));

  std::uint32_t ppm = 0;
  bool saturated = false;
  FOS_CHECK(!utilisation_ppm(1, 0, ppm, saturated));
  FOS_CHECK(utilisation_ppm(1, 2, ppm, saturated));
  FOS_CHECK_EQ(ppm, static_cast<std::uint32_t>(500000));
  FOS_CHECK(utilisation_ppm(~0ULL, ~0ULL, ppm, saturated));
  FOS_CHECK_EQ(ppm, static_cast<std::uint32_t>(1000000));
  FOS_CHECK(utilisation_ppm(~0ULL, 1, ppm, saturated));
  FOS_CHECK_EQ(ppm, kUtilisationPpmSaturated);
  FOS_CHECK(saturated);
}

FOS_TEST(capacity_vectors_compare_componentwise_and_check_overflow) {
  const CapacityVector small{1, 2, 3, 4};
  const CapacityVector large{2, 2, 3, 4};
  FOS_CHECK(!small.dominates(large));
  FOS_CHECK(large.dominates(small));
  FOS_CHECK(small.exceeds_any(large) == false);
  FOS_CHECK(large.exceeds_any(small));
  CapacityVector total{};
  FOS_CHECK(checked_add(small, large, total));
  FOS_CHECK_EQ(total.packets_per_second, static_cast<std::uint64_t>(3));
  CapacityVector overflow{};
  FOS_CHECK(!checked_add(CapacityVector::saturated(), CapacityVector{1, 0, 0, 0}, overflow));
  FOS_CHECK(!checked_sub(small, large, total));
  FOS_CHECK(checked_sub(large, small, total));
  FOS_CHECK_EQ(total.packets_per_second, static_cast<std::uint64_t>(1));
  const CapacityVector saturating = saturating_sub(small, large);
  FOS_CHECK(saturating.is_zero());
  std::uint32_t aggregate = 0;
  bool aggregate_saturated = false;
  FOS_CHECK(aggregate_utilisation_ppm(CapacityVector{5, 0, 0, 0}, CapacityVector{10, 0, 10, 10},
                                      aggregate, aggregate_saturated) == false);
  FOS_CHECK(aggregate_utilisation_ppm(CapacityVector{5, 5, 5, 5}, CapacityVector{10, 10, 10, 10},
                                      aggregate, aggregate_saturated));
  FOS_CHECK_EQ(aggregate, static_cast<std::uint32_t>(500000));
}

FOS_TEST(identities_are_distinct_types_with_invalid_defaults) {
  FlowId flow;
  FOS_CHECK(!flow.is_valid());
  flow = FlowId{7};
  FOS_CHECK(flow.is_valid());
  FOS_CHECK_EQ(flow.value(), static_cast<std::uint64_t>(7));
  FOS_CHECK(FlowId{1} < FlowId{2});
  FOS_CHECK_EQ(to_string(FlowId{42}), std::string{"42"});
  const BootId boot{0x0123456789ABCDEFULL, 0x0FEDCBA987654321ULL};
  FOS_CHECK(boot.is_valid());
  FOS_CHECK_EQ(to_string(boot), std::string{"0123456789abcdef0fedcba987654321"});
  FOS_CHECK(!BootId{}.is_valid());
}

FOS_TEST(capabilities_parse_render_and_reject_unknowns) {
  const CapabilityMask mask =
      capability_bit(Capability::ConnectionTracking) | capability_bit(Capability::VxlanEncap);
  const std::string rendered = render_capabilities(mask);
  FOS_CHECK_EQ(rendered, std::string{"connection-tracking,vxlan-encap"});
  CapabilityMask parsed = 0;
  FOS_CHECK(parse_capabilities(rendered, parsed));
  FOS_CHECK_EQ(parsed, mask);
  FOS_CHECK(!parse_capabilities("connection-tracking,not-a-capability", parsed));
  FOS_CHECK_EQ(parsed, static_cast<CapabilityMask>(0));
  FOS_CHECK(parse_capabilities("none", parsed));
  FOS_CHECK_EQ(parsed, static_cast<CapabilityMask>(0));
  FOS_CHECK_EQ(render_capabilities(0), std::string{"none"});
  FOS_CHECK(satisfies(mask, capability_bit(Capability::ConnectionTracking)));
  FOS_CHECK(!satisfies(mask, capability_bit(Capability::VxlanDecap)));
}

FOS_TEST(model_enumerations_render_stable_names) {
  FOS_CHECK_EQ(to_string(ExecutionDomain::OffloadDevice), std::string_view{"offload-device"});
  FOS_CHECK_EQ(to_string(TargetLifecycle::Draining), std::string_view{"draining"});
  FOS_CHECK_EQ(to_string(EffectState::Verified), std::string_view{"verified"});
  FOS_CHECK_EQ(to_string(MigrationPhase::DestinationAcknowledged),
               std::string_view{"destination-acknowledged"});
  FOS_CHECK_EQ(to_string(LoadModel::SumConservative), std::string_view{"sum-conservative"});
  FOS_CHECK_EQ(to_string(ExecutionDomain::Unknown), std::string_view{"unknown"});
}

FOS_TEST(serialization_round_trips_a_full_durable_state) {
  DurableState state{};
  state.epoch = CoordinatorEpoch{3};
  state.boot = BootId{11, 22};
  state.schedule_generation = ScheduleGeneration{4};
  state.policy_present = true;
  state.policy.generation = PolicyGeneration{2};
  state.policy.domain_preference = DomainPreference::OffloadFirst;
  state.policy.fallback = FallbackPolicy::AllowHostFallback;
  state.policy.load_model = LoadModel::SumConservative;
  state.policy.max_cost_class = CostClass::Standard;
  state.policy.allowed_domains = {ExecutionDomain::Host, ExecutionDomain::OffloadDevice};
  state.policy.forbidden_hosts = {HostId{9}};
  state.policy.forbidden_devices = {DeviceId{4}};
  state.policy.allowed_functions = {ProcessingFunctionId{2}};
  canonicalize(state.policy);

  FlowDescriptor flow{};
  flow.flow = FlowId{5};
  flow.generation = FlowGeneration{2};
  flow.function = ProcessingFunctionId{2};
  flow.statefulness = Statefulness::Stateful;
  flow.exclusive_state_key = ExclusiveStateKeyId{77};
  flow.required_capabilities = capability_bit(Capability::StatefulAcl);
  flow.demand = CapacityVector{10, 20, 30, 40};
  state.flows.push_back(flow);

  TargetDescriptor target{};
  target.target = TargetId{1};
  target.incarnation = TargetIncarnation{2};
  target.capability_generation = CapabilityGeneration{3};
  target.topology_generation = TopologyGeneration{4};
  target.domain = ExecutionDomain::OffloadDevice;
  target.kind = TargetKind::SmartNic;
  target.host = HostId{1};
  target.device = DeviceId{2};
  target.capabilities = capability_bit(Capability::StatefulAcl);
  target.capacity = CapacityVector{100, 200, 300, 400};
  target.reserved = CapacityVector{1, 2, 3, 4};
  target.cost_class = CostClass::Premium;
  target.lifecycle = TargetLifecycle::Ready;
  target.supports_stateful = true;
  target.supports_migration = true;
  target.source = EvidenceSourceId{3};
  target.source_sequence = EvidenceSequence{9};
  target.issued_at = Timestamp{1234};
  state.targets.push_back(target);

  Assignment assignment{};
  assignment.assignment = AssignmentId{8};
  assignment.flow = FlowId{5};
  assignment.flow_generation = FlowGeneration{2};
  assignment.target = TargetId{1};
  assignment.incarnation = TargetIncarnation{2};
  assignment.capability_generation = CapabilityGeneration{3};
  assignment.policy_generation = PolicyGeneration{2};
  assignment.schedule_generation = ScheduleGeneration{4};
  assignment.effect = EffectState::Verified;
  assignment.reason = ReasonCode::AcceptedOffloadPlacement;
  assignment.committed_at = Timestamp{4321};
  state.assignments.push_back(assignment);

  MigrationContract contract{};
  contract.id = MigrationContractId{6};
  contract.flow = FlowId{5};
  contract.flow_generation = FlowGeneration{2};
  contract.statefulness = Statefulness::Stateful;
  contract.state_transfer_defined = true;
  contract.ordering_preserved = true;
  contract.rollback_defined = true;
  contract.exclusive_handoff = true;
  contract.destination_target = TargetId{1};
  contract.destination_incarnation = TargetIncarnation{2};
  contract.destination_capability_generation = CapabilityGeneration{3};
  contract.policy_generation = PolicyGeneration{2};
  contract.max_state_bytes = 4096;
  state.contracts.push_back(contract);

  ReasonCount count{ReasonCode::RejectedCapacityInsufficient, 5};
  state.refusal_counts.push_back(count);
  state.counters.placements_accepted = 11;
  state.counters.history_evictions = 2;
  state.watermarks.push_back(SourceWatermark{EvidenceSourceId{3}, EvidenceSequence{9},
                                             CapabilityGeneration{3}, TargetId{1},
                                             TargetIncarnation{2}});
  state.canonicalize_order();

  CanonicalWriter writer;
  encode(writer, state);
  FOS_REQUIRE(writer.ok());
  DurableState decoded{};
  CanonicalReader reader(writer.bytes());
  FOS_REQUIRE(decode(reader, decoded));
  FOS_CHECK(reader.at_end());
  FOS_CHECK(!reader.failed());
  FOS_CHECK_EQ(decoded, state);
  FOS_CHECK_EQ(digest_of(decoded), digest_of(state));

  // A single flipped byte must be detected by the digest, not silently accepted.
  std::vector<std::byte> damaged(writer.bytes().begin(), writer.bytes().end());
  damaged[damaged.size() / 2] = static_cast<std::byte>(
      std::to_integer<std::uint8_t>(damaged[damaged.size() / 2]) ^ 0x01U);
  FOS_CHECK(Sha256::of(std::span<const std::byte>(damaged.data(), damaged.size())) != digest_of(state));
}

FOS_TEST(serialization_rejects_unknown_enum_values_and_unknown_capability_bits) {
  CanonicalWriter writer;
  writer.u64(1);              // flow
  writer.u64(1);              // generation
  writer.u64(1);              // function
  writer.u8(9);               // invalid Statefulness
  writer.u64(0);              // exclusive state key
  writer.u64(0);              // capability mask
  writer.u8(1);               // cost class
  encode(writer, CapacityVector{});
  writer.u8(0);               // locality
  writer.u64(0);              // anchor
  writer.boolean(false);
  writer.boolean(false);
  FOS_REQUIRE(writer.ok());
  CanonicalReader reader(writer.bytes());
  FlowDescriptor flow{};
  FOS_CHECK(!decode(reader, flow));
  FOS_CHECK_EQ(reader.reason(), ReasonCode::RejectedMalformedInput);

  // A capability bit that this runtime does not define can never be smuggled in
  // through a capability mask.
  CanonicalWriter mask_writer;
  mask_writer.u64(1);   // flow
  mask_writer.u64(1);   // generation
  mask_writer.u64(1);   // function
  mask_writer.u8(1);    // stateless
  mask_writer.u64(0);   // exclusive state key
  mask_writer.u64(1ULL << 40U);
  mask_writer.u8(1);    // cost class
  encode(mask_writer, CapacityVector{});
  mask_writer.u8(0);    // locality
  mask_writer.u64(0);   // anchor
  mask_writer.boolean(false);
  mask_writer.boolean(false);
  FOS_REQUIRE(mask_writer.ok());
  CanonicalReader second(mask_writer.bytes());
  FlowDescriptor masked{};
  FOS_CHECK(!decode(second, masked));
  FOS_CHECK_EQ(second.reason(), ReasonCode::RejectedUnknownCapability);
}

FOS_TEST(explanation_rendering_is_canonical_and_digestible) {
  Explanation explanation{};
  explanation.primary = ReasonCode::AcceptedOffloadPlacement;
  explanation.flow = FlowId{1};
  explanation.flow_generation = FlowGeneration{1};
  explanation.target = TargetId{2};
  explanation.epoch = CoordinatorEpoch{4};
  explanation.boot = BootId{1, 2};
  explanation.evaluation_instant = Timestamp{99};
  ExplanationStep step{};
  step.code = ReasonCode::AcceptedEvidenceRecorded;
  step.value_a = 5;
  step.value_b = 6;
  step.detail = "fresh load evidence";
  explanation.accepted.push_back(step);
  const std::string first = render(explanation);
  const std::string second = render(explanation);
  FOS_CHECK_EQ(first, second);
  FOS_CHECK(first.find("AcceptedOffloadPlacement") != std::string::npos);
  FOS_CHECK(first.find("AcceptedEvidenceRecorded") != std::string::npos);
  FOS_CHECK_EQ(digest_of(explanation), digest_of(explanation));
}
