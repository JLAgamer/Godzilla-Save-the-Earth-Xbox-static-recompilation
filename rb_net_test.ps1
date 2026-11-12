# rb_net_test.ps1 - two copies of the game on this PC playing one rollback
# netplay match against each other (gz_rbnet.c), and how they compare.
#
#   .\rb_net_test.ps1                      scripted pads, 40 ms each way, 120 s
#   .\rb_net_test.ps1 -LagMs 80 -Seconds 180 -At "1500 3000"
#   .\rb_net_test.ps1 -Script 0            your controller plays player 1 in
#                                          the host window (player 2 idles)
#
# Each side's log is rb_net_<tag>_p1.log / _p2.log. With -At, both sides write
# their memory and a picture at those ticks (rb_shots\<tag>_p<n>_m<tick>.bin,
# _t<tick>.bmp); the memory is compared at the end.
param(
    [int]$Seconds = 120,
    [int]$LagMs = 40,
    [int]$Delay = 2,
    [int]$Script = 1,
    [string]$At = "1500",
    [string]$Tag = "net",
    [hashtable]$Env = @{}
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$exe = Join-Path $root "build\Release\godzilla_ste_recomp.exe"
$shotDir = Join-Path $root "rb_shots"
New-Item -ItemType Directory -Force $shotDir | Out-Null

function Start-Side([int]$side) {
    $log = Join-Path $root "rb_net_${Tag}_p$side.log"
    $vars = [ordered]@{
        GZ_FULLSCREEN = "0"; GZ_WINDOW_SCALE = "1"; GZ_RENDER_SCALE = "0"; GZ_ANISO = "16"
        GZ_BIOLLANTE_SLOT = "1"; RECOMP_OVERLAY_DIR = (Join-Path $root "mods\biollante_slot")
        RECOMP_USB = "1"; RECOMP_PB_EXEC = "1"; RECOMP_PB_WALK = "dma"; RECOMP_AC97_READY = "1"
        RECOMP_HEAP_RECLAIM = "1"; RECOMP_PB_INVENTORY = "0"; RECOMP_DISPATCH_GATE = "1"
        RECOMP_KERNEL_LOG_BUDGET = "200"; RECOMP_AUDIO_BUF_SAMPLES = "1024"; RECOMP_AUDIO_BUF_COUNT = "4"
        RECOMP_USB_PADS = "2"
        GZ_QUICK_FIGHT = "1"; GZ_MOVIES = "0"; GZ_MOVIE_SKIP_FRAMES = "20"; GZ_QF_DELAY = "60"
        GZ_QF_P1 = "godzilla2k"; GZ_QF_P2 = "gigan"; GZ_QF_CITY = "New_Tokyo_Main_DAY"
        GZ_RB_NET = "1"; GZ_RB_SIDE = "$side"; GZ_RB_DELAY = "$Delay"; GZ_RB_LAG_MS = "$LagMs"
        GZ_RB_PORT = $(if ($side -eq 1) { "7790" } else { "7791" })
        GZ_RB_PEER = "127.0.0.1:7790"
        GZ_RB_SCRIPT = $(if ($Script -or $side -eq 2) { "1" } else { "0" })
        GZ_RB_DUMP_AT = $At; GZ_RB_SHOT_AT = $At
        GZ_SHOT_FILE = (Join-Path $shotDir "${Tag}_p$side")
        # each copy its own saves and hard-disk cache: the game keeps its cache
        # files open, and a second copy sharing them crashes at boot
        GZ_SAVE_DIR = (Join-Path $shotDir "saves_p$side")
    }
    foreach ($k in $Env.Keys) { $vars[$k] = $Env[$k] }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "cmd.exe"
    $psi.Arguments = "/c `"`"$exe`" > `"$log`" 2>&1`""
    $psi.WorkingDirectory = $root
    $psi.UseShellExecute = $false
    foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = [string]$vars[$k] }
    return [System.Diagnostics.Process]::Start($psi)
}

$p1 = Start-Side 1
Start-Sleep -Seconds 2
$p2 = Start-Side 2
$t0 = Get-Date
while ((-not $p1.HasExited -or -not $p2.HasExited) -and ((Get-Date) - $t0).TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 500 }
$alive1 = -not $p1.HasExited; $alive2 = -not $p2.HasExited
Get-Process godzilla_ste_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($p in $p1, $p2) { if (-not $p.HasExited) { $p.Kill() } }

foreach ($side in 1, 2) {
    $log = Join-Path $root "rb_net_${Tag}_p$side.log"
    $alive = if ($side -eq 1) { $alive1 } else { $alive2 }
    "== player $side : " + $(if ($alive) { "still running (stopped)" } else { "EXITED" })
    Select-String -Path $log -Pattern "\[RBN\]|\[CRASH\]|\[RB\] no tick|cannot go back" | Select-Object -Last 8 | ForEach-Object { "   " + $_.Line }
}

# compare the two sides' memory at each tick in -At
$memdiff = Join-Path $root "recomp_config\tools\memdiff.py"
foreach ($t in ($At -split '[ ,]+' | Where-Object { $_ })) {
    $a = Join-Path $shotDir "${Tag}_p1_m$t.bin"; $b = Join-Path $shotDir "${Tag}_p2_m$t.bin"
    if ((Test-Path $a) -and (Test-Path $b)) {
        "== memory at tick $t"
        $env:PY_PYTHON3 = "3.13"
        & py -3 -I $memdiff $a $b | Select-Object -First 25
    } else { "== tick ${t}: no memory from " + $(if (Test-Path $a) { "player 2" } else { "player 1" }) }
}
