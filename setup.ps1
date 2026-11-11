<#
.SYNOPSIS
  Builds the Godzilla: Save the Earth (Xbox) recompilation from your own game disc.

.DESCRIPTION
  Nothing from the game ships with this repository. This script:
    1. fetches the xboxrecomp toolkit (pinned version) and applies patches\xboxrecomp-v0.17.0.patch,
       and SDL3 (controllers) and Recomp Achievements (pinned versions) into third_party\
    2. extracts your disc image (or copies your extracted folder) into game\GodzillaSTE
    3. translates the game's default.xbe into C (src\recomp\gen)
    4. builds the patched menu bundles in mods\ from your ControlRoom.bdl
    5. compiles build\Release\godzilla_ste_recomp.exe

  Requirements: Git, Python 3.10+ (py launcher) with capstone (pip install capstone),
  Visual Studio 2022 or newer (or Build Tools) with "Desktop development with C++".

.EXAMPLE
  .\setup.ps1 -Iso "D:\dumps\Godzilla - Save the Earth.iso"
.EXAMPLE
  .\setup.ps1 -GameDir "D:\dumps\GodzillaSTE"      # an already extracted disc (has default.xbe)
.EXAMPLE
  .\setup.ps1                                       # game\GodzillaSTE already in place: regenerate + build
#>
param(
    [string]$Iso,
    [string]$GameDir,
    [string]$ToolkitDir = (Join-Path $PSScriptRoot "third_party\xboxrecomp"),
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$Game = Join-Path $Root "game\GodzillaSTE"
$ToolkitRepo = "https://github.com/sp00nznet/xboxrecomp.git"
$ToolkitCommit = "343b6d8"                      # v0.17.0
$Patch = Join-Path $Root "patches\xboxrecomp-v0.17.0.patch"
$KnownXbeSha1 = "c2ef3448786662e5797a1d9e545b3859967c01ed"   # Godzilla: Save the Earth (USA, Europe)
$Sdl3Version = "3.4.18"
$Sdl3Sha256 = "78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd"   # SDL3-devel-3.4.18-VC.zip
$AchRepo = "https://github.com/elripalda/RecompAchievements.git"
$AchCommit = "9a23d1f749ae4586f3bdd166464943454fa4dc1a"

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }
# Windows PowerShell 5.1 turns anything a program writes to stderr into an error
# record, which "Stop" makes fatal (git prints progress there). Programs are
# judged by their exit code only.
function Run($exe, [string[]]$argv) {
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $exe @argv 2>&1 | ForEach-Object { "$_" } } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$exe $($argv -join ' ') failed (exit $LASTEXITCODE)" }
}
function Try-Run($exe, [string[]]$argv) {
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $exe @argv 2>&1 | Out-Null } finally { $ErrorActionPreference = $prev }
    return ($LASTEXITCODE -eq 0)
}

# -- prerequisites ------------------------------------------------------------
Step "Checking tools"
foreach ($t in @("git", "py")) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t was not found on PATH." }
}
if (-not (Try-Run py @("-3", "-c", "import capstone"))) { throw "Python package 'capstone' is missing: run  py -3 -m pip install capstone" }
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $c = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if ($vs -and (Test-Path $c)) { $cmake = $c }
    }
}
if (-not $cmake -and -not $SkipBuild) { throw "CMake not found. Install Visual Studio (C++ workload) or CMake." }

# -- toolkit ------------------------------------------------------------------
Step "xboxrecomp toolkit ($ToolkitCommit + patch)"
if (-not (Test-Path (Join-Path $ToolkitDir ".git"))) {
    Run git @("clone", $ToolkitRepo, $ToolkitDir)
}
Push-Location $ToolkitDir
try {
    if (Try-Run git @("apply", "--reverse", "--check", "--ignore-whitespace", $Patch)) {
        Write-Host "patch already applied"
    } else {
        Run git @("checkout", "-q", $ToolkitCommit)
        Run git @("apply", "--ignore-whitespace", $Patch)
        Write-Host "patch applied"
    }
} finally { Pop-Location }

# -- libraries --------------------------------------------------------------------
# SDL3 for controllers (zlib license; SDL3.dll goes next to the program). The
# download is checked against its SHA-256 from the GitHub release.
Step "SDL3 $Sdl3Version (controllers)"
$sdlDir = Join-Path $Root "third_party\sdl3"
if (-not (Test-Path (Join-Path $sdlDir "SDL3-$Sdl3Version\cmake\SDL3Config.cmake"))) {
    $zip = Join-Path $Root "third_party\SDL3-devel-$Sdl3Version-VC.zip"
    New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
    $prevProgress = $ProgressPreference; $ProgressPreference = "SilentlyContinue"
    try { Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/libsdl-org/SDL/releases/download/release-$Sdl3Version/SDL3-devel-$Sdl3Version-VC.zip" -OutFile $zip }
    finally { $ProgressPreference = $prevProgress }
    $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    if ($got -ne $Sdl3Sha256) { Remove-Item -Force $zip; throw "SDL3 download has SHA-256 $got, expected $Sdl3Sha256" }
    Expand-Archive -Force $zip $sdlDir
    Remove-Item -Force $zip
} else { Write-Host "already there" }

# Recomp Achievements (its own license, free to use in games: see its LICENSE).
# Does nothing unless RecompLauncher started the game.
Step "Recomp Achievements ($AchCommit)"
$achDir = Join-Path $Root "third_party\RecompAchievements"
if (-not (Test-Path (Join-Path $achDir ".git"))) { Run git @("clone", "-q", $AchRepo, $achDir) }
Push-Location $achDir
try { Run git @("-c", "advice.detachedHead=false", "checkout", "-q", $AchCommit) } finally { Pop-Location }

# -- game files -----------------------------------------------------------------
Step "Game files"
if ($Iso) {
    if (-not (Test-Path $Iso)) { throw "Disc image not found: $Iso" }
    New-Item -ItemType Directory -Force $Game | Out-Null
    Push-Location $ToolkitDir
    try { Run py @("-3", "-m", "tools.xiso", "unpack", $Iso, "-o", $Game) } finally { Pop-Location }
} elseif ($GameDir) {
    if (-not (Test-Path (Join-Path $GameDir "default.xbe"))) { throw "$GameDir has no default.xbe" }
    New-Item -ItemType Directory -Force $Game | Out-Null
    Copy-Item -Recurse -Force (Join-Path $GameDir "*") $Game
}
$xbe = Join-Path $Game "default.xbe"
if (-not (Test-Path $xbe)) { throw "No game: pass -Iso <disc image> or -GameDir <extracted folder>." }
$sha = (Get-FileHash -Algorithm SHA1 $xbe).Hash.ToLower()
if ($sha -ne $KnownXbeSha1) {
    Write-Warning "default.xbe SHA-1 $sha is not the tested release ($KnownXbeSha1). Continuing; other versions may not work."
} else { Write-Host "default.xbe matches the tested release" }

# -- translate the game code ------------------------------------------------------
Step "Translating default.xbe to C (several minutes)"
$tgf = Join-Path $ToolkitDir "game_files"
New-Item -ItemType Directory -Force $tgf | Out-Null
Copy-Item -Force $xbe (Join-Path $tgf "default.xbe")
$gen = Join-Path $Root "src\recomp\gen"
New-Item -ItemType Directory -Force $gen | Out-Null
Push-Location $ToolkitDir
try {
    Run py @("-3", "-m", "tools.xbe_parser", "game_files/default.xbe", "--json", "game_files/default_analysis.json")
    Run py @("-3", "-m", "tools.disasm", "game_files/default.xbe", "--seed-functions", (Join-Path $Root "recomp_config\seed_functions.json"))
    Run py @("-3", "-m", "tools.func_id", "game_files/default.xbe")
    Run py @("-3", "-m", "tools.abi_analysis", "game_files/default.xbe")
    Run py @("-3", "-m", "tools.recomp", "game_files/default.xbe", "--all", "--split", "250",
             "--gen-dir", $gen, "--exclude-manual", (Join-Path $Root "src\recomp_manual.c"))
} finally { Pop-Location }
# sound-chip register writes without the trap (see the script)
Run py @("-3", (Join-Path $Root "recomp_config\tools\fast_mmio.py"), $gen)

# -- menu mods (built from your own ControlRoom.bdl) -------------------------------
Step "Menu additions (Options entries, Biollante cell)"
$bdl = Join-Path $Game "ShellData\ControlRoom.bdl"
$menu = Join-Path $Root "mods\menu\ShellData"
$bio = Join-Path $Root "mods\biollante_slot\ShellData"
New-Item -ItemType Directory -Force $menu, $bio | Out-Null
Run py @("-3", (Join-Path $Root "recomp_config\tools\make_options_items.py"), $bdl, (Join-Path $menu "ControlRoom.bdl"))
Run py @("-3", (Join-Path $Root "recomp_config\tools\make_biollante_slot.py"), (Join-Path $menu "ControlRoom.bdl"), (Join-Path $bio "ControlRoom.bdl"), "-30")

# -- build ----------------------------------------------------------------------------
if (-not $SkipBuild) {
    Step "Building (Release)"
    Run $cmake @("-S", $Root, "-B", (Join-Path $Root "build"), "-DXBOXRECOMP_DIR=$ToolkitDir")
    Run $cmake @("--build", (Join-Path $Root "build"), "--config", "Release", "--parallel")
    Write-Host "`nDone. Run play.bat to play." -ForegroundColor Green
}
