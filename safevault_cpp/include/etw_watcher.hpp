#pragma once

#include "watch.hpp"
#include <atomic>
#include <thread>

#ifdef _WIN32
// Starts an ETW session that watches the kernel‑file provider and pushes
// filesystem events into the supplied TsQueue. The function blocks until
// `stop_flag` becomes true.
void start_etw_watcher(const std::filesystem::path& root,
                       TsQueue<WatchEvent>& queue,
                       std::atomic<bool>& stop_flag);
#endif // _WIN32
