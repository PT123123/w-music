#requires -Version 5.1
# Dump the internal assembly identity + referenced assemblies of the key winmds.
$out = Join-Path $PSScriptRoot '..\build\gen\xaml-full\asmident.txt'
$lines = New-Object System.Collections.Generic.List[string]
$files = @(
  'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Facade\windows.winmd',
  'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\facade\Windows.WinMD',
  'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd',
  'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd',
  'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Windows.winmd',
  'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd',
  'C:\Users\ted\Desktop\w-music\build\gen\winmd-full\w-music.winmd'
)
foreach ($f in $files) {
    $lines.Add("=== $f")
    try {
        $an = [System.Reflection.AssemblyName]::GetAssemblyName($f)
        $lines.Add("    Identity : $($an.Name), Version=$($an.Version)")
    } catch { $lines.Add("    Identity : <error> $($_.Exception.Message)") }
    try {
        # Metadata-only inspection via System.Reflection.Metadata if present
        Add-Type -AssemblyName System.Reflection.Metadata -ErrorAction SilentlyContinue | Out-Null
        $fs = [IO.File]::OpenRead($f)
        try {
            $pe = [System.Reflection.PortableExecutable.PEReader]::new($fs)
            $md = [System.Reflection.Metadata.PEReaderExtensions]::GetMetadataReader($pe)
            foreach ($h in $md.AssemblyReferences) {
                $ar = $md.GetAssemblyReference($h)
                $lines.Add("    -> ref $($md.GetString($ar.Name)) Ver=$($ar.Version)")
            }
            $pe.Dispose()
        } finally { $fs.Dispose() }
    } catch { $lines.Add("    refs   : <error> $($_.Exception.Message)") }
}
[IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote $out"
