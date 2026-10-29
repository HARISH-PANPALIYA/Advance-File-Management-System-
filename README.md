# FileGuard — Event-Driven File Management and Recovery System

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/Build-CMake%20%2F%20Ninja-success.svg)](https://cmake.org)
[![React](https://img.shields.io/badge/Frontend-React%2018%20%2B%20TypeScript-61dafb.svg)](https://react.dev)
[![SQLite](https://img.shields.io/badge/Database-SQLite%20WAL-lightgrey.svg)](https://sqlite.org)

**FileGuard** is a high-reliability operating-system-level file management, journaled recovery, and performance benchmarking system. Built on modern C++20, SQLite WAL, and a restrained React desktop interface, FileGuard replaces fragile browser-based filesystem access with a sandboxed, authoritative native backend providing:

1. **Persistent LIFO Undo / Redo**: Deterministic reversal of file/dir creation, rename, move, copy, delete, and overwrite across application and OS restarts.
2. **Conflict Detection**: SHA-256 pre-hash and metadata validation preventing destructive overwrites when external modifications occur.
3. **Write-Ahead Operation Journal**: Application-level WAL state machine in SQLite with redo-branch invalidation and crash recovery reconciliation.
4. **Quarantine-Based Recoverable Deletion**: Safe two-phase unlinking via a managed vault, preserving directory structures and original paths.
5. **Native OS Event Monitoring**: Asynchronous kernel event capture (`ReadDirectoryChangesW` on Windows with overlapped I/O) streaming directly to the UI via Server-Sent Events (SSE).
6. **Transactional Batch Operations**: Atomic multi-file transactions with automated forward execution and compensating backward rollback upon failure.
7. **OS Performance Laboratory**: Reproducible micro-benchmarking comparing sequential vs. concurrent multi-threaded scans, event-driven latency vs. periodic polling, and journaling overhead.

---

## Documentation Quick Links

Detailed documentation is available in the [`docs/`](file:///d:/EventDrivenFileSystem/docs/) directory:

- 📘 [**1. Operating System Concepts & Implementation Architecture**](file:///d:/EventDrivenFileSystem/docs/1_OS_CONCEPTS_AND_ARCHITECTURE.md): Complete analysis of all OS concepts used (inodes, stable identity, asynchronous kernel notifications, WAL, transactions, concurrency, snapshots, sandboxing) and where they are located.
- 🚀 [**2. How to Build, Test, and Run FileGuard**](file:///d:/EventDrivenFileSystem/docs/2_HOW_TO_RUN.md): Step-by-step instructions for one-click PowerShell startup, CLI flags, manual CMake/Ninja builds, test execution, and troubleshooting.
- 🛠️ [**3. C++ Header Files & Architecture Reference**](file:///d:/EventDrivenFileSystem/docs/3_HEADER_FILES_AND_FUNCTIONS.md): Exhaustive index of all headers in `safevault_cpp/include/` with class and function summaries.

---

## 1. Operating System Concepts Demonstrated

- **Write-Ahead Logging (WAL)**: Guaranteeing crash consistency at the application layer by persisting intent and recovery pre-images before applying filesystem mutations.
- **Asynchronous I/O & Kernel Notifications**: Utilizing Windows `ReadDirectoryChangesW` and `FILE_NOTIFY_INFORMATION` with overlapped I/O and event deduplication instead of wasteful polling loops.
- **Transactional Consistency & Compensating Operations**: Implementing atomic-like multi-file batch mutations through forward intent records and inverse compensation upon mid-batch failures.
- **Concurrency & Parallel Filesystem Traversal**: Demonstrating Amdahl's law and thread pool scaling via recursive work-stealing directory scanning vs. single-threaded sequential traversal.
- **Integrity Verification & Content Hashing**: Streaming SHA-256 digests across file contents to detect post-operation divergence and data corruption.
- **Security Sandboxing & Path Canonicalization**: Enforcing strict approved-root boundaries using `std::filesystem::weakly_canonical` to eliminate directory traversal (`../`) vulnerabilities.

---

## 2. Architecture & Technology Stack

```text
React 18 + TypeScript Desktop UI (Port 5173)
        │
        │ HTTP REST & Server-Sent Events (SSE)
        ▼
C++20 FileGuard Engine (Port 8787)
        │
        ├── FilesystemService (Canonical sandboxing, approved roots, directory traversal)
        ├── CommandManager (Concrete commands: create, rename, move, copy, delete, overwrite)
        ├── OperationJournal (SQLite WAL mode, schema migrations, LIFO undo/redo stack)
        ├── RecoveryService (Managed quarantine, version snapshots, SHA-256 integrity)
        ├── EventMonitor (Native ReadDirectoryChangesW, event coalescing, SSE broadcast)
        ├── BenchmarkService (Reproducible OS lab experiments & CSV export)
        └── ApiServer (Embedded cpp-httplib REST & SSE endpoints)
        │
        ▼
Local Host Operating System Filesystem
```

### Technology Breakdown
- **Core Systems Language**: C++20 (`std::filesystem`, `std::jthread`, `std::chrono`).
- **Database**: SQLite 3 configured with Write-Ahead Logging (`PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL`).
- **HTTP / SSE Networking**: Header-only `cpp-httplib` (threaded socket listener bound to loopback `127.0.0.1`).
- **Serialization**: `nlohmann/json` modern JSON library.
- **Integrity**: Clean, portable C++ SHA-256 implementation.
- **Frontend**: React 18, TypeScript 5.7, Vite 6, Lucide React icons. Styled using a restrained, dark graphite desktop aesthetic (no generic AI marketing tropes or purple gradients).

---

## 3. Repository Structure

```text
EventDrivenFileSystem/
├── CMakeLists.txt                # Root CMake entry point
├── run_all.ps1                   # Master full-stack PowerShell launcher
├── README.md                     # Project overview and quick links
├── docs/                         # Comprehensive architectural and operational documentation
│   ├── 1_OS_CONCEPTS_AND_ARCHITECTURE.md # Analysis of all OS concepts and implementation locations
│   ├── 2_HOW_TO_RUN.md           # Step-by-step build, run, and testing instructions
│   └── 3_HEADER_FILES_AND_FUNCTIONS.md   # Detailed C++ header and function catalog
├── storage/                      # Managed storage directory
│   ├── safevault.sqlite3         # Authoritative SQLite WAL database
│   ├── quarantine/               # Recoverable deletion vault
│   └── snapshots/                # File version recovery pre-images
├── watched/                      # Default active monitored directory (.gitkeep)
├── safevault_cpp/                # C++20 Core Systems Engine
│   ├── CMakeLists.txt            # Build configuration
│   ├── include/                  # 13 subsystem header files
│   ├── src/                      # Implementation files
│   ├── tests/
│   │   └── test_main.cpp         # Automated test suite (21 comprehensive tests)
│   └── third_party/              # Bundled libraries (httplib, json, sqlite3)
├── frontend/                     # React + TypeScript Desktop UI
│   ├── src/
│   │   ├── App.tsx               # Main Desktop Interface
│   │   ├── api.ts                # Strongly-typed C++ backend client
│   │   ├── styles.css            # Refined desktop design tokens
│   │   └── main.tsx              # React DOM entry point
│   ├── index.html                # HTML5 entry with Inter & JetBrains Mono
│   ├── package.json              # NPM manifest
│   └── vite.config.ts            # Vite proxy configuration (/api -> :8787)
└── documentary/                  # Historical OS design notes
```

---

## 4. Prerequisites

- **Operating System**: Windows 10/11 (64-bit) or Linux (GCC 11+ with inotify).
- **C++ Compiler**: GCC 11+, Clang 13+, or MSVC 2019+ supporting C++20.
- **Build System**: CMake 3.20+ and Ninja (or Make / NMake).
- **Node.js**: Node 18+ LTS and npm 9+.

---

## 5. Quick Start (Windows)

The simplest way to build, test, and launch the complete system is via the master PowerShell script:

```powershell
# In PowerShell:
.\run_all.ps1
```

The script automatically:
1. Detects CMake, C++ toolchain (Ninja/GCC/MSVC), and Node/npm.
2. Builds `safevault.exe` and `fileguard_tests.exe` via CMake and Ninja.
3. Initializes the SQLite database and directory structure.
4. Installs frontend npm packages (on initial setup).
5. Launches the C++ API server on port 8787 and Vite frontend on port 5173.
6. Opens the default browser to `http://localhost:5173`.
7. Catches `Ctrl+C` to cleanly terminate all background child processes.

### Useful Launcher Options
```powershell
# Run automated tests before launching:
.\run_all.ps1 -RunTests

# Skip C++ compilation if already built:
.\run_all.ps1 -SkipBuild

# Custom ports without auto-opening browser:
.\run_all.ps1 -Port 9000 -FrontendPort 3000 -NoBrowser
```

---

## 6. Manual Build Instructions

### C++ Backend
```bash
# Configure build directory
cmake -B safevault_cpp/build -G Ninja -S safevault_cpp

# Compile core binary and test suite
cmake --build safevault_cpp/build

# Run test suite
./safevault_cpp/build/fileguard_tests.exe
```

### React Frontend
```bash
cd frontend
npm install
npm run build      # Production bundle validation
npm run dev        # Development server
```

---

## 7. Command-Line Interface (CLI)

The `safevault.exe` executable provides rich standalone subcommands:

```bash
# Initialize storage and SQLite WAL schema
./safevault.exe --database storage/safevault.sqlite3 init

# Start full HTTP/SSE server watching a directory
./safevault.exe --database storage/safevault.sqlite3 serve --bind 127.0.0.1:8787 --watch watched

# Undo the last filesystem mutation
./safevault.exe --database storage/safevault.sqlite3 undo

# Redo the last undone mutation
./safevault.exe --database storage/safevault.sqlite3 redo

# Display journaled history
./safevault.exe --database storage/safevault.sqlite3 history

# Fast indexed/recursive search
./safevault.exe search --root watched "report"

# Run standalone benchmark suite
./safevault.exe bench scan --dir watched --files 5000
./safevault.exe bench journal --ops 500
```

---

## 8. Frontend-Backend API Contract

| Endpoint | Method | Description |
|---|---|---|
| `/api/health` | `GET` | Health check and engine uptime status |
| `/api/files?path=...` | `GET` | Directory listing with file metadata and sizes |
| `/api/metadata?path=...` | `GET` | Detailed file metadata and SHA-256 hash |
| `/api/search?root=...&query=...` | `GET` | Recursive filesystem search |
| `/api/operations` | `POST` | Execute single filesystem operation (create, rename, move, copy, delete, overwrite) |
| `/api/history` | `GET` | Retrieve journaled operations timeline |
| `/api/undo` | `POST` | Execute conflict-aware LIFO undo |
| `/api/redo` | `POST` | Execute conflict-aware redo |
| `/api/versions?path=...` | `GET` | List saved pre-image snapshots for a file |
| `/api/versions/checkpoint` | `POST` | Manually capture a snapshot version |
| `/api/restore` | `POST` | Restore a file to a previous version |
| `/api/quarantine` | `GET` | List recoverable quarantined items |
| `/api/quarantine/restore` | `POST` | Restore an item from quarantine to original path |
| `/api/transactions` | `POST` | Execute multi-file transactional batch operation |
| `/api/watch/start` | `POST` | Attach native OS watcher to a directory |
| `/api/watch/stop` | `POST` | Stop active directory watcher |
| `/api/watch/status` | `GET` | Check watcher state and monitored path |
| `/api/events` | `GET` | Query recent filesystem events |
| `/api/events/stream` | `GET` | Live Server-Sent Events (SSE) notification stream |
| `/api/benchmarks/run` | `POST` | Execute OS benchmark experiment (scan, watch_vs_poll, journal) |
| `/api/benchmarks/{id}` | `GET` | Retrieve results of a benchmark run |
| `/api/settings` | `GET` | Approved roots, database location, and storage metrics |

---

## 9. Design of Core Features

### Persistent Undo / Redo & Redo Branch Invalidation
Each modifying operation creates an `OperationRecord` in SQLite with status `INTENT`. Following filesystem execution, status transitions to `COMMITTED` with pre- and post-state hashes.
- **Undo**: Queries `get_last_undoable_operation()`, verifies that the target file currently matches `expected_dest_hash` (conflict detection), reverses the mutation, and marks the record `UNDONE`.
- **Redo**: Queries `get_last_redoable_operation()`, re-applies the mutation, and restores `COMMITTED` status.
- **Branch Invalidation**: When any new operation executes while undone operations exist in the journal, those undone operations are marked `SUPERSEDED` (`is_undoable = 0`), guaranteeing that divergent redo branches cannot corrupt the filesystem.

### Conflict Detection
Before an operation is undone or redone, FileGuard computes the live file's SHA-256 hash and verifies its existence:
- If an external program altered or removed the file while it was in an undone/redone state, FileGuard aborts the action with a structured conflict error (`409 Conflict: target file was modified externally`).

### Transactional Batch Operations
Multi-file operations (e.g. batch copy, rename, move, delete) are grouped under a unique `transaction_id`. Operations execute deterministically. If an operation fails mid-batch:
- FileGuard automatically initiates **compensating inverse operations** for every completed item in reverse order.
- The transaction record logs `status = 'ROLLED_BACK'` with detailed failure diagnostics.

### Native OS Directory Monitoring
On Windows, `EventMonitor` spins an asynchronous worker thread using `ReadDirectoryChangesW` with a 64 KB overlapped buffer.
- Events (`FILE_ACTION_ADDED`, `MODIFIED`, `REMOVED`, `RENAMED`) are unpacked, deduplicated, attributed, and journaled into SQLite.
- Active SSE listeners receive instant push updates without web client polling.

---

## 10. Automated Test Suite

FileGuard includes an automated C++ test suite verifying all core behaviors under controlled temporary environments:

```powershell
.\safevault_cpp\build\fileguard_tests.exe
```

### Verified Scenarios:
1. `test_sha256`: Digest correctness for empty strings, text buffers, and file contents.
2. `test_filesystem_service`: Canonical sandboxing, path traversal rejection, directory creation, file writes, copies, moves, and metadata calculation.
3. `test_undo_redo_primitives`: Creation, rename, copy, recoverable delete, and overwrite reversal.
4. `test_persistent_undo_across_restart`: Simulates process crash/restart by destroying and re-instantiating engine components against the same SQLite database and restoring files.
5. `test_redo_branch_invalidation`: Verifies that a new mutation correctly invalidates previous redo entries (`SUPERSEDED`).
6. `test_conflict_detection`: Verifies that external modifications trigger conflict detection and prevent silent overwrites.
7. `test_batch_transactions`: Tests multi-file transactions, mid-batch injection failures, and automatic backward compensation rollback.
8. `test_benchmark_service`: Validates micro-benchmark execution, sequential vs. concurrent scan consistency, and CSV persistence.

---

## 11. Security and Safety Considerations

- **Approved-Root Sandboxing**: Path resolution utilizes lexical cleaning and physical canonicalization (`weakly_canonical`). Requests attempting directory traversal outside approved roots (`../`) return `403 Forbidden`.
- **Loopback Binding**: The C++ server binds exclusively to `127.0.0.1` to prevent unauthorized remote network access.
- **Controlled Quarantine**: Deleted items are moved to `.safevault_storage/quarantine/` with unique IDs, avoiding permanent data loss.
- **Disposable Benchmarking**: Benchmark datasets are created inside isolated temporary directories and automatically cleaned up upon test completion.