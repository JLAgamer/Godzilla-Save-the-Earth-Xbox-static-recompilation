@echo off
rem Play the recomp with a real controller (any XInput pad; a PS5 pad works
rem through Steam Input). Logs go to play_fight.log next to this file.
rem
rem Optional: set GZ_PROFILE=1 below to sample performance. Mid-fight, create
rem the file profile.go next to this script (e.g. "type nul > profile.go")
rem and the profile is written to profile.txt about 10 seconds later.
cd /d "%~dp0"

rem ---------------------------------------------------------------- options
rem Fullscreen (borderless, 4:3 letterboxed): 1 = on.
set GZ_FULLSCREEN=0
rem Window size when not fullscreen, in multiples of 640x480 (1..4).
set GZ_WINDOW_SCALE=2
rem Internal resolution in multiples of 640x480 (1..6). 0 = match the window.
set GZ_RENDER_SCALE=0
rem Texture filtering: 16 = mip-mapped 16x anisotropic (sharp), 1 = original.
set GZ_ANISO=16
rem Tournament mode and PlayStation / Xbox button icons are switched in game:
rem Options menu, the last two entries (saved in gz_settings.ini).
rem Biollante (cut from the roster, complete on the disc): 1 = add her cell to
rem the monster select screen (the green "?" at the end of the top row).
rem The patched menu layout lives in mods\biollante_slot; rebuild it with
rem   py recomp_config\tools\make_biollante_slot.py game\GodzillaSTE\ShellData\ControlRoom.bdl mods\biollante_slot\ShellData\ControlRoom.bdl -30
set GZ_BIOLLANTE_SLOT=1
set RECOMP_OVERLAY_DIR=%~dp0mods\menu
if "%GZ_BIOLLANTE_SLOT%"=="1" set RECOMP_OVERLAY_DIR=%~dp0mods\biollante_slot
rem Other ways to pick her: hold both triggers while starting a match, or
rem GZ_BIOLLANTE=2 makes player 2 (e.g. the computer) Biollante every match.
set GZ_BIOLLANTE=
rem In game: F12 = screenshot to screenshots\, F9 = trace a frame into the log,
rem F10 = water reflection on/off.
rem ----------------------------------------------------------------------------

set RECOMP_USB=1
set RECOMP_PB_EXEC=1
set RECOMP_PB_WALK=dma
set RECOMP_AC97_READY=1
rem Reuse memory the game frees (without it, menus then a big stage such as
rem the Mothership ran the 64 MB out and crashed).
set RECOMP_HEAP_RECLAIM=1
set RECOMP_PB_INVENTORY=0
set RECOMP_KERNEL_LOG_BUDGET=200
rem Audio: 4 x 1024-sample buffers (~85 ms) instead of 4 x 512 (~43 ms),
rem which underran thousands of times per session (stutter). AUDIO_HOST logs
rem each dropout with its cause.
set RECOMP_AUDIO_BUF_SAMPLES=1024
set RECOMP_AUDIO_BUF_COUNT=4
set RECOMP_AUDIO_HOST=1
rem set GZ_SLEEP_LOG=1
rem set GZ_TICK_LOG=1
rem Diagnostics, off for normal play (remove "rem" to use): frame limiter and
rem clock logs, per-second channel levels, and the mixed output as a WAV
rem (the WAV grows about 11 MB per minute).
rem set RECOMP_APU_LEVELS=1
rem set RECOMP_AUDIO_WAV=%~dp0play_fight_audio.wav

set GZ_PROFILE=
if "%GZ_PROFILE%"=="1" (
    set GZ_PROFILE=%~dp0profile.txt
    set GZ_GPU_TRACE_FILE=%~dp0profile.go
)

rem Quick fight: skip the menus. Once the intro logos are over the recomp fills
rem in the match the menus would have set up and starts it (GZ_QUICK_FIGHT).
rem Change the monsters or the city here; names are the game's own file names,
rem e.g. godzilla2k, gigan (P2 is the computer).
set GZ_QUICK_FIGHT=1
set GZ_MOVIES=0
set GZ_MOVIE_SKIP_FRAMES=20
set GZ_QF_DELAY=60
set GZ_QF_P1=godzilla2k
set GZ_QF_P2=gigan
set GZ_QF_CITY=New_Tokyo_Main_DAY

echo Starting Godzilla: Save the Earth (recomp) - quick fight, no menus.
"%~dp0build\Release\godzilla_ste_recomp.exe" > "%~dp0play_fight.log" 2>&1
echo Game exited. Log: %~dp0play_fight.log
pause
