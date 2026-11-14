# rb_menu_test.ps1 - the Rollback Netplay entry end to end. Two copies of the
# game each go to Multi-Player > Rollback Netplay with scripted pad presses
# (RECOMP_PAD_LIVE); the prompt is answered by posting to its buttons: copy 1
# hosts, copy 2 joins 127.0.0.1. Both should restart as one netplay session.
# Then -After steps (side:inputs, as in rb_session_test.ps1) are played.
param([int]$Seconds = 60, [string[]]$After = @(), [int]$StepWait = 3)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$work = Join-Path $root "rb_shots\mnet"
Get-ChildItem $work -Include "*.bmp", "*.log" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
$exe = Join-Path $root "build\Release\godzilla_ste_recomp.exe"

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class Win {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(64);
      GetClassName(h, sb, 64); if (p == pid && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
"@

function Start-Copy([int]$n) {
    $pad = Join-Path $work "pad$n.txt"; Set-Content $pad "" -NoNewline
    $vars = [ordered]@{
        GZ_FULLSCREEN = "0"; GZ_WINDOW_SCALE = "1"; GZ_MOVIES = "0"; GZ_MOVIE_SKIP_FRAMES = "20"
        RECOMP_USB = "1"; RECOMP_PB_EXEC = "1"; RECOMP_PB_WALK = "dma"; RECOMP_AC97_READY = "1"
        RECOMP_HEAP_RECLAIM = "1"; RECOMP_PB_INVENTORY = "0"; RECOMP_DISPATCH_GATE = "1"
        RECOMP_KERNEL_LOG_BUDGET = "200"; RECOMP_PAD_LIVE = $pad; GZ_OPT_LOG = "1"
        GZ_BIOLLANTE_SLOT = "1"; RECOMP_OVERLAY_DIR = (Join-Path $root "mods\biollante_slot")
        GZ_SAVE_DIR = (Join-Path $root "rb_shots\saves_menu$n")
        GZ_RB_PORT = $(if ($n -eq 1) { "7790" } else { "7791" })
        GZ_SHOT_FILE = (Join-Path $work "shot$n")
    }
    $log = Join-Path $work "copy$n.log"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "cmd.exe"; $psi.Arguments = "/c `"`"$exe`" > `"$log`" 2>&1`""
    $psi.WorkingDirectory = $root; $psi.UseShellExecute = $false
    foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = [string]$vars[$k] }
    [System.Diagnostics.Process]::Start($psi) | Out-Null
}

foreach ($n in 1, 2) {
    $sd = Join-Path $root "rb_shots\saves_menu$n"
    New-Item -ItemType Directory -Force $sd | Out-Null
    Copy-Item -Recurse -Force (Join-Path $root "rb_shots\saves_menu\UDATA") $sd
    Start-Copy $n
    Start-Sleep -Seconds 3
}
# wait for both to reach the main menu
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 120) {
    $ok = 0
    foreach ($n in 1, 2) { if ((Select-String -Path (Join-Path $work "copy$n.log") -Pattern "MOVIE" -ErrorAction SilentlyContinue).Count -ge 6) { $ok++ } }
    if ($ok -eq 2) { break }
    Start-Sleep -Seconds 1
}
Start-Sleep -Seconds 10
$steps = @("down:150", "down:150", "down:150", "down:150", "down:150", "a:300", "a:300", "a:300", "a:300")
$k = 0
foreach ($s in $steps) {
    foreach ($n in 1, 2) { Add-Content (Join-Path $work "pad$n.txt") $s }
    Start-Sleep -Seconds 3
    $k++
    foreach ($n in 1, 2) { Set-Content (Join-Path $work "shot$n") "" }       # a picture of each step
    Start-Sleep -Seconds 1
    foreach ($n in 1, 2) { $b = Join-Path $work "shot$n.bmp"; if (Test-Path $b) { Move-Item -Force $b (Join-Path $work "c${n}_step$k.bmp") } }
}

# answer the prompts. The copies start again reading their pad files from
# the beginning, so those are emptied first.
foreach ($n in 1, 2) { Set-Content (Join-Path $work "pad$n.txt") "" -NoNewline }
$games = Get-Process godzilla_ste_recomp | Sort-Object StartTime
"copies running: $($games.Count)"
foreach ($i in 0, 1) {
    $p = $games[$i]
    $w = [IntPtr]::Zero
    for ($k = 0; $k -lt 60 -and $w -eq [IntPtr]::Zero; $k++) { $w = [Win]::Find([uint32]$p.Id, "GzRollbackPrompt"); if ($w -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 } }
    if ($w -eq [IntPtr]::Zero) { "copy $($i+1): no prompt"; continue }
    if ($i -eq 1) {
        # join the copy that hosted: its port is the one in its own settings
        $hp = if ((Select-String -Path (Join-Path $work "copy1.log") -Pattern "hosting" -ErrorAction SilentlyContinue)) { 7790 } else { 7791 }
        [Win]::SendMessage([Win]::GetDlgItem($w, 101), 0x000C, [IntPtr]::Zero, "127.0.0.1:$hp") | Out-Null   # WM_SETTEXT
        [Win]::PostMessage($w, 0x0111, [IntPtr]103, [Win]::GetDlgItem($w, 103)) | Out-Null    # BM_CLICK Join
        "copy 2: join clicked"
    } else {
        [Win]::PostMessage($w, 0x0111, [IntPtr]102, [Win]::GetDlgItem($w, 102)) | Out-Null    # BM_CLICK Host
        "copy 1: host clicked"
    }
    Start-Sleep -Seconds 2
}
Start-Sleep -Seconds 6
foreach ($p in Get-Process godzilla_ste_recomp -ErrorAction SilentlyContinue) {
    "pid $($p.Id) started $($p.StartTime.ToString('HH:mm:ss')): prompt " + ([Win]::Find([uint32]$p.Id, "GzRollbackPrompt") -ne [IntPtr]::Zero) + ", message box " + ([Win]::Find([uint32]$p.Id, "#32770") -ne [IntPtr]::Zero)
}
# the restarted copies: wait for the session's tick 0, then play the steps
$t1 = Get-Date
while (((Get-Date) - $t1).TotalSeconds -lt 90) {
    $ok = 0
    foreach ($n in 1, 2) { if (Select-String -Path (Join-Path $work "copy$n.log") -Pattern "match on: tick 0" -ErrorAction SilentlyContinue) { $ok++ } }
    if ($ok -eq 2) { break }
    Start-Sleep -Seconds 1
}
"session started on both: $($ok -eq 2) after $([int]((Get-Date) - $t1).TotalSeconds) s"
Start-Sleep -Seconds 25
$k = 0
foreach ($s in $After) {
    $k++
    $side, $in = $s -split ':', 2
    Add-Content (Join-Path $work "pad$side.txt") $in
    Start-Sleep -Seconds $StepWait
    foreach ($n in 1, 2) { Set-Content (Join-Path $work "shot$n") "" }
    Start-Sleep -Milliseconds 1500
    foreach ($n in 1, 2) { $b = Join-Path $work "shot$n.bmp"; try { if (Test-Path $b) { Move-Item -Force $b (Join-Path $work ("c${n}_s{0:D2}.bmp" -f $k)) } } catch { } }
}
$t1 = Get-Date
while (((Get-Date) - $t1).TotalSeconds -lt $Seconds) { Start-Sleep -Seconds 3 }
Get-Process godzilla_ste_recomp -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($n in 1, 2) {
    "== copy $n"
    Select-String -Path (Join-Path $work "copy$n.log") -Pattern "\[RBN\]|CRASH|QUICKFIGHT\]" | Select-Object -Last 10 | ForEach-Object { "   " + $_.Line }
}
