#requires -Version 5.1
# Prototype: midlrt(all idl) -> XamlCompiler Pass1+Pass2 with LocalAssembly +
# app winmds referenced. Validates the full protocol and emits .g.cpp so we can
# inspect how InitializeComponent loads the XBF. Throwaway.
param()
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-probe2\full.log'
Start-Transcript -Path $log -Force | Out-Null

$Root = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$BuildDir = Join-Path $Root 'build'
$GenDir = Join-Path $BuildDir 'gen\xaml-full'
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null
$winmdDir = Join-Path $BuildDir 'gen\winmd-full'
Remove-Item $winmdDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $winmdDir | Out-Null

# --- tool env ---
$saved = Get-Content (Join-Path $BuildDir 'toolenv.json') -Raw | ConvertFrom-Json
foreach ($n in @('PATH','INCLUDE','LIB','LIBPATH')) { Set-Item -Path "env:$n" -Value ([string]$saved.$n) }
$midlrt = (Get-Command midlrt.exe -ErrorAction Stop).Source
$sdkVersion = (Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$unionMetadata = "C:\Program Files (x86)\Windows Kits\10\UnionMetadata\$sdkVersion"
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$nugetRoot = "$env:USERPROFILE\.nuget\packages"
$xc = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\net472\XamlCompiler.exe'

function Newest-PackageDir([string]$id){ Get-ChildItem "$nugetRoot\$id" -Directory | Where-Object { $_.Name -notmatch '-' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 }
$wasdkRefs = @()
$winuiPkgDir = Newest-PackageDir 'microsoft.windowsappsdk.winui'
$wasdkRefs += @(Get-ChildItem (Join-Path $winuiPkgDir.FullName 'metadata') -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
foreach ($pkgId in @('microsoft.windowsappsdk.interactiveexperiences','microsoft.windowsappsdk.foundation')) {
    $pkg = Newest-PackageDir $pkgId; if (-not $pkg) { continue }
    $metaRoot = Join-Path $pkg.FullName 'metadata'
    $metaDir = Get-ChildItem $metaRoot -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if ($metaDir) { $metaRoot = $metaDir.FullName }
    $wasdkRefs += @(Get-ChildItem $metaRoot -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
}

# --- midlrt: compile ALL idls in one invocation -> single w-music.winmd ---
$idlOrder = @('Models.idl','ViewModels.idl','Views\DiscoverPage.idl','Views\LibraryPage.idl','Views\NowPlayingPage.idl','Views\OnlinePage.idl','App.idl','MainWindow.idl')
$mergedWinmd = Join-Path $winmdDir 'w-music.winmd'
$mergedHdr = Join-Path $winmdDir 'w-music.h'
$a = @()
foreach ($idl in $idlOrder) { $a += "$SrcDir\$idl" }
$a += @('/nologo', '/W1', '/nomidl', '/metadata_dir', $unionMetadata)
foreach ($w in $wasdkRefs) { $a += @('/reference', $w) }
$a += @('/reference', (Join-Path $unionMetadata 'Windows.winmd'))
$a += @('/I', $SrcDir, '/I', (Join-Path $sdkRoot "Include\$sdkVersion\winrt"), '/winmd', $mergedWinmd, '/h', $mergedHdr)
$mlog = Join-Path $winmdDir 'midl-all.log'
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try { & $midlrt @a *>&1 | Out-File -FilePath $mlog -Encoding utf8 -Width 4096 } finally { $ErrorActionPreference = $prev }
Write-Host "midlrt all exit=$LASTEXITCODE  winmd? $(Test-Path $mergedWinmd)"
if ($LASTEXITCODE -ne 0) { Get-Content $mlog | Where-Object { $_ -match 'error' } | ForEach-Object { Write-Host "   $_" }; throw 'midlrt all' }
$localWinmds = @($mergedWinmd)

# --- XamlCompiler ---
function Item([string]$p, [string]$name) { [ordered]@{ ItemSpec=$p; Metadata=[ordered]@{ Identity=$p; FullPath=$p; ReferenceAssemblyName=$name } } }
$refMap = [ordered]@{
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd' = 'Microsoft.UI.Xaml'
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.interactiveexperiences\2.1.3\metadata\10.0.18362.0\Microsoft.UI.winmd' = 'Microsoft.UI'
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Text.winmd' = 'Microsoft.UI.Text'
    'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Windows.winmd' = 'Windows'
    'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd' = 'Windows.Foundation.FoundationContract'
    'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd' = 'Windows.Foundation.UniversalApiContract'
    'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\mscorlib.dll' = 'mscorlib'
}
$refs = $refMap.Keys | ForEach-Object { Item $_ $refMap[$_] }
# NOTE: local app winmds are provided via LocalAssembly only (not ReferenceAssemblies)
# to avoid the union winmd being double-included (WMC0901).

$apps = @( Item (Resolve-Path (Join-Path $SrcDir 'App.xaml')).Path '' )
$pages = @(
    (Resolve-Path (Join-Path $SrcDir 'MainWindow.xaml')).Path,
    (Resolve-Path (Join-Path $SrcDir 'Views\DiscoverPage.xaml')).Path,
    (Resolve-Path (Join-Path $SrcDir 'Views\LibraryPage.xaml')).Path,
    (Resolve-Path (Join-Path $SrcDir 'Views\NowPlayingPage.xaml')).Path,
    (Resolve-Path (Join-Path $SrcDir 'Views\OnlinePage.xaml')).Path
) | ForEach-Object { Item $_ '' }

function XamlInput([bool]$pass1) {
    $mode = if ($pass1) { 'RealBuildPass1' } else { 'RealBuildPass2' }
    [ordered]@{
        Language='CppWinRT'; LanguageSourceExtension='.cpp'; RootNamespace='w_music'; ProjectName='w-music'
        ProjectPath=(Resolve-Path (Join-Path $SrcDir 'w-music.vcxproj')).Path
        IsPass1=$pass1; CompileMode=$mode; OutputPath=$GenDir
        XamlPages=$pages; XamlApplications=$apps
        ReferenceAssemblies=$refs
        ReferenceAssemblyPaths=@(
            Item 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata' ''
            Item 'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0' ''
            Item 'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0' ''
        )
        WindowsSdkPath='C:\Program Files (x86)\Windows Kits\10'
        VCInstallPath32='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x86\vcmeta.dll'
        VCInstallPath64='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\vcmeta.dll'
        GenXbfPath='C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\'
        SavedStateFile=(Join-Path $GenDir 'XamlSaveStateFile.xml')
        FeatureControlFlags='EnableDefaultValidationContextGeneration'
        UseVCMetaManaged=$true; XAMLFingerprint=$true
        TargetPlatformMinVersion='10.0.19041.0'; PrecompiledHeaderFile=''; CIncludeDirectories=''
        PriIndexName='w_music'; CodeGenerationControlFlags=''; EnabledXamlOptionalChanges=''; DisabledXamlOptionalChanges=''
        XamlResourceMapName=''; XamlComponentResourceLocation=''; VCInstallDir=''
        FingerprintIgnorePaths=@('C:\Program Files (x86)\Windows Kits\10','C:\Users\ted\.nuget\packages')
        SuppressWarnings=$null; DisableXbfGeneration=$false; OutputType='WinExe'
        LocalAssembly=@( Item $mergedWinmd 'w_music' )
    }
}
foreach ($p1 in @($true,$false)) {
    $in = XamlInput $p1
    $pn = if ($p1) {1} else {2}
    $ij = Join-Path $GenDir "in$pn.json"; $oj = Join-Path $GenDir "out$pn.json"
    [IO.File]::WriteAllText($ij, ($in | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "== XamlCompiler Pass$pn"
    & $xc $ij $oj
    Write-Host "   exit=$?"
    if (Test-Path $oj) {
        $o = Get-Content $oj -Raw | ConvertFrom-Json
        Write-Host "   CodeFiles: $($o.GeneratedCodeFiles.Count)  Xbf: $($o.GeneratedXbfFiles.Count)"
        $o.GeneratedCodeFiles | ForEach-Object { Write-Host "     $($_.Substring($GenDir.Length+1))" }
        $errs = $o.MSBuildLogEntries | Where-Object { $_.ErrorCode -match 'WMC' -or ($_.Message -match 'error' -and $_.Message -notmatch 'perfXC') }
        Write-Host "   WMC/err: $($errs.Count)"; $errs | ForEach-Object { Write-Host "     [$($_.ErrorCode)] $($_.Message)" }
    }
}
