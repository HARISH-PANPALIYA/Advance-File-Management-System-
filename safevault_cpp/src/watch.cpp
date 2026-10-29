#include "watch.hpp"


#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/inotify.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

// ─────────────────────────────────────────────
// Internal event passed from watcher → journal thread
// ─────────────────────────────────────────────
struct WatchEvent {
    EventKind             kind;
    std::filesystem::path path;
};

// ─────────────────────────────────────────────
// Thread-safe queue
// ─────────────────────────────────────────────
template<typename T>
class TsQueue {
public:
    void push(T item) {
        std::lock_guard<std::mutex> lk(mtx_);
        q_.push(std::move(item));
        cv_.notify_one();
    }
    // Returns false if stopped
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&]{ return !q_.empty() || stopped_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
    void stop() {
        std::lock_guard<std::mutex> lk(mtx_);
        stopped_ = true;
        cv_.notify_all();
    }
private:
    std::queue<T>           q_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    stopped_{false};
};

// ─────────────────────────────────────────────
// Snapshot cache (protected by mutex)
// ─────────────────────────────────────────────
using SnapshotMap = std::unordered_map<std::string, std::vector<uint8_t>>;

static std::vector<uint8_t> read_bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

static SnapshotMap initial_snapshots(const std::filesystem::path& root) {
    SnapshotMap snap;
    std::error_code ec;
    for (auto& entry : std::filesystem::recursive_directory_iterator(root,
             std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (entry.is_regular_file(ec)) {
            auto data = read_bytes(entry.path());
            if (!data.empty())
                snap[entry.path().string()] = std::move(data);
        }
    }
    return snap;
}

// ─────────────────────────────────────────────
// Journal loop — runs on background thread
// ─────────────────────────────────────────────
static void journal_loop(TsQueue<WatchEvent>& queue,
                         Journal& journal,
                         SnapshotMap& snapshots,
                         std::mutex& snap_mtx)
{
    WatchEvent ev;
    while (queue.pop(ev)) {
        std::string key = ev.path.string();

        // Save previous content before modify / delete
        if (ev.kind == EventKind::Modify || ev.kind == EventKind::Delete) {
            std::lock_guard<std::mutex> lk(snap_mtx);
            auto it = snapshots.find(key);
            if (it != snapshots.end()) {
                try { journal.save_version(ev.path, it->second); }
                catch (const std::exception& e) {
                    std::cerr << "Could not save version for "
                              << key << ": " << e.what() << "\n";
                }
            }
        }

        // Record the event
        try {
            journal.append(EventRecord::now(ev.kind, ev.path, std::nullopt,
#ifdef _WIN32
                           static_cast<uint32_t>(GetCurrentProcessId())
#else
                           static_cast<uint32_t>(getpid())
#endif
                           ));
        } catch (const std::exception& e) {
            std::cerr << "Could not journal " << key << ": " << e.what() << "\n";
            continue;
        }

        // Update snapshot
        {
            std::lock_guard<std::mutex> lk(snap_mtx);
            if (ev.kind != EventKind::Delete) {
                auto data = read_bytes(ev.path);
                if (!data.empty()) snapshots[key] = std::move(data);
            } else {
                snapshots.erase(key);
            }
        }

        const char* label =
            ev.kind == EventKind::Create ? "CREATE" :
            ev.kind == EventKind::Modify ? "MODIFY" :
            ev.kind == EventKind::Delete ? "DELETE" : "RENAME";
        std::cout << "[" << label << "] " << key << "\n";
    }
}

// ─────────────────────────────────────────────
// Menu helpers
// ─────────────────────────────────────────────
static std::string normalize_path(std::string s) {
    // Strip surrounding quotes
    while (!s.empty() && (s.front() == '"' || s.front() == '\'')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == '"' || s.back()  == '\'')) s.pop_back();
    // Trim whitespace
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

static void print_history(Journal& jrnl,
                           const std::filesystem::path& root,
                           const std::filesystem::path* filter_path)
{
    auto events = filter_path
        ? jrnl.history(std::filesystem::path(*filter_path))
        : jrnl.history_under(root);

    if (events.empty()) {
        std::cout << "No journal events recorded yet.\n";
        return;
    }
    // Show last 50, oldest first
    size_t start = events.size() > 50 ? events.size() - 50 : 0;
    for (size_t i = start; i < events.size(); ++i) {
        const auto& e = events[i];
        std::cout << "#" << std::setfill('0') << std::setw(4) << e.id
                  << " " << e.timestamp
                  << " " << e.kind
                  << " pid=" << e.pid
                  << " " << e.path.string() << "\n";
    }
}

static void list_versions(Journal& jrnl) {
    std::cout << "Enter the full file path:\n";
    std::string line;
    std::getline(std::cin, line);
    line = normalize_path(line);
    if (line.empty()) return;

    std::filesystem::path p(line);
    auto vers = jrnl.versions(p);
    if (vers.empty()) {
        std::cout << "No saved versions for " << line << "\n";
        return;
    }
    std::cout << "Version ID is the number after #. "
                 "PID values in activity are not version IDs.\n";
    for (const auto& v : vers)
        std::cout << "#" << v.id << " " << v.created_at
                  << " " << v.size_bytes << " bytes\n";
}

static void rollback(Journal& jrnl) {
    std::cout << "Enter the full file path to roll back:\n";
    std::string line;
    std::getline(std::cin, line);
    line = normalize_path(line);
    if (line.empty()) return;

    std::filesystem::path target(line);
    auto vers = jrnl.versions(target);
    if (vers.empty()) {
        std::cout << "No saved versions for " << line
                  << ". Modify the file while the watcher is running first.\n";
        return;
    }
    std::cout << "Available versions (use the number after #):\n";
    for (const auto& v : vers)
        std::cout << "#" << v.id << " " << v.created_at
                  << " " << v.size_bytes << " bytes\n";

    std::cout << "Enter the saved version ID:\n";
    std::string id_str;
    std::getline(std::cin, id_str);
    id_str = normalize_path(id_str);
    int64_t id = 0;
    try { id = std::stoll(id_str); }
    catch (...) {
        std::cout << "Invalid version ID. Use a number such as 1 or 2.\n";
        return;
    }

    try {
        auto [stored_path, content] = jrnl.version_content(id);
        // Case-insensitive path comparison
        auto norm = [](const std::string& s) {
            std::string t = s;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            return t;
        };
        if (norm(stored_path.string()) != norm(target.string())) {
            std::cout << "That version belongs to a different file. "
                         "Choose an ID listed above.\n";
            return;
        }
        std::ofstream f(stored_path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(content.data()),
                static_cast<std::streamsize>(content.size()));
        std::cout << "Restored version #" << id << " to "
                  << stored_path.string() << "\n";
    } catch (const std::exception&) {
        std::cout << "Version #" << id << " was not found. "
                     "Choose an ID listed above.\n";
    }
}

// ─────────────────────────────────────────────
// Menu loop (main thread)
// ─────────────────────────────────────────────
static void menu_loop(Journal& jrnl, const std::filesystem::path& root) {
    while (true) {
        std::cout << "\nSafeVault menu\n"
                     "1. Show recent activity\n"
                     "2. Show file history\n"
                     "3. Search files\n"
                     "4. List saved versions\n"
                     "5. Roll back a file\n"
                     "6. Keep watching (refresh)\n"
                     "7. Stop watcher\n";

        std::string choice;
        std::getline(std::cin, choice);
        choice = normalize_path(choice);

        if (choice == "1") {
            print_history(jrnl, root, nullptr);
        } else if (choice == "2") {
            std::cout << "Enter a full file path, or press Enter for all files:\n";
            std::string path_str;
            std::getline(std::cin, path_str);
            path_str = normalize_path(path_str);
            if (path_str.empty()) {
                print_history(jrnl, root, nullptr);
            } else {
                std::filesystem::path fp(path_str);
                print_history(jrnl, root, &fp);
            }
        } else if (choice == "3") {
            std::cout << "Use the separate command: "
                         "safevault search \"your query\" --root \""
                      << root.string() << "\"\n";
        } else if (choice == "4") {
            list_versions(jrnl);
        } else if (choice == "5") {
            rollback(jrnl);
        } else if (choice == "6") {
            std::cout << "Watching...\n";
        } else if (choice == "7") {
            std::cout << "Watcher stopped.\n";
            return;
        } else {
            std::cout << "Choose an option from 1 to 7.\n";
        }
    }
}

// ─────────────────────────────────────────────
// Platform watcher implementations
// ─────────────────────────────────────────────

#ifdef _WIN32

// Translate Windows FILE_ACTION_* to EventKind
static std::optional<EventKind> win_action_to_kind(DWORD action) {
    switch (action) {
        case FILE_ACTION_ADDED:            return EventKind::Create;
        case FILE_ACTION_MODIFIED:         return EventKind::Modify;
        case FILE_ACTION_REMOVED:          return EventKind::Delete;
        case FILE_ACTION_RENAMED_NEW_NAME: return EventKind::Rename;
        default:                           return std::nullopt;
    }
}

static void platform_watch_thread(const std::filesystem::path& root,
                                   TsQueue<WatchEvent>& queue,
                                   std::atomic<bool>& stop_flag)
{
    // DWORD-aligned buffer required by FILE_NOTIFY_INFORMATION
    static const DWORD BUF_SIZE = 65536;
    alignas(DWORD) uint8_t buf[BUF_SIZE];

    HANDLE dir = CreateFileW(
        root.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (dir == INVALID_HANDLE_VALUE) {
        std::cerr << "[Watcher] Cannot open directory: " << root.string()
                  << "  error=" << GetLastError() << "\n";
        return;
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, /*manualReset=*/TRUE, /*initialState=*/FALSE, nullptr);
    if (!ov.hEvent) {
        CloseHandle(dir);
        std::cerr << "[Watcher] CreateEvent failed\n";
        return;
    }

    // ── Issue the FIRST read BEFORE entering the loop ────────────
    // With overlapped I/O, ReadDirectoryChangesW returns immediately.
    // ERROR_IO_PENDING is normal (not an error).
    auto issue_read = [&]() -> bool {
        ResetEvent(ov.hEvent);
        DWORD dummy = 0;
        BOOL ok = ReadDirectoryChangesW(
            dir,
            buf,
            BUF_SIZE,
            TRUE,   // watch subdirectories recursively
            FILE_NOTIFY_CHANGE_FILE_NAME  |
            FILE_NOTIFY_CHANGE_DIR_NAME   |
            FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_SIZE,
            &dummy,
            &ov,
            nullptr);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            std::cerr << "[Watcher] ReadDirectoryChangesW failed error=" << GetLastError() << "\n";
            return false;
        }
        return true;
    };

    if (!issue_read()) {
        CloseHandle(ov.hEvent);
        CloseHandle(dir);
        return;
    }

    // ── Main loop: wait → process → re-issue ─────────────────────
    while (!stop_flag.load()) {
        // Wait up to 500 ms so we can re-check stop_flag
        DWORD wait = WaitForSingleObject(ov.hEvent, 500);

        if (wait == WAIT_TIMEOUT)
            continue;   // no changes yet, loop and check stop_flag

        if (wait != WAIT_OBJECT_0)
            break;      // unexpected error

        DWORD bytes_returned = 0;
        if (!GetOverlappedResult(dir, &ov, &bytes_returned, FALSE))
            break;

        if (bytes_returned > 0) {
            const uint8_t* ptr = buf;
            while (true) {
                const auto* fni =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);

                auto kind_opt = win_action_to_kind(fni->Action);
                if (kind_opt) {
                    std::wstring wname(fni->FileName,
                                       fni->FileNameLength / sizeof(WCHAR));
                    queue.push({*kind_opt, root / wname});
                }

                if (fni->NextEntryOffset == 0) break;
                ptr += fni->NextEntryOffset;
            }
        }

        // Re-issue the read for the next batch of changes
        if (!issue_read()) break;
    }

    // Cancel any pending I/O before closing handles
    CancelIo(dir);
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
}

#else  // Linux inotify

// Simplified inotify watcher (non-recursive, single dir for prototype)
static void platform_watch_thread(const std::filesystem::path& root,
                                   TsQueue<WatchEvent>& queue,
                                   std::atomic<bool>& stop_flag)
{
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) throw std::runtime_error("inotify_init1 failed");

    int wd = inotify_add_watch(fd, root.string().c_str(),
                                IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO);
    if (wd < 0) { close(fd); throw std::runtime_error("inotify_add_watch failed"); }

    alignas(struct inotify_event) char buf[4096];
    while (!stop_flag.load()) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            break;
        }
        for (char* p = buf; p < buf + n; ) {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            if (ev->len > 0) {
                std::filesystem::path abs = root / ev->name;
                EventKind kind;
                bool valid = true;
                if      (ev->mask & IN_CREATE)   kind = EventKind::Create;
                else if (ev->mask & IN_MODIFY)   kind = EventKind::Modify;
                else if (ev->mask & IN_DELETE)   kind = EventKind::Delete;
                else if (ev->mask & IN_MOVED_TO) kind = EventKind::Rename;
                else valid = false;
                if (valid) queue.push({kind, abs});
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    inotify_rm_watch(fd, wd);
    close(fd);
}

#endif // _WIN32

// ─────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────
void watch(const std::filesystem::path& root,
           const std::filesystem::path& db_path)
{
    // Two separate Journal connections (mirrors Rust two-open pattern)
    Journal journal_bg(db_path);
    Journal journal_menu(db_path);

    TsQueue<WatchEvent> queue;
    SnapshotMap         snapshots  = initial_snapshots(root);
    std::mutex          snap_mtx;
    std::atomic<bool>   stop_flag{false};

    std::cout << "Watching " << root.string()
              << ". The menu is active; changes are journaled immediately.\n";

    // Background: OS watcher → queue
    std::thread watcher_thread([&]() {
        try { platform_watch_thread(root, queue, stop_flag); }
        catch (const std::exception& e) {
            std::cerr << "[Watcher] watcher thread error: " << e.what() << "\n";
        }
    });

    // Background: queue → SQLite
    std::thread journal_thread([&]() {
        journal_loop(queue, journal_bg, snapshots, snap_mtx);
    });

    // Foreground: interactive menu
    menu_loop(journal_menu, root);

    // Shutdown
    stop_flag.store(true);
    queue.stop();
    if (watcher_thread.joinable()) watcher_thread.join();
    if (journal_thread.joinable()) journal_thread.join();
}
