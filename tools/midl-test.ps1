#requires -Version 5.1
# Isolate test: can midlrt compile App.idl / MainWindow.idl with the current
# WindowsAppSDK 2.3.0 metadata? These are currently marked "optional/skipped"
# in dev-build.ps1. Import the cached VS dev env and try.
param()
$ErrorActionPreference = 'Stop'
$Transcript = Join-Path $PSScriptRoot '..\build\gen\xaml-probe2\midl-test.log'
Start-Transcript -Path $Transcript -Force | Out-Null

$Root   = Split-Path -Parent $PSScriptRoot
$SrcDir = Join-Path $Root 'src\w-music'
$BuildDir = Join-Path $Root 'build'
$toolEnvCache = Join-Path $BuildDir 'toolenv.json'
$saved = Get-Content $toolEnvCache -Raw | ConvertFrom-Json
foreach ($name in @('PATH','INCLUDE','LIB','LIBPATH')) {
    Set-Item -Path "env:$name" -Value ([string]$saved.$name)
}
$midlrt = (Get-Command midlrt.exe -ErrorAction Stop).Source
Write-Host "midlrt  : $midlrt"

$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -property installationPath
$sdkVersion = (Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$unionMetadata = "C:\Program Files (x86)\Windows Kits\10\UnionMetadata\$sdkVersion"
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$nugetRoot = "$env:USERPROFILE\.nuget\packages"

function Newest-PackageDir([string]$id) {
    Get-ChildItem "$nugetRoot\$id" -Directory |
        Where-Object { $_.Name -notmatch '-' } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
}
$wasdkRefs = @()
$winuiPkgDir = Newest-PackageDir 'microsoft.windowsappsdk.winui'
$wasdkRefs += @(Get-ChildItem (Join-Path $winuiPkgDir.FullName 'metadata') -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
foreach ($pkgId in @('microsoft.windowsappsdk.interactiveexperiences','microsoft.windowsappsdk.foundation')) {
    $pkg = Newest-PackageDir $pkgId
    if (-not $pkg) { continue }
    $metaRoot = Join-Path $pkg.FullName 'metadata'
    $metaDir = Get-ChildItem $metaRoot -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if ($metaDir) { $metaRoot = $metaDir.FullName }
    $wasdkRefs += @(Get-ChildItem $metaRoot -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
}
$webview2Pkg = Get-ChildItem "$nugetRoot\microsoft.web.webview2" -Directory -ErrorAction SilentlyContinue | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if ($webview2Pkg) { $wasdkRefs += @(Get-ChildItem (Join-Path $webview2Pkg.FullName 'lib') -Filter '*.winmd' -Recurse | Select-Object -ExpandProperty FullName) }

$srcOut = Join-Path $BuildDir 'gen\midltest'
New-Item -ItemType Directory -Force -Path $srcOut | Out-Null
foreach ($idl in @('App.idl','MainWindow.idl')) {
    Write-Host "=== midlrt $idl ==="
    $outWinmd = Join-Path $srcOut ([IO.Path]::GetFileNameWithoutExtension($idl) + '.winmd')
    $outHdr   = Join-Path $srcOut ([IO.Path]::GetFileNameWithoutExtension($idl) + '.h')
    $args = @("$SrcDir\$idl", '/nologo', '/W1', '/nomidl', '/metadata_dir', $unionMetadata)
    foreach ($w in $wasdkRefs) { $args += @('/reference', $w) }
    $args += @('/reference', (Join-Path $unionMetadata 'Windows.winmd'))
    $args += @('/I', $SrcDir, '/I', (Join-Path $sdkRoot "Include\$sdkVersion\winrt"), '/winmd', $outWinmd, '/h', $outHdr)
    $midlLog = Join-Path $srcOut ([IO.Path]::GetFileNameWithoutExtension($idl) + '.midl.log')
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $midlrt @args *>&1 | Out-File -FilePath $midlLog -Encoding utf8 -Width 4096 } finally { $ErrorActionPreference = $prev }
    Write-Host "   exit=$LASTEXITCODE"
    if (Test-Path $outWinmd) { Write-Host "   OK winmd: $outWinmd" } else { Write-Host "   NO winmd produced" }
    Write-Host "   --- midlrt output ---"
    Get-Content $midlLog | ForEach-Object { Write-Host "   $_" }
}
