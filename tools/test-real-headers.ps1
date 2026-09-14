#requires -Version 5.1
# Verification: replace the generated .xaml.g.h shims with the REAL markup-compiler
# output and rebuild with ninja. Proves the real codegen is drop-in compatible with
# the existing code-behind + ninja pipeline.
param()
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-full\test-real-headers.log'
Start-Transcript -Path $log -Force | Out-Null

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$real = Join-Path $BuildDir 'gen\xaml-full'
$dst = Join-Path $BuildDir 'gen\component\w_music'

$saved = Get-Content (Join-Path $BuildDir 'toolenv.json') -Raw | ConvertFrom-Json
foreach ($n in @('PATH','INCLUDE','LIB','LIBPATH')) { Set-Item -Path "env:$n" -Value ([string]$saved.$n) }

$copy = @('App.xaml.g.h','MainWindow.xaml.g.h','DiscoverPage.xaml.g.h','LibraryPage.xaml.g.h',
          'NowPlayingPage.xaml.g.h','OnlinePage.xaml.g.h',
          'XamlBindingInfo.xaml.g.h','XamlTypeInfo.xaml.g.h','XamlMetaDataProvider.h')
foreach ($f in $copy) {
    $src = Join-Path $real $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $dst $f) -Force; Write-Host "  copied $f" }
    else { Write-Host "  MISSING $f" }
}

$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$ninja = Get-ChildItem (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake') -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
Write-Host "ninja: $ninja"
$ninjaLog = Join-Path $BuildDir 'ninja-real.txt'
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
& $ninja -C $BuildDir *>&1 | Out-File $ninjaLog -Encoding utf8
$code = $LASTEXITCODE
$ErrorActionPreference = $prev
Write-Host "==== ninja exit=$code ===="
Get-Content $ninjaLog | ForEach-Object { Write-Host "  $_" }
Stop-Transcript | Out-Null
