#include "journal.hpp"
#include "passthrough.hpp"
#include "search.hpp"
#include "watch.hpp"
#include "filesystem_service.hpp"
#include "recovery_service.hpp"
#include "command_manager.hpp"
#include "event_monitor.hpp"
#include "benchmark_service.hpp"
#include "api_server.hpp"
#include "fileguard_tests.hpp"

#include <json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

using namespace fileguard;

// ─────────────────────────────────────────────
// .env loader
// ─────────────────────────────────────────────
static void load_dotenv(const std::string& file_path) {
    std::ifstream f(file_path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find('#');
        if (pos != std::string::npos) line = line.substr(0, pos);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1, value.size() - 2);

#ifdef _WIN32
        SetEnvironmentVariableA(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 0);
#endif
    }
}

static void print_usage() {
    std::cout << "FileGuard — Event-Driven File Management and Recovery System\n"
              << "Usage: safevault [options] <command> [args]\n\n"
              << "Options:\n"
              << "  --database <path>       SQLite WAL journal path (default: storage/safevault.sqlite3)\n\n"
              << "Commands:\n"
              << "  init                    Initialize database and storage directories\n"
              << "  serve                   Start full-stack HTTP/SSE API server\n"
              << "    --bind <host:port>    Bind address (default: 127.0.0.1:8787)\n"
              << "    --watch <path>        Directory to monitor on launch (default: watched)\n"
              << "  undo                    Undo the most recent operation\n"
              << "  redo                    Redo the most recently undone operation\n"
              << "  status                  Show system status and undo/redo state\n"
              << "  history                 Display operation history journal\n"
              << "  quarantine              List items in quarantine vault\n"
              << "  demo                    Populate demo workspace with test files\n";
}

// ─────────────────────────────────────────────
// Main Entry Point
// ─────────────────────────────────────────────
int main(int argc, char** argv) {
    load_dotenv(".env");
    load_dotenv("../.env");

    std::string db_path = "storage/safevault.sqlite3";
    std::string command = "";
    std::string srv_bind = "127.0.0.1:8787";
    std::string watch_dir_default = "watched";
    int history_limit = 50;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--database" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--bind" && i + 1 < argc) {
            srv_bind = argv[++i];
        } else if (arg == "--watch" && i + 1 < argc) {
            watch_dir_default = argv[++i];
        } else if (arg == "--limit" && i + 1 < argc) {
            history_limit = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] != '-' && command.empty()) {
            command = arg;
        }
    }

    if (command.empty()) {
        command = "serve"; // Default to serve if no command provided
    }

    try {
        if (command == "init") {
            Journal jrnl(db_path);
            std::filesystem::path storage_dir = std::filesystem::path(db_path).parent_path();
            if (storage_dir.empty()) storage_dir = "storage";
            std::filesystem::create_directories(storage_dir / "snapshots");
            std::filesystem::create_directories(storage_dir / "quarantine");
            std::cout << "[FileGuard] Initialized database and recovery store at: " << db_path << "\n";
            return 0;
        }

        if (command == "serve") {
            auto colon = srv_bind.rfind(':');
            std::string host = colon != std::string::npos ? srv_bind.substr(0, colon) : "127.0.0.1";
            int port = colon != std::string::npos ? std::stoi(srv_bind.substr(colon + 1)) : 8787;

            std::filesystem::path storage_dir = std::filesystem::path(db_path).parent_path();
            if (storage_dir.empty()) storage_dir = "storage";
            std::filesystem::create_directories(storage_dir / "snapshots");
            std::filesystem::create_directories(storage_dir / "quarantine");

            Journal journal(db_path);
            std::vector<std::filesystem::path> approved_roots = {
                std::filesystem::current_path(),
                std::filesystem::current_path() / "watched",
                storage_dir
            };
            FilesystemService fs_service(approved_roots);
            RecoveryService recovery_service(journal, fs_service, storage_dir);
            CommandManager command_mgr(journal, fs_service, recovery_service);
            EventMonitor event_monitor(journal, fs_service);
            BenchmarkService benchmark_service(journal, fs_service, storage_dir / "benchmarks_scratch");

            std::error_code ec;
            if (!watch_dir_default.empty()) {
                std::filesystem::create_directories(watch_dir_default, ec);
                event_monitor.start(watch_dir_default);
                std::cout << "[FileGuard] Active native watcher attached to: " << watch_dir_default << "\n";
            }

            ApiServer server(journal, fs_service, recovery_service, command_mgr, event_monitor, benchmark_service);
            std::cout << "[FileGuard] Server listening on http://" << host << ":" << port << "\n";
            server.listen(host, port);
            return 0;
        }

        if (command == "undo") {
            Journal journal(db_path);
            FilesystemService fs_service;
            RecoveryService recovery(journal, fs_service, std::filesystem::path(db_path).parent_path());
            CommandManager mgr(journal, fs_service, recovery);

            auto res = mgr.undo_last();
            if (res.success) {
                std::cout << "[SUCCESS] " << res.message << "\n";
            } else {
                std::cerr << "[ERROR] " << res.error << "\n";
                return 1;
            }
            return 0;
        }

        if (command == "redo") {
            Journal journal(db_path);
            FilesystemService fs_service;
            RecoveryService recovery(journal, fs_service, std::filesystem::path(db_path).parent_path());
            CommandManager mgr(journal, fs_service, recovery);

            auto res = mgr.redo_last();
            if (res.success) {
                std::cout << "[SUCCESS] " << res.message << "\n";
            } else {
                std::cerr << "[ERROR] " << res.error << "\n";
                return 1;
            }
            return 0;
        }

        if (command == "status") {
            Journal journal(db_path);
            FilesystemService fs_service;
            RecoveryService recovery(journal, fs_service, std::filesystem::path(db_path).parent_path());
            CommandManager mgr(journal, fs_service, recovery);

            std::cout << "========================================\n"
                      << "  FileGuard Status\n"
                      << "========================================\n"
                      << "Database: " << db_path << "\n"
                      << "Can Undo: " << (mgr.can_undo() ? "YES" : "NO") << "\n"
                      << "Can Redo: " << (mgr.can_redo() ? "YES" : "NO") << "\n";

            auto last_undo = mgr.peek_undo();
            if (last_undo) {
                std::cout << "Next Undo: [" << last_undo->id << "] "
                          << op_type_str(last_undo->type) << " " << last_undo->dest_path << "\n";
            }
            return 0;
        }

        if (command == "test") {
            return fileguard::run_all_tests();
        }

        if (command == "demo") {
            FilesystemService fs_service;
            fs_service.populate_demo_workspace("watched");
            std::cout << "[FileGuard] Populated demo workspace at 'watched/'\n";
            return 0;
        }

        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL ERROR] " << e.what() << "\n";
        return 1;
    }
}
