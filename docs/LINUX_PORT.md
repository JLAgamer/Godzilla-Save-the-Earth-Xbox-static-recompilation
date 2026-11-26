# Linux / Steam Deck port

Goal: a native Linux x86-64 build of `godzilla_ste_recomp` that runs on a
Steam Deck (SteamOS 3, Arch-based, Mesa radeonsi/RADV, 1280x800, built-in
controller through Steam Input), next to the Windows build, which stays as it
is.

Until the native build is playable, **the Windows build under Proton is the
way to play on a Deck**. Add `godzilla_ste_recomp.exe` as a non-Steam game,
force a Proton version, and pass `--game <disc or folder>`. D3D11 goes
through DXVK, and XInput and SDL3 pads work under Proton. The toolkit
already allows for Proton: it links `xinput1_4`, and its paths are
case-insensitive.

## Where things stand

The xboxrecomp toolkit (`third_party/xboxrecomp`, v0.17.0 + our patch) is
already built for Linux first:

| Toolkit layer | Linux implementation already in the toolkit |
|---|---|
| Win32 primitives (threads, events, critical sections, `VirtualAlloc`, `MapViewOfFile`, `VirtualQuery`, QPC, `Sleep`, `CreateFile`...) | `src/platform/win32_compat.c` over pthreads/mmap. `xbox_winnt.h` pulls it in on POSIX |
| Guest address space (fixed guest VAs, RAM mirrors, trapped MMIO pages) | `xbox_memory_layout.c` / `guest_vmem.c`: mmap with `MAP_FIXED_NOREPLACE`, guest VA + `g_memory_offset` |
| MMIO traps (APU, AC'97, USB OHCI) | `xbox_PosixMmioFault()` from a SIGSEGV/SIGBUS handler, with an x86-64 instruction decoder |
| Kernel file I/O, saves, `Z:` cache | `kernel_file.c` / `kernel_path.c` over POSIX. Guest paths resolve **case-insensitively** (a `strcasecmp` walk of each component). Saves default to `$XDG_DATA_HOME/xboxrecomp` |
| Crypto | OpenSSL in place of bcrypt |
| Window + framebuffer present | `video/fb_present_sdl.c` (SDL2) |
| D3D8 layer | `d3d/d3d8_gl.c` (OpenGL 3.3 core via SDL2 + libepoxy). Not used by this game: it drives the pushbuffer executor directly |
| Audio out | `apu/apu_sdl2.c` |
| Pads / keyboard | `input/xinput_device.c` with SDL2 GameController on POSIX, `keyboard_sdl.c` |

What was Windows-only is **this repo's own `src/`**:

| File | Windows dependency | Linux plan | Status |
|---|---|---|---|
| `CMakeLists.txt` | D3D11/DXGI/XInput/winmm/ws2_32/iphlpapi/dbghelp/shell32/bcrypt link list, `/bigobj`, `/MAP` | `if(WIN32)` for the Windows list. Linux links SDL2, OpenSSL, `dl`, `m`, with `-rdynamic`, `-fwrapv -fno-strict-aliasing`, and `-w` on the generated files | done |
| `main.c` | `WinMain`, VEH + dbghelp crash report, `timeBeginPeriod`, named mutex and `FindFirstFile` for the disk cache, `_wfopen` | Shared `gz_run()`. POSIX `main`, sigaction handler (MMIO traps → `xbox_PosixMmioFault`, else a crash report with `dladdr`, chained to the previous handler). Cache through `gz_host` | done |
| `gz_launch.c` | UTF-16 paths everywhere, `CommandLineToArgvW`, `GetModuleFileNameW`, BCrypt SHA-1, `MessageBoxW`, `FindFirstFileW`, `CopyFileW`, `%LOCALAPPDATA%` | Native-char paths through `gz_host.h` (`gzch`, `GZT()`, `GZ_SEP`). `/proc/self/exe`, OpenSSL SHA-1, SDL message box, `$XDG_DATA_HOME/GodzillaSTE` | done |
| `gz_host.c/.h` (new) | – | The host shim: Windows half = the calls the code made before, POSIX half = libc | done |
| `gz_net.c` | Winsock, iphlpapi | BSD sockets. The guest's Winsock option numbers, FIONBIO/FIONREAD and error codes are translated. `getifaddrs` for the broadcast address. `select(maxfd+1)` | done, not run |
| `recomp_manual.c` | `<windows.h>`, `<xinput.h>` | Toolkit's `xbox_winnt.h` (Sleep, QPC, `CreateThread`, `VirtualQuery`, `XInputGetState` over SDL2) | done |
| `gz_settings.c`, `gz_controls.c` | `_putenv_s`, `_stricmp` | Mapped in `gz_host.h` | done |
| `gpu_d3d11.c` (3.1k lines) | Direct3D 11, HLSL, Win32 window, hotkeys, screenshots, shader cache | **New back end on `nv2a_backend.h`**: OpenGL 3.3/4.x (or Vulkan) with SDL2. Until then: stub → CPU rasteriser + SDL2 window | stubbed (`gz_posix_stubs.c`) |
| `gz_movie.c` | `CreateProcess` ffmpeg, pipes, `PlaySound` | `posix_spawn`/`popen` ffmpeg, audio through SDL2 | stubbed (movies off; also the default) |
| `gz_profile.c` | thread suspend + `GetThreadContext` | `SIGPROF`/`perf`, or leave Windows-only | stubbed |
| `gz_ach.c` | none | RecompAchievements SDK already builds on POSIX | as is |
| `play.bat` | cmd | `play.sh`: the same env, `exec ./godzilla_ste_recomp --game ...` | todo |
| `package.ps1` | PowerShell zip | `package.sh`: tar.gz with the binary, `play.sh`, licences, and a note on SDL2/OpenSSL (SteamOS has both) | todo |
| `setup.ps1` | PowerShell, `py` | `setup.sh` (git, `python3`, the same steps). Translating the XBE is OS-independent: the generated C can be copied over from Windows | todo |

## Things that differ on Linux

- **Case-sensitive file system.** Guest paths are safe: the kernel layer
  resolves each component case-insensitively. Host-side paths the game code
  builds itself must match case exactly: `mods/menu/ShellData/ControlRoom.bdl`,
  `game/GodzillaSTE/default.xbe`, `gz_settings.ini`, `textures_ps/`,
  `shader_cache/`. All of them are written by this program, so they match.
  The one risk is a user-extracted folder whose `ShellData` or `default.xbe`
  is in a different case. `gz_launch.c` opens those directly and would then
  report "not this game". If that turns up, give `gz_host` a
  case-insensitive open.
- **Separators.** Host paths use `GZ_SEP` / `GZ_SEP_A`. `RECOMP_OVERLAY_DIR`
  and `GZ_SAVE_DIR` are host paths, so on Linux they use `/`.
- **`WCHAR` is not `wchar_t` on POSIX** (the toolkit makes it UTF-16
  `uint16_t`, as on the Xbox). Game-side host paths are `gzch` = `char` there,
  never `WCHAR`.
- **Low guest addresses.** The guest image sits at VA 0x00010000 +
  `g_memory_offset`. The toolkit picks the offset, so `vm.mmap_min_addr`
  doesn't matter.
- **Compiler semantics.** The generated C assumes wrapping signed arithmetic
  and type-punned memory access (`-fwrapv -fno-strict-aliasing`). Thread-local
  guest registers use `__thread` (the toolkit's `RECOMP_TLS`).
- **Frame pacing / timers.** `timeBeginPeriod` has no equivalent and none is
  needed: `nanosleep` is precise. Check that the 30/60 fps limiter and the
  1.2x turbo in `recomp_manual.c` still pace correctly. They use the toolkit's
  QPC (`clock_gettime`) and `Sleep`.

## Steam Deck specifics

- **Display 1280x800 (16:10).** The game renders 640x480 (4:3) or our 16:9
  widescreen. The Linux back end should default to fullscreen at the panel's
  size with aspect-correct letter/pillarboxing. Render scale 2 (1280x960)
  fits as 1067x800 at 4:3.
- **Controller.** Steam Input exposes the Deck as an Xbox-style pad over
  SDL2. The toolkit's SDL2 GameController path covers it, and
  `gz_controls.ini` remapping applies on top. Glyphs: the Options > button
  icons setting already switches PS/Xbox prompts.
- **Launch.** Add it as a non-Steam game: `play.sh` as the target, with
  `--game` pointing at the disc image or extracted folder on the SD card. In
  Game Mode it starts fullscreen (`--fullscreen` or
  `RECOMPLAUNCHER_MODE=bigpicture`).
- **Performance.** The CPU rasteriser will be too slow on the Deck's Zen 2
  cores at render scale 2, so the GPU back end is the step that makes it
  playable. The 30 fps option already exists for slow machines.
- **Libraries.** SteamOS ships SDL2, OpenSSL 3 and Mesa. Build in the Steam
  Linux Runtime "sniper" SDK container (Debian 11 based) for a binary that
  runs on SteamOS and desktop distros alike.

## Steps

1. **Compile and link on Linux** (this pass): CMake splits by platform,
   `gz_host` shim, POSIX `main`, BSD-socket System Link, stubs for the
   renderer, movies and profiler. *Done: compiles and links (cross-compiled,
   not run yet). See "Build status" below.*
2. **Boot on Linux with the CPU rasteriser**: reach the title screen through
   `fb_present_sdl.c`, with sound through `apu_sdl2.c` and a pad through SDL2.
   Fix whatever the toolkit's patch or our `recomp_manual.c` assume about
   Windows at run time.
3. **GPU back end** `src/gpu_gl.c` on `nv2a_backend.h` (clear / draw / flip /
   draw_program). Port `gpu_d3d11.c`: its HLSL becomes GLSL 330 (or SPIR-V
   for Vulkan), the D3D11 state objects become GL state, and the SDL2 window
   (fullscreen, scale, widescreen) replaces the Win32 window. Keep the hotkey,
   screenshot and shader-cache features behind the same `GZ_*` switches.
4. **Movies**: `gz_movie.c` on `posix_spawn` + pipe (ffmpeg is the same), and
   audio through an SDL2 audio stream in place of `PlaySound`.
5. **Scripts and packaging**: `play.sh`, `setup.sh`, `package.sh`, a
   `recomplauncher.json` entry for the Linux program name, and Deck
   instructions in the README.
6. **Deck polish**: default to fullscreen at 1280x800, a Steam Input
   template, frame pacing at 30/60 on the Deck's 60 Hz panel (and 90 Hz on
   the OLED), suspend/resume.

## Build (Linux)

```bash
# Debian/Ubuntu/WSL (toolkit's script also handles Arch/SteamOS and Fedora)
bash third_party/xboxrecomp/tools/linux/install_deps.sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target godzilla_ste_recomp -j"$(nproc)"
```

`third_party/` (patched toolkit, RecompAchievements) and `src/recomp/gen/`
come from `setup.ps1`. Both can be copied from a Windows checkout as they
are, because the translated C is the same on every host.

## Build status

**2026-11-25, first pass: the Linux build compiles and links.** Not run yet:
no Linux machine was available.

How: cross-compiled on Windows with Zig 0.13.0
(`zig cc -target x86_64-linux-gnu.2.31`, Clang 18), through a CMake
toolchain file and the Ninja that comes with VS Build Tools. SDL2 2.26.5,
OpenSSL 3.0.22 and libepoxy 1.5.10 came from the Debian 12 `.deb` packages,
unpacked as a sysroot. All of that stays outside the repo. A real Linux box
or the Steam Runtime SDK just uses the normal `cmake` steps above.

- All 119 objects of the game target compile: the 60 generated
  `recomp_*.c` files, the dispatch table, every game-side file, and the
  whole toolkit including its SDL2 window, audio, pads and GL D3D8 layer.
- Links to an x86-64 ELF (`godzilla_ste_recomp`). It needs only
  `libSDL2-2.0.so.0`, `libcrypto.so.3`, `libepoxy.so.0` and glibc ≥ 2.31,
  all of which SteamOS ships. 13,975 `sub_*` guest functions are exported
  (`ENABLE_EXPORTS`), so crash reports name the guest function.
- Fixed on the way:
  - `recomp_dispatch_init` was used without a prototype in `main.c` (MSVC
    accepts that).
  - `PAGE_GUARD` and `YieldProcessor` are missing from the toolkit's POSIX
    layer (defined in `recomp_manual.c`).
  - `FNM_CASEFOLD` needs `_GNU_SOURCE` (`gz_host.c`).
  - `gz_stat_decode_*`, counters `gpu_d3d11.c` defines and `recomp_manual.c`
    prints, are now defined in `gz_posix_stubs.c`.
- Toolkit issue, worked around in our CMake: `nv2a_vsh_interp.h`, as our
  patch leaves it, uses `size_t` without `<stddef.h>`. On POSIX the build
  force-includes `stddef.h` for that file. The real fix is one line in
  `patches/xboxrecomp-v0.17.0.patch`. Note that the local
  `third_party/xboxrecomp` has changes (SDL3 input and more) that the
  committed patch doesn't contain. Reconcile the two before editing the
  patch.
- Warnings: only in the toolkit, all cosmetic (`%lu` given a 32-bit `DWORD`,
  duplicate `NV097_*` macros in `nv2a_pgraph_d3d11.c`, `//` within comments).
  None come from the game-side files.

Next (step 2): run it on Linux (a Steam Deck, a Linux PC, or WSL with WSLg).
Copy the binary with `game/`, `mods/` and `gz_settings.ini` next to it, start
it with `--game game/GodzillaSTE`, and work through what fails at run time.
Expect the CPU rasteriser to be slow.
