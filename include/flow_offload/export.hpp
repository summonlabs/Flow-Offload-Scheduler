// Flow Offload Scheduler - canonical machine-readable export.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_EXPORT_HPP
#define FLOW_OFFLOAD_EXPORT_HPP

#include <cstddef>
#include <string>

#include "flow_offload/engine.hpp"

namespace flow_offload {

struct ExportOptions {
  bool include_flows{true};
  bool include_assignments{true};
  bool include_migrations{true};
  bool include_counters{true};
  bool include_recovery{true};
  std::size_t max_bytes{kMaxExportBytes};
};

/// Canonical JSON. Object keys are emitted in a fixed order, arrays follow the
/// canonical order of their contents, and no value depends on hash iteration or
/// on the platform. The same state always exports byte-identically.
[[nodiscard]] std::string export_json(const EngineExport& state, const ExportOptions& options = {});

/// Canonical binary export: a length-delimited sequence of canonical records.
[[nodiscard]] std::string export_binary(const EngineExport& state);

/// Stable human-readable rendering for inspection tooling.
[[nodiscard]] std::string export_text(const EngineExport& state);

/// Canonical digest of the JSON export, usable as a state fingerprint.
[[nodiscard]] std::string export_digest(const EngineExport& state);

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_EXPORT_HPP
