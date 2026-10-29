# 1. FileGuard Project Information

## Project Name

**FileGuard — Event-Driven File Management and Recovery System** (formerly SafeVault FS / ChronoFS) is an operating-system-level file management, journaled recovery, and performance benchmarking system. It records filesystem mutations, preserves pre-image file versions, supports deterministic undo/redo across system restarts, detects conflicting external modifications, executes transactional batch operations, and monitors real-time directory events using native OS facilities (`ReadDirectoryChangesW` on Windows, `inotify` on Linux).

## Problem Statement

Standard filesystems expose only the instantaneous state of a storage volume. If a user inadvertently deletes or overwrites a critical file, a script runs amok, or external processes alter directories unexpectedly, recovery requires manual intervention, shadow copy restoration, or remote backups.

FileGuard addresses this fundamental limitation by combining:
1. **Application-Level Write-Ahead Journaling**: Recording operation intent, pre-state, and recovery artifacts in SQLite before committing mutations to the filesystem.
2. **Persistent LIFO Undo/Redo Engine**: Allowing users to roll back or re-apply mutations safely across system restarts, with strict redo-branch invalidation upon new operations.
3. **Conflict Detection**: Verifying expected pre-hashes and modification timestamps before reversal to avoid silently overwriting external changes.
4. **Quarantine-Based Deletion**: Moving deleted files and directories into a dedicated, managed quarantine vault rather than unlinking them permanently.
5. **Native OS Event Monitoring**: Utilizing kernel notifications (`ReadDirectoryChangesW`) to stream live filesystem changes to a React UI via Server-Sent Events (SSE).
6. **OS Performance Laboratory**: Running reproducible micro-benchmarks comparing sequential vs. concurrent multi-threaded directory scans, event detection latency vs. periodic polling, and write-ahead journaling overhead.

## Architecture

```text
React + TypeScript Desktop UI (Port 5173)
        │
        │ HTTP REST & Server-Sent Events (SSE)
        ▼
C++17/20 Local Application Server (Port 8787)
        │
        ├── FilesystemService (Canonical sandboxing, approved roots, directory traversal)
        ├── CommandManager (Concrete commands: create, rename, move, copy, delete, overwrite)
        ├── OperationJournal (SQLite WAL mode, schema migrations, LIFO undo/redo stack)
        ├── RecoveryService (Managed quarantine, version snapshots, SHA-256 integrity verification)
        ├── EventMonitor (Native ReadDirectoryChangesW, event deduplication, SSE broadcast)
        └── BenchmarkService (Reproducible OS lab experiments & CSV export)
        │
        ▼
Local Host Operating System Filesystem
```

## Implementation History & Evolution

- **Prototype Stage**: Initial exploratory prototype implemented in Python (`watchdog`) and Rust (`safevault/`).
- **Production Engine**: Authoritative implementation in modern C++ (`safevault_cpp/`) leveraging `std::filesystem`, SQLite WAL, `httplib`, and `nlohmann/json`. The C++ engine serves as the single source of truth for all filesystem mutations, recovery, and monitoring.
- **Frontend**: High-density desktop interface built with React 18, TypeScript, Vite, and Lucide icons, styled in a restrained dark graphite desktop palette.
