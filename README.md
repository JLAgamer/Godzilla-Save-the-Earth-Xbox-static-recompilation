# Godzilla: Save the Earth — Xbox static recompilation

![Overall progress](https://img.shields.io/badge/overall-80%25-green)
![Boot and menus](https://img.shields.io/badge/Boot_and_menus-95%25-brightgreen) ![Versus fights](https://img.shields.io/badge/Versus_fights-90%25-brightgreen) ![Other modes](https://img.shields.io/badge/Other_modes-20%25-red) ![Stages](https://img.shields.io/badge/Stages-70%25-yellow) ![Graphics](https://img.shields.io/badge/Graphics-90%25-brightgreen)
![Audio](https://img.shields.io/badge/Audio-85%25-green) ![Movies](https://img.shields.io/badge/Movies-100%25-brightgreen) ![Widescreen](https://img.shields.io/badge/Widescreen-90%25-brightgreen) ![High frame rate](https://img.shields.io/badge/High_frame_rate-95%25-brightgreen) ![Online (System Link)](https://img.shields.io/badge/Online_%28System_Link%29-60%25-yellow)

A native Windows port of the Xbox version of *Godzilla: Save the Earth* (2004), made by statically
recompiling the game's `default.xbe` into C with the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
toolkit and running it on a Direct3D 11 renderer.

**No game code or data is included.** You need your own copy of the game: a disc image or extracted
files dumped from a disc you own. The setup script translates *your* `default.xbe` and builds from it.

## Features

- Runs natively on Windows with a D3D11 renderer for the Xbox GPU's command stream
- Higher internal resolution (up to 6x), mip-mapped 16x anisotropic filtering, borderless fullscreen
- **Widescreen 16:9**: a wider view at the camera (culling follows), HUD kept in shape and pinned to the corners
- **Frame rate 60 / 120 / 144 / 165 / 240**, with game speed kept to real time when the PC can't hold the rate
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
| Boot and menus | 95% | Every menu works, including the new Options entries and the Biollante cell |
| Versus fights | 90% | Played many times (mostly Tokyo): HUD, beams, destruction, win/lose |
| Other modes | 20% | Action, Melee, Survival and Challenges have not been played through yet |
| Stages | 70% | The ten city and arena stages are checked in fights (picture and frame pacing); the Mothership and the challenge stages are not |
| Graphics | 90% | Combiners, cube maps, water reflections, fog; rare effects may still be wrong |
| Audio | 85% | Music and effects play; menu crackle reported once, not reproduced |
| Movies | 100% | Intro, story and credits play through ffmpeg |
| Widescreen | 90% | Wider camera with matching culling, HUD in the corners; not checked on every stage |
| High frame rate | 95% | 240 fps holds on the ten checked stages; the heaviest moments can dip briefly; the attract demo is not re-checked |
| Online (System Link) | 60% | Works between two copies on one PC; not yet tested PC to PC over the internet |

## Requirements

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

Run `play.bat`. Any XInput controller works (a PlayStation controller through Steam Input, or DS4Windows).

- **Options menu** (last entries): Tournament mode, Button icons, Frame rate, Aspect — saved in `gz_settings.ini`
- **`play.bat`** options at the top: fullscreen, window size, internal resolution, texture filtering,
  online play, movies
- **Keys:** F12 screenshot (`screenshots\`), F9 trace one frame into the log, F10 water reflection on/off
- **Biollante:** the green "?" cell at the end of the top row of the monster select screen
- **`play_fight.bat`** boots straight into a match replaying the last match you started from the menus
- **Online:** set `GZ_NET=1` (and `GZ_NET_PEER=<other player's IP>` over the internet) in `play.bat` on both
  PCs, forward UDP ports 1000–1002 or use a VPN such as Tailscale/ZeroTier, then Multi-Player > System Link.
  Online uses tournament mode so both sides have the same roster.

Logs go to `play.log` next to the launcher; include it when reporting a problem.

## Known issues

- Not every stage and mode has been played through; the recompilation can still hit untranslated paths.
- 240 fps holds on the ten city and arena stages on a fast PC (tested: RTX 3080, Ryzen 7 7800X3D, 2x
  internal resolution). The heaviest moments (many buildings collapsing at once) can dip to about 200 for
  a second or two; game speed stays correct. The first run compiles shaders into `shader_cache\` and is
  less smooth than later ones.
- Online play has been tested between two copies on one PC (including simulated lag and loss), not yet
  between two PCs over the internet.

## How it works

- `src/recomp_manual.c` — hand-written replacements and hooks for specific game functions (by address):
  frame pacing, widescreen camera, Options entries, tournament mode, Biollante, quick-fight boot, the
  network (XNet → Winsock) shims
- `src/gpu_d3d11.c` — the D3D11 back end: register combiners and vertex programs as HLSL, render targets,
  texture cache and replacement, widescreen presentation
- `src/gz_movie.c`, `src/gz_net.c`, `src/gz_settings.c` — movies, sockets, settings
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

## License

MIT for the code in this repository (see `LICENSE`).

*Godzilla: Save the Earth* is © its respective owners (Atari, Pipeworks Software, Toho). This project is not
affiliated with or endorsed by them and contains none of their code or assets.
