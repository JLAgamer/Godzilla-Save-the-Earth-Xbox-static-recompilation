cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Godzilla: Save the Earth (Xbox) - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(godzilla_ste_recomp C)

# The C runtime linked into the program (every target, the toolkit's too), so
# a release runs without the Visual C++ Redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
# setup.ps1 fetches and patches it into third_party/xboxrecomp.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xboxrecomp/CMakeLists.txt")
    set(_xr_default "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xboxrecomp")
else()
    set(_xr_default "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp")
endif()
set(XBOXRECOMP_DIR "${_xr_default}" CACHE PATH
    "Path to the xboxrecomp toolkit root directory (patched, see patches/)")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
# SDL3 for controllers (setup.ps1 fetches the development package into
# third_party\sdl3). Without it the build uses XInput only.
file(GLOB _sdl3_cfg "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl3/SDL3-*/cmake/SDL3Config.cmake")
if(_sdl3_cfg AND NOT SDL3_DIR)
    list(GET _sdl3_cfg 0 _sdl3_cfg)
    get_filename_component(_sdl3_dir "${_sdl3_cfg}" DIRECTORY)
    set(SDL3_DIR "${_sdl3_dir}" CACHE PATH "SDL3 development package (cmake folder)")
endif()
find_package(SDL3 CONFIG QUIET)   # here too, for copying SDL3.dll below

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# nv2a_vsh_interp.h (as patches/xboxrecomp-v0.17.0.patch leaves it) uses
# size_t without <stddef.h>; on Windows <windows.h> got there first. The real
# fix is that include in the patch; until then, force it on POSIX only.
if(NOT WIN32)
    set_source_files_properties("${XBOXRECOMP_DIR}/src/kernel/nv2a_vsh_interp.c"
        TARGET_DIRECTORY xbox_kernel PROPERTIES COMPILE_OPTIONS "-include;stddef.h")
endif()

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/gz_host.c            # host services (paths, files, args, SHA-1) for Windows and POSIX
    src/gz_net.c             # System Link on Winsock / BSD sockets (GZ_NET=1)
    src/gz_settings.c        # in-game switches (Options menu), gz_settings.ini
    src/gz_launch.c          # --game / --prepare / --log for launchers (recomplauncher.json)
    src/gz_controls.c        # controller remapping and deadzones (gz_controls.ini)
    src/gz_ach.c             # Recomp Achievements events (match results)
)
if(WIN32)
    list(APPEND GAME_SOURCES
        src/gpu_d3d11.c      # D3D11 render back end for the NV2A pushbuffer executor
        src/gz_movie.c       # host XMV playback through ffmpeg
        src/gz_profile.c     # GZ_PROFILE sampling profiler (started mid-fight by the back end)
    )
else()
    # Linux / Steam Deck (docs/LINUX_PORT.md): those three are Windows only
    # for now; the CPU rasteriser draws and movies are skipped.
    list(APPEND GAME_SOURCES src/gz_posix_stubs.c)
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
if(WIN32)
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler          # gpu_d3d11.c compiles its shaders at startup
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    ws2_32
    iphlpapi             # gz_net.c: the local network's broadcast address
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
    shell32              # gz_host.c: CommandLineToArgvW
    bcrypt               # gz_host.c: SHA-1 of the player's default.xbe
)
else()
    # Linux / Steam Deck. The toolkit brings SDL2, epoxy, OpenSSL and
    # pthreads with its libraries; these are what the game-side files use.
    find_package(SDL2 REQUIRED)          # gz_host.c: message boxes
    find_package(OpenSSL REQUIRED)       # gz_host.c: SHA-1 of default.xbe
    target_link_libraries(${PROJECT_NAME} PRIVATE
        SDL2::SDL2 OpenSSL::Crypto ${CMAKE_DL_LIBS} m)
    # Export the generated sub_XXXXXXXX symbols, so the crash handler's dladdr
    # names the guest function a fault happened in (main.c).
    set_target_properties(${PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)
    # The translated code relies on x86 integer behaviour: signed overflow
    # wraps, and guest memory is read through any pointer type. MSVC gives
    # both; GCC and Clang only with these. The generated files are large and
    # machine-written, so their warnings are not useful.
    target_compile_options(${PROJECT_NAME} PRIVATE -fwrapv -fno-strict-aliasing)
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# The program only runs the default.xbe it was translated from: record its
# SHA-1 so --game can tell a player when their copy is a different one.
set(GZ_XBE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/game/GodzillaSTE/default.xbe")
if(EXISTS "${GZ_XBE_FILE}")
    file(SHA1 "${GZ_XBE_FILE}" GZ_XBE_SHA1)
    set_property(SOURCE src/gz_launch.c APPEND PROPERTY COMPILE_DEFINITIONS GZ_XBE_SHA1="${GZ_XBE_SHA1}")
endif()

# Recomp Achievements (setup.ps1 fetches it into third_party). Its own license
# (free to use in games): third_party\RecompAchievements\LICENSE.
set(GZ_ACH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/RecompAchievements/sdk")
if(EXISTS "${GZ_ACH_DIR}/CMakeLists.txt")
    add_subdirectory("${GZ_ACH_DIR}" "${CMAKE_BINARY_DIR}/rl_achievements")
    target_link_libraries(${PROJECT_NAME} PRIVATE rl_achievements)
    set_property(SOURCE src/gz_ach.c APPEND PROPERTY COMPILE_DEFINITIONS GZ_ACHIEVEMENTS)
endif()

# SDL3.dll next to the program (controllers; see src/input in the toolkit).
if(TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${PROJECT_NAME}>)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # Bring-up: a .map file plus a fixed base (no ASLR) so a crash RIP maps
        # straight to a function: RVA = RIP - 0x140000000.
        /MAP
        /DYNAMICBASE:NO
        /FIXED
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
