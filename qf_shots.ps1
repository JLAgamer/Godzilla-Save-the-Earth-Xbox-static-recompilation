# qf_shots.ps1 - a quick fight with a scripted pad for player 1, taking a
# picture every few seconds: for looking at rendering problems (player
# reports) without playing by hand.
#
#   .\qf_shots.ps1                                Rodan vs Gigan, 16:9, 120 fps
#   .\qf_shots.ps1 -Wide 0 -Rate 60 -Tag ref       the same at 4:3, 60 fps
#   .\qf_shots.ps1 -P1 godzilla2k -City New_Tokyo_Main_DAY -Steps "lright:2000","a:200"
#
# Needs gz_quickfight.bin (a Versus match started once from the menus).
# Pictures go to rb_shots\qf\<tag>_NN.bmp, the log to rb_shots\qf\<tag>.log.
param(
    [string]$P1 = "rodan",
    [string]$P2 = "gigan",
    [string]$City = "New_Tokyo_Main",
    [int]$Wide = 1,
    [int]$Rate = 120,
    [int]$Shots = 20,
    [double]$Every = 2.0,
    [int]$Settle = 30,
    [string[]]$Steps = @("lup:1500", "a:150", "a:150", "b:150", "a:150", "lright:1200", "a:150", "a:150",
                         "x:200", "a:150", "lleft:1500", "a:150", "b:150", "a:150", "ldown:1200", "a:150",
                         "a:150", "y:200", "a:150", "lup:1500", "a:150", "a:150", "b:150", "a:150"),
    [string]$Tag = "qf",
    [hashtable]$Env = @{}
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$exe = Join-Path $root "build\Release\godzilla_ste_recomp.exe"
$dir = Join-Path $root "rb_shots\qf"
New-Item -ItemType Directory -Force $dir | Out-Null
Get-ChildItem $dir -Filter "${Tag}_*.bmp" -ErrorAction SilentlyContinue | Remove-Item -Force
$pad = Join-Path $dir "${Tag}_pad.txt"; Set-Content $pad "" -NoNewline
$shot = Join-Path $dir "${Tag}_shot"
$log = Join-Path $dir "$Tag.log"

$vars = [ordered]@{
    GZ_FULLSCREEN = "0"; GZ_WINDOW_SCALE = "1"; GZ_RENDER_SCALE = "0"; GZ_ANISO = "16"
    GZ_BIOLLANTE_SLOT = "1"; RECOMP_OVERLAY_DIR = (Join-Path $root "mods\biollante_slot")
    RECOMP_USB = "1"; RECOMP_PB_EXEC = "1"; RECOMP_PB_WALK = "dma"; RECOMP_AC97_READY = "1"
    RECOMP_HEAP_RECLAIM = "1"; RECOMP_PB_INVENTORY = "0"; RECOMP_DISPATCH_GATE = "1"
    RECOMP_KERNEL_LOG_BUDGET = "200"
    GZ_QUICK_FIGHT = "1"; GZ_MOVIES = "0"; GZ_MOVIE_SKIP_FRAMES = "20"; GZ_QF_DELAY = "60"
    GZ_QF_P1 = $P1; GZ_QF_P2 = $P2; GZ_QF_CITY = $City
    GZ_WIDE = "$Wide"; GZ_RATE = "$Rate"
    RECOMP_PAD_LIVE = $pad; GZ_SHOT_FILE = $shot
}
foreach ($k in $Env.Keys) { $vars[$k] = [string]$Env[$k] }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "cmd.exe"; $psi.Arguments = "/c `"`"$exe`" > `"$log`" 2>&1`""
$psi.WorkingDirectory = $root; $psi.UseShellExecute = $false
foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = [string]$vars[$k] }
[System.Diagnostics.Process]::Start($psi) | Out-Null
Start-Sleep -Seconds $Settle
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$times = Join-Path $dir "${Tag}_times.txt"; Set-Content $times ""
for ($i = 0; $i -lt $Shots; $i++) {
    if ($i -lt $Steps.Count -and $Steps[$i]) { Add-Content $pad $Steps[$i] }
    Add-Content $times ("{0} {1}" -f $i, $sw.ElapsedMilliseconds)
    Set-Content $shot ""
    Start-Sleep -Milliseconds ([int]($Every * 1000))
    $b = "$shot.bmp"
    try { if (Test-Path $b) { Move-Item -Force $b (Join-Path $dir ("{0}_{1:D2}.bmp" -f $Tag, $i)) } } catch { }
}
Get-Process godzilla_ste_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
Select-String -Path $log -Pattern "QUICKFIGHT|SETTINGS|CRASH|exception" | Select-Object -First 6 | ForEach-Object { $_.Line }
(Get-ChildItem $dir -Filter "${Tag}_*.bmp").Count.ToString() + " pictures in $dir"
