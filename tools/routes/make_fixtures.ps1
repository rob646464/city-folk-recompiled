# Rebuild the shared fixture saves from scratch (never committed; out/ is gitignored):
#   powershell -File tools\routes\make_fixtures.ps1 [-Only town-new] [-Out <dir>]
# Each fixture = a route that ends with the game's own save, run with run.ps1's deterministic
# aids (CF_RNG_SEED, CF_CLOCK_START, CF_CLOCK_FRAMES: the same town every time); the run's NAND
# folder is copied to <Out>\<name>\nand when rvforest.dat was written. Use one with
# CF_FIXTURE=<name> (CLAUDE.md "Test runs").
param(
    [string]$Only = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Out) {
    # The main checkout's out\fixtures (worktrees live in <main>\.claude\worktrees\<name>).
    $main = $root
    if ($root -match '^(.*)\\\.claude\\worktrees\\[^\\]+$') { $main = $Matches[1] }
    $Out = Join-Path $main "out\fixtures"
}
# NOTE (2026-10-28): tools\routes\first_save.txt (new game -> town hall -> Tom Nook's house -> HUD
# Save -> "Save and quit") is not written yet; until it is, this script stops with "no route".
$fixtures = @(
    # name, route, seconds
    @{ Name = "town-new"; Route = "tools\routes\first_save.txt"; Seconds = 900 }
)
foreach ($f in $fixtures) {
    if ($Only -and $f.Name -ne $Only) { continue }
    if (-not (Test-Path (Join-Path $root $f.Route))) { throw "$($f.Name): no route $($f.Route) yet" }
    $run = "fixture-$($f.Name)"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run.ps1") -Name $run -Route (Join-Path $root $f.Route) -Seconds $f.Seconds -Env "CF_SAVE_LOG=1"
    $nand = Join-Path $root "build\runs\$run\nand"
    $save = Join-Path $nand "title\00010000\52555545\data\rvforest.dat"
    if (-not (Test-Path $save)) { throw "$($f.Name): no rvforest.dat written (see build\runs\$run\log.txt)" }
    $dst = Join-Path $Out "$($f.Name)\nand"
    if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item -Recurse $nand $dst
    Remove-Item -Recurse -Force (Join-Path $dst "tmp") -ErrorAction SilentlyContinue
    "$($f.Name): $((Get-Item $save).Length) bytes -> $dst"
}
