#requires -Version 5.1
<#
.SYNOPSIS
    Deploy a built w-music into the local workshop: C:\workshop\w-music-<version>\.

.DESCRIPTION
    The workshop keeps one folder per release, named <name>-<version> (aura-1.2.6,
    aw-qtui-0.1.28, quire-0.1.1), holding exactly what that build needs to run.
    For w-music that is more than the exe: the app is unpackaged and
    framework-dependent, so every file the process loads at startup has to sit
    beside it -- see the payload table in step 2.

    The version comes from version.txt at the repo root, and this script does not
    bump it on the deploy pass: `just workshop-deploy` bumps first (-BumpOnly),
    builds, and only then calls this script to copy. The order matters, because
    tools\dev-build.ps1 bakes version.txt into the exe as a VERSIONINFO resource
    (build\obj\version.rc) -- "build first, bump after" would leave
    C:\workshop\w-music-<version> holding a binary that reports <version-1>, and
    nothing afterwards could tell those two builds apart.

    User data is not part of the deploy: library.json / settings.json / providers
    live in %LOCALAPPDATA%\w-music (src\w-music\Services\AppPaths.cpp), so a new
    build folder neither carries nor overwrites them.

.PARAMETER BumpOnly
    Advance version.txt by one patch (0.1.0 -> 0.1.1), mirror the new number into
    the package/assembly manifests, and stop -- no copying. Called before building.

.NOTES
    Usage (the order is the point):
        pwsh -File tools\workshop-deploy.ps1 -BumpOnly   # 1) before the build
        pwsh -File build.ps1 -Release                    # 2) build that version
        pwsh -File tools\workshop-deploy.ps1             # 3) copy + self-check
        Start-Process C:\workshop\w-music-<ver>\w-music.exe
#>
param(
    [switch]$BumpOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root        = Split-Path -Parent $PSScriptRoot
$BuildDir    = Join-Path $Root 'build'
$VersionFile = Join-Path $Root 'version.txt'
$AppName     = 'w-music'
$Workshop    = 'C:\workshop'

function Step([string]$message) {
    Write-Host "== $message" -ForegroundColor Cyan
}

function Get-AppVersion {
    # version.txt is parsed strictly on purpose: it is the only place a release
    # version is typed, and both the embedded VERSIONINFO resource and the
    # workshop folder name read it.
    if (-not (Test-Path $VersionFile)) { throw "version.txt not found: $VersionFile" }
    $text = (Get-Content $VersionFile -Raw).Trim()
    if ($text -notmatch '^\d+\.\d+\.\d+$') {
        throw "version.txt must hold a plain x.y.z version, got '$text'"
    }
    return $text
}

function Get-NextPatch([string]$version) {
    $parts = $version.Split('.')
    return '{0}.{1}.{2}' -f $parts[0], $parts[1], ([int]$parts[2] + 1)
}

function Test-VersionInfo([object]$VersionInfo, [string]$Version) {
    $reported = @([string]$VersionInfo.FileVersion, [string]$VersionInfo.FileVersionRaw)
    return [bool](($reported -contains $Version) -or ($reported -contains "$Version.0"))
}

function Assert-ReleaseBuild {
    $configFile = Join-Path $BuildDir 'config.txt'
    if (-not (Test-Path -LiteralPath $configFile)) {
        throw "missing build configuration marker: $configFile"
    }
    $config = (Get-Content -LiteralPath $configFile -Raw).Trim()
    if ($config -ne 'Release') {
        throw "build is not Release ($config); run '.\\build.ps1 -Release' first"
    }
    $ninjaFile = Join-Path $BuildDir 'build.ninja'
    if (-not (Test-Path -LiteralPath $ninjaFile)) {
        throw "missing build definition: $ninjaFile"
    }
    if (-not ([IO.File]::ReadAllText($ninjaFile)).Contains('/O2 /Oi /Gy /DNDEBUG')) {
        throw "build definition has no Release flags: $ninjaFile"
    }
}

function Get-RunningApp {
    # A running w-music per object, with Path already read: .Path throws for
    # processes this user may not inspect, and reading it behind a try here keeps
    # every call site free of that dance.
    $result = @()
    foreach ($process in (Get-Process -Name $AppName -ErrorAction SilentlyContinue)) {
        $path = ''
        try { $path = [string]$process.Path } catch { $path = '' }
        $result += [pscustomobject]@{ Id = $process.Id; Path = $path }
    }
    return $result
}

function Set-ManifestVersion {
    # Mirrors the release version into the MSIX / assembly identity so the
    # declared identity cannot drift away from version.txt. Both files are
    # MSIX-only metadata (this repo builds the unpackaged path), which is exactly
    # why they are easy to forget -- do it here rather than by hand. The 4th field
    # of an identity is a revision, not a release number, so it stays 0.
    #
    # The lookbehind keeps the match off MinVersion / MaxVersionTested, which also
    # end in "Version=".
    param([string]$Path, [string]$Version)

    if (-not (Test-Path $Path)) { return }
    $text = [IO.File]::ReadAllText($Path)
    $updated = $text -replace '(?<![\w])([Vv]ersion=)"\d+\.\d+\.\d+\.\d+"', ('${1}"' + $Version + '.0"')
    if ($updated -eq $text) { return }
    [IO.File]::WriteAllText($Path, $updated)
    Write-Host "  identity : $([IO.Path]::GetFileName($Path)) -> $Version.0"
}

# ---------------------------------------------------------------------------
# 1. version
# ---------------------------------------------------------------------------
$version = Get-AppVersion

if ($BumpOnly) {
    $next = Get-NextPatch $version
    Step "bumping: $version -> $next"
    [IO.File]::WriteAllText($VersionFile, "$next`n", (New-Object System.Text.UTF8Encoding($false)))
    Set-ManifestVersion (Join-Path $Root 'src\w-music\Package.appxmanifest') $next
    Set-ManifestVersion (Join-Path $Root 'src\w-music\app.manifest') $next
    Write-Host "  [ver]    version.txt and the manifests read $next (bumped before the build)"
    return
}

# ---------------------------------------------------------------------------
# 2. payload
# ---------------------------------------------------------------------------
$dest = Join-Path $Workshop "$AppName-$version"
Step "deploying $AppName $version -> $dest"
Assert-ReleaseBuild

# Files, not "the exe". An unpackaged WinUI 3 app resolves all of this relative to
# its own directory -- there is no package to read it from -- so the folder has to
# hold a copy of each or the process dies before any w-music code runs:
#
#   w-music.exe                                 the app itself
#   Microsoft.WindowsAppRuntime.Bootstrap.dll   imported by name at load time; the
#                                               OS only searches the app directory
#                                               and the system paths, never the
#                                               NuGet cache it came from
#   app.ico                                     window + tray icon (MainWindow.cpp
#                                               and TrayIcon.cpp load it by path)
#   w_music\*.xbf                               compiled XAML: every generated
#                                               InitializeComponent() loads
#                                               ms-appx:///w_music/<Page>.xaml, and
#                                               ms-appx:/// *is* the exe directory
#
# adapters\ is deliberately not copied: the app scans <exe>\adapters for *.json
# (OnlineProviderService.cpp), so a folder holding the *.json.example templates
# would only be noise. Real adapters live in %LOCALAPPDATA%\w-music\providers, or
# wherever WMUSIC_PROVIDER_DIR points -- outside the build, so they survive a
# redeploy.
$payloadFiles = @('w-music.exe', 'Microsoft.WindowsAppRuntime.Bootstrap.dll', 'app.ico')
$payloadDirs = @('w_music')

foreach ($name in ($payloadFiles + $payloadDirs)) {
    $source = Join-Path $BuildDir $name
    if (-not (Test-Path -LiteralPath $source)) {
        throw "missing build output: $source -- run 'just build' (debug) or '.\build.ps1 -Release' first"
    }
}

$xbfDir = Join-Path $BuildDir 'w_music'
if (@(Get-ChildItem -LiteralPath $xbfDir -Filter '*.xbf' -File).Count -eq 0) {
    throw "missing compiled XAML files: $xbfDir"
}

# A build that predates version.txt never picked up this version, and deploying it
# would file a binary reporting <version-1> under w-music-<version> -- the one
# thing these numbered folders exist to prevent. Checked *before* copying, and
# hard, because "the exe is not what the folder claims" is not a detail to warn
# about while launching it. (A skipped/failed build is how it happens: a failed
# one cannot get this far, since build.ps1 propagates dev-build's exit code, but
# an edit to version.txt followed by no build can.)
$built = Get-Item -LiteralPath (Join-Path $BuildDir 'w-music.exe')
$stamped = Get-Item -LiteralPath $VersionFile
if ($built.LastWriteTimeUtc -lt $stamped.LastWriteTimeUtc) {
    throw "build\w-music.exe ($($built.LastWriteTime)) is older than version.txt ($($stamped.LastWriteTime)) -- this build did not pick up v$version. Rebuild first (.gitignore'd build\ is fine to reuse): .\build.ps1 -Release, or use `just workshop-deploy`."
}
if (-not (Test-VersionInfo $built.VersionInfo $version)) {
    throw "build\w-music.exe reports $($built.VersionInfo.FileVersion), not $version; rebuild with .\build.ps1 -Release"
}

$inside = @()
if (Test-Path -LiteralPath $dest) {
    $inside = @(Get-RunningApp | Where-Object { $_.Path -like "$dest\*" })
    if ($inside.Count -gt 0) {
        throw "$AppName is running from $dest (pid $(($inside | ForEach-Object { $_.Id }) -join ', ')) -- exit it from the tray icon, then deploy again"
    }
    Write-Host "  replacing $dest (same version deployed before)"
}

[IO.Directory]::CreateDirectory($Workshop) | Out-Null
$token = [Guid]::NewGuid().ToString('N')
$staging = Join-Path $Workshop ".w-music-$version-$token.staging"
$backup = Join-Path $Workshop ".w-music-$version-$token.backup"
$deployed = $false
try {
    [IO.Directory]::CreateDirectory($staging) | Out-Null
    foreach ($name in $payloadFiles) { Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination (Join-Path $staging $name) -Force }
    foreach ($name in $payloadDirs) { Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination (Join-Path $staging $name) -Recurse -Force }

    $stagedExe = Join-Path $staging 'w-music.exe'
    if (-not (Test-VersionInfo (Get-Item -LiteralPath $stagedExe).VersionInfo $version)) {
        throw "staged exe reports $((Get-Item -LiteralPath $stagedExe).VersionInfo.FileVersion), not $version"
    }
    if (@(Get-ChildItem -LiteralPath (Join-Path $staging 'w_music') -Filter '*.xbf' -File).Count -eq 0) {
        throw "staged deployment has no XAML files"
    }

    if (Test-Path -LiteralPath $dest) { [IO.Directory]::Move($dest, $backup) }
    [IO.Directory]::Move($staging, $dest)
    $deployed = $true
    if (Test-Path -LiteralPath $backup) { [IO.Directory]::Delete($backup, $true) }
}
catch {
    if (-not $deployed -and (Test-Path -LiteralPath $staging)) {
        try { [IO.Directory]::Delete($staging, $true) } catch {}
    }
    if (-not $deployed -and (Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $dest)) {
        try { [IO.Directory]::Move($backup, $dest) } catch {}
    }
    throw
}

$exe = Join-Path $dest 'w-music.exe'
$versionInfo = (Get-Item -LiteralPath $exe).VersionInfo
if (-not (Test-VersionInfo $versionInfo $version)) {
    throw "$exe reports $($versionInfo.FileVersion), not $version"
}

$running = @(Get-RunningApp)
if ($running.Count -gt 0) {
    $pids = ($running | ForEach-Object { $_.Id }) -join ', '
    Write-Host "  [note] $AppName is already running (pid $pids); launching this build takes over:" -ForegroundColor Yellow
    Write-Host '         the old instance is asked to exit gracefully (single-instance hand-off by' -ForegroundColor Yellow
    Write-Host '         exe path, from 0.1.22 on; older builds ignore the request -- exit those' -ForegroundColor Yellow
    Write-Host '         from the tray once and the hand-off is automatic from then on).' -ForegroundColor Yellow
}

$pages = @(Get-ChildItem -LiteralPath (Join-Path $dest 'w_music') -Filter '*.xbf' -File).Count
Write-Host ("  [done] {0} ({1:N1} MiB, v{2}, Release)" -f $exe, ((Get-Item -LiteralPath $exe).Length / 1MB), $version) -ForegroundColor Green
Write-Host "         w_music\ ($pages xbf) + Microsoft.WindowsAppRuntime.Bootstrap.dll + app.ico"
Write-Host "         user data stays in $env:LOCALAPPDATA\$AppName (library.json, settings.json, providers)"

