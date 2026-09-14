#requires -Version 5.1
# Hypothesis: with /metadata_dir pointing at the Facade folder (which contains
# windows.winmd), midlrt no longer needs the full union and emits contract-named
# assembly refs, so XamlCompiler can run off facade+contracts (no union) with no
# WMC1006 and no union-vs-UniversalApiContract WMC0901 duplicate.
param([string]$MetadataDir = 'facade')
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-full\final.log'
Start-Transcript -Path $log -Force | Out-Null

$Root = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$BuildDir = Join-Path $Root 'build'
$GenDir = Join-Path $BuildDir 'gen\xaml-full'
$winmdDir = Join-Path $BuildDir 'gen\winmd-final'
Remove-Item $winmdDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $winmdDir | Out-Null
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null

$saved = Get-Content (Join-Path $BuildDir 'toolenv.json') -Raw | ConvertFrom-Json
foreach ($n in @('PATH','INCLUDE','LIB','LIBPATH')) { Set-Item -Path "env:$n" -Value ([string]$saved.$n) }
$midlrt = (Get-Command midlrt.exe -ErrorAction Stop).Source
$sdkVersion = (Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$unionMetadata = "$sdkRoot\UnionMetadata\$sdkVersion"
$facadeDir = "$sdkRoot\UnionMetadata\$sdkVersion\Facade"
$refsRoot = "$sdkRoot\References\$sdkVersion"
$nugetRoot = "$env:USERPROFILE\.nuget\packages"
$xc = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\net472\XamlCompiler.exe'
$fc = Join-Path $refsRoot 'Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd'
$uc = Join-Path $refsRoot 'Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd'
$facade = Join-Path $facadeDir 'windows.winmd'

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

$idlList = @('Models.idl','ViewModels.idl','App.idl','MainWindow.idl','Views\DiscoverPage.idl','Views\LibraryPage.idl','Views\NowPlayingPage.idl','Views\OnlinePage.idl')
$sb = [System.Text.StringBuilder]::new()
foreach ($idl in $idlList) { [void]$sb.AppendLine("#include `"$(Join-Path $SrcDir $idl)`"") }
$allIdl = Join-Path $winmdDir 'w_music.idl'
[IO.File]::WriteAllText($allIdl, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

$mergedWinmd = Join-Path $winmdDir 'w-music.winmd'
$mergedHdr = Join-Path $winmdDir 'w-music.h'
if ($MetadataDir -eq 'facade') { $md = $facadeDir } else { $md = $unionMetadata }
$a = @($allIdl, '/nologo', '/W1', '/nomidl', '/metadata_dir', $md)
foreach ($w in $wasdkRefs) { $a += @('/reference', $w) }
$a += @('/reference', $fc); $a += @('/reference', $uc)
$a += @('/I', $SrcDir, '/I', (Join-Path $SrcDir 'Views'), '/I', (Join-Path $sdkRoot "Include\$sdkVersion\winrt"), '/winmd', $mergedWinmd, '/h', $mergedHdr)
$mlog = Join-Path $winmdDir 'midl.log'
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try { & $midlrt @a *>&1 | Out-File -FilePath $mlog -Encoding utf8 -Width 4096 } finally { $ErrorActionPreference = $prev }
Write-Host "midlrt exit=$LASTEXITCODE winmd? $(Test-Path $mergedWinmd) (metadata_dir=$md)"
if ($LASTEXITCODE -ne 0) { Get-Content $mlog | Where-Object { $_ -match 'error' } | ForEach-Object { Write-Host "   $_" } }

# dump refs of the produced winmd
try {
    $toolDir = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\net472'
    Add-Type -Path (Join-Path $toolDir 'System.Collections.Immutable.dll') -ErrorAction Stop
    Add-Type -Path (Join-Path $toolDir 'System.Reflection.Metadata.dll') -ErrorAction Stop
    $fs = [IO.File]::OpenRead($mergedWinmd); $pe = New-Object System.Reflection.PortableExecutable.PEReader($fs); $md2 = [System.Reflection.Metadata.PEReaderExtensions]::GetMetadataReader($pe)
    Write-Host "  w_music.winmd AssemblyRefs:"
    foreach ($h in $md2.AssemblyReferences) { $ar = $md2.GetAssemblyReference($h); Write-Host "    $($md2.GetString($ar.Name)) $($ar.Version)" }
    $pe.Dispose(); $fs.Dispose()
} catch { Write-Host "  refs dump failed: $($_.Exception.Message)" }

function Item([string]$p, [string]$name) { [ordered]@{ ItemSpec=$p; Metadata=[ordered]@{ Identity=$p; FullPath=$p; ReferenceAssemblyName=$name } } }
$refs = @(
    (Item 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd' 'Microsoft.UI.Xaml'),
    (Item 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.interactiveexperiences\2.1.3\metadata\10.0.18362.0\Microsoft.UI.winmd' 'Microsoft.UI'),
    (Item 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Text.winmd' 'Microsoft.UI.Text'),
    (Item $facade 'Windows'),
    (Item $fc 'Windows.Foundation.FoundationContract'),
    (Item $uc 'Windows.Foundation.UniversalApiContract'),
    (Item 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\mscorlib.dll' 'mscorlib')
)
$paths = @( (Item 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata' ''), (Item $refsRoot '') )
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
        ReferenceAssemblies=$refs; ReferenceAssemblyPaths=$paths
        WindowsSdkPath=$sdkRoot
        VCInstallPath32='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x86\vcmeta.dll'
        VCInstallPath64='C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\vcmeta.dll'
        GenXbfPath='C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\'
        SavedStateFile=(Join-Path $GenDir 'XamlSaveStateFile.xml')
        FeatureControlFlags='EnableDefaultValidationContextGeneration'
        UseVCMetaManaged=$true; XAMLFingerprint=$true
        TargetPlatformMinVersion='10.0.19041.0'; PrecompiledHeaderFile=''; CIncludeDirectories=''
        PriIndexName='w_music'; CodeGenerationControlFlags=''; EnabledXamlOptionalChanges=''; DisabledXamlOptionalChanges=''
        XamlResourceMapName=''; XamlComponentResourceLocation=''; VCInstallDir=''
        FingerprintIgnorePaths=@($sdkRoot,$nugetRoot)
        SuppressWarnings=$null; DisableXbfGeneration=$false; OutputType='WinExe'
        LocalAssembly=@( Item $mergedWinmd 'w_music' )
    }
}
foreach ($p1 in @($true,$false)) {
    $in = XamlInput $p1; $pn = if ($p1) {1} else {2}
    $ij = Join-Path $GenDir "final_in$pn.json"; $oj = Join-Path $GenDir "final_out$pn.json"
    [IO.File]::WriteAllText($ij, ($in | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
    & $xc $ij $oj | Out-Null
    Write-Host "== Pass$pn"
    if (Test-Path $oj) {
        $o = Get-Content $oj -Raw | ConvertFrom-Json
        $errs = @($o.MSBuildLogEntries | Where-Object { $_.ErrorCode -match 'WMC' -or ($_.Message -match 'error' -and $_.Message -notmatch 'perfXC') })
        Write-Host "   CodeFiles=$($o.GeneratedCodeFiles.Count) Xbf=$($o.GeneratedXbfFiles.Count) errors=$($errs.Count)"
        $o.GeneratedCodeFiles | ForEach-Object { Write-Host "     $([IO.Path]::GetFileName($_))" }
        ($errs | ForEach-Object { "[$($_.ErrorCode)] $($_.Message)" } | Sort-Object -Unique) | Select-Object -First 8 | ForEach-Object { Write-Host "     $($_.Substring(0,[Math]::Min(180,$_.Length)))" }
    }
}
Stop-Transcript | Out-Null
