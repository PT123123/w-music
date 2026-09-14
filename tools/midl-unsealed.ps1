#requires -Version 5.1
param()
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-probe2\unsealed.log'
Start-Transcript -Path $log -Force | Out-Null
$Root = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$BuildDir = Join-Path $Root 'build'
$saved = Get-Content (Join-Path $BuildDir 'toolenv.json') -Raw | ConvertFrom-Json
foreach ($n in @('PATH','INCLUDE','LIB','LIBPATH')) { Set-Item -Path "env:$n" -Value ([string]$saved.$n) }
$midlrt = (Get-Command midlrt.exe -ErrorAction Stop).Source
$sdkVersion = (Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$unionMetadata = "C:\Program Files (x86)\Windows Kits\10\UnionMetadata\$sdkVersion"
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$nugetRoot = "$env:USERPROFILE\.nuget\packages"
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
$srcOut = Join-Path $BuildDir 'gen\midltest2'
New-Item -ItemType Directory -Force -Path $srcOut | Out-Null
$idl = Join-Path $BuildDir 'gen\xaml-probe2\App.test.idl'
$outWinmd = Join-Path $srcOut 'App.test.winmd'
$outHdr = Join-Path $srcOut 'App.test.h'
$args = @($idl, '/nologo', '/W1', '/nomidl', '/metadata_dir', $unionMetadata)
foreach ($w in $wasdkRefs) { $args += @('/reference', $w) }
$args += @('/reference', (Join-Path $unionMetadata 'Windows.winmd'))
$args += @('/I', $SrcDir, '/I', (Join-Path $sdkRoot "Include\$sdkVersion\winrt"), '/winmd', $outWinmd, '/h', $outHdr)
$midlLog = Join-Path $srcOut 'App.test.midl.log'
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try { & $midlrt @args *>&1 | Out-File -FilePath $midlLog -Encoding utf8 -Width 4096 } finally { $ErrorActionPreference = $prev }
Write-Host "exit=$LASTEXITCODE"
Write-Host "winmd? $(Test-Path $outWinmd)"
Get-Content $midlLog | ForEach-Object { if ($_ -match 'error|warning') { Write-Host "  $_" } }
