#include "api_server.hpp"
#include "sha256.hpp"
#include <httplib.h>
#include <json.hpp>

#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <chrono>
#include <mutex>

namespace fileguard {

using json = nlohmann::json;

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

} // namespace

class ApiServer::Impl {
public:
    Impl(Journal& journal,
         FilesystemService& fs_service,
         RecoveryService& recovery_service,
         CommandManager& command_mgr,
         EventMonitor& event_monitor,
         BenchmarkService& benchmark_service)
        : journal_(journal),
          fs_service_(fs_service),
          recovery_(recovery_service),
          command_mgr_(command_mgr),
          event_monitor_(event_monitor),
          benchmark_service_(benchmark_service) {
        command_mgr_.set_mutation_notifier([this](const std::filesystem::path& p1, const std::filesystem::path& p2) {
            event_monitor_.register_expected_mutation(p1, p2, 4500);
        });
        setup_routes();
    }

    void listen(const std::string& host, int port) {
        std::cout << "[FileGuard Server] Starting on http://" << host << ":" << port << "\n";
        svr_.listen(host.c_str(), port);
    }

    void stop() {
        svr_.stop();
    }

private:
    Journal&           journal_;
    FilesystemService& fs_service_;
    RecoveryService&   recovery_;
    CommandManager&    command_mgr_;
    EventMonitor&      event_monitor_;
    BenchmarkService&  benchmark_service_;

    httplib::Server    svr_;

    void set_cors(httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    }

    void setup_routes() {
        // Pre-flight CORS
        svr_.Options(".*", [this](const httplib::Request&, httplib::Response& res) {
            set_cors(res);
            res.status = 204;
        });

        svr_.set_post_routing_handler([this](const httplib::Request&, httplib::Response& res) {
            set_cors(res);
        });

        // ── GET /api/health ─────────────────────────────
        svr_.Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
            json out = {
                {"status", "ok"},
                {"app", "FileGuard"},
                {"version", "1.0.0"},
                {"can_undo", command_mgr_.can_undo()},
                {"can_redo", command_mgr_.can_redo()},
                {"can_undo_content", command_mgr_.can_undo_content()},
                {"can_redo_content", command_mgr_.can_redo_content()},
                {"can_undo_name", command_mgr_.can_undo_name()},
                {"can_redo_name", command_mgr_.can_redo_name()}
            };
            auto last_undo = command_mgr_.peek_undo();
            if (last_undo) {
                out["last_undo_op"] = {
                    {"id", last_undo->id},
                    {"type", op_type_str(last_undo->type)},
                    {"source", last_undo->source_path},
                    {"dest", last_undo->dest_path},
                    {"transaction_id", last_undo->transaction_id}
                };
            }
            auto last_redo = command_mgr_.peek_redo();
            if (last_redo) {
                out["last_redo_op"] = {
                    {"id", last_redo->id},
                    {"type", op_type_str(last_redo->type)},
                    {"source", last_redo->source_path},
                    {"dest", last_redo->dest_path},
                    {"transaction_id", last_redo->transaction_id}
                };
            }
            auto last_undo_content = command_mgr_.peek_undo_content();
            if (last_undo_content) {
                out["last_undo_content_op"] = {
                    {"id", last_undo_content->id},
                    {"type", op_type_str(last_undo_content->type)},
                    {"source", last_undo_content->source_path},
                    {"dest", last_undo_content->dest_path},
                    {"transaction_id", last_undo_content->transaction_id}
                };
            }
            auto last_redo_content = command_mgr_.peek_redo_content();
            if (last_redo_content) {
                out["last_redo_content_op"] = {
                    {"id", last_redo_content->id},
                    {"type", op_type_str(last_redo_content->type)},
                    {"source", last_redo_content->source_path},
                    {"dest", last_redo_content->dest_path},
                    {"transaction_id", last_redo_content->transaction_id}
                };
            }
            auto last_undo_name = command_mgr_.peek_undo_name();
            if (last_undo_name) {
                out["last_undo_name_op"] = {
                    {"id", last_undo_name->id},
                    {"type", op_type_str(last_undo_name->type)},
                    {"source", last_undo_name->source_path},
                    {"dest", last_undo_name->dest_path},
                    {"transaction_id", last_undo_name->transaction_id}
                };
            }
            auto last_redo_name = command_mgr_.peek_redo_name();
            if (last_redo_name) {
                out["last_redo_name_op"] = {
                    {"id", last_redo_name->id},
                    {"type", op_type_str(last_redo_name->type)},
                    {"source", last_redo_name->source_path},
                    {"dest", last_redo_name->dest_path},
                    {"transaction_id", last_redo_name->transaction_id}
                };
            }
            res.set_content(out.dump(), "application/json");
        });

        // ── GET /api/files?path=... ─────────────────────
        svr_.Get("/api/files", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                std::string path_str = req.has_param("path") ? req.get_param_value("path") : "";
                if (path_str.empty() || path_str == "." || path_str == "/") {
                    auto w_st = event_monitor_.get_status();
                    if (w_st.is_active && !w_st.watched_path.empty()) {
                        path_str = w_st.watched_path;
                    } else {
                        auto roots = fs_service_.get_approved_roots();
                        path_str = roots.empty() ? "." : roots.front().string();
                    }
                }

                auto listing = fs_service_.list_directory(path_str, false);
                json out;
                out["current_path"] = listing.current_path;
                out["parent_path"] = listing.parent_path;
                out["total_files"] = listing.total_files;
                out["total_directories"] = listing.total_directories;
                out["total_size_bytes"] = listing.total_size_bytes;

                json items = json::array();
                for (const auto& it : listing.items) {
                    items.push_back({
                        {"path", it.path},
                        {"name", it.name},
                        {"extension", it.extension},
                        {"is_directory", it.is_directory},
                        {"size_bytes", it.size_bytes},
                        {"modified_at", it.modified_at},
                        {"is_readonly", it.is_readonly}
                    });
                }
                out["items"] = items;
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── GET /api/metadata?path=... ──────────────────
        svr_.Get("/api/metadata", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                if (!req.has_param("path")) {
                    res.status = 400;
                    res.set_content(json({{"error", "Missing required 'path' parameter"}}).dump(), "application/json");
                    return;
                }
                std::string p = req.get_param_value("path");
                auto meta = fs_service_.get_metadata(p, true);
                json out = {
                    {"path", meta.path},
                    {"name", meta.name},
                    {"extension", meta.extension},
                    {"is_directory", meta.is_directory},
                    {"size_bytes", meta.size_bytes},
                    {"modified_at", meta.modified_at},
                    {"is_readonly", meta.is_readonly},
                    {"hash", meta.hash}
                };
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 404;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── GET /api/file/content?path=... ──────────────
        svr_.Get("/api/file/content", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                if (!req.has_param("path")) {
                    res.status = 400;
                    res.set_content(json({{"error", "Missing required 'path' parameter"}}).dump(), "application/json");
                    return;
                }
                std::string p = req.get_param_value("path");
                std::vector<uint8_t> data;
                fs_service_.read_file(p, data);

                bool is_binary = false;
                size_t check_len = std::min<size_t>(data.size(), 8000);
                for (size_t i = 0; i < check_len; ++i) {
                    if (data[i] == 0) { is_binary = true; break; }
                }

                json out;
                std::string norm_p = p;
                std::replace(norm_p.begin(), norm_p.end(), '\\', '/');
                out["path"] = norm_p;
                out["size_bytes"] = data.size();
                out["is_binary"] = is_binary;
                if (!is_binary) {
                    // Cap preview size to 1MB for safety
                    if (data.size() > 1024 * 1024) {
                        out["content"] = std::string(data.begin(), data.begin() + 1024 * 1024) + "\n... [Preview truncated: file exceeds 1 MB]";
                    } else {
                        out["content"] = std::string(data.begin(), data.end());
                    }
                } else {
                    out["content"] = "[Binary file: preview not supported]";
                }
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 404;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── GET /api/file/download?path=... ─────────────
        svr_.Get("/api/file/download", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                if (!req.has_param("path")) {
                    res.status = 400;
                    res.set_content(json({{"error", "Missing required 'path' parameter"}}).dump(), "application/json");
                    return;
                }
                std::string p = req.get_param_value("path");
                std::vector<uint8_t> data;
                fs_service_.read_file(p, data);

                std::string filename = std::filesystem::path(p).filename().string();
                res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
                res.set_content(reinterpret_cast<const char*>(data.data()), data.size(), "application/octet-stream");
            } catch (const std::exception& e) {
                res.status = 404;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── GET /api/search?root=...&query=... ──────────
        svr_.Get("/api/search", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                SearchOptions opt;
                if (req.has_param("root")) opt.root_path = req.get_param_value("root");
                if (req.has_param("query")) opt.query = req.get_param_value("query");
                if (req.has_param("content")) opt.content_query = req.get_param_value("content");
                if (req.has_param("ext")) {
                    std::string exts = req.get_param_value("ext");
                    std::istringstream iss(exts);
                    std::string token;
                    while (std::getline(iss, token, ',')) {
                        if (!token.empty()) opt.extensions.push_back(token);
                    }
                }
                if (req.has_param("min_size")) {
                    opt.min_size = std::stoull(req.get_param_value("min_size"));
                }
                if (req.has_param("max_size")) {
                    opt.max_size = std::stoull(req.get_param_value("max_size"));
                }

                auto results = fs_service_.search(opt);
                json out = json::array();
                for (const auto& r : results) {
                    out.push_back({
                        {"path", r.path},
                        {"name", r.name},
                        {"extension", r.extension},
                        {"is_directory", r.is_directory},
                        {"size_bytes", r.size_bytes},
                        {"modified_at", r.modified_at}
                    });
                }
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── POST /api/operations ────────────────────────
        svr_.Post("/api/operations", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string type = j.at("type").get<std::string>();

                CommandResult cr;

                if (type == "CREATE_FILE") {
                    std::string path = j.at("path").get<std::string>();
                    std::string content = j.value("content", "");
                    event_monitor_.notify_app_mutation(path);
                    cr = command_mgr_.execute_create_file(path, content);
                } else if (type == "CREATE_DIR") {
                    std::string path = j.at("path").get<std::string>();
                    event_monitor_.notify_app_mutation(path);
                    cr = command_mgr_.execute_create_directory(path);
                } else if (type == "RENAME") {
                    std::string src = j.at("source").get<std::string>();
                    std::string dst = j.at("destination").get<std::string>();
                    event_monitor_.notify_app_mutation(src);
                    event_monitor_.notify_app_mutation(dst);
                    cr = command_mgr_.execute_rename(src, dst);
                } else if (type == "MOVE") {
                    std::string src = j.at("source").get<std::string>();
                    std::string dst = j.at("destination").get<std::string>();
                    event_monitor_.notify_app_mutation(src);
                    event_monitor_.notify_app_mutation(dst);
                    cr = command_mgr_.execute_move(src, dst);
                } else if (type == "COPY") {
                    std::string src = j.at("source").get<std::string>();
                    std::string dst = j.at("destination").get<std::string>();
                    bool overwrite = j.value("overwrite", false);
                    event_monitor_.notify_app_mutation(dst);
                    cr = command_mgr_.execute_copy(src, dst, overwrite);
                } else if (type == "QUARANTINE") {
                    std::string path = j.at("path").get<std::string>();
                    std::string reason = j.value("reason", "User initiated quarantine");
                    event_monitor_.notify_app_mutation(path);
                    cr = command_mgr_.execute_quarantine(path, reason);
                } else if (type == "DELETE") {
                    std::string path = j.at("path").get<std::string>();
                    event_monitor_.notify_app_mutation(path);
                    cr = command_mgr_.execute_delete(path);
                } else if (type == "OVERWRITE") {
                    std::string path = j.at("path").get<std::string>();
                    std::string content = j.at("content").get<std::string>();
                    std::vector<uint8_t> bytes(content.begin(), content.end());
                    event_monitor_.notify_app_mutation(path);
                    cr = command_mgr_.execute_overwrite(path, bytes);
                } else {
                    res.status = 400;
                    res.set_content(json({{"error", "Unsupported operation type: " + type}}).dump(), "application/json");
                    return;
                }

                json out = {
                    {"success", cr.success},
                    {"operation_id", cr.operation_id},
                    {"message", cr.message},
                    {"error", cr.error},
                    {"has_conflict", cr.has_conflict},
                    {"conflict_reason", cr.conflict_reason},
                    {"can_undo", command_mgr_.can_undo()},
                    {"can_redo", command_mgr_.can_redo()}
                };

                if (!cr.success) {
                    res.status = cr.has_conflict ? 409 : 400;
                }
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── GET /api/operations/:id ─────────────────────
        svr_.Get(R"(/api/operations/([a-zA-Z0-9_\-]+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string op_id = req.matches[1];
            auto op = journal_.get_operation(op_id);
            if (!op) {
                res.status = 404;
                res.set_content(json({{"error", "Operation not found: " + op_id}}).dump(), "application/json");
                return;
            }
            json out = {
                {"id", op->id},
                {"seq", op->seq},
                {"type", op_type_str(op->type)},
                {"status", op_status_str(op->status)},
                {"source_path", op->source_path},
                {"dest_path", op->dest_path},
                {"transaction_id", op->transaction_id},
                {"created_at", op->created_at},
                {"completed_at", op->completed_at},
                {"expected_source_hash", op->expected_source_hash},
                {"expected_dest_hash", op->expected_dest_hash},
                {"recovery_artifact_path", op->recovery_artifact_path},
                {"details_json", op->details_json},
                {"is_undoable", op->is_undoable},
                {"is_undone", op->is_undone},
                {"error_message", op->error_message}
            };
            res.set_content(out.dump(), "application/json");
        });

        // ── GET /api/history ────────────────────────────
        svr_.Get("/api/history", [this](const httplib::Request& req, httplib::Response& res) {
            size_t limit = 100;
            if (req.has_param("limit")) {
                try { limit = std::stoull(req.get_param_value("limit")); } catch (...) {}
            }
            auto ops = journal_.get_operations(limit);
            json out = json::array();
            for (const auto& op : ops) {
                out.push_back({
                    {"id", op.id},
                    {"seq", op.seq},
                    {"type", op_type_str(op.type)},
                    {"status", op_status_str(op.status)},
                    {"source_path", op.source_path},
                    {"dest_path", op.dest_path},
                    {"transaction_id", op.transaction_id},
                    {"created_at", op.created_at},
                    {"completed_at", op.completed_at},
                    {"is_undoable", op.is_undoable},
                    {"is_undone", op.is_undone},
                    {"error_message", op.error_message}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        // ── POST /api/undo ──────────────────────────────
        svr_.Post("/api/undo", [this](const httplib::Request& req, httplib::Response& res) {
            std::string scope = "all";
            if (req.has_param("scope")) {
                scope = req.get_param_value("scope");
            } else if (!req.body.empty()) {
                try {
                    auto j = json::parse(req.body);
                    scope = j.value("scope", "all");
                } catch (...) {}
            }

            CommandResult cr;
            if (scope == "content") {
                cr = command_mgr_.undo_content();
            } else if (scope == "name") {
                cr = command_mgr_.undo_name();
            } else {
                cr = command_mgr_.undo_last();
            }

            json out = {
                {"success", cr.success},
                {"operation_id", cr.operation_id},
                {"message", cr.message},
                {"error", cr.error},
                {"has_conflict", cr.has_conflict},
                {"conflict_reason", cr.conflict_reason},
                {"can_undo", command_mgr_.can_undo()},
                {"can_redo", command_mgr_.can_redo()},
                {"can_undo_content", command_mgr_.can_undo_content()},
                {"can_redo_content", command_mgr_.can_redo_content()},
                {"can_undo_name", command_mgr_.can_undo_name()},
                {"can_redo_name", command_mgr_.can_redo_name()}
            };
            if (!cr.success) {
                res.status = cr.has_conflict ? 409 : 400;
            }
            res.set_content(out.dump(), "application/json");
        });

        // ── POST /api/redo ──────────────────────────────
        svr_.Post("/api/redo", [this](const httplib::Request& req, httplib::Response& res) {
            std::string scope = "all";
            if (req.has_param("scope")) {
                scope = req.get_param_value("scope");
            } else if (!req.body.empty()) {
                try {
                    auto j = json::parse(req.body);
                    scope = j.value("scope", "all");
                } catch (...) {}
            }

            CommandResult cr;
            if (scope == "content") {
                cr = command_mgr_.redo_content();
            } else if (scope == "name") {
                cr = command_mgr_.redo_name();
            } else {
                cr = command_mgr_.redo_last();
            }

            json out = {
                {"success", cr.success},
                {"operation_id", cr.operation_id},
                {"message", cr.message},
                {"error", cr.error},
                {"has_conflict", cr.has_conflict},
                {"conflict_reason", cr.conflict_reason},
                {"can_undo", command_mgr_.can_undo()},
                {"can_redo", command_mgr_.can_redo()},
                {"can_undo_content", command_mgr_.can_undo_content()},
                {"can_redo_content", command_mgr_.can_redo_content()},
                {"can_undo_name", command_mgr_.can_undo_name()},
                {"can_redo_name", command_mgr_.can_redo_name()}
            };
            if (!cr.success) {
                res.status = cr.has_conflict ? 409 : 400;
            }
            res.set_content(out.dump(), "application/json");
        });

        // ── GET /api/versions?path=... ──────────────────
        svr_.Get("/api/versions", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                if (!req.has_param("path")) {
                    res.status = 400;
                    res.set_content(json({{"error", "Missing 'path' parameter"}}).dump(), "application/json");
                    return;
                }
                std::string path_str = req.get_param_value("path");
                auto list = recovery_.list_versions(path_str);
                json out = json::array();
                for (const auto& v : list) {
                    out.push_back({
                        {"id", v.id},
                        {"path", v.path},
                        {"created_at", v.created_at},
                        {"size_bytes", v.size_bytes},
                        {"sha256_hash", v.sha256_hash},
                        {"source_op_id", v.source_op_id}
                    });
                }
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── POST /api/versions/checkpoint ───────────────
        svr_.Post("/api/versions/checkpoint", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string path = j.at("path").get<std::string>();
                auto snap = recovery_.create_snapshot(path, "manual_checkpoint");
                json out = {
                    {"success", true},
                    {"id", snap.id},
                    {"path", snap.path},
                    {"size_bytes", snap.size_bytes},
                    {"sha256_hash", snap.sha256_hash},
                    {"created_at", snap.created_at}
                };
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── POST /api/restore ───────────────────────────
        svr_.Post("/api/restore", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                int64_t vid = j.at("version_id").get<int64_t>();
                bool backup_current = j.value("backup_current", true);

                recovery_.restore_snapshot(vid, backup_current);
                res.set_content(json({{"success", true}, {"message", "Restored snapshot #" + std::to_string(vid)}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── GET /api/quarantine ─────────────────────────
        svr_.Get("/api/quarantine", [this](const httplib::Request&, httplib::Response& res) {
            auto list = recovery_.list_quarantine();
            json out = json::array();
            for (const auto& q : list) {
                out.push_back({
                    {"id", q.id},
                    {"original_path", q.original_path},
                    {"quarantined_at", q.quarantined_at},
                    {"size_bytes", q.size_bytes},
                    {"is_directory", q.is_directory},
                    {"sha256_hash", q.sha256_hash},
                    {"is_restored", q.is_restored},
                    {"operation_id", q.operation_id}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        // ── POST /api/quarantine ───────────────────────
        svr_.Post("/api/quarantine", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string path = j.at("path").get<std::string>();
                std::string reason = j.value("reason", "User initiated quarantine");
                event_monitor_.notify_app_mutation(path);
                auto cr = command_mgr_.execute_quarantine(path, reason);
                if (!cr.success) {
                    res.status = 400;
                    res.set_content(json({{"success", false}, {"error", cr.error}}).dump(), "application/json");
                    return;
                }
                res.set_content(json({
                    {"success", true},
                    {"operation_id", cr.operation_id},
                    {"message", cr.message}
                }).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── POST /api/quarantine/restore ────────────────
        svr_.Post("/api/quarantine/restore", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string qid = j.at("id").get<std::string>();
                bool overwrite = j.value("overwrite", false);
                std::string destination = j.value("destination", "");
                recovery_.restore_quarantined_item(qid, overwrite, destination);
                res.set_content(json({{"success", true}, {"message", "Item restored from quarantine."}}).dump(), "application/json");
            } catch (const std::exception& e) {
                std::string err = e.what();
                bool conflict = err.find("already exists") != std::string::npos || err.find("conflict") != std::string::npos;
                res.status = conflict ? 409 : 400;
                res.set_content(json({{"success", false}, {"conflict", conflict}, {"error", err}}).dump(), "application/json");
            }
        });

        // ── POST /api/quarantine/delete ─────────────────
        svr_.Post("/api/quarantine/delete", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string qid = j.at("id").get<std::string>();
                recovery_.permanently_delete_quarantined_item(qid);
                res.set_content(json({{"success", true}, {"message", "Permanently deleted quarantined item."}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── POST /api/transactions ──────────────────────
        svr_.Post("/api/transactions", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string type = j.at("type").get<std::string>();
                CommandResult cr;

                if (type == "BATCH_DELETE") {
                    std::vector<std::string> paths;
                    if (j.contains("paths")) {
                        paths = j.at("paths").get<std::vector<std::string>>();
                    } else if (j.contains("items")) {
                        for (const auto& item : j.at("items")) {
                            if (item.is_string()) paths.push_back(item.get<std::string>());
                            else if (item.is_object() && item.contains("path")) paths.push_back(item["path"].get<std::string>());
                        }
                    }
                    cr = command_mgr_.execute_batch_delete(paths);
                } else if (type == "BATCH_MOVE") {
                    std::vector<BatchItem> items;
                    for (const auto& it : j.at("items")) {
                        items.push_back({it.at("source").get<std::string>(), it.at("destination").get<std::string>()});
                    }
                    cr = command_mgr_.execute_batch_move(items);
                } else if (type == "BATCH_COPY") {
                    std::vector<BatchItem> items;
                    for (const auto& it : j.at("items")) {
                        items.push_back({it.at("source").get<std::string>(), it.at("destination").get<std::string>()});
                    }
                    cr = command_mgr_.execute_batch_copy(items);
                } else if (type == "BATCH_RENAME") {
                    std::vector<BatchItem> items;
                    for (const auto& it : j.at("items")) {
                        items.push_back({it.at("source").get<std::string>(), it.at("destination").get<std::string>()});
                    }
                    cr = command_mgr_.execute_batch_rename(items);
                } else {
                    res.status = 400;
                    res.set_content(json({{"error", "Unknown transaction type: " + type}}).dump(), "application/json");
                    return;
                }

                json out = {
                    {"success", cr.success},
                    {"transaction_id", cr.operation_id},
                    {"message", cr.message},
                    {"error", cr.error}
                };
                if (!cr.success) res.status = 400;
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── POST /api/transactions/undo ─────────────────
        svr_.Post("/api/transactions/undo", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string tx_id;
                if (j.contains("transaction_id") && j["transaction_id"].is_string()) {
                    tx_id = j["transaction_id"].get<std::string>();
                } else if (j.contains("id") && j["id"].is_string()) {
                    tx_id = j["id"].get<std::string>();
                } else if (j.contains("tx_id") && j["tx_id"].is_string()) {
                    tx_id = j["tx_id"].get<std::string>();
                }
                if (tx_id.empty()) {
                    res.status = 400;
                    res.set_content(json({{"success", false}, {"error", "Missing transaction_id or id in request body"}}).dump(), "application/json");
                    return;
                }
                auto cr = command_mgr_.undo_transaction(tx_id);
                if (!cr.success) {
                    res.status = 400;
                    res.set_content(json({{"success", false}, {"error", cr.error}}).dump(), "application/json");
                    return;
                }
                res.set_content(json({{"success", true}, {"message", cr.message.empty() ? "Transaction undone successfully." : cr.message}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── POST /api/transactions/redo ─────────────────
        svr_.Post("/api/transactions/redo", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string tx_id;
                if (j.contains("transaction_id") && j["transaction_id"].is_string()) {
                    tx_id = j["transaction_id"].get<std::string>();
                } else if (j.contains("id") && j["id"].is_string()) {
                    tx_id = j["id"].get<std::string>();
                } else if (j.contains("tx_id") && j["tx_id"].is_string()) {
                    tx_id = j["tx_id"].get<std::string>();
                }
                if (tx_id.empty()) {
                    res.status = 400;
                    res.set_content(json({{"success", false}, {"error", "Missing transaction_id or id in request body"}}).dump(), "application/json");
                    return;
                }
                auto cr = command_mgr_.redo_transaction(tx_id);
                if (!cr.success) {
                    res.status = 400;
                    res.set_content(json({{"success", false}, {"error", cr.error}}).dump(), "application/json");
                    return;
                }
                res.set_content(json({{"success", true}, {"message", cr.message.empty() ? "Transaction redone successfully." : cr.message}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── GET /api/transactions/:id ───────────────────
        svr_.Get(R"(/api/transactions/([a-zA-Z0-9_\-]+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string tx_id = req.matches[1];
            auto tx = journal_.get_transaction(tx_id);
            if (!tx) {
                res.status = 404;
                res.set_content(json({{"error", "Transaction not found: " + tx_id}}).dump(), "application/json");
                return;
            }
            auto ops = journal_.get_operations_for_transaction(tx_id);
            json jops = json::array();
            for (const auto& op : ops) {
                jops.push_back({
                    {"id", op.id},
                    {"seq", op.seq},
                    {"type", op_type_str(op.type)},
                    {"status", op_status_str(op.status)},
                    {"source_path", op.source_path},
                    {"dest_path", op.dest_path},
                    {"created_at", op.created_at},
                    {"completed_at", op.completed_at},
                    {"is_undoable", op.is_undoable},
                    {"is_undone", op.is_undone},
                    {"error_message", op.error_message}
                });
            }
            json out = {
                {"id", tx->id},
                {"type", tx->type},
                {"status", tx->status},
                {"total_ops", tx->total_ops},
                {"completed_ops", tx->completed_ops},
                {"failed_ops", tx->failed_ops},
                {"created_at", tx->created_at},
                {"completed_at", tx->completed_at},
                {"error_message", tx->error_message},
                {"operations", jops}
            };
            res.set_content(out.dump(), "application/json");
        });

        // ── Watcher: Start / Stop / Status ──────────────
        svr_.Post("/api/watch/start", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string path = j.at("path").get<std::string>();
                bool ok = event_monitor_.start(path);
                res.set_content(json({{"success", ok}, {"watched_path", path}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        svr_.Post("/api/watch/stop", [this](const httplib::Request&, httplib::Response& res) {
            event_monitor_.stop();
            res.set_content(json({{"success", true}, {"message", "Watcher stopped"}}).dump(), "application/json");
        });

        svr_.Get("/api/watch/status", [this](const httplib::Request&, httplib::Response& res) {
            auto st = event_monitor_.get_status();
            std::string wp = st.watched_path;
            std::replace(wp.begin(), wp.end(), '\\', '/');
            json out = {
                {"is_active", st.is_active},
                {"watched_path", wp},
                {"backend_type", st.backend_type},
                {"events_captured", st.events_captured},
                {"overflow_count", st.overflow_count},
                {"last_event_time", st.last_event_time}
            };
            res.set_content(out.dump(), "application/json");
        });

        // ── GET /api/events ─────────────────────────────
        svr_.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
            size_t limit = 100;
            if (req.has_param("limit")) {
                try { limit = std::stoull(req.get_param_value("limit")); } catch (...) {}
            }
            auto list = event_monitor_.get_recent_events(limit);
            json out = json::array();
            for (const auto& ev : list) {
                out.push_back({
                    {"id", ev.id},
                    {"kind", ev.kind},
                    {"path", ev.path.string()},
                    {"old_path", ev.old_path ? ev.old_path->string() : ""},
                    {"timestamp", ev.timestamp},
                    {"pid", ev.pid},
                    {"is_external", ev.is_external}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        // ── GET /api/events/stream (SSE) ────────────────
        svr_.Get("/api/events/stream", [this](const httplib::Request&, httplib::Response& res) {
            set_cors(res);
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");

            struct StreamState {
                std::mutex mtx;
                std::vector<std::string> pending_events;
                int listener_id{-1};
            };

            auto state = std::make_shared<StreamState>();

            state->listener_id = event_monitor_.add_listener([state](const EventRecord& ev) {
                json j = {
                    {"id", ev.id},
                    {"kind", ev.kind},
                    {"path", ev.path.string()},
                    {"old_path", ev.old_path ? ev.old_path->string() : ""},
                    {"timestamp", ev.timestamp},
                    {"pid", ev.pid},
                    {"is_external", ev.is_external}
                };
                std::string sse_msg = "data: " + j.dump() + "\n\n";
                std::lock_guard<std::mutex> lk(state->mtx);
                state->pending_events.push_back(sse_msg);
            });

            res.set_chunked_content_provider("text/event-stream",
                [this, state](size_t, httplib::DataSink& sink) -> bool {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    if (!sink.is_writable()) {
                        event_monitor_.remove_listener(state->listener_id);
                        return false;
                    }

                    std::vector<std::string> to_send;
                    {
                        std::lock_guard<std::mutex> lk(state->mtx);
                        to_send.swap(state->pending_events);
                    }

                    for (const auto& msg : to_send) {
                        if (!sink.write(msg.data(), msg.size())) {
                            event_monitor_.remove_listener(state->listener_id);
                            return false;
                        }
                    }

                    // Send periodic heartbeat comment to keep connection alive
                    std::string ping = ": ping\n\n";
                    if (!sink.write(ping.data(), ping.size())) {
                        event_monitor_.remove_listener(state->listener_id);
                        return false;
                    }
                    return true;
                },
                [this, state](bool) {
                    event_monitor_.remove_listener(state->listener_id);
                }
            );
        });

        // ── Benchmarks ──────────────────────────────────
        svr_.Post("/api/benchmarks/run", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string exp = j.at("experiment").get<std::string>();

                json out;
                if (exp == "scan") {
                    int files = j.value("file_count", 1000);
                    int threads = j.value("threads", 4);
                    auto r = benchmark_service_.run_scan_benchmark(files, threads);
                    if (!r.correctness_verified) {
                        r.speedup = 0.0;
                    }
                    out = {
                        {"experiment", r.experiment},
                        {"total_files", r.total_files_generated},
                        {"seq_elapsed_ms", r.seq_elapsed_ms},
                        {"seq_throughput", r.seq_throughput_files_per_sec},
                        {"seq_files_found", r.seq_files_found},
                        {"seq_bytes_found", r.seq_bytes_found},
                        {"conc_elapsed_ms", r.conc_elapsed_ms},
                        {"conc_throughput", r.conc_throughput_files_per_sec},
                        {"conc_files_found", r.conc_files_found},
                        {"conc_bytes_found", r.conc_bytes_found},
                        {"threads", r.threads_used},
                        {"speedup", r.speedup},
                        {"correctness", r.correctness_verified},
                        {"correctness_verified", r.correctness_verified},
                        {"timestamp", r.timestamp}
                    };
                } else if (exp == "watcher") {
                    int files = j.value("file_count", 20);
                    double interval = j.value("interval_ms", 100.0);
                    auto r = benchmark_service_.run_watcher_vs_polling_bench(files, interval);
                    out = {
                        {"experiment", r.experiment},
                        {"file_count", r.test_file_count},
                        {"polling_interval_ms", r.polling_interval_ms},
                        {"polling_elapsed_ms", r.polling_elapsed_ms},
                        {"polling_scans", r.polling_scans},
                        {"polling_min_latency_ms", r.polling_min_latency_ms},
                        {"polling_median_latency_ms", r.polling_median_latency_ms},
                        {"polling_p95_latency_ms", r.polling_p95_latency_ms},
                        {"polling_avg_latency_ms", r.polling_avg_latency_ms},
                        {"watcher_elapsed_ms", r.watcher_elapsed_ms},
                        {"watcher_events", r.watcher_events_received},
                        {"watcher_min_latency_ms", r.watcher_min_latency_ms},
                        {"watcher_median_latency_ms", r.watcher_median_latency_ms},
                        {"watcher_p95_latency_ms", r.watcher_p95_latency_ms},
                        {"watcher_avg_latency_ms", r.watcher_avg_latency_ms},
                        {"latency_speedup", r.latency_speedup},
                        {"cpu_io_summary", r.cpu_io_summary},
                        {"timestamp", r.timestamp}
                    };
                } else if (exp == "journal") {
                    int ops = j.value("ops_count", 100);
                    auto r = benchmark_service_.run_journal_overhead_bench(ops);
                    out = {
                        {"experiment", r.experiment},
                        {"ops_count", r.ops_count},
                        {"raw_fs_elapsed_ms", r.raw_fs_elapsed_ms},
                        {"raw_throughput", r.raw_throughput_ops_per_sec},
                        {"raw_throughput_ops_per_sec", r.raw_throughput_ops_per_sec},
                        {"journaled_elapsed_ms", r.journaled_elapsed_ms},
                        {"journaled_throughput", r.journaled_throughput_ops_per_sec},
                        {"journaled_throughput_ops_per_sec", r.journaled_throughput_ops_per_sec},
                        {"overhead_pct", r.overhead_percentage},
                        {"overhead_percentage", r.overhead_percentage},
                        {"recovery_time_ms", r.recovery_time_ms},
                        {"recovered_ops", r.recovered_ops},
                        {"timestamp", r.timestamp}
                    };
                } else {
                    res.status = 400;
                    res.set_content(json({{"error", "Unknown experiment: " + exp}}).dump(), "application/json");
                    return;
                }
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        svr_.Get("/api/benchmarks", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.has_param("format") && req.get_param_value("format") == "csv") {
                res.set_content(benchmark_service_.export_csv(), "text/csv");
                return;
            }
            auto list = benchmark_service_.get_history(50);
            json out = json::array();
            for (const auto& b : list) {
                json r_json = json::object();
                try { r_json = json::parse(b.results_json); } catch (...) {}
                out.push_back({
                    {"id", b.id},
                    {"type", b.type},
                    {"timestamp", b.timestamp},
                    {"duration_ms", b.duration_ms},
                    {"items_processed", b.items_processed},
                    {"throughput", b.throughput},
                    {"details", r_json}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        svr_.Get(R"(/api/benchmarks/([a-zA-Z0-9_\-]+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            auto b = journal_.get_benchmark(id);
            if (!b) {
                res.status = 404;
                res.set_content(json({{"error", "Benchmark not found: " + id}}).dump(), "application/json");
                return;
            }
            json r_json = json::object();
            try { r_json = json::parse(b->results_json); } catch (...) {}
            json out = {
                {"id", b->id},
                {"type", b->type},
                {"timestamp", b->timestamp},
                {"duration_ms", b->duration_ms},
                {"items_processed", b->items_processed},
                {"throughput", b->throughput},
                {"details", r_json}
            };
            res.set_content(out.dump(), "application/json");
        });

        // ── System Drives ──────────────────────────────
        svr_.Get("/api/system/drives", [this](const httplib::Request&, httplib::Response& res) {
            auto drives = fs_service_.get_system_drives();
            json out = json::array();
            for (const auto& d : drives) {
                out.push_back({
                    {"letter", d.letter},
                    {"label", d.label},
                    {"path", d.path},
                    {"total_bytes", d.total_bytes},
                    {"free_bytes", d.free_bytes}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        // ── Managed Roots ───────────────────────────────
        svr_.Get("/api/roots", [this](const httplib::Request&, httplib::Response& res) {
            auto roots = journal_.list_managed_roots();
            json out = json::array();
            for (const auto& r : roots) {
                out.push_back({
                    {"path", r.path},
                    {"name", r.name},
                    {"is_monitored", r.is_monitored},
                    {"is_demo", r.is_demo},
                    {"added_at", r.added_at}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        svr_.Post("/api/roots/add", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string p = j.at("path").get<std::string>();
                std::string n = j.value("name", "");
                bool mon = j.value("is_monitored", true);
                bool demo = j.value("is_demo", false);

                if (n.empty()) {
                    std::filesystem::path fp(p);
                    n = fp.filename().empty() ? p : fp.filename().string();
                }

                ManagedRootRecord r;
                r.path = p;
                std::replace(r.path.begin(), r.path.end(), '\\', '/');
                r.name = n;
                r.is_monitored = mon;
                r.is_demo = demo;
                journal_.add_managed_root(r);
                fs_service_.add_approved_root(p);

                res.set_content(json({{"success", true}, {"root", {
                    {"path", r.path},
                    {"name", r.name},
                    {"is_monitored", r.is_monitored},
                    {"is_demo", r.is_demo}
                }}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        svr_.Post("/api/roots/remove", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string p = j.at("path").get<std::string>();
                journal_.remove_managed_root(p);
                res.set_content(json({{"success", true}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        svr_.Post("/api/demo/populate", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                std::filesystem::path target = std::filesystem::current_path() / "watched";
                if (!req.body.empty()) {
                    try {
                        auto j = json::parse(req.body);
                        if (j.contains("path")) {
                            target = j["path"].get<std::string>();
                        }
                    } catch (...) {}
                }
                fs_service_.populate_demo_workspace(target);

                ManagedRootRecord mr;
                mr.path = target.string();
                std::replace(mr.path.begin(), mr.path.end(), '\\', '/');
                mr.name = "Demo Workspace (watched/)";
                mr.is_monitored = true;
                mr.is_demo = true;
                journal_.add_managed_root(mr);
                fs_service_.add_approved_root(target);

                res.set_content(json({{"success", true}, {"path", mr.path}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── Document Save with Conflict Detection ───────
        svr_.Post("/api/file/save", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string path_str = j.at("path").get<std::string>();
                std::string content = j.at("content").get<std::string>();
                std::string expected_pre_hash = j.value("expected_pre_hash", "");
                std::string change_summary = j.value("change_summary", "Document edit in FileGuard");

                std::filesystem::path p(path_str);
                std::error_code ec;

                int64_t old_snap_id = 0;
                std::string current_hash = "";
                if (std::filesystem::exists(p, ec)) {
                    current_hash = sha256_file(p);
                    if (!expected_pre_hash.empty() && current_hash != expected_pre_hash) {
                        res.status = 409;
                        res.set_content(json({
                            {"success", false},
                            {"conflict", true},
                            {"error", "Conflict: File on disk was modified externally since loading."},
                            {"current_hash", current_hash},
                            {"expected_hash", expected_pre_hash}
                        }).dump(), "application/json");
                        return;
                    }
                    old_snap_id = recovery_.create_snapshot(p).id;
                }

                event_monitor_.register_expected_mutation(p, "", 4500);

                std::vector<uint8_t> data(content.begin(), content.end());
                if (!fs_service_.write_file(p, data)) {
                    res.status = 500;
                    res.set_content(json({{"error", "Failed to write file: " + p.string()}}).dump(), "application/json");
                    return;
                }

                std::string new_hash = sha256_file(p);
                uint64_t new_size = std::filesystem::file_size(p, ec);
                if (ec) new_size = data.size();

                int64_t new_snap_id = recovery_.create_snapshot(p).id;

                auto fid = journal_.get_or_create_file_identity(p.string());
                int64_t rev_num = journal_.get_next_revision_number(fid.id);
                std::string rev_id = journal_.generate_sequential_id("REV");

                ContentRevisionRecord rev;
                rev.id = rev_id;
                rev.file_identity_id = fid.id;
                rev.revision_number = rev_num;
                rev.path = p.string();
                std::replace(rev.path.begin(), rev.path.end(), '\\', '/');
                rev.size_bytes = new_size;
                rev.sha256_hash = new_hash;
                rev.snapshot_id = new_snap_id;
                rev.source_type = "USER_EDIT";
                rev.change_summary = change_summary;
                journal_.record_content_revision(rev);

                OperationRecord op;
                op.id = journal_.generate_sequential_id("OP");
                op.type = current_hash.empty() ? OpType::OpCreateFile : OpType::OpOverwrite;
                op.status = OpStatus::Executed;
                op.dest_path = rev.path;
                op.expected_source_hash = current_hash;
                op.expected_dest_hash = new_hash;

                json det;
                det["revision_id"] = rev_id;
                det["revision_number"] = rev_num;
                det["pre_version_id"] = old_snap_id;
                det["post_version_id"] = new_snap_id;
                det["initial_content"] = content;
                op.details_json = det.dump();

                journal_.log_operation_intent(op);
                journal_.set_operation_completed(op.id, utc_now_rfc3339());

                res.set_content(json({
                    {"success", true},
                    {"new_hash", new_hash},
                    {"revision_id", rev_id},
                    {"revision_number", rev_num},
                    {"file_identity_id", fid.id}
                }).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── History & Revisions ─────────────────────────
        svr_.Get("/api/history/file", [this](const httplib::Request& req, httplib::Response& res) {
            std::string path = req.get_param_value("path");
            if (path.empty()) {
                res.status = 400;
                res.set_content(json({{"error", "Missing path parameter"}}).dump(), "application/json");
                return;
            }
            auto fid = journal_.find_file_identity_by_path(path);
            auto name_loc_list = journal_.get_name_location_history_for_path(path);
            auto revs_list = journal_.get_content_revisions_for_path(path);
            auto ops_list = journal_.get_operations_for_file(path, 100);

            json nl_json = json::array();
            for (const auto& nl : name_loc_list) {
                nl_json.push_back({
                    {"id", nl.id},
                    {"op_type", nl.op_type},
                    {"source_path", nl.source_path},
                    {"dest_path", nl.dest_path},
                    {"created_at", nl.created_at}
                });
            }

            json revs_json = json::array();
            for (const auto& r : revs_list) {
                revs_json.push_back({
                    {"id", r.id},
                    {"revision_number", r.revision_number},
                    {"path", r.path},
                    {"size_bytes", r.size_bytes},
                    {"sha256_hash", r.sha256_hash},
                    {"snapshot_id", r.snapshot_id},
                    {"source_type", r.source_type},
                    {"change_summary", r.change_summary},
                    {"created_at", r.created_at}
                });
            }

            json ops_json = json::array();
            for (const auto& op : ops_list) {
                ops_json.push_back({
                    {"id", op.id},
                    {"seq", op.seq},
                    {"type", op_type_str(op.type)},
                    {"status", op_status_str(op.status)},
                    {"source", op.source_path},
                    {"dest", op.dest_path},
                    {"created_at", op.created_at},
                    {"is_undoable", op.is_undoable},
                    {"is_undone", op.is_undone}
                });
            }

            auto cand_list = fid ? journal_.get_external_candidates_for_identity(fid->id)
                                 : journal_.get_external_candidates_for_path(path);
            json cand_json = json::array();
            for (const auto& c : cand_list) {
                cand_json.push_back({
                    {"id", c.id},
                    {"seq", c.detection_seq},
                    {"path", c.path},
                    {"old_path", c.old_path},
                    {"file_identity_id", c.file_identity_id},
                    {"change_type", c.change_type},
                    {"detected_at", c.detected_at},
                    {"status", c.status},
                    {"candidate_snapshot_path", c.candidate_snapshot_path},
                    {"candidate_hash", c.candidate_hash},
                    {"candidate_size", c.candidate_size},
                    {"prior_accepted_hash", c.prior_accepted_hash},
                    {"prior_accepted_snapshot_id", c.prior_accepted_snapshot_id},
                    {"resolution_notes", c.resolution_notes},
                    {"resolved_at", c.resolved_at}
                });
            }

            json out = {
                {"file_identity", fid ? json({
                    {"id", fid->id},
                    {"current_path", fid->current_path},
                    {"initial_path", fid->initial_path},
                    {"created_at", fid->created_at},
                    {"last_modified_at", fid->last_modified_at}
                }) : json(nullptr)},
                {"name_location_history", nl_json},
                {"content_revisions", revs_json},
                {"operations", ops_json},
                {"external_candidates", cand_json}
            };
            res.set_content(out.dump(), "application/json");
        });

        svr_.Get("/api/history/folder", [this](const httplib::Request& req, httplib::Response& res) {
            std::string path = req.get_param_value("path");
            if (path.empty()) {
                res.status = 400;
                res.set_content(json({{"error", "Missing path parameter"}}).dump(), "application/json");
                return;
            }
            auto ops = journal_.get_operations_for_folder(path, 100);
            json out = json::array();
            for (const auto& op : ops) {
                out.push_back({
                    {"id", op.id},
                    {"seq", op.seq},
                    {"type", op_type_str(op.type)},
                    {"status", op_status_str(op.status)},
                    {"source", op.source_path},
                    {"dest", op.dest_path},
                    {"created_at", op.created_at},
                    {"is_undoable", op.is_undoable},
                    {"is_undone", op.is_undone}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        svr_.Get("/api/revisions", [this](const httplib::Request& req, httplib::Response& res) {
            std::string path = req.get_param_value("path");
            auto revs = journal_.get_content_revisions_for_path(path);
            json out = json::array();
            for (const auto& r : revs) {
                out.push_back({
                    {"id", r.id},
                    {"revision_number", r.revision_number},
                    {"path", r.path},
                    {"size_bytes", r.size_bytes},
                    {"sha256_hash", r.sha256_hash},
                    {"snapshot_id", r.snapshot_id},
                    {"source_type", r.source_type},
                    {"change_summary", r.change_summary},
                    {"created_at", r.created_at}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        svr_.Post("/api/revisions/restore", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                int64_t snap_id = j.at("snapshot_id").get<int64_t>();
                std::string target_path = j.at("path").get<std::string>();

                std::filesystem::path tp(target_path);
                event_monitor_.notify_app_mutation(tp);
                recovery_.restore_snapshot(snap_id);

                std::string new_hash = sha256_file(tp);
                auto fid = journal_.get_or_create_file_identity(tp.string());
                int64_t rev_num = journal_.get_next_revision_number(fid.id);
                std::string rev_id = journal_.generate_sequential_id("REV");

                ContentRevisionRecord rev;
                rev.id = rev_id;
                rev.file_identity_id = fid.id;
                rev.revision_number = rev_num;
                rev.path = tp.string();
                std::replace(rev.path.begin(), rev.path.end(), '\\', '/');
                rev.size_bytes = std::filesystem::file_size(tp);
                rev.sha256_hash = new_hash;
                rev.snapshot_id = snap_id;
                rev.source_type = "UNDO_RESTORE";
                rev.change_summary = "Restored from revision snapshot " + std::to_string(snap_id);
                journal_.record_content_revision(rev);

                res.set_content(json({{"success", true}, {"revision_id", rev_id}, {"new_hash", new_hash}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── External Changes Workflow ───────────────────
        svr_.Get("/api/external-changes", [this](const httplib::Request& req, httplib::Response& res) {
            std::string status = req.has_param("status") ? req.get_param_value("status") : "";
            auto list = journal_.list_external_candidates(status);
            json out = json::array();
            for (const auto& c : list) {
                out.push_back({
                    {"id", c.id},
                    {"seq", c.detection_seq},
                    {"path", c.path},
                    {"old_path", c.old_path},
                    {"file_identity_id", c.file_identity_id},
                    {"change_type", c.change_type},
                    {"detected_at", c.detected_at},
                    {"status", c.status},
                    {"candidate_snapshot_path", c.candidate_snapshot_path},
                    {"candidate_hash", c.candidate_hash},
                    {"candidate_size", c.candidate_size},
                    {"prior_accepted_hash", c.prior_accepted_hash},
                    {"prior_accepted_snapshot_id", c.prior_accepted_snapshot_id},
                    {"resolution_notes", c.resolution_notes},
                    {"resolved_at", c.resolved_at}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        svr_.Post("/api/external-changes/accept", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string id = j.at("id").get<std::string>();
                std::string notes = j.value("notes", "Accepted external modification");

                auto opt = journal_.get_external_candidate(id);
                if (!opt) {
                    res.status = 404;
                    res.set_content(json({{"error", "External candidate not found: " + id}}).dump(), "application/json");
                    return;
                }

                journal_.update_external_candidate_status(id, "ACCEPTED", notes);

                auto fid = journal_.get_or_create_file_identity(opt->path);
                int64_t rev_num = journal_.get_next_revision_number(fid.id);
                std::string rev_id = journal_.generate_sequential_id("REV");

                ContentRevisionRecord rev;
                rev.id = rev_id;
                rev.file_identity_id = fid.id;
                rev.revision_number = rev_num;
                rev.path = opt->path;
                rev.size_bytes = opt->candidate_size;
                rev.sha256_hash = opt->candidate_hash;
                rev.snapshot_id = 0;
                rev.source_type = "EXTERNAL_ACCEPTED";
                rev.change_summary = "Accepted external change (" + id + "): " + notes;
                journal_.record_content_revision(rev);

                res.set_content(json({{"success", true}, {"candidate_id", id}, {"revision_id", rev_id}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        svr_.Post("/api/external-changes/reject", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string id = j.at("id").get<std::string>();
                std::string notes = j.value("notes", "Rejected external modification; restored prior version");

                auto opt = journal_.get_external_candidate(id);
                if (!opt) {
                    res.status = 404;
                    res.set_content(json({{"error", "External candidate not found: " + id}}).dump(), "application/json");
                    return;
                }

                if (opt->change_type == "EXTERNAL_RENAME" && !opt->old_path.empty()) {
                    std::filesystem::path cur_p(opt->path);
                    std::filesystem::path orig_p(opt->old_path);
                    std::error_code ec;

                    if (!std::filesystem::exists(cur_p, ec)) {
                        res.status = 400;
                        res.set_content(json({{"error", "Current renamed file no longer exists: " + cur_p.string()}}).dump(), "application/json");
                        return;
                    }
                    if (std::filesystem::exists(orig_p, ec)) {
                        res.status = 409;
                        res.set_content(json({{"error", "Cannot restore original name: destination already occupied: " + orig_p.string()}}).dump(), "application/json");
                        return;
                    }

                    event_monitor_.register_expected_mutation(cur_p, orig_p, 4500);
                    fs_service_.rename_item(cur_p, orig_p);

                    if (!opt->file_identity_id.empty()) {
                        journal_.update_file_identity_path(opt->file_identity_id, orig_p.string());
                        NameLocationRecord nl;
                        nl.id = journal_.generate_sequential_id("OP");
                        nl.file_identity_id = opt->file_identity_id;
                        nl.seq = 0;
                        nl.op_type = "RENAME_RESTORE";
                        nl.source_path = cur_p.string();
                        nl.dest_path = orig_p.string();
                        nl.created_at = utc_now_rfc3339();
                        journal_.record_name_location(nl);
                    }

                    journal_.update_external_candidate_status(id, "REJECTED", "Rejected external rename; restored filename back to " + orig_p.filename().string());
                    res.set_content(json({{"success", true}, {"candidate_id", id}, {"status", "REJECTED"}, {"restored_path", orig_p.string()}}).dump(), "application/json");
                    return;
                }

                std::filesystem::path live_path(opt->path);
                event_monitor_.register_expected_mutation(live_path, "", 4500);

                if (opt->prior_accepted_snapshot_id > 0) {
                    recovery_.restore_snapshot(opt->prior_accepted_snapshot_id);
                } else if (opt->change_type == "EXTERNAL_CREATE") {
                    std::error_code ec;
                    if (std::filesystem::exists(live_path, ec)) {
                        recovery_.quarantine_item(live_path, "reject_" + id);
                    }
                }

                journal_.update_external_candidate_status(id, "REJECTED", notes);

                res.set_content(json({{"success", true}, {"candidate_id", id}, {"status", "REJECTED"}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        svr_.Post("/api/external-changes/recover", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                std::string id = j.at("id").get<std::string>();
                std::string mode = j.value("mode", "new_copy");
                std::string target_path = j.value("target_path", "");

                auto opt = journal_.get_external_candidate(id);
                if (!opt) {
                    res.status = 404;
                    res.set_content(json({{"error", "Candidate not found: " + id}}).dump(), "application/json");
                    return;
                }

                std::filesystem::path cand_snap(opt->candidate_snapshot_path);
                if (!std::filesystem::exists(cand_snap)) {
                    res.status = 404;
                    res.set_content(json({{"error", "Candidate snapshot archive missing: " + cand_snap.string()}}).dump(), "application/json");
                    return;
                }

                std::error_code ec;
                if (mode == "new_copy") {
                    if (target_path.empty()) {
                        std::filesystem::path orig_p(opt->path);
                        target_path = (orig_p.parent_path() / (orig_p.stem().string() + "_recovered_" + id + orig_p.extension().string())).string();
                    }
                    std::filesystem::path dest_p(target_path);
                    event_monitor_.notify_app_mutation(dest_p);
                    std::filesystem::copy_file(cand_snap, dest_p, std::filesystem::copy_options::overwrite_existing, ec);

                    journal_.update_external_candidate_status(id, "RECOVERED", "Recovered as separate file: " + target_path);
                    res.set_content(json({{"success", true}, {"recovered_path", target_path}}).dump(), "application/json");
                } else {
                    std::filesystem::path dest_p(opt->path);
                    event_monitor_.notify_app_mutation(dest_p);
                    std::filesystem::copy_file(cand_snap, dest_p, std::filesystem::copy_options::overwrite_existing, ec);

                    auto fid = journal_.get_or_create_file_identity(opt->path);
                    int64_t rev_num = journal_.get_next_revision_number(fid.id);
                    std::string rev_id = journal_.generate_sequential_id("REV");

                    ContentRevisionRecord rev;
                    rev.id = rev_id;
                    rev.file_identity_id = fid.id;
                    rev.revision_number = rev_num;
                    rev.path = opt->path;
                    rev.size_bytes = opt->candidate_size;
                    rev.sha256_hash = opt->candidate_hash;
                    rev.snapshot_id = 0;
                    rev.source_type = "EXTERNAL_ACCEPTED";
                    rev.change_summary = "Recovered rejected external change (" + id + ") as new revision";
                    journal_.record_content_revision(rev);

                    journal_.update_external_candidate_status(id, "RECOVERED", "Applied rejected candidate as revision " + std::to_string(rev_num));
                    res.set_content(json({{"success", true}, {"revision_id", rev_id}, {"revision_number", rev_num}}).dump(), "application/json");
                }
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        svr_.Get("/api/external-changes/diff", [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.get_param_value("id");
            auto opt = journal_.get_external_candidate(id);
            if (!opt) {
                res.status = 404;
                res.set_content(json({{"error", "Candidate not found: " + id}}).dump(), "application/json");
                return;
            }

            std::string candidate_text = "";
            {
                std::ifstream in(opt->candidate_snapshot_path, std::ios::binary);
                if (in) {
                    std::ostringstream ss;
                    ss << in.rdbuf();
                    candidate_text = ss.str();
                }
            }

            std::string prior_text = "";
            if (opt->prior_accepted_snapshot_id > 0) {
                try {
                    auto pair = journal_.version_content(opt->prior_accepted_snapshot_id);
                    prior_text = std::string(pair.second.begin(), pair.second.end());
                } catch (...) {}
            }
            // If prior_accepted_snapshot_id == 0, prior_text is strictly empty ("")
            // NEVER load the modified live file on disk, which caused identical diff displays.

            res.set_content(json({
                {"candidate_id", id},
                {"path", opt->path},
                {"prior_content", prior_text},
                {"candidate_content", candidate_text},
                {"candidate_hash", opt->candidate_hash},
                {"prior_hash", opt->prior_accepted_hash}
            }).dump(), "application/json");
        });

        // ── Folder Insights ─────────────────────────────
        svr_.Get("/api/insights", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                std::string path_str = req.has_param("path") ? req.get_param_value("path") : "";
                if (path_str.empty()) {
                    auto roots = fs_service_.get_approved_roots();
                    path_str = roots.empty() ? std::filesystem::current_path().string() : roots.front().string();
                }

                std::filesystem::path folder(path_str);
                auto insights = fs_service_.get_folder_insights(folder, 5000);

                json types_arr = json::array();
                for (const auto& t : insights.type_distribution) {
                    types_arr.push_back({
                        {"extension", t.extension},
                        {"count", t.count},
                        {"total_bytes", t.total_bytes}
                    });
                }

                json dups_arr = json::array();
                for (const auto& d : insights.duplicates) {
                    dups_arr.push_back({
                        {"sha256_hash", d.sha256_hash},
                        {"size_bytes", d.size_bytes},
                        {"paths", d.paths},
                        {"reclaimable_bytes", (d.paths.size() - 1) * d.size_bytes}
                    });
                }

                json largest_arr = json::array();
                for (const auto& lf : insights.largest_files) {
                    largest_arr.push_back({
                        {"path", lf.path},
                        {"name", lf.name},
                        {"size_bytes", lf.size_bytes}
                    });
                }

                json out = {
                    {"folder_path", insights.folder_path},
                    {"total_files", insights.total_files},
                    {"total_directories", insights.total_directories},
                    {"total_bytes", insights.total_bytes},
                    {"reclaimable_bytes", insights.reclaimable_bytes},
                    {"type_distribution", types_arr},
                    {"duplicates", dups_arr},
                    {"largest_files", largest_arr},
                    {"empty_directories", insights.empty_directories}
                };
                res.set_content(out.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });

        // ── Transactions ────────────────────────────────
        svr_.Get("/api/transactions", [this](const httplib::Request&, httplib::Response& res) {
            auto txs = journal_.list_transactions(50);
            json out = json::array();
            for (const auto& tx : txs) {
                auto ops = journal_.get_operations_for_transaction(tx.id);
                json ops_arr = json::array();
                for (const auto& op : ops) {
                    ops_arr.push_back({
                        {"id", op.id},
                        {"seq", op.seq},
                        {"type", op_type_str(op.type)},
                        {"status", op_status_str(op.status)},
                        {"source", op.source_path},
                        {"dest", op.dest_path},
                        {"created_at", op.created_at},
                        {"is_undoable", op.is_undoable},
                        {"is_undone", op.is_undone}
                    });
                }
                out.push_back({
                    {"id", tx.id},
                    {"type", tx.type},
                    {"status", tx.status},
                    {"total_ops", tx.total_ops},
                    {"completed_ops", tx.completed_ops},
                    {"failed_ops", tx.failed_ops},
                    {"created_at", tx.created_at},
                    {"completed_at", tx.completed_at},
                    {"error_message", tx.error_message},
                    {"operations", ops_arr}
                });
            }
            res.set_content(out.dump(), "application/json");
        });

        // ── Settings ────────────────────────────────────
        svr_.Get("/api/settings", [this](const httplib::Request&, httplib::Response& res) {
            auto roots = fs_service_.get_approved_roots();
            json roots_arr = json::array();
            for (const auto& r : roots) {
                std::string rs = r.string();
                std::replace(rs.begin(), rs.end(), '\\', '/');
                roots_arr.push_back(rs);
            }

            auto storage_stats = recovery_.get_storage_usage();
            auto watch_st = event_monitor_.get_status();
            std::string wp = watch_st.watched_path;
            std::replace(wp.begin(), wp.end(), '\\', '/');
            std::string qd = recovery_.get_quarantine_dir().string();
            std::replace(qd.begin(), qd.end(), '\\', '/');
            std::string sd = recovery_.get_snapshots_dir().string();
            std::replace(sd.begin(), sd.end(), '\\', '/');

            json out = {
                {"approved_roots", roots_arr},
                {"watched_path", wp},
                {"watcher_active", watch_st.is_active},
                {"quarantine_dir", qd},
                {"snapshots_dir", sd},
                {"storage_usage", {
                    {"quarantine_bytes", storage_stats.quarantine_bytes},
                    {"snapshots_bytes", storage_stats.snapshots_bytes},
                    {"total_bytes", storage_stats.total_bytes},
                    {"quarantine_items", storage_stats.quarantine_items_count},
                    {"snapshots_count", storage_stats.snapshots_count}
                }}
            };
            res.set_content(out.dump(), "application/json");
        });

        svr_.Post("/api/settings", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = json::parse(req.body);
                if (j.contains("approved_roots")) {
                    std::vector<std::filesystem::path> roots;
                    for (const auto& r : j["approved_roots"]) {
                        roots.emplace_back(r.get<std::string>());
                    }
                    fs_service_.set_approved_roots(roots);
                }
                res.set_content(json({{"success", true}}).dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json({{"error", e.what()}}).dump(), "application/json");
            }
        });
    }
};

ApiServer::ApiServer(Journal& journal,
                     FilesystemService& fs_service,
                     RecoveryService& recovery_service,
                     CommandManager& command_mgr,
                     EventMonitor& event_monitor,
                     BenchmarkService& benchmark_service)
    : journal_(journal),
      fs_service_(fs_service),
      recovery_(recovery_service),
      command_mgr_(command_mgr),
      event_monitor_(event_monitor),
      benchmark_service_(benchmark_service),
      impl_(std::make_unique<Impl>(journal, fs_service, recovery_service, command_mgr, event_monitor, benchmark_service)) {}

ApiServer::~ApiServer() = default;

void ApiServer::listen(const std::string& host, int port) {
    impl_->listen(host, port);
}

void ApiServer::stop() {
    impl_->stop();
}

} // namespace fileguard
