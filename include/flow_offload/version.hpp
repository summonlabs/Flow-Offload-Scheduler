// Flow Offload Scheduler - version and compatibility constants.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_VERSION_HPP
#define FLOW_OFFLOAD_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace flow_offload {

/// Library version reported by the build and embedded in exports.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Semantic version of the persisted state model. Bumped whenever the meaning of
/// a persisted field changes; a store written under a different value is refused
/// rather than reinterpreted.
inline constexpr std::uint32_t kStateSemanticsVersion = 1;

/// Version of the durable on-disk container (byte layout). Independent of the
/// semantics version: a container version bump may be readable by an older
/// reader only if the semantics version also matches.
inline constexpr std::uint32_t kSnapshotContainerVersion = 1;
inline constexpr std::uint32_t kJournalContainerVersion = 1;

/// Version of the wire protocol spoken by the service transport.
inline constexpr std::uint32_t kProtocolVersion = 1;

/// Canonical export schema version.
inline constexpr std::uint32_t kExportSchemaVersion = 1;

[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_VERSION_HPP
