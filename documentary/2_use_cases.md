# 2. SafeVault FS Use Cases

## 2.1 Accidental file overwrite

### Situation

A user replaces an important document with incorrect content.

### SafeVault response

1. The watcher detects the modification.
2. The previous content is saved as a version.
3. The event is written to the journal.
4. The user lists saved versions.
5. The user restores the required version.

## 2.2 Destructive development script

### Situation

A script accidentally deletes or changes source code, datasets, or generated
files.

### SafeVault response

- The operation is recorded with timestamp and process information.
- Previous versions can be inspected.
- Deleted or overwritten content can be restored when a stored version exists.

## 2.3 Ransomware or mass encryption

### Situation

A process rapidly modifies many files and writes high-entropy encrypted
content.

### Planned SafeVault response

- Calculate Shannon entropy for incoming write buffers.
- Count file modifications per PID within a time window.
- Detect suspicious bursts, such as more than 50 modified files in 3 seconds.
- Freeze or block the suspicious process pending approval.
- Preserve the pre-attack versions for recovery.

## 2.4 Forensic investigation

### Situation

An administrator needs to understand what changed and when.

### SafeVault response

The journal can provide:

- Event type.
- Path.
- Timestamp.
- Process ID.
- Parent process ID where available.
- Executable path.
- Previous path for rename operations.

## 2.5 Natural-language file search

### Situation

A user remembers the meaning of a file but not its exact name.

Example:

> Find the PDF invoices from last March that mention AWS.

### SafeVault response

1. Groq converts the natural-language request into a strict JSON schema.
2. SafeVault extracts keywords, file extensions, dates, and intent.
3. The Rust search engine traverses directories concurrently.
4. Matching results are sent to the frontend.

## 2.6 Point-in-time recovery

### Situation

A user needs the state of a file or directory as it existed at an earlier
time.

### Planned SafeVault response

- Query journal events up to a target timestamp.
- Resolve the latest valid version pointer for each file.
- Reconstruct files from stored chunks.
- Present a preview or diff before restoring.

## 2.7 Developer workflow protection

SafeVault can protect:

- Source code.
- Configuration files.
- Research datasets.
- Experiment outputs.
- Build artifacts.
- Documentation.

It complements Git by recording filesystem changes that may happen outside
normal commits.
