# rb_test.ps1 - run a scripted quick fight with rollbacks and report how it ended.
#
#   .\rb_test.ps1                  rollback 5 ticks every 30, 60 s
#   .\rb_test.ps1 -Rb 0            the same match without rollbacks (reference)
#   .\rb_test.ps1 -Seconds 90 -Shots "610 1210"
#
# The log goes to rb_test_<tag>.log, pictures to rb_shots\<tag>_t<tick>.bmp.
param(
    [int]$Rb = 5,
    [int]$Seconds = 60,
    [string]$Shots = "",
    [string]$Tag = "",
    [hashtable]$Env = @{}
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $Tag) { $Tag = "rb$Rb" }
$exe = Join-Path $root "build\Release\godzilla_ste_recomp.exe"
$log = Join-Path $root "rb_test_$Tag.log"
$shotDir = Join-Path $root "rb_shots"
New-Item -ItemType Directory -Force $shotDir | Out-Null

$vars = [ordered]@{
    GZ_FULLSCREEN = "0"; GZ_WINDOW_SCALE = "1"; GZ_RENDER_SCALE = "0"; GZ_ANISO = "16"
    GZ_BIOLLANTE_SLOT = "1"; RECOMP_OVERLAY_DIR = (Join-Path $root "mods\biollante_slot")
    RECOMP_USB = "1"; RECOMP_PB_EXEC = "1"; RECOMP_PB_WALK = "dma"; RECOMP_AC97_READY = "1"
    RECOMP_HEAP_RECLAIM = "1"; RECOMP_PB_INVENTORY = "0"; RECOMP_DISPATCH_GATE = "1"
    RECOMP_KERNEL_LOG_BUDGET = "200"; RECOMP_AUDIO_BUF_SAMPLES = "1024"; RECOMP_AUDIO_BUF_COUNT = "4"
    GZ_QUICK_FIGHT = "1"; GZ_MOVIES = "0"; GZ_MOVIE_SKIP_FRAMES = "20"; GZ_QF_DELAY = "60"
    GZ_QF_P1 = "godzilla2k"; GZ_QF_P2 = "gigan"; GZ_QF_CITY = "New_Tokyo_Main_DAY"
    GZ_RB_SCRIPT = "1"; GZ_RB_SCRIPT_RB = "$Rb"; GZ_RB_SHOT_AT = $Shots
    # Two pads plugged in: the game polls only ports with a controller, and the
    # script plays both players through the poll.
    RECOMP_USB_PADS = "2"
    GZ_SHOT_FILE = (Join-Path $shotDir $Tag)
}
foreach ($k in $Env.Keys) { $vars[$k] = $Env[$k] }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "cmd.exe"
$psi.Arguments = "/c `"`"$exe`" > `"$log`" 2>&1`""
$psi.WorkingDirectory = $root
$psi.UseShellExecute = $false
foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = [string]$vars[$k] }
$p = [System.Diagnostics.Process]::Start($psi)
$t0 = Get-Date
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 500 }
$alive = -not $p.HasExited
$game = Get-Process godzilla_ste_recomp -ErrorAction SilentlyContinue
if ($game) { $game | Stop-Process -Force }
if (-not $p.HasExited) { $p.Kill() }
$elapsed = [int]((Get-Date) - $t0).TotalSeconds

$text = Get-Content $log -ErrorAction SilentlyContinue
$rbs = @($text | Select-String "\[RB\] back ").Count
$tick0 = ($text | Select-String "scripted match: tick 0" | Select-Object -First 1)
"== $Tag : " + $(if ($alive) { "still running after $elapsed s (stopped)" } else { "EXITED after $elapsed s" }) + ", $rbs rollback log lines"
if ($tick0) { "   history started" } else { "   history never started" }
$text | Select-String "\[RB\]|exception|crash|fault|ACCESS_VIOLATION|FATAL|heap" | Select-Object -Last 25 | ForEach-Object { "   " + $_.Line }
