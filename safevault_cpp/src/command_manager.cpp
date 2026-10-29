#include "command_manager.hpp"
#include "sha256.hpp"
#include <json.hpp>

#include <chrono>
#include <random>
#include <sstream>
#include <iostream>

namespace fileguard {

using json = nlohmann::json;

namespace {

std::string utc_now_str() {
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

} // namespace

CommandManager::CommandManager(Journal& journal, FilesystemService& fs_service, RecoveryService& recovery_service)
    : journal_(journal), fs_service_(fs_service), recovery_(recovery_service) {
    recover_interrupted_operations();
}

std::string CommandManager::generate_op_id(OpType type) {
    static std::mt19937_64 rng(std::random_device{}());
    uint64_t rnd = rng();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << op_type_str(type) << "_" << now_ms << "_" << std::hex << rnd;
    return oss.str();
}

bool CommandManager::check_file_conflict(const std::filesystem::path& p, const std::string& expected_hash, std::string& reason) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        reason = "File does not exist: " + p.string();
        return true;
    }
    if (!expected_hash.empty()) {
        std::string cur_hash = sha256_file(p);
        if (cur_hash != expected_hash) {
            reason = "Content conflict: current hash " + cur_hash + " does not match expected " + expected_hash;
            return true;
        }
    }
    return false;
}

CommandResult CommandManager::execute_create_file(const std::filesystem::path& path, const std::string& content, const std::string& transaction_id) {
    CommandResult res;
    auto valid_path = fs_service_.canonicalize_and_validate(path);

    std::error_code ec;
    if (std::filesystem::exists(valid_path, ec)) {
        res.success = false;
        res.error = "File already exists at destination: " + valid_path.string();
        return res;
    }

    std::string op_id = generate_op_id(OpType::OpCreateFile);
    res.operation_id = op_id;

    auto fid = journal_.get_or_create_file_identity(valid_path.string());

    OperationRecord op;
    op.id = op_id;
    op.type = OpType::OpCreateFile;
    op.status = OpStatus::Intent;
    op.dest_path = valid_path.string();
    op.transaction_id = transaction_id;
    op.created_at = utc_now_str();

    json det;
    det["initial_content"] = content;
    det["file_identity_id"] = fid.id;
    op.details_json = det.dump();

    journal_.log_operation_intent(op);
    journal_.invalidate_redo_branch_after(0);

    try {
        notify_mutation(valid_path);
        fs_service_.create_file(valid_path, content);
        std::string hash = sha256_file(valid_path);

        int64_t rev_num = 1;
        std::string rev_id = journal_.generate_sequential_id("REV");
        ContentRevisionRecord rev;
        rev.id = rev_id;
        rev.file_identity_id = fid.id;
        rev.revision_number = rev_num;
        rev.path = valid_path.string();
        rev.size_bytes = content.size();
        rev.sha256_hash = hash;
        rev.source_type = "APP_CREATE";
        rev.change_summary = "Created in FileGuard";
        journal_.record_content_revision(rev);

        NameLocationRecord nl;
        nl.id = journal_.generate_sequential_id("OP");
        nl.file_identity_id = fid.id;
        nl.seq = op.seq;
        nl.op_type = "CREATE";
        nl.source_path = "";
        nl.dest_path = valid_path.string();
        nl.created_at = utc_now_str();
        journal_.record_name_location(nl);

        det["revision_id"] = rev.id;
        op.details_json = det.dump();

        journal_.update_operation_execution(op_id, utc_now_str(), hash, "", op.details_json);
        res.success = true;
        res.message = "File created successfully: " + valid_path.string();
        return res;
    } catch (const std::exception& e) {
        journal_.update_operation_status(op_id, OpStatus::Failed, e.what());
        res.success = false;
        res.error = e.what();
        return res;
    }
}

CommandResult CommandManager::execute_create_directory(const std::filesystem::path& path, const std::string& transaction_id) {
    CommandResult res;
    auto valid_path = fs_service_.canonicalize_and_validate(path);

    std::error_code ec;
    if (std::filesystem::exists(valid_path, ec)) {
        res.success = false;
        res.error = "Directory or file already exists at destination: " + valid_path.string();
        return res;
    }

    std::string op_id = generate_op_id(OpType::OpCreateDir);
    res.operation_id = op_id;

    OperationRecord op;
    op.id = op_id;
    op.type = OpType::OpCreateDir;
    op.status = OpStatus::Intent;
    op.dest_path = valid_path.string();
    op.transaction_id = transaction_id;
    op.created_at = utc_now_str();

    journal_.log_operation_intent(op);
    journal_.invalidate_redo_branch_after(0);

    try {
        notify_mutation(valid_path);
        fs_service_.create_directory(valid_path);
        journal_.set_operation_completed(op_id, utc_now_str());
        res.success = true;
        res.message = "Directory created: " + valid_path.string();
        return res;
    } catch (const std::exception& e) {
        journal_.update_operation_status(op_id, OpStatus::Failed, e.what());
        res.success = false;
        res.error = e.what();
        return res;
    }
}

CommandResult CommandManager::execute_rename(const std::filesystem::path& source, const std::filesystem::path& dest, const std::string& transaction_id) {
    CommandResult res;
    auto valid_src = fs_service_.canonicalize_and_validate(source);
    auto valid_dst = fs_service_.canonicalize_and_validate(dest);

    std::error_code ec;
    if (!std::filesystem::exists(valid_src, ec)) {
        res.success = false;
        res.error = "Source does not exist: " + valid_src.string();
        return res;
    }
    if (std::filesystem::exists(valid_dst, ec) && !std::filesystem::equivalent(valid_src, valid_dst, ec)) {
        res.success = false;
        res.error = "Destination already exists: " + valid_dst.string();
        return res;
    }

    std::string op_id = generate_op_id(OpType::OpRename);
    res.operation_id = op_id;

    std::string hash = std::filesystem::is_regular_file(valid_src, ec) ? sha256_file(valid_src) : "";

    auto fid = journal_.get_or_create_file_identity(valid_src.string());

    OperationRecord op;
    op.id = op_id;
    op.type = OpType::OpRename;
    op.status = OpStatus::Intent;
    op.source_path = valid_src.string();
    op.dest_path = valid_dst.string();
    op.transaction_id = transaction_id;
    op.expected_source_hash = hash;
    op.expected_dest_hash = hash;
    op.created_at = utc_now_str();

    json det;
    det["file_identity_id"] = fid.id;
    op.details_json = det.dump();

    journal_.log_operation_intent(op);
    journal_.invalidate_redo_branch_after(0);

    try {
        notify_mutation(valid_src, valid_dst);
        fs_service_.rename_item(valid_src, valid_dst);
        journal_.update_file_identity_path(fid.id, valid_dst.string());

        NameLocationRecord nl;
        nl.id = journal_.generate_sequential_id("OP");
        nl.file_identity_id = fid.id;
        nl.seq = op.seq;
        nl.op_type = "RENAME";
        nl.source_path = valid_src.string();
        nl.dest_path = valid_dst.string();
        nl.created_at = utc_now_str();
        journal_.record_name_location(nl);

        journal_.update_operation_execution(op_id, utc_now_str(), hash, "", op.details_json);
        res.success = true;
        res.message = "Renamed: " + valid_src.filename().string() + " -> " + valid_dst.filename().string();
        return res;
    } catch (const std::exception& e) {
        journal_.update_operation_status(op_id, OpStatus::Failed, e.what());
        res.success = false;
        res.error = e.what();
        return res;
    }
}

CommandResult CommandManager::execute_move(const std::filesystem::path& source, const std::filesystem::path& dest, const std::string& transaction_id) {
    CommandResult res;
    auto valid_src = fs_service_.canonicalize_and_validate(source);
    auto valid_dst = fs_service_.canonicalize_and_validate(dest);

    std::error_code ec;
    if (!std::filesystem::exists(valid_src, ec)) {
        res.success = false;
        res.error = "Source does not exist: " + valid_src.string();
        return res;
    }
    if (std::filesystem::exists(valid_dst, ec) && !std::filesystem::equivalent(valid_src, valid_dst, ec)) {
        res.success = false;
        res.error = "Destination already exists: " + valid_dst.string();
        return res;
    }

    std::string op_id = generate_op_id(OpType::OpMove);
    res.operation_id = op_id;

    std::string hash = std::filesystem::is_regular_file(valid_src, ec) ? sha256_file(valid_src) : "";

    auto fid = journal_.get_or_create_file_identity(valid_src.string());

    OperationRecord op;
    op.id = op_id;
    op.type = OpType::OpMove;
    op.status = OpStatus::Intent;
    op.source_path = valid_src.string();
    op.dest_path = valid_dst.string();
    op.transaction_id = transaction_id;
    op.expected_source_hash = hash;
    op.expected_dest_hash = hash;
    op.created_at = utc_now_str();

    json det;
    det["file_identity_id"] = fid.id;
    op.details_json = det.dump();

    journal_.log_operation_intent(op);
    journal_.invalidate_redo_branch_after(0);

    try {
        notify_mutation(valid_src, valid_dst);
        fs_service_.move_item(valid_src, valid_dst);
        journal_.update_file_identity_path(fid.id, valid_dst.string());

        NameLocationRecord nl;
        nl.id = journal_.generate_sequential_id("OP");
        nl.file_identity_id = fid.id;
        nl.seq = op.seq;
        nl.op_type = "MOVE";
        nl.source_path = valid_src.string();
        nl.dest_path = valid_dst.string();
        nl.created_at = utc_now_str();
        journal_.record_name_location(nl);

        journal_.update_operation_execution(op_id, utc_now_str(), hash, "", op.details_json);
        res.success = true;
        res.message = "Moved " + valid_src.string() + " to " + valid_dst.string();
        return res;
    } catch (const std::exception& e) {
        journal_.update_operation_status(op_id, OpStatus::Failed, e.what());
        res.success = false;
        res.error = e.what();
        return res;
    }
}

CommandResult CommandManager::execute_copy(const std::filesystem::path& source, const std::filesystem::path& dest, bool overwrite, const std::string& transaction_id) {
    CommandResult res;
    auto valid_src = fs_service_.canonicalize_and_validate(source);
    auto valid_dst = fs_service_.canonicalize_and_validate(dest);

    std::error_code ec;
    if (!std::filesystem::exists(valid_src, ec)) {
        res.success = false;
        res.error = "Source does not exist: " + valid_src.string();
        return res;
    }
    if (std::filesystem::exists(valid_dst, ec) && !overwrite) {
        res.success = false;
        res.error = "Destination already exists: " + valid_dst.string();
        return res;
    }

    std::string op_id = generate_op_id(OpType::OpCopy);
    res.operation_id = op_id;

    std::string hash = std::filesystem::is_regular_file(valid_src, ec) ? sha256_file(valid_src) : "";

    OperationRecord op;
    op.id = op_id;
    op.type = OpType::OpCopy;
    op.status = OpStatus::Intent;
    op.source_path = valid_src.string();
    op.dest_path = valid_dst.string();
    op.transaction_id = transaction_id;
    op.expected_source_hash = hash;
    op.created_at = utc_now_str();

    journal_.log_operation_intent(op);
    journal_.invalidate_redo_branch_after(0);

    try {
        notify_mutation(valid_dst);
        fs_service_.copy_item(valid_src, valid_dst, overwrite);
        std::string dst_hash = std::filesystem::is_regular_file(valid_dst, ec) ? sha256_file(valid_dst) : "";

        auto fid = journal_.get_or_create_file_identity(valid_dst.string());
        int64_t rev_num = journal_.get_next_revision_number(fid.id);
        std::string rev_id = journal_.generate_sequential_id("REV");
        ContentRevisionRecord rev;
        rev.id = rev_id;
        rev.file_identity_id = fid.id;
        rev.revision_number = rev_num;
        rev.path = valid_dst.string();
        rev.size_bytes = std::filesystem::is_regular_file(valid_dst, ec) ? std::filesystem::file_size(valid_dst, ec) : 0;
        rev.sha256_hash = dst_hash;
        rev.source_type = "APP_COPY";
        rev.change_summary = "Copied in FileGuard";
        journal_.record_content_revision(rev);

        json det;
        det["file_identity_id"] = fid.id;
        det["revision_id"] = rev.id;

        journal_.update_operation_execution(op_id, utc_now_str(), dst_hash, "", det.dump());
        res.success = true;
        res.message = "Copied to " + valid_dst.string();
        return res;
    } catch (const std::exception& e) {
        journal_.update_operation_status(op_id, OpStatus::Failed, e.what());
        res.success = false;
        res.error = e.what();
        return res;
    }
}

CommandResult CommandManager::execute_delete(const std::filesystem::path& path, const std::string& transaction_id) {
    return execute_quarantine(path, "Deleted (moved to quarantine)", transaction_id);
}

CommandResult CommandManager::execute_quarantine(const std::filesystem::path& path, const std::string& reason, const std::string& transaction_id) {
    CommandResult res;
    auto valid_path = fs_service_.canonicalize_and_validate(path);

    std::error_code ec;
    if (!std::filesystem::exists(valid_path, ec)) {
        res.success = false;
        res.error = "Item to quarantine does not exist: " + valid_path.string();
        return res;
    }

    std::string op_id = generate_op_id(OpType::OpDelete);
    res.operation_id = op_id;

    std::string hash = std::filesystem::is_regular_file(valid_path, ec) ? sha256_file(valid_path) : "";

    auto fid = journal_.get_or_create_file_identity(valid_path.string());

    OperationRecord op;
    op.id = op_id;
    op.type = OpType::OpDelete;
    op.status = OpStatus::Intent;
    op.source_path = valid_path.string();
    op.transaction_id = transaction_id;
    op.expected_source_hash = hash;
    op.created_at = utc_now_str();

    journal_.log_operation_intent(op);
    journal_.invalidate_redo_branch_after(0);

    try {
        notify_mutation(valid_path);
        auto qe = recovery_.quarantine_item(valid_path, op_id);
        json det;
        det["quarantine_id"] = qe.id;
        det["is_directory"] = qe.is_directory;
        det["size_bytes"] = qe.size_bytes;
        det["file_identity_id"] = fid.id;
        det["reason"] = reason.empty() ? "User initiated quarantine" : reason;

        NameLocationRecord nl;
        nl.id = journal_.generate_sequential_id("OP");
        nl.file_identity_id = fid.id;
        nl.seq = op.seq;
        nl.op_type = "QUARANTINE";
        nl.source_path = valid_path.string();
        nl.dest_path = qe.quarantine_path;
        nl.created_at = utc_now_str();
        journal_.record_name_location(nl);

        journal_.update_operation_execution(op_id, utc_now_str(), "", qe.quarantine_path, det.dump());
        res.success = true;
        res.message = "Moved to quarantine: " + valid_path.filename().string();
        return res;
    } catch (const std::exception& e) {
        journal_.update_operation_status(op_id, OpStatus::Failed, e.what());
        res.success = false;
        res.error = e.what();
        return res;
    }
}

CommandResult CommandManager::execute_overwrite(const std::filesystem::path& path, const std::vector<uint8_t>& new_content, const std::string& transaction_id) {
    CommandResult res;
    auto valid_path = fs_service_.canonicalize_and_validate(path);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(valid_path, ec)) {
        res.success = false;
        res.error = "Target is not a regular file: " + valid_path.string();
        return res;
    }

    std::string op_id = generate_op_id(OpType::OpOverwrite);
    res.operation_id = op_id;

    std::string pre_hash = sha256_file(valid_path);
    auto fid = journal_.get_or_create_file_identity(valid_path.string());

    OperationRecord op;
    op.id = op_id;
    op.type = OpType::OpOverwrite;
    op.status = OpStatus::Intent;
    op.dest_path = valid_path.string();
    op.transaction_id = transaction_id;
    op.expected_source_hash = pre_hash;
    op.created_at = utc_now_str();

    // 1. Snapshot pre-image
    auto pre_snap = recovery_.create_snapshot(valid_path, op_id);
    op.recovery_artifact_path = pre_snap.snapshot_path;

    json det;
    det["file_identity_id"] = fid.id;
    det["pre_version_id"] = pre_snap.id;
    det["pre_hash"] = pre_hash;
    op.details_json = det.dump();

    journal_.log_operation_intent(op);
    journal_.invalidate_redo_branch_after(0);

    try {
        notify_mutation(valid_path);
        // 2. Overwrite file
        fs_service_.write_file(valid_path, new_content);
        std::string post_hash = sha256_file(valid_path);
        auto post_snap = recovery_.create_snapshot(valid_path, op_id);

        int64_t rev_num = journal_.get_next_revision_number(fid.id);
        std::string rev_id = journal_.generate_sequential_id("REV");
        ContentRevisionRecord rev;
        rev.id = rev_id;
        rev.file_identity_id = fid.id;
        rev.revision_number = rev_num;
        rev.path = valid_path.string();
        rev.size_bytes = new_content.size();
        rev.sha256_hash = post_hash;
        rev.snapshot_id = post_snap.id;
        rev.source_type = "APP_EDIT";
        rev.change_summary = "Content edit in FileGuard";
        journal_.record_content_revision(rev);

        det["post_version_id"] = post_snap.id;
        det["post_hash"] = post_hash;
        det["revision_id"] = rev_id;
        det["revision_number"] = rev_num;

        journal_.update_operation_execution(op_id, utc_now_str(), post_hash, pre_snap.snapshot_path, det.dump());
        res.success = true;
        res.message = "File overwritten with backup: " + valid_path.filename().string();
        return res;
    } catch (const std::exception& e) {
        journal_.update_operation_status(op_id, OpStatus::Failed, e.what());
        res.success = false;
        res.error = e.what();
        return res;
    }
}

bool CommandManager::can_undo() const {
    return journal_.get_last_undoable_operation().has_value();
}

bool CommandManager::can_redo() const {
    return journal_.get_last_redoable_operation().has_value();
}

std::optional<OperationRecord> CommandManager::peek_undo() const {
    return journal_.get_last_undoable_operation();
}

std::optional<OperationRecord> CommandManager::peek_redo() const {
    return journal_.get_last_redoable_operation();
}

bool CommandManager::can_undo_content() const {
    return journal_.get_last_undoable_content_operation().has_value();
}

bool CommandManager::can_redo_content() const {
    return journal_.get_last_redoable_content_operation().has_value();
}

std::optional<OperationRecord> CommandManager::peek_undo_content() const {
    return journal_.get_last_undoable_content_operation();
}

std::optional<OperationRecord> CommandManager::peek_redo_content() const {
    return journal_.get_last_redoable_content_operation();
}

CommandResult CommandManager::undo_content() {
    auto op_opt = journal_.get_last_undoable_content_operation();
    if (!op_opt) {
        CommandResult res;
        res.success = false;
        res.error = "No content edits available to undo.";
        return res;
    }
    if (!op_opt->transaction_id.empty()) {
        return undo_transaction(op_opt->transaction_id);
    }
    return undo_operation(*op_opt);
}

CommandResult CommandManager::redo_content() {
    auto op_opt = journal_.get_last_redoable_content_operation();
    if (!op_opt) {
        CommandResult res;
        res.success = false;
        res.error = "No content edits available to redo.";
        return res;
    }
    if (!op_opt->transaction_id.empty()) {
        return redo_transaction(op_opt->transaction_id);
    }
    return redo_operation(*op_opt);
}

bool CommandManager::can_undo_name() const {
    return journal_.get_last_undoable_name_operation().has_value();
}

bool CommandManager::can_redo_name() const {
    return journal_.get_last_redoable_name_operation().has_value();
}

std::optional<OperationRecord> CommandManager::peek_undo_name() const {
    return journal_.get_last_undoable_name_operation();
}

std::optional<OperationRecord> CommandManager::peek_redo_name() const {
    return journal_.get_last_redoable_name_operation();
}

CommandResult CommandManager::undo_name() {
    auto op_opt = journal_.get_last_undoable_name_operation();
    if (!op_opt) {
        CommandResult res;
        res.success = false;
        res.error = "No rename or move operations available to undo.";
        return res;
    }
    if (!op_opt->transaction_id.empty()) {
        return undo_transaction(op_opt->transaction_id);
    }
    return undo_operation(*op_opt);
}

CommandResult CommandManager::redo_name() {
    auto op_opt = journal_.get_last_redoable_name_operation();
    if (!op_opt) {
        CommandResult res;
        res.success = false;
        res.error = "No rename or move operations available to redo.";
        return res;
    }
    if (!op_opt->transaction_id.empty()) {
        return redo_transaction(op_opt->transaction_id);
    }
    return redo_operation(*op_opt);
}

CommandResult CommandManager::undo_last() {
    auto op_opt = journal_.get_last_undoable_operation();
    if (!op_opt) {
        CommandResult res;
        res.success = false;
        res.error = "No operations available to undo.";
        return res;
    }
    if (!op_opt->transaction_id.empty()) {
        return undo_transaction(op_opt->transaction_id);
    }
    return undo_operation(*op_opt);
}

CommandResult CommandManager::redo_last() {
    auto op_opt = journal_.get_last_redoable_operation();
    if (!op_opt) {
        CommandResult res;
        res.success = false;
        res.error = "No operations available to redo.";
        return res;
    }
    if (!op_opt->transaction_id.empty()) {
        return redo_transaction(op_opt->transaction_id);
    }
    return redo_operation(*op_opt);
}

CommandResult CommandManager::undo_operation(const OperationRecord& op) {
    CommandResult res;
    res.operation_id = op.id;
    std::error_code ec;

    std::string fid_id;
    try {
        auto det = json::parse(op.details_json);
        fid_id = det.value("file_identity_id", "");
    } catch (...) {}

    switch (op.type) {
        case OpType::OpCreateFile: {
            std::filesystem::path target(op.dest_path);
            if (!fid_id.empty()) {
                auto fid_opt = journal_.get_file_identity(fid_id);
                if (fid_opt && std::filesystem::exists(fid_opt->current_path, ec)) {
                    target = std::filesystem::path(fid_opt->current_path);
                }
            }
            if (!std::filesystem::exists(target, ec)) {
                journal_.mark_operation_undone(op.id);
                res.success = true;
                res.message = "File already absent, undo marked complete.";
                return res;
            }
            std::string reason;
            if (check_file_conflict(target, op.expected_dest_hash, reason)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Conflict during undo: " + reason;
                res.error = res.conflict_reason;
                journal_.update_operation_status(op.id, OpStatus::Conflict, reason);
                return res;
            }
            notify_mutation(target);
            fs_service_.remove_item(target);
            journal_.mark_operation_undone(op.id);
            res.success = true;
            res.message = "Undid file creation: " + target.filename().string();
            return res;
        }

        case OpType::OpCreateDir: {
            std::filesystem::path target(op.dest_path);
            if (!std::filesystem::exists(target, ec)) {
                journal_.mark_operation_undone(op.id);
                res.success = true;
                res.message = "Directory already absent, undo complete.";
                return res;
            }
            bool is_empty = std::filesystem::is_empty(target, ec);
            if (!is_empty) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Cannot undo directory creation: directory is not empty (" + target.string() + ")";
                res.error = res.conflict_reason;
                journal_.update_operation_status(op.id, OpStatus::Conflict, res.conflict_reason);
                return res;
            }
            notify_mutation(target);
            fs_service_.remove_item(target);
            journal_.mark_operation_undone(op.id);
            res.success = true;
            res.message = "Undid directory creation: " + target.filename().string();
            return res;
        }

        case OpType::OpRename: {
            std::filesystem::path current_loc(op.dest_path);
            std::filesystem::path original_loc(op.source_path);

            if (!fid_id.empty()) {
                auto fid_opt = journal_.get_file_identity(fid_id);
                if (fid_opt && std::filesystem::exists(fid_opt->current_path, ec)) {
                    current_loc = std::filesystem::path(fid_opt->current_path);
                }
            }

            if (!std::filesystem::exists(current_loc, ec)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "File no longer at destination: " + current_loc.string();
                res.error = res.conflict_reason;
                return res;
            }
            if (std::filesystem::exists(original_loc, ec) && !std::filesystem::equivalent(current_loc, original_loc, ec)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Original path already occupied: " + original_loc.string();
                res.error = res.conflict_reason;
                return res;
            }

            notify_mutation(current_loc, original_loc);
            fs_service_.rename_item(current_loc, original_loc);
            if (!fid_id.empty()) {
                journal_.update_file_identity_path(fid_id, original_loc.string());
                NameLocationRecord nl;
                nl.id = journal_.generate_sequential_id("OP");
                nl.file_identity_id = fid_id;
                nl.seq = op.seq;
                nl.op_type = "UNDO_RENAME";
                nl.source_path = current_loc.string();
                nl.dest_path = original_loc.string();
                nl.created_at = utc_now_str();
                journal_.record_name_location(nl);
            }
            journal_.mark_operation_undone(op.id);
            res.success = true;
            res.message = "Undid rename: reverted to " + original_loc.filename().string();
            return res;
        }

        case OpType::OpMove: {
            std::filesystem::path current_loc(op.dest_path);
            std::filesystem::path original_loc(op.source_path);

            if (!fid_id.empty()) {
                auto fid_opt = journal_.get_file_identity(fid_id);
                if (fid_opt && std::filesystem::exists(fid_opt->current_path, ec)) {
                    current_loc = std::filesystem::path(fid_opt->current_path);
                }
            }

            if (!std::filesystem::exists(current_loc, ec)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Moved item not found at destination: " + current_loc.string();
                res.error = res.conflict_reason;
                return res;
            }
            if (std::filesystem::exists(original_loc, ec) && !std::filesystem::equivalent(current_loc, original_loc, ec)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Original path already occupied: " + original_loc.string();
                res.error = res.conflict_reason;
                return res;
            }

            if (original_loc.has_parent_path()) {
                std::filesystem::create_directories(original_loc.parent_path(), ec);
            }

            notify_mutation(current_loc, original_loc);
            fs_service_.move_item(current_loc, original_loc);
            if (!fid_id.empty()) {
                journal_.update_file_identity_path(fid_id, original_loc.string());
                NameLocationRecord nl;
                nl.id = journal_.generate_sequential_id("OP");
                nl.file_identity_id = fid_id;
                nl.seq = op.seq;
                nl.op_type = "UNDO_MOVE";
                nl.source_path = current_loc.string();
                nl.dest_path = original_loc.string();
                nl.created_at = utc_now_str();
                journal_.record_name_location(nl);
            }
            journal_.mark_operation_undone(op.id);
            res.success = true;
            res.message = "Undid move: returned to " + original_loc.string();
            return res;
        }

        case OpType::OpCopy: {
            std::filesystem::path copy_loc(op.dest_path);
            if (!std::filesystem::exists(copy_loc, ec)) {
                journal_.mark_operation_undone(op.id);
                res.success = true;
                res.message = "Copy destination already removed.";
                return res;
            }
            std::string reason;
            if (std::filesystem::is_regular_file(copy_loc, ec) &&
                check_file_conflict(copy_loc, op.expected_dest_hash, reason)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Copy was modified externally since creation: " + reason;
                res.error = res.conflict_reason;
                return res;
            }
            notify_mutation(copy_loc);
            fs_service_.remove_item(copy_loc);
            journal_.mark_operation_undone(op.id);
            res.success = true;
            res.message = "Undid copy: removed copy at " + copy_loc.string();
            return res;
        }

        case OpType::OpDelete: {
            std::filesystem::path orig_loc(op.source_path);
            if (std::filesystem::exists(orig_loc, ec)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Cannot restore deleted item: path already occupied: " + orig_loc.string();
                res.error = res.conflict_reason;
                return res;
            }

            std::string qid;
            try {
                auto det = json::parse(op.details_json);
                qid = det.value("quarantine_id", "");
            } catch (...) {}

            if (qid.empty()) {
                res.success = false;
                res.error = "Missing quarantine identifier in operation record.";
                return res;
            }

            notify_mutation(orig_loc);
            recovery_.restore_quarantined_item(qid, false);
            if (!fid_id.empty()) {
                journal_.update_file_identity_path(fid_id, orig_loc.string());
            }
            journal_.mark_operation_undone(op.id);
            res.success = true;
            res.message = "Undid deletion: restored " + orig_loc.filename().string() + " from quarantine.";
            return res;
        }

        case OpType::OpOverwrite: {
            std::filesystem::path target(op.dest_path);
            int64_t pre_ver = 0;
            try {
                auto det = json::parse(op.details_json);
                pre_ver = det.value("pre_version_id", 0);
            } catch (...) {}

            if (!fid_id.empty()) {
                auto fid_opt = journal_.get_file_identity(fid_id);
                if (fid_opt && std::filesystem::exists(fid_opt->current_path, ec)) {
                    target = std::filesystem::path(fid_opt->current_path);
                }
            }

            if (!std::filesystem::exists(target, ec)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Target file no longer exists for undo overwrite: " + target.string();
                res.error = res.conflict_reason;
                return res;
            }

            std::string reason;
            if (check_file_conflict(target, op.expected_dest_hash, reason)) {
                res.success = false;
                res.has_conflict = true;
                res.conflict_reason = "Conflict during undo overwrite: " + reason;
                res.error = res.conflict_reason;
                return res;
            }

            if (pre_ver <= 0) {
                res.success = false;
                res.error = "No pre-image version recorded for overwrite undo.";
                return res;
            }

            notify_mutation(target);
            recovery_.restore_snapshot_to(pre_ver, target, false);
            journal_.mark_operation_undone(op.id);
            res.success = true;
            res.message = "Undid overwrite: restored previous version of " + target.filename().string();
            return res;
        }
    }

    res.success = false;
    res.error = "Unknown operation type";
    return res;
}

CommandResult CommandManager::redo_operation(const OperationRecord& op) {
    CommandResult res;
    res.operation_id = op.id;
    std::error_code ec;

    std::string fid_id;
    try {
        auto det = json::parse(op.details_json);
        fid_id = det.value("file_identity_id", "");
    } catch (...) {}

    switch (op.type) {
        case OpType::OpCreateFile: {
            std::filesystem::path target(op.dest_path);
            if (std::filesystem::exists(target, ec)) {
                res.success = false;
                res.error = "Cannot redo create: file already exists at " + target.string();
                return res;
            }
            std::string initial_content;
            try {
                auto det = json::parse(op.details_json);
                initial_content = det.value("initial_content", "");
            } catch (...) {}
            notify_mutation(target);
            fs_service_.create_file(target, initial_content);
            if (!fid_id.empty()) {
                journal_.update_file_identity_path(fid_id, target.string());
            }
            journal_.mark_operation_redone(op.id);
            res.success = true;
            res.message = "Redid file creation: " + target.filename().string();
            return res;
        }

        case OpType::OpCreateDir: {
            std::filesystem::path target(op.dest_path);
            if (std::filesystem::exists(target, ec)) {
                res.success = false;
                res.error = "Cannot redo create: directory already exists at " + target.string();
                return res;
            }
            notify_mutation(target);
            fs_service_.create_directory(target);
            journal_.mark_operation_redone(op.id);
            res.success = true;
            res.message = "Redid directory creation: " + target.filename().string();
            return res;
        }

        case OpType::OpRename: {
            std::filesystem::path orig(op.source_path);
            std::filesystem::path dest(op.dest_path);

            if (!fid_id.empty()) {
                auto fid_opt = journal_.get_file_identity(fid_id);
                if (fid_opt && std::filesystem::exists(fid_opt->current_path, ec)) {
                    orig = std::filesystem::path(fid_opt->current_path);
                }
            }

            if (!std::filesystem::exists(orig, ec)) {
                res.success = false;
                res.error = "Cannot redo rename: source file missing: " + orig.string();
                return res;
            }
            if (std::filesystem::exists(dest, ec) && !std::filesystem::equivalent(orig, dest, ec)) {
                res.success = false;
                res.error = "Cannot redo rename: destination already occupied: " + dest.string();
                return res;
            }
            notify_mutation(orig, dest);
            fs_service_.rename_item(orig, dest);
            if (!fid_id.empty()) {
                journal_.update_file_identity_path(fid_id, dest.string());
                NameLocationRecord nl;
                nl.id = journal_.generate_sequential_id("OP");
                nl.file_identity_id = fid_id;
                nl.seq = op.seq;
                nl.op_type = "REDO_RENAME";
                nl.source_path = orig.string();
                nl.dest_path = dest.string();
                nl.created_at = utc_now_str();
                journal_.record_name_location(nl);
            }
            journal_.mark_operation_redone(op.id);
            res.success = true;
            res.message = "Redid rename: " + dest.filename().string();
            return res;
        }

        case OpType::OpMove: {
            std::filesystem::path orig(op.source_path);
            std::filesystem::path dest(op.dest_path);

            if (!fid_id.empty()) {
                auto fid_opt = journal_.get_file_identity(fid_id);
                if (fid_opt && std::filesystem::exists(fid_opt->current_path, ec)) {
                    orig = std::filesystem::path(fid_opt->current_path);
                }
            }

            if (!std::filesystem::exists(orig, ec)) {
                res.success = false;
                res.error = "Cannot redo move: source file missing: " + orig.string();
                return res;
            }
            if (std::filesystem::exists(dest, ec) && !std::filesystem::equivalent(orig, dest, ec)) {
                res.success = false;
                res.error = "Cannot redo move: destination already occupied: " + dest.string();
                return res;
            }
            notify_mutation(orig, dest);
            fs_service_.move_item(orig, dest);
            if (!fid_id.empty()) {
                journal_.update_file_identity_path(fid_id, dest.string());
                NameLocationRecord nl;
                nl.id = journal_.generate_sequential_id("OP");
                nl.file_identity_id = fid_id;
                nl.seq = op.seq;
                nl.op_type = "REDO_MOVE";
                nl.source_path = orig.string();
                nl.dest_path = dest.string();
                nl.created_at = utc_now_str();
                journal_.record_name_location(nl);
            }
            journal_.mark_operation_redone(op.id);
            res.success = true;
            res.message = "Redid move: " + dest.string();
            return res;
        }

        case OpType::OpCopy: {
            std::filesystem::path orig(op.source_path);
            std::filesystem::path dest(op.dest_path);
            if (!std::filesystem::exists(orig, ec)) {
                res.success = false;
                res.error = "Cannot redo copy: source missing: " + orig.string();
                return res;
            }
            notify_mutation(dest);
            fs_service_.copy_item(orig, dest, true);
            journal_.mark_operation_redone(op.id);
            res.success = true;
            res.message = "Redid copy: " + dest.string();
            return res;
        }

        case OpType::OpDelete: {
            std::filesystem::path target(op.source_path);
            if (!fid_id.empty()) {
                auto fid_opt = journal_.get_file_identity(fid_id);
                if (fid_opt && std::filesystem::exists(fid_opt->current_path, ec)) {
                    target = std::filesystem::path(fid_opt->current_path);
                }
            }
            if (!std::filesystem::exists(target, ec)) {
                journal_.mark_operation_redone(op.id);
                res.success = true;
                res.message = "Item already absent.";
                return res;
            }
            notify_mutation(target);
            recovery_.quarantine_item(target, op.id);
            journal_.mark_operation_redone(op.id);
            res.success = true;
            res.message = "Redid delete (quarantined): " + target.filename().string();
            return res;
        }

        case OpType::OpOverwrite: {
            int64_t post_ver = 0;
            try {
                auto det = json::parse(op.details_json);
                post_ver = det.value("post_version_id", 0);
            } catch (...) {}
            if (post_ver <= 0) {
                res.success = false;
                res.error = "No post-image version recorded for redo overwrite.";
                return res;
            }
            std::filesystem::path target(op.dest_path);
            if (!fid_id.empty()) {
                auto fid_opt = journal_.get_file_identity(fid_id);
                if (fid_opt && std::filesystem::exists(fid_opt->current_path, ec)) {
                    target = std::filesystem::path(fid_opt->current_path);
                }
            }
            notify_mutation(target);
            recovery_.restore_snapshot_to(post_ver, target, false);
            journal_.mark_operation_redone(op.id);
            res.success = true;
            res.message = "Redid overwrite: restored post-image.";
            return res;
        }
    }

    res.success = false;
    res.error = "Unknown operation type";
    return res;
}

CommandResult CommandManager::execute_batch_delete(const std::vector<std::string>& paths,
                                                   std::function<void(const BatchProgress&)> progress_cb) {
    CommandResult res;
    std::string tx_id = generate_op_id(OpType::OpDelete);
    tx_id = "tx_del_" + tx_id;
    res.operation_id = tx_id;

    TransactionRecord tx;
    tx.id = tx_id;
    tx.type = "BATCH_DELETE";
    tx.status = "IN_PROGRESS";
    tx.total_ops = static_cast<int32_t>(paths.size());
    tx.created_at = utc_now_str();
    journal_.create_transaction(tx);

    std::vector<std::string> executed_op_ids;

    BatchProgress prog;
    prog.transaction_id = tx_id;
    prog.total = tx.total_ops;
    prog.status = "IN_PROGRESS";

    for (size_t i = 0; i < paths.size(); ++i) {
        prog.current_item = paths[i];
        if (progress_cb) progress_cb(prog);

        std::filesystem::path p(paths[i]);
        auto item_res = execute_delete(p, tx_id);
        if (!item_res.success) {
            // Failure! Roll back previous items
            prog.failed++;
            prog.status = "FAILED";
            prog.error_message = item_res.error;
            journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "FAILED", item_res.error);

            // Rollback completed ops in this batch
            for (auto it = executed_op_ids.rbegin(); it != executed_op_ids.rend(); ++it) {
                auto op = journal_.get_operation(*it);
                if (op) undo_operation(*op);
            }
            journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "ROLLED_BACK", "Compensated after failure");

            res.success = false;
            res.error = "Batch delete failed at item '" + paths[i] + "': " + item_res.error + ". Completed items were rolled back.";
            return res;
        }

        executed_op_ids.push_back(item_res.operation_id);
        prog.completed++;
        journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "IN_PROGRESS");
    }

    prog.status = "COMPLETED";
    if (progress_cb) progress_cb(prog);
    journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "COMPLETED");

    res.success = true;
    res.message = "Batch delete completed: " + std::to_string(prog.completed) + " items quarantined.";
    return res;
}

CommandResult CommandManager::execute_batch_move(const std::vector<BatchItem>& items,
                                                 std::function<void(const BatchProgress&)> progress_cb) {
    CommandResult res;
    std::string tx_id = "tx_move_" + generate_op_id(OpType::OpMove);
    res.operation_id = tx_id;

    TransactionRecord tx;
    tx.id = tx_id;
    tx.type = "BATCH_MOVE";
    tx.status = "IN_PROGRESS";
    tx.total_ops = static_cast<int32_t>(items.size());
    tx.created_at = utc_now_str();
    journal_.create_transaction(tx);

    std::vector<std::string> executed_op_ids;
    BatchProgress prog;
    prog.transaction_id = tx_id;
    prog.total = tx.total_ops;
    prog.status = "IN_PROGRESS";

    for (const auto& item : items) {
        prog.current_item = item.source + " -> " + item.destination;
        if (progress_cb) progress_cb(prog);

        auto item_res = execute_move(item.source, item.destination, tx_id);
        if (!item_res.success) {
            prog.failed++;
            prog.status = "FAILED";
            prog.error_message = item_res.error;
            journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "FAILED", item_res.error);

            for (auto it = executed_op_ids.rbegin(); it != executed_op_ids.rend(); ++it) {
                auto op = journal_.get_operation(*it);
                if (op) undo_operation(*op);
            }
            journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "ROLLED_BACK", "Compensated after failure");

            res.success = false;
            res.error = "Batch move failed at '" + item.source + "': " + item_res.error + ". Completed items were rolled back.";
            return res;
        }

        executed_op_ids.push_back(item_res.operation_id);
        prog.completed++;
        journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "IN_PROGRESS");
    }

    prog.status = "COMPLETED";
    if (progress_cb) progress_cb(prog);
    journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "COMPLETED");

    res.success = true;
    res.message = "Batch move completed: " + std::to_string(prog.completed) + " items moved.";
    return res;
}

CommandResult CommandManager::execute_batch_copy(const std::vector<BatchItem>& items,
                                                 std::function<void(const BatchProgress&)> progress_cb) {
    CommandResult res;
    std::string tx_id = "tx_copy_" + generate_op_id(OpType::OpCopy);
    res.operation_id = tx_id;

    TransactionRecord tx;
    tx.id = tx_id;
    tx.type = "BATCH_COPY";
    tx.status = "IN_PROGRESS";
    tx.total_ops = static_cast<int32_t>(items.size());
    tx.created_at = utc_now_str();
    journal_.create_transaction(tx);

    std::vector<std::string> executed_op_ids;
    BatchProgress prog;
    prog.transaction_id = tx_id;
    prog.total = tx.total_ops;
    prog.status = "IN_PROGRESS";

    for (const auto& item : items) {
        prog.current_item = item.source + " -> " + item.destination;
        if (progress_cb) progress_cb(prog);

        auto item_res = execute_copy(item.source, item.destination, false, tx_id);
        if (!item_res.success) {
            prog.failed++;
            prog.status = "FAILED";
            prog.error_message = item_res.error;
            journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "FAILED", item_res.error);

            for (auto it = executed_op_ids.rbegin(); it != executed_op_ids.rend(); ++it) {
                auto op = journal_.get_operation(*it);
                if (op) undo_operation(*op);
            }
            journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "ROLLED_BACK", "Compensated after failure");

            res.success = false;
            res.error = "Batch copy failed at '" + item.source + "': " + item_res.error + ". Completed items were rolled back.";
            return res;
        }

        executed_op_ids.push_back(item_res.operation_id);
        prog.completed++;
        journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "IN_PROGRESS");
    }

    prog.status = "COMPLETED";
    if (progress_cb) progress_cb(prog);
    journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "COMPLETED");

    res.success = true;
    res.message = "Batch copy completed: " + std::to_string(prog.completed) + " items copied.";
    return res;
}

CommandResult CommandManager::execute_batch_rename(const std::vector<BatchItem>& items,
                                                   std::function<void(const BatchProgress&)> progress_cb) {
    CommandResult res;
    std::string tx_id = "tx_rename_" + generate_op_id(OpType::OpRename);
    res.operation_id = tx_id;

    TransactionRecord tx;
    tx.id = tx_id;
    tx.type = "BATCH_RENAME";
    tx.status = "IN_PROGRESS";
    tx.total_ops = static_cast<int32_t>(items.size());
    tx.created_at = utc_now_str();
    journal_.create_transaction(tx);

    std::vector<std::string> executed_op_ids;
    BatchProgress prog;
    prog.transaction_id = tx_id;
    prog.total = tx.total_ops;
    prog.status = "IN_PROGRESS";

    for (const auto& item : items) {
        prog.current_item = item.source + " -> " + item.destination;
        if (progress_cb) progress_cb(prog);

        auto item_res = execute_rename(item.source, item.destination, tx_id);
        if (!item_res.success) {
            prog.failed++;
            prog.status = "FAILED";
            prog.error_message = item_res.error;
            journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "FAILED", item_res.error);

            for (auto it = executed_op_ids.rbegin(); it != executed_op_ids.rend(); ++it) {
                auto op = journal_.get_operation(*it);
                if (op) undo_operation(*op);
            }
            journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "ROLLED_BACK", "Compensated after failure");

            res.success = false;
            res.error = "Batch rename failed at '" + item.source + "': " + item_res.error + ". Completed items were rolled back.";
            return res;
        }

        executed_op_ids.push_back(item_res.operation_id);
        prog.completed++;
        journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "IN_PROGRESS");
    }

    prog.status = "COMPLETED";
    if (progress_cb) progress_cb(prog);
    journal_.update_transaction_progress(tx_id, prog.completed, prog.failed, "COMPLETED");

    res.success = true;
    res.message = "Batch rename completed: " + std::to_string(prog.completed) + " items renamed.";
    return res;
}

CommandResult CommandManager::undo_transaction(const std::string& tx_id) {
    CommandResult res;
    auto ops = journal_.get_operations_for_transaction(tx_id);
    if (ops.empty()) {
        res.success = false;
        res.error = "No operations found for transaction " + tx_id;
        return res;
    }

    int undone = 0;
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        if (!it->is_undone) {
            auto r = undo_operation(*it);
            if (!r.success) {
                res.success = false;
                res.error = "Failed to undo item in transaction: " + r.error;
                return res;
            }
            undone++;
        }
    }

    journal_.update_transaction_progress(tx_id, 0, 0, "ROLLED_BACK", "User transaction undo");
    res.success = true;
    res.message = "Batch transaction undone (" + std::to_string(undone) + " operations reversed).";
    return res;
}

CommandResult CommandManager::redo_transaction(const std::string& tx_id) {
    CommandResult res;
    auto ops = journal_.get_operations_for_transaction(tx_id);
    if (ops.empty()) {
        res.success = false;
        res.error = "No operations found for transaction " + tx_id;
        return res;
    }

    int redone = 0;
    for (const auto& op : ops) {
        if (op.is_undone) {
            auto r = redo_operation(op);
            if (!r.success) {
                res.success = false;
                res.error = "Failed to redo item in transaction: " + r.error;
                return res;
            }
            redone++;
        }
    }

    journal_.update_transaction_progress(tx_id, static_cast<int32_t>(ops.size()), 0, "COMPLETED", "User transaction redo");
    res.success = true;
    res.message = "Batch transaction redone (" + std::to_string(redone) + " operations reapplied).";
    return res;
}

void CommandManager::recover_interrupted_operations() {
    // Scan operations table for any in 'INTENT' status (indicates crash during operation)
    auto ops = journal_.get_operations(200);
    for (const auto& op : ops) {
        if (op.status == OpStatus::Intent) {
            // Check filesystem state
            std::error_code ec;
            std::filesystem::path dest(op.dest_path);
            std::filesystem::path src(op.source_path);

            if (op.type == OpType::OpCreateFile || op.type == OpType::OpCreateDir) {
                if (std::filesystem::exists(dest, ec)) {
                    journal_.set_operation_completed(op.id, utc_now_str());
                } else {
                    journal_.update_operation_status(op.id, OpStatus::Failed, "Process terminated before creation completed");
                }
            } else if (op.type == OpType::OpDelete) {
                if (!std::filesystem::exists(src, ec)) {
                    journal_.set_operation_completed(op.id, utc_now_str());
                } else {
                    journal_.update_operation_status(op.id, OpStatus::Failed, "Process terminated before delete completed");
                }
            } else {
                journal_.update_operation_status(op.id, OpStatus::Failed, "Interrupted operation detected on restart");
            }
        }
    }
}

} // namespace fileguard
