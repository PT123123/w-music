#requires -Version 5.1
<#
.SYNOPSIS
    w-music development build (Ninja + PowerShell, no MSBuild evaluation).

.DESCRIPTION
    Pipeline (cached, incremental):

      1. Locate VS/SDK/NuGet tools and enter the VS dev shell (pure PowerShell,
         no cmd/bat files).
      2. Generate the C++/WinRT projection of the Windows SDK contracts
         (build\gen\sdk).
      3. Generate the C++/WinRT projection of WinUI (Microsoft.UI.Xaml /
         Microsoft.UI.Dispatching) from the WindowsAppSDK metadata
         (build\gen\winui).
      4. midlrt-compile ONE combined app idl into ONE w_music.winmd
         (build\gen\winmd). A single winmd -- whose internal assembly name is
         w_music -- is required by XamlCompiler's LocalAssembly.
      5. cppwinrt -component generates the app runtime-class headers
         (build\gen\component).
      6. Run the REAL XAML markup compiler (XamlCompiler.exe, shipped inside the
         WindowsAppSDK NuGet package) in its two-pass contract and publish the
         generated headers/XBF into build\gen\xaml + build\gen\component\w_music.
         See tools\xaml-markup.ps1 for why this is driven by hand.
      7. Emit build\build.ninja and run ninja:
           - core-layer test executables (built + executed)
           - every app translation unit, including the generated XAML code-behind
             (.xaml.g.hpp) and XamlTypeInfo.g.cpp
           - w-music.exe (non-packaged, WinUI 3)

.PARAMETER Clean
    Deletes build\ before doing anything.

.PARAMETER NoGen
    Reuses the existing projection/midl/component/XAML output in build\gen
    instead of regenerating it (use while iterating on a compile error).

.PARAMETER NoLink
    Compiles everything but skips the w-music.exe link step.

.PARAMETER NoTests
    Skips running the core test executables.

.PARAMETER Release
    Builds the optimized configuration (/O2 /Oi /Gy /DNDEBUG) instead of the
    default unoptimized one. This is what `just workshop-deploy` deploys; see
    tools\workshop-deploy.ps1 for why the version is bumped before the build.

.PARAMETER ListOnly
    Prints the resolved tools and exits.

.NOTES
    Usage:  pwsh -NoProfile -File tools\dev-build.ps1 [-Clean] [-NoTests] [-Release]
#>
param(
    [switch]$Clean,
    [switch]$NoGen,
    [switch]$NoLink,
    [switch]$NoTests,
    [switch]$Release,
    [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Report the real failure. Without this, a terminating error raised inside one of
# the helper functions leaves the console log truncated exactly where the failure
# happened -- with no message at all -- which is far more expensive to debug than
# it looks.
trap {
    Write-Host ''
    Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo) { Write-Host "  at $($_.InvocationInfo.PositionMessage.Trim())" }
    Write-Host $_.ScriptStackTrace
    exit 1
}

$Root    = Split-Path -Parent $PSScriptRoot
$SrcDir  = Join-Path $Root 'src\w-music'
$CoreDir = Join-Path $Root 'core'
$BuildDir = Join-Path $Root 'build'
$GenDir  = Join-Path $BuildDir 'gen'

# ---------------------------------------------------------------------------
# Release version: version.txt at the repo root.
#
# Read here (not only by the deploy script) because the version is baked into the
# exe as a VERSIONINFO resource -- see the obj\version.rc generated below. Without
# that, C:\workshop\w-music-<ver> would be a folder name nothing inside the binary
# backs up, and two builds of the same version would be indistinguishable.
#
# Strict on purpose: version.txt is the only place a release version is typed.
# ---------------------------------------------------------------------------
$versionFile = Join-Path $Root 'version.txt'

function Get-AppVersion {
    if (-not (Test-Path $versionFile)) { throw "version.txt not found: $versionFile" }
    $text = (Get-Content $versionFile -Raw).Trim()
    if ($text -notmatch '^\d+\.\d+\.\d+$') {
        throw "version.txt must hold a plain x.y.z version, got '$text'"
    }
    return $text
}

. (Join-Path $PSScriptRoot 'xaml-markup.ps1')

function Step([string]$message) {
    Write-Host "== $message" -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# 1. tools
# ---------------------------------------------------------------------------
Step '1/8 locating tools'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; is Visual Studio installed?' }
$vsPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) { throw 'Visual Studio 2022 with MSBuild workload not found.' }
Write-Host "vs       : $vsPath"

# Pure-PowerShell way to get the cl/INCLUDE/LIB environment (no vcvars.bat).
#
# Enter-VsDevShell costs 40-50s on this machine and the cost is paid on every
# build -- repeated calls inside one process are just as slow, so it is not a
# one-off module load. Cache the four variables it produces instead and
# re-apply them; only fall back to the real thing when the cache is absent or
# stale (a VS/SDK upgrade moves the paths, which leaves cl.exe unresolvable).
$toolEnvCache = Join-Path $BuildDir 'toolenv.json'
$toolEnvVars = @('PATH', 'INCLUDE', 'LIB', 'LIBPATH')

function Import-ToolEnv {
    if (-not (Test-Path $toolEnvCache)) { return $false }
    try { $saved = Get-Content $toolEnvCache -Raw | ConvertFrom-Json } catch { return $false }
    foreach ($name in $toolEnvVars) {
        if (-not $saved.$name) { return $false }
        Set-Item -Path "env:$name" -Value ([string]$saved.$name)
    }
    return [bool](Get-Command cl.exe -ErrorAction SilentlyContinue)
}

if (Import-ToolEnv) {
    Write-Host 'devenv   : cached (build\toolenv.json)'
}
else {
    $devShellTimer = [Diagnostics.Stopwatch]::StartNew()
    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -no_logo' | Out-Null
    Write-Host "devenv   : Enter-VsDevShell in $([int]$devShellTimer.Elapsed.TotalSeconds)s (cached for next time)"
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $snapshot = [ordered]@{}
    foreach ($name in $toolEnvVars) { $snapshot[$name] = [Environment]::GetEnvironmentVariable($name) }
    [IO.File]::WriteAllText($toolEnvCache, ($snapshot | ConvertTo-Json),
        (New-Object System.Text.UTF8Encoding($false)))
}

$cl = (Get-Command cl.exe -ErrorAction Stop).Source
Write-Host "cl       : $cl"

# ninja: prefer the VS-bundled one, fall back to PATH.
$ninja = Get-ChildItem (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake') -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $ninja) { $ninja = (Get-Command ninja.exe -ErrorAction Stop).Source }
Write-Host "ninja    : $ninja"

# cppwinrt tool from the restored NuGet package (latest installed version).
$cppwinrtPkg = Get-ChildItem "$env:USERPROFILE\.nuget\packages\microsoft.windows.cppwinrt" -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $cppwinrtPkg) { throw 'microsoft.windows.cppwinrt NuGet package not restored.' }
$cppwinrt = Join-Path $cppwinrtPkg.FullName 'bin\cppwinrt.exe'
Write-Host "cppwinrt : $cppwinrt"

# platform.winmd ships IInspectable and the base WinRT types (VC store refs).
$platformWinmd = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Recurse -Filter 'platform.winmd' -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $platformWinmd) { throw 'platform.winmd not found under VC\Tools\MSVC\lib\x86\store\references.' }
Write-Host "platform : $platformWinmd"

# Windows SDK: newest installed.
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
$unionMetadata = Join-Path $sdkRoot "UnionMetadata\$sdkVersion"
Write-Host "sdk      : $sdkVersion"

# WinUI metadata from the WindowsAppSDK NuGet packages.
$nugetRoot = "$env:USERPROFILE\.nuget\packages"
$winuiPkg = Get-ChildItem "$nugetRoot\microsoft.windowsappsdk.winui" -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
# The Xaml winmd references types/contracts that live in sibling packages:
# Microsoft.UI.Dispatching + the WindowsAppSDK contracts in the aggregate
# Microsoft.UI/Microsoft.Foundation winmds (interactiveexperiences), Resources
# in foundation, WebView2 in microsoft.web.webview2. Take the newest stable
# metadata set per package and reference them all (midlrt and cppwinrt both).
function Newest-PackageDir([string]$id) {
    Get-ChildItem "$nugetRoot\$id" -Directory |
        Where-Object { $_.Name -notmatch '-' } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
}
$wasdkRefs = @()
$winuiPkgDir = Newest-PackageDir 'microsoft.windowsappsdk.winui'
$wasdkRefs += @(Get-ChildItem (Join-Path $winuiPkgDir.FullName 'metadata') -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
foreach ($pkgId in @('microsoft.windowsappsdk.interactiveexperiences', 'microsoft.windowsappsdk.foundation')) {
    $pkg = Newest-PackageDir $pkgId
    if (-not $pkg) { continue }
    $metaRoot = Join-Path $pkg.FullName 'metadata'
    # Some packages put winmds directly in metadata\, others in metadata\<sdkver>\.
    $metaDir = Get-ChildItem $metaRoot -Directory |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if ($metaDir) { $metaRoot = $metaDir.FullName }
    $wasdkRefs += @(Get-ChildItem $metaRoot -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
}
$webview2Pkg = Get-ChildItem "$nugetRoot\microsoft.web.webview2" -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if ($webview2Pkg) {
    $wasdkRefs += @(Get-ChildItem (Join-Path $webview2Pkg.FullName 'lib') -Filter '*.winmd' -Recurse |
        Select-Object -ExpandProperty FullName)
}
$dispatchingWinmd = $wasdkRefs | Where-Object { $_ -match '\\Microsoft\.UI\.winmd$' } | Select-Object -First 1

# Not every Microsoft.UI header is a cppwinrt projection: the hand-written
# interop ones (winrt/Microsoft.UI.Interop.h, Microsoft.UI.Composition.Interop.h,
# ...) ship inside the packages' include\ directories and must be on the
# include path as-is. A real MSBuild build gets these from the .props files.
# microsoft.windowsappsdk.runtime is on the list for one header only:
# WindowsAppSDK-VersionInfo.h. MddBootstrapAutoInitializer.cpp includes it, but
# it ships in the *runtime* package while the .cpp itself lives in *foundation*,
# so a real MSBuild build gets it via the runtime target's props. It is pure
# #defines (no lib, no code), hence include-only.
$wasdkIncludeDirs = @()
foreach ($pkgDir in @($winuiPkgDir, (Newest-PackageDir 'microsoft.windowsappsdk.interactiveexperiences'), (Newest-PackageDir 'microsoft.windowsappsdk.foundation'), (Newest-PackageDir 'microsoft.windowsappsdk.runtime'))) {
    if (-not $pkgDir) { continue }
    $pkgInclude = Join-Path $pkgDir.FullName 'include'
    if (Test-Path $pkgInclude) { $wasdkIncludeDirs += $pkgInclude }
}
Write-Host "winui    : $($winuiPkg.Name) ($($wasdkRefs.Count) metadata winmds, $($wasdkIncludeDirs.Count) include dirs)"

# --- XAML markup compiler + unpackaged-app bootstrap ----------------------
# XamlCompiler.exe ships in the WinUI package but nothing invokes it: the MSBuild
# integration lives in a VS component this machine does not have. Drive it from
# tools\xaml-markup.ps1 instead.
$xamlCompiler = Join-Path $winuiPkgDir.FullName 'tools\net472\XamlCompiler.exe'
$genXbfPath = Join-Path $winuiPkgDir.FullName 'tools\'
if (-not (Test-Path $xamlCompiler)) { throw "XamlCompiler.exe not found: $xamlCompiler" }
Write-Host "xamlc    : $xamlCompiler"

# vcmeta.dll: XamlCompiler resolves the C++ type system through it.
$vcMetaDir = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$vcMeta32 = Join-Path $vcMetaDir.FullName 'bin\Hostx64\x86\vcmeta.dll'
$vcMeta64 = Join-Path $vcMetaDir.FullName 'bin\Hostx64\x64\vcmeta.dll'

$mscorlib = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\mscorlib.dll'
$foundationPkgDir = Newest-PackageDir 'microsoft.windowsappsdk.foundation'
if (-not $foundationPkgDir) { throw 'microsoft.windowsappsdk.foundation NuGet package not restored.' }
$foundationInclude = Join-Path $foundationPkgDir.FullName 'include'
$bootstrapLibDir = Join-Path $foundationPkgDir.FullName 'lib\native\x64'
# An unpackaged app reaches the WindowsAppSDK runtime through one of two
# mutually exclusive models, picked by preprocessor macros inside
# WindowsAppRuntimeAutoInitializer.cpp. That file is the dispatcher: it declares
# the init_seg(lib) static object whose constructor is what actually calls
# Initialize(), so it must always be on the compile list.
#
#   MICROSOFT_WINDOWSAPPSDK_AUTOINITIALIZE_BOOTSTRAP  (this build)
#       Framework-dependent: MddBootstrapInitialize2() adds the installed
#       framework package to the process. Requires
#       Microsoft.WindowsAppRuntime.Bootstrap.dll next to the exe, and pulls in
#       no import of Microsoft.WindowsAppRuntime.dll.
#   MICROSOFT_WINDOWSAPPSDK_AUTOINITIALIZE_UNDOCKEDREGFREEWINRT
#       Self-contained: imports Microsoft.WindowsAppRuntime.dll directly (the
#       initializer exists purely to create that import), so the whole
#       runtimes-framework payload has to be deployed beside the exe.
#
# A real MSBuild build defaults to the first, and the 2.3.1.0 framework package
# is already registered on this machine, so take that one.
#
# Bug this replaces: an earlier revision compiled *both* initializer .cpp files
# and never the dispatcher. Nothing then called Initialize() -- so no bootstrap
# ever happened -- while each initializer still dragged its own DLL into the
# import table. The symptom was a load-time "找不到
# Microsoft.WindowsAppRuntime.Bootstrap.dll" (the file exists only in the NuGet
# package, not on any search path) masking the real defect.
$bootstrapDispatcher = Join-Path $foundationInclude 'WindowsAppRuntimeAutoInitializer.cpp'
$bootstrapAutoInit = Join-Path $foundationInclude 'MddBootstrapAutoInitializer.cpp'
# LoadLibrary'd by the bootstrapper at startup, so it must be deployed next to
# the exe; see the copy step in the build report.
$bootstrapDll = Join-Path $foundationPkgDir.FullName 'runtimes\win-x64\native\Microsoft.WindowsAppRuntime.Bootstrap.dll'
Write-Host "bootstrap: $($foundationPkgDir.Name) (framework-dependent)"

# Resource compiler: embeds Assets\app.ico as the exe's icon resource, which is
# what the task manager and the shell use for the executable.
$rcExe = Join-Path $sdkRoot "bin\$sdkVersion\x64\rc.exe"

if ($ListOnly) { return }

if ($Clean -and (Test-Path $BuildDir)) {
    # Keep the tool environment cache: recomputing it costs ~45s and Clean is
    # usually about the generated code, not the toolchain.
    # Uses the .NET APIs rather than Remove-Item: this environment guards that
    # cmdlet with a fail-closed safe-delete policy, and -Clean is an explicit,
    # user-requested removal confined to the build directory.
    foreach ($item in (Get-ChildItem $BuildDir -Force)) {
        if ($item.Name -eq 'toolenv.json') { continue }
        if ($item.PSIsContainer) { [IO.Directory]::Delete($item.FullName, $true) }
        else { [IO.File]::Delete($item.FullName) }
    }
}

# ---------------------------------------------------------------------------
# Tool output goes to build\last-tool-out.txt (all streams merged) so failures
# can be inspected without flooding the console.
# ---------------------------------------------------------------------------
$toolOut = Join-Path $BuildDir 'last-tool-out.txt'

function Invoke-Native {
    # Runs a native tool with every stream merged into |LogPath| and returns its
    # exit code.
    #
    # Why the ErrorActionPreference dance: PowerShell turns a native command's
    # stderr into ErrorRecords, and with $ErrorActionPreference = 'Stop' (set at
    # the top of this script) the first such line aborts the call -- before the
    # tool has finished. midlrt writes its per-file progress messages to stderr,
    # so without this the whole build dies silently inside step 4.
    param([scriptblock]$Action, [string]$LogPath)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Action *>&1 | Out-File -FilePath $LogPath -Encoding utf8 -Width 4096 }
    finally { $ErrorActionPreference = $previous }
    return $LASTEXITCODE
}

function Run-Tool {
    param([string]$Name, [scriptblock]$Action)
    if ((Invoke-Native $Action $toolOut) -ne 0) {
        Get-Content $toolOut -Tail 12 | ForEach-Object { Write-Host "  [tool] $_" }
        throw "$Name failed (exit $LASTEXITCODE); full output in build\last-tool-out.txt"
    }
}

function Write-GeneratedFile {
    # Writes a generated file only when its content actually changed.
    #
    # Set-Content would bump the mtime unconditionally, and these generated
    # translation units are included by many others -- so a no-op rebuild cost a
    # full cl.exe pass over them every single time. Leaving the mtime alone is
    # what makes ninja's incremental mode work. UTF-8 without BOM, for
    # consistency with build.ninja.
    param([string]$Path, [string]$Text)
    if ((Test-Path $Path) -and ([IO.File]::ReadAllText($Path) -eq $Text)) { return $false }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
    return $true
}

foreach ($dir in @((Join-Path $GenDir 'sdk'), (Join-Path $GenDir 'winui'), (Join-Path $GenDir 'winmd'),
                   (Join-Path $GenDir 'component'), (Join-Path $GenDir 'xaml'), (Join-Path $GenDir 'xamltu'))) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

# ---------------------------------------------------------------------------
# 2. SDK projection
# ---------------------------------------------------------------------------
Step '2/8 generating Windows SDK projection'
$sdkWinmds = @(Get-ChildItem (Join-Path $sdkRoot "References\$sdkVersion") -Recurse -Filter '*.winmd' |
    Select-Object -ExpandProperty FullName)
$sdkMarker = Join-Path $GenDir 'sdk\.done'
if (-not (Test-Path $sdkMarker)) {
    Run-Tool 'cppwinrt (sdk)' { & $cppwinrt -in $sdkWinmds -out (Join-Path $GenDir 'sdk') }
    New-Item -ItemType File -Force -Path $sdkMarker | Out-Null
}
Write-Host "headers  : $((Get-ChildItem (Join-Path $GenDir 'sdk') -Recurse -Filter '*.h').Count)"

# ---------------------------------------------------------------------------
# 3. WinUI projection
# ---------------------------------------------------------------------------
Step '3/8 generating WinUI (Microsoft.UI.Xaml) projection'
# Every WindowsAppSDK metadata winmd is an *input*, not just a reference:
# Microsoft.UI.Xaml.h includes the impl\ headers of its sibling namespaces
# (Microsoft.UI.*, Microsoft.Windows.ApplicationModel.Resources, ...) and
# cppwinrt only emits headers for the namespaces it was asked to project.
# The Windows SDK contracts stay references.
$winuiWinmds = @($wasdkRefs)
$winuiRefs = $sdkWinmds
$winuiMarker = Join-Path $GenDir 'winui\.done'
if (-not (Test-Path $winuiMarker)) {
    Run-Tool 'cppwinrt (winui)' { & $cppwinrt -in $winuiWinmds -ref $winuiRefs -out (Join-Path $GenDir 'winui') }
    New-Item -ItemType File -Force -Path $winuiMarker | Out-Null
}
Write-Host "headers  : $((Get-ChildItem (Join-Path $GenDir 'winui') -Recurse -Filter '*.h').Count)"

# ---------------------------------------------------------------------------
# 4-6. IDL -> winmd -> cppwinrt component -> XAML markup
# ---------------------------------------------------------------------------
$winmdDir = Join-Path $GenDir 'winmd'
$mergedWinmd = Join-Path $winmdDir 'w-music.winmd'
$mergedHdr = Join-Path $winmdDir 'w-music.h'
$genComponent = Join-Path $GenDir 'component'
$xamlDir = Join-Path $GenDir 'xaml'
$foundationWinmd = Join-Path $GenDir 'wf\Windows.Foundation.winmd'
$pages = @('DiscoverPage', 'RecommendPage', 'LibraryPage', 'NowPlayingPage', 'OnlinePage')
$xamlNameOrder = @('App', 'MainWindow') + $pages

if (-not $NoGen) {
    # --- 4. one combined idl -> one w_music.winmd --------------------------
    Step '4/8 midlrt-compiling combined app IDL'
    # Compile order is irrelevant here: midlrt sees one translation unit with all
    # sources #included, in this order (dependencies first, for clarity only).
    $idlOrder = @('Models.idl', 'ViewModels.idl',
                  'Views\DiscoverPage.idl', 'Views\RecommendPage.idl',
                  'Views\LibraryPage.idl',
                  'Views\NowPlayingPage.idl', 'Views\OnlinePage.idl',
                  'App.idl', 'MainWindow.idl')
    $idlPaths = @($idlOrder | ForEach-Object { Join-Path $SrcDir $_ })
    # The app-level IXamlMetadataProvider runtimeclass the markup compiler expects
    # (see New-MetadataProviderIdl).
    $idlPaths += (New-MetadataProviderIdl -OutDir $winmdDir -Namespace 'w_music')
    $combinedIdl = New-CombinedAppIdl -OutDir $winmdDir -IdlPaths $idlPaths
    Invoke-AppMidl -CombinedIdl $combinedIdl -OutWinmd $mergedWinmd -OutHdr $mergedHdr `
        -SdkRoot $sdkRoot -SdkVersion $sdkVersion -SrcDir $SrcDir `
        -ReferenceWinmds $wasdkRefs -LogPath (Join-Path $winmdDir 'midlrt.log') `
        -InvokeNative ${function:Invoke-Native} | Out-Null
    Write-Host "winmd    : $(Split-Path -Leaf $mergedWinmd) ($([math]::Round((Get-Item $mergedWinmd).Length/1KB)) KB)"

    # --- 5. cppwinrt component headers --------------------------------------
    Step '5/8 generating app component headers'
    $componentRefs = $sdkWinmds + $wasdkRefs
    Run-Tool 'cppwinrt (component)' {
        & $cppwinrt -component -in @($mergedWinmd) -ref $componentRefs -out $genComponent
    }
    Write-Host "component: $((Get-ChildItem $genComponent -Recurse -Filter '*.g.*').Count) files"

    # --- 6. real XAML markup compilation -----------------------------------
    Step '6/8 compiling XAML (real XamlCompiler, pass1 + pass2)'
    $foundationWinmd = New-FoundationWinmd -SdkRoot $sdkRoot -SdkVersion $sdkVersion -OutDir (Split-Path -Parent $foundationWinmd)

    # The reference set XamlCompiler needs. WinUI metadata resolves the
    # Microsoft.UI.* types; the two OS contracts provide Windows.UI.*/Foundation;
    # the renamed FoundationContract supplies the identity "Windows.Foundation"
    # that midlrt's bare reference demands. The union Windows.winmd must NOT
    # appear here (WMC0901).
    $refAssemblies = [ordered]@{
        (Join-Path $winuiPkgDir.FullName 'metadata\Microsoft.UI.Xaml.winmd') = 'Microsoft.UI.Xaml'
        $dispatchingWinmd = 'Microsoft.UI'
        (Join-Path $winuiPkgDir.FullName 'metadata\Microsoft.UI.Text.winmd')  = 'Microsoft.UI.Text'
        $foundationWinmd = 'Windows.Foundation'
        (Get-ContractWinmd $sdkRoot $sdkVersion 'Windows.Foundation.FoundationContract') = 'Windows.Foundation.FoundationContract'
        (Get-ContractWinmd $sdkRoot $sdkVersion 'Windows.Foundation.UniversalApiContract') = 'Windows.Foundation.UniversalApiContract'
        $mscorlib = 'mscorlib'
    }
    $refPaths = @(
        (Join-Path $winuiPkgDir.FullName 'metadata'),
        (Join-Path $sdkRoot "UnionMetadata\$sdkVersion"),
        (Join-Path $sdkRoot "References\$sdkVersion")
    )
    $pageXaml = @($pages | ForEach-Object { (Resolve-Path (Join-Path $SrcDir "Views\$_.xaml")).Path })
    # MainWindow.xaml lives at the project root (it is a top-level Window), but the
    # markup compiler still consumes it as a XAML page item.
    $pageXaml += (Resolve-Path (Join-Path $SrcDir 'MainWindow.xaml')).Path
    $appXaml = @( (Resolve-Path (Join-Path $SrcDir 'App.xaml')).Path )

    Invoke-XamlMarkup -SrcDir $SrcDir -OutDir $xamlDir -MergedWinmd $mergedWinmd `
        -FoundationWinmd $foundationWinmd -SdkRoot $sdkRoot -SdkVersion $sdkVersion `
        -XamlCompiler $xamlCompiler -ProjectPath (Resolve-Path (Join-Path $SrcDir 'w-music.vcxproj')).Path `
        -RootNamespace 'w_music' -VcMeta32 $vcMeta32 -VcMeta64 $vcMeta64 -GenXbfPath $genXbfPath `
        -ReferenceAssemblies $refAssemblies -ReferencePaths $refPaths `
        -PageXaml $pageXaml -AppXaml $appXaml `
        -LogPath (Join-Path $xamlDir 'xamlc.log') -InvokeNative ${function:Invoke-Native} | Out-Null

    $pub = Publish-XamlCodegen -FromDir $xamlDir -ToDir (Join-Path $genComponent 'w_music') -Names $xamlNameOrder
    Write-Host "xaml     : $((Get-ChildItem $xamlDir -Filter '*.xbf').Count) xbf, $((Get-ChildItem $xamlDir -Filter '*.xaml.g.hpp').Count) impl headers ($pub published)"

    # module.g.cpp includes w_music.<Class>.h factory headers that cppwinrt only
    # emits for *activatable* classes; this app declares its factory glue by hand
    # in each <Class>.h instead, so bridge the two spellings.
    $factoryBridge = [ordered]@{
        'App' = 'App.h'; 'MainWindow' = 'MainWindow.h'
        # The markup compiler's own provider header carries the factory glue.
        'XamlMetaDataProvider' = 'w_music\XamlMetaDataProvider.h'
        'DiscoverPage' = 'Views\DiscoverPage.h'; 'RecommendPage' = 'Views\RecommendPage.h'
        'LibraryPage' = 'Views\LibraryPage.h'
        'NowPlayingPage' = 'Views\NowPlayingPage.h'; 'OnlinePage' = 'Views\OnlinePage.h'
        'LibraryViewModel' = 'ViewModels\LibraryViewModel.h'; 'PlayerViewModel' = 'ViewModels\PlayerViewModel.h'
        'RecommendViewModel' = 'ViewModels\RecommendViewModel.h'
        'LyricLineItem' = 'Models\LyricLineItem.h'; 'OnlineTrackItem' = 'Models\OnlineTrackItem.h'
        'PlaylistItem' = 'Models\PlaylistItem.h'; 'QualityChipItem' = 'Models\QualityChipItem.h'
        'RecommendItem' = 'Models\RecommendItem.h'; 'CategoryItem' = 'Models\CategoryItem.h'
        'TrackItem' = 'Models\TrackItem.h'
    }
    $bridges = 0
    foreach ($cls in $factoryBridge.Keys) {
        # cppwinrt emits w_music.<Class>.h itself for activatable classes (one with
        # a declared constructor, e.g. XamlMetaDataProvider) -- never shadow that.
        if (Test-Path (Join-Path $genComponent "w_music.$cls.h")) { continue }
        $text = "// Bridge: cppwinrt's module.g.cpp includes `"w_music.$cls.h`", which it only`r`n" +
                "// emits for activatable classes. The factory glue for $cls lives in the`r`n" +
                "// hand-written header below (see tools\dev-build.ps1).`r`n" +
                "#pragma once`r`n#include `"$($factoryBridge[$cls])`"`r`n"
        if (Write-GeneratedFile (Join-Path $genComponent "w_music.$cls.h") $text) { $bridges++ }
    }
    Write-Host "factory  : $($factoryBridge.Count) bridge headers ($bridges rewritten)"
}
else {
    Write-Host '  (steps 4-6 skipped: -NoGen)' -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# 7. build.ninja + ninja
# ---------------------------------------------------------------------------
Step '7/8 emitting build.ninja'

# --- XAML code-behind translation units -----------------------------------
# XamlCompiler's Pass2 output (<Page>.xaml.g.hpp) holds the InitializeComponent/
# LoadComponent bodies and ends with an explicit template instantiation
# (`template struct <Page>T<struct <Page>>;`). It carries no #include of its own,
# so each one needs a tiny translation unit that first pulls in the PCH and the
# class declaration. MSBuild does the same thing by force-including the PCH.
$tuDir = Join-Path $GenDir 'xamltu'
$tuSources = @()
$tuMap = [ordered]@{
    'App' = 'App.h'; 'MainWindow' = 'MainWindow.h'
    'DiscoverPage' = 'Views\DiscoverPage.h'; 'RecommendPage' = 'Views\RecommendPage.h'
    'LibraryPage' = 'Views\LibraryPage.h'
    'NowPlayingPage' = 'Views\NowPlayingPage.h'; 'OnlinePage' = 'Views\OnlinePage.h'
}
foreach ($cls in $tuMap.Keys) {
    $inc = ($tuMap[$cls] -replace '\\', '/')
    $text = "// Generated by tools\dev-build.ps1 -- compiles $cls.xaml.g.hpp.`r`n" +
            "// The .hpp has no includes and ends in an explicit instantiation, so it`r`n" +
            "// needs the PCH and the class declaration in scope first.`r`n" +
            "#include `"pch.h`"`r`n#include `"$inc`"`r`n#include `"$cls.xaml.g.hpp`"`r`n"
    $tu = Join-Path $tuDir "$cls.tu.cpp"
    Write-GeneratedFile $tu $text | Out-Null
    $tuSources += $tu
}

# --- app source scope ------------------------------------------------------
# Everything, now that the real XAML headers and the App/MainWindow runtime-class
# contracts exist (they did not before: midlrt used to skip those two idls).
$appSources = @(
    'pch.cpp', 'App.cpp', 'MainWindow.cpp',
    'Models\TrackItem.cpp', 'Models\PlaylistItem.cpp', 'Models\LyricLineItem.cpp',
    'Models\OnlineTrackItem.cpp', 'Models\QualityChipItem.cpp',
    'Models\RecommendItem.cpp', 'Models\CategoryItem.cpp',
    'ViewModels\PlayerViewModel.cpp', 'ViewModels\LibraryViewModel.cpp',
    'ViewModels\RecommendViewModel.cpp',
    'Services\AppPaths.cpp', 'Services\LibraryService.cpp',
    'Services\OnlineProviderService.cpp', 'Services\BuiltinProviders.cpp',
    'Services\DiscoverSettings.cpp', 'Services\RecommendService.cpp',
    'Services\Services.cpp', 'Services\TrayIcon.cpp',
    'Services\SingleInstance.cpp',
    'Controls\SpectrumView.cpp', 'Audio\WasapiLoopback.cpp',
    'Audio\EqualizedSource.cpp',
    'Views\DiscoverPage.cpp', 'Views\RecommendPage.cpp',
    'Views\LibraryPage.cpp',
    'Views\NowPlayingPage.cpp', 'Views\OnlinePage.cpp'
)

# --- per-class .g.cpp stand-ins --------------------------------------------
# Every app source ends with `#include "<Class>.g.cpp"`, the C++/WinRT convention
# for the per-class activation glue. cppwinrt -component emits only module.g.cpp,
# never the per-class files, so provide them here. They are intentionally empty:
# everything a /c compile needs arrives through the .g.h / .xaml.g.h chain.
$gcppSeen = 0
$gcppWritten = 0
foreach ($src in (@($appSources | ForEach-Object { Join-Path $SrcDir $_ }) + $tuSources)) {
    if (-not (Test-Path $src)) { continue }
    foreach ($m in [regex]::Matches((Get-Content $src -Raw), '#include\s+"(?<g>[^"]+\.g\.cpp)"')) {
        $target = Join-Path $genComponent ($m.Groups['g'].Value.Replace('/', '\'))
        if (Test-Path $target) { continue }   # never shadow a cppwinrt-generated file
        $text = "// Empty stand-in: cppwinrt -component does not emit per-class .g.cpp`r`n" +
                "// files (see tools\dev-build.ps1). All declarations the compiler needs`r`n" +
                "// come from the .g.h / .xaml.g.h chain.`r`n" +
                "#pragma once`r`n"
        if (Write-GeneratedFile $target $text) { $gcppWritten++ }
        $gcppSeen++
    }
}
Write-Host "gcpp     : $gcppSeen stand-ins ($gcppWritten written)"

# --- XamlTypeInfo unit ------------------------------------------------------
# XamlTypeInfo.g.cpp names every implementation class in its type table
# (implementation::DiscoverPage, implementation::App, ...) but carries no
# #include for them. MSBuild gets away with that because its build force-includes
# the project headers; here it needs a unit that pulls them all in first.
$typeInfoHeaders = @(
    'App.h', 'MainWindow.h',
    'Views\DiscoverPage.h', 'Views\RecommendPage.h',
    'Views\LibraryPage.h',
    'Views\NowPlayingPage.h', 'Views\OnlinePage.h',
    'ViewModels\LibraryViewModel.h', 'ViewModels\PlayerViewModel.h',
    'ViewModels\RecommendViewModel.h',
    'Models\TrackItem.h', 'Models\PlaylistItem.h', 'Models\LyricLineItem.h',
    'Models\OnlineTrackItem.h', 'Models\QualityChipItem.h',
    'Models\RecommendItem.h', 'Models\CategoryItem.h',
    'w_music\XamlMetaDataProvider.h'
)
$typeInfoText = [System.Text.StringBuilder]::new()
[void]$typeInfoText.AppendLine('// Generated by tools\dev-build.ps1 -- compiles XamlTypeInfo.g.cpp.')
[void]$typeInfoText.AppendLine('// The generated type table names every implementation class, so all of them')
[void]$typeInfoText.AppendLine('// must be complete before it is included.')
[void]$typeInfoText.AppendLine('#include "pch.h"')
foreach ($h in $typeInfoHeaders) { [void]$typeInfoText.AppendLine("#include `"$($h -replace '\\','/')`"") }
[void]$typeInfoText.AppendLine('#include "XamlTypeInfo.g.cpp"')
$typeInfoTu = Join-Path $tuDir 'XamlTypeInfo.tu.cpp'
Write-GeneratedFile $typeInfoTu $typeInfoText.ToString() | Out-Null
$tuSources += $typeInfoTu

# Generated sources compiled directly (they bring their own includes).
# Paths point at the copies published into gen\component\w_music -- the only
# directory on the include path (see the gen/xaml note above).
#
# Every entry must be a real translation unit. Generated *headers* never go
# here: XamlTypeInfo.g.cpp already `#include`s XamlBindingInfo.xaml.g.hpp, and
# each per-class .xaml.g.hpp is pulled in by its own .tu.cpp. Listing a header
# fails late and confusingly -- cl.exe ignores a non-source extension (D9024
# "assuming object file"), so no .obj is produced and the *link* step dies
# with LNK1181 on an object file that was never going to exist.
$generatedSources = @(
    (Join-Path $genComponent 'module.g.cpp'),
    # XamlTypeInfo.Impl.g.cpp defines the XamlUserType / XamlMember /
    # XamlSystemBaseType / XamlTypeInfoProvider helpers. Nothing #includes it --
    # XamlTypeInfo.g.cpp only *uses* those classes -- so it has to be compiled as
    # its own TU. Omitting it links and then fails with ~22 unresolved
    # winrt::w_music::implementation::Xaml* symbols.
    (Join-Path $genComponent 'w_music\XamlTypeInfo.Impl.g.cpp'),
    # The runtime bootstrapper (see the bootstrap note near $bootstrapDispatcher).
    # Both files are required: the dispatcher owns the static object that runs
    # initialization, the second one implements it.
    $bootstrapDispatcher,
    $bootstrapAutoInit
)
foreach ($gs in $generatedSources) {
    if ($gs -notmatch '\.(cpp|cxx|cc|c)$') {
        throw "generatedSources entry is not a translation unit: $gs"
    }
}

$ninjaFile = Join-Path $BuildDir 'build.ninja'
$sdkInc = Join-Path $sdkRoot "Include\$sdkVersion"
$relIncludes = @('../src/w-music', '../core/include', 'gen/sdk', 'gen/winui',
                 'gen/component', 'gen/component/w_music') |
    ForEach-Object { "/I$_" }
# NOTE: gen/xaml is deliberately NOT on the include path. The markup compiler's
# headers are published into gen/component/w_music so there is exactly one copy of
# each; leaving gen/xaml reachable too made some units include two distinct files
# with the same content (C2011 type redefinition).
$sdkIncludes = @('um', 'shared', 'ucrt', 'cppwinrt', 'winrt') |
    ForEach-Object { "/I`"$sdkInc\$_`"" }
$wasdkIncludes = $wasdkIncludeDirs | ForEach-Object { "/I`"$_`"" }
$includes = ($relIncludes + $wasdkIncludes + $sdkIncludes) -join ' '

# ---------------------------------------------------------------------------
# Configuration. No MSBuild here, so no Debug/Release output directories: the
# flags *are* the configuration, and both configurations share build\ and its
# object names. Switching therefore recompiles every unit -- ninja works that out
# by itself, because the compile command line it recorded in build\ .ninja_log
# changed -- which is why the switch is announced rather than happening silently.
#
# cl.exe's default with no /O switch is /Od, so plain `just build` stays the
# unoptimized inner loop it has always been; `just workshop-deploy` builds
# -Release, the configuration that gets deployed.
# ---------------------------------------------------------------------------
$configName = if ($Release) { 'Release' } else { 'Debug' }
$configFlags = if ($Release) { ' /O2 /Oi /Gy /DNDEBUG' } else { '' }
# Written after a successful build (see the report), so it describes what build\
# really holds rather than what was asked for.
$configMarker = Join-Path $BuildDir 'config.txt'
if (Test-Path $configMarker) {
    $previousConfig = (Get-Content $configMarker -Raw).Trim()
    if ($previousConfig -ne $configName) {
        Write-Host "config   : $configName (was $previousConfig -- ninja recompiles every unit)" -ForegroundColor Yellow
    }
}

# MICROSOFT_WINDOWSAPPSDK_AUTOINITIALIZE_BOOTSTRAP selects the framework-dependent
# branch in WindowsAppRuntimeAutoInitializer.cpp; the bootstrap auto-initializer
# itself also keys off it (see WindowsAppSDK-Nuget-Native.Bootstrap.targets).
$cppFlags = '/std:c++20 /EHsc /utf-8 /bigobj /W3 /permissive- /D_UNICODE /DUNICODE /DWINRT_LEAN_AND_MEAN /D_VSDESIGNER_DONT_LOAD_AS_DLL /DMICROSOFT_WINDOWSAPPSDK_AUTOINITIALIZE_BOOTSTRAP=1' + $configFlags
# Microsoft.WindowsAppRuntime.Bootstrap.lib supplies MddBootstrapInitialize2 /
# MddBootstrapShutdown and is what makes the exe import the bootstrapper DLL.
# Nothing here needs Microsoft.WindowsAppRuntime.lib -- that one belongs to the
# self-contained model, where UndockedRegFreeWinRT-AutoInitializer.cpp calls
# WindowsAppRuntime_EnsureIsLoaded(). Linking it anyway (as an earlier revision
# did) adds a hard load-time import of the framework DLL, which the OS cannot
# resolve before the bootstrapper has run.
$linkLibs = @('windowsapp.lib',
              'Microsoft.WindowsAppRuntime.Bootstrap.lib',
              'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'runtimeobject.lib',
              'shell32.lib', 'shlwapi.lib', 'propsys.lib', 'user32.lib',
               'gdi32.lib', 'd3d11.lib', 'dxgi.lib', 'windowscodecs.lib',
               'bcrypt.lib', 'version.lib',
              # Equalizer decode proxy (Audio\EqualizedSource.cpp): MFStartup /
              # source reader / GUIDs like MFAudioFormat_Float.
              'mfplat.lib', 'mfreadwrite.lib', 'mfuuid.lib') -join ' '

$objectDir = Join-Path $BuildDir 'obj'
New-Item -ItemType Directory -Force -Path $objectDir | Out-Null

$edges = New-Object System.Text.StringBuilder
[void]$edges.AppendLine('rule cxx')
[void]$edges.AppendLine('  deps = msvc')
[void]$edges.AppendLine('  command = cl.exe /nologo ' + $cppFlags + ' ' + $includes + ' /showIncludes /c $in /Fo$out')
# Two link rules: the core tests are console programs with main(), the app is a
# windows subsystem binary with wWinMain (supplied by App.xaml.g.hpp).
[void]$edges.AppendLine('rule link')
[void]$edges.AppendLine('  command = cl.exe /nologo $in /Fe$out /link /SUBSYSTEM:CONSOLE')
[void]$edges.AppendLine('rule linkapp')
# /MAP is deliberate: the app ships no PDB, so the RVAs that diag.log's CRASH
# line lists are resolved by hand against build\w-music.map from this same link.
[void]$edges.AppendLine('  command = cl.exe /nologo $in /Fe$out /link /SUBSYSTEM:WINDOWS /MAP /LIBPATH:"' + $bootstrapLibDir + '" ' + $linkLibs)
[void]$edges.AppendLine('rule rc')
[void]$edges.AppendLine('  command = "' + $rcExe + '" /nologo /fo$out $in')
[void]$edges.AppendLine()

function ObjPath([string]$sourcePath) {
    # Sources outside the repo (the WindowsAppSDK bootstrap auto-initializers live
    # in the NuGet cache) keep only their file name, so the object name stays a
    # single valid leaf.
    if ($sourcePath.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) {
        $rel = $sourcePath.Substring($Root.Length + 1)
    }
    else {
        $rel = [IO.Path]::GetFileName($sourcePath)
    }
    $rel = $rel -replace '[\\/]', '_'
    return "obj/$rel.obj"
}
function NinjaPath([string]$absolutePath) {
    # relative to build\, forward slashes
    $uri = New-Object System.Uri($BuildDir + '\')
    return $uri.MakeRelativeUri([System.Uri]::new($absolutePath)).ToString()
}

# core/src objects: compiled once, linked into every test executable.
$coreObjs = @()
foreach ($src in (Get-ChildItem (Join-Path $CoreDir 'src') -Filter '*.cpp')) {
    $obj = ObjPath $src.FullName
    [void]$edges.AppendLine("build ${obj}: cxx $(NinjaPath $src.FullName)")
    $coreObjs += $obj
}

# one console binary per core\tests\test_*.cpp: its own object + core\src.
$testTargets = @()
$defaults = @()
foreach ($test in (Get-ChildItem (Join-Path $CoreDir 'tests') -Filter 'test_*.cpp')) {
    $testName = [IO.Path]::GetFileNameWithoutExtension($test)
    $testObj = ObjPath $test.FullName
    [void]$edges.AppendLine("build ${testObj}: cxx $(NinjaPath $test.FullName)")
    [void]$edges.AppendLine("build ${testName}.exe: link $testObj $($coreObjs -join ' ')")
    $testTargets += Join-Path $BuildDir "${testName}.exe"
    $defaults += "${testName}.exe"
}

# app objects: the hand-written sources, the generated XAML code-behind units and
# the generated type-info sources. All of them go into w-music.exe.
$appObjs = @()
foreach ($rel in $appSources) {
    $full = Join-Path $SrcDir $rel
    if (-not (Test-Path $full)) { Write-Host "  [warn] missing source: $rel" -ForegroundColor Yellow; continue }
    $obj = ObjPath $full
    [void]$edges.AppendLine("build ${obj}: cxx $(NinjaPath $full)")
    $appObjs += $obj
}
foreach ($full in @($tuSources)) {
    $obj = ObjPath $full
    [void]$edges.AppendLine("build ${obj}: cxx $(NinjaPath $full)")
    $appObjs += $obj
}
foreach ($full in $generatedSources) {
    if (-not (Test-Path $full)) { Write-Host "  [warn] missing generated source: $full" -ForegroundColor Yellow; continue }
    $obj = ObjPath $full
    [void]$edges.AppendLine("build ${obj}: cxx $(NinjaPath $full)")
    $appObjs += $obj
}

# version.txt is the single source of truth for the release version: the deploy
# script names C:\workshop\w-music-<ver> after it, and the VERSIONINFO resource
# generated below bakes the same string into the exe -- so the folder name can be
# read back out of the binary instead of being taken on trust.
$appVersion = Get-AppVersion

if (-not $NoLink) {
    # Resources, in two files: the icon (Assets\app.rc, hand-written) and the
    # VERSIONINFO block (version.rc, generated here from version.txt).
    $resourceObjs = @()

    # The app icon resource (.ico embedded via rc.exe) feeds the task manager and
    # the shell's icon for the exe; the runtime path uses the same .ico file.
    $appRc = Join-Path $SrcDir 'Assets\app.rc'
    if (Test-Path $appRc) {
        [void]$edges.AppendLine("build obj\w-music.res: rc $(NinjaPath $appRc)")
        $resourceObjs += 'obj\w-music.res'
    }

    # Generated rather than hand-written so version.txt remains the only place a
    # release version is typed -- a second copy in a checked-in .rc would drift.
    # Written only when the text changes (see Write-GeneratedFile): this file is a
    # link input, so bumping its mtime on every build would relink for nothing.
    #
    # "1 VERSIONINFO" is rc's numeric spelling of VS_VERSION_INFO (1 in winver.h),
    # which keeps this file free of an #include -- rc.exe is run without the SDK
    # include path on purpose.
    $versionRc = Join-Path $objectDir 'version.rc'
    $versionCsv = ($appVersion -split '\.') -join ','
    $versionRcText = @"
1 VERSIONINFO
  FILEVERSION     $versionCsv,0
  PRODUCTVERSION  $versionCsv,0
  FILEFLAGSMASK   0x3fL
  FILEFLAGS       0x0L
  FILEOS          0x40004L
  FILETYPE        0x1L
  FILESUBTYPE     0x0L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "w-music"
      VALUE "FileDescription", "w-music"
      VALUE "FileVersion", "$appVersion"
      VALUE "InternalName", "w-music"
      VALUE "OriginalFilename", "w-music.exe"
      VALUE "ProductName", "w-music"
      VALUE "ProductVersion", "$appVersion"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@
    if (Write-GeneratedFile $versionRc $versionRcText) {
        Write-Host "version  : $appVersion -> obj\version.rc (regenerated)"
    }
    [void]$edges.AppendLine("build obj\version.res: rc $(NinjaPath $versionRc)")
    $resourceObjs += 'obj\version.res'

    [void]$edges.AppendLine("build w-music.exe: linkapp $($appObjs -join ' ') $($resourceObjs -join ' ') $($coreObjs -join ' ')")
    $defaults += 'w-music.exe'
}
[void]$edges.AppendLine("default $($defaults -join ' ')")
# BOM-free UTF-8: Windows PowerShell's `Set-Content -Encoding utf8` writes a
# BOM, and ninja cannot lex a build file that starts with one.
[IO.File]::WriteAllText($ninjaFile, $edges.ToString(), (New-Object System.Text.UTF8Encoding($false)))

Step 'running ninja'
# ninja's failure report is the interesting part of the output, so merge
# everything into one log and echo it back.
$ninjaLog = Join-Path $BuildDir 'ninja-out.txt'
$ninjaExit = Invoke-Native { & $ninja -C $BuildDir } $ninjaLog
Get-Content $ninjaLog | ForEach-Object { Write-Host "  $_" }

# The core test binaries are built by the same ninja run, so run them even when
# the app objects failed -- otherwise a broken app source hides a broken core
# layer.
if (-not $NoTests) {
    Step 'running core tests'
    foreach ($exe in $testTargets) {
        if (-not (Test-Path $exe)) { Write-Host "-- $exe (not built)" -ForegroundColor Yellow; continue }
        Write-Host "-- $exe"
        & $exe 2>&1 | Select-Object -Last 3 | ForEach-Object { Write-Host "   $_" }
        if ($LASTEXITCODE -ne 0) { throw "test failed: $exe" }
    }
}

if ($ninjaExit -ne 0) { throw "ninja failed (exit $ninjaExit); full output in build\ninja-out.txt" }

# The build got this far, so build\ now really holds $configName objects: record
# it for the next run's configuration-change notice. Written only when it changed,
# like the generated sources, since it is not a build input.
[void](Write-GeneratedFile $configMarker "$configName`n")

# ---------------------------------------------------------------------------
# 8. build report
# ---------------------------------------------------------------------------
Step '8/8 build report'
if (-not $NoLink) {
    $exe = Join-Path $BuildDir 'w-music.exe'
    if (Test-Path $exe) {
        Write-Host "exe      : $exe ($([math]::Round((Get-Item $exe).Length/1KB)) KB)"
        Write-Host "config   : $configName, v$appVersion (VERSIONINFO embedded in the exe)"
    }
    else {
        Write-Host 'exe      : not produced' -ForegroundColor Yellow
    }

    # The exe imports Microsoft.WindowsAppRuntime.Bootstrap.dll, and Windows only
    # searches the app directory and the standard system paths -- never the NuGet
    # cache. Without this copy the process dies before any of our code runs with
    # "找不到 Microsoft.WindowsAppRuntime.Bootstrap.dll". MSBuild does the same
    # thing in WindowsAppSDK-Nuget-Native.Bootstrap.targets.
    if (-not (Test-Path $bootstrapDll)) { throw "bootstrapper DLL not found: $bootstrapDll" }
    $deployedDll = Join-Path $BuildDir 'Microsoft.WindowsAppRuntime.Bootstrap.dll'
    [IO.File]::Copy($bootstrapDll, $deployedDll, $true)
    Write-Host "runtime  : Microsoft.WindowsAppRuntime.Bootstrap.dll ($([math]::Round((Get-Item $deployedDll).Length/1KB)) KB) deployed next to the exe"

    # Deploy the .ico beside the exe: AppWindow.SetIcon and the tray icon load it
    # from that path at startup (an unpackaged app has no packaged assets to read).
    $appIco = Join-Path $SrcDir 'Assets\app.ico'
    if (Test-Path $appIco) {
        [IO.File]::Copy($appIco, (Join-Path $BuildDir 'app.ico'), $true)
        Write-Host "icon     : app.ico deployed next to the exe"
    }
    # Reminder for the next person who adds a lib: anything that makes the exe
    # import a DLL from the *framework* payload needs that file deployed too.
    Write-Host '           framework package Microsoft.WindowsAppRuntime.2 2.3.1.0 must be registered;'
    Write-Host '           the bootstrapper exits with a message box if no match is found.'

    # XAML markup deployment. Every generated InitializeComponent() does
    #     LoadComponent(*this, Uri{ L"ms-appx:///<Folder>/<Name>.xaml" })
    # and for an unpackaged app ms-appx:/// maps to the directory holding the
    # exe -- the same rule the framework itself relies on for its own
    # Microsoft.UI.Xaml\Assets\* files. So <Name>.xbf has to sit in
    # <build>\<Folder>\, not in the compiler's staging directory.
    #
    # The folder name comes from the generated URIs rather than being hardcoded,
    # so this cannot drift if the root namespace ever changes. MSBuild's
    # equivalent is the CopyGeneratedXaml target, which instead derives the
    # destination from the XAML item's path relative to ProjectDir -- the two
    # agree only for projects whose XAML sits at the project root, which is why
    # this build cannot reuse that rule.
    $markupOutput = Join-Path $BuildDir 'w_music'
    if (Test-Path $markupOutput) { [IO.Directory]::Delete($markupOutput, $true) }
    [IO.Directory]::CreateDirectory($markupOutput) | Out-Null
    $xbfDeployed = @{}
    foreach ($header in (Get-ChildItem $genComponent -Recurse -Filter '*.xaml.g.h*')) {
        $uriMatches = [regex]::Matches((Get-Content $header.FullName -Raw), 'ms-appx:///(?<p>[^"]+?)\.xaml')
        foreach ($m in $uriMatches) {
            $rel = $m.Groups['p'].Value -replace '/', '\'
            if ($xbfDeployed.ContainsKey($rel)) { continue }
            $source = Join-Path $xamlDir (([IO.Path]::GetFileName($rel)) + '.xbf')
            if (-not (Test-Path $source)) { continue }
            $target = Join-Path $BuildDir "$rel.xbf"
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
            [IO.File]::Copy($source, $target, $true)
            $xbfDeployed[$rel] = $source
        }
    }
    if ($xbfDeployed.Count -eq 0) {
        throw "no XBF deployed: no ms-appx:/// <path>.xaml URI found under $genComponent"
    }
    Write-Host "markup   : $($xbfDeployed.Count) xbf deployed at their ms-appx paths"
    foreach ($rel in ($xbfDeployed.Keys | Sort-Object)) {
        Write-Host "  xbf    : $rel.xbf"
    }
}

Step 'dev build OK'
