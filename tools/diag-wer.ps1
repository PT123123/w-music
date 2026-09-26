$ErrorActionPreference = 'SilentlyContinue'
$events = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1001; StartTime = (Get-Date).AddMinutes(-30) }
foreach ($e in $events) {
    if ($e.Message -match 'w-music' -and $e.Message -match '5d5d68d1|c000027b') {
        Write-Output ('===== ' + $e.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss'))
        Write-Output $e.Message.Substring(0, [Math]::Min(3500, $e.Message.Length))
    }
}
