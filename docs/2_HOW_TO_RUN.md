# FileGuard: How to Build, Test, and Run

This guide provides step-by-step instructions for cloning, building, running, and testing **FileGuard** on Windows and Linux/WSL2.

---

## Prerequisites & Development Tools

Ensure the following tools are installed and available on your system `PATH`:

| Tool | Minimum Version | Purpose | Verification Command |
| :--- | :--- | :--- | :--- |
| **C++ Compiler** | C++20 capable (GCC 13+, Clang 16+, MSVC 2022) | Compiles native C++ core & server | `g++ --version` or `cl` |
| **CMake** | 3.16 or higher | C++ build configuration | `cmake --version` |
| **Ninja** or **Make**| Latest | High-speed C++ build generator | `ninja --version` |
| **Node.js** | 18.x or higher | Frontend runtime environment | `node -v` |
| **npm** | 9.x or higher | Frontend package manager | `npm -v` |
| **PowerShell** | 5.1 or PowerShell 7 (Windows) | Orchestration script execution | `$PSVersionTable.PSVersion` |

---

## Method 1: One-Click Quick Start (Recommended)

FileGuard includes an intelligent PowerShell orchestrator script: [`run_all.ps1`](file:///d:/EventDrivenFileSystem/run_all.ps1).

### Basic Launch

Open PowerShell in the project root directory (`D:\EventDrivenFileSystem`) and execute:

```powershell
.\run_all.ps1
```

### What `run_all.ps1` Does Automatically:
1. **Checks Prerequisites:** Validates that CMake, a C++ compiler, Node.js, and npm are installed.
2. **Builds the C++ Core:** Compiles `safevault.exe`, `fileguard_server.exe`, and `fileguard_tests.exe` using CMake and Ninja.
3. **Initializes Storage & Journal:** Creates `storage/`, initializes `storage/safevault.sqlite3`, and enables SQLite Write-Ahead Logging (WAL).
4. **Installs Frontend Dependencies:** Runs `npm install` inside `frontend/` if `node_modules` is absent.
5. **Starts Full-Stack Services:**
   - C++ API Server at **`http://127.0.0.1:8787`**
   - React + Vite UI at **`http://localhost:5173`**
6. **Opens Browser:** Automatically launches your default web browser to the FileGuard dashboard.

---

### Useful CLI Flags for `run_all.ps1`

You can customize the launcher behavior with parameters:

```powershell
# Run the automated test suite before launching the application
.\run_all.ps1 -RunTests

# Skip C++ compilation if binaries already exist (faster restart)
.\run_all.ps1 -SkipBuild

# Start the servers without opening a new browser window
.\run_all.ps1 -NoBrowser

# Clean and recompile from scratch
.\run_all.ps1 -Clean

# Run on a custom port (default is 8787)
.\run_all.ps1 -Port 9000

# Combined example: fast restart without browser launch
.\run_all.ps1 -SkipBuild -NoBrowser
```

To safely shut down the servers, simply press **`Ctrl+C`** in the PowerShell window. The script terminates background child processes cleanly.

---

## Method 2: Manual Step-by-Step Build & Run

If you prefer building and executing each component manually or are running on Linux / CI environments:

### Step 1: Build the C++ Backend & Test Suite

```bash
# 1. Create and navigate to the build directory
cd safevault_cpp
mkdir -p build && cd build

# 2. Configure with CMake (specifying Ninja generator and C++20)
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..

# 3. Build all targets
ninja
```

This compiles:
- `safevault.exe` (Main CLI & API Server binary)
- `fileguard_server.exe` (API Server standalone)
- `fileguard_tests.exe` (Automated Test Suite)

---

### Step 2: Run the Automated Test Suite

Run the compiled test binary:

```bash
# On Windows
.\safevault_cpp\build\fileguard_tests.exe

# On Linux / WSL
./safevault_cpp/build/fileguard_tests
```

**Expected Output:**
```text
========================================
  FileGuard Automated Test Suite
========================================
[RUNNING] test_sha256 ... [PASSED]
[RUNNING] test_filesystem_operations ... [PASSED]
[RUNNING] test_undo_redo_primitives ... [PASSED]
[RUNNING] test_persistent_restart ... [PASSED]
[RUNNING] test_redo_branch_invalidation ... [PASSED]
[RUNNING] test_conflict_detection ... [PASSED]
[RUNNING] test_batch_transactions ... [PASSED]
[RUNNING] test_benchmarks ... [PASSED]
[RUNNING] test_file_identity_and_histories ... [PASSED]
[RUNNING] test_external_candidates ... [PASSED]
[RUNNING] test_folder_insights_and_duplicates ... [PASSED]
[RUNNING] test_multistep_undo_redo_chain ... [PASSED]
[RUNNING] test_external_rename_rejection ... [PASSED]
[RUNNING] test_rename_edit_undo_redo_sequence ... [PASSED]
[RUNNING] test_quarantine_advanced_operations ... [PASSED]
[RUNNING] test_batch_transaction_details_and_undo ... [PASSED]
[RUNNING] test_rename_preserves_complete_file_history ... [PASSED]
[RUNNING] test_independent_content_and_name_undo_redo ... [PASSED]
[RUNNING] test_batch_move_multi_folder_restoration ... [PASSED]
[RUNNING] test_per_file_external_candidate_archive_across_renames ... [PASSED]
[RUNNING] test_batch_transaction_atomic_undo_and_redo ... [PASSED]
========================================
Tests Passed: 21 / 21
========================================
```

---

### Step 3: Start the C++ API Server

Initialize storage and start the HTTP service:

```bash
# 1. Initialize SQLite database schema
.\safevault_cpp\build\safevault.exe --database storage/safevault.sqlite3 init

# 2. Start API server monitoring the "watched" directory
.\safevault_cpp\build\safevault.exe --database storage/safevault.sqlite3 serve --bind 0.0.0.0:8787 --watch watched
```

Verify backend health in another terminal or browser:
```bash
curl http://127.0.0.1:8787/api/health
```

---

### Step 4: Start the React Frontend

Open a new terminal window:

```bash
cd frontend

# Install dependencies (only needed first time)
npm install

# Start Vite development server
npm run dev
```

The frontend will be live at: **`http://localhost:5173`**

---

## Troubleshooting & FAQ

### Port 8787 or 5173 is already in use
If an earlier process was not terminated cleanly, close any running instances:
```powershell
Get-Process -Name "safevault*", "fileguard*", "node" -ErrorAction SilentlyContinue | Stop-Process -Force
```

### Windows 11 Smart App Control (SAC) Notification
On newer Windows 11 systems with Smart App Control (SAC) enabled in Evaluation or Enforce mode, local unsigned C++ binaries compiled via MinGW or GCC may trigger a reputation prompt.
- `run_all.ps1` detects this automatically and seamlessly falls back to the WSL2 environment without requiring registry modifications or security downgrades.
- Alternatively, you can add an exclusion for the repository folder in Windows Security -> Virus & threat protection settings.

### Rebuilding Frontend for Production
To generate an optimized production bundle:
```bash
cd frontend
npm run build
```
The output will be placed in `frontend/dist/`.

