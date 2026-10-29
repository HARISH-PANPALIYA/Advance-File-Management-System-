#pragma once
#include "journal.hpp"
#include "filesystem_service.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace fileguard {

struct StorageUsage {
    uint64_t quarantine_bytes{0};
    uint64_t snapshots_bytes{0};
    uint64_t total_bytes{0};
    size_t   quarantine_items_count{0};
    size_t   snapshots_count{0};
};

class RecoveryService {
public:
    RecoveryService(Journal& journal, FilesystemService& fs_service, std::filesystem::path storage_root);

    // Quarantine management
    QuarantineEntry quarantine_item(const std::filesystem::path& path, const std::string& op_id = "");
    bool restore_quarantined_item(const std::string& qid, bool overwrite = false, const std::filesystem::path& custom_destination = "");
    bool permanently_delete_quarantined_item(const std::string& qid);
    std::vector<QuarantineEntry> list_quarantine() const;
    std::optional<QuarantineEntry> get_quarantine_item(const std::string& qid) const;

    // File snapshot versions
    VersionRecord create_snapshot(const std::filesystem::path& path, const std::string& source_op = "");
    bool restore_snapshot(int64_t version_id, bool backup_current = true);
    bool restore_snapshot_to(int64_t version_id, const std::filesystem::path& target_dest, bool backup_current = false);
    std::vector<VersionRecord> list_versions(const std::filesystem::path& path) const;
    std::optional<VersionRecord> get_version(int64_t version_id) const;

    // Integrity and storage verification
    bool verify_integrity(const std::filesystem::path& path, const std::string& expected_hash) const;
    StorageUsage get_storage_usage() const;
    void clean_retention(size_t max_snapshots_per_file = 20, int days_quarantine_retention = 30);

    const std::filesystem::path& get_quarantine_dir() const { return quarantine_dir_; }
    const std::filesystem::path& get_snapshots_dir() const { return snapshots_dir_; }

private:
    Journal&           journal_;
    FilesystemService& fs_service_;
    std::filesystem::path storage_root_;
    std::filesystem::path quarantine_dir_;
    std::filesystem::path snapshots_dir_;
};

} // namespace fileguard
