# 5. Architecture Evolution: C++ Core Engineering & Multi-Language Analysis

## 5.1 Project Evolution Overview

During early research and prototyping, the project explored multiple systems languages:
- **Python**: Useful for rapid proof-of-concept directory observation using `watchdog`.
- **Rust**: Explored in `safevault/` for memory safety, ownership modeling, and compiler-checked concurrency.
- **Modern C++ (C++17/C++20)**: Adopted as the **authoritative, primary systems engine** in `safevault_cpp/` for production deployment, native Win32/POSIX API direct integration, standardized `std::filesystem`, low-overhead SQLite integration, and minimal toolchain friction.

## 5.2 Rationale for C++ as Authoritative Engine

### 1. Direct OS Kernel & Win32 Integration
Filesystem event monitoring requires low-level asynchronous I/O and kernel structures:
- On Windows: Overlapped I/O with `ReadDirectoryChangesW`, `FILE_NOTIFY_INFORMATION` buffer unpacking, and wide-character Win32 API calls (`LPCWSTR`).
- In modern C++, these native system APIs bind with zero foreign-function interface (FFI) overhead and direct pointer manipulation without `unsafe` blocks.

### 2. Standardized Portable Filesystem Primitives (`std::filesystem`)
Introduced in C++17 and enhanced in C++20, `std::filesystem` provides clean, standard-library-level representations for:
- Lexical and physical canonicalization (`std::filesystem::canonical`, `weakly_canonical`).
- Path decomposition, extension extraction, and recursive iteration (`recursive_directory_iterator`).
- Cross-platform file status inspection (`std::filesystem::file_size`, `last_write_time`, `is_directory`).

### 3. Native Embedded SQLite WAL Engine
SQLite is authored in pure C. Incorporating SQLite into modern C++ provides seamless RAII encapsulation:
- Prepared statements wrapped in lightweight RAII classes (`sqlite3_stmt*`).
- Zero-copy parameter binding and fast blob/string retrieval.
- Direct control over WAL checkpoints, `PRAGMA synchronous = NORMAL`, and transaction boundaries (`BEGIN IMMEDIATE`, `COMMIT`, `ROLLBACK`).

### 4. RAII and Deterministic Resource Lifecycle
Modern C++ uses Resource Acquisition Is Initialization (RAII) to guarantee leak-free operation:
- Mutex locks via `std::lock_guard` and `std::unique_lock`.
- Thread pools and worker lifecycles managed deterministically.
- File descriptors and handles automatically closed during stack unwinding.

## 5.3 Comparative Systems Takeaways

| Feature / Metric | Python Prototype | Rust Prototype | Modern C++ Core (FileGuard) |
|---|---|---|---|
| **Authoritative Status** | Retired (Proof-of-Concept) | Historical Prototype | **Primary Production Engine** |
| **Runtime Overhead** | High (GIL, Interpreter) | Zero (Native binary) | **Zero (Native binary)** |
| **OS API Integration** | High-level wrapper | FFI / WinAPI crate | **Direct Win32 / POSIX APIs** |
| **Filesystem Safety** | Basic exception handling | Checked types | **Strict Approved-Root Sandbox** |
| **Concurrency Model** | Threads limited by GIL | Channels / Rayon | **std::thread, std::async, Mutex** |
| **Journaling** | Basic SQLite script | Rusqlite WAL | **Robust C++ SQLite WAL Service** |
| **Build Artifact** | Script collection | Cargo workspace | **Single CMake/Ninja Binary** |
