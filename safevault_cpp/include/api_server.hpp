#pragma once
#include "journal.hpp"
#include "filesystem_service.hpp"
#include "recovery_service.hpp"
#include "command_manager.hpp"
#include "event_monitor.hpp"
#include "benchmark_service.hpp"

#include <string>
#include <memory>

namespace fileguard {

class ApiServer {
public:
    ApiServer(Journal& journal,
              FilesystemService& fs_service,
              RecoveryService& recovery_service,
              CommandManager& command_mgr,
              EventMonitor& event_monitor,
              BenchmarkService& benchmark_service);
    ~ApiServer();

    void listen(const std::string& host, int port);
    void stop();

private:
    Journal&           journal_;
    FilesystemService& fs_service_;
    RecoveryService&   recovery_;
    CommandManager&    command_mgr_;
    EventMonitor&      event_monitor_;
    BenchmarkService&  benchmark_service_;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fileguard
