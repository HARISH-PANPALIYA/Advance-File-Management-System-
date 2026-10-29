#pragma once
#include "journal.hpp"
#include "filesystem_service.hpp"
#include "recovery_service.hpp"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>

namespace fileguard {

struct CommandResult {
    bool        success{false};
    std::string operation_id;
    std::string message;
    std::string error;
    bool        has_conflict{false};
    std::string conflict_reason;
};

struct BatchProgress {
    std::string transaction_id;
    int32_t     total{0};
    int32_t     completed{0};
    int32_t     failed{0};
    std::string current_item;
    std::string status; // PENDING, IN_PROGRESS, COMPLETED, FAILED, ROLLED_BACK
    std::string error_message;
};

struct BatchItem {
    std::string source;
    std::string destination; // optional or computed
};

class CommandManager {
public:
    CommandManager(Journal& journal, FilesystemService& fs_service, RecoveryService& recovery_service);

    // Core Single Commands
    CommandResult execute_create_file(const std::filesystem::path& path, const std::string& content = "", const std::string& transaction_id = "");
    CommandResult execute_create_directory(const std::filesystem::path& path, const std::string& transaction_id = "");
    CommandResult execute_rename(const std::filesystem::path& source, const std::filesystem::path& dest, const std::string& transaction_id = "");
    CommandResult execute_move(const std::filesystem::path& source, const std::filesystem::path& dest, const std::string& transaction_id = "");
    CommandResult execute_copy(const std::filesystem::path& source, const std::filesystem::path& dest, bool overwrite = false, const std::string& transaction_id = "");
    CommandResult execute_delete(const std::filesystem::path& path, const std::string& transaction_id = ""); // Recoverable delete via quarantine
    CommandResult execute_quarantine(const std::filesystem::path& path, const std::string& reason = "User initiated quarantine", const std::string& transaction_id = "");
    CommandResult execute_overwrite(const std::filesystem::path& path, const std::vector<uint8_t>& new_content, const std::string& transaction_id = "");

    // Global Undo / Redo
    CommandResult undo_last();
    CommandResult redo_last();
    bool can_undo() const;
    bool can_redo() const;
    std::optional<OperationRecord> peek_undo() const;
    std::optional<OperationRecord> peek_redo() const;

    // Independent Content Undo / Redo
    CommandResult undo_content();
    CommandResult redo_content();
    bool can_undo_content() const;
    bool can_redo_content() const;
    std::optional<OperationRecord> peek_undo_content() const;
    std::optional<OperationRecord> peek_redo_content() const;

    // Independent Name / Location Undo / Redo
    CommandResult undo_name();
    CommandResult redo_name();
    bool can_undo_name() const;
    bool can_redo_name() const;
    std::optional<OperationRecord> peek_undo_name() const;
    std::optional<OperationRecord> peek_redo_name() const;

    // Mutation notification binding
    using MutationNotifier = std::function<void(const std::filesystem::path&, const std::filesystem::path&)>;
    void set_mutation_notifier(MutationNotifier cb) { mutation_notifier_ = std::move(cb); }

    // Batch Transactions
    CommandResult execute_batch_delete(const std::vector<std::string>& paths,
                                       std::function<void(const BatchProgress&)> progress_cb = nullptr);
    CommandResult execute_batch_move(const std::vector<BatchItem>& items,
                                     std::function<void(const BatchProgress&)> progress_cb = nullptr);
    CommandResult execute_batch_copy(const std::vector<BatchItem>& items,
                                     std::function<void(const BatchProgress&)> progress_cb = nullptr);
    CommandResult execute_batch_rename(const std::vector<BatchItem>& items,
                                       std::function<void(const BatchProgress&)> progress_cb = nullptr);

    // Rollback / Undo / Redo for Batch
    CommandResult undo_transaction(const std::string& tx_id);
    CommandResult redo_transaction(const std::string& tx_id);

    // Crash recovery check on startup
    void recover_interrupted_operations();

private:
    Journal&           journal_;
    FilesystemService& fs_service_;
    RecoveryService&   recovery_;

    std::string generate_op_id(OpType type);
    CommandResult undo_operation(const OperationRecord& op);
    CommandResult redo_operation(const OperationRecord& op);

    bool check_file_conflict(const std::filesystem::path& p, const std::string& expected_hash, std::string& reason);

    MutationNotifier mutation_notifier_;
    void notify_mutation(const std::filesystem::path& p1, const std::filesystem::path& p2 = "") {
        if (mutation_notifier_) {
            try { mutation_notifier_(p1, p2); } catch (...) {}
        }
    }
};

} // namespace fileguard
