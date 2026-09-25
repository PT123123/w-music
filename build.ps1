#requires -Version 5.1
<#
.SYNOPSIS
    w-music build entry point: Ninja + PowerShell. No MSBuild, no .bat.

.DESCRIPTION
    Thin wrapper so the documented `.\build.ps1` keeps working. All the real
    work lives in tools\dev-build.ps1: locate VS/SDK/NuGet tools, enter the VS
    dev shell (pure PowerShell, no vcvars.bat), generate the C++/WinRT
    projections, midlrt-compile the app IDL, emit build\build.ninja and run it.

    MSIX packaging is not part of this loop: it still needs MSBuild plus the VS
    "C++ v143 UWP tools" component, which this machine does not have.

.NOTES
    Usage:  .\build.ps1 [-Clean] [-NoGen] [-NoTests] [-Release] [-ListOnly]

    -Release is the optimized configuration `just workshop-deploy` deploys; the
    default (no switch) is the unoptimized debug build, same as always.
#>
param(
    [switch]$Clean,
    [switch]$NoGen,
    [switch]$NoTests,
    [switch]$Release,
    [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'

$devBuild = Join-Path $PSScriptRoot 'tools\dev-build.ps1'

try {
    & $devBuild -Clean:$Clean -NoGen:$NoGen -NoTests:$NoTests -Release:$Release -ListOnly:$ListOnly
    # dev-build.ps1 reports failure by calling `exit 1` from its trap, and `exit`
    # inside a script invoked with & only ends *that* script: control comes back
    # here with $LASTEXITCODE set. Without this check the `exit 0` below would turn
    # a failed compile into a successful one, and callers (`just workshop-deploy`)
    # would happily go on to deploy whatever stale exe happens to be in build\.
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    exit 0
}
catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
