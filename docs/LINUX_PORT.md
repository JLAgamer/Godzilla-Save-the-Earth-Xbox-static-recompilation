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
| `gpu_d3d11.c` (3.1k lines) | Direct3D 11, HLSL, Win32 window, hotkeys, screenshots, shader cache | `src/gpu_vk.c`: Vulkan 1.3 on the same `nv2a_backend.h`, same HLSL compiled to SPIR-V, SDL2 window. CPU rasteriser + the toolkit's window when the PC has no Vulkan 1.3 | first version (see "Vulkan renderer") |
| `gz_movie.c` | `CreateProcess` ffmpeg, pipes, `PlaySound` | Movies now play through the game's own XMV player on both systems (`GZ_MOVIES=1`, see `recomp_manual.c`), which needs nothing from the host. `gz_movie.c` is the `GZ_MOVIES=ffmpeg` fallback: `posix_spawn` ffmpeg with its frames on a pipe, the soundtrack WAV on its own SDL2 audio device | done; the ffmpeg fallback not run on Linux |
| `gz_profile.c` | thread suspend + `GetThreadContext` | `SIGPROF`/`perf`, or leave Windows-only | stubbed |
| `gz_ach.c` | none | RecompAchievements SDK already builds on POSIX | as is |
| `play.bat` | cmd | `play.sh`: remembers the copy in `game_path.txt` like Play.bat, then `exec ./godzilla_ste_recomp --game ...` (gz_launch sets the env) | done |
| `package.ps1` | PowerShell zip | `package_linux.py`: `…-Linux-x64.tar.gz` with the binary, `play.sh`, `recomplauncher.json`, textures and licences. SDL2/OpenSSL 3/libepoxy come from the system (SteamOS has them). `recomplauncher.json` lists `godzilla_ste_recomp` after the `.exe` and declares glibc 2.31 | done (v0.3.0-beta.1) |
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
- **Movies.** The game's own movie player plays them (picture and sound),
  so nothing has to be installed on SteamOS.
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
3. **GPU back end**: `src/gpu_vk.c`, Vulkan. *First version done: menus and
   fights draw like D3D11 on Windows (`GZ_GPU=vulkan`).* Still to come is
   listed under "Vulkan renderer" below.
4. **Movies**: `gz_movie.c` on `posix_spawn` + pipe (ffmpeg is the same), and
   audio through an SDL2 audio device in place of `PlaySound`. *Done, and
   then superseded: the game's own XMV player works now (it only lacked the
   vblank clock it paces by), on every system.*
5. **Scripts and packaging**: `play.sh`, `setup.sh`, `package.sh`, a
   `recomplauncher.json` entry for the Linux program name, and Deck
   instructions in the README.
6. **Deck polish**: default to fullscreen at 1280x800, a Steam Input
   template, frame pacing at 30/60 on the Deck's 60 Hz panel (and 90 Hz on
   the OLED), suspend/resume.

## Vulkan renderer

`src/gpu_vk.c` is the GPU renderer for Linux and the Steam Deck. It follows
`gpu_d3d11.c` function for function:

- **Same shaders.** `recomp_config/tools/build_vk_shaders.py` takes `s_hlsl`
  out of `gpu_d3d11.c` and compiles it with Microsoft's DXC (`-spirv`,
  `-HV 2018`, `-fvk-invert-y`, bindings b→0, t→16, s→32) into
  `src/gpu_vk_spirv.h`, which is committed. Run the script again after any
  change to `s_hlsl`. The header records the source's hash
  (`GZ_VK_HLSL_HASH`).
- **Same rules:** render targets per surface address, grown and never
  shrunk, and one depth buffer per depth surface; the texture cache, content
  hashing and replacement textures (`textures_ps/`); the widescreen HUD rules;
  the blend, depth and stencil mapping; per-channel clears; letterboxing.
- **Vulkan 1.3:** dynamic rendering, plus dynamic depth and stencil state.
  A pipeline is just a blend state plus a colour mask, so there are few of
  them, cached in `shader_cache/vk_pipelines.bin`. Depth clamp stands in for
  D3D11's `DepthClipEnable = FALSE`. D24S8 is used where the GPU has it, and
  D32S8 otherwise (AMD, so the Steam Deck).
- **No link dependency:** `libvulkan.so.1` / `vulkan-1.dll` is loaded at run
  time. At startup `main()` checks for a usable GPU
  (`gz_gpu_vk_available()`); without one the game falls back to the CPU
  rasteriser in the toolkit's window.
- **Window:** the renderer opens its own window on the executor thread (SDL2
  on Linux, Win32 on Windows), as the D3D11 one does, and pumps its events in
  `flip()`, keyboard included. On Linux the toolkit's window then stays
  closed (`RECOMP_FB_WINDOW=0`); controllers update themselves without it.
- **Build:** CMake compiles it whenever Vulkan headers are found:
  `third_party/Vulkan-Headers/include`, `-DGZ_VULKAN_INCLUDE=…`, or a
  system/SDK copy (`libvulkan-dev` on Debian/Ubuntu, `vulkan-headers` on
  Arch/SteamOS). Without headers the build still works, with only the CPU
  rasteriser on Linux.
- **Testing on Windows:** `GZ_GPU=vulkan` uses it instead of D3D11.
  `GZ_SHOT_FILE=<path>` (create the file `<path>` and the next flip writes
  `<path>.bmp`) and F12 take screenshots. `GZ_VK_DEVICE=<n>` picks a GPU.

Checked on Windows (RTX 5050, NVIDIA driver 32.0.16.2124), against D3D11:

- the main menu matches;
- a quick fight (Godzilla 2000 vs Gigan, New Tokyo) draws monsters, city,
  water, HUD, atomic breath and explosions alike;
- translucent buildings in some frames are the game fading out a building
  near the camera, not a renderer fault.

Both of D3D11's speed-ups are ported:

- **GPU vertex programs** (`draw_program`): `vs_prog` with a vertex layout
  per packed-input mask (binding 0 per vertex, binding 1 the per-draw
  values), the program, constants and per-draw buffers at bindings 1–3, and
  culling by the rasterizer with dynamic cull mode / front face
  (`GZ_GPU_VSH=0`, `GZ_PROG_CULL=0` as in D3D11).
- **Specialised pixel shaders:** `ps_spec` is `ps_main` built through its
  `SPEC` path with 48 specialisation constants. `build_vk_shaders.py`
  prepends the constants, so the shared HLSL is unchanged. Each combiner
  setup gets a pipeline built on a worker thread, with the generic shader
  drawing until it is ready (`GZ_PS_SPEC=0`: generic only). The driver keeps
  the compiled pipelines in `shader_cache/vk_pipelines.bin`.

Quick fight, RTX 5050, average of the window title's fps over 40 s:

| | render scale 2 | render scale 3 |
|---|---|---|
| D3D11 | 60 | – |
| Vulkan before (CPU vertex programs, generic shader) | 47 | 43 |
| + GPU vertex programs | 57 | – |
| + specialised pixel shaders | 60 | 60 |

60 is the game's frame cap. For comparison, D3D11 with only its generic
shader manages 26, and 42 with vertex programs on the CPU.

Not yet done:

1. Frame readback (`RECOMP_FB_DUMP`), traces, F9/F10, `GZ_SHOT_BURST`, and a
   Linux F12 key.
2. Pre-building the specialised pipelines an earlier run used, as D3D11
   preloads its variants. Until then each run draws a new setup with the
   generic shader for its first few frames.
3. Running it on Linux at all: it compiles and links there, but has only run
   on Windows.

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
