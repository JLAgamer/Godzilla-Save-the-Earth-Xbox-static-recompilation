# rb_lobby_test.ps1 - two copies of the game on this PC in the Rollback
# Netplay lobby (what Multi-Player > Rollback Netplay > Host / Join starts
# again into), driven by pad steps. After each step both copies take a
# picture. The match the lobby starts plays on; with -Env @{GZ_KO="20"}
# player 2 is knocked out 20 s in, and both should come back to the lobby.
#
#   .\rb_lobby_test.ps1 -Steps (Get-Content rb_lobby_steps.txt)
#
# A step is side:inputs, side 1 = host, 2 = player 2; inputs are
# RECOMP_PAD_LIVE lines (a button or stick and how long, in ms). A step
# "wait:<seconds>" just waits (no picture).
#
# Output in rb_shots\lobby: p1.log/p2.log, p<n>_sNN.bmp per step, pair_sNN.png.
param(
    [string[]]$Steps = @(),
    [int]$Settle = 8,
    [int]$StepWait = 2,
    [int]$After = 5,
    [int]$LagMs = 0,
    [string]$Profile = "",
    [hashtable]$Env = @{},
    [hashtable]$Env2 = @{}
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $Profile) { $Profile = Join-Path $root "rb_shots\saves_menu" }
$work = Join-Path $root "rb_shots\lobby"
New-Item -ItemType Directory -Force $work | Out-Null
Get-ChildItem $work -Include "*.bmp", "*.png", "*.log", "pad*.txt" -Recurse -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
$exe = Join-Path $root "build\Release\godzilla_ste_recomp.exe"

function Start-Side([int]$n) {
    $pad = Join-Path $work "pad$n.txt"; Set-Content $pad "" -NoNewline
    $vars = [ordered]@{
        GZ_FULLSCREEN = "0"; GZ_WINDOW_SCALE = "1"; GZ_MOVIES = "0"; GZ_MOVIE_SKIP_FRAMES = "20"
        RECOMP_USB = "1"; RECOMP_PB_EXEC = "1"; RECOMP_PB_WALK = "dma"; RECOMP_AC97_READY = "1"
        RECOMP_HEAP_RECLAIM = "1"; RECOMP_PB_INVENTORY = "0"; RECOMP_DISPATCH_GATE = "1"
        RECOMP_KERNEL_LOG_BUDGET = "200"; RECOMP_PAD_LIVE = $pad
        GZ_BIOLLANTE_SLOT = "1"; RECOMP_OVERLAY_DIR = (Join-Path $root "mods\biollante_slot")
        GZ_RB_NET = "1"; GZ_RB_LOBBY = "1"; GZ_RB_SIDE = "$n"; GZ_RB_LAG_MS = "$LagMs"
        GZ_RB_PORT = $(if ($n -eq 1) { "7790" } else { "7791" }); GZ_RB_PEER = "127.0.0.1:7790"
        GZ_RB_PROFILE_FROM = $Profile
        GZ_SAVE_DIR = (Join-Path $root "rb_shots\lobby_p$n")
        GZ_RATE = "60"; GZ_TOURNAMENT = "1"; GZ_RB_LOG_ROLLBACKS = "1"
        GZ_SHOT_FILE = (Join-Path $work "shot$n"); GZ_RB_HASHLOG = (Join-Path $work "hash$n.bin")
    }
    foreach ($k in $Env.Keys) { $vars[$k] = [string]$Env[$k] }
    if ($n -eq 2) { foreach ($k in $Env2.Keys) { $vars[$k] = [string]$Env2[$k] } }
    $log = Join-Path $work "p$n.log"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "cmd.exe"; $psi.Arguments = "/c `"`"$exe`" >> `"$log`" 2>&1`""
    $psi.WorkingDirectory = $root; $psi.UseShellExecute = $false
    foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = [string]$vars[$k] }
    [System.Diagnostics.Process]::Start($psi) | Out-Null
}

function Shot([string]$name) {
    foreach ($n in 1, 2) { Set-Content (Join-Path $work "shot$n") "" }
    Start-Sleep -Milliseconds 1500
    foreach ($n in 1, 2) {
        $b = Join-Path $work "shot$n.bmp"
        try { if (Test-Path $b) { Move-Item -Force $b (Join-Path $work "p${n}_$name.bmp") } } catch { }
    }
}

foreach ($n in 1, 2) { New-Item -ItemType Directory -Force (Join-Path $root "rb_shots\lobby_p$n") | Out-Null }
Start-Side 1; Start-Sleep -Seconds 2; Start-Side 2
Start-Sleep -Seconds $Settle
Shot "s00"
$k = 0
foreach ($s in $Steps) {
    $k++
    $side, $in = $s -split ':', 2
    if ($side -eq "wait") { Start-Sleep -Seconds ([int]$in); continue }
    Add-Content (Join-Path $work "pad$side.txt") $in
    Start-Sleep -Seconds $StepWait
    Shot ("s{0:D2}" -f $k)
}
Start-Sleep -Seconds $After
Shot "s99"
Get-Process godzilla_ste_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
foreach ($n in 1, 2) {
    "== player $n"
    Select-String -Path (Join-Path $work "p$n.log") -Pattern "\[LOBBY\]|\[RBN\] lobby|match on|\[MATCH\]|\[KO\]|DESYNC|CRASH|no tick" |
        ForEach-Object { "   " + $_.Line }
}
Add-Type -AssemblyName System.Drawing
foreach ($f in Get-ChildItem $work -Filter "p1_s*.bmp") {
    $o = Join-Path $work ($f.Name -replace '^p1_', 'p2_')
    if (-not (Test-Path $o)) { continue }
    try {
        $a = [System.Drawing.Image]::FromFile($f.FullName); $b = [System.Drawing.Image]::FromFile($o)
        $c = New-Object System.Drawing.Bitmap 1280, 480; $g = [System.Drawing.Graphics]::FromImage($c)
        $g.DrawImage($a, 0, 0, 640, 480); $g.DrawImage($b, 640, 0, 640, 480)
        $c.Save((Join-Path $work (($f.BaseName -replace "^p1_", "pair_") + ".png")), [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $a.Dispose(); $b.Dispose(); $c.Dispose()
    } catch { }
}
