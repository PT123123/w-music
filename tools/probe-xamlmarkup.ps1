#requires -Version 5.1
# Isolate the Invoke-XamlMarkup call from dev-build.ps1 and surface the real
# exception. Uses the already-built winmd + projection, so it runs in seconds
# instead of re-running midlrt/cppwinrt.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$BuildDir = Join-Path $Root 'build'
$GenDir = Join-Path $BuildDir 'gen'
. (Join-Path $PSScriptRoot 'xaml-markup.ps1')

# Same helper as dev-build.ps1, so the scriptblock-forwarding path is identical.
function Invoke-Native {
    param([scriptblock]$Action, [string]$LogPath)
    Write-Host "  [invoke-native] action=$(if ($null -eq $Action) { '<NULL>' } else { 'ok' }) log=$LogPath"
    if ($null -eq $Action) { throw 'Invoke-Native received a NULL action scriptblock' }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Action *>&1 | Out-File -FilePath $LogPath -Encoding utf8 -Width 4096 }
    finally { $ErrorActionPreference = $previous }
    return $LASTEXITCODE
}

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
$nugetRoot = "$env:USERPROFILE\.nuget\packages"
function Newest([string]$id) {
    Get-ChildItem "$nugetRoot\$id" -Directory | Where-Object { $_.Name -notmatch '-' } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
}
$winuiPkg = Newest 'microsoft.windowsappsdk.winui'
$iePkg = Newest 'microsoft.windowsappsdk.interactiveexperiences'
$foundationPkg = Newest 'microsoft.windowsappsdk.foundation'
$dispatchingWinmd = Get-ChildItem (Join-Path $iePkg.FullName 'metadata') -Recurse -Filter 'Microsoft.UI.winmd' |
    Select-Object -First 1 -ExpandProperty FullName
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcDir = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1

$outDir = Join-Path $GenDir 'xaml-probe'
$pages = @('DiscoverPage', 'LibraryPage', 'NowPlayingPage', 'OnlinePage')
$pageXaml = @($pages | ForEach-Object { (Resolve-Path (Join-Path $SrcDir "Views\$_.xaml")).Path })
$pageXaml += (Resolve-Path (Join-Path $SrcDir 'MainWindow.xaml')).Path
$appXaml = @( (Resolve-Path (Join-Path $SrcDir 'App.xaml')).Path )

$refAssemblies = [ordered]@{
    (Join-Path $winuiPkg.FullName 'metadata\Microsoft.UI.Xaml.winmd') = 'Microsoft.UI.Xaml'
    $dispatchingWinmd = 'Microsoft.UI'
    (Join-Path $winuiPkg.FullName 'metadata\Microsoft.UI.Text.winmd') = 'Microsoft.UI.Text'
    (Join-Path $GenDir 'wf\Windows.Foundation.winmd') = 'Windows.Foundation'
    (Get-ContractWinmd $sdkRoot $sdkVersion 'Windows.Foundation.FoundationContract') = 'Windows.Foundation.FoundationContract'
    (Get-ContractWinmd $sdkRoot $sdkVersion 'Windows.Foundation.UniversalApiContract') = 'Windows.Foundation.UniversalApiContract'
    (Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\mscorlib.dll') = 'mscorlib'
}
Write-Host 'refs:'
foreach ($k in $refAssemblies.Keys) { Write-Host "  $(if (Test-Path $k) { 'OK  ' } else { 'MISS' }) $k" }

try {
    Invoke-XamlMarkup -SrcDir $SrcDir -OutDir $outDir `
        -MergedWinmd (Join-Path $GenDir 'winmd\w-music.winmd') `
        -FoundationWinmd (Join-Path $GenDir 'wf\Windows.Foundation.winmd') `
        -SdkRoot $sdkRoot -SdkVersion $sdkVersion `
        -XamlCompiler (Join-Path $winuiPkg.FullName 'tools\net472\XamlCompiler.exe') `
        -ProjectPath (Resolve-Path (Join-Path $SrcDir 'w-music.vcxproj')).Path `
        -RootNamespace 'w_music' `
        -VcMeta32 (Join-Path $vcDir.FullName 'bin\Hostx64\x86\vcmeta.dll') `
        -VcMeta64 (Join-Path $vcDir.FullName 'bin\Hostx64\x64\vcmeta.dll') `
        -GenXbfPath (Join-Path $winuiPkg.FullName 'tools\') `
        -ReferenceAssemblies $refAssemblies `
        -ReferencePaths @((Join-Path $winuiPkg.FullName 'metadata'), (Join-Path $sdkRoot "UnionMetadata\$sdkVersion"), (Join-Path $sdkRoot "References\$sdkVersion")) `
        -PageXaml $pageXaml -AppXaml $appXaml `
        -LogPath (Join-Path $outDir 'xamlc.log') `
        -InvokeNative ${function:Invoke-Native} | Out-Null
    Write-Host 'Invoke-XamlMarkup: SUCCESS'
}
catch {
    Write-Host ''
    Write-Host "EXCEPTION TYPE : $($_.Exception.GetType().FullName)"
    Write-Host "EXCEPTION MSG  : $($_.Exception.Message)"
    Write-Host "SCRIPT STACK   :"
    Write-Host $_.ScriptStackTrace
}
