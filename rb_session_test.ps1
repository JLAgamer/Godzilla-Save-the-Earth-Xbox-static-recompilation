# rb_session_test.ps1 - two copies of the game on this PC started as one
# rollback netplay session (what Multi-Player > Rollback Netplay restarts
# into), driven by pad steps. After each step both copies take a picture.
#
#   .\rb_session_test.ps1 -Steps (Get-Content rb_session_fight.txt)
#   .\rb_session_test.ps1 -Steps "1:down:150","1:a:300","2:start:200"
#
# A step is side:inputs, side 1 = host (player 1), 2 = client (player 2);
# inputs are RECOMP_PAD_LIVE lines (a button or stick and how long, in ms).
#
# Needs a profile to share (-Profile, a folder holding the game's UDATA).
# Output goes to rb_shots\sess: p1.log/p2.log, p<n>_sNN.bmp per step,
# pair_sNN.png side by side, hash1.bin/hash2.bin (GZ_RB_HASHLOG; compare
# them with recomp_config\tools\rb_hashdiff.py), and with -DumpAt "t1 t2"
# each side's memory at those ticks (shot<n>_m<t>.bin, for memdiff.py).
param(
    [string[]]$Steps = @(),
    [int]$Settle = 25,
    [int]$StepWait = 3,
    [int]$After = 5,
    [int]$LagMs = 0,
    [string]$DumpAt = "",
    [string]$Profile = "",
    [hashtable]$Env = @{}
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $Profile) { $Profile = Join-Path $root "rb_shots\saves_menu" }
$work = Join-Path $root "rb_shots\sess"
New-Item -ItemType Directory -Force $work | Out-Null
Get-ChildItem $work -Include "*.bmp", "*.png", "*.bin", "*.log" -Recurse -ErrorAction SilentlyContinue |
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
        GZ_RB_NET = "1"; GZ_RB_SESSION = "1"; GZ_RB_SIDE = "$n"; GZ_RB_LAG_MS = "$LagMs"
        GZ_RB_PORT = $(if ($n -eq 1) { "7790" } else { "7791" }); GZ_RB_PEER = "127.0.0.1:7790"
        GZ_RB_PROFILE_FROM = $Profile
        GZ_SAVE_DIR = (Join-Path $root "rb_shots\sess_p$n")
        GZ_RATE = "60"; GZ_TOURNAMENT = "1"; GZ_RB_DUMP_AT = $DumpAt; GZ_RB_LOG_ROLLBACKS = "1"
        GZ_SHOT_FILE = (Join-Path $work "shot$n"); GZ_RB_HASHLOG = (Join-Path $work "hash$n.bin")
    }
    foreach ($k in $Env.Keys) { $vars[$k] = [string]$Env[$k] }
    $log = Join-Path $work "p$n.log"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "cmd.exe"; $psi.Arguments = "/c `"`"$exe`" > `"$log`" 2>&1`""
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

foreach ($n in 1, 2) { New-Item -ItemType Directory -Force (Join-Path $root "rb_shots\sess_p$n") | Out-Null }
Start-Side 1; Start-Sleep -Seconds 2; Start-Side 2
Start-Sleep -Seconds $Settle
Shot "s00"
$k = 0
foreach ($s in $Steps) {
    $k++
    $side, $in = $s -split ':', 2
    Add-Content (Join-Path $work "pad$side.txt") $in
    Start-Sleep -Seconds $StepWait
    Shot ("s{0:D2}" -f $k)
}
Start-Sleep -Seconds $After
Get-Process godzilla_ste_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
foreach ($n in 1, 2) {
    "== player $n"
    Select-String -Path (Join-Path $work "p$n.log") -Pattern "\[RBN\] tick|CRASH|no tick" |
        Select-Object -Last 3 | ForEach-Object { "   " + $_.Line }
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
