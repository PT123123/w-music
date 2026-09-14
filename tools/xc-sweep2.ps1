#requires -Version 5.1
# Focused sweep: does the union in ReferenceAssemblies (WITHOUT also listing the
# UnionMetadata folder in ReferenceAssemblyPaths) resolve bare Windows.Foundation
# without the WMC0901 duplicate?
param()
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-full\sweep2.log'
Start-Transcript -Path $log -Force | Out-Null

$Root = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$BuildDir = Join-Path $Root 'build'
$GenDir = Join-Path $BuildDir 'gen\xaml-full'
$mergedWinmd = Join-Path $BuildDir 'gen\winmd-full\w-music.winmd'

$sdk = '10.0.26100.0'
$union = "C:\Program Files (x86)\Windows Kits\10\UnionMetadata\$sdk\Windows.winmd"
$facade = "C:\Program Files (x86)\Windows Kits\10\UnionMetadata\$sdk\Facade\windows.winmd"
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

$pathsNoUnion = @((Join-Path $env:USERPROFILE '.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata'), "C:\Program Files (x86)\Windows Kits\10\References\$sdk")
$cases = @(
  @{ Name='V1 alias FC as Windows.Foundation + UC';
     RefExtra=@(@($fc,'Windows.Foundation'),@($uc,'Windows.Foundation.UniversalApiContract'));
     Paths=$pathsNoUnion }
  @{ Name='V2 alias FC as both Windows.Foundation and FoundationContract + UC';
     RefExtra=@(@($fc,'Windows.Foundation'),@($fc,'Windows.Foundation.FoundationContract'),@($uc,'Windows.Foundation.UniversalApiContract'));
     Paths=$pathsNoUnion }
  @{ Name='V3 alias UC as Windows.Foundation + FC(named contract)';
     RefExtra=@(@($uc,'Windows.Foundation'),@($fc,'Windows.Foundation.FoundationContract'));
     Paths=$pathsNoUnion }
  @{ Name='V4 alias FC as Windows.Foundation + UC + facade(named Windows)';
     RefExtra=@(@($fc,'Windows.Foundation'),@($uc,'Windows.Foundation.UniversalApiContract'),@($facade,'Windows'));
     Paths=$pathsNoUnion }
)

function XamlInput([bool]$pass1, $extra, $pathsArr) {
    $mode = if ($pass1) { 'RealBuildPass1' } else { 'RealBuildPass2' }
    $refs = @( (Item $winuiXaml 'Microsoft.UI.Xaml'), (Item $winuiUI 'Microsoft.UI'), (Item $winuiText 'Microsoft.UI.Text') )
    foreach ($e in $extra) { $refs += (Item $e[0] $e[1]) }
    $refs += (Item $mscorlib 'mscorlib')
    $paths = @(); foreach ($p in $pathsArr) { $paths += (Item $p '') }
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
        SuppressWarnings=$null; DisableXbfGeneration=$false; OutputType='WinExe'
        LocalAssembly=@( Item $mergedWinmd 'w_music' )
    }
}
foreach ($c in $cases) {
    Write-Host "================= CASE: $($c.Name) ================="
    $ok = $true
    foreach ($p1 in @($true,$false)) {
        $in = XamlInput $p1 $c.RefExtra $c.Paths
        $pn = if ($p1) {1} else {2}
        $ij = Join-Path $GenDir "s2_in$pn.json"; $oj = Join-Path $GenDir "s2_out$pn.json"
        [IO.File]::WriteAllText($ij, ($in | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
        & $xc $ij $oj | Out-Null
        if (Test-Path $oj) {
            $o = Get-Content $oj -Raw | ConvertFrom-Json
            $errs = @($o.MSBuildLogEntries | Where-Object { $_.ErrorCode -match 'WMC' -or ($_.Message -match 'error' -and $_.Message -notmatch 'perfXC') })
            $codes = ($errs | ForEach-Object { $_.ErrorCode }) -join ','
            if ($errs.Count -gt 0) { $ok = $false }
            Write-Host "  Pass$pn : CodeFiles=$($o.GeneratedCodeFiles.Count) Xbf=$($o.GeneratedXbfFiles.Count) err=$($errs.Count) [$codes]"
            if ($errs.Count -gt 0 -and $errs.Count -le 4) { $errs | ForEach-Object { Write-Host "      [$($_.ErrorCode)] $($_.Message.Substring(0,[Math]::Min(150,$_.Message.Length)))" } }
        } else { Write-Host "  Pass$pn : NO OUTPUT"; $ok = $false }
    }
    Write-Host "  -> $(if($ok){'CLEAN'}else{'has errors'})"
}
Stop-Transcript | Out-Null
