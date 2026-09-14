#requires -Version 5.1
# For each candidate winmd, report whether it defines (TypeDef) or forwards (ExportedType)
# the three conflicting types, and list its top-level namespaces count.
$toolDir = 'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\tools\net472'
$out = Join-Path $PSScriptRoot '..\build\gen\xaml-full\mdtypedefs.txt'
$lines = New-Object System.Collections.Generic.List[string]
Add-Type -Path (Join-Path $toolDir 'System.Collections.Immutable.dll') -ErrorAction Stop
Add-Type -Path (Join-Path $toolDir 'System.Reflection.Metadata.dll') -ErrorAction Stop

$targets = @('Windows.UI.Xaml.DependencyObject','Windows.UI.Xaml.Controls.TreeViewNode','Windows.UI.Core.CoreDispatcher','Windows.Foundation.Collections.IVectorView`1','Windows.Foundation.IAsyncAction')
$files = @(
  'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Windows.winmd',
  'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Facade\windows.winmd',
  'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.UniversalApiContract\19.0.0.0\Windows.Foundation.UniversalApiContract.winmd',
  'C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd',
  'C:\Users\ted\.nuget\packages\microsoft.windowsappsdk.winui\2.3.0\metadata\Microsoft.UI.Xaml.winmd'
)
foreach ($f in $files) {
    $lines.Add("=== $([IO.Path]::GetFileName($f))")
    try {
        $fs = [IO.File]::OpenRead($f); $pe = New-Object System.Reflection.PortableExecutable.PEReader($fs); $md = [System.Reflection.Metadata.PEReaderExtensions]::GetMetadataReader($pe)
        $defs = New-Object 'System.Collections.Generic.HashSet[string]'
        foreach ($h in $md.TypeDefinitions) {
            $td = $md.GetTypeDefinition($h)
            $ns = $md.GetString($td.Namespace); $nm = $md.GetString($td.Name)
            [void]$defs.Add("$ns.$nm")
        }
        $fwd = New-Object 'System.Collections.Generic.HashSet[string]'
        foreach ($h in $md.ExportedTypes) {
            $et = $md.GetExportedType($h)
            $ns = $md.GetString($et.Namespace); $nm = $md.GetString($et.Name)
            [void]$fwd.Add("$ns.$nm")
        }
        foreach ($t in $targets) {
            $d = if ($defs.Contains($t)) { 'DEF' } else { '-' }
            $x = if ($fwd.Contains($t)) { 'FWD' } else { '-' }
            $lines.Add("  $d $x  $t")
        }
        $lines.Add("  (total TypeDefs=$($md.TypeDefinitions.Count))")
        $pe.Dispose(); $fs.Dispose()
    } catch { $lines.Add("  <error> $($_.Exception.Message)") }
}
[IO.File]::WriteAllLines($out, $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote $out"
