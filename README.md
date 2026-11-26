[![Verified developer on RecompStore](https://recompstore.com/assets/press/verified-dark.svg)](https://recompstore.com/dev/jlagamer)
[![Get it on RecompLauncher](https://recomplauncher.com/assets/press/launcher/badge-dark.svg)](https://recomplauncher.com/game/godzilla-save-the-earth-xbox-static-recompilation)
# Godzilla: Save the Earth — Xbox static recompilation

![Overall progress](https://img.shields.io/badge/overall-84%25-green)
![Boot and menus](https://img.shields.io/badge/Boot_and_menus-95%25-brightgreen) ![Versus fights](https://img.shields.io/badge/Versus_fights-95%25-brightgreen) ![Other modes](https://img.shields.io/badge/Other_modes-25%25-red) ![Stages](https://img.shields.io/badge/Stages-80%25-green) ![Graphics](https://img.shields.io/badge/Graphics-93%25-brightgreen)
![Audio](https://img.shields.io/badge/Audio-95%25-brightgreen) ![Movies](https://img.shields.io/badge/Movies-100%25-brightgreen) ![Widescreen](https://img.shields.io/badge/Widescreen-97%25-brightgreen) ![High frame rate](https://img.shields.io/badge/High_frame_rate-95%25-brightgreen) ![Online (System Link)](https://img.shields.io/badge/Online_%28System_Link%29-60%25-yellow)

A native Windows and Linux port of the Xbox version of *Godzilla: Save the Earth* (2004), made by
statically recompiling the game's `default.xbe` into C with the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit and running it on a Direct3D 11 renderer
(Windows) or a Vulkan renderer (Linux and the Steam Deck, and on Windows as an option).

**Linux and the Steam Deck are in beta** (the `linux-port` branch, `v0.3.0-beta` pre-releases): the Linux
build compiles and is published for testing, but nobody has run it on Linux yet. See
[docs/LINUX_PORT.md](docs/LINUX_PORT.md).

**You need your own copy of the game**: a disc image or extracted files dumped from a disc you own. This
repository holds no game code or data; building from source translates *your* `default.xbe`. Release
builds contain the game's program translated to C and compiled, and read everything else (art, sound,
stages, movies) from your copy.

## How to play

**Windows**

1. Download `GodzillaSTE-Recomp-<version>-Windows-x64.zip` from
   [Releases](https://github.com/JLAgamer/Godzilla-Save-the-Earth-Xbox-static-recompilation/releases) and unzip it.
2. Drag your copy of *Godzilla: Save the Earth* for Xbox (USA/Europe) onto `Play.bat`: the disc image
   (`.iso`, `.xiso.iso`) or the extracted folder with `default.xbe`. The first start unpacks the disc next to
   the program (about 800 MB) and remembers where it is; after that, double-click `Play.bat`.
3. Or let [RecompLauncher](https://recomplauncher.com) install and set it up.

Windows 10/11 (64-bit) with a Direct3D 11 graphics card.

**Linux and Steam Deck (beta, untested)**

1. Download `GodzillaSTE-Recomp-<version>-Linux-x64.tar.gz` from a `v0.3.0-beta` pre-release and unpack it.
2. Run `./play.sh "/path/to/your disc image or game folder"` once. It unpacks the disc next to the program
   and remembers where your copy is; after that, `./play.sh` on its own starts the game. RecompLauncher can
   also set it up.

64-bit Linux with glibc 2.31 or newer, SDL2, OpenSSL 3 and libepoxy (SteamOS and most desktop systems have
them), and a GPU and driver with Vulkan 1.3 (the Steam Deck and current Mesa and NVIDIA drivers have it).
Without Vulkan 1.3 the game draws on the processor, which is slow. On the Steam Deck, the Windows build
running through Proton is still the safe way to play; add `play.sh` as a non-Steam game to try the native one.

## Features

- Runs natively on Windows with a D3D11 renderer for the Xbox GPU's command stream, and on Linux (beta) with a
  Vulkan renderer built from the same shaders. A **Renderer** setting picks Direct3D 11, Vulkan or software
- Higher internal resolution (up to 6x), mip-mapped 16x anisotropic filtering, borderless fullscreen
- **Widescreen 16:9**: a wider view at the camera (culling follows), HUD kept in shape and pinned to the corners
- **Frame rate 30 / 60 / 120 / 144**, at the game's own speed. Pick a rate your PC can hold (30 for a PC that slows down in fights): the game steps by frames, so a PC that falls short of 120 or 144 plays a little slow, as the console did when it dropped frames
- **In-game Options entries**: tournament mode, PlayStation or Xbox button icons (with a PlayStation controller picture), frame rate, aspect ratio, game speed
- **Turbo game speed**: the whole game runs 1.2x faster (a player suggestion; System Link stays at normal speed)
- **Tournament mode**: every monster and city unlocked, whatever the profile holds (cheats stay as you set them)
- **Biollante**: the cut monster (complete on the disc) gets a cell on the monster select screen
- **FMV movies** (intro, story, credits) played by the game's own movie player, nothing extra to install
- **System Link over the internet/LAN** (the game's own Xbox LAN multiplayer, on UDP sockets)
- Texture replacement packs (drop BMPs into a folder), screenshots (F12)

## Platforms

| Platform | Status |
|---|---|
| Windows 10/11 | Plays (everything in the Progress table below) |
| Windows, Vulkan renderer | Draws the menus and fights like Direct3D 11, as fast (a choice in the Renderer setting, for testing) |
| Linux | Beta: compiles and links (x86-64), published for testing; never run on Linux yet |
| Steam Deck | The Windows build through Proton is the way to play; the native Linux build is untested there |

## Progress

Estimates from what has been checked in game so far. Overall is the average of the areas.

| Area | Done | Status |
|---|---|---|
| Boot and menus | 95% | Every menu works, including the new Options entries and the Biollante cell; they stay after returning from a match |
| Versus fights | 95% | Played on all ten city and arena stages: HUD, beams, destruction, win/lose |
| Other modes | 25% | Action's first stage plays through to its results screen; the rest of Action, Melee, Survival and Challenges have not been played through yet |
| Stages | 80% | The ten city and arena stages and the Mothership are checked in fights; the challenge stages are not |
| Graphics | 93% | Combiners, cube maps, water reflections, fog, point-sprite particles; rare effects may still be wrong. Fixed: MechaGodzilla 2 and 3's missiles now show in flight with their trails |
| Audio | 95% | Music, effects and voices sound right in play; fixed: menu crackle, corrupted voice lines and UI sounds (memory and timing bugs in the sound emulation) |
| Movies | 100% | Intro, story and credits play through the game's own XMV player (GZ_MOVIES=1; `ffmpeg` plays them through ffmpeg instead); skipping them (GZ_MOVIES=0) is silent |
| Widescreen | 97% | Wider view with matching culling in fights and menus, HUD in the corners. Fixed: the round-end screen dims the whole picture, and the Action results screen keeps its layout. The pause menu's dimming may still cover only the middle (not rechecked) |
| High frame rate | 95% | Up to 144 fps (higher rates were too unstable in play); game speed correct at a rate the PC holds (it used to run up to 30% fast at 120/144) |
| Online (System Link) | 60% | Works between two copies on one PC; not yet tested PC to PC over the internet |

## Requirements (building from source, Windows)

- Windows 10/11, a GPU with Direct3D 11
- Your own dump of **Godzilla: Save the Earth (Xbox, USA/Europe)** — a `.iso`/`.xiso.iso` disc image or the
  extracted folder containing `default.xbe`. Tested with `default.xbe` SHA-1
  `c2ef3448786662e5797a1d9e545b3859967c01ed`; setup warns if yours differs.
- [Git](https://git-scm.com/), [Python 3.10+](https://www.python.org/) with `py -3 -m pip install capstone`
- Visual Studio 2022 or newer (Community or Build Tools) with **Desktop development with C++**

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

### Linux

`setup.ps1` runs on Windows only for now. Run it once on Windows (or copy `third_party/` and
`src/recomp/gen/` from a Windows checkout; the translated C is the same on every system), then:

```bash
bash third_party/xboxrecomp/tools/linux/install_deps.sh   # Debian/Ubuntu; also Arch/SteamOS and Fedora
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target godzilla_ste_recomp -j"$(nproc)"
```

The Vulkan renderer needs only the Vulkan headers to build (`third_party/Vulkan-Headers`, or a system or SDK
copy); its shaders are compiled ahead of time into `src/gpu_vk_spirv.h` by
`recomp_config/tools/build_vk_shaders.py`. The beta builds were cross-compiled on Windows with Zig; how is
in [docs/LINUX_PORT.md](docs/LINUX_PORT.md).

## Play

Run `play.bat`. Controllers go through SDL3, so Xbox, PlayStation (DualSense, DualShock 4, over USB or
Bluetooth), Switch Pro and most other pads work directly, without Steam Input or DS4Windows. Without
`SDL3.dll` next to the program (or with `RECOMP_PAD=xinput`) it uses XInput as before. Pads are ignored
while another program's window is in front (`RECOMP_PAD_BACKGROUND=1` reads them anyway).

- **Controller layout:** `gz_controls.ini` (written with comments the first time the game starts) maps every
  Xbox button and stick to a control of your pad by position (`south`, `east`, `left_shoulder`, ...), with
  stick deadzones and a trigger threshold. Delete it to get the defaults back.

- **Options menu** (last entries): Tournament mode, Button icons, Frame rate, Aspect, Game speed — saved in `gz_settings.ini`,
  which lists every setting with a comment. The first start picks button icons from the controller plugged in.
- **`play.bat`** options at the top: fullscreen, window size, internal resolution, texture filtering,
  online play, movies
- **Keys:** F12 screenshot (`screenshots\`), F9 trace one frame into the log, F10 water reflection on/off
- **Biollante:** the green "?" cell at the end of the top row of the monster select screen
- **`play_fight.bat`** boots straight into a match replaying the last match you started from the menus
- **Online:** set `online=1` (and `online_peer=<other player's IP>` over the internet) in `gz_settings.ini`, or
  `GZ_NET` / `GZ_NET_PEER` in `play.bat`, on both PCs, forward UDP ports 1000–1002 or use a VPN such as Tailscale/ZeroTier, then Multi-Player > System Link.
  Online uses tournament mode so both sides have the same roster.

Logs go to `play.log` next to the launcher (`logs/godzilla.log` with `play.sh` on Linux); include it when
reporting a problem.

**Saves** are in `%LOCALAPPDATA%\GodzillaSTE` (on Linux `~/.local/share/GodzillaSTE`, but `play.sh` and
RecompLauncher keep them in `saves/` next to the program, as described below). Earlier versions used the
toolkit's shared `%LOCALAPPDATA%\xboxrecomp`; the first start copies this game's saves from there (the old folder
is left alone).

**On Linux** `play.sh` takes the place of `play.bat`, with the same defaults; the options below work there too.
Controllers go through SDL2. F12 screenshots are Windows only for now.

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
| `--window-scale N`, `--render-scale N`, `--aniso N` | The same as `GZ_WINDOW_SCALE`, `GZ_RENDER_SCALE`, `GZ_ANISO` in play.bat. |
| `--movies 1\|0\|ffmpeg` | Movies (`GZ_MOVIES`): `1` (the default) plays them through the game's own player, `0` skips them, `ffmpeg` plays them through an `ffmpeg` program next to the game or on PATH instead. |
| `--renderer vulkan\|software` | The renderer. Without it: Direct3D 11 on Windows, Vulkan on Linux. `software` draws on the processor (slow), for when the others show nothing. |

Started with `--game` from a folder it can write to (a RecompLauncher install), or with a `portable.txt`
next to the program, it keeps saves in the program's `saves\` folder too. `.\package.ps1 -Version 1.0.0` packs a build into
`dist\GodzillaSTE-Recomp-v1.0.0-Windows-x64.zip` with `recomplauncher.json` next to the program, and
`python package_linux.py --version 1.0.0 --program <linux binary>` packs `...-Linux-x64.tar.gz` with `play.sh`.

## Known issues

- Not every stage and mode has been played through; the recompilation can still hit untranslated paths.
- The frame rate setting goes from 30 to 144. If fights slow down and the sound breaks up, the PC can't hold
  the rate: pick 30 (Options > Frame rate). The first run compiles shaders into `shader_cache\` and is
  less smooth than later ones.
- Fixed since v0.1.7: a crash at startup, while the main menu loaded, after a file had gone missing
  from the game's disk cache (`%LOCALAPPDATA%\GodzillaSTE\Cache`). The game rebuilds the cache itself.
- Online play has been tested between two copies on one PC (including simulated lag and loss), not yet
  between two PCs over the internet.
- Linux: the build has never been run on Linux, so expect problems there; please send `logs/godzilla.log`.

## How it works

- `src/recomp_manual.c` — hand-written replacements and hooks for specific game functions (by address):
  frame pacing, widescreen camera, Options entries, tournament mode, Biollante, quick-fight boot, the
  network (XNet → sockets) shims, and the vblank clock the game's movie player paces by
- `src/gpu_d3d11.c` — the D3D11 back end: register combiners and vertex programs as HLSL, render targets,
  texture cache and replacement, widescreen presentation
- `src/gpu_vk.c` — the Vulkan back end (Linux, and Windows as an option): the same HLSL compiled to SPIR-V
  (`src/gpu_vk_spirv.h`), GPU vertex programs, pixel-shader variants built in the background
- `src/gz_host.c` — files, paths, folders and message boxes for Windows and Linux; `src/gz_posix_stubs.c` —
  what is still Windows only (the profiler)
- `src/gz_movie.c` (the `--movies ffmpeg` fallback), `src/gz_net.c` (Winsock / BSD sockets), `src/gz_settings.c`,
  `src/gz_controls.c` — movies, sockets, settings, controller layout
- `src/gz_launch.c` — `--game` / `--prepare` (disc unpacking and menu additions in C), save folder
- `src/gz_ach.c` — achievement events when a match ends (`match_won`, `match_won_<monster>`, ...)
- `patches/` — changes to xboxrecomp this game needs (overlay folder, vertex-program export, APU mixdown, …)
- `recomp_config/` — function seeds for the lifter, the menu patch tools, and the automated test scripts
  used during development (bash, Git for Windows)
- `textures_ps/` — original drawings of cross/circle/square/triangle prompts and a generic controller,
  named after the textures they replace

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet — the recompilation toolkit and runtime
- [Capstone](https://www.capstone-engine.org/) (disassembly), [FFmpeg](https://ffmpeg.org/) (optional movie fallback)
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) and
  [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) (Vulkan renderer and its shaders, at
  build time); [SDL2](https://libsdl.org/) on Linux (window, sound, controllers); [Zig](https://ziglang.org/)
  (cross-compiling the Linux betas)
- Reverse engineering, code and button artwork were produced with Claude Code (Anthropic's Claude models)
  directed by the project author.
- [SDL3](https://libsdl.org/) (controllers, zlib license); [Recomp Achievements](https://github.com/elripalda/RecompAchievements)
  (achievement events for RecompLauncher, its own license, free for games), with rcheevos and cJSON (MIT)

## License

MIT for the code in this repository (see `LICENSE`).

*Godzilla: Save the Earth* is © its respective owners (Atari, Pipeworks Software, Toho). This project is not
affiliated with or endorsed by them. The repository contains none of their code or assets; release builds
contain the game's program code translated to C, and no game assets.
