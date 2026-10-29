#pragma once
#include "journal.hpp"
#include "filesystem_service.hpp"
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <chrono>

namespace fileguard {

struct WatcherVsPollingResult {
    std::string experiment{"WatcherVsPolling"};
    int32_t     test_file_count{0};
    double      polling_interval_ms{100.0};
    double      polling_elapsed_ms{0.0};
    int32_t     polling_scans{0};
    double      polling_min_latency_ms{0.0};
    double      polling_median_latency_ms{0.0};
    double      polling_p95_latency_ms{0.0};
    double      polling_avg_latency_ms{0.0};
    double      watcher_elapsed_ms{0.0};
    int32_t     watcher_events_received{0};
    double      watcher_min_latency_ms{0.0};
    double      watcher_median_latency_ms{0.0};
    double      watcher_p95_latency_ms{0.0};
    double      watcher_avg_latency_ms{0.0};
    double      latency_speedup{0.0};
    std::string cpu_io_summary;
    std::string timestamp;
};

struct ScanBenchmarkResult {
    std::string experiment{"ScanSequentialVsConcurrent"};
    int32_t     total_files_generated{0};
    int32_t     depth{3};
    // Sequential
    double      seq_elapsed_ms{0.0};
    uint64_t    seq_files_found{0};
    uint64_t    seq_bytes_found{0};
    double      seq_throughput_files_per_sec{0.0};
    // Concurrent
    double      conc_elapsed_ms{0.0};
    uint64_t    conc_files_found{0};
    uint64_t    conc_bytes_found{0};
    double      conc_throughput_files_per_sec{0.0};
    int32_t     threads_used{4};
    bool        correctness_verified{false};
    double      speedup{0.0};
    std::string timestamp;
};

struct JournalOverheadResult {
    std::string experiment{"JournalOverhead"};
    int32_t     ops_count{100};
    double      raw_fs_elapsed_ms{0.0};
    double      raw_throughput_ops_per_sec{0.0};
    double      journaled_elapsed_ms{0.0};
    double      journaled_throughput_ops_per_sec{0.0};
    double      overhead_percentage{0.0};
    double      recovery_time_ms{0.0};
    int32_t     recovered_ops{0};
    std::string timestamp;
};

class BenchmarkService {
public:
    BenchmarkService(Journal& journal, FilesystemService& fs_service, std::filesystem::path temp_bench_dir);
    ~BenchmarkService();

    // Experiments
    WatcherVsPollingResult run_watcher_vs_polling_bench(int file_count = 30, double polling_interval_ms = 100.0);
    ScanBenchmarkResult    run_scan_benchmark(int file_count = 1000, int threads = 4);
    JournalOverheadResult  run_journal_overhead_bench(int ops_count = 100);

    // Results history
    std::vector<BenchmarkRecord> get_history(size_t limit = 50) const;
    std::string export_csv() const;

private:
    Journal&           journal_;
    FilesystemService& fs_service_;
    std::filesystem::path temp_bench_dir_;

    void setup_temp_dir();
    void cleanup_temp_dir();
    std::string generate_bench_id(const std::string& type);
};

} // namespace fileguard
