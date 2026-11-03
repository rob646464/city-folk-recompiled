# Configure and build from a plain PowerShell by importing the MSVC developer environment.
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Preset windows-clang] [-Configure]
param(
    [string]$Preset = "windows-clang",
    [switch]$Configure,
    [string]$Target = "",
    [string]$Disc = "",  # sets CF_DISC (extracted disc folder or image) when configuring
    # Developer build of another version (RUUP01 / RUUJ01 / RUUK01; docs/SETUP.md "Version-aware
    # code"): its own build folder build/<preset>-<id>, CF_GAME_ID=<id>, -Disc that version's disc
    # (needed the first time).
    [string]$Game = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Visual Studio C++ tools not found" }

# Import vcvars64 into this process.
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
# CMake/Ninja installed by winget may not be on PATH in older shells.
foreach ($p in @("$env:ProgramFiles\CMake\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links")) {
    if (Test-Path $p) { $env:PATH = "$p;$env:PATH" }
}

Push-Location $root
try {
    if ($Game) {
        $dir = "build/$Preset-$Game"
        if ($Configure -or $Disc -or -not (Test-Path "$dir/build.ninja")) {
            if (-not $Disc -and -not (Test-Path "$dir/CMakeCache.txt")) { throw "-Game needs -Disc the first time" }
            $cfg = @("--preset", $Preset, "-B", $dir, "-DCF_GAME_ID=$Game")
            if ($Disc) { $cfg += "-DCF_DISC=$($Disc -replace '\\', '/')" }
            cmake @cfg
            if ($LASTEXITCODE) { throw "configure failed" }
        }
        if ($Target) { cmake --build $dir --target $Target } else { cmake --build $dir --target cfrecomp }
        if ($LASTEXITCODE) { throw "build failed" }
        return
    }
    if ($Configure -or $Disc -or -not (Test-Path "build/$Preset/build.ninja")) {
        if ($Disc) { cmake --preset $Preset "-DCF_DISC=$($Disc -replace '\\', '/')" } else { cmake --preset $Preset }
        if ($LASTEXITCODE) { throw "configure failed" }
    }
    if ($Target) { cmake --build --preset $Preset --target $Target } else { cmake --build --preset $Preset }
    if ($LASTEXITCODE) { throw "build failed" }
} finally {
    Pop-Location
}
