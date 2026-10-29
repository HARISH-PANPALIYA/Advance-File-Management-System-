#include "recovery_service.hpp"
#include "sha256.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <random>

namespace fileguard {

namespace {

std::string generate_id(const std::string& prefix) {
    static std::mt19937_64 rng(std::random_device{}());
    uint64_t val = rng();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << prefix << "_" << now_ms << "_" << std::hex << val;
    return oss.str();
}

uint64_t calculate_dir_size(const std::filesystem::path& dir, size_t& file_count) {
    uint64_t total = 0;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir,
             std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
            file_count++;
        }
    }
    return total;
}

} // namespace

RecoveryService::RecoveryService(Journal& journal, FilesystemService& fs_service, std::filesystem::path storage_root)
    : journal_(journal), fs_service_(fs_service), storage_root_(std::move(storage_root)) {
    quarantine_dir_ = storage_root_ / "quarantine";
    snapshots_dir_ = storage_root_ / "snapshots";
    std::error_code ec;
    std::filesystem::create_directories(quarantine_dir_, ec);
    std::filesystem::create_directories(snapshots_dir_, ec);
}

QuarantineEntry RecoveryService::quarantine_item(const std::filesystem::path& path, const std::string& op_id) {
    auto valid_path = fs_service_.canonicalize_and_validate(path);
    std::error_code ec;
    if (!std::filesystem::exists(valid_path, ec)) {
        throw std::runtime_error("Item to quarantine does not exist: " + valid_path.string());
    }

    bool is_dir = std::filesystem::is_directory(valid_path, ec);
    std::string qid = generate_id(is_dir ? "qdir" : "qfile");
    std::filesystem::path target_quarantine = quarantine_dir_ / qid;

    uint64_t size_bytes = 0;
    std::string hash;

    if (!is_dir) {
        size_bytes = std::filesystem::file_size(valid_path, ec);
        hash = sha256_file(valid_path);
        // Copy to quarantine first
        std::filesystem::copy_file(valid_path, target_quarantine,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            throw std::runtime_error("Failed to copy item to quarantine: " + ec.message());
        }
    } else {
        std::filesystem::copy(valid_path, target_quarantine,
            std::filesystem::copy_options::recursive, ec);
        if (ec) {
            throw std::runtime_error("Failed to copy directory to quarantine: " + ec.message());
        }
        size_t count = 0;
        size_bytes = calculate_dir_size(target_quarantine, count);
    }

    // Now safely remove from working tree
    fs_service_.remove_item(valid_path);

    QuarantineEntry entry;
    entry.id = qid;
    entry.original_path = valid_path.string();
    entry.quarantine_path = target_quarantine.string();
    entry.size_bytes = size_bytes;
    entry.is_directory = is_dir;
    entry.sha256_hash = hash;
    entry.is_restored = false;
    entry.operation_id = op_id;

    journal_.save_quarantine_entry(entry);
    return entry;
}

bool RecoveryService::restore_quarantined_item(const std::string& qid, bool overwrite, const std::filesystem::path& custom_destination) {
    auto entry_opt = journal_.get_quarantine_entry(qid);
    if (!entry_opt) {
        throw std::runtime_error("Quarantine entry not found: " + qid);
    }
    const auto& entry = *entry_opt;

    std::filesystem::path qpath(entry.quarantine_path);
    std::filesystem::path dest_path = custom_destination.empty() ? std::filesystem::path(entry.original_path) : fs_service_.canonicalize_and_validate(custom_destination);
    std::error_code ec;

    if (!std::filesystem::exists(qpath, ec)) {
        throw std::runtime_error("Quarantined data file is missing: " + qpath.string());
    }

    // Check integrity if file
    if (!entry.is_directory && !entry.sha256_hash.empty()) {
        std::string cur_hash = sha256_file(qpath);
        if (cur_hash != entry.sha256_hash) {
            throw std::runtime_error("Quarantine artifact hash verification failed! Data may be corrupted.");
        }
    }

    // Check destination collision
    if (std::filesystem::exists(dest_path, ec) && !overwrite) {
        throw std::runtime_error("Restore conflict: item already exists at " + dest_path.string());
    }

    if (dest_path.has_parent_path()) {
        std::filesystem::create_directories(dest_path.parent_path(), ec);
    }

    if (!entry.is_directory) {
        std::filesystem::copy_file(qpath, dest_path,
            overwrite ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none, ec);
        if (ec) {
            throw std::runtime_error("Failed to restore quarantined file: " + ec.message());
        }
    } else {
        std::filesystem::copy(qpath, dest_path,
            std::filesystem::copy_options::recursive | (overwrite ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none), ec);
        if (ec) {
            throw std::runtime_error("Failed to restore quarantined directory: " + ec.message());
        }
    }

    journal_.mark_quarantine_restored(qid);
    return true;
}

bool RecoveryService::permanently_delete_quarantined_item(const std::string& qid) {
    auto entry_opt = journal_.get_quarantine_entry(qid);
    if (!entry_opt) {
        throw std::runtime_error("Quarantine entry not found: " + qid);
    }
    std::filesystem::path qpath(entry_opt->quarantine_path);
    std::error_code ec;
    std::filesystem::remove_all(qpath, ec);
    journal_.delete_quarantine_entry(qid);
    return true;
}

std::vector<QuarantineEntry> RecoveryService::list_quarantine() const {
    return journal_.list_quarantine_entries(false);
}

std::optional<QuarantineEntry> RecoveryService::get_quarantine_item(const std::string& qid) const {
    return journal_.get_quarantine_entry(qid);
}

VersionRecord RecoveryService::create_snapshot(const std::filesystem::path& path, const std::string& source_op) {
    auto valid_path = fs_service_.canonicalize_and_validate(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(valid_path, ec)) {
        throw std::runtime_error("Cannot snapshot non-file: " + valid_path.string());
    }

    std::string hash = sha256_file(valid_path);
    uint64_t size = std::filesystem::file_size(valid_path, ec);

    std::string snap_id = generate_id("snap_" + hash.substr(0, 12));
    std::filesystem::path snap_path = snapshots_dir_ / snap_id;

    std::filesystem::copy_file(valid_path, snap_path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("Failed to copy snapshot file: " + ec.message());
    }

    VersionRecord vr;
    vr.path = valid_path.string();
    vr.size_bytes = size;
    vr.sha256_hash = hash;
    vr.snapshot_path = snap_path.string();
    vr.source_op_id = source_op;

    vr.id = journal_.save_version_record(vr);
    return vr;
}

bool RecoveryService::restore_snapshot_to(int64_t version_id, const std::filesystem::path& target_dest, bool backup_current) {
    auto ver_opt = journal_.get_version_by_id(version_id);
    if (!ver_opt) {
        throw std::runtime_error("Version snapshot #" + std::to_string(version_id) + " not found.");
    }
    const auto& ver = *ver_opt;

    std::filesystem::path snap_path(ver.snapshot_path);
    std::filesystem::path target_path = target_dest.empty() ? std::filesystem::path(ver.path) : target_dest;
    std::error_code ec;

    if (!std::filesystem::exists(snap_path, ec)) {
        throw std::runtime_error("Snapshot file missing on disk: " + snap_path.string());
    }

    // Verify snapshot integrity
    if (!ver.sha256_hash.empty()) {
        std::string cur_hash = sha256_file(snap_path);
        if (cur_hash != ver.sha256_hash) {
            throw std::runtime_error("Snapshot integrity verification failed for version #" + std::to_string(version_id));
        }
    }

    // If target currently exists, take a snapshot of it first so we don't lose current state
    if (backup_current && std::filesystem::exists(target_path, ec)) {
        try {
            create_snapshot(target_path, "pre_restore_backup");
        } catch (...) { /* best-effort backup */ }
    }

    if (target_path.has_parent_path()) {
        std::filesystem::create_directories(target_path.parent_path(), ec);
    }

    std::filesystem::copy_file(snap_path, target_path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("Failed to overwrite file with snapshot: " + ec.message());
    }
    return true;
}

bool RecoveryService::restore_snapshot(int64_t version_id, bool backup_current) {
    return restore_snapshot_to(version_id, "", backup_current);
}

std::vector<VersionRecord> RecoveryService::list_versions(const std::filesystem::path& path) const {
    auto valid_path = fs_service_.canonicalize_and_validate(path);
    return journal_.list_versions(valid_path);
}

std::optional<VersionRecord> RecoveryService::get_version(int64_t version_id) const {
    return journal_.get_version_by_id(version_id);
}

bool RecoveryService::verify_integrity(const std::filesystem::path& path, const std::string& expected_hash) const {
    if (expected_hash.empty()) return true;
    std::string actual = sha256_file(path);
    return actual == expected_hash;
}

StorageUsage RecoveryService::get_storage_usage() const {
    StorageUsage usage;
    usage.quarantine_bytes = calculate_dir_size(quarantine_dir_, usage.quarantine_items_count);
    usage.snapshots_bytes = calculate_dir_size(snapshots_dir_, usage.snapshots_count);
    usage.total_bytes = usage.quarantine_bytes + usage.snapshots_bytes;
    return usage;
}

void RecoveryService::clean_retention(size_t max_snapshots_per_file, int days_quarantine_retention) {
    // Basic retention cleanup utility
    (void)max_snapshots_per_file;
    (void)days_quarantine_retention;
}

} // namespace fileguard
