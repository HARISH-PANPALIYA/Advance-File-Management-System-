#include "event_monitor.hpp"
#include "sha256.hpp"

#include <iostream>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/inotify.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <poll.h>
#endif

namespace fileguard {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

EventMonitor::EventMonitor(Journal& journal, FilesystemService& fs_service)
    : journal_(journal), fs_service_(fs_service) {
}

EventMonitor::~EventMonitor() {
    stop();
}

bool EventMonitor::start(const std::filesystem::path& dir_to_watch) {
    stop();

    auto valid_dir = fs_service_.canonicalize_and_validate(dir_to_watch);
    std::error_code ec;
    if (!std::filesystem::is_directory(valid_dir, ec)) {
        throw std::runtime_error("Watched path is not a directory: " + valid_dir.string());
    }

    watched_dir_ = valid_dir;
    is_running_.store(true);
    events_captured_.store(0);
    overflow_count_.store(0);

    worker_thread_ = std::thread(&EventMonitor::run_watcher_loop, this);
    return true;
}

void EventMonitor::stop() {
    if (is_running_.load()) {
        is_running_.store(false);
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}

WatcherStatus EventMonitor::get_status() const {
    WatcherStatus st;
    st.is_active = is_running_.load();
    st.watched_path = watched_dir_.string();
#ifdef _WIN32
    st.backend_type = "Windows ReadDirectoryChangesW (Async Overlapped)";
#else
    st.backend_type = "Linux inotify";
#endif
    st.events_captured = events_captured_.load();
    st.overflow_count = overflow_count_.load();
    st.last_event_time = last_event_time_;
    return st;
}

std::vector<EventRecord> EventMonitor::get_recent_events(size_t limit) const {
    return journal_.history(std::nullopt, limit);
}

int EventMonitor::add_listener(EventCallback cb) {
    std::lock_guard<std::mutex> lk(listeners_mtx_);
    int id = next_listener_id_++;
    listeners_[id] = std::move(cb);
    return id;
}

void EventMonitor::remove_listener(int listener_id) {
    std::lock_guard<std::mutex> lk(listeners_mtx_);
    listeners_.erase(listener_id);
}

std::string EventMonitor::normalize_path_key(const std::filesystem::path& p) {
    if (p.empty()) return "";
    std::error_code ec;
    auto cp = std::filesystem::weakly_canonical(p, ec);
    std::string s = (ec ? p : cp).string();
    std::replace(s.begin(), s.end(), '/', '\\');
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
#endif
    while (s.size() > 3 && s.back() == '\\') {
        s.pop_back();
    }
    return s;
}

void EventMonitor::notify_app_mutation(const std::filesystem::path& p) {
    register_expected_mutation(p, "", 4500);
}

void EventMonitor::register_expected_mutation(const std::filesystem::path& p, const std::filesystem::path& old_p, int64_t ttl_ms) {
    std::string norm_p = normalize_path_key(p);
    std::string norm_old = old_p.empty() ? "" : normalize_path_key(old_p);
    int64_t expires = now_ms() + ttl_ms;

    std::lock_guard<std::mutex> lk(coalesce_mtx_);
    int64_t current = now_ms();
    expected_mutations_.erase(
        std::remove_if(expected_mutations_.begin(), expected_mutations_.end(),
                       [current](const ExpectedMutation& m) { return m.expires_at_ms < current; }),
        expected_mutations_.end());

    expected_mutations_.push_back({norm_p, norm_old, expires});
}

bool EventMonitor::is_app_mutation(const std::filesystem::path& p, const std::optional<std::filesystem::path>& old_p) {
    std::string norm_p = normalize_path_key(p);
    std::string norm_old = old_p.has_value() ? normalize_path_key(*old_p) : "";
    int64_t current = now_ms();

    std::lock_guard<std::mutex> lk(coalesce_mtx_);
    for (auto it = expected_mutations_.begin(); it != expected_mutations_.end(); ) {
        if (it->expires_at_ms < current) {
            it = expected_mutations_.erase(it);
            continue;
        }
        bool match = false;
        if (!norm_p.empty() && it->normalized_path == norm_p) {
            match = true;
        }
        if (!norm_old.empty() && it->normalized_old_path == norm_old) {
            match = true;
        }
        if (!norm_p.empty() && it->normalized_old_path == norm_p) {
            match = true;
        }
        if (!norm_old.empty() && it->normalized_path == norm_old) {
            match = true;
        }
        if (match) {
            return true;
        }
        ++it;
    }
    return false;
}

void EventMonitor::dispatch_event(EventRecord ev) {
    std::string key = ev.path.string() + "_" + ev.kind;
    int64_t current = now_ms();

    // Event coalescing for rapid consecutive events (e.g. modify loop)
    {
        std::lock_guard<std::mutex> lk(coalesce_mtx_);
        auto it = last_event_times_.find(key);
        if (it != last_event_times_.end()) {
            if (current - it->second < 200) {
                // Coalesce / deduplicate
                return;
            }
        }
        last_event_times_[key] = current;
    }

    ev.is_external = !is_app_mutation(ev.path, ev.old_path);
    events_captured_++;
    last_event_time_ = ev.timestamp;

    // If external modification or creation or rename of a file, create ExternalCandidateRecord
    if (ev.is_external) {
        std::error_code ec;
        if (ev.kind == "Rename" || (std::filesystem::exists(ev.path, ec) && std::filesystem::is_regular_file(ev.path, ec))) {
            try {
                FileIdentityRecord fid;
                if (ev.kind == "Rename" && ev.old_path.has_value() && !ev.old_path->empty()) {
                    auto fid_opt = journal_.find_file_identity_by_path(ev.old_path->string());
                    if (fid_opt) {
                        fid = *fid_opt;
                        journal_.update_file_identity_path(fid.id, ev.path.string());
                    } else {
                        fid = journal_.get_or_create_file_identity(ev.path.string());
                    }
                } else {
                    fid = journal_.get_or_create_file_identity(ev.path.string());
                }

                std::string h = std::filesystem::exists(ev.path, ec) ? sha256_file(ev.path) : "";
                uint64_t sz = std::filesystem::exists(ev.path, ec) ? std::filesystem::file_size(ev.path, ec) : 0;
                if (ec) sz = 0;

                auto prior_revs = journal_.get_content_revisions(fid.id);
                std::string prior_h = prior_revs.empty() ? "" : prior_revs.front().sha256_hash;
                int64_t prior_snap_id = prior_revs.empty() ? 0 : prior_revs.front().snapshot_id;

                std::filesystem::path cand_snap_path;
                if (std::filesystem::exists(ev.path, ec) && !h.empty()) {
                    std::filesystem::path snap_dir = std::filesystem::current_path() / "storage" / "snapshots";
                    std::filesystem::create_directories(snap_dir, ec);
                    std::string snap_file_name = "cand_" + h.substr(0, 16) + "_" + std::to_string(now_ms()) + ".dat";
                    cand_snap_path = snap_dir / snap_file_name;
                    std::filesystem::copy_file(ev.path, cand_snap_path, std::filesystem::copy_options::overwrite_existing, ec);
                }

                if (ev.kind == "Rename") {
                    ExternalCandidateRecord cand;
                    cand.id = journal_.generate_sequential_id("X");
                    cand.detection_seq = journal_.get_next_external_seq();
                    cand.path = ev.path.string();
                    std::replace(cand.path.begin(), cand.path.end(), '\\', '/');
                    cand.old_path = ev.old_path.has_value() ? ev.old_path->string() : "";
                    std::replace(cand.old_path.begin(), cand.old_path.end(), '\\', '/');
                    cand.file_identity_id = fid.id;
                    cand.change_type = "EXTERNAL_RENAME";
                    cand.detected_at = ev.timestamp;
                    cand.status = "PENDING";
                    cand.candidate_snapshot_path = cand_snap_path.string();
                    std::replace(cand.candidate_snapshot_path.begin(), cand.candidate_snapshot_path.end(), '\\', '/');
                    cand.candidate_hash = h;
                    cand.candidate_size = sz;
                    cand.prior_accepted_hash = prior_h;
                    cand.prior_accepted_snapshot_id = prior_snap_id;
                    cand.resolution_notes = "External rename detected from " + cand.old_path + " to " + cand.path;
                    journal_.record_external_candidate(cand);

                    NameLocationRecord nl;
                    nl.id = journal_.generate_sequential_id("OP");
                    nl.file_identity_id = fid.id;
                    nl.seq = 0;
                    nl.op_type = "EXTERNAL_RENAME";
                    nl.source_path = cand.old_path;
                    nl.dest_path = cand.path;
                    nl.created_at = ev.timestamp;
                    journal_.record_name_location(nl);
                } else if (h != prior_h && !h.empty()) {
                    ExternalCandidateRecord cand;
                    cand.id = journal_.generate_sequential_id("X");
                    cand.detection_seq = journal_.get_next_external_seq();
                    cand.path = ev.path.string();
                    std::replace(cand.path.begin(), cand.path.end(), '\\', '/');
                    cand.old_path = "";
                    cand.file_identity_id = fid.id;
                    cand.change_type = (ev.kind == "CREATE" || ev.kind == "Create") ? "EXTERNAL_CREATE" : "EXTERNAL_MODIFY";
                    cand.detected_at = ev.timestamp;
                    cand.status = "PENDING";
                    cand.candidate_snapshot_path = cand_snap_path.string();
                    std::replace(cand.candidate_snapshot_path.begin(), cand.candidate_snapshot_path.end(), '\\', '/');
                    cand.candidate_hash = h;
                    cand.candidate_size = sz;
                    cand.prior_accepted_hash = prior_h;
                    cand.prior_accepted_snapshot_id = prior_snap_id;
                    cand.resolution_notes = "Detected external modification outside FileGuard";
                    journal_.record_external_candidate(cand);
                }
            } catch (const std::exception& e) {
                std::cerr << "[EventMonitor] Error recording external candidate: " << e.what() << "\n";
            }
        }
    }

    // Persist in journal SQLite
    try {
        ev.id = journal_.append_event(ev);
    } catch (const std::exception& e) {
        std::cerr << "[EventMonitor] Error saving event: " << e.what() << "\n";
    }

    // Broadcast to SSE listeners
    std::vector<EventCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(listeners_mtx_);
        for (const auto& pair : listeners_) {
            callbacks.push_back(pair.second);
        }
    }

    for (const auto& cb : callbacks) {
        try {
            cb(ev);
        } catch (...) {}
    }
}

#ifdef _WIN32

void EventMonitor::run_watcher_loop() {
    static const DWORD BUF_SIZE = 65536;
    alignas(DWORD) uint8_t buf[BUF_SIZE];

    HANDLE dir = CreateFileW(
        watched_dir_.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (dir == INVALID_HANDLE_VALUE) {
        std::cerr << "[EventMonitor] Failed to open directory for watching: " << watched_dir_.string() << "\n";
        is_running_.store(false);
        return;
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        CloseHandle(dir);
        is_running_.store(false);
        return;
    }

    auto issue_read = [&]() -> bool {
        ResetEvent(ov.hEvent);
        DWORD dummy = 0;
        BOOL ok = ReadDirectoryChangesW(
            dir,
            buf,
            BUF_SIZE,
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME  |
            FILE_NOTIFY_CHANGE_DIR_NAME   |
            FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_SIZE,
            &dummy,
            &ov,
            nullptr);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        return true;
    };

    if (!issue_read()) {
        CloseHandle(ov.hEvent);
        CloseHandle(dir);
        is_running_.store(false);
        return;
    }

    std::optional<std::filesystem::path> pending_old_rename;

    while (is_running_.load()) {
        DWORD wait_res = WaitForSingleObject(ov.hEvent, 200);
        if (wait_res == WAIT_TIMEOUT) {
            continue;
        }

        if (wait_res != WAIT_OBJECT_0) {
            break;
        }

        DWORD bytes_ret = 0;
        if (!GetOverlappedResult(dir, &ov, &bytes_ret, FALSE)) {
            if (GetLastError() == ERROR_NOTIFY_ENUM_DIR) {
                overflow_count_++;
            }
            break;
        }

        if (bytes_ret == 0) {
            overflow_count_++;
        } else {
            const uint8_t* ptr = buf;
            while (true) {
                const auto* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
                std::wstring rel_name(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
                std::filesystem::path full_p = watched_dir_ / rel_name;

                switch (fni->Action) {
                    case FILE_ACTION_ADDED:
                        dispatch_event(EventRecord::now(EventKind::Create, full_p));
                        break;
                    case FILE_ACTION_REMOVED:
                        dispatch_event(EventRecord::now(EventKind::Delete, full_p));
                        break;
                    case FILE_ACTION_MODIFIED:
                        dispatch_event(EventRecord::now(EventKind::Modify, full_p));
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        pending_old_rename = full_p;
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        dispatch_event(EventRecord::now(EventKind::Rename, full_p, pending_old_rename));
                        pending_old_rename.reset();
                        break;
                    default:
                        break;
                }

                if (fni->NextEntryOffset == 0) break;
                ptr += fni->NextEntryOffset;
            }
        }

        if (!issue_read()) break;
    }

    CancelIo(dir);
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
    is_running_.store(false);
}

#else

void EventMonitor::run_watcher_loop() {
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) {
        is_running_.store(false);
        return;
    }

    int wd = inotify_add_watch(fd, watched_dir_.string().c_str(),
                                IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (wd < 0) {
        close(fd);
        is_running_.store(false);
        return;
    }

    std::optional<std::filesystem::path> pending_old_rename;
    alignas(struct inotify_event) char buf[4096];

    while (is_running_.load()) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int ret = poll(&pfd, 1, 200);
        if (ret <= 0) continue;

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) continue;

        for (char* p = buf; p < buf + n; ) {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            if (ev->len > 0) {
                std::filesystem::path full_p = watched_dir_ / ev->name;
                if (ev->mask & IN_CREATE) {
                    dispatch_event(EventRecord::now(EventKind::Create, full_p));
                } else if (ev->mask & IN_MODIFY) {
                    dispatch_event(EventRecord::now(EventKind::Modify, full_p));
                } else if (ev->mask & IN_DELETE) {
                    dispatch_event(EventRecord::now(EventKind::Delete, full_p));
                } else if (ev->mask & IN_MOVED_FROM) {
                    pending_old_rename = full_p;
                } else if (ev->mask & IN_MOVED_TO) {
                    dispatch_event(EventRecord::now(EventKind::Rename, full_p, pending_old_rename));
                    pending_old_rename.reset();
                }
            }
            p += sizeof(struct inotify_event) + ev->len;
        }
    }

    inotify_rm_watch(fd, wd);
    close(fd);
    is_running_.store(false);
}

#endif

} // namespace fileguard
