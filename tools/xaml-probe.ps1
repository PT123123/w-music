#requires -Version 5.1
# POC: drive the real WinUI 3 C++ XamlCompiler.exe for one page and capture
# its JSON protocol (input.json / output.json) so we can bake it into
# dev-build.ps1. Throwaway experiment — not part of the build.
param([string]$Page = 'Views\DiscoverPage.xaml')

$ErrorActionPreference = 'Stop'
$Root   = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$GenDir = Join-Path $Root 'build\gen\xaml-probe'
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null

$xc = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\net472\XamlCompiler.exe'

$refs = @(
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd',
    'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Text.winmd',
    'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Windows.winmd'
)
$pagePath = (Resolve-Path (Join-Path $SrcDir $Page)).Path

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

$in = [ordered]@{
    Language                     = 'CppWinRT'
    LanguageSourceExtension      = '.cpp'
    RootNamespace                = 'w_music'
    ProjectName                  = 'w-music'
    ProjectPath                  = (Resolve-Path (Join-Path $SrcDir 'w-music.vcxproj')).Path
    IsPass1                      = $true
    CompileMode                  = 'RealBuildPass1'
    OutputPath                   = $GenDir
    XamlPages                    = @( Item $pagePath '' )
    XamlApplications             = @()
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

$inJson  = Join-Path $GenDir 'input.json'
$outJson = Join-Path $GenDir 'output.json'
[IO.File]::WriteAllText($inJson, ($in | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))

Write-Host "== running XamlCompiler on $Page"
& $xc $inJson $outJson
Write-Host "exit=$?"
Write-Host "== output.json (first 80 lines) =="
if (Test-Path $outJson) {
    Get-Content $outJson | Select-Object -First 80 | ForEach-Object { Write-Host "  $_" }
}
else { Write-Host "  (no output.json produced)" }
Write-Host "== generated files in $GenDir =="
Get-ChildItem $GenDir -Recurse | ForEach-Object { Write-Host "  $($_.FullName)" }
