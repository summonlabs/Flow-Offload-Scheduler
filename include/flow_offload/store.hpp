// Flow Offload Scheduler - versioned, integrity-checked durable store.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_STORE_HPP
#define FLOW_OFFLOAD_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <string>

#include "flow_offload/state.hpp"

namespace flow_offload {

/// Classification of what a recovery actually did. Every outcome is reported;
/// a damaged store never silently becomes an empty store.
enum class RecoveryKind : std::uint8_t {
  EmptyStore = 0,
  CleanReopen = 1,
  TornTailTruncated = 2,
  JournalReplayed = 3,
  SnapshotOnly = 4,
  JournalOnly = 5,
  Corrupt = 6,
  IncompatibleVersion = 7,
  SemanticsMismatch = 8,
  Oversized = 9,
  Unavailable = 10,
};

[[nodiscard]] std::string_view to_string(RecoveryKind kind) noexcept;

/// Durable boundaries at which a process may be hard-killed. The hook exists so
/// that crash behaviour is exercised against a real process death instead of a
/// simulated exception.
enum class CrashPoint : std::uint8_t {
  BeforeJournalAppend = 0,
  AfterJournalAppendBeforeFlush = 1,
  AfterJournalFlushBeforeSnapshot = 2,
  AfterSnapshotWriteBeforeRename = 3,
  AfterSnapshotRenameBeforeJournalRewrite = 4,
  AfterJournalRewriteBeforeAcknowledge = 5,
};

[[nodiscard]] std::string_view to_string(CrashPoint point) noexcept;

struct RecoveryReport {
  RecoveryKind kind{RecoveryKind::EmptyStore};
  ReasonCode code{ReasonCode::RecoveryEmptyStore};
  bool usable{false};
  bool snapshot_used{false};
  bool journal_used{false};
  std::size_t records_replayed{0};
  std::uint64_t bytes_truncated{0};
  std::uint64_t commit_sequence{0};
  Sha256Digest state_digest{};
  std::string detail;

  friend bool operator==(const RecoveryReport&, const RecoveryReport&) noexcept = default;
};

struct StoreConfig {
  std::filesystem::path directory;
  std::uint64_t max_snapshot_bytes{kMaxSnapshotBytes};
  std::uint64_t max_journal_bytes{kMaxJournalBytes};
  std::uint32_t max_journal_records{512};
  /// Compaction is triggered once this many records have been appended since the
  /// last snapshot, or once the journal exceeds half of max_journal_bytes.
  std::uint32_t compaction_record_threshold{64};
  bool flush_to_device{true};
};

/// A log-structured store: an integrity-checked snapshot plus an append-only
/// journal of full-state commit records. Every commit is durable before it is
/// acknowledged, so a crash can lose at most the in-flight commit and never
/// reports success for a commit that did not reach the device.
class Store {
 public:
  using CrashHook = void (*)(CrashPoint point, void* context);

  Store() = default;
  explicit Store(const StoreConfig& config);
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  /// Opens or creates the store and recovers the newest durable state. Never
  /// throws on malformed content; damage is classified in the returned report.
  RecoveryReport open();

  /// Re-reads both files and validates them without modifying anything.
  [[nodiscard]] RecoveryReport verify_integrity() const;

  void close();
  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] bool has_state() const noexcept { return has_state_; }
  [[nodiscard]] const DurableState& state() const noexcept { return state_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }

  /// Durably records p state. The commit is acknowledged only after the record
  /// has reached the device.
  Status commit(const DurableState& state);

  /// Writes a new snapshot and restarts the journal. Compaction never loses the
  /// most recent acknowledged commit.
  Status compact(const DurableState& state);

  [[nodiscard]] std::uint64_t commit_sequence() const noexcept { return commit_sequence_; }
  [[nodiscard]] std::uint64_t journal_bytes() const noexcept { return journal_bytes_; }
  [[nodiscard]] std::uint32_t records_since_snapshot() const noexcept {
    return records_since_snapshot_;
  }
  [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept {
    return snapshot_path_;
  }
  [[nodiscard]] const std::filesystem::path& journal_path() const noexcept { return journal_path_; }

  /// Test seam: the hook runs at each durable boundary. A test that kills the
  /// process inside the hook proves the crash classification for real.
  void set_crash_hook(CrashHook hook, void* context) noexcept {
    crash_hook_ = hook;
    crash_context_ = context;
  }

  /// Test seam: forces the next commit to fail with the given reason without
  /// touching the files, so the caller-visible failure path is exercised.
  void set_next_commit_failure(ReasonCode code) noexcept { forced_failure_ = code; }

 private:
  void run_hook(CrashPoint point) const;
  [[nodiscard]] bool write_atomic(const std::filesystem::path& target,
                                  std::span<const std::byte> bytes, ReasonCode& failure) const;
  [[nodiscard]] bool append_record(const DurableState& state, ReasonCode& failure);
  [[nodiscard]] bool rewrite_journal(std::uint64_t base_sequence, const Sha256Digest& base_digest,
                                     ReasonCode& failure);
  [[nodiscard]] bool open_journal_for_append(ReasonCode& failure);

  StoreConfig config_{};
  std::filesystem::path snapshot_path_;
  std::filesystem::path journal_path_;
  bool open_{false};
  bool has_state_{false};
  DurableState state_{};
  RecoveryReport recovery_{};
  std::uint64_t commit_sequence_{0};
  std::uint64_t journal_bytes_{0};
  std::uint32_t records_since_snapshot_{0};
  Sha256Digest snapshot_digest_{};
  void* file_handle_{nullptr};
  CrashHook crash_hook_{nullptr};
  void* crash_context_{nullptr};
  ReasonCode forced_failure_{ReasonCode::Ok};
};

/// Derives the boot identity for a coordinator incarnation. Deterministic from
/// the previous boot and the new epoch, so two coordinators that recover the
/// same store cannot accidentally claim the same incarnation.
[[nodiscard]] BootId derive_boot_id(BootId previous, CoordinatorEpoch epoch) noexcept;

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_STORE_HPP
