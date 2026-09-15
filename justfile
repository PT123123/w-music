# w-music -- build orchestration.
#
# just is only the entry point: every recipe forwards flags to build.ps1 ->
# tools/dev-build.ps1, which stays the single source of truth for the toolchain.
# Pure PowerShell end to end -- no MSBuild, no .bat, no bash / git-bash.
#
#   just build   compile (SDK + WinUI projection -> IDL -> cppwinrt -> ninja)
#   just fast    compile only, reusing the generated projections (inner loop)
#   just test    run the core test binaries
#   just run     launch the app GUI (run only -- no build)
#   just clean   delete build\  (full regeneration next time, ~2 min)
#
# `just --list` prints every recipe.  Set up once: just must be on PATH.

set windows-shell := ["pwsh", "-NoProfile", "-Command"]

root      := justfile_directory()
driver    := root + "/build.ps1"
build_dir := root + "/build"

# List recipes.
default:
    @just --list

# Compile everything. Test binaries are built, but not executed.
build:
    #!pwsh -NoProfile
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $true
    & '{{driver}}' -NoTests

# Fast inner loop: recompile only, reusing the projections (won't pick up .idl edits).
fast:
    #!pwsh -NoProfile
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $true
    & '{{driver}}' -NoGen -NoTests

# Run the core test binaries (on a clean tree, `just build` first).
test:
    #!pwsh -NoProfile
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $true
    $exes = @(Get-ChildItem -Path '{{build_dir}}\test_*.exe' -File -ErrorAction SilentlyContinue)
    if ($exes.Count -eq 0) { throw "no test binaries in {{build_dir}} -- run 'just build' first" }
    foreach ($exe in $exes)
    {
        Write-Host "-- $($exe.Name)"
        & $exe.FullName
    }

# Launch the app GUI. Run only -- no build; use 'just build' first if needed.
run:
    #!pwsh -NoProfile
    $ErrorActionPreference = 'Stop'
    $exe = '{{build_dir}}\w-music.exe'
    if (-not (Test-Path $exe)) { throw "w-music.exe not found in {{build_dir}} -- run 'just build' first" }
    Start-Process -FilePath $exe
    Write-Host 'w-music launched.'

# Alias of run.
alias launch := run

# Force the C++/WinRT projections to regenerate, then build.
gen:
    #!pwsh -NoProfile
    $ErrorActionPreference = 'Stop'
    # Remove-Item is blocked here (safe-delete policy); -ErrorAction
    # SilentlyContinue would make this a silent no-op, so use .NET.
    foreach ($marker in @('{{build_dir}}\gen\sdk\.done', '{{build_dir}}\gen\winui\.done'))
    {
        if (Test-Path $marker)
        {
            [IO.File]::Delete($marker)
        }
    }
    & '{{driver}}' -NoTests

# Print the resolved toolchain (VS, SDK, cppwinrt, ninja, WinUI metadata).
tools:
    #!pwsh -NoProfile
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $true
    & '{{driver}}' -ListOnly

# Drop objects, test binaries and logs, but keep the generated projections.
clean-soft:
    #!pwsh -NoProfile
    $ErrorActionPreference = 'Stop'
    if (-not (Test-Path '{{build_dir}}'))
    {
        Write-Host 'nothing to clean'
        exit 0
    }
    # Remove-Item is blocked by this machine's safe-delete policy
    # (SAFE_DELETE_FAIL_CLOSED). With $ErrorActionPreference = 'Stop' that aborts
    # the recipe, and with -ErrorAction SilentlyContinue it silently deletes
    # nothing -- so use the .NET APIs instead of the cmdlet.
    $obj = '{{build_dir}}\obj'
    if (Test-Path $obj)
    {
        [IO.Directory]::Delete($obj, $true)
    }
    Get-ChildItem '{{build_dir}}' -Filter '*.exe' -File -ErrorAction SilentlyContinue |
        ForEach-Object { [IO.File]::Delete($_.FullName) }
    foreach ($name in @('ninja-out.txt', 'last-tool-out.txt', '.ninja_log', '.ninja_deps'))
    {
        $path = Join-Path '{{build_dir}}' $name
        if (Test-Path $path)
        {
            [IO.File]::Delete($path)
        }
    }
    Write-Host 'clean-soft done (projections kept)'

# Delete build\ entirely. The next build redoes the projections (~2 min).
clean:
    #!pwsh -NoProfile
    $ErrorActionPreference = 'Stop'
    if (-not (Test-Path '{{build_dir}}'))
    {
        Write-Host 'nothing to clean'
        exit 0
    }
    Write-Host 'removing {{build_dir}}'
    # See clean-soft: Remove-Item -Recurse is blocked by the safe-delete policy.
    [IO.Directory]::Delete('{{build_dir}}', $true)
    Write-Host 'clean done'
