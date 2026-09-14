#requires -Version 5.1
# Dump AssemblyRef table + TypeRefs' resolution scopes for key winmds, using the
# System.Reflection.Metadata shipped with the WinUI tools.
$toolDir = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\net472'
$out = Join-Path $PSScriptRoot '..\build\gen\xaml-full\mdrefs.txt'
$lines = New-Object System.Collections.Generic.List[string]
try {
    Add-Type -Path (Join-Path $toolDir 'System.Collections.Immutable.dll') -ErrorAction Stop
    Add-Type -Path (Join-Path $toolDir 'System.Reflection.Metadata.dll') -ErrorAction Stop
} catch { $lines.Add("Add-Type failed: $($_.Exception.Message)") }

$files = @(
  'C:\Users\ted\Desktop\w-music\build\gen\winmd-full\w-music.winmd',
  'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd',
  'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Facade\windows.winmd',
  'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd'
)
foreach ($f in $files) {
    $lines.Add("=== $f")
    try {
        $fs = [IO.File]::OpenRead($f)
        try {
            $pe = New-Object System.Reflection.PortableExecutable.PEReader($fs)
            $md = [System.Reflection.Metadata.PEReaderExtensions]::GetMetadataReader($pe)
            $lines.Add("  AssemblyRefs:")
            foreach ($h in $md.AssemblyReferences) {
                $ar = $md.GetAssemblyReference($h)
                $lines.Add("    $($md.GetString($ar.Name)), Version=$($ar.Version)")
            }
            $pe.Dispose()
        } finally { $fs.Dispose() }
    } catch { $lines.Add("  <error> $($_.Exception.Message)") }
}
[IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote $out"
