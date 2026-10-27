# Launches lmport.exe (or -Exe cfrecomp), waits, and saves a PNG of its window (for checking rendering).
#   powershell -ExecutionPolicy Bypass -File tools\capture_window.ps1 -Seconds 8 -Out build\shot.png [-ExeArgs "--foo"]
param(
    [int]$Seconds = 8,
    [string]$Out = "build\shot.png",
    [string]$ExeArgs = "",
    [switch]$KeepRunning,
    [string]$Exe = "lmport",
    [string]$Keys = ""   # SendKeys string pressed before capturing, e.g. "{F1}"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@
[Win]::SetProcessDPIAware() | Out-Null

$argList = if ($ExeArgs) { $ExeArgs } else { $null }
$exePath = ".\build\windows-clang\$Exe.exe"
$p = if ($argList) { Start-Process $exePath -ArgumentList $argList -PassThru `
        -RedirectStandardOutput build\run.log -RedirectStandardError build\err.log }
     else { Start-Process $exePath -PassThru `
        -RedirectStandardOutput build\run.log -RedirectStandardError build\err.log }
Start-Sleep -Seconds $Seconds
$p.Refresh()
if ($p.HasExited) { throw ("$Exe exited early with 0x{0:X}" -f $p.ExitCode) }
$h = $p.MainWindowHandle
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
if ($Keys) {
    (New-Object -ComObject WScript.Shell).SendKeys($Keys)
    Start-Sleep -Milliseconds 700
}
$r = New-Object Win+RECT
[Win]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object Win+POINT
[Win]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = $r.R - $r.L; $hgt = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
# PrintWindow (client area, full DWM content) captures this window even when other
# windows overlap it, unlike a screen copy.
$hdc = $g.GetHdc()
[Win]::PrintWindow($h, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc)
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out (${w}x$hgt)"
if (-not $KeepRunning) { Stop-Process $p -Force }
