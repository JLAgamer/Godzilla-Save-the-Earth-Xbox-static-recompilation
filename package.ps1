<#
.SYNOPSIS
  Packs a built recomp into a release zip: GodzillaSTE-Recomp-v<version>-Windows-x64.zip

.DESCRIPTION
  The zip holds the program, SDL3.dll (controllers), recomplauncher.json (so
  RecompLauncher can set the game up and start it), the button-icon drawings
  in textures_ps\, README.md, LICENSE and the libraries' licenses. Nothing from the game disc goes in: no game\ folder, no mods\
  (those are built from the player's own ControlRoom.bdl, by setup.ps1 or by
  "godzilla_ste_recomp.exe --prepare --game <disc>").

  Note: godzilla_ste_recomp.exe is the game's default.xbe translated to C and
  compiled. Whether to publish it is the project owner's decision; this script
  only builds the zip locally.

.EXAMPLE
  .\package.ps1 -Version 1.0.0
#>
param(
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$Exe = Join-Path $Root "build\Release\godzilla_ste_recomp.exe"
if (-not (Test-Path $Exe)) { throw "Build first (.\setup.ps1): $Exe not found." }
$Version = $Version.TrimStart("v")

$Out = Join-Path $Root "dist"
$Stage = Join-Path $Out "stage"
$Zip = Join-Path $Out "GodzillaSTE-Recomp-v$Version-Windows-x64.zip"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force $Stage | Out-Null

Copy-Item $Exe $Stage
Copy-Item (Join-Path $Root "recomplauncher.json") $Stage
Copy-Item (Join-Path $Root "README.md") $Stage
Copy-Item (Join-Path $Root "LICENSE") $Stage
Copy-Item -Recurse (Join-Path $Root "textures_ps") (Join-Path $Stage "textures_ps")

# Play.bat for players without a launcher: drag the disc image (or the
# extracted folder) onto it once; it remembers it in game_path.txt.
$playBat = @'
@echo off
rem Godzilla: Save the Earth recomp. The first time, drag your disc image
rem (.iso / .xiso.iso) or extracted game folder onto this file; it is
rem remembered in game_path.txt, so afterwards just double-click.
cd /d "%~dp0"
if not "%~1"=="" (echo %~1> game_path.txt)
if not exist game_path.txt (
    echo Drag your Godzilla: Save the Earth disc image or game folder onto Play.bat.
    pause
    exit /b 1
)
set /p GAME=<game_path.txt
echo Starting... the first start unpacks the disc next to this file (about 800 MB).
start "" godzilla_ste_recomp.exe --game "%GAME%" --log "%~dp0logs\godzilla.log"
'@
Set-Content -Path (Join-Path $Stage "Play.bat") -Value ($playBat -replace "`r?`n", "`r`n") -Encoding ascii -NoNewline

# Controllers: SDL3.dll (without it the program falls back to XInput pads).
$sdl = Join-Path (Split-Path $Exe) "SDL3.dll"
$licenses = Join-Path $Stage "licenses"
New-Item -ItemType Directory -Force $licenses | Out-Null
if (Test-Path $sdl) {
    Copy-Item $sdl $Stage
    $sdlLicense = Get-ChildItem (Join-Path $Root "third_party\sdl3\SDL3-*\LICENSE.txt") | Select-Object -First 1
    if ($sdlLicense) { Copy-Item $sdlLicense.FullName (Join-Path $licenses "SDL3.txt") }
} else { Write-Warning "SDL3.dll not found next to the program: controllers will use XInput only." }
# Recomp Achievements and the libraries it bundles, if the build used it.
$ach = Join-Path $Root "third_party\RecompAchievements"
if (Test-Path (Join-Path $ach "LICENSE")) {
    Copy-Item (Join-Path $ach "LICENSE") (Join-Path $licenses "RecompAchievements.txt")
    foreach ($lib in @("rcheevos", "cjson")) {
        $l = Get-ChildItem (Join-Path $ach "sdk\third_party\$lib") -Filter "LICENSE*" -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($l) { Copy-Item $l.FullName (Join-Path $licenses "$lib.txt") }
    }
}

if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Zip
Remove-Item -Recurse -Force $Stage
Write-Host "Wrote $Zip"
