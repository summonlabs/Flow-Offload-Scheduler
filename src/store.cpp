// Flow Offload Scheduler - versioned, integrity-checked durable store.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Layout
// ------
// state.snapshot : fixed 128-byte header + canonical DurableState payload
//    0   8  magic "FOSSNP01"        32  8  boot.high          64  32 payload digest
//    8   4  container version       40  8  boot.low           96  32 header digest
//   12   4  semantics version       48  8  payload length          (over bytes 0..95)
//   16   4  flags                   56  8  commit sequence
//   20   4  reserved (must be 0)    24  8  coordinator epoch
//
// state.journal  : fixed 96-byte header + records
//    0   8  magic "FOSJNL01"        16  8  base commit sequence
//    8   4  container version       24  32 base payload digest
//   12   4  semantics version       56  8  reserved (must be 0)
//                                    64  32 header digest (over bytes 0..63)
//   record: 16-byte header (payload length u32, sequence u64, kind u8, 3 reserved)
//           + payload + 32-byte digest over header+payload.
//
// A commit is acknowledged only after the record has been flushed to the device.

#include "flow_offload/store.hpp"

#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#include "flow_offload/canonical.hpp"
#include "flow_offload/serialize.hpp"

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace flow_offload {
namespace {

constexpr std::uint8_t kSnapshotMagic[8] = {'F', 'O', 'S', 'S', 'N', 'P', '0', '1'};
constexpr std::uint8_t kJournalMagic[8] = {'F', 'O', 'S', 'J', 'N', 'L', '0', '1'};
constexpr std::size_t kSnapshotHeaderSize = kSnapshotHeaderBytes;
constexpr std::size_t kJournalHeaderSize = kJournalHeaderBytes;
constexpr std::size_t kRecordHeaderSize = kJournalRecordHeaderBytes;
constexpr std::size_t kDigestSize = 32;
constexpr std::uint8_t kRecordKindFullState = 1;

void put_u32(std::byte* p, std::uint32_t value) noexcept {
  p[0] = static_cast<std::byte>((value >> 24U) & 0xFFU);
  p[1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  p[2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  p[3] = static_cast<std::byte>(value & 0xFFU);
}

void put_u64(std::byte* p, std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    p[i] = static_cast<std::byte>((value >> ((7U - i) * 8U)) & 0xFFU);
  }
}

std::uint32_t get_u32(const std::byte* p) noexcept {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
}

std::uint64_t get_u64(const std::byte* p) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
  }
  return value;
}

/// Opens a file with a share mode that lets a second reader - a verification
/// tool, for example - open the same store while it is in use. The default CRT
/// share mode refuses that, which would make an integrity check impossible while
/// the runtime is running.
[[nodiscard]] std::FILE* open_shared(const std::filesystem::path& path, const char* mode) noexcept {
#if defined(_WIN32)
  return ::_fsopen(path.string().c_str(), mode, _SH_DENYNO);
#else
  return std::fopen(path.string().c_str(), mode);
#endif
}

bool sync_file(std::FILE* file) noexcept {
  if (std::fflush(file) != 0) {
    return false;
  }
#if defined(_WIN32)
  return _commit(_fileno(file)) == 0;
#else
  return ::fsync(fileno(file)) == 0;
#endif
}

[[nodiscard]] bool read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes,
                                     std::vector<std::byte>& out, ReasonCode& failure) noexcept {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    failure = ReasonCode::RejectedStoreUnavailable;
    return false;
  }
  if (size > max_bytes) {
    failure = ReasonCode::RejectedOversizedInput;
    return false;
  }
  std::FILE* file = open_shared(path, "rb");
  if (file == nullptr) {
    failure = ReasonCode::RejectedStoreUnavailable;
    return false;
  }
  try {
    out.assign(static_cast<std::size_t>(size), std::byte{0});
  } catch (...) {
    std::fclose(file);
    failure = ReasonCode::RejectedResourceExhausted;
    return false;
  }
  const std::size_t read = out.empty() ? 0U : std::fread(out.data(), 1, out.size(), file);
  const bool short_read = read != out.size();
  std::fclose(file);
  if (short_read) {
    failure = ReasonCode::RejectedTruncatedInput;
    return false;
  }
  return true;
}

[[nodiscard]] Sha256Digest digest_range(const std::vector<std::byte>& bytes, std::size_t begin,
                                        std::size_t end) noexcept {
  return Sha256::of(std::span<const std::byte>(bytes.data() + begin, end - begin));
}

/// Everything a recovery pass produced. Shared by open() and verify_integrity()
/// so that a read-only verification performs exactly the same validation.
struct RecoveryPass {
  RecoveryReport report{};
  DurableState state{};
  bool has_state{false};
  std::uint64_t sequence{0};
  Sha256Digest snapshot_digest{};
  bool snapshot_used{false};
  bool journal_header_ok{false};
  bool journal_exists{false};
  std::size_t journal_good_bytes{0};
  std::uint32_t records_replayed{0};
  bool torn_tail{false};
};

[[nodiscard]] bool probe_journal_header(const std::vector<std::byte>& bytes) noexcept {
  if (bytes.size() < kJournalHeaderSize) {
    return false;
  }
  if (std::memcmp(bytes.data(), kJournalMagic, sizeof(kJournalMagic)) != 0) {
    return false;
  }
  Sha256Digest stored{};
  std::memcpy(stored.bytes.data(), bytes.data() + 64, kDigestSize);
  return digest_range(bytes, 0, 64) == stored;
}

[[nodiscard]] RecoveryPass recover(const StoreConfig& config) noexcept {
  RecoveryPass pass{};
  RecoveryReport& report = pass.report;
  report.detail.reserve(160);

  const std::filesystem::path snapshot_path = config.directory / "state.snapshot";
  const std::filesystem::path journal_path = config.directory / "state.journal";

  std::error_code ec;
  const bool snapshot_exists = std::filesystem::exists(snapshot_path, ec) && !ec;
  std::error_code journal_ec;
  const bool journal_exists = std::filesystem::exists(journal_path, journal_ec) && !journal_ec;
  pass.journal_exists = journal_exists;

  bool snapshot_damaged = false;
  if (snapshot_exists) {
    std::vector<std::byte> bytes;
    ReasonCode failure = ReasonCode::Ok;
    if (!read_file_bounded(snapshot_path, config.max_snapshot_bytes, bytes, failure)) {
      report.kind = (failure == ReasonCode::RejectedOversizedInput) ? RecoveryKind::Oversized
                                                                   : RecoveryKind::Corrupt;
      report.code = (failure == ReasonCode::RejectedOversizedInput)
                        ? ReasonCode::RecoveryOversizedRecord
                        : ReasonCode::RecoveryTruncatedSnapshot;
      report.usable = false;
      report.detail = std::string{"snapshot could not be read: "} +
                      std::string{to_string(failure)};
      return pass;
    }
    if (bytes.size() < kSnapshotHeaderSize) {
      snapshot_damaged = true;
      report.detail = "snapshot is shorter than its header";
    } else if (std::memcmp(bytes.data(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
      snapshot_damaged = true;
      report.detail = "snapshot magic mismatch";
    } else {
      const std::uint32_t container_version = get_u32(bytes.data() + 8);
      const std::uint32_t semantics_version = get_u32(bytes.data() + 12);
      if (container_version != kSnapshotContainerVersion) {
        report.kind = RecoveryKind::IncompatibleVersion;
        report.code = ReasonCode::RecoveryIncompatibleVersion;
        report.usable = false;
        report.detail = "snapshot container version is not supported";
        return pass;
      }
      if (semantics_version != kStateSemanticsVersion) {
        report.kind = RecoveryKind::SemanticsMismatch;
        report.code = ReasonCode::RecoverySemanticsMismatch;
        report.usable = false;
        report.detail = "snapshot semantics version is not supported";
        return pass;
      }
      Sha256Digest stored_payload_digest{};
      Sha256Digest stored_header_digest{};
      std::memcpy(stored_payload_digest.bytes.data(), bytes.data() + 64, kDigestSize);
      std::memcpy(stored_header_digest.bytes.data(), bytes.data() + 96, kDigestSize);
      const std::uint32_t reserved = get_u32(bytes.data() + 20);
      const std::uint64_t payload_length = get_u64(bytes.data() + 48);
      if (reserved != 0U) {
        snapshot_damaged = true;
        report.detail = "snapshot reserved field is not zero";
      } else if (digest_range(bytes, 0, 96) != stored_header_digest) {
        snapshot_damaged = true;
        report.detail = "snapshot header digest mismatch";
      } else if (payload_length != (bytes.size() - kSnapshotHeaderSize)) {
        snapshot_damaged = true;
        report.detail = "snapshot payload length mismatch";
      } else if (digest_range(bytes, kSnapshotHeaderSize, bytes.size()) != stored_payload_digest) {
        snapshot_damaged = true;
        report.detail = "snapshot payload digest mismatch";
      } else {
        CanonicalReader reader(std::span<const std::byte>(bytes.data() + kSnapshotHeaderSize,
                                                          bytes.size() - kSnapshotHeaderSize));
        DurableState decoded{};
        if (!decode(reader, decoded) || reader.failed() || !reader.at_end()) {
          snapshot_damaged = true;
          report.detail = "snapshot payload is not a canonical state record";
        } else {
          pass.snapshot_used = true;
          pass.sequence = get_u64(bytes.data() + 56);
          pass.snapshot_digest = stored_payload_digest;
          pass.state = std::move(decoded);
          pass.has_state = true;
        }
      }
    }
  }

  bool journal_used = false;
  bool torn_tail = false;
  std::size_t last_good = 0;

  if (journal_exists) {
    std::vector<std::byte> bytes;
    ReasonCode failure = ReasonCode::Ok;
    if (!read_file_bounded(journal_path, config.max_journal_bytes, bytes, failure)) {
      report.kind = (failure == ReasonCode::RejectedOversizedInput) ? RecoveryKind::Oversized
                                                                    : RecoveryKind::Corrupt;
      report.code = (failure == ReasonCode::RejectedOversizedInput)
                        ? ReasonCode::RecoveryOversizedRecord
                        : ReasonCode::RecoveryCorruptJournal;
      report.usable = false;
      report.detail = std::string{"journal could not be read: "} +
                      std::string{to_string(failure)};
      return pass;
    }
    if (bytes.size() < kJournalHeaderSize) {
      torn_tail = true;
      last_good = 0;
      report.detail = "journal is shorter than its header";
    } else if (std::memcmp(bytes.data(), kJournalMagic, sizeof(kJournalMagic)) != 0) {
      torn_tail = true;
      last_good = 0;
      report.detail = "journal magic mismatch";
    } else {
      const std::uint32_t container_version = get_u32(bytes.data() + 8);
      const std::uint32_t semantics_version = get_u32(bytes.data() + 12);
      if (container_version != kJournalContainerVersion) {
        report.kind = RecoveryKind::IncompatibleVersion;
        report.code = ReasonCode::RecoveryIncompatibleVersion;
        report.usable = false;
        report.detail = "journal container version is not supported";
        return pass;
      }
      if (semantics_version != kStateSemanticsVersion) {
        report.kind = RecoveryKind::SemanticsMismatch;
        report.code = ReasonCode::RecoverySemanticsMismatch;
        report.usable = false;
        report.detail = "journal semantics version is not supported";
        return pass;
      }
      Sha256Digest stored_header_digest{};
      std::memcpy(stored_header_digest.bytes.data(), bytes.data() + 64, kDigestSize);
      if (digest_range(bytes, 0, 64) != stored_header_digest) {
        torn_tail = true;
        last_good = 0;
        report.detail = "journal header digest mismatch";
      } else {
        pass.journal_header_ok = true;
        const std::uint64_t base_sequence = get_u64(bytes.data() + 16);
        if (pass.snapshot_used && base_sequence > pass.sequence) {
          report.kind = RecoveryKind::Corrupt;
          report.code = ReasonCode::RecoveryCorruptJournal;
          report.usable = false;
          report.detail = "journal base is newer than the snapshot it claims";
          return pass;
        }
        std::size_t offset = kJournalHeaderSize;
        last_good = kJournalHeaderSize;
        while (offset < bytes.size()) {
          const std::size_t available = bytes.size() - offset;
          if (available < kRecordHeaderSize) {
            torn_tail = true;
            report.detail = "journal ends inside a record header";
            break;
          }
          const std::uint32_t payload_length = get_u32(bytes.data() + offset);
          if (payload_length > kMaxJournalRecordBytes ||
              static_cast<std::uint64_t>(payload_length) + kRecordHeaderSize + kDigestSize >
                  config.max_journal_bytes) {
            report.kind = RecoveryKind::Oversized;
            report.code = ReasonCode::RecoveryOversizedRecord;
            report.usable = false;
            report.detail = "journal record length exceeds the configured bound";
            return pass;
          }
          const std::uint64_t record_sequence = get_u64(bytes.data() + offset + 4);
          const std::uint8_t record_kind = std::to_integer<std::uint8_t>(bytes[offset + 12]);
          const std::size_t total = kRecordHeaderSize + payload_length + kDigestSize;
          if (available < total) {
            torn_tail = true;
            report.detail = "journal ends inside a record payload";
            break;
          }
          const Sha256Digest computed =
              digest_range(bytes, offset, offset + kRecordHeaderSize + payload_length);
          Sha256Digest stored{};
          std::memcpy(stored.bytes.data(), bytes.data() + offset + kRecordHeaderSize + payload_length,
                      kDigestSize);
          if (computed != stored) {
            // A damaged record followed by an intact one is mid-file corruption
            // and must never be silently skipped.
            bool found_valid_after = false;
            for (std::size_t scan = offset + 1; scan + kRecordHeaderSize <= bytes.size(); ++scan) {
              const std::uint32_t scan_length = get_u32(bytes.data() + scan);
              if (scan_length > kMaxJournalRecordBytes) {
                continue;
              }
              const std::size_t scan_total = kRecordHeaderSize + scan_length + kDigestSize;
              if (scan_total > bytes.size() - scan) {
                continue;
              }
              Sha256Digest scan_stored{};
              std::memcpy(scan_stored.bytes.data(),
                          bytes.data() + scan + kRecordHeaderSize + scan_length, kDigestSize);
              if (digest_range(bytes, scan, scan + kRecordHeaderSize + scan_length) == scan_stored) {
                found_valid_after = true;
                break;
              }
            }
            if (found_valid_after) {
              report.kind = RecoveryKind::Corrupt;
              report.code = ReasonCode::RecoveryCorruptJournal;
              report.usable = false;
              report.detail = "journal record digest mismatch with intact records after it";
              return pass;
            }
            torn_tail = true;
            report.detail = "journal tail record digest mismatch";
            break;
          }
          if (record_kind != kRecordKindFullState) {
            report.kind = RecoveryKind::Corrupt;
            report.code = ReasonCode::RecoveryCorruptJournal;
            report.usable = false;
            report.detail = "journal record kind is not recognised";
            return pass;
          }
          if (record_sequence <= pass.sequence) {
            // Already represented by the snapshot; skipping keeps the newer
            // state and makes a post-compaction crash idempotent.
            offset += total;
            last_good = offset;
            continue;
          }
          CanonicalReader reader(
              std::span<const std::byte>(bytes.data() + offset + kRecordHeaderSize, payload_length));
          DurableState decoded{};
          if (!decode(reader, decoded) || reader.failed() || !reader.at_end()) {
            report.kind = RecoveryKind::Corrupt;
            report.code = ReasonCode::RecoveryCorruptJournal;
            report.usable = false;
            report.detail = "journal record payload is not a canonical state record";
            return pass;
          }
          pass.state = std::move(decoded);
          pass.sequence = record_sequence;
          pass.snapshot_digest = computed;
          pass.has_state = true;
          journal_used = true;
          ++pass.records_replayed;
          offset += total;
          last_good = offset;
        }
      }
    }
  }

  if (!pass.has_state) {
    if (snapshot_damaged) {
      report.kind = RecoveryKind::Corrupt;
      report.code = ReasonCode::RecoveryCorruptSnapshot;
      report.usable = false;
      report.detail = report.detail.empty() ? "no usable durable state" : report.detail;
      return pass;
    }
    if (torn_tail) {
      // Nothing had been committed yet, so a torn first record is a truncation
      // rather than damage. The classification still reports it.
      report.kind = RecoveryKind::TornTailTruncated;
      report.code = ReasonCode::RecoveryTornTailTruncated;
      report.usable = true;
      report.detail = report.detail.empty() ? "journal tail was truncated" : report.detail;
      report.snapshot_used = false;
      report.journal_used = false;
      return pass;
    }
    report.kind = RecoveryKind::EmptyStore;
    report.code = ReasonCode::RecoveryEmptyStore;
    report.usable = true;
    report.detail = "store directory holds no durable state";
    return pass;
  }

  pass.journal_good_bytes = torn_tail ? last_good : 0;
  pass.torn_tail = torn_tail;

  report.snapshot_used = pass.snapshot_used;
  report.journal_used = journal_used;
  report.records_replayed = pass.records_replayed;
  report.commit_sequence = pass.sequence;
  report.state_digest = pass.snapshot_digest;
  report.usable = true;
  if (torn_tail) {
    report.kind = RecoveryKind::TornTailTruncated;
    report.code = ReasonCode::RecoveryTornTailTruncated;
  } else if (pass.snapshot_used && pass.records_replayed > 0) {
    report.kind = RecoveryKind::JournalReplayed;
    report.code = ReasonCode::RecoveryJournalReplayed;
  } else if (!pass.snapshot_used && journal_used) {
    report.kind = RecoveryKind::JournalOnly;
    report.code = ReasonCode::RecoveryJournalOnly;
  } else if (pass.snapshot_used && journal_exists) {
    report.kind = RecoveryKind::CleanReopen;
    report.code = ReasonCode::RecoveryCleanReopen;
  } else if (pass.snapshot_used) {
    report.kind = RecoveryKind::SnapshotOnly;
    report.code = ReasonCode::RecoverySnapshotOnly;
  } else {
    report.kind = RecoveryKind::EmptyStore;
    report.code = ReasonCode::RecoveryEmptyStore;
  }
  pass.state.canonicalize_order();
  return pass;
}

}  // namespace

std::string_view to_string(RecoveryKind kind) noexcept {
  switch (kind) {
    case RecoveryKind::EmptyStore:
      return std::string_view{"empty-store"};
    case RecoveryKind::CleanReopen:
      return std::string_view{"clean-reopen"};
    case RecoveryKind::TornTailTruncated:
      return std::string_view{"torn-tail-truncated"};
    case RecoveryKind::JournalReplayed:
      return std::string_view{"journal-replayed"};
    case RecoveryKind::SnapshotOnly:
      return std::string_view{"snapshot-only"};
    case RecoveryKind::JournalOnly:
      return std::string_view{"journal-only"};
    case RecoveryKind::Corrupt:
      return std::string_view{"corrupt"};
    case RecoveryKind::IncompatibleVersion:
      return std::string_view{"incompatible-version"};
    case RecoveryKind::SemanticsMismatch:
      return std::string_view{"semantics-mismatch"};
    case RecoveryKind::Oversized:
      return std::string_view{"oversized"};
    case RecoveryKind::Unavailable:
      return std::string_view{"unavailable"};
  }
  return std::string_view{"unavailable"};
}

std::string_view to_string(CrashPoint point) noexcept {
  switch (point) {
    case CrashPoint::BeforeJournalAppend:
      return std::string_view{"before-journal-append"};
    case CrashPoint::AfterJournalAppendBeforeFlush:
      return std::string_view{"after-journal-append-before-flush"};
    case CrashPoint::AfterJournalFlushBeforeSnapshot:
      return std::string_view{"after-journal-flush-before-snapshot"};
    case CrashPoint::AfterSnapshotWriteBeforeRename:
      return std::string_view{"after-snapshot-write-before-rename"};
    case CrashPoint::AfterSnapshotRenameBeforeJournalRewrite:
      return std::string_view{"after-snapshot-rename-before-journal-rewrite"};
    case CrashPoint::AfterJournalRewriteBeforeAcknowledge:
      return std::string_view{"after-journal-rewrite-before-acknowledge"};
  }
  return std::string_view{"unknown"};
}

BootId derive_boot_id(BootId previous, CoordinatorEpoch epoch) noexcept {
  std::byte seed[16];
  put_u64(seed, previous.high);
  put_u64(seed + 8, previous.low);
  const Sha256Digest first = Sha256::of(std::span<const std::byte>(seed, sizeof(seed)));
  std::byte epoch_bytes[8];
  put_u64(epoch_bytes, epoch.value());
  Sha256 hasher;
  hasher.update(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(first.bytes.data()), first.bytes.size()));
  hasher.update(std::span<const std::byte>(epoch_bytes, sizeof(epoch_bytes)));
  const Sha256Digest final_digest = hasher.finish();
  BootId out{};
  out.high = get_u64(reinterpret_cast<const std::byte*>(final_digest.bytes.data()));
  out.low = get_u64(reinterpret_cast<const std::byte*>(final_digest.bytes.data()) + 8);
  if (!out.is_valid()) {
    out.low = 1;
  }
  return out;
}

Store::Store(const StoreConfig& config) : config_(config) {
  snapshot_path_ = config_.directory / "state.snapshot";
  journal_path_ = config_.directory / "state.journal";
}

Store::~Store() { close(); }

void Store::run_hook(CrashPoint point) const {
  if (crash_hook_ != nullptr) {
    crash_hook_(point, crash_context_);
  }
}

bool Store::write_atomic(const std::filesystem::path& target, std::span<const std::byte> bytes,
                         ReasonCode& failure) const {
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  std::FILE* file = open_shared(temporary, "wb");
  if (file == nullptr) {
    failure = ReasonCode::RejectedStoreUnavailable;
    return false;
  }
  if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
    std::fclose(file);
    failure = ReasonCode::PersistCommitFailed;
    return false;
  }
  if (config_.flush_to_device && !sync_file(file)) {
    std::fclose(file);
    failure = ReasonCode::PersistCommitFailed;
    return false;
  }
  if (std::fclose(file) != 0) {
    failure = ReasonCode::PersistCommitFailed;
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    failure = ReasonCode::PersistCommitFailed;
    return false;
  }
  return true;
}

RecoveryReport Store::open() {
  close();
  recovery_ = RecoveryReport{};

  if (config_.directory.empty()) {
    recovery_.kind = RecoveryKind::Unavailable;
    recovery_.code = ReasonCode::RejectedStoreUnavailable;
    recovery_.usable = false;
    recovery_.detail = "no store directory configured";
    return recovery_;
  }

  std::error_code ec;
  if (!std::filesystem::exists(config_.directory, ec)) {
    std::filesystem::create_directories(config_.directory, ec);
    if (ec) {
      recovery_.kind = RecoveryKind::Unavailable;
      recovery_.code = ReasonCode::RejectedStoreUnavailable;
      recovery_.usable = false;
      recovery_.detail = "store directory could not be created";
      return recovery_;
    }
  }

  RecoveryPass pass = recover(config_);
  if (!pass.report.usable) {
    recovery_ = pass.report;
    return recovery_;
  }

  // Repair: truncate a torn tail, then make sure a usable header exists.
  if (pass.journal_exists && pass.torn_tail) {
    std::error_code size_ec;
    const std::uintmax_t original = std::filesystem::file_size(journal_path_, size_ec);
    if (!size_ec) {
      const std::uintmax_t target = pass.journal_good_bytes;
      if (target <= original) {
        std::error_code resize_ec;
        std::filesystem::resize_file(journal_path_, target, resize_ec);
        if (resize_ec) {
          recovery_ = pass.report;
          recovery_.kind = RecoveryKind::Corrupt;
          recovery_.code = ReasonCode::RecoveryCorruptJournal;
          recovery_.usable = false;
          recovery_.detail = "torn journal tail could not be truncated";
          return recovery_;
        }
        recovery_.bytes_truncated = static_cast<std::uint64_t>(original - target);
      }
    }
  }

  ReasonCode failure = ReasonCode::Ok;
  const bool header_usable = pass.journal_exists && pass.journal_header_ok && !pass.torn_tail;
  if (!header_usable) {
    const std::uint64_t base = pass.snapshot_used ? pass.sequence : 0;
    const Sha256Digest base_digest = pass.snapshot_used ? pass.snapshot_digest : Sha256Digest{};
    if (!rewrite_journal(base, base_digest, failure)) {
      recovery_ = pass.report;
      recovery_.kind = RecoveryKind::Unavailable;
      recovery_.code = failure;
      recovery_.usable = false;
      recovery_.detail = "journal header could not be established";
      return recovery_;
    }
  }
  if (!open_journal_for_append(failure)) {
    recovery_ = pass.report;
    recovery_.kind = RecoveryKind::Unavailable;
    recovery_.code = failure;
    recovery_.usable = false;
    recovery_.detail = "journal could not be opened for append";
    return recovery_;
  }

  std::error_code size_ec;
  const std::uintmax_t journal_size = std::filesystem::file_size(journal_path_, size_ec);
  journal_bytes_ = size_ec ? 0 : static_cast<std::uint64_t>(journal_size);

  // Preserve the truncation accounting computed above.
  const std::uint64_t truncated = recovery_.bytes_truncated;
  recovery_ = pass.report;
  recovery_.bytes_truncated = truncated;
  state_ = std::move(pass.state);
  has_state_ = pass.has_state;
  commit_sequence_ = pass.sequence;
  snapshot_digest_ = pass.snapshot_digest;
  records_since_snapshot_ = pass.records_replayed;
  open_ = true;
  return recovery_;
}

RecoveryReport Store::verify_integrity() const { return recover(config_).report; }

bool Store::rewrite_journal(std::uint64_t base_sequence, const Sha256Digest& base_digest,
                            ReasonCode& failure) {
  std::vector<std::byte> header(kJournalHeaderSize, std::byte{0});
  std::memcpy(header.data(), kJournalMagic, sizeof(kJournalMagic));
  put_u32(header.data() + 8, kJournalContainerVersion);
  put_u32(header.data() + 12, kStateSemanticsVersion);
  put_u64(header.data() + 16, base_sequence);
  std::memcpy(header.data() + 24, base_digest.bytes.data(), kDigestSize);
  put_u64(header.data() + 56, 0);
  const Sha256Digest header_digest = Sha256::of(std::span<const std::byte>(header.data(), 64));
  std::memcpy(header.data() + 64, header_digest.bytes.data(), kDigestSize);
  if (file_handle_ != nullptr) {
    std::fclose(static_cast<std::FILE*>(file_handle_));
    file_handle_ = nullptr;
  }
  std::FILE* file = open_shared(journal_path_, "wb");
  if (file == nullptr) {
    failure = ReasonCode::RejectedStoreUnavailable;
    return false;
  }
  if (std::fwrite(header.data(), 1, header.size(), file) != header.size() ||
      (config_.flush_to_device && !sync_file(file))) {
    std::fclose(file);
    failure = ReasonCode::PersistCommitFailed;
    return false;
  }
  if (std::fclose(file) != 0) {
    failure = ReasonCode::PersistCommitFailed;
    return false;
  }
  journal_bytes_ = static_cast<std::uint64_t>(header.size());
  records_since_snapshot_ = 0;
  return true;
}

bool Store::open_journal_for_append(ReasonCode& failure) {
  if (file_handle_ != nullptr) {
    std::fclose(static_cast<std::FILE*>(file_handle_));
    file_handle_ = nullptr;
  }
  std::FILE* file = open_shared(journal_path_, "ab");
  if (file == nullptr) {
    failure = ReasonCode::RejectedStoreUnavailable;
    return false;
  }
  file_handle_ = file;
  return true;
}

bool Store::append_record(const DurableState& state, ReasonCode& failure) {
  CanonicalWriter payload;
  encode(payload, state);
  if (!payload.ok() || payload.size() > kMaxJournalRecordBytes) {
    failure = ReasonCode::RejectedOversizedInput;
    return false;
  }
  const std::size_t total = kRecordHeaderSize + payload.size() + kDigestSize;
  if (journal_bytes_ + total > config_.max_journal_bytes) {
    failure = ReasonCode::PersistRotationRequired;
    return false;
  }
  std::vector<std::byte> record;
  try {
    record.assign(total, std::byte{0});
  } catch (...) {
    failure = ReasonCode::RejectedResourceExhausted;
    return false;
  }
  put_u32(record.data(), static_cast<std::uint32_t>(payload.size()));
  put_u64(record.data() + 4, commit_sequence_ + 1);
  record[12] = static_cast<std::byte>(kRecordKindFullState);
  record[13] = std::byte{0};
  record[14] = std::byte{0};
  record[15] = std::byte{0};
  std::memcpy(record.data() + kRecordHeaderSize, payload.bytes().data(), payload.size());
  const Sha256Digest record_digest =
      Sha256::of(std::span<const std::byte>(record.data(), kRecordHeaderSize + payload.size()));
  std::memcpy(record.data() + kRecordHeaderSize + payload.size(), record_digest.bytes.data(),
              kDigestSize);

  auto* file = static_cast<std::FILE*>(file_handle_);
  if (file == nullptr) {
    failure = ReasonCode::RejectedStoreUnavailable;
    return false;
  }
  run_hook(CrashPoint::BeforeJournalAppend);
  run_hook(CrashPoint::AfterJournalAppendBeforeFlush);
  if (std::fwrite(record.data(), 1, record.size(), file) != record.size()) {
    failure = ReasonCode::PersistCommitFailed;
    return false;
  }
  if (config_.flush_to_device && !sync_file(file)) {
    failure = ReasonCode::PersistCommitFailed;
    return false;
  }
  journal_bytes_ += static_cast<std::uint64_t>(record.size());
  ++commit_sequence_;
  ++records_since_snapshot_;
  state_ = state;
  state_.canonicalize_order();
  has_state_ = true;
  return true;
}

Status Store::commit(const DurableState& state) {
  if (forced_failure_ != ReasonCode::Ok) {
    const ReasonCode code = forced_failure_;
    forced_failure_ = ReasonCode::Ok;
    return Status::refused(code);
  }
  if (!open_ || file_handle_ == nullptr) {
    return Status::refused(ReasonCode::RejectedStoreUnavailable);
  }
  ReasonCode failure = ReasonCode::Ok;
  if (!append_record(state, failure)) {
    return Status::refused(failure);
  }
  run_hook(CrashPoint::AfterJournalFlushBeforeSnapshot);
  if (records_since_snapshot_ >= config_.compaction_record_threshold ||
      journal_bytes_ >= config_.max_journal_bytes / 2U) {
    const Status compacted = compact(state);
    if (compacted.failed()) {
      // The commit itself is already durable on the device. Reporting a failure
      // here would claim a durable commit was lost, so the compaction problem is
      // surfaced as its own reason instead.
      return Status::accepted(ReasonCode::PersistRotationRequired);
    }
    return Status::accepted(ReasonCode::RecoveryCompacted);
  }
  return Status::accepted(ReasonCode::PersistCommitted);
}

Status Store::compact(const DurableState& state) {
  if (!open_) {
    return Status::refused(ReasonCode::RejectedStoreUnavailable);
  }
  CanonicalWriter payload;
  encode(payload, state);
  if (!payload.ok()) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  if (static_cast<std::uint64_t>(payload.size()) + kSnapshotHeaderSize > config_.max_snapshot_bytes) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  const std::uint64_t next_sequence = commit_sequence_ + 1;
  std::vector<std::byte> file_bytes;
  try {
    file_bytes.assign(kSnapshotHeaderSize + payload.size(), std::byte{0});
  } catch (...) {
    return Status::refused(ReasonCode::RejectedResourceExhausted);
  }
  std::memcpy(file_bytes.data(), kSnapshotMagic, sizeof(kSnapshotMagic));
  put_u32(file_bytes.data() + 8, kSnapshotContainerVersion);
  put_u32(file_bytes.data() + 12, kStateSemanticsVersion);
  put_u32(file_bytes.data() + 16, 0);
  put_u32(file_bytes.data() + 20, 0);
  put_u64(file_bytes.data() + 24, state.epoch.value());
  put_u64(file_bytes.data() + 32, state.boot.high);
  put_u64(file_bytes.data() + 40, state.boot.low);
  put_u64(file_bytes.data() + 48, static_cast<std::uint64_t>(payload.size()));
  put_u64(file_bytes.data() + 56, next_sequence);
  const Sha256Digest payload_digest = Sha256::of(payload.bytes());
  std::memcpy(file_bytes.data() + 64, payload_digest.bytes.data(), kDigestSize);
  std::memcpy(file_bytes.data() + kSnapshotHeaderSize, payload.bytes().data(), payload.size());
  const Sha256Digest header_digest = Sha256::of(std::span<const std::byte>(file_bytes.data(), 96));
  std::memcpy(file_bytes.data() + 96, header_digest.bytes.data(), kDigestSize);

  run_hook(CrashPoint::AfterJournalFlushBeforeSnapshot);
  run_hook(CrashPoint::AfterSnapshotWriteBeforeRename);
  ReasonCode failure = ReasonCode::Ok;
  if (!write_atomic(snapshot_path_, std::span<const std::byte>(file_bytes.data(), file_bytes.size()),
                    failure)) {
    return Status::refused(failure);
  }
  run_hook(CrashPoint::AfterSnapshotRenameBeforeJournalRewrite);
  commit_sequence_ = next_sequence;
  snapshot_digest_ = payload_digest;
  if (!rewrite_journal(next_sequence, payload_digest, failure)) {
    return Status::refused(failure);
  }
  if (!open_journal_for_append(failure)) {
    return Status::refused(failure);
  }
  run_hook(CrashPoint::AfterJournalRewriteBeforeAcknowledge);
  return Status::accepted(ReasonCode::RecoveryCompacted);
}

void Store::close() {
  if (file_handle_ != nullptr) {
    std::fclose(static_cast<std::FILE*>(file_handle_));
    file_handle_ = nullptr;
  }
  open_ = false;
}

}  // namespace flow_offload
