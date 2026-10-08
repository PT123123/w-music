#requires -Version 5.1
<#
.SYNOPSIS
    Build and run tools\pick_thread_probe.cpp -- the thread proof behind the
    "add music folder" freeze fix.

.DESCRIPTION
    Reuses build\toolenv.json (the cached Enter-VsDevShell result that
    dev-build.ps1 writes) so no 40-50s dev-shell pass is paid here. The probe is
    a plain Win32 console program: it creates a real owner window on its main
    thread, shows IFileOpenDialog(FOS_PICKFOLDERS) from that thread and then from
    a separate STA thread, and asserts with the OS's own detector
    (IsHungAppWindow) plus a WM_TIMER heartbeat that only the first configuration
    freezes the owner thread.

    Kill process is bounded: the probe is watched with a timeout and killed by
    pid if it overshoots (a dialog left open would otherwise hang this script).

.NOTES
    Usage:  pwsh -NoProfile -File tools\pick-thread-probe.ps1
#>
param(
    [int]$TimeoutSeconds = 150
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$src = Join-Path $Root 'tools\pick_thread_probe.cpp'
$exe = Join-Path $BuildDir 'pick_thread-probe.exe'
$objDir = Join-Path $BuildDir 'probe-obj'

$toolEnv = Join-Path $BuildDir 'toolenv.json'
if (-not (Test-Path $toolEnv)) { throw "no $toolEnv -- run 'just build' once first" }
$saved = Get-Content $toolEnv -Raw | ConvertFrom-Json
foreach ($name in @('PATH', 'INCLUDE', 'LIB', 'LIBPATH')) {
    if (-not $saved.$name) { throw "toolenv cache missing $name" }
    Set-Item -Path "env:$name" -Value ([string]$saved.$name)
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe not on PATH' }

New-Item -ItemType Directory -Force -Path $objDir | Out-Null
Push-Location $objDir
try {
    & cl.exe /nologo /std:c++20 /EHsc /W3 /MT /DUNICODE /D_UNICODE `
        /Fo"$objDir\" "$src" /Fe"$exe" /link /SUBSYSTEM:CONSOLE
    if ($LASTEXITCODE -ne 0) { throw "probe compile failed (exit $LASTEXITCODE)" }
}
finally { Pop-Location }

if (-not (Test-Path $exe)) { throw "probe exe missing: $exe" }
Write-Host "probe   : $exe (mtime $((Get-Item $exe).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')))"

$stdout = Join-Path $BuildDir 'pick_thread-probe.log'
$process = Start-Process -FilePath $exe -WorkingDirectory $BuildDir `
    -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $BuildDir 'pick_thread-probe.err') `
    -PassThru -WindowStyle Hidden
if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    Write-Host "probe overshot ${TimeoutSeconds}s; killing pid $($process.Id)" -ForegroundColor Red
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Get-Content $stdout -Raw -ErrorAction SilentlyContinue | Write-Host
    throw 'probe timed out'
}

Get-Content $stdout -Raw | Write-Host
Write-Host "exit code: $($process.ExitCode)"
if ($process.ExitCode -ne 0) { throw "probe failed (exit $($process.ExitCode))" }
