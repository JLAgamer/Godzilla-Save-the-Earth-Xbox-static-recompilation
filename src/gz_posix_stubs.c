/*
 * gz_posix_stubs.c - stand-ins, on POSIX (Linux / Steam Deck), for the
 * game-side files that are still Windows only. Each keeps the entry points
 * the rest of the program calls and reports the feature as off, the same as
 * the Windows build does when the feature is switched off:
 *
 *   gpu_d3d11.c   (Direct3D 11 renderer)  -> src/gpu_vk.c, the Vulkan back
 *                 end, when the PC has Vulkan 1.3; else the executor's CPU
 *                 rasteriser in the toolkit's SDL2 window (fb_present_sdl.c).
 *   gz_movie.c    (ffmpeg + PlaySound)    -> GZ_MOVIES=0: movies skipped.
 *   gz_profile.c  (thread sampling)       -> GZ_PROFILE ignored.
 *
 * See docs/LINUX_PORT.md.
 */
#ifndef _WIN32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Texture-decode statistics the D3D11 back end keeps and recomp_manual.c's
 * GZ_STATS line prints (LONG, a 32-bit int in the toolkit's POSIX types).
 * Nothing decodes on this host yet, so they stay 0. */
volatile int32_t gz_stat_decode_us, gz_stat_decode_n, gz_stat_decode_px;

/* The Vulkan back end (src/gpu_vk.c) when this PC can run it, else the CPU
 * rasteriser in the toolkit's window. GZ_GPU=0: always the CPU. main.c makes
 * the same choice first, to decide who owns the window. */
int gz_gpu_vk_available(void);
int gz_gpu_vk_register(void);

int gz_gpu_register(void)
{
    const char *e = getenv("GZ_GPU");
    if (!(e && *e == '0') && gz_gpu_vk_available())
        return gz_gpu_vk_register();
    fprintf(stderr, "[GPU] no GPU back end (GZ_GPU=0, or no Vulkan 1.3 here): CPU rasteriser\n");
    return 0;
}

int gz_movie_enabled(void)
{
    static int said;
    if (!said) { said = 1; fprintf(stderr, "[MOVIE] no host movie player on this host yet: movies are skipped\n"); }
    return 0;
}
int  gz_movie_open(uint32_t obj, const char *xbox_path) { (void)obj; (void)xbox_path; return 0; }
int  gz_movie_update(uint32_t obj, uint8_t *dst) { (void)obj; (void)dst; return 0; }
void gz_movie_close(void) {}
void gz_movie_tick(void) {}

void gz_profile_start(void)
{
    if (getenv("GZ_PROFILE")) fprintf(stderr, "[PROFILE] GZ_PROFILE is Windows only for now; ignored\n");
}
#endif
