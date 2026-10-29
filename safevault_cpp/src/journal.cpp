#include "journal.hpp"
#include "sha256.hpp"
#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <algorithm>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace fileguard {

namespace {

std::string utc_now_rfc3339() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &tt);
#else
    gmtime_r(&tt, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

uint32_t current_pid() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

struct ScopedStmt {
    sqlite3_stmt* s{nullptr};
    explicit ScopedStmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare error: ") + sqlite3_errmsg(db) + " in " + sql);
        }
    }
    ~ScopedStmt() { if (s) sqlite3_finalize(s); }
    ScopedStmt(const ScopedStmt&) = delete;
    ScopedStmt& operator=(const ScopedStmt&) = delete;
};

std::string get_text_col(sqlite3_stmt* s, int col) {
    const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(s, col));
    return txt ? std::string(txt) : std::string();
}

} // namespace

std::string event_kind_str(EventKind k) {
    switch (k) {
        case EventKind::Create: return "CREATE";
        case EventKind::Modify: return "MODIFY";
        case EventKind::Delete: return "DELETE";
        case EventKind::Rename: return "RENAME";
    }
    return "UNKNOWN";
}

EventKind event_kind_from_str(const std::string& s) {
    std::string up = s;
    std::transform(up.begin(), up.end(), up.begin(), ::toupper);
    if (up == "CREATE") return EventKind::Create;
    if (up == "MODIFY") return EventKind::Modify;
    if (up == "DELETE") return EventKind::Delete;
    if (up == "RENAME") return EventKind::Rename;
    throw std::runtime_error("Unknown event kind: " + s);
}

EventRecord EventRecord::now(EventKind k,
                             std::filesystem::path p,
                             std::optional<std::filesystem::path> old_p,
                             uint32_t pid,
                             bool is_external) {
    EventRecord r;
    r.id          = 0;
    r.kind        = event_kind_str(k);
    r.path        = std::move(p);
    r.old_path    = std::move(old_p);
    r.timestamp   = utc_now_rfc3339();
    r.pid         = pid ? pid : current_pid();
    r.is_external = is_external;
    return r;
}

std::string op_type_str(OpType t) {
    switch (t) {
        case OpType::OpCreateFile: return "CREATE_FILE";
        case OpType::OpCreateDir:  return "CREATE_DIR";
        case OpType::OpRename:     return "RENAME";
        case OpType::OpMove:       return "MOVE";
        case OpType::OpCopy:       return "COPY";
        case OpType::OpDelete:     return "DELETE";
        case OpType::OpOverwrite:  return "OVERWRITE";
    }
    return "UNKNOWN";
}

OpType op_type_from_str(const std::string& s) {
    if (s == "CREATE_FILE") return OpType::OpCreateFile;
    if (s == "CREATE_DIR")  return OpType::OpCreateDir;
    if (s == "RENAME")      return OpType::OpRename;
    if (s == "MOVE")        return OpType::OpMove;
    if (s == "COPY")        return OpType::OpCopy;
    if (s == "DELETE")      return OpType::OpDelete;
    if (s == "OVERWRITE")   return OpType::OpOverwrite;
    throw std::runtime_error("Unknown op type: " + s);
}

std::string op_status_str(OpStatus s) {
    switch (s) {
        case OpStatus::Intent:     return "INTENT";
        case OpStatus::Executed:   return "EXECUTED";
        case OpStatus::Undone:     return "UNDONE";
        case OpStatus::Redone:     return "REDONE";
        case OpStatus::Failed:     return "FAILED";
        case OpStatus::Conflict:   return "CONFLICT";
        case OpStatus::Superseded: return "SUPERSEDED";
    }
    return "UNKNOWN";
}

OpStatus op_status_from_str(const std::string& s) {
    if (s == "INTENT")     return OpStatus::Intent;
    if (s == "EXECUTED")   return OpStatus::Executed;
    if (s == "UNDONE")     return OpStatus::Undone;
    if (s == "REDONE")     return OpStatus::Redone;
    if (s == "FAILED")     return OpStatus::Failed;
    if (s == "CONFLICT")   return OpStatus::Conflict;
    if (s == "SUPERSEDED") return OpStatus::Superseded;
    throw std::runtime_error("Unknown op status: " + s);
}

Journal::Journal(const std::filesystem::path& db_path) : db_path_(db_path) {
    std::error_code ec;
    if (db_path.has_parent_path()) {
        std::filesystem::create_directories(db_path.parent_path(), ec);
    }

    if (sqlite3_open(db_path.string().c_str(), &db_) != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "Failed to open SQLite database";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(err);
    }

    init_schema();
}

Journal::~Journal() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Journal::Journal(Journal&& other) noexcept : db_(other.db_), db_path_(std::move(other.db_path_)) {
    other.db_ = nullptr;
}

Journal& Journal::operator=(Journal&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = other.db_;
        db_path_ = std::move(other.db_path_);
        other.db_ = nullptr;
    }
    return *this;
}

void Journal::exec(const char* sql) const {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "SQLite exec failed";
        sqlite3_free(err);
        throw std::runtime_error(msg + " in: " + sql);
    }
}

void Journal::init_schema() {
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");

    // Schema migrations
    exec("CREATE TABLE IF NOT EXISTS schema_migrations ("
         "version INTEGER PRIMARY KEY, "
         "applied_at TEXT NOT NULL"
         ");");

    // Operations log for undo/redo
    exec("CREATE TABLE IF NOT EXISTS operations ("
         "id TEXT PRIMARY KEY, "
         "seq INTEGER NOT NULL, "
         "type TEXT NOT NULL, "
         "status TEXT NOT NULL, "
         "source_path TEXT, "
         "dest_path TEXT, "
         "transaction_id TEXT, "
         "created_at TEXT NOT NULL, "
         "completed_at TEXT, "
         "expected_source_hash TEXT, "
         "expected_dest_hash TEXT, "
         "recovery_artifact_path TEXT, "
         "details_json TEXT, "
         "is_undoable INTEGER NOT NULL DEFAULT 1, "
         "is_undone INTEGER NOT NULL DEFAULT 0, "
         "error_message TEXT"
         ");");
    exec("CREATE INDEX IF NOT EXISTS idx_ops_seq ON operations(seq);");
    exec("CREATE INDEX IF NOT EXISTS idx_ops_tx ON operations(transaction_id);");

    // Multi-file batch transactions
    exec("CREATE TABLE IF NOT EXISTS transactions ("
         "id TEXT PRIMARY KEY, "
         "type TEXT NOT NULL, "
         "status TEXT NOT NULL, "
         "total_ops INTEGER NOT NULL, "
         "completed_ops INTEGER NOT NULL DEFAULT 0, "
         "failed_ops INTEGER NOT NULL DEFAULT 0, "
         "created_at TEXT NOT NULL, "
         "completed_at TEXT, "
         "error_message TEXT"
         ");");

    // File snapshot versions
    exec("CREATE TABLE IF NOT EXISTS file_versions ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT, "
         "path TEXT NOT NULL, "
         "created_at TEXT NOT NULL, "
         "size_bytes INTEGER NOT NULL, "
         "sha256_hash TEXT, "
         "snapshot_path TEXT, "
         "source_op_id TEXT"
         ");");
    exec("CREATE INDEX IF NOT EXISTS idx_versions_path ON file_versions(path);");

    // Quarantine items
    exec("CREATE TABLE IF NOT EXISTS quarantine_entries ("
         "id TEXT PRIMARY KEY, "
         "original_path TEXT NOT NULL, "
         "quarantine_path TEXT NOT NULL, "
         "quarantined_at TEXT NOT NULL, "
         "size_bytes INTEGER NOT NULL, "
         "is_directory INTEGER NOT NULL DEFAULT 0, "
         "sha256_hash TEXT, "
         "is_restored INTEGER NOT NULL DEFAULT 0, "
         "operation_id TEXT"
         ");");

    // Filesystem events (from native watcher)
    exec("CREATE TABLE IF NOT EXISTS filesystem_events ("
         "id INTEGER PRIMARY KEY AUTOINCREMENT, "
         "kind TEXT NOT NULL, "
         "path TEXT NOT NULL, "
         "old_path TEXT, "
         "timestamp TEXT NOT NULL, "
         "pid INTEGER NOT NULL, "
         "is_external INTEGER NOT NULL DEFAULT 1"
         ");");
    exec("CREATE INDEX IF NOT EXISTS idx_events_path ON filesystem_events(path);");

    // Performance Benchmarks
    exec("CREATE TABLE IF NOT EXISTS benchmark_runs ("
         "id TEXT PRIMARY KEY, "
         "type TEXT NOT NULL, "
         "timestamp TEXT NOT NULL, "
         "duration_ms REAL NOT NULL, "
         "items_processed INTEGER NOT NULL, "
         "throughput REAL NOT NULL, "
         "results_json TEXT"
         ");");

    // Settings
    exec("CREATE TABLE IF NOT EXISTS settings ("
         "key TEXT PRIMARY KEY, "
         "value TEXT NOT NULL"
         ");");

    // Sequence Counters for sequential ID generation (e.g. X-0001, REV-0001)
    exec("CREATE TABLE IF NOT EXISTS sequence_counters ("
         "prefix TEXT PRIMARY KEY, "
         "current_val INTEGER NOT NULL DEFAULT 0"
         ");");

    // Managed Roots (drives, watched folders, user additions)
    exec("CREATE TABLE IF NOT EXISTS managed_roots ("
         "path TEXT PRIMARY KEY, "
         "name TEXT NOT NULL, "
         "is_monitored INTEGER NOT NULL DEFAULT 1, "
         "is_demo INTEGER NOT NULL DEFAULT 0, "
         "added_at TEXT NOT NULL"
         ");");

    // File Identities (stable logical identity surviving renames/moves)
    exec("CREATE TABLE IF NOT EXISTS file_identities ("
         "id TEXT PRIMARY KEY, "
         "current_path TEXT NOT NULL, "
         "initial_path TEXT NOT NULL, "
         "created_at TEXT NOT NULL, "
         "last_modified_at TEXT NOT NULL, "
         "file_index INTEGER NOT NULL DEFAULT 0"
         ");");
    exec("CREATE INDEX IF NOT EXISTS idx_fids_path ON file_identities(current_path);");

    // Name and Location History (OP-0001, distinct from content revisions)
    exec("CREATE TABLE IF NOT EXISTS name_location_history ("
         "id TEXT PRIMARY KEY, "
         "file_identity_id TEXT NOT NULL, "
         "seq INTEGER NOT NULL, "
         "op_type TEXT NOT NULL, "
         "source_path TEXT NOT NULL, "
         "dest_path TEXT NOT NULL, "
         "created_at TEXT NOT NULL, "
         "source_op_id TEXT"
         ");");
    exec("CREATE INDEX IF NOT EXISTS idx_nl_fid ON name_location_history(file_identity_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_nl_dest ON name_location_history(dest_path);");

    // Content Revisions (REV-0001, monotonically increasing revisions)
    exec("CREATE TABLE IF NOT EXISTS content_revisions ("
         "id TEXT PRIMARY KEY, "
         "file_identity_id TEXT NOT NULL, "
         "revision_number INTEGER NOT NULL, "
         "path TEXT NOT NULL, "
         "created_at TEXT NOT NULL, "
         "size_bytes INTEGER NOT NULL, "
         "sha256_hash TEXT NOT NULL, "
         "snapshot_id INTEGER NOT NULL DEFAULT 0, "
         "source_type TEXT NOT NULL, "
         "change_summary TEXT"
         ");");
    exec("CREATE INDEX IF NOT EXISTS idx_rev_fid ON content_revisions(file_identity_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_rev_path ON content_revisions(path);");

    // External Candidates (X-0001: External changes inbox)
    exec("CREATE TABLE IF NOT EXISTS external_candidates ("
         "id TEXT PRIMARY KEY, "
         "detection_seq INTEGER NOT NULL, "
         "path TEXT NOT NULL, "
         "old_path TEXT, "
         "file_identity_id TEXT, "
         "change_type TEXT NOT NULL, "
         "detected_at TEXT NOT NULL, "
         "status TEXT NOT NULL DEFAULT 'PENDING', "
         "candidate_snapshot_path TEXT, "
         "candidate_hash TEXT, "
         "candidate_size INTEGER DEFAULT 0, "
         "prior_accepted_hash TEXT, "
         "prior_accepted_snapshot_id INTEGER DEFAULT 0, "
         "resolution_notes TEXT, "
         "resolved_at TEXT"
         ");");
    exec("CREATE INDEX IF NOT EXISTS idx_ext_status ON external_candidates(status);");
    exec("CREATE INDEX IF NOT EXISTS idx_ext_path ON external_candidates(path);");

    // Seamless migration for existing database schema
    {
        char* err_msg = nullptr;
        sqlite3_exec(db_, "ALTER TABLE external_candidates ADD COLUMN old_path TEXT;", nullptr, nullptr, &err_msg);
        if (err_msg) {
            sqlite3_free(err_msg);
        }
    }

    // Check migration version
    ScopedStmt s(db_, "INSERT OR IGNORE INTO schema_migrations (version, applied_at) VALUES (2, ?);");
    std::string now = utc_now_rfc3339();
    sqlite3_bind_text(s.s, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::begin_transaction() { exec("BEGIN TRANSACTION;"); }
void Journal::commit_transaction() { exec("COMMIT;"); }
void Journal::rollback_transaction() { exec("ROLLBACK;"); }

int64_t Journal::append_event(const EventRecord& ev) {
    const char* sql = "INSERT INTO filesystem_events (kind, path, old_path, timestamp, pid, is_external) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, ev.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, ev.path.string().c_str(), -1, SQLITE_TRANSIENT);
    if (ev.old_path) {
        sqlite3_bind_text(s.s, 3, ev.old_path->string().c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(s.s, 3);
    }
    sqlite3_bind_text(s.s, 4, ev.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 5, ev.pid);
    sqlite3_bind_int(s.s, 6, ev.is_external ? 1 : 0);

    if (sqlite3_step(s.s) != SQLITE_DONE) {
        throw std::runtime_error(std::string("append_event step: ") + sqlite3_errmsg(db_));
    }
    return sqlite3_last_insert_rowid(db_);
}

std::vector<EventRecord> Journal::history(std::optional<std::filesystem::path> path, size_t limit) const {
    std::vector<EventRecord> list;
    std::string sql = "SELECT id, kind, path, old_path, timestamp, pid, is_external FROM filesystem_events ";
    if (path) sql += "WHERE path = ? ";
    sql += "ORDER BY id DESC LIMIT ?;";

    ScopedStmt s(db_, sql.c_str());
    int param = 1;
    if (path) {
        sqlite3_bind_text(s.s, param++, path->string().c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(s.s, param, static_cast<int64_t>(limit));

    while (sqlite3_step(s.s) == SQLITE_ROW) {
        EventRecord r;
        r.id = sqlite3_column_int64(s.s, 0);
        r.kind = get_text_col(s.s, 1);
        r.path = get_text_col(s.s, 2);
        if (sqlite3_column_type(s.s, 3) != SQLITE_NULL) {
            r.old_path = get_text_col(s.s, 3);
        }
        r.timestamp = get_text_col(s.s, 4);
        r.pid = static_cast<uint32_t>(sqlite3_column_int64(s.s, 5));
        r.is_external = sqlite3_column_int(s.s, 6) != 0;
        list.push_back(std::move(r));
    }
    return list;
}

std::vector<EventRecord> Journal::history_under(const std::filesystem::path& root, size_t limit) const {
    std::vector<EventRecord> list;
    std::string sql = "SELECT id, kind, path, old_path, timestamp, pid, is_external FROM filesystem_events "
                      "WHERE path LIKE ? ORDER BY id DESC LIMIT ?;";
    ScopedStmt s(db_, sql.c_str());
    std::string pattern = root.string() + "%";
    sqlite3_bind_text(s.s, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 2, static_cast<int64_t>(limit));

    while (sqlite3_step(s.s) == SQLITE_ROW) {
        EventRecord r;
        r.id = sqlite3_column_int64(s.s, 0);
        r.kind = get_text_col(s.s, 1);
        r.path = get_text_col(s.s, 2);
        if (sqlite3_column_type(s.s, 3) != SQLITE_NULL) {
            r.old_path = get_text_col(s.s, 3);
        }
        r.timestamp = get_text_col(s.s, 4);
        r.pid = static_cast<uint32_t>(sqlite3_column_int64(s.s, 5));
        r.is_external = sqlite3_column_int(s.s, 6) != 0;
        list.push_back(std::move(r));
    }
    return list;
}

void Journal::log_operation_intent(const OperationRecord& op) {
    // Determine next monotonic seq
    int64_t next_seq = 1;
    {
        ScopedStmt seq_stmt(db_, "SELECT COALESCE(MAX(seq), 0) + 1 FROM operations;");
        if (sqlite3_step(seq_stmt.s) == SQLITE_ROW) {
            next_seq = sqlite3_column_int64(seq_stmt.s, 0);
        }
    }

    const char* sql = "INSERT INTO operations (id, seq, type, status, source_path, dest_path, "
                      "transaction_id, created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, op.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 2, next_seq);
    sqlite3_bind_text(s.s, 3, op_type_str(op.type).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, op_status_str(op.status).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 5, op.source_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 6, op.dest_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 7, op.transaction_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 8, op.created_at.empty() ? utc_now_rfc3339().c_str() : op.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 9, op.completed_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 10, op.expected_source_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 11, op.expected_dest_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 12, op.recovery_artifact_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 13, op.details_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s.s, 14, op.is_undoable ? 1 : 0);
    sqlite3_bind_int(s.s, 15, op.is_undone ? 1 : 0);
    sqlite3_bind_text(s.s, 16, op.error_message.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(s.s) != SQLITE_DONE) {
        throw std::runtime_error(std::string("log_operation_intent step: ") + sqlite3_errmsg(db_));
    }
}

void Journal::update_operation_status(const std::string& op_id, OpStatus status, const std::string& error_msg) {
    const char* sql = "UPDATE operations SET status = ?, error_message = ? WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, op_status_str(status).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, error_msg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, op_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::set_operation_completed(const std::string& op_id, const std::string& completed_at) {
    const char* sql = "UPDATE operations SET status = 'EXECUTED', completed_at = ? WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, completed_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, op_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::update_operation_execution(const std::string& op_id,
                                         const std::string& completed_at,
                                         const std::string& dest_hash,
                                         const std::string& artifact_path,
                                         const std::string& details_json) {
    const char* sql = "UPDATE operations SET status = 'EXECUTED', completed_at = ?, "
                      "expected_dest_hash = CASE WHEN ? != '' THEN ? ELSE expected_dest_hash END, "
                      "recovery_artifact_path = CASE WHEN ? != '' THEN ? ELSE recovery_artifact_path END, "
                      "details_json = CASE WHEN ? != '' THEN ? ELSE details_json END "
                      "WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, completed_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, dest_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, dest_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, artifact_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 5, artifact_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 6, details_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 7, details_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 8, op_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::set_operation_recovery_artifact(const std::string& op_id, const std::string& artifact_path) {
    const char* sql = "UPDATE operations SET recovery_artifact_path = ? WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, artifact_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, op_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::optional<OperationRecord> Journal::get_operation(const std::string& op_id) const {
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, op_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        return r;
    }
    return std::nullopt;
}

std::vector<OperationRecord> Journal::get_operations(size_t limit) const {
    std::vector<OperationRecord> list;
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations ORDER BY seq DESC LIMIT ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_int64(s.s, 1, static_cast<int64_t>(limit));
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        list.push_back(std::move(r));
    }
    return list;
}

std::optional<OperationRecord> Journal::get_last_undoable_operation() const {
    // The most recently executed, non-undone operation that is undoable
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE is_undoable = 1 AND is_undone = 0 AND (status = 'EXECUTED' OR status = 'REDONE') "
                      "ORDER BY seq DESC LIMIT 1;";
    ScopedStmt s(db_, sql);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        return r;
    }
    return std::nullopt;
}

std::optional<OperationRecord> Journal::get_last_redoable_operation() const {
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE is_undoable = 1 AND is_undone = 1 AND status = 'UNDONE' "
                      "ORDER BY seq ASC LIMIT 1;";
    ScopedStmt s(db_, sql);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        return r;
    }
    return std::nullopt;
}

std::optional<OperationRecord> Journal::get_last_undoable_content_operation() const {
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE is_undoable = 1 AND is_undone = 0 AND (status = 'EXECUTED' OR status = 'REDONE') "
                      "AND type = 'OVERWRITE' "
                      "ORDER BY seq DESC LIMIT 1;";
    ScopedStmt s(db_, sql);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        return r;
    }
    return std::nullopt;
}

std::optional<OperationRecord> Journal::get_last_redoable_content_operation() const {
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE is_undoable = 1 AND is_undone = 1 AND status = 'UNDONE' "
                      "AND type = 'OVERWRITE' "
                      "ORDER BY seq ASC LIMIT 1;";
    ScopedStmt s(db_, sql);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        return r;
    }
    return std::nullopt;
}

std::optional<OperationRecord> Journal::get_last_undoable_name_operation() const {
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE is_undoable = 1 AND is_undone = 0 AND (status = 'EXECUTED' OR status = 'REDONE') "
                      "AND (type = 'RENAME' OR type = 'MOVE') "
                      "ORDER BY seq DESC LIMIT 1;";
    ScopedStmt s(db_, sql);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        return r;
    }
    return std::nullopt;
}

std::optional<OperationRecord> Journal::get_last_redoable_name_operation() const {
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE is_undoable = 1 AND is_undone = 1 AND status = 'UNDONE' "
                      "AND (type = 'RENAME' OR type = 'MOVE') "
                      "ORDER BY seq ASC LIMIT 1;";
    ScopedStmt s(db_, sql);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        return r;
    }
    return std::nullopt;
}

void Journal::mark_operation_undone(const std::string& op_id) {
    const char* sql = "UPDATE operations SET status = 'UNDONE', is_undone = 1 WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, op_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::mark_operation_redone(const std::string& op_id) {
    const char* sql = "UPDATE operations SET status = 'REDONE', is_undone = 0 WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, op_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::invalidate_redo_branch_after(int64_t seq) {
    (void)seq;
    // When a new mutation occurs after an undo, any undone operations can no longer be redone
    const char* sql = "UPDATE operations SET is_undoable = 0, is_undone = 0, status = 'SUPERSEDED' WHERE is_undone = 1;";
    ScopedStmt s(db_, sql);
    sqlite3_step(s.s);
}

std::vector<OperationRecord> Journal::get_operations_for_transaction(const std::string& tx_id) const {
    std::vector<OperationRecord> list;
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE transaction_id = ? ORDER BY seq ASC;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, tx_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        list.push_back(std::move(r));
    }
    return list;
}

void Journal::create_transaction(const TransactionRecord& tx) {
    const char* sql = "INSERT INTO transactions (id, type, status, total_ops, completed_ops, failed_ops, created_at, error_message) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, tx.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, tx.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, tx.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s.s, 4, tx.total_ops);
    sqlite3_bind_int(s.s, 5, tx.completed_ops);
    sqlite3_bind_int(s.s, 6, tx.failed_ops);
    sqlite3_bind_text(s.s, 7, tx.created_at.empty() ? utc_now_rfc3339().c_str() : tx.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 8, tx.error_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::update_transaction_progress(const std::string& tx_id, int completed, int failed, const std::string& status, const std::string& error) {
    const char* sql = "UPDATE transactions SET completed_ops = ?, failed_ops = ?, status = ?, completed_at = ?, error_message = ? WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_int(s.s, 1, completed);
    sqlite3_bind_int(s.s, 2, failed);
    sqlite3_bind_text(s.s, 3, status.c_str(), -1, SQLITE_TRANSIENT);
    std::string now = (status == "COMPLETED" || status == "FAILED" || status == "ROLLED_BACK") ? utc_now_rfc3339() : "";
    sqlite3_bind_text(s.s, 4, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 5, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 6, tx_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::optional<TransactionRecord> Journal::get_transaction(const std::string& tx_id) const {
    const char* sql = "SELECT id, type, status, total_ops, completed_ops, failed_ops, created_at, completed_at, error_message "
                      "FROM transactions WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, tx_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        TransactionRecord tx;
        tx.id = get_text_col(s.s, 0);
        tx.type = get_text_col(s.s, 1);
        tx.status = get_text_col(s.s, 2);
        tx.total_ops = sqlite3_column_int(s.s, 3);
        tx.completed_ops = sqlite3_column_int(s.s, 4);
        tx.failed_ops = sqlite3_column_int(s.s, 5);
        tx.created_at = get_text_col(s.s, 6);
        tx.completed_at = get_text_col(s.s, 7);
        tx.error_message = get_text_col(s.s, 8);
        return tx;
    }
    return std::nullopt;
}

int64_t Journal::save_version_record(const VersionRecord& ver) {
    const char* sql = "INSERT INTO file_versions (path, created_at, size_bytes, sha256_hash, snapshot_path, source_op_id) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, ver.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, ver.created_at.empty() ? utc_now_rfc3339().c_str() : ver.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 3, static_cast<int64_t>(ver.size_bytes));
    sqlite3_bind_text(s.s, 4, ver.sha256_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 5, ver.snapshot_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 6, ver.source_op_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) != SQLITE_DONE) {
        throw std::runtime_error(std::string("save_version_record error: ") + sqlite3_errmsg(db_));
    }
    return sqlite3_last_insert_rowid(db_);
}

std::vector<VersionRecord> Journal::list_versions(const std::filesystem::path& path) const {
    std::vector<VersionRecord> list;
    const char* sql = "SELECT id, path, created_at, size_bytes, sha256_hash, snapshot_path, source_op_id "
                      "FROM file_versions WHERE path = ? ORDER BY id DESC;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, path.string().c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        VersionRecord v;
        v.id = sqlite3_column_int64(s.s, 0);
        v.path = get_text_col(s.s, 1);
        v.created_at = get_text_col(s.s, 2);
        v.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(s.s, 3));
        v.sha256_hash = get_text_col(s.s, 4);
        v.snapshot_path = get_text_col(s.s, 5);
        v.source_op_id = get_text_col(s.s, 6);
        list.push_back(std::move(v));
    }
    return list;
}

std::optional<VersionRecord> Journal::get_version_by_id(int64_t version_id) const {
    const char* sql = "SELECT id, path, created_at, size_bytes, sha256_hash, snapshot_path, source_op_id "
                      "FROM file_versions WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_int64(s.s, 1, version_id);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        VersionRecord v;
        v.id = sqlite3_column_int64(s.s, 0);
        v.path = get_text_col(s.s, 1);
        v.created_at = get_text_col(s.s, 2);
        v.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(s.s, 3));
        v.sha256_hash = get_text_col(s.s, 4);
        v.snapshot_path = get_text_col(s.s, 5);
        v.source_op_id = get_text_col(s.s, 6);
        return v;
    }
    return std::nullopt;
}

void Journal::save_quarantine_entry(const QuarantineEntry& qe) {
    const char* sql = "INSERT INTO quarantine_entries (id, original_path, quarantine_path, quarantined_at, size_bytes, is_directory, sha256_hash, is_restored, operation_id) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, qe.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, qe.original_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, qe.quarantine_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, qe.quarantined_at.empty() ? utc_now_rfc3339().c_str() : qe.quarantined_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 5, static_cast<int64_t>(qe.size_bytes));
    sqlite3_bind_int(s.s, 6, qe.is_directory ? 1 : 0);
    sqlite3_bind_text(s.s, 7, qe.sha256_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s.s, 8, qe.is_restored ? 1 : 0);
    sqlite3_bind_text(s.s, 9, qe.operation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::vector<QuarantineEntry> Journal::list_quarantine_entries(bool include_restored) const {
    std::vector<QuarantineEntry> list;
    std::string sql = "SELECT id, original_path, quarantine_path, quarantined_at, size_bytes, is_directory, sha256_hash, is_restored, operation_id "
                      "FROM quarantine_entries ";
    if (!include_restored) {
        sql += "WHERE is_restored = 0 ";
    }
    sql += "ORDER BY quarantined_at DESC;";
    ScopedStmt s(db_, sql.c_str());
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        QuarantineEntry qe;
        qe.id = get_text_col(s.s, 0);
        qe.original_path = get_text_col(s.s, 1);
        qe.quarantine_path = get_text_col(s.s, 2);
        qe.quarantined_at = get_text_col(s.s, 3);
        qe.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(s.s, 4));
        qe.is_directory = sqlite3_column_int(s.s, 5) != 0;
        qe.sha256_hash = get_text_col(s.s, 6);
        qe.is_restored = sqlite3_column_int(s.s, 7) != 0;
        qe.operation_id = get_text_col(s.s, 8);
        list.push_back(std::move(qe));
    }
    return list;
}

std::optional<QuarantineEntry> Journal::get_quarantine_entry(const std::string& qid) const {
    const char* sql = "SELECT id, original_path, quarantine_path, quarantined_at, size_bytes, is_directory, sha256_hash, is_restored, operation_id "
                      "FROM quarantine_entries WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, qid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        QuarantineEntry qe;
        qe.id = get_text_col(s.s, 0);
        qe.original_path = get_text_col(s.s, 1);
        qe.quarantine_path = get_text_col(s.s, 2);
        qe.quarantined_at = get_text_col(s.s, 3);
        qe.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(s.s, 4));
        qe.is_directory = sqlite3_column_int(s.s, 5) != 0;
        qe.sha256_hash = get_text_col(s.s, 6);
        qe.is_restored = sqlite3_column_int(s.s, 7) != 0;
        qe.operation_id = get_text_col(s.s, 8);
        return qe;
    }
    return std::nullopt;
}

void Journal::mark_quarantine_restored(const std::string& qid) {
    const char* sql = "UPDATE quarantine_entries SET is_restored = 1 WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, qid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::delete_quarantine_entry(const std::string& qid) {
    const char* sql = "DELETE FROM quarantine_entries WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, qid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::save_benchmark(const BenchmarkRecord& rec) {
    const char* sql = "INSERT INTO benchmark_runs (id, type, timestamp, duration_ms, items_processed, throughput, results_json) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, rec.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, rec.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, rec.timestamp.empty() ? utc_now_rfc3339().c_str() : rec.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(s.s, 4, rec.duration_ms);
    sqlite3_bind_int64(s.s, 5, rec.items_processed);
    sqlite3_bind_double(s.s, 6, rec.throughput);
    sqlite3_bind_text(s.s, 7, rec.results_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::vector<BenchmarkRecord> Journal::list_benchmarks(size_t limit) const {
    std::vector<BenchmarkRecord> list;
    const char* sql = "SELECT id, type, timestamp, duration_ms, items_processed, throughput, results_json "
                      "FROM benchmark_runs ORDER BY timestamp DESC LIMIT ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_int64(s.s, 1, static_cast<int64_t>(limit));
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        BenchmarkRecord b;
        b.id = get_text_col(s.s, 0);
        b.type = get_text_col(s.s, 1);
        b.timestamp = get_text_col(s.s, 2);
        b.duration_ms = sqlite3_column_double(s.s, 3);
        b.items_processed = sqlite3_column_int64(s.s, 4);
        b.throughput = sqlite3_column_double(s.s, 5);
        b.results_json = get_text_col(s.s, 6);
        list.push_back(std::move(b));
    }
    return list;
}

std::optional<BenchmarkRecord> Journal::get_benchmark(const std::string& id) const {
    const char* sql = "SELECT id, type, timestamp, duration_ms, items_processed, throughput, results_json "
                      "FROM benchmark_runs WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        BenchmarkRecord b;
        b.id = get_text_col(s.s, 0);
        b.type = get_text_col(s.s, 1);
        b.timestamp = get_text_col(s.s, 2);
        b.duration_ms = sqlite3_column_double(s.s, 3);
        b.items_processed = sqlite3_column_int64(s.s, 4);
        b.throughput = sqlite3_column_double(s.s, 5);
        b.results_json = get_text_col(s.s, 6);
        return b;
    }
    return std::nullopt;
}

std::string Journal::get_setting(const std::string& key, const std::string& default_val) const {
    const char* sql = "SELECT value FROM settings WHERE key = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        return get_text_col(s.s, 0);
    }
    return default_val;
}

void Journal::set_setting(const std::string& key, const std::string& val) {
    const char* sql = "INSERT INTO settings (key, value) VALUES (?, ?) "
                      "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, val.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

int64_t Journal::save_version(const std::filesystem::path& path, const std::vector<uint8_t>& content) {
    // Legacy support: compute hash, save to managed folder, record in db
    std::filesystem::path storage_dir = db_path_.parent_path() / "snapshots";
    std::filesystem::create_directories(storage_dir);
    std::string hash = sha256_buffer(content);
    std::string snap_fname = hash + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::filesystem::path snap_file = storage_dir / snap_fname;

    std::ofstream out(snap_file, std::ios::binary);
    if (out && !content.empty()) {
        out.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    }

    VersionRecord vr;
    vr.path = path.string();
    vr.created_at = utc_now_rfc3339();
    vr.size_bytes = content.size();
    vr.sha256_hash = hash;
    vr.snapshot_path = snap_file.string();
    return save_version_record(vr);
}

std::pair<std::filesystem::path, std::vector<uint8_t>> Journal::version_content(int64_t version_id) const {
    auto opt = get_version_by_id(version_id);
    if (!opt) {
        throw std::runtime_error("Version ID not found: " + std::to_string(version_id));
    }
    std::vector<uint8_t> data;
    std::ifstream in(opt->snapshot_path, std::ios::binary);
    if (in) {
        in.seekg(0, std::ios::end);
        size_t sz = static_cast<size_t>(in.tellg());
        in.seekg(0, std::ios::beg);
        data.resize(sz);
        if (sz > 0) {
            in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
        }
    }
    return {std::filesystem::path(opt->path), data};
}

std::vector<TransactionRecord> Journal::list_transactions(size_t limit) const {
    std::vector<TransactionRecord> list;
    const char* sql = "SELECT id, type, status, total_ops, completed_ops, failed_ops, created_at, completed_at, error_message "
                      "FROM transactions ORDER BY created_at DESC LIMIT ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_int64(s.s, 1, static_cast<int64_t>(limit));
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        TransactionRecord tx;
        tx.id = get_text_col(s.s, 0);
        tx.type = get_text_col(s.s, 1);
        tx.status = get_text_col(s.s, 2);
        tx.total_ops = sqlite3_column_int(s.s, 3);
        tx.completed_ops = sqlite3_column_int(s.s, 4);
        tx.failed_ops = sqlite3_column_int(s.s, 5);
        tx.created_at = get_text_col(s.s, 6);
        tx.completed_at = get_text_col(s.s, 7);
        tx.error_message = get_text_col(s.s, 8);
        list.push_back(std::move(tx));
    }
    return list;
}

void Journal::add_managed_root(const ManagedRootRecord& root) {
    const char* sql = "INSERT INTO managed_roots (path, name, is_monitored, is_demo, added_at) VALUES (?, ?, ?, ?, ?) "
                      "ON CONFLICT(path) DO UPDATE SET name = excluded.name, is_monitored = excluded.is_monitored, is_demo = excluded.is_demo;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, root.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, root.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s.s, 3, root.is_monitored ? 1 : 0);
    sqlite3_bind_int(s.s, 4, root.is_demo ? 1 : 0);
    std::string added = root.added_at.empty() ? utc_now_rfc3339() : root.added_at;
    sqlite3_bind_text(s.s, 5, added.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

void Journal::remove_managed_root(const std::string& path) {
    const char* sql = "DELETE FROM managed_roots WHERE path = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::vector<ManagedRootRecord> Journal::list_managed_roots() const {
    std::vector<ManagedRootRecord> list;
    const char* sql = "SELECT path, name, is_monitored, is_demo, added_at FROM managed_roots ORDER BY name ASC;";
    ScopedStmt s(db_, sql);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        ManagedRootRecord r;
        r.path = get_text_col(s.s, 0);
        r.name = get_text_col(s.s, 1);
        r.is_monitored = sqlite3_column_int(s.s, 2) != 0;
        r.is_demo = sqlite3_column_int(s.s, 3) != 0;
        r.added_at = get_text_col(s.s, 4);
        list.push_back(std::move(r));
    }
    return list;
}

bool Journal::is_path_in_managed_roots(const std::string& path) const {
    auto roots = list_managed_roots();
    if (roots.empty()) return true; // Default open if no explicit roots configured
    std::filesystem::path check_p(path);
    std::error_code ec;
    auto canon_check = std::filesystem::weakly_canonical(check_p, ec);
    std::string p_str = (ec ? check_p : canon_check).string();
    std::transform(p_str.begin(), p_str.end(), p_str.begin(), ::tolower);

    for (const auto& r : roots) {
        std::filesystem::path root_p(r.path);
        auto canon_root = std::filesystem::weakly_canonical(root_p, ec);
        std::string r_str = (ec ? root_p : canon_root).string();
        std::transform(r_str.begin(), r_str.end(), r_str.begin(), ::tolower);

        if (p_str == r_str || p_str.rfind(r_str + "\\", 0) == 0 || p_str.rfind(r_str + "/", 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string Journal::generate_sequential_id(const std::string& prefix) {
    const char* inc_sql = "INSERT INTO sequence_counters (prefix, current_val) VALUES (?, 1) "
                          "ON CONFLICT(prefix) DO UPDATE SET current_val = current_val + 1;";
    {
        ScopedStmt s(db_, inc_sql);
        sqlite3_bind_text(s.s, 1, prefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s.s);
    }
    const char* get_sql = "SELECT current_val FROM sequence_counters WHERE prefix = ?;";
    int64_t val = 1;
    {
        ScopedStmt s(db_, get_sql);
        sqlite3_bind_text(s.s, 1, prefix.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.s) == SQLITE_ROW) {
            val = sqlite3_column_int64(s.s, 0);
        }
    }
    std::ostringstream oss;
    oss << prefix << "-";
    int pad = (prefix == "TX") ? 5 : 4;
    oss << std::setw(pad) << std::setfill('0') << val;
    return oss.str();
}

FileIdentityRecord Journal::get_or_create_file_identity(const std::string& path) {
    auto existing = find_file_identity_by_path(path);
    if (existing) {
        return *existing;
    }

    std::string fid = generate_sequential_id("FID");
    std::string now = utc_now_rfc3339();

    // Query max file_index
    int64_t next_idx = 1;
    {
        const char* q = "SELECT COALESCE(MAX(file_index), 0) + 1 FROM file_identities;";
        ScopedStmt s(db_, q);
        if (sqlite3_step(s.s) == SQLITE_ROW) {
            next_idx = sqlite3_column_int64(s.s, 0);
        }
    }

    const char* sql = "INSERT INTO file_identities (id, current_path, initial_path, created_at, last_modified_at, file_index) "
                      "VALUES (?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, fid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 6, next_idx);
    sqlite3_step(s.s);

    FileIdentityRecord rec;
    rec.id = fid;
    rec.current_path = path;
    rec.initial_path = path;
    rec.created_at = now;
    rec.last_modified_at = now;
    rec.file_index = next_idx;
    return rec;
}

void Journal::update_file_identity_path(const std::string& id, const std::string& new_path) {
    const char* sql = "UPDATE file_identities SET current_path = ?, last_modified_at = ? WHERE id = ?;";
    ScopedStmt s(db_, sql);
    std::string now = utc_now_rfc3339();
    sqlite3_bind_text(s.s, 1, new_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::optional<FileIdentityRecord> Journal::get_file_identity(const std::string& id) const {
    const char* sql = "SELECT id, current_path, initial_path, created_at, last_modified_at, file_index FROM file_identities WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        FileIdentityRecord rec;
        rec.id = get_text_col(s.s, 0);
        rec.current_path = get_text_col(s.s, 1);
        rec.initial_path = get_text_col(s.s, 2);
        rec.created_at = get_text_col(s.s, 3);
        rec.last_modified_at = get_text_col(s.s, 4);
        rec.file_index = sqlite3_column_int64(s.s, 5);
        return rec;
    }
    return std::nullopt;
}

std::optional<FileIdentityRecord> Journal::find_file_identity_by_path(const std::string& path) const {
    std::string norm1 = path;
    std::replace(norm1.begin(), norm1.end(), '\\', '/');
    std::string norm2 = path;
    std::replace(norm2.begin(), norm2.end(), '/', '\\');

    // 1. Direct match on current_path (fastest and most common)
    {
        const char* sql = "SELECT id, current_path, initial_path, created_at, last_modified_at, file_index "
                          "FROM file_identities WHERE current_path = ? OR current_path = ? "
                          "OR LOWER(current_path) = LOWER(?) OR LOWER(current_path) = LOWER(?);";
        ScopedStmt s(db_, sql);
        sqlite3_bind_text(s.s, 1, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 2, norm2.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 3, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 4, norm2.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.s) == SQLITE_ROW) {
            FileIdentityRecord rec;
            rec.id = get_text_col(s.s, 0);
            rec.current_path = get_text_col(s.s, 1);
            rec.initial_path = get_text_col(s.s, 2);
            rec.created_at = get_text_col(s.s, 3);
            rec.last_modified_at = get_text_col(s.s, 4);
            rec.file_index = sqlite3_column_int64(s.s, 5);
            return rec;
        }
    }

    // 2. Match on initial_path
    {
        const char* sql = "SELECT id, current_path, initial_path, created_at, last_modified_at, file_index "
                          "FROM file_identities WHERE initial_path = ? OR initial_path = ? "
                          "OR LOWER(initial_path) = LOWER(?) OR LOWER(initial_path) = LOWER(?);";
        ScopedStmt s(db_, sql);
        sqlite3_bind_text(s.s, 1, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 2, norm2.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 3, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 4, norm2.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.s) == SQLITE_ROW) {
            FileIdentityRecord rec;
            rec.id = get_text_col(s.s, 0);
            rec.current_path = get_text_col(s.s, 1);
            rec.initial_path = get_text_col(s.s, 2);
            rec.created_at = get_text_col(s.s, 3);
            rec.last_modified_at = get_text_col(s.s, 4);
            rec.file_index = sqlite3_column_int64(s.s, 5);
            return rec;
        }
    }

    // 3. Match through name_location_history (previous names before rename/move)
    {
        const char* sql = "SELECT fi.id, fi.current_path, fi.initial_path, fi.created_at, fi.last_modified_at, fi.file_index "
                          "FROM file_identities fi "
                          "JOIN name_location_history nl ON fi.id = nl.file_identity_id "
                          "WHERE nl.source_path = ? OR nl.source_path = ? OR LOWER(nl.source_path) = LOWER(?) OR LOWER(nl.source_path) = LOWER(?) "
                          "   OR nl.dest_path = ? OR nl.dest_path = ? OR LOWER(nl.dest_path) = LOWER(?) OR LOWER(nl.dest_path) = LOWER(?) "
                          "ORDER BY nl.seq DESC LIMIT 1;";
        ScopedStmt s(db_, sql);
        sqlite3_bind_text(s.s, 1, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 2, norm2.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 3, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 4, norm2.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 5, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 6, norm2.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 7, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 8, norm2.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.s) == SQLITE_ROW) {
            FileIdentityRecord rec;
            rec.id = get_text_col(s.s, 0);
            rec.current_path = get_text_col(s.s, 1);
            rec.initial_path = get_text_col(s.s, 2);
            rec.created_at = get_text_col(s.s, 3);
            rec.last_modified_at = get_text_col(s.s, 4);
            rec.file_index = sqlite3_column_int64(s.s, 5);
            return rec;
        }
    }

    // 4. Match through content_revisions (revision paths)
    {
        const char* sql = "SELECT fi.id, fi.current_path, fi.initial_path, fi.created_at, fi.last_modified_at, fi.file_index "
                          "FROM file_identities fi "
                          "JOIN content_revisions cr ON fi.id = cr.file_identity_id "
                          "WHERE cr.path = ? OR cr.path = ? OR LOWER(cr.path) = LOWER(?) OR LOWER(cr.path) = LOWER(?) "
                          "ORDER BY cr.revision_number DESC LIMIT 1;";
        ScopedStmt s(db_, sql);
        sqlite3_bind_text(s.s, 1, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 2, norm2.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 3, norm1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 4, norm2.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.s) == SQLITE_ROW) {
            FileIdentityRecord rec;
            rec.id = get_text_col(s.s, 0);
            rec.current_path = get_text_col(s.s, 1);
            rec.initial_path = get_text_col(s.s, 2);
            rec.created_at = get_text_col(s.s, 3);
            rec.last_modified_at = get_text_col(s.s, 4);
            rec.file_index = sqlite3_column_int64(s.s, 5);
            return rec;
        }
    }

    // 5. If relative, try resolving to absolute path or suffix match
    std::filesystem::path fs_p(path);
    if (fs_p.is_relative()) {
        std::error_code ec;
        auto abs_p = std::filesystem::weakly_canonical(fs_p, ec);
        if (ec || abs_p.empty()) abs_p = std::filesystem::absolute(fs_p, ec);
        if (!abs_p.empty() && abs_p != fs_p) {
            auto res = find_file_identity_by_path(abs_p.string());
            if (res) return res;
        }

        std::string suffix1 = "%" + norm1;
        std::string suffix2 = "%" + norm2;
        const char* sql = "SELECT id, current_path, initial_path, created_at, last_modified_at, file_index "
                          "FROM file_identities WHERE current_path LIKE ? OR current_path LIKE ? "
                          "ORDER BY last_modified_at DESC LIMIT 1;";
        ScopedStmt s(db_, sql);
        sqlite3_bind_text(s.s, 1, suffix1.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s.s, 2, suffix2.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s.s) == SQLITE_ROW) {
            FileIdentityRecord rec;
            rec.id = get_text_col(s.s, 0);
            rec.current_path = get_text_col(s.s, 1);
            rec.initial_path = get_text_col(s.s, 2);
            rec.created_at = get_text_col(s.s, 3);
            rec.last_modified_at = get_text_col(s.s, 4);
            rec.file_index = sqlite3_column_int64(s.s, 5);
            return rec;
        }
    }

    return std::nullopt;
}

void Journal::record_name_location(const NameLocationRecord& rec) {
    const char* sql = "INSERT INTO name_location_history (id, file_identity_id, seq, op_type, source_path, dest_path, created_at, source_op_id) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, rec.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, rec.file_identity_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 3, rec.seq);
    sqlite3_bind_text(s.s, 4, rec.op_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 5, rec.source_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 6, rec.dest_path.c_str(), -1, SQLITE_TRANSIENT);
    std::string now = rec.created_at.empty() ? utc_now_rfc3339() : rec.created_at;
    sqlite3_bind_text(s.s, 7, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 8, rec.source_op_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::vector<NameLocationRecord> Journal::get_name_location_history(const std::string& file_identity_id) const {
    std::vector<NameLocationRecord> list;
    const char* sql = "SELECT id, file_identity_id, seq, op_type, source_path, dest_path, created_at, source_op_id "
                      "FROM name_location_history WHERE file_identity_id = ? ORDER BY seq ASC;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, file_identity_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        NameLocationRecord r;
        r.id = get_text_col(s.s, 0);
        r.file_identity_id = get_text_col(s.s, 1);
        r.seq = sqlite3_column_int64(s.s, 2);
        r.op_type = get_text_col(s.s, 3);
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.created_at = get_text_col(s.s, 6);
        r.source_op_id = get_text_col(s.s, 7);
        list.push_back(std::move(r));
    }
    return list;
}

std::vector<NameLocationRecord> Journal::get_name_location_history_for_path(const std::string& path) const {
    auto fid = find_file_identity_by_path(path);
    if (fid) {
        return get_name_location_history(fid->id);
    }
    std::vector<NameLocationRecord> list;
    const char* sql = "SELECT id, file_identity_id, seq, op_type, source_path, dest_path, created_at, source_op_id "
                      "FROM name_location_history WHERE dest_path = ? OR source_path = ? ORDER BY seq ASC;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        NameLocationRecord r;
        r.id = get_text_col(s.s, 0);
        r.file_identity_id = get_text_col(s.s, 1);
        r.seq = sqlite3_column_int64(s.s, 2);
        r.op_type = get_text_col(s.s, 3);
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.created_at = get_text_col(s.s, 6);
        r.source_op_id = get_text_col(s.s, 7);
        list.push_back(std::move(r));
    }
    return list;
}

int64_t Journal::get_next_revision_number(const std::string& file_identity_id) const {
    const char* sql = "SELECT COALESCE(MAX(revision_number), 0) + 1 FROM content_revisions WHERE file_identity_id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, file_identity_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        return sqlite3_column_int64(s.s, 0);
    }
    return 1;
}

void Journal::record_content_revision(const ContentRevisionRecord& rev) {
    const char* sql = "INSERT INTO content_revisions (id, file_identity_id, revision_number, path, created_at, size_bytes, sha256_hash, snapshot_id, source_type, change_summary) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, rev.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, rev.file_identity_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 3, rev.revision_number);
    sqlite3_bind_text(s.s, 4, rev.path.c_str(), -1, SQLITE_TRANSIENT);
    std::string now = rev.created_at.empty() ? utc_now_rfc3339() : rev.created_at;
    sqlite3_bind_text(s.s, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 6, static_cast<int64_t>(rev.size_bytes));
    sqlite3_bind_text(s.s, 7, rev.sha256_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 8, rev.snapshot_id);
    sqlite3_bind_text(s.s, 9, rev.source_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 10, rev.change_summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::vector<ContentRevisionRecord> Journal::get_content_revisions(const std::string& file_identity_id) const {
    std::vector<ContentRevisionRecord> list;
    const char* sql = "SELECT id, file_identity_id, revision_number, path, created_at, size_bytes, sha256_hash, snapshot_id, source_type, change_summary "
                      "FROM content_revisions WHERE file_identity_id = ? ORDER BY revision_number DESC;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, file_identity_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        ContentRevisionRecord r;
        r.id = get_text_col(s.s, 0);
        r.file_identity_id = get_text_col(s.s, 1);
        r.revision_number = sqlite3_column_int64(s.s, 2);
        r.path = get_text_col(s.s, 3);
        r.created_at = get_text_col(s.s, 4);
        r.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(s.s, 5));
        r.sha256_hash = get_text_col(s.s, 6);
        r.snapshot_id = sqlite3_column_int64(s.s, 7);
        r.source_type = get_text_col(s.s, 8);
        r.change_summary = get_text_col(s.s, 9);
        list.push_back(std::move(r));
    }
    return list;
}

std::vector<ContentRevisionRecord> Journal::get_content_revisions_for_path(const std::string& path) const {
    auto fid = find_file_identity_by_path(path);
    if (fid) {
        return get_content_revisions(fid->id);
    }
    std::vector<ContentRevisionRecord> list;
    const char* sql = "SELECT id, file_identity_id, revision_number, path, created_at, size_bytes, sha256_hash, snapshot_id, source_type, change_summary "
                      "FROM content_revisions WHERE path = ? ORDER BY revision_number DESC;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        ContentRevisionRecord r;
        r.id = get_text_col(s.s, 0);
        r.file_identity_id = get_text_col(s.s, 1);
        r.revision_number = sqlite3_column_int64(s.s, 2);
        r.path = get_text_col(s.s, 3);
        r.created_at = get_text_col(s.s, 4);
        r.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(s.s, 5));
        r.sha256_hash = get_text_col(s.s, 6);
        r.snapshot_id = sqlite3_column_int64(s.s, 7);
        r.source_type = get_text_col(s.s, 8);
        r.change_summary = get_text_col(s.s, 9);
        list.push_back(std::move(r));
    }
    return list;
}

int64_t Journal::get_next_external_seq() {
    const char* sql = "SELECT COALESCE(MAX(detection_seq), 0) + 1 FROM external_candidates;";
    ScopedStmt s(db_, sql);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        return sqlite3_column_int64(s.s, 0);
    }
    return 1;
}

void Journal::record_external_candidate(const ExternalCandidateRecord& cand) {
    const char* sql = "INSERT INTO external_candidates (id, detection_seq, path, old_path, file_identity_id, change_type, detected_at, status, candidate_snapshot_path, candidate_hash, candidate_size, prior_accepted_hash, prior_accepted_snapshot_id, resolution_notes, resolved_at) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, cand.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 2, cand.detection_seq);
    sqlite3_bind_text(s.s, 3, cand.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, cand.old_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 5, cand.file_identity_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 6, cand.change_type.c_str(), -1, SQLITE_TRANSIENT);
    std::string now = cand.detected_at.empty() ? utc_now_rfc3339() : cand.detected_at;
    sqlite3_bind_text(s.s, 7, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 8, cand.status.empty() ? "PENDING" : cand.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 9, cand.candidate_snapshot_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 10, cand.candidate_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 11, static_cast<int64_t>(cand.candidate_size));
    sqlite3_bind_text(s.s, 12, cand.prior_accepted_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 13, cand.prior_accepted_snapshot_id);
    sqlite3_bind_text(s.s, 14, cand.resolution_notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 15, cand.resolved_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::vector<ExternalCandidateRecord> Journal::list_external_candidates(const std::string& status_filter) const {
    std::vector<ExternalCandidateRecord> list;
    std::string sql_str = "SELECT id, detection_seq, path, old_path, file_identity_id, change_type, detected_at, status, candidate_snapshot_path, candidate_hash, candidate_size, prior_accepted_hash, prior_accepted_snapshot_id, resolution_notes, resolved_at FROM external_candidates ";
    if (!status_filter.empty()) {
        sql_str += "WHERE status = ? ";
    }
    sql_str += "ORDER BY detection_seq DESC;";
    ScopedStmt s(db_, sql_str.c_str());
    if (!status_filter.empty()) {
        sqlite3_bind_text(s.s, 1, status_filter.c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        ExternalCandidateRecord r;
        r.id = get_text_col(s.s, 0);
        r.detection_seq = sqlite3_column_int64(s.s, 1);
        r.path = get_text_col(s.s, 2);
        r.old_path = get_text_col(s.s, 3);
        r.file_identity_id = get_text_col(s.s, 4);
        r.change_type = get_text_col(s.s, 5);
        r.detected_at = get_text_col(s.s, 6);
        r.status = get_text_col(s.s, 7);
        r.candidate_snapshot_path = get_text_col(s.s, 8);
        r.candidate_hash = get_text_col(s.s, 9);
        r.candidate_size = static_cast<uint64_t>(sqlite3_column_int64(s.s, 10));
        r.prior_accepted_hash = get_text_col(s.s, 11);
        r.prior_accepted_snapshot_id = sqlite3_column_int64(s.s, 12);
        r.resolution_notes = get_text_col(s.s, 13);
        r.resolved_at = get_text_col(s.s, 14);
        list.push_back(std::move(r));
    }
    return list;
}

std::optional<ExternalCandidateRecord> Journal::get_external_candidate(const std::string& id) const {
    const char* sql = "SELECT id, detection_seq, path, old_path, file_identity_id, change_type, detected_at, status, candidate_snapshot_path, candidate_hash, candidate_size, prior_accepted_hash, prior_accepted_snapshot_id, resolution_notes, resolved_at FROM external_candidates WHERE id = ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s.s) == SQLITE_ROW) {
        ExternalCandidateRecord r;
        r.id = get_text_col(s.s, 0);
        r.detection_seq = sqlite3_column_int64(s.s, 1);
        r.path = get_text_col(s.s, 2);
        r.old_path = get_text_col(s.s, 3);
        r.file_identity_id = get_text_col(s.s, 4);
        r.change_type = get_text_col(s.s, 5);
        r.detected_at = get_text_col(s.s, 6);
        r.status = get_text_col(s.s, 7);
        r.candidate_snapshot_path = get_text_col(s.s, 8);
        r.candidate_hash = get_text_col(s.s, 9);
        r.candidate_size = static_cast<uint64_t>(sqlite3_column_int64(s.s, 10));
        r.prior_accepted_hash = get_text_col(s.s, 11);
        r.prior_accepted_snapshot_id = sqlite3_column_int64(s.s, 12);
        r.resolution_notes = get_text_col(s.s, 13);
        r.resolved_at = get_text_col(s.s, 14);
        return r;
    }
    return std::nullopt;
}

void Journal::update_external_candidate_status(const std::string& id, const std::string& status, const std::string& notes) {
    const char* sql = "UPDATE external_candidates SET status = ?, resolution_notes = ?, resolved_at = ? WHERE id = ?;";
    ScopedStmt s(db_, sql);
    std::string now = utc_now_rfc3339();
    sqlite3_bind_text(s.s, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s.s);
}

std::vector<ExternalCandidateRecord> Journal::get_external_candidates_for_identity(const std::string& file_identity_id) const {
    std::vector<ExternalCandidateRecord> list;
    const char* sql = "SELECT id, detection_seq, path, old_path, file_identity_id, change_type, detected_at, status, candidate_snapshot_path, candidate_hash, candidate_size, prior_accepted_hash, prior_accepted_snapshot_id, resolution_notes, resolved_at "
                      "FROM external_candidates WHERE file_identity_id = ? ORDER BY detection_seq DESC;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, file_identity_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        ExternalCandidateRecord r;
        r.id = get_text_col(s.s, 0);
        r.detection_seq = sqlite3_column_int64(s.s, 1);
        r.path = get_text_col(s.s, 2);
        r.old_path = get_text_col(s.s, 3);
        r.file_identity_id = get_text_col(s.s, 4);
        r.change_type = get_text_col(s.s, 5);
        r.detected_at = get_text_col(s.s, 6);
        r.status = get_text_col(s.s, 7);
        r.candidate_snapshot_path = get_text_col(s.s, 8);
        r.candidate_hash = get_text_col(s.s, 9);
        r.candidate_size = static_cast<uint64_t>(sqlite3_column_int64(s.s, 10));
        r.prior_accepted_hash = get_text_col(s.s, 11);
        r.prior_accepted_snapshot_id = sqlite3_column_int64(s.s, 12);
        r.resolution_notes = get_text_col(s.s, 13);
        r.resolved_at = get_text_col(s.s, 14);
        list.push_back(std::move(r));
    }
    return list;
}

std::vector<ExternalCandidateRecord> Journal::get_external_candidates_for_path(const std::string& path) const {
    auto fid = find_file_identity_by_path(path);
    if (fid) {
        return get_external_candidates_for_identity(fid->id);
    }
    std::string norm1 = path;
    std::replace(norm1.begin(), norm1.end(), '\\', '/');
    std::string norm2 = path;
    std::replace(norm2.begin(), norm2.end(), '/', '\\');

    std::vector<ExternalCandidateRecord> list;
    const char* sql = "SELECT id, detection_seq, path, old_path, file_identity_id, change_type, detected_at, status, candidate_snapshot_path, candidate_hash, candidate_size, prior_accepted_hash, prior_accepted_snapshot_id, resolution_notes, resolved_at "
                      "FROM external_candidates WHERE path = ? OR path = ? OR old_path = ? OR old_path = ? ORDER BY detection_seq DESC;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, norm1.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, norm2.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, norm1.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, norm2.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        ExternalCandidateRecord r;
        r.id = get_text_col(s.s, 0);
        r.detection_seq = sqlite3_column_int64(s.s, 1);
        r.path = get_text_col(s.s, 2);
        r.old_path = get_text_col(s.s, 3);
        r.file_identity_id = get_text_col(s.s, 4);
        r.change_type = get_text_col(s.s, 5);
        r.detected_at = get_text_col(s.s, 6);
        r.status = get_text_col(s.s, 7);
        r.candidate_snapshot_path = get_text_col(s.s, 8);
        r.candidate_hash = get_text_col(s.s, 9);
        r.candidate_size = static_cast<uint64_t>(sqlite3_column_int64(s.s, 10));
        r.prior_accepted_hash = get_text_col(s.s, 11);
        r.prior_accepted_snapshot_id = sqlite3_column_int64(s.s, 12);
        r.resolution_notes = get_text_col(s.s, 13);
        r.resolved_at = get_text_col(s.s, 14);
        list.push_back(std::move(r));
    }
    return list;
}

std::vector<OperationRecord> Journal::get_operations_for_file(const std::string& path, size_t limit) const {
    auto fid = find_file_identity_by_path(path);
    std::string fid_filter = fid ? ("%\"file_identity_id\":\"" + fid->id + "\"%") : "";

    std::string norm1 = path;
    std::replace(norm1.begin(), norm1.end(), '\\', '/');
    std::string norm2 = path;
    std::replace(norm2.begin(), norm2.end(), '/', '\\');

    std::vector<OperationRecord> list;
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations "
                      "WHERE source_path = ? OR source_path = ? OR dest_path = ? OR dest_path = ? "
                      "   OR (LENGTH(?) > 0 AND details_json LIKE ?) "
                      "ORDER BY seq DESC LIMIT ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, norm1.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, norm2.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, norm1.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, norm2.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 5, fid_filter.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 6, fid_filter.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 7, static_cast<int64_t>(limit));
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        list.push_back(std::move(r));
    }
    return list;
}

std::vector<OperationRecord> Journal::get_operations_for_folder(const std::string& folder_path, size_t limit) const {
    std::vector<OperationRecord> list;
    std::string prefix1 = folder_path + "\\%";
    std::string prefix2 = folder_path + "/%";
    const char* sql = "SELECT id, seq, type, status, source_path, dest_path, transaction_id, "
                      "created_at, completed_at, expected_source_hash, expected_dest_hash, "
                      "recovery_artifact_path, details_json, is_undoable, is_undone, error_message "
                      "FROM operations WHERE source_path LIKE ? OR source_path LIKE ? OR dest_path LIKE ? OR dest_path LIKE ? "
                      "ORDER BY seq DESC LIMIT ?;";
    ScopedStmt s(db_, sql);
    sqlite3_bind_text(s.s, 1, prefix1.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 2, prefix2.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 3, prefix1.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.s, 4, prefix2.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.s, 5, static_cast<int64_t>(limit));
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        OperationRecord r;
        r.id = get_text_col(s.s, 0);
        r.seq = sqlite3_column_int64(s.s, 1);
        r.type = op_type_from_str(get_text_col(s.s, 2));
        r.status = op_status_from_str(get_text_col(s.s, 3));
        r.source_path = get_text_col(s.s, 4);
        r.dest_path = get_text_col(s.s, 5);
        r.transaction_id = get_text_col(s.s, 6);
        r.created_at = get_text_col(s.s, 7);
        r.completed_at = get_text_col(s.s, 8);
        r.expected_source_hash = get_text_col(s.s, 9);
        r.expected_dest_hash = get_text_col(s.s, 10);
        r.recovery_artifact_path = get_text_col(s.s, 11);
        r.details_json = get_text_col(s.s, 12);
        r.is_undoable = sqlite3_column_int(s.s, 13) != 0;
        r.is_undone = sqlite3_column_int(s.s, 14) != 0;
        r.error_message = get_text_col(s.s, 15);
        list.push_back(std::move(r));
    }
    return list;
}

} // namespace fileguard
