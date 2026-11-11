@echo off
rem Play the recomp with a real controller (any XInput pad; a PS5 pad works
rem through Steam Input). Logs go to play.log next to this file.
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
rem Online play (the game's System Link over real networking): 1 = on.
rem Then use Multi-Player > System Link in the menus; one player creates a
rem game, the other selects it. Online turns tournament mode on.
rem   GZ_NET_PEER = the other player's IP address (several: comma separated).
rem                 Leave empty on the same home network (LAN broadcast).
rem   UDP ports 1000-1002 must reach each other (port forward or a VPN such
rem   as Tailscale / ZeroTier / Hamachi).
set GZ_NET=0
set GZ_NET_PEER=
rem In game: F12 = screenshot to screenshots\, F9 = trace a frame into the log,
rem F10 = water reflection on/off.
rem Intro and story movies: 1 = play (needs ffmpeg.exe on PATH), 0 = skip.
set GZ_MOVIES=0
rem ----------------------------------------------------------------------------

set RECOMP_USB=1
rem Four controllers plugged into the emulated Xbox (local multiplayer): the Nth
rem pad on this PC is player N, and ones not connected read as idle.
set RECOMP_USB_PADS=4
set RECOMP_PB_EXEC=1
set RECOMP_PB_WALK=dma
set RECOMP_AC97_READY=1
rem Reuse memory the game frees (without it, menus then a big stage such as
rem the Mothership ran the 64 MB out and crashed).
set RECOMP_HEAP_RECLAIM=1
set RECOMP_PB_INVENTORY=0
rem Sound and other device code that the game runs "at raised interrupt level" is not interleaved with
rem other threads, as on the console (without it, music sometimes played at full volume on every channel).
set RECOMP_DISPATCH_GATE=1
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
rem set RECOMP_AUDIO_WAV=%~dp0play_audio.wav
rem Per-voice sound statistics and sound-register race counters:
rem set RECOMP_APU_TRACE=1
rem set RECOMP_APU_RACE_LOG=1

set GZ_PROFILE=
if "%GZ_PROFILE%"=="1" (
    set GZ_PROFILE=%~dp0profile.txt
    set GZ_GPU_TRACE_FILE=%~dp0profile.go
)

echo Starting Godzilla: Save the Earth (recomp). Close the game window to quit.
build\Release\godzilla_ste_recomp.exe > play.log 2>&1
echo Exited. Log: %~dp0play.log
