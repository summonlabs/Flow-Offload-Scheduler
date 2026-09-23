// Flow Offload Scheduler - reason code rendering and parsing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/reason.hpp"

#include <array>

namespace flow_offload {
namespace {

struct ReasonEntry {
  ReasonCode code;
  std::string_view name;
};

/// The table is in declaration order and is scanned linearly. The list is small
/// and fixed, so a sorted table would buy nothing and would add a second place
/// where the two orders could disagree.
constexpr std::array<ReasonEntry, reason_code_count()> kReasonTable = {{
#define FLOW_OFFLOAD_REASON_ENTRY(name, value) ReasonEntry{ReasonCode::name, std::string_view{#name}},
    FLOW_OFFLOAD_REASON_CODES(FLOW_OFFLOAD_REASON_ENTRY)
#undef FLOW_OFFLOAD_REASON_ENTRY
}};

constexpr bool table_is_complete() noexcept {
  for (std::size_t i = 0; i < kReasonTable.size(); ++i) {
    if (!kReasonTable[i].name.empty()) {
      continue;
    }
    return false;
  }
  return true;
}

constexpr bool table_is_unique() noexcept {
  for (std::size_t i = 0; i < kReasonTable.size(); ++i) {
    for (std::size_t j = i + 1; j < kReasonTable.size(); ++j) {
      if (kReasonTable[i].code == kReasonTable[j].code ||
          kReasonTable[i].name == kReasonTable[j].name) {
        return false;
      }
    }
  }
  return true;
}

static_assert(table_is_complete(), "every reason code must carry a canonical name");
static_assert(table_is_unique(), "reason codes and names must be unique");

}  // namespace

std::string_view to_string(ReasonCode code) noexcept {
  const std::uint32_t value = reason_value(code);
  for (const ReasonEntry& entry : kReasonTable) {
    if (reason_value(entry.code) == value) {
      return entry.name;
    }
  }
  return std::string_view{"UnknownReasonCode"};
}

bool reason_from_string(std::string_view text, ReasonCode& out) noexcept {
  if (text.empty()) {
    return false;
  }
  for (const ReasonEntry& entry : kReasonTable) {
    if (entry.name == text) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

std::string Status::to_string() const {
  if (ok()) {
    return std::string{"ok:"} + std::string{flow_offload::to_string(code_)};
  }
  return std::string{"failed:"} + std::string{flow_offload::to_string(code_)};
}

}  // namespace flow_offload
