// Flow Offload Scheduler - stable reason codes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_REASON_HPP
#define FLOW_OFFLOAD_REASON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace flow_offload {

/// Stable numeric reason codes. The numeric value is part of the machine-readable
/// contract: it is persisted, exported, and transported. Values are never reused
/// for a different meaning.
///
///  0        success
///  100-149  input validation
///  120-139  generation / evidence semantics
///  150-199  eligibility and policy constraints
///  200-219  capacity accounting
///  250-279  authority, epochs, fencing
///  300-329  migration lifecycle
///  400-449  acceptance
///  500-529  persistence and recovery
///  600-619  transport protocol
///  650-669  cancellation and shutdown
///  900-929  resources and internal invariants
#define FLOW_OFFLOAD_REASON_CODES(X)                                        \
  X(Ok, 0)                                                                  \
  X(RejectedMalformedInput, 100)                                            \
  X(RejectedOversizedInput, 101)                                            \
  X(RejectedTruncatedInput, 102)                                            \
  X(RejectedCorruptInput, 103)                                              \
  X(RejectedDuplicateIdentity, 104)                                         \
  X(RejectedConflictingDuplicate, 105)                                      \
  X(RejectedUnknownFlow, 106)                                               \
  X(RejectedUnknownTarget, 107)                                             \
  X(RejectedUnknownFunction, 108)                                           \
  X(RejectedUnknownCapability, 109)                                         \
  X(RejectedEmptyRequest, 110)                                              \
  X(RejectedTooManyEntries, 111)                                            \
  X(RejectedUnsupportedVersion, 112)                                        \
  X(RejectedNonCanonicalEncoding, 113)                                      \
  X(RejectedStaleFlowGeneration, 120)                                       \
  X(RejectedStaleCapabilityGeneration, 121)                                 \
  X(RejectedStaleTopologyGeneration, 122)                                   \
  X(RejectedStalePolicyGeneration, 123)                                     \
  X(RejectedStaleScheduleGeneration, 124)                                   \
  X(RejectedStaleIncarnation, 125)                                          \
  X(RejectedStaleEvidence, 126)                                             \
  X(RejectedFutureGeneration, 127)                                          \
  X(RejectedMissingEvidence, 130)                                           \
  X(RejectedReplayedEvidence, 131)                                          \
  X(RejectedOutOfOrderEvidence, 132)                                        \
  X(RejectedConflictingEvidence, 133)                                       \
  X(RejectedUnknownEvidenceSource, 134)                                     \
  X(RejectedUnsupportedTarget, 150)                                         \
  X(RejectedUnsupportedFunction, 151)                                       \
  X(RejectedUnsupportedCapability, 152)                                     \
  X(RejectedTargetLifecycleUnknown, 153)                                    \
  X(RejectedTargetNotReady, 154)                                            \
  X(RejectedTargetDraining, 155)                                            \
  X(RejectedTargetRemoved, 156)                                             \
  X(RejectedTargetDegraded, 157)                                            \
  X(RejectedTargetQuiesced, 158)                                            \
  X(RejectedPolicyForbidsDomain, 160)                                       \
  X(RejectedPolicyForbidsHost, 161)                                         \
  X(RejectedPolicyForbidsDevice, 162)                                       \
  X(RejectedPolicyCostClass, 163)                                           \
  X(RejectedPolicyFunctionNotAllowed, 164)                                  \
  X(RejectedPolicyHeadroom, 165)                                            \
  X(RejectedNoEligibleTarget, 166)                                          \
  X(RejectedIncompatibleTarget, 167)                                        \
  X(RejectedAffinityUnsatisfiable, 168)                                     \
  X(RejectedCapacityInsufficient, 200)                                      \
  X(RejectedCapacityUnknown, 201)                                           \
  X(RejectedCapacityOverSubscribed, 202)                                    \
  X(RejectedCapacityOverflow, 203)                                          \
  X(RejectedFenceActive, 250)                                               \
  X(RejectedFencedByRestart, 251)                                           \
  X(RejectedFencedBySupersession, 252)                                      \
  X(RejectedStaleCoordinatorEpoch, 253)                                     \
  X(RejectedStaleBootId, 254)                                               \
  X(RejectedAuthorizationMissing, 255)                                      \
  X(RejectedAuthorizationSuperseded, 256)                                   \
  X(RejectedConflictingAuthority, 257)                                      \
  X(RejectedExclusiveStateConflict, 258)                                    \
  X(RejectedRecommendationMismatch, 259)                                    \
  X(RejectedStatefulMigrationUnsafe, 300)                                   \
  X(RejectedMigrationContractMissing, 301)                                  \
  X(RejectedMigrationContractMismatch, 302)                                 \
  X(RejectedMigrationAlreadyInFlight, 303)                                  \
  X(RejectedMigrationNotPermitted, 304)                                     \
  X(RejectedMigrationSourceBusy, 305)                                       \
  X(RejectedMigrationAttemptUnknown, 306)                                   \
  X(RejectedMigrationAttemptTerminal, 307)                                  \
  X(RejectedMigrationAckOutOfOrder, 308)                                    \
  X(RejectedMigrationAckDuplicate, 309)                                     \
  X(RejectedMigrationEffectBeforeAck, 310)                                  \
  X(RejectedMigrationVerificationMismatch, 311)                             \
  X(RejectedMigrationSameTarget, 312)                                       \
  X(RejectedMigrationLimitReached, 313)                                     \
  X(RejectedMigrationFenced, 314)                                           \
  X(AcceptedHostPlacement, 400)                                             \
  X(AcceptedOffloadPlacement, 401)                                          \
  X(AcceptedStickyRetention, 402)                                           \
  X(AcceptedFallbackToHost, 403)                                            \
  X(AcceptedFallbackToOffload, 404)                                         \
  X(AcceptedWithoutLoadEvidence, 405)                                       \
  X(AcceptedNoChangeRequired, 406)                                          \
  X(AcceptedRecommendationIssued, 407)                                      \
  X(AcceptedAuthorizationGranted, 408)                                      \
  X(AcceptedMigrationIntentIssued, 409)                                     \
  X(AcceptedSourceAckRecorded, 410)                                         \
  X(AcceptedDestinationAckRecorded, 411)                                    \
  X(AcceptedAppliedEffectRecorded, 412)                                     \
  X(AcceptedVerifiedEffectRecorded, 413)                                    \
  X(AcceptedMigrationCompleted, 414)                                        \
  X(AcceptedMigrationAborted, 415)                                          \
  X(AcceptedRebalanceBounded, 416)                                          \
  X(AcceptedIdempotentReplay, 417)                                          \
  X(AcceptedTargetRegistered, 418)                                          \
  X(AcceptedTargetUpdated, 419)                                             \
  X(AcceptedTargetRemoved, 420)                                             \
  X(AcceptedEvidenceRecorded, 421)                                          \
  X(AcceptedPolicyApplied, 422)                                             \
  X(AcceptedFlowRegistered, 423)                                            \
  X(AcceptedFlowUpdated, 424)                                               \
  X(AcceptedCancelled, 425)                                                 \
  X(AcceptedDrained, 426)                                                   \
  X(AcceptedSchedulePublished, 427)                                         \
  X(AcceptedMigrationSuperseded, 428)                                       \
  X(RecoveryCleanReopen, 500)                                               \
  X(RecoveryTornTailTruncated, 501)                                        \
  X(RecoveryJournalReplayed, 502)                                           \
  X(RecoverySnapshotOnly, 503)                                              \
  X(RecoveryJournalOnly, 504)                                               \
  X(RecoveryEmptyStore, 505)                                                \
  X(RecoveryIncompatibleVersion, 506)                                       \
  X(RecoverySemanticsMismatch, 507)                                         \
  X(RecoveryCorruptSnapshot, 508)                                           \
  X(RecoveryCorruptJournal, 509)                                            \
  X(RecoveryTruncatedSnapshot, 510)                                         \
  X(RecoveryOversizedRecord, 511)                                           \
  X(RecoveryFencedPreRestartAuthority, 512)                                 \
  X(RecoveryEpochAdvanced, 513)                                             \
  X(RecoveryCompacted, 514)                                                 \
  X(RecoveryEvidenceDiscarded, 515)                                         \
  X(PersistCommitted, 520)                                                  \
  X(PersistCommitFailed, 521)                                               \
  X(PersistNotConfigured, 522)                                              \
  X(PersistRotationRequired, 523)                                           \
  X(ProtocolOk, 600)                                                        \
  X(ProtocolUnsupportedVersion, 601)                                        \
  X(ProtocolMalformedFrame, 602)                                            \
  X(ProtocolOversizedFrame, 603)                                            \
  X(ProtocolUnknownOperation, 604)                                          \
  X(ProtocolBackpressure, 605)                                              \
  X(ProtocolShuttingDown, 606)                                              \
  X(ProtocolConnectionLimit, 607)                                           \
  X(ProtocolTruncatedFrame, 608)                                            \
  X(ProtocolIoError, 609)                                                   \
  X(ProtocolNotConnected, 610)                                              \
  X(RejectedCancelled, 650)                                                 \
  X(RejectedShuttingDown, 651)                                              \
  X(AcceptedShutdownRequested, 652)                                         \
  X(RejectedResourceExhausted, 900)                                         \
  X(RejectedQueueFull, 901)                                                 \
  X(RejectedWorkerLimit, 902)                                               \
  X(RejectedHistoryLimit, 903)                                              \
  X(RejectedInternalInvariant, 904)                                         \
  X(RejectedNotSupported, 905)                                              \
  X(RejectedStoreUnavailable, 906)                                          \
  X(RejectedLockUnavailable, 907)

enum class ReasonCode : std::uint32_t {
#define FLOW_OFFLOAD_REASON_ENUM(name, value) name = value,
  FLOW_OFFLOAD_REASON_CODES(FLOW_OFFLOAD_REASON_ENUM)
#undef FLOW_OFFLOAD_REASON_ENUM
};

/// Canonical, stable, human-readable spelling of a reason code. The returned view
/// is identical to the enumerator name, so it is safe to persist and compare.
[[nodiscard]] std::string_view to_string(ReasonCode code) noexcept;

/// Numeric value; the machine-readable identity of the code.
[[nodiscard]] constexpr std::uint32_t reason_value(ReasonCode code) noexcept {
  return static_cast<std::uint32_t>(code);
}

/// True when the code denotes an accepted operation.
///
/// The set is an explicit whitelist: anything not listed - including any code
/// added later without a deliberate decision - classifies as a failure, so a
/// new code can never accidentally report success.
[[nodiscard]] constexpr bool is_acceptance(ReasonCode code) noexcept {
  const std::uint32_t v = reason_value(code);
  if (v == 0U || (v >= 400U && v < 500U)) {
    return true;
  }
  switch (code) {
    case ReasonCode::PersistCommitted:
    case ReasonCode::PersistNotConfigured:
    case ReasonCode::RecoveryCompacted:
    case ReasonCode::RecoveryCleanReopen:
    case ReasonCode::RecoveryJournalReplayed:
    case ReasonCode::RecoverySnapshotOnly:
    case ReasonCode::RecoveryJournalOnly:
    case ReasonCode::RecoveryEmptyStore:
    case ReasonCode::RecoveryTornTailTruncated:
    case ReasonCode::RecoveryEpochAdvanced:
    case ReasonCode::RecoveryFencedPreRestartAuthority:
    case ReasonCode::RecoveryEvidenceDiscarded:
    case ReasonCode::ProtocolOk:
    case ReasonCode::AcceptedShutdownRequested:
      return true;
    default:
      return false;
  }
}

/// True when the code denotes anything other than an accepted operation. The
/// default is refusal: an unrecognised outcome is never reported as success.
[[nodiscard]] constexpr bool is_refusal(ReasonCode code) noexcept {
  return !is_acceptance(code);
}

/// True when the code denotes a recovery or persistence classification.
[[nodiscard]] constexpr bool is_recovery(ReasonCode code) noexcept {
  const std::uint32_t v = reason_value(code);
  return v >= 500U && v < 600U;
}

/// True when the code denotes a transport protocol outcome.
[[nodiscard]] constexpr bool is_protocol(ReasonCode code) noexcept {
  const std::uint32_t v = reason_value(code);
  return v >= 600U && v < 650U;
}

/// True when the code denotes a cancellation or shutdown outcome.
[[nodiscard]] constexpr bool is_cancellation(ReasonCode code) noexcept {
  const std::uint32_t v = reason_value(code);
  return v >= 650U && v < 700U;
}

/// Parses the canonical spelling back to a code. Returns false for unknown text;
/// unknown text is never coerced to a default code.
[[nodiscard]] bool reason_from_string(std::string_view text, ReasonCode& out) noexcept;

/// Total number of defined reason codes.
[[nodiscard]] constexpr std::size_t reason_code_count() noexcept {
  std::size_t n = 0;
#define FLOW_OFFLOAD_REASON_COUNT(name, value) n += 1;
  FLOW_OFFLOAD_REASON_CODES(FLOW_OFFLOAD_REASON_COUNT)
#undef FLOW_OFFLOAD_REASON_COUNT
  return n;
}

/// A status is a reason code with an explicit accept/refuse classification
/// derived from the code itself, so a refusal can never be reported as success.
class Status {
 public:
  constexpr Status() noexcept = default;
  constexpr explicit Status(ReasonCode code) noexcept : code_(code) {}

  [[nodiscard]] static constexpr Status accepted(ReasonCode code = ReasonCode::Ok) noexcept {
    return Status(code);
  }
  [[nodiscard]] static constexpr Status refused(ReasonCode code) noexcept {
    return Status(code);
  }

  [[nodiscard]] constexpr ReasonCode code() const noexcept { return code_; }
  [[nodiscard]] constexpr bool ok() const noexcept { return is_acceptance(code_); }
  [[nodiscard]] constexpr bool failed() const noexcept { return !ok(); }
  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(Status a, Status b) noexcept { return a.code_ == b.code_; }
  friend constexpr bool operator!=(Status a, Status b) noexcept { return !(a == b); }

 private:
  ReasonCode code_{ReasonCode::Ok};
};

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_REASON_HPP
