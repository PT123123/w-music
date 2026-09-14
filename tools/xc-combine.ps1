#requires -Version 5.1
# Prototype v2: build ONE w_music.winmd (all 8 runtimeclasses) via a single
# midlrt call on a combined "all.idl" that #includes every source idl, then
# run XamlCompiler Pass1+Pass2 with that winmd as LocalAssembly. Deterministic
# replacement for the multi-idl single-call (which only compiled the first idl)
# and for per-file + mdmerge (which hit MDM2018). Throwaway probe.
param()
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-full\combine.log'
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

# --- build combined all.idl (deterministic single-translation-unit) ---
$idlList = @(
    (Join-Path $SrcDir 'Models.idl'),
    (Join-Path $SrcDir 'ViewModels.idl'),
    (Join-Path $SrcDir 'App.idl'),
    (Join-Path $SrcDir 'MainWindow.idl'),
    (Join-Path $SrcDir 'Views\DiscoverPage.idl'),
    (Join-Path $SrcDir 'Views\LibraryPage.idl'),
    (Join-Path $SrcDir 'Views\NowPlayingPage.idl'),
    (Join-Path $SrcDir 'Views\OnlinePage.idl')
)
$sb = [System.Text.StringBuilder]::new()
foreach ($idl in $idlList) { [void]$sb.AppendLine("#include `"$idl`"") }
# NOTE: midlrt derives the winmd ASSEMBLY NAME from the input .idl filename (NOT from
# the /winmd output path). It must be "w_music" so it matches RootNamespace + LocalAssembly.
$allIdl = Join-Path $winmdDir 'w_music.idl'
[IO.File]::WriteAllText($allIdl, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote $allIdl : $($idlList.Count) includes"

$mergedWinmd = Join-Path $winmdDir 'w-music.winmd'
$mergedHdr = Join-Path $winmdDir 'w-music.h'
# midlrt REQUIRES the union Windows.winmd in /metadata_dir (else MIDL4034: cannot
# load Windows.Winmd). It emits a bare "Windows.Foundation, 255.255.255.255"
# assembly ref in the result, so XamlCompiler must be given the union as well.
$refsRoot = "C:\Program Files (x86)\Windows Kits\10\References\$sdkVersion"
$a = @($allIdl, '/nologo', '/W1', '/nomidl', '/metadata_dir', $unionMetadata)
foreach ($w in $wasdkRefs) { $a += @('/reference', $w) }
$a += @('/reference', (Join-Path $refsRoot 'Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd'))
$a += @('/reference', (Join-Path $refsRoot 'Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd'))
$a += @('/I', $SrcDir, '/I', (Join-Path $SrcDir 'Views'), '/I', (Join-Path $sdkRoot "Include\$sdkVersion\winrt"), '/winmd', $mergedWinmd, '/h', $mergedHdr)
$mlog = Join-Path $winmdDir 'midl-all.log'
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try { & $midlrt @a *>&1 | Out-File -FilePath $mlog -Encoding utf8 -Width 4096 } finally { $ErrorActionPreference = $prev }
Write-Host "midlrt all exit=$LASTEXITCODE  winmd? $(Test-Path $mergedWinmd)"

# --- XamlCompiler ---
function Item([string]$p, [string]$name) { [ordered]@{ ItemSpec=$p; Metadata=[ordered]@{ Identity=$p; FullPath=$p; ReferenceAssemblyName=$name } } }
$refMap = [ordered]@{
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd' = 'Microsoft.UI.Xaml'
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.interactiveexperiences\2.1.3\metadata\10.0.18362.0\Microsoft.UI.winmd' = 'Microsoft.UI'
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Text.winmd' = 'Microsoft.UI.Text'
    # Reference the OS contracts directly and NOT the union: the union duplicates the
    # contracts' Windows.UI.* types (WMC0901). Because w_music.winmd is compiled against
    # these same contracts, its Windows.Foundation refs are 4.0.0.0 and resolve here
    # (no more 255.255.255.255 -> WMC1006).
    'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd' = 'Windows.Foundation.FoundationContract'
    'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd' = 'Windows.Foundation.UniversalApiContract'
    'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\mscorlib.dll' = 'mscorlib'
}
$refs = $refMap.Keys | ForEach-Object { Item $_ $refMap[$_] }

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
Stop-Transcript | Out-Null
