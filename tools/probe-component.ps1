#requires -Version 5.1
# Regenerate cppwinrt -component into a scratch dir and inspect the page .g.cpp,
# to learn how the real toolchain wires the XAML .g.hpp into the build.
param()
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-full\probe-component.log'
Start-Transcript -Path $log -Force | Out-Null

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$out = Join-Path $BuildDir 'gen\component-probe'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $out | Out-Null

$saved = Get-Content (Join-Path $BuildDir 'toolenv.json') -Raw | ConvertFrom-Json
foreach ($n in @('PATH','INCLUDE','LIB','LIBPATH')) { Set-Item -Path "env:$n" -Value ([string]$saved.$n) }

$nugetRoot = "$env:USERPROFILE\.nuget\packages"
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = (Get-ChildItem "$sdkRoot\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$cppwinrtPkg = Get-ChildItem "$nugetRoot\microsoft.windows.cppwinrt" -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$cppwinrt = Join-Path $cppwinrtPkg.FullName 'bin\cppwinrt.exe'

function Newest-PackageDir([string]$id) { Get-ChildItem "$nugetRoot\$id" -Directory | Where-Object { $_.Name -notmatch '-' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 }
$wasdkRefs = @()
$winuiPkgDir = Newest-PackageDir 'microsoft.windowsappsdk.winui'
$wasdkRefs += @(Get-ChildItem (Join-Path $winuiPkgDir.FullName 'metadata') -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
foreach ($pkgId in @('microsoft.windowsappsdk.interactiveexperiences','microsoft.windowsappsdk.foundation')) {
    $pkg = Newest-PackageDir $pkgId; if (-not $pkg) { continue }
    $metaRoot = Join-Path $pkg.FullName 'metadata'
    $d = Get-ChildItem $metaRoot -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if ($d) { $metaRoot = $d.FullName }
    $wasdkRefs += @(Get-ChildItem $metaRoot -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
}
$sdkWinmds = @(Get-ChildItem (Join-Path $sdkRoot "References\$sdkVersion") -Recurse -Filter '*.winmd' | Select-Object -ExpandProperty FullName)
$localWinmds = @((Join-Path $BuildDir 'gen\winmd-full\w-music.winmd'))
if (-not (Test-Path $localWinmds[0])) { Write-Host "no w-music.winmd; run xc-final first"; Stop-Transcript | Out-Null; return }

$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
& $cppwinrt -component -in $localWinmds -ref ($sdkWinmds + $wasdkRefs) -out $out 2>&1 | Out-File (Join-Path $out 'cppwinrt.log') -Encoding utf8
$ErrorActionPreference = $prev
Write-Host "cppwinrt exit=$LASTEXITCODE"
Write-Host "--- files ---"
Get-ChildItem $out -Recurse -File | ForEach-Object { Write-Host "  $($_.FullName.Substring($out.Length+1))" }
foreach ($f in @('Views\DiscoverPage.g.cpp','DiscoverPage.g.cpp','w_music\DiscoverPage.g.h')) {
    $p = Join-Path $out $f
    if (Test-Path $p) {
        Write-Host "===== $f (first 40 lines) ====="
        Get-Content $p | Select-Object -First 40 | ForEach-Object { Write-Host "   $_" }
    }
}
Stop-Transcript | Out-Null
