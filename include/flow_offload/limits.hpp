// Flow Offload Scheduler - bounded resource limits.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_LIMITS_HPP
#define FLOW_OFFLOAD_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace flow_offload {

/// Every externally influenced size, count and payload is bounded before any
/// allocation happens. Exceeding a bound is an observable refusal with a stable
/// reason code, never a silent truncation.

inline constexpr std::size_t kMaxCanonicalBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaxStringBytes = 4096U;
inline constexpr std::size_t kMaxNameBytes = 128U;
inline constexpr std::size_t kMaxExportBytes = 128U * 1024U * 1024U;

inline constexpr std::size_t kMaxTargets = 65536U;
inline constexpr std::size_t kMaxFlows = 1048576U;
inline constexpr std::size_t kMaxEvidenceSources = 4096U;
inline constexpr std::size_t kMaxLoadRecords = 1048576U;
inline constexpr std::size_t kMaxMigrationContracts = 65536U;
inline constexpr std::size_t kMaxMigrationRecords = 262144U;
inline constexpr std::size_t kMaxAuthorizationRecords = 1048576U;
inline constexpr std::size_t kMaxRequestRecords = 65536U;
inline constexpr std::size_t kMaxHistoryRecords = 4096U;
inline constexpr std::size_t kMaxExplanationSteps = 64U;
inline constexpr std::size_t kMaxRankedCandidates = 4096U;
inline constexpr std::size_t kMaxAlternatives = 16U;
inline constexpr std::size_t kMaxRebalanceMigrations = 1024U;

inline constexpr std::uint32_t kMaxWorkers = 32U;
inline constexpr std::size_t kMaxQueueDepth = 4096U;
inline constexpr std::size_t kMaxConnections = 64U;
inline constexpr std::size_t kMaxFrameBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxFramePayloadBytes = kMaxFrameBytes - 64U;

inline constexpr std::uint64_t kMaxJournalRecordBytes = 16U * 1024U * 1024U;
inline constexpr std::uint64_t kMaxSnapshotBytes = 512U * 1024U * 1024U;
inline constexpr std::uint64_t kMaxJournalBytes = 256U * 1024U * 1024U;
inline constexpr std::uint64_t kMaxStoreBytes = 1024U * 1024U * 1024U;

/// Fixed-size header sizes used by the persistence layout. These must match the
/// byte layout documented in store.cpp exactly; a mismatch would make the
/// recovery path read a header digest as if it were a record.
inline constexpr std::size_t kSnapshotHeaderBytes = 128U;
inline constexpr std::size_t kJournalHeaderBytes = 96U;
inline constexpr std::size_t kJournalRecordHeaderBytes = 16U;

static_assert(kJournalHeaderBytes == 8U + 4U + 4U + 8U + 32U + 8U + 32U,
              "journal header layout must be magic+versions+base+digest+reserved+digest");
static_assert(kSnapshotHeaderBytes == 8U + 4U + 4U + 4U + 4U + 8U + 8U + 8U + 8U + 8U + 32U + 32U,
              "snapshot header layout must match store.cpp");

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_LIMITS_HPP
