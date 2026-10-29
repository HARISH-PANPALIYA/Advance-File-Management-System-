# SafeVault FS Rust foundation

This crate is the Rust implementation track for SafeVault FS. The existing
Python watchdog prototype remains in the repository as a reference
implementation.

## Phase 1 status

- SQLite event journal with WAL mode.
- Create, modify, delete, and rename event types.
- PID, parent PID (on Linux), executable path, and UTC timestamp attribution.
- A backing-directory passthrough API that journals intent before mutation.
- `history` CLI query.
- Natural-language `search` CLI command using local parsing by default and
  Groq JSON parsing when `GROQ_API_KEY` is configured.

## Search

Without a Groq key, the CLI performs a safe local filename search:

```text
cargo run -- search "PDF invoices AWS" --root .
```

With Groq parsing enabled:

```powershell
$env:GROQ_API_KEY = "your-key"
cargo run -- search "Find PDF invoices from last March mentioning AWS" --root .
```

The React frontend in `../frontend` calls the future Tauri command
`search_files` when running inside Tauri and uses a local demo fallback in a
regular browser during development. To use Groq from the browser safely,
start the local parser API in another terminal:

```text
cargo run -- serve
```

The API listens on `http://127.0.0.1:8787` and exposes only the parsed search
schema. The Groq key remains inside the Rust process and is never sent to the
frontend.

## Continuous terminal watcher

For continuous tracking independent of the browser, run:

```powershell
cargo run -- watch "D:\path\to\folder"
```

The watcher keeps running in the terminal, records filesystem changes
immediately into SQLite WAL, and provides a menu:

1. Show recent activity
2. Show file history
3. Show search command
4. List saved versions
5. Roll back a file
6. Continue watching
7. Stop watcher

When a watched file is modified or deleted, the watcher saves its previous
contents in the `file_versions` SQLite table before updating the event log.
Choose menu option 4, enter the exact full path, and note the version ID.
Then choose option 5 and enter that ID to restore the contents.

Version capture begins when the watcher starts, so start the watcher before
editing files. The current implementation stores readable file contents and
is intended for files that fit comfortably in the local SQLite database.

The database is shared with the other CLI commands, so history remains
available after the watcher exits:

```powershell
cargo run -- history --path "D:\path\to\folder\file.txt"
```
- FUSE integration is reserved for the `fuse` feature and will be added after
  the journal contract is stable.

## Usage

From this directory, with Rust installed:

```text
cargo run -- init
cargo run -- record create watched/example.txt
cargo run -- history --path watched/example.txt
```

The default database is `.safevault_storage/safevault.sqlite3`.
