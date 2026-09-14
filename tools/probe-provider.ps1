#requires -Version 5.1
# Throwaway probe: does [default_interface] make midlrt mark
# w_music.XamlMetaDataProvider's interface as the default one? Cheap (~1 min)
# versus a full rebuild (~6 min) for the same answer.
param([switch]$NoDefaultInterface)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
. (Join-Path $PSScriptRoot 'xaml-markup.ps1')

$saved = Get-Content (Join-Path $BuildDir 'toolenv.json') -Raw | ConvertFrom-Json
foreach ($n in @('PATH', 'INCLUDE', 'LIB', 'LIBPATH')) { Set-Item -Path "env:$n" -Value ([string]$saved.$n) }

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
$nugetRoot = "$env:USERPROFILE\.nuget\packages"
function Newest([string]$id) {
    Get-ChildItem "$nugetRoot\$id" -Directory | Where-Object { $_.Name -notmatch '-' } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
}
$winuiPkgDir = Newest 'microsoft.windowsappsdk.winui'
$iePkg = Newest 'microsoft.windowsappsdk.interactiveexperiences'
$wasdkRefs = @(
    (Join-Path $winuiPkgDir.FullName 'metadata\Microsoft.UI.Xaml.winmd'),
    (Join-Path $winuiPkgDir.FullName 'metadata\Microsoft.UI.Text.winmd'),
    (Get-ChildItem (Join-Path $iePkg.FullName 'metadata') -Recurse -Filter 'Microsoft.UI.winmd' |
        Select-Object -First 1 -ExpandProperty FullName)
)

$outDir = Join-Path $BuildDir 'gen\probe-provider'
if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$attr = if ($NoDefaultInterface) { '' } else { "    [default_interface]`r`n" }
$idl = Join-Path $outDir 'XamlMetaDataProvider.idl'
[IO.File]::WriteAllText($idl, @"
namespace w_music
{
$attr    runtimeclass XamlMetaDataProvider : Microsoft.UI.Xaml.Markup.IXamlMetadataProvider
    {
        XamlMetaDataProvider();
    };
}
"@, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "attribute: $(if ($NoDefaultInterface) { 'NONE' } else { '[default_interface]' })"

$combined = New-CombinedAppIdl -OutDir $outDir -IdlPaths @($idl)
$winmd = Join-Path $outDir 'w-music.winmd'
$hdr = Join-Path $outDir 'w-music.h'
Invoke-AppMidl -CombinedIdl $combined -OutWinmd $winmd -OutHdr $hdr `
    -SdkRoot $sdkRoot -SdkVersion $sdkVersion -SrcDir (Join-Path $Root 'src\w-music') `
    -ReferenceWinmds $wasdkRefs -LogPath (Join-Path $outDir 'midlrt.log') `
    -InvokeNative ${function:Invoke-Native} | Out-Null
Write-Host 'midlrt OK'

$block = (Get-Content $hdr -Raw) -split "`n" | Select-String -Pattern 'Class w_music.XamlMetaDataProvider' -SimpleMatch -Context 0, 8
foreach ($c in $block) { Write-Host $c.Line; $c.Context.PostContext | ForEach-Object { Write-Host $_ } }
