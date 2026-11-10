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
    [string]$Disc = "C:\Users\rmelv\Desktop\Animal Crossing City Folk Recompiled\US DATA\DATA",
    # Build folder under build/ (another version's dev build: windows-clang-RUUP01 etc., made by
    # tools\build.ps1 -Game <id>; pass that version's -Disc too).
    [string]$Build = "windows-clang",
    # settings.ini lines written into the scratch APPDATA before the start, "[audio]|output=3|..."
    # ('|' separates lines; for settings the game reads at boot, which CF_SETTINGS_AT is too late for).
    [string]$Settings = "",
    # A whole settings.ini copied into the scratch APPDATA instead (e.g. a copy of a user's report).
    [string]$SettingsFile = "",
    # Executable in the build folder (e.g. a copy of an older build for before/after runs).
    [string]$Exe = "cfrecomp.exe",
    # Extra command-line arguments for the game, space-separated (e.g. "--window 3840x2160").
    [string]$ExtraArgs = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$run = Join-Path $root "build\runs\$Name"
foreach ($d in @("appdata", "card", "shots")) {
    $p = Join-Path $run $d
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    New-Item -ItemType Directory -Force $p | Out-Null
}
if ($Settings -or $SettingsFile) {
    $sd = Join-Path $run "appdata\Animal Crossing City Folk Recompiled"
    New-Item -ItemType Directory -Force $sd | Out-Null
    if ($SettingsFile) { Copy-Item $SettingsFile (Join-Path $sd "settings.ini") }
    else { ($Settings -split '\|') -join "`n" | Out-File -Encoding ascii (Join-Path $sd "settings.ini") }
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
    SDL_JOYSTICK_DIRECTINPUT = "0"; SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS = "0";
    # Same town and clock every run (routes walk the town; -Env "CF_RNG_SEED=" for the real thing).
    CF_RNG_SEED = "12345678"; CF_CLOCK_START = "2026-10-27 10:00"; CF_CLOCK_FRAMES = "1"
}
if ($Shots) { $vars.CF_SHOT = "$(Join-Path $run 'shots'),$Shots" }
foreach ($kv in ($Env -split ';')) {
    if ($kv -match '^([^=]+)=(.*)$') { $vars[$Matches[1]] = $Matches[2] }
}
# Set for this run only: the previous values come back afterwards, so a second run.ps1 call in the
# same PowerShell (a batch of runs) does not inherit this run's -Env (CF_FIXTURE, CF_POKE, ...).
$savedEnv = @{}
foreach ($k in $vars.Keys) {
    $savedEnv[$k] = [Environment]::GetEnvironmentVariable($k)
    Set-Item -Path "env:$k" -Value $vars[$k]
}

$exe = Join-Path $root "build\$Build\$Exe"
$argList = @("--disc", $Disc)
if ($ExtraArgs) { $argList += ($ExtraArgs -split " ") }
if ($Route) { $argList += @("--input", "@$((Resolve-Path $Route).Path)") }
elseif ($Script) { $argList += @("--input", $Script) }
$log = Join-Path $run "log.txt"
$sw = [Diagnostics.Stopwatch]::StartNew()
$ErrorActionPreference = "Continue"  # stderr lines arrive as error records
& $exe @argList 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
$code = $LASTEXITCODE
foreach ($k in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($k, $savedEnv[$k]) }
"exit $code after $([int]$sw.Elapsed.TotalSeconds) s, log $log"
