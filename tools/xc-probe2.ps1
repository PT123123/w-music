#requires -Version 5.1
# POC: drive XamlCompiler.exe Pass1+Pass2 for ALL xaml files (App + MainWindow + 4 pages)
# to discover the full generated artifact set and the .g.cpp XBF-loading mechanism.
param()

$ErrorActionPreference = 'Stop'
$Transcript = Join-Path $PSScriptRoot '..\build\gen\xaml-probe2\transcript.log'
Start-Transcript -Path $Transcript -Force | Out-Null
$Root   = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$GenDir = Join-Path $Root 'build\gen\xaml-probe2'
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null

$xc = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\net472\XamlCompiler.exe'

function Item([string]$p, [string]$name) {
    [ordered]@{ ItemSpec = $p; Metadata = [ordered]@{ Identity = $p; FullPath = $p; ReferenceAssemblyName = $name } }
}

$refMap = [ordered]@{
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd'                                  = 'Microsoft.UI.Xaml'
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.interactiveexperiences\2.1.3\metadata\10.0.18362.0\Microsoft.UI.winmd'           = 'Microsoft.UI'
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Text.winmd'                                    = 'Microsoft.UI.Text'
    'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Windows.winmd'                                                    = 'Windows'
    'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd' = 'Windows.Foundation.FoundationContract'
    'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd' = 'Windows.Foundation.UniversalApiContract'
    'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\mscorlib.dll'                                                                     = 'mscorlib'
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

function Build-Input([bool]$pass1) {
    $mode = if ($pass1) { 'RealBuildPass1' } else { 'RealBuildPass2' }
    [ordered]@{
        Language                     = 'CppWinRT'
        LanguageSourceExtension      = '.cpp'
        RootNamespace                = 'w_music'
        ProjectName                  = 'w-music'
        ProjectPath                  = (Resolve-Path (Join-Path $SrcDir 'w-music.vcxproj')).Path
        IsPass1                      = $pass1
        CompileMode                  = $mode
        OutputPath                   = $GenDir
        XamlPages                    = $pages
        XamlApplications             = $apps
        ReferenceAssemblies          = $refs
        ReferenceAssemblyPaths       = @(
            Item 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata' ''
            Item 'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0' ''
            Item 'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0' ''
        )
        WindowsSdkPath               = 'C:\Program Files (x86)\Windows Kits\10'
        VCInstallPath32              = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x86\vcmeta.dll'
        VCInstallPath64              = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\vcmeta.dll'
        GenXbfPath                   = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\'
        SavedStateFile               = (Join-Path $GenDir 'XamlSaveStateFile.xml')
        FeatureControlFlags          = 'EnableDefaultValidationContextGeneration'
        UseVCMetaManaged             = $true
        XAMLFingerprint              = $true
        TargetPlatformMinVersion     = '10.0.19041.0'
        PrecompiledHeaderFile        = ''
        CIncludeDirectories          = ''
        PriIndexName                 = 'w_music'
        CodeGenerationControlFlags   = ''
        EnabledXamlOptionalChanges   = ''
        DisabledXamlOptionalChanges  = ''
        XamlResourceMapName          = ''
        XamlComponentResourceLocation = ''
        VCInstallDir                 = ''
        FingerprintIgnorePaths       = @('C:\Program Files (x86)\Windows Kits\10', 'C:\Users\ted\.nuget\packages')
        SuppressWarnings             = $null
        DisableXbfGeneration         = $false
        OutputType                   = 'WinExe'
    }
}

function Run-Pass([bool]$pass1) {
    $passNum = if ($pass1) { 1 } else { 2 }
    $in  = Build-Input $pass1
    $inJson  = Join-Path $GenDir ("input.pass{0}.json" -f $passNum)
    $outJson = Join-Path $GenDir ("output.pass{0}.json" -f $passNum)
    [IO.File]::WriteAllText($inJson, ($in | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "== Pass$passNum"
    & $xc $inJson $outJson
    Write-Host "   exit=$?"
    if (Test-Path $outJson) {
        $o = Get-Content $outJson -Raw | ConvertFrom-Json
        Write-Host "   GeneratedCodeFiles: $($o.GeneratedCodeFiles.Count)"
        $o.GeneratedCodeFiles | ForEach-Object { Write-Host "     $($_.Substring($GenDir.Length+1))" }
        Write-Host "   GeneratedXbfFiles: $($o.GeneratedXbfFiles.Count)"
        $o.GeneratedXbfFiles | ForEach-Object { Write-Host "     $($_.Substring($GenDir.Length+1))" }
        Write-Host "   GeneratedXamlFiles: $($o.GeneratedXamlFiles.Count)"
        $errs = $o.MSBuildLogEntries | Where-Object { $_.ErrorCode -match 'WMC' -or ($_.Message -match 'error' -and $_.Message -notmatch 'perfXC') }
        Write-Host "   WMC/error entries: $($errs.Count)"
        $errs | ForEach-Object { Write-Host "     [$($_.ErrorCode)] $($_.Message)" }
    }
}

Run-Pass $true
Run-Pass $false
Write-Host "== all generated files =="
Get-ChildItem $GenDir -Recurse -File | ForEach-Object { Write-Host "  $($_.FullName.Substring($GenDir.Length+1))" }
