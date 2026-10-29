#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <memory>

struct sqlite3;

namespace fileguard {

enum class EventKind { Create, Modify, Delete, Rename };

std::string event_kind_str(EventKind k);
EventKind   event_kind_from_str(const std::string& s);

struct EventRecord {
    int64_t                              id{0};
    std::string                          kind;
    std::filesystem::path                path;
    std::optional<std::filesystem::path> old_path;
    std::string                          timestamp; // RFC-3339 UTC
    uint32_t                             pid{0};
    bool                                 is_external{true};

    static EventRecord now(EventKind k,
                           std::filesystem::path path,
                           std::optional<std::filesystem::path> old_path = std::nullopt,
                           uint32_t pid = 0,
                           bool is_external = true);
};

#if defined(_WIN32)
#  ifdef CreateFile
#    undef CreateFile
#  endif
#  ifdef CreateDirectory
#    undef CreateDirectory
#  endif
#  ifdef DeleteFile
#    undef DeleteFile
#  endif
#  ifdef CopyFile
#    undef CopyFile
#  endif
#  ifdef MoveFile
#    undef MoveFile
#  endif
#endif

enum class OpType {
    OpCreateFile,
    OpCreateDir,
    OpRename,
    OpMove,
    OpCopy,
    OpDelete,
    OpOverwrite
};

std::string op_type_str(OpType t);
OpType      op_type_from_str(const std::string& s);

enum class OpStatus {
    Intent,
    Executed,
    Undone,
    Redone,
    Failed,
    Conflict,
    Superseded
};

std::string op_status_str(OpStatus s);
OpStatus    op_status_from_str(const std::string& s);

struct OperationRecord {
    std::string           id;
    int64_t               seq{0};
    OpType                type{OpType::OpCreateFile};
    OpStatus              status{OpStatus::Intent};
    std::string           source_path;
    std::string           dest_path;
    std::string           transaction_id;
    std::string           created_at;
    std::string           completed_at;
    std::string           expected_source_hash;
    std::string           expected_dest_hash;
    std::string           recovery_artifact_path;
    std::string           details_json;
    bool                  is_undoable{true};
    bool                  is_undone{false};
    std::string           error_message;
};

struct TransactionRecord {
    std::string id;
    std::string type;
    std::string status;
    int32_t     total_ops{0};
    int32_t     completed_ops{0};
    int32_t     failed_ops{0};
    std::string created_at;
    std::string completed_at;
    std::string error_message;
};

struct VersionRecord {
    int64_t     id{0};
    std::string path;
    std::string created_at;
    uint64_t    size_bytes{0};
    std::string sha256_hash;
    std::string snapshot_path;
    std::string source_op_id;
};

struct QuarantineEntry {
    std::string id;
    std::string original_path;
    std::string quarantine_path;
    std::string quarantined_at;
    uint64_t    size_bytes{0};
    bool        is_directory{false};
    std::string sha256_hash;
    bool        is_restored{false};
    std::string operation_id;
};

struct BenchmarkRecord {
    std::string id;
    std::string type;
    std::string timestamp;
    double      duration_ms{0.0};
    int64_t     items_processed{0};
    double      throughput{0.0};
    std::string results_json;
};

// Phase 2 New Persistent Structures
struct ManagedRootRecord {
    std::string path;
    std::string name;
    bool        is_monitored{true};
    bool        is_demo{false};
    std::string added_at;
};

struct FileIdentityRecord {
    std::string id;            // e.g. "FID-0001"
    std::string current_path;
    std::string initial_path;
    std::string created_at;
    std::string last_modified_at;
    int64_t     file_index{0};
};

struct NameLocationRecord {
    std::string id;            // e.g. "OP-0001"
    std::string file_identity_id;
    int64_t     seq{0};
    std::string op_type;       // "CREATE", "RENAME", "MOVE"
    std::string source_path;
    std::string dest_path;
    std::string created_at;
    std::string source_op_id;
};

struct ContentRevisionRecord {
    std::string id;            // e.g. "REV-0001"
    std::string file_identity_id;
    int64_t     revision_number{1}; // 1, 2, 3...
    std::string path;
    std::string created_at;
    uint64_t    size_bytes{0};
    std::string sha256_hash;
    int64_t     snapshot_id{0};
    std::string source_type;   // "USER_EDIT", "UNDO_RESTORE", "EXTERNAL_ACCEPTED", "INITIAL_CREATE"
    std::string change_summary;
};

struct ExternalCandidateRecord {
    std::string id;            // e.g. "X-0001"
    int64_t     detection_seq{0};
    std::string path;
    std::string old_path;      // Original path before external rename
    std::string file_identity_id;
    std::string change_type;   // "EXTERNAL_MODIFY", "EXTERNAL_CREATE", "EXTERNAL_DELETE", "EXTERNAL_RENAME"
    std::string detected_at;
    std::string status;        // "PENDING", "ACCEPTED", "REJECTED", "RECOVERED"
    std::string candidate_snapshot_path;
    std::string candidate_hash;
    uint64_t    candidate_size{0};
    std::string prior_accepted_hash;
    int64_t     prior_accepted_snapshot_id{0};
    std::string resolution_notes;
    std::string resolved_at;
};

class Journal {
public:
    explicit Journal(const std::filesystem::path& db_path);
    ~Journal();

    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&&) noexcept;
    Journal& operator=(Journal&&) noexcept;

    // Schema and transaction utilities
    void init_schema();
    void begin_transaction();
    void commit_transaction();
    void rollback_transaction();

    // Event log
    int64_t append_event(const EventRecord& ev);
    std::vector<EventRecord> history(std::optional<std::filesystem::path> path = std::nullopt, size_t limit = 100) const;
    std::vector<EventRecord> history_under(const std::filesystem::path& root, size_t limit = 100) const;

    // Operation journal for undo/redo
    void log_operation_intent(const OperationRecord& op);
    void update_operation_status(const std::string& op_id, OpStatus status, const std::string& error_msg = "");
    void set_operation_completed(const std::string& op_id, const std::string& completed_at);
    void update_operation_execution(const std::string& op_id,
                                    const std::string& completed_at,
                                    const std::string& dest_hash = "",
                                    const std::string& artifact_path = "",
                                    const std::string& details_json = "");
    void set_operation_recovery_artifact(const std::string& op_id, const std::string& artifact_path);
    std::optional<OperationRecord> get_operation(const std::string& op_id) const;
    std::vector<OperationRecord> get_operations(size_t limit = 100) const;
    std::optional<OperationRecord> get_last_undoable_operation() const;
    std::optional<OperationRecord> get_last_redoable_operation() const;
    std::optional<OperationRecord> get_last_undoable_content_operation() const;
    std::optional<OperationRecord> get_last_redoable_content_operation() const;
    std::optional<OperationRecord> get_last_undoable_name_operation() const;
    std::optional<OperationRecord> get_last_redoable_name_operation() const;
    void mark_operation_undone(const std::string& op_id);
    void mark_operation_redone(const std::string& op_id);
    void invalidate_redo_branch_after(int64_t seq);
    std::vector<OperationRecord> get_operations_for_transaction(const std::string& tx_id) const;

    // Batch Transactions
    void create_transaction(const TransactionRecord& tx);
    void update_transaction_progress(const std::string& tx_id, int completed, int failed, const std::string& status, const std::string& error = "");
    std::optional<TransactionRecord> get_transaction(const std::string& tx_id) const;
    std::vector<TransactionRecord> list_transactions(size_t limit = 50) const;

    // File Versions
    int64_t save_version_record(const VersionRecord& ver);
    std::vector<VersionRecord> list_versions(const std::filesystem::path& path) const;
    std::optional<VersionRecord> get_version_by_id(int64_t version_id) const;

    // Quarantine
    void save_quarantine_entry(const QuarantineEntry& qe);
    std::vector<QuarantineEntry> list_quarantine_entries(bool include_restored = false) const;
    std::optional<QuarantineEntry> get_quarantine_entry(const std::string& qid) const;
    void mark_quarantine_restored(const std::string& qid);
    void delete_quarantine_entry(const std::string& qid);

    // Managed Roots
    void add_managed_root(const ManagedRootRecord& root);
    void remove_managed_root(const std::string& path);
    std::vector<ManagedRootRecord> list_managed_roots() const;
    bool is_path_in_managed_roots(const std::string& path) const;

    // File Identity
    FileIdentityRecord get_or_create_file_identity(const std::string& path);
    void update_file_identity_path(const std::string& id, const std::string& new_path);
    std::optional<FileIdentityRecord> get_file_identity(const std::string& id) const;
    std::optional<FileIdentityRecord> find_file_identity_by_path(const std::string& path) const;

    // Independent Name/Location History
    void record_name_location(const NameLocationRecord& rec);
    std::vector<NameLocationRecord> get_name_location_history(const std::string& file_identity_id) const;
    std::vector<NameLocationRecord> get_name_location_history_for_path(const std::string& path) const;

    // Independent Content Revisions
    void record_content_revision(const ContentRevisionRecord& rev);
    std::vector<ContentRevisionRecord> get_content_revisions(const std::string& file_identity_id) const;
    std::vector<ContentRevisionRecord> get_content_revisions_for_path(const std::string& path) const;
    int64_t get_next_revision_number(const std::string& file_identity_id) const;

    // External Candidates Workflow
    void record_external_candidate(const ExternalCandidateRecord& cand);
    std::vector<ExternalCandidateRecord> list_external_candidates(const std::string& status_filter = "") const;
    std::optional<ExternalCandidateRecord> get_external_candidate(const std::string& id) const;
    std::vector<ExternalCandidateRecord> get_external_candidates_for_identity(const std::string& file_identity_id) const;
    std::vector<ExternalCandidateRecord> get_external_candidates_for_path(const std::string& path) const;
    void update_external_candidate_status(const std::string& id, const std::string& status, const std::string& notes = "");
    int64_t get_next_external_seq();

    // Specific Operation Queries for File / Folder View
    std::vector<OperationRecord> get_operations_for_file(const std::string& path, size_t limit = 100) const;
    std::vector<OperationRecord> get_operations_for_folder(const std::string& folder_path, size_t limit = 100) const;

    // Sequential ID Generation
    std::string generate_sequential_id(const std::string& prefix);

    // Benchmarks
    void save_benchmark(const BenchmarkRecord& rec);
    std::vector<BenchmarkRecord> list_benchmarks(size_t limit = 50) const;
    std::optional<BenchmarkRecord> get_benchmark(const std::string& id) const;

    // Settings
    std::string get_setting(const std::string& key, const std::string& default_val = "") const;
    void set_setting(const std::string& key, const std::string& val);

    // Legacy backwards-compatibility for existing CLI
    int64_t append(const EventRecord& ev) { return append_event(ev); }
    int64_t save_version(const std::filesystem::path& path, const std::vector<uint8_t>& content);
    std::vector<VersionRecord> versions(const std::filesystem::path& path) const { return list_versions(path); }
    std::pair<std::filesystem::path, std::vector<uint8_t>> version_content(int64_t version_id) const;

private:
    sqlite3* db_{nullptr};
    std::filesystem::path db_path_;
    void exec(const char* sql) const;
};

} // namespace fileguard

// Bring common types into global namespace for compatibility with existing files if needed
using fileguard::EventKind;
using fileguard::EventRecord;
using fileguard::VersionRecord;
using fileguard::Journal;
