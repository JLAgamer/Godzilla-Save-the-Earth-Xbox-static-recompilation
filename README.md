# Godzilla: Save the Earth — Xbox static recompilation

![Overall progress](https://img.shields.io/badge/overall-82%25-green)
![Boot and menus](https://img.shields.io/badge/Boot_and_menus-95%25-brightgreen) ![Versus fights](https://img.shields.io/badge/Versus_fights-95%25-brightgreen) ![Other modes](https://img.shields.io/badge/Other_modes-20%25-red) ![Stages](https://img.shields.io/badge/Stages-80%25-green) ![Graphics](https://img.shields.io/badge/Graphics-90%25-brightgreen)
![Audio](https://img.shields.io/badge/Audio-95%25-brightgreen) ![Movies](https://img.shields.io/badge/Movies-100%25-brightgreen) ![Widescreen](https://img.shields.io/badge/Widescreen-95%25-brightgreen) ![High frame rate](https://img.shields.io/badge/High_frame_rate-95%25-brightgreen) ![Online (System Link)](https://img.shields.io/badge/Online_%28System_Link%29-60%25-yellow)

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

A native Windows port of the Xbox version of *Godzilla: Save the Earth* (2004), made by statically
recompiling the game's `default.xbe` into C with the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
toolkit and running it on a Direct3D 11 renderer.

**You need your own copy of the game**: a disc image or extracted files dumped from a disc you own. This
repository holds no game code or data; building from source translates *your* `default.xbe`. Release
builds contain the game's program translated to C and compiled, and read everything else (art, sound,
stages, movies) from your copy.

## How to play

1. Download `GodzillaSTE-Recomp-<version>-Windows-x64.zip` from
   [Releases](https://github.com/JLAgamer/Godzilla-Save-the-Earth-Xbox-static-recompilation/releases) and unzip it.
2. Drag your copy of *Godzilla: Save the Earth* for Xbox (USA/Europe) onto `Play.bat`: the disc image
   (`.iso`, `.xiso.iso`) or the extracted folder with `default.xbe`. The first start unpacks the disc next to
   the program (about 800 MB) and remembers where it is; after that, double-click `Play.bat`.
3. Or let [RecompLauncher](https://recomplauncher.com) install and set it up.

Windows 10/11 (64-bit) with a Direct3D 11 graphics card. Movies need `ffmpeg.exe` next to the program or on
`PATH`; without it they are skipped.

## Features

- Runs natively on Windows with a D3D11 renderer for the Xbox GPU's command stream
- Higher internal resolution (up to 6x), mip-mapped 16x anisotropic filtering, borderless fullscreen
- **Widescreen 16:9**: a wider view at the camera (culling follows), HUD kept in shape and pinned to the corners
- **Frame rate 60 / 120 / 144**, at the game's own speed. Pick a rate your PC can hold: the game steps by frames, so a PC that falls short of 120 or 144 plays a little slow, as the console did when it dropped frames
- **In-game Options entries**: tournament mode, PlayStation or Xbox button icons, frame rate, aspect ratio
- **Tournament mode**: every monster and city unlocked, whatever the profile holds
- **Biollante**: the cut monster (complete on the disc) gets a cell on the monster select screen
- **FMV movies** (intro, story, credits) decoded with ffmpeg
- **System Link over the internet/LAN** (the game's own Xbox LAN multiplayer, on UDP sockets)
- Texture replacement packs (drop BMPs into a folder), screenshots (F12)

## Progress

Estimates from what has been checked in game so far. Overall is the average of the areas.

| Area | Done | Status |
|---|---|---|
| Boot and menus | 95% | Every menu works, including the new Options entries and the Biollante cell; they stay after returning from a match |
| Versus fights | 95% | Played on all ten city and arena stages: HUD, beams, destruction, win/lose |
| Other modes | 20% | Action, Melee, Survival and Challenges have not been played through yet |
| Stages | 80% | The ten city and arena stages and the Mothership are checked in fights; the challenge stages are not |
| Graphics | 90% | Combiners, cube maps, water reflections, fog; rare effects may still be wrong |
| Audio | 95% | Music, effects and voices sound right in play; fixed: menu crackle, corrupted voice lines and UI sounds (memory and timing bugs in the sound emulation) |
| Movies | 100% | Intro, story and credits play through ffmpeg; skipping them (GZ_MOVIES=0) is silent |
| Widescreen | 95% | Wider view with matching culling in fights and menus, HUD in the corners; the pause menu's dimming covers only the middle |
| High frame rate | 95% | Up to 144 fps (higher rates were too unstable in play); game speed correct at a rate the PC holds (it used to run up to 30% fast at 120/144) |
| Online (System Link) | 60% | Works between two copies on one PC; not yet tested PC to PC over the internet |

## Requirements (building from source)

- Windows 10/11, a GPU with Direct3D 11
- Your own dump of **Godzilla: Save the Earth (Xbox, USA/Europe)** — a `.iso`/`.xiso.iso` disc image or the
  extracted folder containing `default.xbe`. Tested with `default.xbe` SHA-1
  `c2ef3448786662e5797a1d9e545b3859967c01ed`; setup warns if yours differs.
- [Git](https://git-scm.com/), [Python 3.10+](https://www.python.org/) with `py -3 -m pip install capstone`
- Visual Studio 2022 or newer (Community or Build Tools) with **Desktop development with C++**
- Optional: [ffmpeg](https://ffmpeg.org/) on `PATH` for the movies (without it they are skipped)

## Build

From PowerShell in the repository folder:

```powershell
.\setup.ps1 -Iso "D:\dumps\Godzilla - Save the Earth.iso"
# or, for an extracted disc:
.\setup.ps1 -GameDir "D:\dumps\GodzillaSTE"
```

This fetches xboxrecomp v0.17.0 into `third_party\`, applies `patches\xboxrecomp-v0.17.0.patch`,
extracts the game into `game\GodzillaSTE`, translates `default.xbe` to C (several minutes), builds the
menu additions from your `ControlRoom.bdl` into `mods\`, and compiles
`build\Release\godzilla_ste_recomp.exe`. Re-run `.\setup.ps1` with no arguments to rebuild.

If PowerShell refuses to run the script: `powershell -ExecutionPolicy Bypass -File .\setup.ps1 -Iso ...`

## Play

Run `play.bat`. Controllers go through SDL3, so Xbox, PlayStation (DualSense, DualShock 4, over USB or
Bluetooth), Switch Pro and most other pads work directly, without Steam Input or DS4Windows. Without
`SDL3.dll` next to the program (or with `RECOMP_PAD=xinput`) it uses XInput as before. Pads are ignored
while another program's window is in front (`RECOMP_PAD_BACKGROUND=1` reads them anyway).

- **Controller layout:** `gz_controls.ini` (written with comments the first time the game starts) maps every
  Xbox button and stick to a control of your pad by position (`south`, `east`, `left_shoulder`, ...), with
  stick deadzones and a trigger threshold. Delete it to get the defaults back.

- **Options menu** (last entries): Tournament mode, Button icons, Frame rate, Aspect — saved in `gz_settings.ini`,
  which lists every setting with a comment. The first start picks button icons from the controller plugged in.
- **`play.bat`** options at the top: fullscreen, window size, internal resolution, texture filtering,
  online play, movies
- **Keys:** F12 screenshot (`screenshots\`), F9 trace one frame into the log, F10 water reflection on/off
- **Biollante:** the green "?" cell at the end of the top row of the monster select screen
- **`play_fight.bat`** boots straight into a match replaying the last match you started from the menus
- **Online:** set `online=1` (and `online_peer=<other player's IP>` over the internet) in `gz_settings.ini`, or
  `GZ_NET` / `GZ_NET_PEER` in `play.bat`, on both PCs, forward UDP ports 1000–1002 or use a VPN such as Tailscale/ZeroTier, then Multi-Player > System Link.
  Online uses tournament mode so both sides have the same roster.

Logs go to `play.log` next to the launcher; include it when reporting a problem.

**Saves** are in `%LOCALAPPDATA%\GodzillaSTE`. Earlier versions used the toolkit's shared
`%LOCALAPPDATA%\xboxrecomp`; the first start copies this game's saves from there (the old folder is left alone).

### RecompLauncher and command-line options

`recomplauncher.json` lets [RecompLauncher](https://recomplauncher.com) set the game up from your disc
image or folder and start it, with the main settings on the game's page. The same options work from
a shortcut or script (without them the program starts exactly as before, from `game\GodzillaSTE`):

| Option | |
|---|---|
| `--game <path>` | Your copy: an Xbox disc image (`.iso`, `.xiso.iso`) or the extracted folder with `default.xbe`. A disc image is unpacked once into `game\GodzillaSTE` next to the program (about 800 MB). Settings, `mods\`, `shader_cache\` and `screenshots\` are then kept next to the program, and play.bat's defaults apply. |
| `--prepare` | With `--game`: no window. Checks the copy (its `default.xbe` must be the one the program was built from), unpacks it, builds the menu additions in `mods\`, and exits. |
| `--log <file>` | Writes the log there; the two before it are kept as `<file>.1` and `<file>.2`. |
| `--fullscreen`, `--windowed` | Window mode (`GZ_FULLSCREEN`). Started from RecompLauncher's Big Picture mode it is full screen unless you pick otherwise. |
| `--window-scale N`, `--render-scale N`, `--aniso N`, `--movies 0\|1` | The same as `GZ_WINDOW_SCALE`, `GZ_RENDER_SCALE`, `GZ_ANISO`, `GZ_MOVIES` in play.bat. |

Started with `--game` from a folder it can write to (a RecompLauncher install), or with a `portable.txt`
next to the program, it keeps saves in the program's `saves\` folder too. `.\package.ps1 -Version 1.0.0` packs a build into
`dist\GodzillaSTE-Recomp-v1.0.0-Windows-x64.zip` with `recomplauncher.json` next to the program.

## Known issues

- Not every stage and mode has been played through; the recompilation can still hit untranslated paths.
- The frame rate setting goes up to 144. The first run compiles shaders into `shader_cache\` and is
  less smooth than later ones.
- Online play has been tested between two copies on one PC (including simulated lag and loss), not yet
  between two PCs over the internet.

## How it works

- `src/recomp_manual.c` — hand-written replacements and hooks for specific game functions (by address):
  frame pacing, widescreen camera, Options entries, tournament mode, Biollante, quick-fight boot, the
  network (XNet → Winsock) shims
- `src/gpu_d3d11.c` — the D3D11 back end: register combiners and vertex programs as HLSL, render targets,
  texture cache and replacement, widescreen presentation
- `src/gz_movie.c`, `src/gz_net.c`, `src/gz_settings.c`, `src/gz_controls.c` — movies, sockets, settings, controller layout
- `src/gz_launch.c` — `--game` / `--prepare` (disc unpacking and menu additions in C), save folder
- `src/gz_ach.c` — achievement events when a match ends (`match_won`, `match_won_<monster>`, ...)
- `patches/` — changes to xboxrecomp this game needs (overlay folder, vertex-program export, APU mixdown, …)
- `recomp_config/` — function seeds for the lifter, the menu patch tools, and the automated test scripts
  used during development (bash, Git for Windows)
- `textures_ps/` — original drawings of cross/circle/square/triangle prompts and a generic controller,
  named after the textures they replace

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet — the recompilation toolkit and runtime
- [Capstone](https://www.capstone-engine.org/) (disassembly), [FFmpeg](https://ffmpeg.org/) (movies)
- Reverse engineering, code and button artwork were produced with Claude Code (Anthropic's Claude models)
  directed by the project author.
- [SDL3](https://libsdl.org/) (controllers, zlib license); [Recomp Achievements](https://github.com/elripalda/RecompAchievements)
  (achievement events for RecompLauncher, its own license, free for games), with rcheevos and cJSON (MIT)

## License

MIT for the code in this repository (see `LICENSE`).

*Godzilla: Save the Earth* is © its respective owners (Atari, Pipeworks Software, Toho). This project is not
affiliated with or endorsed by them. The repository contains none of their code or assets; release builds
contain the game's program code translated to C, and no game assets.
