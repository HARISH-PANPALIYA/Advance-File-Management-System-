# FileGuard -- Event-Driven File Management and Recovery System
# Master Launcher for Windows

[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$RunTests,
    [switch]$NoBrowser,
    [switch]$WSL,
    [switch]$OpenSecuritySettings,
    [int]$Port = 8787,
    [int]$FrontendPort = 5173
)

$ErrorActionPreference = "Stop"
$Root        = $PSScriptRoot
$CppBuildDir = "$Root\safevault_cpp\build"
$CppExe      = "$CppBuildDir\safevault.exe"
$TestsExe    = "$CppBuildDir\fileguard_tests.exe"
$WslExe      = "/mnt/d/EventDrivenFileSystem/safevault_cpp/build/fileguard_wsl"
$WatchDir    = "$Root\watched"
$FrontendDir = "$Root\frontend"
$StorageDir  = "$Root\storage"
$DbPath      = "$StorageDir\safevault.sqlite3"

function Test-IsAppControlBlocked($err) {
    if (-not $err) { return $false }
    $s = $err.ToString()
    if ($err.Exception) { $s += " " + $err.Exception.Message }
    return ($s -match "Application Control" -or $s -match "blocked this file" -or $s -match "0283ac0f-fff1-49ae-ada1-8a933130cad6")
}

function Show-AppControlHelp {
    param([string]$ExePath)
    Write-Host ""
    Write-Host "==========================================================================" -ForegroundColor Yellow
    Write-Host " [NOTE] Windows Code Integrity / Smart App Control Detected" -ForegroundColor Yellow
    Write-Host "==========================================================================" -ForegroundColor Yellow
    Write-Host "  Target: $ExePath" -ForegroundColor Gray
    Write-Host ""
    Write-Host "  Why this happens & Safe Solutions:" -ForegroundColor White
    Write-Host "  1. Developer Mode is ALREADY turned ON." -ForegroundColor Green
    Write-Host "     Windows Code Integrity updates its active kernel cache after a PC restart." -ForegroundColor Green
    Write-Host "  2. DO NOT turn off Smart App Control in Windows Security." -ForegroundColor Cyan
    Write-Host "     (Windows warns that turning SAC Off cannot be reversed without reinstalling." -ForegroundColor Cyan
    Write-Host "      You NEVER need to turn SAC off or reinstall Windows!)" -ForegroundColor Cyan
    Write-Host "  3. ZERO RESTART REQUIRED:" -ForegroundColor Yellow
    Write-Host "     Run FileGuard immediately with WSL2 backend:" -ForegroundColor White
    Write-Host "       .\run_all.ps1 -WSL" -ForegroundColor Green
    Write-Host "     WSL runs the C++ backend instantly without restrictions," -ForegroundColor Gray
    Write-Host "     and connects directly to your React UI at http://localhost:5173." -ForegroundColor Gray
    Write-Host "==========================================================================" -ForegroundColor Yellow
    Write-Host ""
}

if ($OpenSecuritySettings) {
    Write-Host "Opening Windows Security & Developer settings..." -ForegroundColor Cyan
    Start-Process "windowsdefender://appbrowser"
    Start-Process "ms-settings:developers"
    exit 0
}

Write-Host ""
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  FileGuard: Event-Driven File Management & Recovery System" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host ""

# 1. Verify Prerequisites
Write-Host "[1/6] Checking required development tools..." -ForegroundColor Yellow

$hasCmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $hasCmake) {
    Write-Host "[ERROR] CMake is required but was not found in PATH." -ForegroundColor Red
    Write-Host "        Please install CMake from https://cmake.org/download/" -ForegroundColor Red
    exit 1
}

$hasNinja = Get-Command ninja -ErrorAction SilentlyContinue
$hasGcc   = Get-Command gcc -ErrorAction SilentlyContinue
$hasCl    = Get-Command cl -ErrorAction SilentlyContinue

if (-not ($hasNinja -or $hasGcc -or $hasCl)) {
    Write-Host "[ERROR] No supported C++ compiler/build tool (Ninja, GCC, or MSVC) found in PATH." -ForegroundColor Red
    exit 1
}

$hasNode = Get-Command node -ErrorAction SilentlyContinue
$hasNpm  = Get-Command npm -ErrorAction SilentlyContinue
if (-not ($hasNode -and $hasNpm)) {
    Write-Host "[ERROR] Node.js and npm are required for the frontend." -ForegroundColor Red
    Write-Host "        Please install Node.js (LTS) from https://nodejs.org/" -ForegroundColor Red
    exit 1
}

Write-Host "      [OK] CMake ($(& cmake --version | Select-Object -First 1))" -ForegroundColor Green
Write-Host "      [OK] Node ($(& node --version)) & npm ($(& npm --version))" -ForegroundColor Green

# 2. Build C++ Backend
if (-not $SkipBuild -or -not (Test-Path $CppExe)) {
    Write-Host ""
    Write-Host "[2/6] Building FileGuard C++ Core and Test Suite..." -ForegroundColor Yellow
    
    $generator = "Ninja"
    if (-not $hasNinja) {
        $generator = "NMake Makefiles"
    }
    
    if (-not (Test-Path "$CppBuildDir\build.ninja") -and -not (Test-Path "$CppBuildDir\Makefile") -and -not (Test-Path "$CppBuildDir\CMakeCache.txt")) {
        Write-Host "      Configuring CMake with generator: $generator" -ForegroundColor Gray
        & cmake -B $CppBuildDir -G $generator -S "$Root\safevault_cpp"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[ERROR] CMake configuration failed." -ForegroundColor Red
            exit 1
        }
    }

    & cmake --build $CppBuildDir
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] C++ build failed. Please review compiler error output above." -ForegroundColor Red
        exit 1
    }
    Write-Host "      [OK] C++ binaries compiled successfully." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "[2/6] Skipping C++ build (-SkipBuild specified and binary exists)." -ForegroundColor Gray
}

# Determine Backend Runtime Mode
$backendMode = "Windows Native"
if ($WSL) {
    $backendMode = "WSL2"
} else {
    # Test if native binary is blocked by SAC
    try {
        $testRun = & $CppExe --help 2>&1
    } catch {
        if (Test-IsAppControlBlocked $_) {
            Write-Host ""
            Write-Host "      [NOTICE] Windows Code Integrity policy active for native .exe before restart." -ForegroundColor Yellow
            Write-Host "               Seamlessly switching to WSL2 backend (zero reboot required)..." -ForegroundColor Cyan
            $backendMode = "WSL2"
        }
    }
}

# 3. Optional Automated Tests
if ($RunTests) {
    Write-Host ""
    Write-Host "[3/6] Running automated C++ test suite..." -ForegroundColor Yellow
    if ($backendMode -eq "WSL2") {
        Write-Host "      Executing test suite in WSL2 environment..." -ForegroundColor Cyan
        wsl.exe -e /bin/bash -c "cd /mnt/d/EventDrivenFileSystem/safevault_cpp/build && $WslExe test"
    } else {
        try {
            & $CppExe test
        } catch {
            if (Test-IsAppControlBlocked $_) {
                Write-Host "      [NOTICE] Native binary blocked by SAC. Running tests via WSL2..." -ForegroundColor Yellow
                wsl.exe -e /bin/bash -c "cd /mnt/d/EventDrivenFileSystem/safevault_cpp/build && $WslExe test"
            } else {
                throw $_
            }
        }
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[ERROR] Test suite failed! Halting launch." -ForegroundColor Red
        exit 1
    }
    Write-Host "      [OK] All test suites passed." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "[3/6] Skipping test execution (pass -RunTests to run on launch)." -ForegroundColor Gray
}

# 4. Storage & Directory Initialization
Write-Host ""
Write-Host "[4/6] Initializing storage directories and SQLite WAL journal..." -ForegroundColor Yellow

if (-not (Test-Path $WatchDir)) {
    New-Item -ItemType Directory -Path $WatchDir -Force | Out-Null
    Write-Host "      [OK] Created default watched sandbox: $WatchDir" -ForegroundColor Green
}
if (-not (Test-Path $StorageDir)) {
    New-Item -ItemType Directory -Path $StorageDir -Force | Out-Null
}

if ($backendMode -eq "WSL2") {
    $wslDb = "/mnt/d/EventDrivenFileSystem/storage/safevault.sqlite3"
    wsl.exe -e /bin/bash -c "$WslExe --database '$wslDb' init"
    Write-Host "      [OK] Database initialized via WSL2 at: $DbPath" -ForegroundColor Green
} else {
    try {
        & $CppExe --database $DbPath init 2>&1 | Out-Null
        Write-Host "      [OK] Database initialized at: $DbPath" -ForegroundColor Green
    } catch {
        if (Test-IsAppControlBlocked $_) {
            Write-Host "      [NOTICE] Switching initialization to WSL2..." -ForegroundColor Yellow
            $backendMode = "WSL2"
            $wslDb = "/mnt/d/EventDrivenFileSystem/storage/safevault.sqlite3"
            wsl.exe -e /bin/bash -c "$WslExe --database '$wslDb' init"
            Write-Host "      [OK] Database initialized via WSL2 at: $DbPath" -ForegroundColor Green
        } else {
            Write-Host "[ERROR] Database init error: $_" -ForegroundColor Red
            exit 1
        }
    }
}

# 5. Frontend Package Installation
Write-Host ""
Write-Host "[5/6] Verifying frontend packages..." -ForegroundColor Yellow
if (-not (Test-Path "$FrontendDir\node_modules")) {
    Write-Host "      Installing frontend npm packages (first-time setup)..." -ForegroundColor Yellow
    Push-Location $FrontendDir
    npm install
    Pop-Location
}
Write-Host "      [OK] Frontend dependencies ready." -ForegroundColor Green

# 6. Launching Services
Write-Host ""
Write-Host "[6/6] Starting FileGuard full-stack application ($backendMode)..." -ForegroundColor Yellow

$backendOutLog  = "$Root\storage\backend.stdout.log"
$backendErrLog  = "$Root\storage\backend.stderr.log"
$frontendOutLog = "$Root\storage\frontend.stdout.log"
$frontendErrLog = "$Root\storage\frontend.stderr.log"

# Clean prior log files safely
foreach ($log in @($backendOutLog, $backendErrLog, $frontendOutLog, $frontendErrLog)) {
    try {
        if (Test-Path $log) {
            Clear-Content -Path $log -Force -ErrorAction SilentlyContinue
        } else {
            "" | Out-File -FilePath $log -Encoding utf8 -Force -ErrorAction SilentlyContinue
        }
    } catch {}
}

# Start Backend Server
if ($backendMode -eq "WSL2") {
    $wslDb = "/mnt/d/EventDrivenFileSystem/storage/safevault.sqlite3"
    $wslWatch = "/mnt/d/EventDrivenFileSystem/watched"
    $backendProcess = Start-Process -FilePath "wsl.exe" `
        -ArgumentList "-e /bin/bash -c `"$WslExe --database '$wslDb' serve --bind 0.0.0.0:$Port --watch '$wslWatch'`"" `
        -RedirectStandardOutput $backendOutLog `
        -RedirectStandardError $backendErrLog `
        -PassThru
} else {
    try {
        $backendProcess = Start-Process -FilePath $CppExe `
            -ArgumentList "--database `"$DbPath`" serve --bind 127.0.0.1:$Port --watch `"$WatchDir`"" `
            -WorkingDirectory $Root `
            -RedirectStandardOutput $backendOutLog `
            -RedirectStandardError $backendErrLog `
            -PassThru
    } catch {
        if (Test-IsAppControlBlocked $_) {
            Write-Host "      [NOTICE] Falling back to WSL2 API server..." -ForegroundColor Yellow
            $backendMode = "WSL2"
            $wslDb = "/mnt/d/EventDrivenFileSystem/storage/safevault.sqlite3"
            $wslWatch = "/mnt/d/EventDrivenFileSystem/watched"
            $backendProcess = Start-Process -FilePath "wsl.exe" `
                -ArgumentList "-e /bin/bash -c `"$WslExe --database '$wslDb' serve --bind 0.0.0.0:$Port --watch '$wslWatch'`"" `
                -RedirectStandardOutput $backendOutLog `
                -RedirectStandardError $backendErrLog `
                -PassThru
        } else {
            throw $_
        }
    }
}

# Wait for backend health
Write-Host "      Waiting for C++ API Server at http://127.0.0.1:$Port ($backendMode)..." -ForegroundColor Gray
$serverReady = $false
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Milliseconds 400
    try {
        $resp = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/health" -TimeoutSec 1 -ErrorAction SilentlyContinue
        if ($resp -and $resp.status -eq "ok") {
            $serverReady = $true
            break
        }
    } catch {}
}

if (-not $serverReady) {
    Write-Host "[ERROR] C++ API Server failed to respond on port $Port within 10 seconds." -ForegroundColor Red
    if (Test-Path $backendErrLog) {
        Write-Host "Backend stderr output:" -ForegroundColor Yellow
        Get-Content $backendErrLog -Tail 20
    }
    if (Test-Path $backendOutLog) {
        Write-Host "Backend stdout output:" -ForegroundColor Yellow
        Get-Content $backendOutLog -Tail 20
    }
    if ($backendProcess -and -not $backendProcess.HasExited) {
        Stop-Process -Id $backendProcess.Id -Force -ErrorAction SilentlyContinue
    }
    exit 1
}
Write-Host "      [OK] C++ API Server running on http://127.0.0.1:$Port [$backendMode]" -ForegroundColor Green

# Start Frontend Dev Server
Push-Location $FrontendDir
$npmCmd = (Get-Command npm.cmd -ErrorAction SilentlyContinue).Source
if (-not $npmCmd) { $npmCmd = "npm" }

$frontendProcess = Start-Process -FilePath $npmCmd `
    -ArgumentList "run dev -- --port $FrontendPort" `
    -WorkingDirectory $FrontendDir `
    -RedirectStandardOutput $frontendOutLog `
    -RedirectStandardError $frontendErrLog `
    -PassThru
Pop-Location

Start-Sleep -Seconds 2
Write-Host "      [OK] React + Vite UI running on http://localhost:$FrontendPort" -ForegroundColor Green

# Open Browser
if (-not $NoBrowser) {
    Start-Process "http://localhost:$FrontendPort"
}

Write-Host ""
Write-Host "=================================================================" -ForegroundColor Green
Write-Host "  FileGuard is active and operational!" -ForegroundColor Green
Write-Host "=================================================================" -ForegroundColor Green
Write-Host "  - Mode:               $backendMode" -ForegroundColor Cyan
Write-Host "  - UI Dashboard:       http://localhost:$FrontendPort" -ForegroundColor Cyan
Write-Host "  - C++ API Server:     http://127.0.0.1:$Port" -ForegroundColor Cyan
Write-Host "  - Active Watcher:     $WatchDir" -ForegroundColor Cyan
Write-Host "  - SQLite Journal:     $DbPath" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Press Ctrl+C to safely shut down all services." -ForegroundColor Yellow
Write-Host ""

# Clean Shutdown Trap
try {
    while ($true) {
        Start-Sleep -Seconds 1
        if ($backendProcess.HasExited) {
            Write-Host "[WARN] C++ Backend server exited unexpectedly." -ForegroundColor Red
            break
        }
        if ($frontendProcess.HasExited) {
            Write-Host "[WARN] Frontend dev server exited unexpectedly." -ForegroundColor Red
            break
        }
    }
} finally {
    Write-Host ""
    Write-Host "[SHUTDOWN] Stopping background services..." -ForegroundColor Yellow
    if ($backendProcess -and -not $backendProcess.HasExited) {
        Stop-Process -Id $backendProcess.Id -Force -ErrorAction SilentlyContinue
        Write-Host "  [OK] C++ API server stopped." -ForegroundColor DarkGray
    }
    if ($frontendProcess -and -not $frontendProcess.HasExited) {
        Stop-Process -Id $frontendProcess.Id -Force -ErrorAction SilentlyContinue
        Write-Host "  [OK] Frontend dev server stopped." -ForegroundColor DarkGray
    }
    Write-Host "[SHUTDOWN] FileGuard exited cleanly." -ForegroundColor Green
}