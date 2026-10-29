# 4. Operating-System Concepts and Principles

## 4.1 Filesystems and system calls

Applications use filesystem operations such as:

- `open`
- `read`
- `write`
- `close`
- `rename`
- `unlink`
- `stat`

SafeVault is intended to observe or intercept these operations through a
filesystem layer. A FUSE implementation moves part of the filesystem logic
from the kernel into a userspace Rust process.

## 4.2 Filesystem in Userspace

FUSE allows a userspace program to implement filesystem behavior. The
operating system forwards filesystem requests to the mounted process.

Benefits:

- Faster development than writing a kernel module.
- Lower risk than modifying kernel code.
- Easier integration with Rust libraries.
- Ability to add journaling and policy decisions around file operations.

Trade-offs:

- Userspace context switches add overhead.
- The mount process must remain available.
- Permissions and platform behavior differ between Linux, macOS, and Windows.

## 4.3 Write-ahead logging

Write-ahead logging means recording intent or previous state before a
modification is considered complete.

SafeVault uses SQLite WAL for journal durability. The basic principle is:

```text
Receive operation
    |
    v
Record event and previous state
    |
    v
Apply filesystem mutation
```

If the process crashes, the journal can help determine what happened and
which version should be recovered.

## 4.4 Transactions and atomicity

A filesystem update should be treated as a logical transaction where possible.
The system should avoid leaving metadata and file content inconsistent.

Important properties:

- Atomic event insertion.
- Consistent version references.
- Clear failure reporting.
- Recovery after interruption.

## 4.5 Process management and attribution

SafeVault records the process ID associated with an operation. The planned
implementation also records:

- Parent PID.
- Executable path.
- User identity where supported.
- Timestamp.

This relates to process management, protection, auditing, and security
principles in operating systems.

## 4.6 Concurrency

Filesystem monitoring and search are concurrent workloads:

- The watcher receives events while users continue editing.
- SQLite readers may inspect history while events are appended.
- Directory traversal can process independent paths in parallel.
- Search results can stream while scanning continues.

The Rust implementation uses or plans to use:

- Threads for long-running services.
- `rayon` for data-parallel filtering.
- `jwalk` for concurrent directory traversal.
- Tokio for asynchronous service integration.

## 4.7 Synchronization

Shared state requires synchronization:

- Watcher event channels.
- SQLite connections.
- Version snapshot state.
- Search result streams.

The implementation must prevent races such as reading a file while its
version metadata is only partially written.

## 4.8 Storage management

The planned storage layer uses:

- Content-defined chunks.
- Cryptographic hashes.
- Reference counts or version pointers.
- Compression.
- Hidden internal storage.

This is related to block allocation, free-space management, caching, and
storage efficiency.

## 4.9 Content-defined chunking

FastCDC or Rabin-style chunking splits data based on content boundaries
rather than fixed offsets. Small edits then affect only nearby chunks instead
of requiring an entire file to be copied.

This supports:

- Deduplication.
- Efficient version storage.
- Faster reconstruction.
- Reduced disk usage.

## 4.10 Caching and consistency

Search indexes and file metadata may become stale while files are changing.
SafeVault must define when data is refreshed and how the UI indicates that a
scan is in progress.

Correctness is more important than showing a fast but stale result.

## 4.11 Protection and access control

The storage directory should not be treated as an ordinary user workspace.
SafeVault should restrict casual modification of internal chunks and metadata
through permissions, path validation, and controlled APIs.

## 4.12 Failure handling and recovery

Expected failures include:

- File moved or deleted during scanning.
- Permission denied.
- Corrupt journal record.
- Disk full.
- Watcher interruption.
- Backend process termination.
- Invalid restore request.

Errors should be explicit and should not silently produce a successful-looking
result.

## 4.13 Security monitoring

The planned threat-defense layer uses:

- Entropy measurement.
- Per-process modification rates.
- Process attribution.
- Event history.
- User approval before suspension is lifted.

These mechanisms are behavioral detection controls, not a replacement for
antivirus software or offline backups.

## 4.14 Portability

FUSE is common on Linux and available through related solutions on macOS.
Windows requires a different integration strategy such as WinFsp or a
platform-specific filesystem API. SafeVault must isolate platform-specific
mount code from the portable journal, storage, and search modules.
