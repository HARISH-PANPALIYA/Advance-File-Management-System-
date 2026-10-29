#pragma once
#include "journal.hpp"
#include "filesystem_service.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <unordered_map>

namespace fileguard {

struct WatcherStatus {
    bool        is_active{false};
    std::string watched_path;
    std::string backend_type; // "ReadDirectoryChangesW" on Windows, "inotify" on Linux
    uint64_t    events_captured{0};
    uint64_t    overflow_count{0};
    std::string last_event_time;
};

class EventMonitor {
public:
    EventMonitor(Journal& journal, FilesystemService& fs_service);
    ~EventMonitor();

    // Start / Stop monitoring
    bool start(const std::filesystem::path& dir_to_watch);
    void stop();
    WatcherStatus get_status() const;

    // Event retrieval
    std::vector<EventRecord> get_recent_events(size_t limit = 100) const;

    // Listeners for SSE streaming
    using EventCallback = std::function<void(const EventRecord&)>;
    int add_listener(EventCallback cb);
    void remove_listener(int listener_id);

    // Notify of application-initiated action to suppress duplicate external noise
    void notify_app_mutation(const std::filesystem::path& p);
    void register_expected_mutation(const std::filesystem::path& p, const std::filesystem::path& old_p = "", int64_t ttl_ms = 4000);

    // Static path normalization helper for consistent cross-subsystem keys
    static std::string normalize_path_key(const std::filesystem::path& p);

private:
    Journal&           journal_;
    FilesystemService& fs_service_;

    std::filesystem::path watched_dir_;
    std::atomic<bool>     is_running_{false};
    std::atomic<uint64_t> events_captured_{0};
    std::atomic<uint64_t> overflow_count_{0};
    std::string           last_event_time_;

    std::thread           worker_thread_;

    mutable std::mutex    listeners_mtx_;
    std::unordered_map<int, EventCallback> listeners_;
    int next_listener_id_{1};

    // Coalescing and app-mutation tracking
    struct ExpectedMutation {
        std::string normalized_path;
        std::string normalized_old_path;
        int64_t     expires_at_ms{0};
    };

    mutable std::mutex              coalesce_mtx_;
    std::unordered_map<std::string, int64_t> last_event_times_;
    std::vector<ExpectedMutation>   expected_mutations_;

    void run_watcher_loop();
    void dispatch_event(EventRecord ev);
    bool is_app_mutation(const std::filesystem::path& p, const std::optional<std::filesystem::path>& old_p = std::nullopt);
};

} // namespace fileguard
