#requires -Version 5.1
param([string]$Mode = 'uniononly', [string]$Suppress = '')
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-full\one.log'
Start-Transcript -Path $log -Force | Out-Null

$Root = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$BuildDir = Join-Path $Root 'build'
$GenDir = Join-Path $BuildDir 'gen\xaml-full'
$mergedWinmd = Join-Path $BuildDir 'gen\winmd-full\w-music.winmd'

$sdk = '10.0.26100.0'
$union = "C:\Program Files (x86)\Windows Kits\10\UnionMetadata\$sdk\Windows.winmd"
$fc = "C:\Program Files (x86)\Windows Kits\10\References\$sdk\Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd"
$uc = "C:\Program Files (x86)\Windows Kits\10\References\$sdk\Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd"
$mscorlib = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\mscorlib.dll'
$winuiXaml = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd'
$winuiUI = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.interactiveexperiences\2.1.3\metadata\10.0.18362.0\Microsoft.UI.winmd'
$winuiText = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Text.winmd'
$xc = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\net472\XamlCompiler.exe'

function Item([string]$p, [string]$name) { [ordered]@{ ItemSpec=$p; Metadata=[ordered]@{ Identity=$p; FullPath=$p; ReferenceAssemblyName=$name } } }
$apps = @( Item (Resolve-Path (Join-Path $SrcDir 'App.xaml')).Path '' )
$pages = @(
    (Resolve-Path (Join-Path $SrcDir 'MainWindow.xaml')).Path,
    (Resolve-Path (Join-Path $SrcDir 'Views\DiscoverPage.xaml')).Path,
    (Resolve-Path (Join-Path $SrcDir 'Views\LibraryPage.xaml')).Path,
    (Resolve-Path (Join-Path $SrcDir 'Views\NowPlayingPage.xaml')).Path,
    (Resolve-Path (Join-Path $SrcDir 'Views\OnlinePage.xaml')).Path
) | ForEach-Object { Item $_ '' }

$wf = 'C:\Users\ted\Desktop\w-music\build\gen\wf2\Windows.Foundation.winmd'
if ($Mode -eq 'uniononly') {
    $refs = @( (Item $winuiXaml 'Microsoft.UI.Xaml'), (Item $winuiUI 'Microsoft.UI'), (Item $winuiText 'Microsoft.UI.Text'), (Item $union 'Windows'), (Item $mscorlib 'mscorlib') )
} elseif ($Mode -eq 'wf') {
    $refs = @( (Item $winuiXaml 'Microsoft.UI.Xaml'), (Item $winuiUI 'Microsoft.UI'), (Item $winuiText 'Microsoft.UI.Text'), (Item $wf 'Windows.Foundation'), (Item $fc 'Windows.Foundation.FoundationContract'), (Item $uc 'Windows.Foundation.UniversalApiContract'), (Item $mscorlib 'mscorlib') )
} else {
    $refs = @( (Item $winuiXaml 'Microsoft.UI.Xaml'), (Item $winuiUI 'Microsoft.UI'), (Item $winuiText 'Microsoft.UI.Text'), (Item $union 'Windows'), (Item $fc 'Windows.Foundation.FoundationContract'), (Item $uc 'Windows.Foundation.UniversalApiContract'), (Item $mscorlib 'mscorlib') )
}
$paths = @( (Item (Join-Path $env:USERPROFILE '.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata') ''), (Item "C:\Program Files (x86)\Windows Kits\10\UnionMetadata\$sdk" ''), (Item "C:\Program Files (x86)\Windows Kits\10\References\$sdk" '') )

function XamlInput([bool]$pass1) {
    $mode = if ($pass1) { 'RealBuildPass1' } else { 'RealBuildPass2' }
    [ordered]@{
        Language='CppWinRT'; LanguageSourceExtension='.cpp'; RootNamespace='w_music'; ProjectName='w-music'
        ProjectPath=(Resolve-Path (Join-Path $SrcDir 'w-music.vcxproj')).Path
        IsPass1=$pass1; CompileMode=$mode; OutputPath=$GenDir
        XamlPages=$pages; XamlApplications=$apps
        ReferenceAssemblies=$refs; ReferenceAssemblyPaths=$paths
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
        SuppressWarnings=$(if ([string]::IsNullOrEmpty($Suppress)) { $null } else { @($Suppress) }); DisableXbfGeneration=$false; OutputType='WinExe'
        LocalAssembly=@( Item $mergedWinmd 'w_music' )
    }
}
foreach ($p1 in @($true,$false)) {
    $in = XamlInput $p1
    $pn = if ($p1) {1} else {2}
    $ij = Join-Path $GenDir "one_in$pn.json"; $oj = Join-Path $GenDir "one_out$pn.json"
    [IO.File]::WriteAllText($ij, ($in | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
    & $xc $ij $oj | Out-Null
    Write-Host "== Pass$pn ($Mode)"
    if (Test-Path $oj) {
        $o = Get-Content $oj -Raw | ConvertFrom-Json
        $errs = @($o.MSBuildLogEntries | Where-Object { $_.ErrorCode -match 'WMC' -or ($_.Message -match 'error' -and $_.Message -notmatch 'perfXC') })
        $u = $errs | ForEach-Object { "[$($_.ErrorCode)] $($_.Message)" } | Sort-Object -Unique
        Write-Host "   errors=$($errs.Count) unique=$($u.Count)"
        $u | Select-Object -First 12 | ForEach-Object { Write-Host "     $($_.Substring(0,[Math]::Min(190,$_.Length)))" }
    }
}
Stop-Transcript | Out-Null
