# FileGuard: Operating System Concepts & Implementation Architecture

This document provides a comprehensive analysis of all **Operating System (OS) concepts** implemented within **FileGuard**, detailing the theoretical principles and their exact locations (files, classes, and functions) in the C++20 and TypeScript codebase.

---

## Table of Contents

1. [File Systems, Inodes & Stable Identity Resolution](#1-file-systems-inodes--stable-identity-resolution)
2. [Kernel Event Monitoring & Asynchronous Notification](#2-kernel-event-monitoring--asynchronous-notification)
3. [Write-Ahead Logging (WAL) & Crash Consistency](#3-write-ahead-logging-wal--crash-consistency)
4. [Atomic Transactions & Compensating Rollback](#4-atomic-transactions--compensating-rollback)
5. [Concurrency, Multithreading & Synchronization](#5-concurrency-multithreading--synchronization)
6. [Snapshots, Versioning & Content Hashing (Content-Addressable Storage)](#6-snapshots-versioning--content-hashing-content-addressable-storage)
7. [Quarantine Isolation, Security & Path Sanitization (Jail Containment)](#7-quarantine-isolation-security--path-sanitization-jail-containment)
8. [Cross-Platform OS Abstraction (Win32 Native vs POSIX/WSL2)](#8-cross-platform-os-abstraction-win32-native-vs-posixwsl2)
9. [Performance Benchmarking & OS Latency Measurement](#9-performance-benchmarking--os-latency-measurement)

---

## 1. File Systems, Inodes & Stable Identity Resolution

### Theoretical Concept
In traditional UNIX and POSIX filesystems, a file's identity is decoupled from its directory entry (dentry) via an **inode** number. When a file is renamed or moved across directories on the same filesystem, its inode number remains constant, preserving file metadata, open file descriptors, and block references. On modern Windows NTFS and FAT32 filesystems, paths are mutable strings subject to case-insensitivity and path separator differences (`/` vs `\`).

### Implementation in FileGuard
FileGuard introduces an OS-level **File Identity Abstraction** (`file_identities` table in SQLite) that assigns a durable, unique `file_identity_id` (e.g., `FID-0001`) to every tracked file. This mirrors the OS inode table.

- **Location:** [`safevault_cpp/include/journal.hpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/include/journal.hpp) & [`safevault_cpp/src/journal.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/journal.cpp)
- **Functions:**
  - `Journal::get_or_create_file_identity(path)`: Checks or generates a durable `FID` record with `initial_path` and `current_path`.
  - `Journal::find_file_identity_by_path(path)`: Implements a **5-tier resolution hierarchy**:
    - *Tier 1:* Exact & case-insensitive match on normalized `current_path`.
    - *Tier 2:* Match on `initial_path`.
    - *Tier 3:* Historical lookup in `name_location_history` (`source_path` or `dest_path`).
    - *Tier 4:* Historical lookup in `content_revisions` table.
    - *Tier 5:* Canonical relative-to-absolute path resolution and path suffix matching.
  - `Journal::update_file_identity_path(fid, new_path)`: Atomically updates `current_path` when a file is renamed or moved.
  - `Journal::record_name_location(nl)`: Appends an entry to the immutable `name_location_history` log.

---

## 2. Kernel Event Monitoring & Asynchronous Notification

### Theoretical Concept
Operating system kernels monitor filesystem changes via interrupt-driven or buffer-driven event queues (such as `ReadDirectoryChangesW` and ETW on Windows, or `inotify` / `fanotify` on Linux). When a user space or external process modifies a file, the OS kernel notifies registered listeners asynchronously.

### Implementation in FileGuard
FileGuard implements both **Win32 Directory Change Notifications** and **Event Tracing for Windows (ETW)** to capture filesystem mutations in real time.

- **Location:** [`safevault_cpp/src/watch.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/watch.cpp), [`safevault_cpp/src/etw_watcher.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/etw_watcher.cpp), and [`safevault_cpp/src/event_monitor.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/event_monitor.cpp)
- **Functions & Architecture:**
  - `WatchSession::run()`: Uses `ReadDirectoryChangesW` with `FILE_NOTIFY_CHANGE_FILE_NAME`, `FILE_NOTIFY_CHANGE_DIR_NAME`, `FILE_NOTIFY_CHANGE_SIZE`, and `FILE_NOTIFY_CHANGE_LAST_WRITE`.
  - `EventMonitor::dispatch_event(ev)`:
    - **Self-Mutation Suppression:** Checks internal mutation timestamps recorded by `notify_mutation()` so app-initiated edits are not misclassified as external modifications.
    - **External Candidate Detection:** When an unexpected change occurs, hashes the file (`sha256_file`) and records an `ExternalCandidateRecord` in status `PENDING`.
    - **Identity Re-link on External Rename:** When the kernel reports a rename (`ev.kind == "Rename"`), FileGuard resolves `ev.old_path` to the existing `FID`, keeping all previous revisions attached to the file.

---

## 3. Write-Ahead Logging (WAL) & Crash Consistency

### Theoretical Concept
Crash consistency is a core OS filesystem principle (used in ext4, NTFS, XFS, and ZFS). Before any metadata or block pointer is updated on disk, an intent record must be written to an append-only log (Write-Ahead Logging). If power is lost or the system crashes mid-operation, the journal is replayed during recovery to either complete (roll-forward) or undo (roll-back) the interrupted state.

### Implementation in FileGuard
FileGuard uses **SQLite in WAL mode** (`PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;`) combined with an explicit application-level two-phase intent journal.

- **Location:** [`safevault_cpp/src/journal.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/journal.cpp) and [`safevault_cpp/src/command_manager.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/command_manager.cpp)
- **Functions:**
  - `Journal::init_schema()`: Configures WAL mode, enables foreign keys, and initializes tables: `operations`, `transactions`, `file_identities`, `content_revisions`, `name_location_history`, `external_candidates`, and `quarantine_entries`.
  - `Journal::log_operation_intent(op)`: Logs state `INTENT` before filesystem mutation occurs.
  - `Journal::update_operation_execution(op_id, ...)`: Updates status to `EXECUTED` once the disk write succeeds.
  - `CommandManager::recover_interrupted_operations()`: Executed during startup. Scans the operations table for any operations left in `INTENT` status, inspects the physical filesystem to verify whether the disk write completed, and transitions the record to `EXECUTED` or `FAILED`.

---

## 4. Atomic Transactions & Compensating Rollback

### Theoretical Concept
Operating systems and database engines group multiple interdependent file operations into an atomic transaction (ACID properties: Atomicity, Consistency, Isolation, Durability). If any step in a multi-file batch fails (e.g., disk full, permission denied, or destination collision), the system must execute **compensating actions** to reverse all previously completed steps, returning the storage state to its exact pre-transaction baseline.

### Implementation in FileGuard
FileGuard implements full multi-file atomic transactions for **Batch Delete (Quarantine)**, **Batch Move**, **Batch Rename**, and **Batch Copy**.

- **Location:** [`safevault_cpp/src/command_manager.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/command_manager.cpp#L1119-L1425)
- **Functions:**
  - `CommandManager::execute_batch_delete(paths)`: Groups file deletions under a single transaction (`tx_del_...`). If file $k$ fails to delete, all items $0 \dots k-1$ are automatically restored from quarantine.
  - `CommandManager::execute_batch_move(items)`: Atomically moves multiple files across directories. On failure, rolls back completed moves.
  - `CommandManager::execute_batch_copy(items)`: Copies files under transaction tracking.
  - `CommandManager::undo_transaction(tx_id)`: Rolls back a completed transaction by iterating through all child operations in reverse chronological order (`seq DESC`), reversing moves, re-creating parent directories, and restoring quarantined files.
  - `CommandManager::redo_transaction(tx_id)`: Reapplies a rolled-back transaction in forward sequence order (`seq ASC`).
  - **Independent Undo Stacks:**
    - `undo_content()` / `redo_content()`: Modifies file bytes at the file's current location (`fid.current_path`) without altering filenames or paths.
    - `undo_name()` / `redo_name()`: Reverses renames and directory moves without failing on content hash diffs and without altering file bytes.

---

## 5. Concurrency, Multithreading & Synchronization

### Theoretical Concept
Operating systems coordinate parallel execution streams via thread scheduling, mutual exclusion locks (`mutex`), condition variables, and thread-safe queues to prevent race conditions and priority inversions.

### Implementation in FileGuard
- **Location:** [`safevault_cpp/include/api_server.hpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/include/api_server.hpp), [`safevault_cpp/src/event_monitor.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/event_monitor.cpp), and [`safevault_cpp/src/benchmark_service.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/benchmark_service.cpp)
- **Mechanisms:**
  - **Multi-threaded HTTP Server:** `httplib::Server` spawns thread pool workers to handle concurrent REST API requests.
  - **Mutex Protected State:** `std::mutex state_mutex_` protects journal mutations, health status aggregation, and benchmark runs.
  - **Atomic Flags:** `std::atomic<bool> is_running_` coordinates graceful shutdown between the API server, watcher threads, and background benchmarks.
  - **Event Queue Synchronization:** Background directory watcher threads push kernel notifications into a synchronized event queue consumed by `EventMonitor`.

---

## 6. Snapshots, Versioning & Content Hashing (Content-Addressable Storage)

### Theoretical Concept
Modern filesystems (e.g., ZFS, Btrfs, Apple APFS) support copy-on-write snapshots and content versioning. By hashing file contents (SHA-256), the storage layer detects identical files (deduplication) and verifies data integrity against bit rot or malicious tampering.

### Implementation in FileGuard
- **Location:** [`safevault_cpp/src/sha256.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/sha256.cpp), [`safevault_cpp/src/recovery_service.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/recovery_service.cpp), and [`safevault_cpp/src/journal.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/journal.cpp)
- **Functions:**
  - `sha256_file(path)`: Computes the 256-bit SHA-256 cryptographic digest of any file via streaming 64KB block reads.
  - `RecoveryService::create_snapshot(path, op_id)`: Copies the file to an immutable versioned snapshot directory (`storage/snapshots/`) stamped with `snapshot_id`, UTC timestamp, and SHA-256 hash.
  - `RecoveryService::restore_snapshot_to(version_id, target, overwrite)`: Restores a specific version snapshot after validating cryptographic integrity.
  - `Journal::get_duplicate_files(root)`: Groups tracked files by SHA-256 hash to identify identical files across directories with zero false positives.

---

## 7. Quarantine Isolation, Security & Path Sanitization (Jail Containment)

### Theoretical Concept
Operating system security frameworks prevent path traversal attacks (e.g., directory traversal via `../` or symlink redirection) by canonicalizing all requested paths and strictly enforcing sandbox jail boundaries. Furthermore, safe deletion policies isolate discarded data in a quarantine space rather than performing immediate destructive unlinking.

### Implementation in FileGuard
- **Location:** [`safevault_cpp/src/filesystem_service.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/filesystem_service.cpp) and [`safevault_cpp/src/recovery_service.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/recovery_service.cpp)
- **Functions:**
  - `FilesystemService::canonicalize_and_validate(p)`: Resolves symlinks, relative path segments (`.`, `..`), and normalizes separators. Verifies that the resolved path is located strictly within an approved managed root directory. Throws `std::runtime_error` on unauthorized access.
  - `RecoveryService::quarantine_item(path, op_id)`: Safely moves deleted or suspicious files into an isolated quarantine vault (`storage/quarantine/`), logging original paths, permissions, and hash in `quarantine_entries`.
  - `RecoveryService::restore_quarantined_item(qid, ...)`: Re-creates original parent directories and copies the quarantined artifact back to its origin.

---

## 8. Cross-Platform OS Abstraction (Win32 Native vs POSIX/WSL2)

### Theoretical Concept
Operating systems provide distinct system call interfaces (`Win32 API` on Windows vs `POSIX / libc` on Linux). Portable systems software implements an abstraction layer isolating platform-specific system calls behind unified interfaces.

### Implementation in FileGuard
FileGuard is designed to run natively on **Windows 11/10** (via Win32 APIs, MinGW-w64, MSVC) and seamlessly on **Linux / WSL2** (POSIX, glibc).

- **Location:** [`run_all.ps1`](file:///d:/EventDrivenFileSystem/run_all.ps1), [`safevault_cpp/CMakeLists.txt`](file:///d:/EventDrivenFileSystem/safevault_cpp/CMakeLists.txt), and [`safevault_cpp/src/main.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/main.cpp)
- **Mechanisms:**
  - `run_all.ps1`: Detects Windows 11 Smart App Control (SAC) or Code Integrity enforcement; if unsigned local native binaries are blocked, it automatically switches execution to WSL2 without requiring a reboot or registry tampering.
  - Standardized C++20 `std::filesystem` used across both platforms for directory iterators, file permissions, file sizes, and renames.

---

## 9. Performance Benchmarking & OS Latency Measurement

### Theoretical Concept
OS performance analysis measures kernel system call latency percentiles ($p_{50}$, $p_{95}$, $p_{99}$) and I/O throughput (ops/sec).

### Implementation in FileGuard
FileGuard includes an integrated **OS Performance Laboratory** that runs real filesystem operations to measure disk and kernel performance:

- **Location:** [`safevault_cpp/src/benchmark_service.cpp`](file:///d:/EventDrivenFileSystem/safevault_cpp/src/benchmark_service.cpp)
- **Experiments:**
  - **Experiment A (Latency Percentiles):** Measures direct raw filesystem write latency vs journaled write latency across 100 samples, calculating median ($p_{50}$), $p_{95}$, and $p_{99}$.
  - **Experiment B (Event Watcher Throughput):** Generates bursts of 500 file operations to test kernel event buffer absorption, queue capacity, and watcher debounce effectiveness.
  - **Experiment C (WAL Concurrency & Correctness):** Runs concurrent read/write transactions to verify that SQLite WAL mode delivers speedup over rollback journaling while maintaining zero corruption.
  - **Experiment D (Crash Recovery Simulation):** Simulates abrupt process termination during batch writes and benchmarks recovery time upon restart.

