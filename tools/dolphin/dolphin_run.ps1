# Dolphin reference run for rendering comparisons (CLAUDE.md "Rendering bugs are checked against
# Dolphin"): a scratch Dolphin user folder set up from the templates here, a scripted Wii Remote +
# Nunchuk over DSU (dsu_server.py), window captures every -Every seconds. docs/RENDERING.md
# "Dolphin reference runs".
#   powershell -File tools\dolphin\dolphin_run.ps1 -User <scratch dir> -Script <dsu script> -Seconds <n> -Out <dir>
#     [-Every <s>] [-From <s>] [-Gecko "<code name>"] [-Fixture <nand dir>]
# - The town: the clean post-save fixture (make_fixtures.ps1 "user-town-clean": no Mr. Resetti, no
#   unsaved session), copied into the user folder's NAND before every run, so a killed run never
#   leaves a Resetti state behind.
# - -Gecko enables one code of RUUE01.ini: "Beach teleport", "Bus stop teleport" (player moved),
#   "Places warps" (make_warps.py: the port's warps in turn, as tools/routes/dolphin_places_warp.txt).
# - Only Dolphin's own PID and the DSU server are stopped. Muted, window at x 6000.
param(
    [Parameter(Mandatory)] [string]$User,
    [Parameter(Mandatory)] [string]$Script,
    [int]$Seconds = 150,
    [Parameter(Mandatory)] [string]$Out,
    [double]$Every = 5,
    [double]$From = 0,
    [string]$Gecko = "",
    [string]$Fixture = "",
    [string]$Dolphin = "C:\Users\rmelv\Desktop\games\Dolphin-x64\Dolphin.exe",
    [string]$MainDol = "C:\Users\rmelv\Desktop\Animal Crossing City Folk Recompiled\US DATA\DATA\sys\main.dol"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
if (-not $Fixture) {
    $main = $root
    if ($root -match '^(.*)\\\.claude\\worktrees\\[^\\]+$') { $main = $Matches[1] }
    $Fixture = Join-Path $main "out\fixtures\user-town-clean\nand"
}
New-Item -ItemType Directory -Force (Join-Path $User "Config"), (Join-Path $User "GameSettings"), $Out | Out-Null
# Config from the templates (Dolphin adds its own keys to them while it runs).
foreach ($f in "Dolphin.ini", "WiimoteNew.ini", "DSUClient.ini") {
    $dst = Join-Path $User "Config\$f"
    if (-not (Test-Path $dst)) { Copy-Item (Join-Path $here $f) $dst }
}
$gs = @(Get-Content (Join-Path $here "RUUE01.ini")) + "[Gecko_Enabled]"
if ($Gecko) { $gs += "`$$Gecko" }
Set-Content -Encoding ascii (Join-Path $User "GameSettings\RUUE01.ini") $gs
# The clean town save, restored before every run.
$wiiData = Join-Path $User "Wii\title\00010000\52555545\data"
New-Item -ItemType Directory -Force $wiiData | Out-Null
foreach ($f in "rvforest.dat", "banner.bin") {
    Copy-Item -Force (Join-Path $Fixture "title\00010000\52555545\data\$f") (Join-Path $wiiData $f)
}
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static IntPtr Biggest(uint pid) {
        IntPtr best = IntPtr.Zero; long area = 0;
        EnumWindows((h, p) => { uint q; GetWindowThreadProcessId(h, out q);
            if (q == pid && IsWindowVisible(h)) { RECT r; GetClientRect(h, out r); long a = (long)(r.R - r.L) * (r.B - r.T);
                if (a > area) { area = a; best = h; } } return true; }, IntPtr.Zero);
        return best;
    }
}
"@
[W]::SetProcessDPIAware() | Out-Null
$dsuLog = Join-Path $Out "dsu.log"
$dsu = Start-Process py -ArgumentList @("`"$here\dsu_server.py`"", "26761", "`"$Script`"", "$Seconds", "`"$dsuLog`"") -PassThru -WindowStyle Hidden
$proc = Start-Process $Dolphin -ArgumentList @("-u", "`"$User`"", "-b", "-e", "`"$MainDol`"") -PassThru
"dolphin pid $($proc.Id) dsu pid $($dsu.Id)" | Out-File (Join-Path $Out "pids.txt")
$sw = [Diagnostics.Stopwatch]::StartNew()
$n = 0
while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $proc.HasExited) {
    $t = $sw.Elapsed.TotalSeconds
    if ($t -ge $From) {
        $h = [W]::Biggest([uint32]$proc.Id)
        if ($h -ne [IntPtr]::Zero) {
            $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
            $w = $r.R - $r.L; $hh = $r.B - $r.T
            if ($w -gt 100) {
                $bmp = New-Object System.Drawing.Bitmap $w, $hh
                $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
                [W]::PrintWindow($h, $dc, 3) | Out-Null
                $g.ReleaseHdc($dc); $g.Dispose()
                $bmp.Save((Join-Path $Out ("d{0:D4}.png" -f [int]$t)), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
                $n++
            }
        }
    }
    Start-Sleep -Milliseconds ([int]($Every * 1000))
}
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
if (-not $dsu.HasExited) { Stop-Process -Id $dsu.Id -Force }
"captures $n"
