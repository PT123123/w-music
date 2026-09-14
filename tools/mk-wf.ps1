#requires -Version 5.1
# Create a synthetic assembly whose INTERNAL name is "Windows.Foundation" by
# round-tripping FoundationContract.winmd -> idl -> winmd (midlrt names the
# assembly after the input idl file stem).
param()
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '..\build\gen\xaml-full\mk-wf.log'
Start-Transcript -Path $log -Force | Out-Null

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$GenDir = Join-Path $BuildDir 'gen\xaml-full'
$wfDir = Join-Path $BuildDir 'gen\wf'
Remove-Item $wfDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $wfDir | Out-Null

$saved = Get-Content (Join-Path $BuildDir 'toolenv.json') -Raw | ConvertFrom-Json
foreach ($n in @('PATH','INCLUDE','LIB','LIBPATH')) { Set-Item -Path "env:$n" -Value ([string]$saved.$n) }

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = (Get-ChildItem "$sdkRoot\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$refsRoot = "$sdkRoot\References\$sdkVersion"
$unionMetadata = "$sdkRoot\UnionMetadata\$sdkVersion"
$fc = Join-Path $refsRoot 'Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd'
$uc = Join-Path $refsRoot 'Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd'
$winmdidl = "$sdkRoot\bin\$sdkVersion\x64\winmdidl.exe"
$midlrt = (Get-Command midlrt.exe -ErrorAction Stop).Source

# 1) winmd -> idl
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
& $winmdidl "/outdir:$wfDir" $fc 2>&1 | Out-File (Join-Path $wfDir 'winmdidl.log') -Encoding utf8
$ErrorActionPreference = $prev
Write-Host "winmdidl exit=$LASTEXITCODE"
Get-ChildItem $wfDir | ForEach-Object { Write-Host "   $($_.Name)" }

# 2) winmdidl already emits "Windows.Foundation.idl" (named from the namespace root)
$wfIdl = Join-Path $wfDir 'Windows.Foundation.idl'
if (-not (Test-Path $wfIdl)) { Write-Host "NO Windows.Foundation.idl PRODUCED"; Stop-Transcript | Out-Null; return }
Write-Host "using $wfIdl"
Write-Host "---- idl head ----"
Get-Content $wfIdl | Select-Object -First 20 | ForEach-Object { Write-Host "   $_" }

# 3) idl -> winmd (internal assembly name = "Windows.Foundation")
$wfWinmd = Join-Path $wfDir 'Windows.Foundation.winmd'
$wfHdr = Join-Path $wfDir 'Windows.Foundation.h'
$a = @($wfIdl, '/nologo', '/W1', '/nomidl', '/metadata_dir', $unionMetadata,
       '/reference', $fc, '/reference', $uc,
       '/I', $wfDir, '/I', "$sdkRoot\Include\$sdkVersion\winrt",
       '/winmd', $wfWinmd, '/h', $wfHdr)
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
try { & $midlrt @a *>&1 | Out-File (Join-Path $wfDir 'midlrt.log') -Encoding utf8 } finally { $ErrorActionPreference = $prev }
Write-Host "midlrt WF exit=$LASTEXITCODE winmd? $(Test-Path $wfWinmd)"
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $wfDir 'midlrt.log') | Where-Object { $_ -match 'error' } | Select-Object -First 8 | ForEach-Object { Write-Host "   $_" } }

# 4) verify internal identity
if (Test-Path $wfWinmd) {
    $an = [System.Reflection.AssemblyName]::GetAssemblyName($wfWinmd)
    Write-Host "   WF identity: $($an.Name) $($an.Version)"
}
Stop-Transcript | Out-Null
