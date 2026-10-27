# Test run of a route with scratch folders (never the user's settings/saves), muted, off screen,
# physical controllers hidden. Run folder: build/runs/<Name> (appdata, card, nand, shots, log.txt).
#   powershell -File tools\routes\run.ps1 -Name r1 -Route tools\routes\new_game.txt -Seconds 200
#     [-Shots "first,last,step"] [-Nand <dir>] [-KeepNand] [-Env "CF_IOS_LOG=1;CF_WPAD_LOG=1"]
# -Nand shares a NAND folder between runs (first save, then a restart that loads it).
param(
    [Parameter(Mandatory)] [string]$Name,
    [string]$Route = "",
    [string]$Script = "",   # inline script instead of a route file (not named Input: see docs/CONTROLS.md)
    [int]$Seconds = 200,
    [string]$Shots = "",
    [string]$Nand = "",
    [switch]$KeepNand,
    [string]$Env = "",
    [string]$Disc = "C:\Users\rmelv\Desktop\Animal Crossing City Folk Recompiled\US DATA\DATA"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$run = Join-Path $root "build\runs\$Name"
foreach ($d in @("appdata", "card", "shots")) {
    $p = Join-Path $run $d
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    New-Item -ItemType Directory -Force $p | Out-Null
}
if (-not $Nand) { $Nand = Join-Path $run "nand" }
if (-not $KeepNand -and (Test-Path $Nand) -and -not $PSBoundParameters.ContainsKey("Nand")) { Remove-Item -Recurse -Force $Nand }
New-Item -ItemType Directory -Force $Nand | Out-Null
$cache = Join-Path $root "build\runs\cache"
New-Item -ItemType Directory -Force $cache | Out-Null

$vars = @{
    APPDATA = (Join-Path $run "appdata"); CF_CACHE_DIR = $cache; CF_CARD_DIR = (Join-Path $run "card");
    CF_NAND_DIR = $Nand; CF_OFFSCREEN = "1"; CF_BACKGROUND = "1"; CF_AUDIO = "0"; CF_QUIT_AFTER = "$Seconds";
    CF_INPUT_CLOCK = "frames"; AURORA_FIFO_INLINE = "1";
    SDL_JOYSTICK_HIDAPI = "0"; SDL_JOYSTICK_RAWINPUT = "0"; SDL_JOYSTICK_WGI = "0"; SDL_XINPUT_ENABLED = "0";
    SDL_JOYSTICK_DIRECTINPUT = "0"; SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS = "0"
}
if ($Shots) { $vars.CF_SHOT = "$(Join-Path $run 'shots'),$Shots" }
foreach ($kv in ($Env -split ';')) {
    if ($kv -match '^([^=]+)=(.*)$') { $vars[$Matches[1]] = $Matches[2] }
}
foreach ($k in $vars.Keys) { Set-Item -Path "env:$k" -Value $vars[$k] }

$exe = Join-Path $root "build\windows-clang\cfrecomp.exe"
$argList = @("--disc", $Disc)
if ($Route) { $argList += @("--input", "@$((Resolve-Path $Route).Path)") }
elseif ($Script) { $argList += @("--input", $Script) }
$log = Join-Path $run "log.txt"
$sw = [Diagnostics.Stopwatch]::StartNew()
$ErrorActionPreference = "Continue"  # stderr lines arrive as error records
& $exe @argList 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
"exit $LASTEXITCODE after $([int]$sw.Elapsed.TotalSeconds) s, log $log"
