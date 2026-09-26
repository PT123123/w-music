# w-music verification helper: real-input clicks + pixel screenshots only.
# No UIA on purpose -- reading w-music's a11y tree crashes XAML on its own
# (see memory wm-crash-debug-technique).
param(
    [Parameter(Mandatory = $true)][string]$Mode,   # shot | click | clickrel | rect
    [string]$Path = '',
    [int]$X = 0,
    [int]$Y = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Native {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@

[Native]::SetProcessDPIAware() | Out-Null

$proc = Get-Process -Name 'w-music' -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw 'w-music process with a window not found' }
$hwnd = $proc.MainWindowHandle

if ([Native]::IsIconic($hwnd)) { [Native]::ShowWindow($hwnd, 9) | Out-Null }  # SW_RESTORE

# ALT tap unlocks SetForegroundWindow on Windows (focus-steal protection).
[Native]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[Native]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)   # KEYEVENTF_KEYUP
Start-Sleep -Milliseconds 80
[Native]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 700

$rect = New-Object Native+RECT
[Native]::GetWindowRect($hwnd, [ref]$rect) | Out-Null

if ($Mode -eq 'rect') {
    Write-Output ("pid={0} rect L={1} T={2} R={3} B={4} W={5} H={6}" -f `
        $proc.Id, $rect.L, $rect.T, $rect.R, $rect.B, ($rect.R - $rect.L), ($rect.B - $rect.T))
    exit 0
}

if ($Mode -eq 'click' -or $Mode -eq 'clickrel') {
    $cx = $X; $cy = $Y
    if ($Mode -eq 'clickrel') {
        $cx = $rect.L + $X
        $cy = $rect.T + $Y
        Write-Output ("rel {0},{1} -> abs {2},{3}" -f $X, $Y, $cx, $cy)
    }
    [Native]::SetCursorPos($cx, $cy) | Out-Null
    Start-Sleep -Milliseconds 150
    [Native]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
    Start-Sleep -Milliseconds 70
    [Native]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
    Write-Output "clicked $cx,$cy (pid $($proc.Id))"
    exit 0
}

if ($Mode -eq 'shot') {
    $w = $rect.R - $rect.L
    $h = $rect.B - $rect.T
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.L, $rect.T, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "shot saved: $Path ($w x $h) at L=$($rect.L) T=$($rect.T)"
    exit 0
}

throw "unknown mode $Mode"
