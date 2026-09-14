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

.PARAMETER ListOnly
    Prints the resolved tools and exits.

.NOTES
    Usage:  pwsh -NoProfile -File tools\dev-build.ps1 [-Clean] [-NoTests]
#>
param(
    [switch]$Clean,
    [switch]$NoGen,
    [switch]$NoLink,
    [switch]$NoTests,
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
$bootstrapAutoInit = Join-Path $foundationInclude 'MddBootstrapAutoInitializer.cpp'
$undockedAutoInit = Join-Path $foundationInclude 'UndockedRegFreeWinRT-AutoInitializer.cpp'
Write-Host "bootstrap: $($foundationPkgDir.Name)"

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
$pages = @('DiscoverPage', 'LibraryPage', 'NowPlayingPage', 'OnlinePage')
$xamlNameOrder = @('App', 'MainWindow') + $pages

if (-not $NoGen) {
    # --- 4. one combined idl -> one w_music.winmd --------------------------
    Step '4/8 midlrt-compiling combined app IDL'
    # Compile order is irrelevant here: midlrt sees one translation unit with all
    # sources #included, in this order (dependencies first, for clarity only).
    $idlOrder = @('Models.idl', 'ViewModels.idl',
                  'Views\DiscoverPage.idl', 'Views\LibraryPage.idl',
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
        'DiscoverPage' = 'Views\DiscoverPage.h'; 'LibraryPage' = 'Views\LibraryPage.h'
        'NowPlayingPage' = 'Views\NowPlayingPage.h'; 'OnlinePage' = 'Views\OnlinePage.h'
        'LibraryViewModel' = 'ViewModels\LibraryViewModel.h'; 'PlayerViewModel' = 'ViewModels\PlayerViewModel.h'
        'LyricLineItem' = 'Models\LyricLineItem.h'; 'OnlineTrackItem' = 'Models\OnlineTrackItem.h'
        'PlaylistItem' = 'Models\PlaylistItem.h'; 'QualityChipItem' = 'Models\QualityChipItem.h'
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
    'DiscoverPage' = 'Views\DiscoverPage.h'; 'LibraryPage' = 'Views\LibraryPage.h'
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
    'ViewModels\PlayerViewModel.cpp', 'ViewModels\LibraryViewModel.cpp',
    'Services\AppPaths.cpp', 'Services\LibraryService.cpp',
    'Services\OnlineProviderService.cpp', 'Services\BuiltinProviders.cpp',
    'Services\DiscoverSettings.cpp', 'Services\Services.cpp',
    'Controls\SpectrumView.cpp', 'Audio\WasapiLoopback.cpp',
    'Views\DiscoverPage.cpp', 'Views\LibraryPage.cpp',
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
    'Views\DiscoverPage.h', 'Views\LibraryPage.h',
    'Views\NowPlayingPage.h', 'Views\OnlinePage.h',
    'ViewModels\LibraryViewModel.h', 'ViewModels\PlayerViewModel.h',
    'Models\TrackItem.h', 'Models\PlaylistItem.h', 'Models\LyricLineItem.h',
    'Models\OnlineTrackItem.h', 'Models\QualityChipItem.h',
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
    $bootstrapAutoInit,
    $undockedAutoInit
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
$cppFlags = '/std:c++20 /EHsc /utf-8 /bigobj /W3 /permissive- /D_UNICODE /DUNICODE /DWINRT_LEAN_AND_MEAN /D_VSDESIGNER_DONT_LOAD_AS_DLL'
# Microsoft.WindowsAppRuntime.lib is needed for WindowsAppRuntime_EnsureIsLoaded,
# which UndockedRegFreeWinRT-AutoInitializer.cpp calls. MddBootstrap*.lib (the
# bootstrapper) and WindowsAppRuntime*.lib (the framework) are separate imports.
$linkLibs = @('windowsapp.lib', 'Microsoft.WindowsAppRuntime.lib',
              'Microsoft.WindowsAppRuntime.Bootstrap.lib',
              'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'runtimeobject.lib',
              'shell32.lib', 'shlwapi.lib', 'propsys.lib', 'user32.lib',
              'gdi32.lib', 'd3d11.lib', 'dxgi.lib', 'windowscodecs.lib',
              'bcrypt.lib') -join ' '

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
[void]$edges.AppendLine('  command = cl.exe /nologo $in /Fe$out /link /SUBSYSTEM:WINDOWS /LIBPATH:"' + $bootstrapLibDir + '" ' + $linkLibs)
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

if (-not $NoLink) {
    [void]$edges.AppendLine("build w-music.exe: linkapp $($appObjs -join ' ') $($coreObjs -join ' ')")
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

# ---------------------------------------------------------------------------
# 8. build report
# ---------------------------------------------------------------------------
Step '8/8 build report'
if (-not $NoLink) {
    $exe = Join-Path $BuildDir 'w-music.exe'
    if (Test-Path $exe) {
        Write-Host "exe      : $exe ($([math]::Round((Get-Item $exe).Length/1KB)) KB)"
    }
    else {
        Write-Host 'exe      : not produced' -ForegroundColor Yellow
    }
}
foreach ($f in (Get-ChildItem $xamlDir -Filter '*.xbf' -ErrorAction SilentlyContinue)) {
    Write-Host "  xbf    : $($f.Name) ($([math]::Round($f.Length/1KB)) KB)"
}

Step 'dev build OK'
