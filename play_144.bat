@echo off
rem Play the recomp with a real controller (any XInput pad; a PS5 pad works
rem through Steam Input). Logs go to play_144.log next to this file.
rem
rem Optional: set GZ_PROFILE=1 below to sample performance. Mid-fight, create
rem the file profile.go next to this script (e.g. "type nul > profile.go")
rem and the profile is written to profile.txt about 10 seconds later.
cd /d "%~dp0"

rem Options menu entries for tournament mode and button icons.
set RECOMP_OVERLAY_DIR=%~dp0mods\menu
set RECOMP_USB=1
set RECOMP_PB_EXEC=1
set RECOMP_PB_WALK=dma
set RECOMP_AC97_READY=1
rem Reuse memory the game frees (without it, menus then a big stage such as
rem the Mothership ran the 64 MB out and crashed).
set RECOMP_HEAP_RECLAIM=1
set RECOMP_KERNEL_LOG_BUDGET=200
rem Audio: 4 x 1024-sample buffers (~85 ms) instead of 4 x 512 (~43 ms),
rem which underran thousands of times per session (stutter). AUDIO_HOST logs
rem each dropout with its cause.
set RECOMP_AUDIO_BUF_SAMPLES=1024
set RECOMP_AUDIO_BUF_COUNT=4
set RECOMP_AUDIO_HOST=1
rem 144 Hz test: GZ_RATE sets the game's own frame-rate setting (normally 60),
rem which its frame limiter paces to. If game speed stays normal, logic steps
rem by real time; if everything runs 2.4x fast, it steps per frame. SLEEP/TICK show
rem what the limiter asked for and who reads the game clock.
set GZ_RATE=144
set GZ_SLEEP_LOG=1
set GZ_TICK_LOG=1
set RECOMP_APU_LEVELS=1

rem Hands-free profile: 8 s of samples starting 45 s after launch (mid-fight).
set GZ_PROFILE=%~dp0profile_144.txt
set GZ_PROFILE_AT=45
rem (the optional profile.go trigger below is disabled here)
set GZ_PROFILE_MANUAL=
if "%GZ_PROFILE_MANUAL%"=="1" (
    set GZ_PROFILE=%~dp0profile.txt
    set GZ_GPU_TRACE_FILE=%~dp0profile.go
)

echo Starting Godzilla: Save the Earth (recomp, 144 Hz test). Close the game window to quit.
build\Release\godzilla_ste_recomp.exe > play_144.log 2>&1
echo Exited. Log: %~dp0play_144.log
