/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>   /* getenv */
#include <windows.h>  /* Sleep, GetTickCount (fence wait) */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* The register model is per-thread in recomp_types.h
 * (extern RECOMP_TLS uint32_t g_eax...). The template declared it as a plain
 * global, so writing g_eax from here hit a bogus address. Match the header. */
#if defined(_MSC_VER)
#  define GZ_TLS __declspec(thread)
#else
#  define GZ_TLS _Thread_local
#endif
extern GZ_TLS uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

#define GZ_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define GZ_MEM8(va)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
/* GameInfo's player blocks (0xD8 bytes from GameInfo + 0x160): this byte is 1
 * for a System Link player, one playing on another console. */
#define GZ_PLAYER_REMOTE 0xA4u

/* ── D3D GPU semaphore (Godzilla: Save the Earth, XDK 5849) ──
 *
 * XDK D3D's ring-space check (sub_00223560) spins until PGRAPH 0x400B10's
 * fence bits match the GPU semaphore word, and the executor only writes
 * that word once it knows where it is. The semaphore pointer lives at
 * device + 0x30; the device pointer is the global at 0x0022E278.
 * See xboxrecomp docs/technical/d3d8ltcg-device-context.md. */
#define GZ_D3D_DEVICE_PTR 0x0022E278u

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_00223560_gen(void);
extern void sub_00223730_gen(void);

/* Register the semaphore before the first semaphore release (0x1D70) is
 * executed: releases the executor runs before it knows the address are lost. */
static void gz_register_semaphore(void)
{
    static uint32_t s_sem;

    if (!s_sem) {
        uint32_t dev = GZ_MEM32(GZ_D3D_DEVICE_PTR);
        uint32_t sem = dev ? GZ_MEM32(dev + 0x30) : 0;

        if (sem) {
            s_sem = sem;
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (device 0x%08X)\n", sem, dev);
        }
    }
}

/* D3D KickOff: queues the 0x0310 fence marker and the 0x1D70 release,
 * then moves DMA_PUT. */
void sub_00223730(void)
{
    gz_register_semaphore();
    sub_00223730_gen();
}

/* ── XNet: XNetGetTitleXnAddr (thiscall, ret 4) ──
 *
 * Networking is stubbed (no NIC interrupt, no DHCP), so the XNet context never
 * finishes initializing and this keeps returning XNET_GET_XNADDR_PENDING (0).
 * The title polls it forever. Report XNET_GET_XNADDR_NONE (1): what a
 * console with no cable says, so the game continues offline.
 * Temporary until real networking lands (MODLOG roadmap item 4). */
#define XNET_GET_XNADDR_PENDING 0u
#define XNET_GET_XNADDR_NONE    1u
extern void sub_0027EB84_gen(void);

void sub_0027EB84(void)
{
    static int s_logged;

    sub_0027EB84_gen();
    if (g_eax == XNET_GET_XNADDR_PENDING) {
        g_eax = XNET_GET_XNADDR_NONE;
        if (!s_logged) {
            s_logged = 1;
            fprintf(stderr, "[XNET] XnAddr pending -> reporting no network (offline)\n");
        }
    }
}

/* ── D3D fence wait: sub_002237F0(fence, flags), stdcall, ret 8 ──
 *
 * Under RECOMP_PB_WALK=dma, D3D's "GPU far behind" path patches a wake-me
 * software method into the pushbuffer and sleeps on the event at
 * device + 0x1DCC, which the GPU interrupt would signal. The runtime doesn't
 * deliver that interrupt: the wait fails at once with STATUS_UNSUCCESSFUL and
 * BlockOnFence (sub_0021ED70) retries forever (boot13 froze at flip 211).
 *
 * Instead, as in xboxrecomp docs/technical/d3d8ltcg-device-context.md: kick
 * off first if this is the newest fence (as the original does), then wait,
 * bounded, until the semaphore the executor writes reaches the fence. The
 * original then takes its own early exit (0x0022380E) and never sleeps. */
extern GZ_TLS uint32_t g_esp;
extern void sub_002237F0_gen(void);

/* 250 ms was too short: a 3D frame on the software rasteriser can take longer,
 * and once the bound ran out the original's sleep path deadlocked the fight
 * (fight2). Wait up to 2 s; if the GPU queue is drained (USER PUT == GET) and the
 * fence still isn't reached, the release can't be pending, so complete it. */
#define GZ_FENCE_WAIT_MS 2000u
#define GZ_NV2A_USER_PUT 0xFD800040u
#define GZ_NV2A_USER_GET 0xFD800044u

/* -- Sound-chip register access without the trap --
 *
 * DSOUND commits a 3D voice's HRTF filter and volumes (about 20 and 7
 * register accesses per voice) every time the game moves a voice or the
 * listener: every 3D voice, every frame. Each access faulted into the APU
 * model; at 240 fps that was a tenth to a fifth of the game thread.
 * recomp_config/tools/fast_mmio.py rewrites those two lifted functions to call
 * these for addresses in the APU window: the same model calls the fault
 * handler ends in, without the exception. */
extern void *g_apu_state;
extern uint64_t mcpx_apu_mmio_read(void *d, uint64_t addr, unsigned int size);
extern void mcpx_apu_mmio_write(void *d, uint64_t addr, uint64_t val, unsigned int size);
void gz_apu_w32(uint32_t addr, uint32_t value)
{
    mcpx_apu_mmio_write(g_apu_state, addr - 0xFE800000u, value, 4);
}
uint32_t gz_apu_r32(uint32_t addr)
{
    return (uint32_t)mcpx_apu_mmio_read(g_apu_state, addr - 0xFE800000u, 4);
}

/* The renderer's begin-frame (sub_000A8510) inserts a fence and blocks on it
 * at once: the game waits until the GPU has finished the whole previous
 * frame before it starts the next, so the game thread and the pushbuffer
 * executor take turns instead of overlapping. Here that wait is for the
 * fence of the begin-frame before: the executor may run up to one frame
 * behind, and both threads work at the same time. D3D still blocks on its
 * own fences wherever it locks a resource the GPU has yet to use, so buffers
 * are not rewritten early. GZ_GPU_LAG=0 restores the original wait. */
static double gz_fence_wait_s;      /* time blocked on the GPU, for the hitch log */
static double gz_fence_wait_max;
static uint32_t gz_fence_wait_ra[4];
static uint32_t gz_lag_fence(uint32_t fence)
{
    static int on = -1;
    static uint32_t prev, have;
    uint32_t use = fence;
    if (on < 0) { const char *e = getenv("GZ_GPU_LAG"); on = !(e && *e == '0'); }
    if (!on) return fence;
    if (have) use = prev;
    prev = fence; have = 1;
    return use;
}

void sub_002237F0(void)
{
    uint32_t fence = GZ_MEM32(g_esp + 4);
    uint32_t dev = GZ_MEM32(GZ_D3D_DEVICE_PTR);
    uint32_t sem_va = dev ? GZ_MEM32(dev + 0x30) : 0;

    {
        /* GZ_FENCE_LOG=1: who blocks on fences, per 2000 calls. */
        static int flog = -1;
        static unsigned n, begin, swap, other, other_ra[4], other_n[4];
        if (flog < 0) flog = getenv("GZ_FENCE_LOG") != NULL;
        if (flog) {
            uint32_t r0 = GZ_MEM32(g_esp), r1 = GZ_MEM32(g_esp + 12);
            int k;
            if (r0 == 0x0021ED7Cu && r1 == 0x000A853Bu) begin++;
            else if (r0 == 0x00223B10u) swap++;
            else {
                uint32_t key = r0 == 0x0021ED7Cu ? r1 : r0;
                other++;
                for (k = 0; k < 4; k++) if (other_ra[k] == key || !other_ra[k]) { other_ra[k] = key; other_n[k]++; break; }
            }
            if (++n >= 2000) {
                fprintf(stderr, "[FENCE] 2000 waits: begin-frame %u, swap %u, other %u (%08X x%u, %08X x%u, %08X x%u, %08X x%u)\n",
                        begin, swap, other, other_ra[0], other_n[0], other_ra[1], other_n[1], other_ra[2], other_n[2], other_ra[3], other_n[3]);
                n = begin = swap = other = 0; memset(other_ra, 0, sizeof other_ra); memset(other_n, 0, sizeof other_n);
            }
        }
    }
    /* Called as BlockOnFence (return 0x0021ED7C) from begin-frame (0x000A853B). */
    if (GZ_MEM32(g_esp) == 0x0021ED7Cu && GZ_MEM32(g_esp + 12) == 0x000A853Bu) {
        fence = gz_lag_fence(fence);
        GZ_MEM32(g_esp + 4) = fence;
    }

    if (sem_va) {
        gz_register_semaphore();
        /* Same test as the original's early exit: done when
         * (latest - fence) >= (latest - semaphore), unsigned. */
#define GZ_FENCE_DONE() \
        ((uint32_t)(GZ_MEM32(dev + 0x2C) - fence) >= (uint32_t)(GZ_MEM32(dev + 0x2C) - GZ_MEM32(sem_va)))
        if (!GZ_FENCE_DONE()) {
            if (fence == GZ_MEM32(dev + 0x2C)) {
                /* Newest fence: its release isn't submitted yet. KickOff(0). */
                g_esp -= 4; GZ_MEM32(g_esp) = 0;            /* argument */
                g_esp -= 4; GZ_MEM32(g_esp) = 0x0022381Fu;  /* return address, as the original call */
                sub_00223730();                              /* ret 4 pops both */
            }
            {
                /* Wait while the executor is still working through the queue
                 * (GET moving, or PUT != GET): a slow frame - a traced one took
                 * several seconds - must never fall into the original's sleep
                 * path, which spins forever on an event nothing signals (the
                 * "run froze" hang). The 2 s bound restarts whenever GET moves;
                 * after 30 s without progress, complete the fence anyway. */
                DWORD t0 = GetTickCount();
                uint32_t get0 = GZ_MEM32(GZ_NV2A_USER_GET);
                int drained = 0;
                LARGE_INTEGER qa, qb, qf2;
                QueryPerformanceCounter(&qa);
                while (!GZ_FENCE_DONE()) {
                    uint32_t get = GZ_MEM32(GZ_NV2A_USER_GET);
                    DWORD dt;
                    if (get != get0) { get0 = get; t0 = GetTickCount(); }
                    dt = GetTickCount() - t0;
                    drained = GZ_MEM32(GZ_NV2A_USER_PUT) == get;
                    if ((drained && dt >= GZ_FENCE_WAIT_MS) || dt >= 30000u) break;
                    Sleep(0);
                }
                QueryPerformanceCounter(&qb); QueryPerformanceFrequency(&qf2);
                {
                    double w = (double)(qb.QuadPart - qa.QuadPart) / (double)qf2.QuadPart;
                    gz_fence_wait_s += w;
                    if (w > gz_fence_wait_max) {
                        gz_fence_wait_max = w;
                        gz_fence_wait_ra[0] = GZ_MEM32(g_esp); gz_fence_wait_ra[1] = GZ_MEM32(g_esp + 12);
                        gz_fence_wait_ra[2] = GZ_MEM32(g_esp + 16); gz_fence_wait_ra[3] = GZ_MEM32(g_esp + 20);
                    }
                }
                if (!GZ_FENCE_DONE()) {
                    static int s_forced;
                    if (s_forced++ < 8)
                        fprintf(stderr, "[D3D] fence %08X not reached (semaphore %08X), GPU queue %s; completing it\n",
                                fence, GZ_MEM32(sem_va), drained ? "drained" : "stalled");
                    GZ_MEM32(sem_va) = fence;
                }
            }
        }
#undef GZ_FENCE_DONE
    }
    sub_002237F0_gen();
}

/* ── Movie skip (bring-up) ──
 *
 * The game's movie class (vtable 0x00342300): decoder at +0x14C, texture at
 * +0x150, "finished" byte at +0x145. sub_001F05B0 is its per-frame Update
 * (thiscall): it does nothing once +0x145 is set, otherwise it asks the XMV
 * decoder for the next frame and sets +0x145 at end of file. GetState
 * (sub_001F0570) then reports 3 = finished.
 *
 * The title's XMV decoder never consumes its read buffers yet (black movie
 * that never ends), so mark every movie finished before Update runs: the game
 * moves on exactly as if the movie had ended. Set GZ_PLAY_MOVIES=1 to let the
 * real decoder run (for working on it). */
extern GZ_TLS uint32_t g_ecx;
extern void sub_001F05B0_gen(void);

static int gz_movies_skipped;   /* read by the quick-fight boot */

static int gz_play_movies(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GZ_PLAY_MOVIES");
        on = e && *e == '1';
    }
    return on;
}

/* Ending a movie on its very first Update crashed: the next screen's widgets
 * load over the following frames, and the intro sequence destroyed a UI
 * widget (vtable 0x00342634) whose array at +0x30 was still 0xCD-filled by
 * the game's own malloc (sub_0017DEA0). So let each movie "play" for a number
 * of Updates first (default 90, about 3 s at 30 fps; GZ_MOVIE_SKIP_FRAMES). */
static int gz_movie_skip_frames(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("GZ_MOVIE_SKIP_FRAMES");
        n = (e && *e) ? atoi(e) : 90;
        if (n < 1) n = 1;
    }
    return n;
}

/* Host playback (src/gz_movie.c): FFmpeg decodes the XMV and each Update copies
 * the frame due into the movie's YUY2 texture (D3D texture object at +0x150,
 * pixel data at its Data field +4, a physical address). GZ_MOVIES=0 or no
 * ffmpeg.exe: the skip below. */
int  gz_movie_enabled(void);
int  gz_movie_open(uint32_t obj, const char *xbox_path);
int  gz_movie_update(uint32_t obj, uint8_t *dst);
void gz_movie_tick(void);
void gz_ach_frame(void);           /* gz_ach.c: achievements, once per frame */
static void gz_pad_activity_tick(void);
void gz_rb_tick(void);
int gz_rb_active(void);
int gz_rb_deterministic(void);
int gz_rb_replaying(void);
extern void sub_001F0650_gen(void);

/* Skipped movies (no host playback): the title's own movie player still
 * puts a soundtrack on a DirectSound stream (16-bit PCM, 44.1 kHz), and what
 * it queues here is broken - noise, a stuck full-scale value - over the
 * skipped intro movies and the menu's idle movie. That PCM stream is kept out
 * of the mix from the movie's Open until a second after the movie is done.
 * The music is an ADPCM stream and is not affected. GZ_MOVIE_MUTE=0: off. */
extern volatile long g_apu_mute_pcm_streams;
static volatile DWORD gz_movie_unmute_at;   /* GetTickCount() to unmute at; 0 = none pending */
static int gz_movie_mute_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("GZ_MOVIE_MUTE"); on = !(e && *e == '0'); }
    return on;
}

/* Once per game frame (the limiter): end the mute a second after the last
 * skipped movie finished. */
static void gz_movie_mute_tick(void)
{
    DWORD at = gz_movie_unmute_at;
    if (at && (LONG)(GetTickCount() - at) >= 0) {
        gz_movie_unmute_at = 0;
        g_apu_mute_pcm_streams = 0;
        fprintf(stderr, "[MOVIE] movie soundtrack unmuted\n");
    }
}

/* Open(this, const char *file, ...): thiscall, ret 20. */
void sub_001F0650(void)
{
    uint32_t movie = g_ecx, name = GZ_MEM32(g_esp + 4);
    if (!gz_movie_enabled() && !gz_play_movies() && gz_movie_mute_on()) {
        if (!g_apu_mute_pcm_streams) fprintf(stderr, "[MOVIE] movie soundtrack muted while skipped\n");
        g_apu_mute_pcm_streams = 1;
        gz_movie_unmute_at = 0;
    }
    sub_001F0650_gen();
    if (name && gz_movie_enabled())
        gz_movie_open(movie, (const char *)((uintptr_t)name + g_xbox_mem_offset));
}

void sub_001F05B0(void)
{
    if (gz_movie_enabled()) {
        uint32_t movie = g_ecx;
        volatile uint8_t *finished =
            (volatile uint8_t *)((uintptr_t)(movie + 0x145u) + g_xbox_mem_offset);
        uint32_t tex = GZ_MEM32(movie + 0x150u);
        uint32_t data = tex ? GZ_MEM32(tex + 4u) : 0;
        uint8_t *dst = data ? (uint8_t *)((uintptr_t)(0x80000000u | data) + g_xbox_mem_offset) : NULL;
        int st = *finished ? 0 : gz_movie_update(movie, dst);
        if (st == 1) {                  /* playing: the title's decoder stays idle */
            g_esp += 4;                 /* ret */
            return;
        }
        if (st == 2) {
            *finished = 1;
            gz_movies_skipped++;
            sub_001F05B0_gen();
            return;
        }
        /* st == 0: not a host movie - fall through to the skip. */
    }
    if (!gz_play_movies()) {
        static uint32_t s_obj;
        static int s_count, s_done;
        uint32_t movie = g_ecx;
        volatile uint8_t *finished =
            (volatile uint8_t *)((uintptr_t)(movie + 0x145u) + g_xbox_mem_offset);

        if (!*finished) {
            if (movie != s_obj || s_done) {    /* a new movie (Open clears +0x145) */
                s_obj = movie;
                s_count = 0;
                s_done = 0;
            }
            if (++s_count >= gz_movie_skip_frames()) {
                *finished = 1;
                s_done = 1;
                gz_movies_skipped++;
                fprintf(stderr, "[MOVIE] skipped movie object 0x%08X after %d frames\n",
                        movie, s_count);
                if (g_apu_mute_pcm_streams) gz_movie_unmute_at = GetTickCount() + 1000u;
            }
        }
    }
    sub_001F05B0_gen();
}

/* ── UI widget dtor guard (bring-up) ──
 *
 * sub_001F4C60 destroys a UI widget (vtable 0x00342634, 0x54 bytes, ctor
 * sub_001F3B90). It frees each element of the array at +0x30 (count +0x34),
 * then the array. The ctor never sets those two fields and the game's malloc
 * fills with 0xCD, so a widget destroyed before it was configured frees
 * 0xCDCDCDCD and crashes in RtlFreeHeap. Seen right after the intro movies.
 * Treat the never-written fields as an empty array. Logged, because the
 * missing configure step is still a bug to find (MODLOG). */
extern void sub_001F4C60_gen(void);

static volatile int g_gz_trace_frees;   /* set once the widget guard fires */

void sub_001F4C60(void)
{
    uint32_t w = g_ecx;

    if (GZ_MEM32(w + 0x30) == 0xCDCDCDCDu || GZ_MEM32(w + 0x34) == 0xCDCDCDCDu) {
        g_gz_trace_frees = 1;
        fprintf(stderr, "[UI] widget 0x%08X destroyed unconfigured (+0x30=%08X +0x34=%08X); treating as empty\n",
                w, GZ_MEM32(w + 0x30), GZ_MEM32(w + 0x34));
        GZ_MEM32(w + 0x30) = 0;
        GZ_MEM32(w + 0x34) = 0;
    }
    sub_001F4C60_gen();
}

/* ── Process heap handle repair (bring-up workaround) ──
 *
 * 0x004BDED4 holds XAPI's process heap handle (0x00F81000). Its only writer is
 * the init at 0x00015E25, yet right after the intro sequence its third byte
 * is zeroed (0x00F81000 -> 0x00001000) by a writer not found yet (see MODLOG:
 * invisible to RECOMP_WATCH, happens during DirectSound teardown). The next
 * malloc then passes heap 0x1000 and crashes. The handle never legitimately
 * changes after init, so remember the first good value and restore it at the
 * getter malloc uses (sub_00017711) and at RtlAllocateHeap / RtlFreeHeap,
 * fixing their heap argument too. Treats the symptom: the stray write may be
 * damaging other state as well. */
#define GZ_PROCESS_HEAP_VA 0x004BDED4u
static uint32_t g_gz_heap;

static void gz_heap_check(void)
{
    uint32_t v = GZ_MEM32(GZ_PROCESS_HEAP_VA);

    if (!g_gz_heap) {
        if (v >= 0x00010000u && v < 0x04000000u)
            g_gz_heap = v;                      /* first good value: set by init */
        return;
    }
    if (v != g_gz_heap) {
        static int s_n;
        if (s_n++ < 4)
            fprintf(stderr, "[HEAPFIX] process heap handle was %08X, restored %08X\n", v, g_gz_heap);
        GZ_MEM32(GZ_PROCESS_HEAP_VA) = g_gz_heap;
    }
}

/* If a caller already loaded the damaged handle, fix the argument too. */
static void gz_heap_fix_arg(void)
{
    uint32_t heap = GZ_MEM32(g_esp + 4);

    if (g_gz_heap && heap != g_gz_heap && heap < 0x00010000u
            && (heap & 0xFFFFu) == (g_gz_heap & 0xFFFFu)) {
        GZ_MEM32(g_esp + 4) = g_gz_heap;
    }
}

/* XAPI GetProcessHeap-style getter: mov eax, [0x004BDED4]; ret */
extern void sub_00017711_gen(void);
void sub_00017711(void)
{
    gz_heap_check();
    sub_00017711_gen();
}

/* RtlFreeHeap(heap, flags, ptr), stdcall */
extern void sub_00019448_gen(void);
void sub_00019448(void)
{
    gz_heap_check();
    gz_heap_fix_arg();
    sub_00019448_gen();
}

/* RtlAllocateHeap(heap, flags, size), stdcall */
extern void sub_00018CCD_gen(void);
void sub_00018CCD(void)
{
    gz_heap_check();
    gz_heap_fix_arg();
    sub_00018CCD_gen();
}

/* D3D ring-space check: spins on PGRAPH 0x400B10 vs the semaphore. */
void sub_00223560(void)
{
    gz_register_semaphore();
    sub_00223560_gen();
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* -- Match results, for achievements (gz_ach.c) -----------------------------
 *
 * The game's scripts reach native objects through per-class "invoke"
 * functions: invoke(this = wrapper, method index, args, result), ret 0x14,
 * reached only through vtables (so only indirect calls, which ask this lookup
 * first). The wrapper's +4 is the object.
 *   GameResults   invoke 0x00104330: 0 getPlayer(i) -> object + i * 0x3C,
 *                 1 setVictoryAchieved, 2 victoryAchieved
 *   PlayerResults invoke 0x00104260: 0 isVictorious, 1 getScore, 2 setScore,
 *                 3 awardPoints, 4 setVictory (sub_000FE970: byte +0x34)
 * A player's results are a 0x3C-byte record: +0x34 victory, +0x38 score. */
#include <string.h>
#ifndef GZ_GAMEINFO
#define GZ_GAMEINFO 0x003F0958u      /* the match setup (see "Quick fight" below) */
#endif
void sub_00104260(void);
void sub_00104330(void);
void gz_ach_event(const char *name);
int gz_net_on(void);
static uint32_t gz_results_base;     /* the GameResults object = player 0's record */
static int gz_results_won[4];

/* "New_Tokyo_Main_DAY" -> "new_tokyo"; names in events are short and lower case. */
static void gz_ach_name(const char *in, char *out, size_t n, int stop_at_main)
{
    size_t k = 0;
    for (; *in && k + 1 < n; in++) {
        if (stop_at_main && !strncmp(in, "_Main", 5)) break;
        out[k++] = (*in >= 'A' && *in <= 'Z') ? (char)(*in + 32) : *in;
    }
    out[k] = 0;
}

/* The match is over (setVictoryAchieved(true)): events for player 1, the one
 * on the first controller. */
static void gz_match_over(void)
{
    const char *gi = (const char *)((uintptr_t)GZ_GAMEINFO + g_xbox_mem_offset);
    uint32_t players = GZ_MEM32(GZ_GAMEINFO + 0x118);
    char monster[40], city[72], ev[128];
    int online = gz_net_on();
    gz_ach_name(gi + 0x160, monster, sizeof monster, 0);
    gz_ach_name(gi + 0xD0, city, sizeof city, 1);
    fprintf(stderr, "[MATCH] over: %u players, player 1 (%s) %s, city %s%s\n", players, monster,
            gz_results_won[0] ? "won" : "lost", city, online ? ", online" : "");
    gz_ach_event("match_played");
    if (online) gz_ach_event("online_match_played");
    if (gz_results_won[0]) {
        gz_ach_event("match_won");
        snprintf(ev, sizeof ev, "match_won_%s", monster); gz_ach_event(ev);
        snprintf(ev, sizeof ev, "match_won_city_%s", city); gz_ach_event(ev);
        if (online) gz_ach_event("online_match_won");
    }
    memset(gz_results_won, 0, sizeof gz_results_won);
}

static void gz_game_results_invoke(void)
{
    uint32_t idx = GZ_MEM32(g_esp + 4), self = g_ecx, obj;
    sub_00104330();
    if (!self || (obj = GZ_MEM32(self + 4)) == 0) return;
    gz_results_base = obj;
    if (idx == 1 && (GZ_MEM32(obj + 0xF8) & 0xFF)) gz_match_over();   /* setVictoryAchieved(true) */
}

static void gz_player_results_invoke(void)
{
    uint32_t idx = GZ_MEM32(g_esp + 4), self = g_ecx;
    sub_00104260();
    if (idx == 4 && self) {                      /* setVictory */
        uint32_t obj = GZ_MEM32(self + 4), off = obj - gz_results_base;
        int won = (GZ_MEM32(obj + 0x34) & 0xFF) != 0;
        if (gz_results_base && obj >= gz_results_base && off % 0x3C == 0 && off / 0x3C < 4)
            gz_results_won[off / 0x3C] = won;
        fprintf(stderr, "[MATCH] setVictory(%d) on results %08X (player %d)\n", won, obj,
                gz_results_base && obj >= gz_results_base && off % 0x3C == 0 ? (int)(off / 0x3C) + 1 : 0);
    }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x00104330u) return gz_game_results_invoke;
    if (xbox_va == 0x00104260u) return gz_player_results_invoke;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── D3D kickoff without the write-combine flush handshake ─────────────
 *
 * sub_00223610 (XDK D3D, called from KickOff) publishes the pushbuffer PUT
 * to the GPU. Before that it sets PFB 0x100410 bit 16 -- flush the CPU's
 * write-combining buffers -- and spins until the "hardware" clears it. Here
 * nothing is write-combined (guest memory is ordinary coherent RAM) and the
 * bit is cleared by the toolkit's ack/executor thread, so every kickoff
 * waited for that thread; profiles put the game thread 60-90% of a fight
 * frame in that spin. This is the same function without the handshake.
 * GZ_KICK_FLUSH=1 runs the original. */
extern GZ_TLS uint32_t g_edx;
extern void sub_00223610_gen(void);
void sub_0021FD00(void);

void sub_00223610(void)
{
    static int flush = -1;
    uint32_t dev = g_ecx, flags, put;

    if (flush < 0) {
        const char *e = getenv("GZ_KICK_FLUSH");
        flush = e && e[0] == '1';
    }
    if (flush) {
        sub_00223610_gen();
        return;
    }
    flags = GZ_MEM32(dev + 8);
    put = (flags & 4) ? GZ_MEM32(dev + 0x770) : GZ_MEM32(dev);
    if (!(flags & 0x2000)) {
        /* (the PFB flush + spin of the original goes here) */
        uint32_t user = GZ_MEM32(dev + 0x1C20);
        GZ_MEM32(user + 0x40) = put & 0x0FFFFFFFu;             /* USER PUT */
        g_edx = put & 0x0FFFFFFFu;
        g_ecx = user;
        g_eax = GZ_MEM32(0x22DDD0);
        if (g_eax) {
            do {
                g_esp -= 4; GZ_MEM32(g_esp) = 0x00223679u;      /* return address */
                sub_0021FD00();
            } while (g_eax != 0);
            GZ_MEM32(dev + 8) = GZ_MEM32(dev + 8) | 0x2000;
        }
    } else {
        uint32_t user = 0x22DDC0u - 0x40u, p = GZ_MEM32(dev) & 0x0FFFFFFFu;
        GZ_MEM32(dev + 0x1C20) = user;
        GZ_MEM32(user + 0x40) = p;
        GZ_MEM32(GZ_MEM32(dev + 0x1C20) + 0x44) = p;
        g_eax = GZ_MEM32(dev + 0x2C) - 2;
        g_ecx = GZ_MEM32(dev + 0x30);
        GZ_MEM32(g_ecx) = g_eax;
        g_edx = GZ_MEM32(dev + 0x2478);
        GZ_MEM32(dev + 0x1DE4) = g_edx;
    }
    g_esp += 4;                                                /* ret */
}

/* ── GZ_TICK_LOG: who reads the millisecond clock, per second ───────────
 *
 * sub_0001500C is the game's GetTickCount (returns *KeTickCount). Fight
 * logic that steps by elapsed time reads it every frame; logic that steps
 * once per frame does not. With GZ_TICK_LOG=1 each second prints the call
 * count and the top return addresses, which tells the two apart before the
 * frame rate is uncapped. */
extern void sub_0001500C_gen(void);

void sub_0001500C(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("GZ_TICK_LOG") != NULL;
    if (on) {
        static uint32_t ret_va[32], ret_n[32], total;
        static DWORD t0;
        uint32_t ra = GZ_MEM32(g_esp);
        DWORD now = GetTickCount();
        int k;
        total++;
        for (k = 0; k < 32 && ret_n[k] && ret_va[k] != ra; k++)
            ;
        if (k < 32) { ret_va[k] = ra; ret_n[k]++; }
        if (!t0) t0 = now;
        if (now - t0 >= 1000) {
            fprintf(stderr, "[TICK] %u calls/s:", total);
            for (k = 0; k < 32 && ret_n[k]; k++)
                if (ret_n[k] * 20 >= total)              /* callers >= 5% */
                    fprintf(stderr, " %08X x%u", ret_va[k], ret_n[k]);
            fprintf(stderr, "\n");
            memset(ret_va, 0, sizeof ret_va);
            memset(ret_n, 0, sizeof ret_n);
            total = 0;
            t0 = now;
        }
    }
    sub_0001500C_gen();
}

/* ── Quick fight: boot straight into a Versus match (GZ_QUICK_FIGHT=1) ──
 *
 * The menus are scripts; when City Select is confirmed they have filled the
 * static GameInfo at 0x003F0958 (script class "ShellGameInfo": game script
 * +0x90, city name +0xD0, player count +0x118, per-player records of 0xD8
 * bytes with the monster name at +0x160 and its file name at +0x17F) and call
 * the shell's launchGame, native sub_0009AA20: no arguments, it posts message
 * 0x800 to the manager at 0x00425BD0. So once the intro movies are over and
 * the main menu has had a few seconds to come up, write a captured match
 * config and call launchGame ourselves.
 *
 * GZ_QF_P1 / GZ_QF_P2: monster names (e.g. godzilla2k, gigan), GZ_QF_CITY:
 * city name (e.g. New_Tokyo_Main_DAY), GZ_QF_DELAY: frames to wait after the
 * last intro movie (default 240). */
/* The quick-fight boot (GZ_QUICK_FIGHT=1) replays the match setup the menus
 * last made: every match started from the menus saves the GameInfo block
 * (0x640 bytes at 0x003F0958) to gz_quickfight.bin (GZ_QF_FILE overrides),
 * from the player's own game. Nothing of the game ships with the source. */
#define GZ_GAMEINFO_SIZE 0x640u
static int gz_qf_launching;
static const char *gz_qf_file(void)
{
    const char *f = getenv("GZ_QF_FILE");
    return (f && *f) ? f : "gz_quickfight.bin";
}
#ifndef GZ_GAMEINFO
#define GZ_GAMEINFO 0x003F0958u
#endif

static void gz_qf_str(uint32_t va, const char *s, size_t max)
{
    volatile char *d = (volatile char *)((uintptr_t)va + g_xbox_mem_offset);
    size_t i = 0;
    for (; s[i] && i + 1 < max; i++) d[i] = s[i];
    for (; i < max; i++) d[i] = 0;
}

/* -- Biollante (cut from the roster, complete on the disc) ----------------
 *
 * bundle/Biollante.zip/.cmf, ActionData/[AS]MBiollante.bsf, hudbiollante.bdl
 * and her sounds all ship on the Xbox disc; only the select screen has no
 * slot for her. A match started with the monster name "biollante" loads and
 * plays her normally. So when a match launches (Shell.launchGame,
 * sub_0009AA20), a player holding BOTH triggers on their controller gets
 * Biollante instead of the monster they picked. GZ_BIOLLANTE=<players>, e.g.
 * "1" or "12", forces it for those players (2 can be the computer). */
#include <xinput.h>
extern void sub_0009AA20_gen(void);
int usb_gamepad_report(int pad, uint8_t *out, int max);

/* When a player last pressed or released a button (any pad), so a match the
 * player started can be told from one the main menu's attract demo starts:
 * the demo runs with nobody touching the controller. Called once per frame. */
static DWORD gz_pad_last_change;
static void gz_pad_activity_tick(void)
{
    static uint8_t last[4][10];
    uint8_t rep[20];
    int i;
    for (i = 0; i < 4; i++) {
        if (usb_gamepad_report(i, rep, sizeof rep) < 20) continue;
        if (memcmp(last[i], rep + 2, 10)) {       /* digital and analog buttons */
            memcpy(last[i], rep + 2, 10);
            gz_pad_last_change = GetTickCount();
        }
    }
}

void sub_0009AA20(void)
{
    const char *force = getenv("GZ_BIOLLANTE");
    uint32_t players = GZ_MEM32(GZ_GAMEINFO + 0x118), i;
    for (i = 0; i < 4 && i < players; i++) {
        XINPUT_STATE st;
        int pick = force && strchr(force, (char)('1' + i)) != NULL;
        if (!pick) {
            /* The pad as the game sees it (real controller or a test script):
             * report bytes 10 and 11 are the two triggers. */
            uint8_t rep[20];
            if (usb_gamepad_report((int)i, rep, sizeof rep) >= 20)
                pick = rep[10] > 128 && rep[11] > 128;
        }
        if (!pick && XInputGetState(i, &st) == ERROR_SUCCESS)
            pick = st.Gamepad.bLeftTrigger > 128 && st.Gamepad.bRightTrigger > 128;
        if (pick) {
            gz_qf_str(GZ_GAMEINFO + 0x160 + i * 0xD8, "biollante", 0x1F);
            gz_qf_str(GZ_GAMEINFO + 0x17F + i * 0xD8, "biollante", 0x1F);
            fprintf(stderr, "[BIOLLANTE] player %u plays Biollante\n", i + 1);
        }
    }
    /* A Versus match the menus set up (two or more players): keep its setup for
     * the quick-fight boot. Not a single-player mode's: replayed without the
     * menus it has no profile, and its save at the end of the match (a save
     * with no name) never finishes. Not the attract demo's either (nobody has
     * touched a controller for 10 s): replayed, it goes back to the menu. */
    /* Not a System Link match's either (a player on another console): it
     * would replace the last local setup with one quick fight cannot play. */
    for (i = 0; i < 4; i++)
        if (GZ_MEM8(GZ_GAMEINFO + 0x160 + i * 0xD8 + GZ_PLAYER_REMOTE)) players = 0;
    if (!gz_qf_launching && players >= 2 && GZ_MEM32(GZ_GAMEINFO + 0x238) != 0
            && gz_pad_last_change && GetTickCount() - gz_pad_last_change < 10000) {
        FILE *f = fopen(gz_qf_file(), "wb");
        if (f) {
            fwrite((const void *)((uintptr_t)GZ_GAMEINFO + g_xbox_mem_offset), 1, GZ_GAMEINFO_SIZE, f);
            fclose(f);
            fprintf(stderr, "[QUICKFIGHT] match setup saved to %s\n", gz_qf_file());
        }
    }
    sub_0009AA20_gen();
}

/* -- Biollante select-screen cell (GZ_BIOLLANTE_SLOT=1) ------------------
 *
 * mods/biollante_slot (RECOMP_OVERLAY_DIR) adds a "biollante" cell to the
 * monster select layout; the select script treats a cell by its name. Whether
 * a cell is selectable comes from the save profile's attribute of that name,
 * read by index through sub_00088930(this = 0x00425BD0, storage, index):
 * index = position in ShellData/attributes.xfg, where biollante (55) is
 * "hidden" and has no monster class. With the slot on, a read of her
 * attribute is answered with Gigan's (91), who is unlocked from the start. */
extern void sub_00088930_gen(void);
#define GZ_ATTR_BIOLLANTE 55u
#define GZ_ATTR_GIGAN     91u

void sub_00088930(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GZ_BIOLLANTE_SLOT");
        on = e && *e == '1';
    }
    {
        /* GZ_ATTR_LOG=1: each attribute index the first time it is read, with
         * its value (finding out which entries a screen looks at). */
        static int log = -1;
        static unsigned char seen[512];
        uint32_t idx = GZ_MEM32(g_esp + 8);
        if (log < 0) log = getenv("GZ_ATTR_LOG") != NULL;
        (void)on;       /* the select screen does not read locks through here; see the tick below */
        sub_00088930_gen();
        if (log && idx < 512 && !seen[idx]) {
            seen[idx] = 1;
            fprintf(stderr, "[ATTR] index %u -> %d\n", idx, (int)g_eax);
        }
    }
}

/* The select screen takes a cell's lock state from the live profile tables:
 * four storage slots at 0x00429180 + n * 0x1068, one 32-bit value per
 * attribute in attributes.xfg order (0 unlocked, 1 locked, 2 hidden; found by
 * searching a RAM snapshot for the file's default pattern). With the slot mod
 * on, her entry (55) is held at "unlocked" in every slot. */
#define GZ_PROFILE_TABLE  0x00429180u
#define GZ_PROFILE_STRIDE 0x1068u
static void gz_biollante_slot_tick(void)
{
    static int on = -1;
    uint32_t n;
    if (on < 0) {
        const char *e = getenv("GZ_BIOLLANTE_SLOT");
        on = e && *e == '1';
        if (on) fprintf(stderr, "[BIOLLANTE] select-screen cell on: her profile entry is held unlocked\n");
    }
    if (!on) return;
    for (n = 0; n < 4; n++) {
        uint32_t va = GZ_PROFILE_TABLE + n * GZ_PROFILE_STRIDE + GZ_ATTR_BIOLLANTE * 4u;
        if (GZ_MEM32(va) == 2u || GZ_MEM32(va) == 1u) GZ_MEM32(va) = 0u;
    }
}

static void gz_quick_fight_tick(void)
{
    static int on = -1, wait, done;
    if (on < 0) {
        const char *e = getenv("GZ_QUICK_FIGHT"), *d = getenv("GZ_QF_DELAY");
        on = e && *e == '1';
        wait = (d && *d) ? atoi(d) : 240;
    }
    if (!on || done || gz_movies_skipped < 6) return;
    if (wait-- > 0) return;
    done = 1;
    {
        volatile unsigned char *gi = (volatile unsigned char *)((uintptr_t)GZ_GAMEINFO + g_xbox_mem_offset);
        const char *p1 = getenv("GZ_QF_P1"), *p2 = getenv("GZ_QF_P2"), *city = getenv("GZ_QF_CITY");
        uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
        unsigned char blob[GZ_GAMEINFO_SIZE];
        size_t i;
        FILE *f = fopen(gz_qf_file(), "rb");
        extern int gz_rbn_on(void), gz_rbn_local_port(void);
        int net_client = gz_rbn_on() && gz_rbn_local_port() == 1;
        if (net_client) {
            /* rollback netplay, player 2: the host's setup replaces this one
             * (gz_rbn_launch_sync), so none of its own is needed */
            if (f) fclose(f);
            for (i = 0; i < sizeof blob; i++) blob[i] = gi[i];
        } else if (!f || fread(blob, 1, sizeof blob, f) != sizeof blob) {
            if (f) fclose(f);
            fprintf(stderr, "[QUICKFIGHT] %s not found: start one match from the menus first "
                    "(it is saved then), and quick fight will replay that setup\n", gz_qf_file());
            return;
        } else
            fclose(f);
        if (!net_client) {
            uint32_t players;
            memcpy(&players, blob + 0x118, 4);
            if (players < 2 || !blob[0x238]) {
                fprintf(stderr, "[QUICKFIGHT] %s holds a single-player setup (from an older version): "
                        "start one Versus match from the menus first, and quick fight will replay that\n", gz_qf_file());
                return;
            }
        }
        for (i = 0; i < sizeof blob; i++) gi[i] = blob[i];
        GZ_MEM32(GZ_GAMEINFO + 0x138) = GetTickCount();        /* random seed */
        {
            /* GZ_QF_SEED=<n>: the match's random seed, for a match that must
             * play the same on every run (netplay passes the host's). */
            const char *sd = getenv("GZ_QF_SEED");
            if (sd && *sd) GZ_MEM32(GZ_GAMEINFO + 0x138) = (uint32_t)strtoul(sd, NULL, 0);
        }
        {
            /* Quick fight is a local match. A player block's byte at +0xA4
             * marks a System Link player (one on another console); a setup
             * kept from a System Link match replayed with it set opens on the
             * network "WAITING FOR PLAYER" screen, and the absent player is
             * dropped from the game a few seconds later. */
            uint32_t p;
            for (p = 0; p < 4; p++)
                GZ_MEM8(GZ_GAMEINFO + 0x160 + p * 0xD8 + GZ_PLAYER_REMOTE) = 0;
        }
        if (p1 && *p1) { gz_qf_str(GZ_GAMEINFO + 0x160, p1, 0x1F); gz_qf_str(GZ_GAMEINFO + 0x17F, p1, 0x1F); }
        if (p2 && *p2) { gz_qf_str(GZ_GAMEINFO + 0x238, p2, 0x1F); gz_qf_str(GZ_GAMEINFO + 0x257, p2, 0x1F); }
        if (city && *city) gz_qf_str(GZ_GAMEINFO + 0xD0, city, 0x40);
        {
            /* GZ_QF_ROUND_TIME=<seconds>, GZ_QF_ROUNDS=<n>: a short match (for
             * testing what follows one, like the results screen). ShellGameInfo
             * get/setRoundTime is the float at +0x120, get/setRounds +0x128. */
            const char *rt = getenv("GZ_QF_ROUND_TIME"), *rn = getenv("GZ_QF_ROUNDS");
            if (rt && *rt) { float t = (float)atof(rt); uint32_t b; memcpy(&b, &t, 4); GZ_MEM32(GZ_GAMEINFO + 0x120) = b; }
            if (rn && *rn) GZ_MEM32(GZ_GAMEINFO + 0x128) = (uint32_t)atoi(rn);
        }
        fprintf(stderr, "[QUICKFIGHT] launching %s vs %s in %s\n",
                (const char *)(gi + 0x160), (const char *)(gi + 0x238), (const char *)(gi + 0xD0));
        {
            /* Rollback netplay (gz_rbnet.c): the host's setup for both sides. */
            extern int gz_rbn_launch_sync(uint8_t *gameinfo, uint32_t size);
            uint8_t setup[GZ_GAMEINFO_SIZE];
            for (i = 0; i < sizeof setup; i++) setup[i] = gi[i];
            if (gz_rbn_launch_sync(setup, (uint32_t)sizeof setup))
                for (i = 0; i < sizeof setup; i++) gi[i] = setup[i];
        }
        g_esp -= 4; GZ_MEM32(g_esp) = 0x00012240u;             /* return address */
        { extern void gz_rb_arm(void); gz_rb_arm(); }
        gz_qf_launching = 1;
        sub_0009AA20();                                         /* Shell::launchGame */
        gz_qf_launching = 0;
        g_eax = eax; g_ecx = ecx; g_edx = edx;
    }
}

/* Test aid: GZ_KO=<seconds> -- that long after a fight's opponent (player 2)
 * appears, set its hit points to 0 so player 1 wins by knockout (to reach a
 * results screen fast). Game object 0x004424F0: monster count at +0x450, list
 * at +0x454 (8 bytes each, in RAM or the 0x80000000 window); monster +0xDC =
 * player number, +0x85C = hit points (float). */
static void gz_ko_tick(void)
{
    static int secs = -1;
    static DWORD last, since;
    static uint32_t cur, done;
    uint32_t n, i, m = 0;
    if (secs < 0) { const char *e = getenv("GZ_KO"); secs = e ? atoi(e) : 0; }
    if (secs <= 0 || GetTickCount() - last < 250u) return;
    last = GetTickCount();
    n = GZ_MEM32(0x004424F0u + 0x450u);
    for (i = 0; i < n && i < 8; i++) {
        uint32_t c = GZ_MEM32(0x004424F0u + 0x454u + i * 8u);
        if (((c >= 0x10000u && c < 0x04000000u) || (c >= 0x80000000u && c < 0x84000000u))
                && GZ_MEM32(c + 0xDCu) == 1u) { m = c; break; }
    }
    if (!m) { cur = 0; return; }
    if (m != cur) { cur = m; since = GetTickCount(); done = 0; }
    if (!done && GetTickCount() - since >= (DWORD)secs * 1000u) {
        float hp;
        uint32_t x = GZ_MEM32(m + 0x85Cu);
        memcpy(&hp, &x, 4);
        if (hp > 0.0f) {
            GZ_MEM32(m + 0x85Cu) = 0;
            fprintf(stderr, "[KO] opponent %08X hit points %g -> 0\n", m, hp);
        }
        done = 1;
    }
}

/* ── Tournament mode: everything unlocked (GZ_TOURNAMENT=1) ─────────────
 *
 * The cheat codes the menus accept set bits in GameInfo's "shell cheats" word
 * (script methods get/set/clearShellCheats; GameInfo + 0x4C0 = 0x003F0E18),
 * which the menu scripts read to decide what is selectable. Tournament mode
 * keeps the unlock bits set every frame, whatever the save profile holds.
 * Only the unlock bits: the script enum ShellCheats (ShellData/rules.bsf)
 * puts the gallery, monster, Monster Island, Mothership and city unlocks at
 * bits 0-12 and the "unlock monsters / all" groups at 25-27; bits 13-24 are
 * real cheats (micro monsters, black and white, technicolor, a dozen
 * continues in Adventure, version and sound-log debug). Setting all 32 turned
 * those on too.
 * GZ_SHELL_CHEATS=<hex> overrides the mask (for finding out what a bit does). */
#define GZ_SHELL_CHEATS_VA (0x003F0958u + 0x4C0u)
int  gz_opt_tournament(void);       /* gz_settings.c */
int  gz_opt_ps_prompts(void);
int  gz_opt_rate(void);
int  gz_opt_wide(void);
void gz_opt_toggle(int which);

static int gz_tournament_on(void)
{
    /* Online (System Link, GZ_NET=1) always uses tournament mode so both
     * sides have the same full roster. */
    static int net = -1;
    if (net < 0) net = getenv("GZ_NET") && getenv("GZ_NET")[0] == '1';
    return net || gz_opt_tournament();
}

static void gz_tournament_tick(void)
{
    static int init, held;
    static uint32_t mask = 0x00001FFFu | 0x0E000000u, before;
    if (!init) {
        const char *m = getenv("GZ_SHELL_CHEATS");
        init = 1;
        if (m && *m) mask = (uint32_t)strtoul(m, NULL, 16);
    }
    if (gz_tournament_on()) {
        if (!held) {
            before = GZ_MEM32(GZ_SHELL_CHEATS_VA);
            held = 1;
            fprintf(stderr, "[TOURNAMENT] on: shell cheat bits %08X held\n", mask);
        }
        GZ_MEM32(GZ_SHELL_CHEATS_VA) |= mask;
    } else if (held) {
        GZ_MEM32(GZ_SHELL_CHEATS_VA) = before;      /* what the profile had */
        held = 0;
        fprintf(stderr, "[TOURNAMENT] off: shell cheat bits back to %08X\n", before);
    }
}

/* -- Options menu toggles ------------------------------------------------
 *
 * mods/menu (RECOMP_OVERLAY_DIR) adds five entries to the Options screen's
 * MainBox list: GzTournament, GzPrompts, GzRate, GzWide and GzSpeed
 * (recomp_config/tools/make_options_items.py). The game's Options script
 * asks the highlighted entry for its name when A is pressed and does nothing
 * for a name it does not know, so the recomp acts on them here.
 *
 * Script natives used (ShellObject family; a script-side "junction" holds
 * the real object at +4):
 *   sub_00097DE0  ShellObject.findObject   (this junction, script string, out)
 *                 -> sub_0007B5A0(this object, char *name), returns the object
 *   sub_0009B0C0  ShellObject.getName      (this junction, out string)
 *                 name = *(char **)(object + 0x3C)
 *   sub_00078F20  ShellText::setText(this object, char *text)
 */
extern void sub_00097DE0_gen(void);
extern void sub_0009B0C0_gen(void);
void sub_0007B5A0(void);
void sub_00078F20(void);

#define GZ_OPT_N 5
static const char *const gz_opt_names[GZ_OPT_N] = { "GzTournament", "GzPrompts", "GzRate", "GzWide", "GzSpeed" };
int gz_opt_turbo(void);

static const char *gz_guest_cstr(uint32_t va)
{
    return va ? (const char *)((uintptr_t)va + g_xbox_mem_offset) : "";
}

static void gz_opt_label(int which, char *out, size_t max)
{
    if (which == 0)
        snprintf(out, max, "TOURNAMENT MODE: %s", gz_tournament_on() ? "ON" : "OFF");
    else if (which == 1)
        snprintf(out, max, "BUTTON ICONS: %s", gz_opt_ps_prompts() ? "PLAYSTATION" : "XBOX");
    else if (which == 2)
        snprintf(out, max, "FRAME RATE: %d", gz_opt_rate());
    else if (which == 3)
        snprintf(out, max, "ASPECT: %s", gz_opt_wide() ? "16:9" : "4:3");
    else
        snprintf(out, max, "GAME SPEED: %s", gz_opt_turbo() ? "TURBO" : "NORMAL");
}

/* Call a guest thiscall function taking one C string; returns eax. */
static uint32_t gz_call_str(void (*fn)(void), uint32_t self, const char *text)
{
    uint32_t sp = g_esp, buf = sp - 0x100u, r;
    size_t n = strlen(text);
    if (n > 0x7F) n = 0x7F;
    memcpy((char *)((uintptr_t)buf + g_xbox_mem_offset), text, n);
    *((char *)((uintptr_t)buf + g_xbox_mem_offset) + n) = 0;
    g_esp = buf - 0x10u;
    g_esp -= 4; GZ_MEM32(g_esp) = buf;              /* argument */
    g_esp -= 4; GZ_MEM32(g_esp) = 0x00097DE0u;      /* return address */
    g_ecx = self;
    fn();
    r = g_eax;
    g_esp = sp;
    return r;
}

static void gz_opt_set_text(uint32_t obj, int which)
{
    char label[64];
    gz_opt_label(which, label, sizeof label);
    gz_call_str(sub_00078F20, obj, label);
}

/* Armed again once no pad holds A (XID report byte 4 = analog A). */
static int   gz_opt_armed = 1;
static DWORD gz_opt_last;
static int gz_opt_a_held(void)
{
    int pad;
    for (pad = 0; pad < 4; pad++) {
        uint8_t rep[20];
        if (usb_gamepad_report(pad, rep, sizeof rep) >= 20 && rep[4] > 32) return 1;
    }
    return 0;
}

static void gz_opt_tick(void)
{
    if (!gz_opt_armed && !gz_opt_a_held()) gz_opt_armed = 1;
}

void sub_00097DE0(void)
{
    uint32_t junction = g_ecx, str = GZ_MEM32(g_esp + 4);
    uint32_t parent = junction ? GZ_MEM32(junction + 4) : 0;
    int is_main = str && !strcmp(gz_guest_cstr(GZ_MEM32(str + 0x1C)), "MainBox");
    int is_sel = str && !strcmp(gz_guest_cstr(GZ_MEM32(str + 0x1C)), "Selector");
    sub_00097DE0_gen();
    if (is_sel && parent) {
        /* The Multi-Player menu's list: its Xbox Live entry ("online") is
         * Rollback Netplay now (see sub_0009B0C0). */
        uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
        uint32_t obj = gz_call_str(sub_0007B5A0, parent, "online");
        if (obj) gz_call_str(sub_00078F20, obj, "ROLLBACK NETPLAY");
        g_eax = eax; g_ecx = ecx; g_edx = edx;
    }
    if (is_main && parent) {
        uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
        int i;
        for (i = 0; i < GZ_OPT_N; i++) {
            uint32_t obj = gz_call_str(sub_0007B5A0, parent, gz_opt_names[i]);
            if (obj) gz_opt_set_text(obj, i);
        }
        g_eax = eax; g_ecx = ecx; g_edx = edx;
    }
}

void sub_0009B0C0(void)
{
    uint32_t junction = g_ecx;
    uint32_t obj = junction ? GZ_MEM32(junction + 4) : 0;
    uint32_t name = obj ? GZ_MEM32(obj + 0x3C) : 0;
    int i, which = -1, rollback = 0;
    {
        /* A menu list asks its entries' names (highlight, select): the menus
         * are back, and a netplay session leaves its fight mode (gz_rbnet.c:
         * no guesses outside a fight). The fight's HUD looks objects up too,
         * but never asks a list entry's name. */
        extern void gz_rbn_set_fight(int on);
        gz_rbn_set_fight(0);
    }
    if (name)
        for (i = 0; i < GZ_OPT_N; i++)
            if (!strcmp(gz_guest_cstr(name), gz_opt_names[i])) which = i;
    if (name && !strcmp(gz_guest_cstr(name), "online")) {
        /* Rollback Netplay in place of Xbox Live: the Multi-Player script
         * hears a name it does not know (so it never starts the Live
         * sign-in), and A on it opens the netplay prompt instead. */
        volatile char *s = (volatile char *)((uintptr_t)name + g_xbox_mem_offset);
        s[0] = 'g'; s[1] = 'z'; s[2] = 'r'; s[3] = 'b';
        sub_0009B0C0_gen();
        s[0] = 'o'; s[1] = 'n'; s[2] = 'l'; s[3] = 'i';
        rollback = 1;
    } else
        sub_0009B0C0_gen();
    if (rollback) {
        DWORD now = GetTickCount();
        if (getenv("GZ_OPT_LOG"))
            fprintf(stderr, "[OPTIONS] getName online (armed %d, A %d)\n", gz_opt_armed, gz_opt_a_held());
        if (gz_opt_armed && gz_opt_a_held() && now - gz_opt_last > 150u) {
            extern void gz_rbn_menu_pick(void);
            uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
            gz_opt_last = now;
            gz_opt_armed = 0;
            gz_rbn_menu_pick();
            g_eax = eax; g_ecx = ecx; g_edx = edx;
        }
        return;
    }
    if (which >= 0) {
        DWORD now = GetTickCount();
        if (getenv("GZ_OPT_LOG")) fprintf(stderr, "[OPTIONS] getName %s\n", gz_opt_names[which]);
        /* The script asks for the name on every menu event (moving the
         * highlight, back, key repeat), not only on select: act once per
         * press of A. */
        if (gz_opt_armed && gz_opt_a_held() && now - gz_opt_last > 150u) {
            uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
            gz_opt_last = now;
            gz_opt_armed = 0;
            gz_opt_toggle(which);
            gz_opt_set_text(obj, which);
            g_eax = eax; g_ecx = ecx; g_edx = edx;
        }
    }
}

/* -- System Link on real sockets (GZ_NET=1, src/gz_net.c) -----------------
 *
 * XNET library entry points the game calls (stdcall thunks that load the
 * library's context from 0x0046F148), identified from the game's socket
 * wrapper class (sub_0018E8A0..0018EAF0) and its network init (sub_0002D760):
 *   0027BDE9 XNetStartup(1)        0027BE00 WSAStartup(2)
 *   0027BEEE WSACleanup(0)         0027BEFB XNetCleanup(0)
 *   0027BABB XNetGetTitleXnAddr(1) 0027BAC6 (1 arg, called once at init)
 *   0027BA2B XNetCreateKey(2)      0027BA36 XNetRegisterKey(2)
 *   0027BA41 XNetUnregisterKey(1)  0027BA4C XNetXnAddrToInAddr(3)
 *   0027BBC1 XNetInAddrToXnAddr(3) 0027BA85 XNetQosListen(5)
 *   0027BAB0 XNetQosRelease(1)
 *   0027BAD1 socket(3)             0027BADC closesocket(1)
 *   0027BAF2 ioctlsocket(3)        0027BAFD setsockopt(5)
 *   0027BB0C bind(3)               0027BB22 select(5)
 *   0027BB5A recvfrom(6)           0027BB78 sendto(6)
 *   0027BF8B WSAGetLastError(0)    0027BF90 WSASetLastError(1)
 * With GZ_NET off every one of them runs the original library code. */
int      gz_net_on(void);
uint32_t gz_net_local_ip(void);
uint32_t gz_net_socket(uint32_t af, uint32_t type, uint32_t proto);
uint32_t gz_net_closesocket(uint32_t h);
uint32_t gz_net_bind(uint32_t h, const void *addr, uint32_t len);
uint32_t gz_net_ioctlsocket(uint32_t h, uint32_t cmd, uint32_t *arg);
uint32_t gz_net_setsockopt(uint32_t h, uint32_t level, uint32_t name, const void *val, uint32_t len);
uint32_t gz_net_select(uint32_t *r, uint32_t *w, uint32_t *e, const uint32_t *timeout);
uint32_t gz_net_recvfrom(uint32_t h, void *buf, uint32_t len, uint32_t flags, void *from, uint32_t *fromlen);
uint32_t gz_net_sendto(uint32_t h, const void *buf, uint32_t len, uint32_t flags, const void *to, uint32_t tolen);
uint32_t gz_net_get_error(void);
void     gz_net_set_error(uint32_t e);
void     gz_net_fill_xnaddr(uint8_t *xna, uint32_t ip_be);
void     gz_net_random(uint8_t *p, uint32_t n);

extern void sub_0027BDE9_gen(void);
extern void sub_0027BE00_gen(void);
extern void sub_0027BEEE_gen(void);
extern void sub_0027BEFB_gen(void);
extern void sub_0027BABB_gen(void);
extern void sub_0027BAC6_gen(void);
extern void sub_0027BA2B_gen(void);
extern void sub_0027BA36_gen(void);
extern void sub_0027BA41_gen(void);
extern void sub_0027BA4C_gen(void);
extern void sub_0027BBC1_gen(void);
extern void sub_0027BA85_gen(void);
extern void sub_0027BAB0_gen(void);
extern void sub_0027BAD1_gen(void);
extern void sub_0027BADC_gen(void);
extern void sub_0027BAF2_gen(void);
extern void sub_0027BAFD_gen(void);
extern void sub_0027BB0C_gen(void);
extern void sub_0027BB22_gen(void);
extern void sub_0027BB5A_gen(void);
extern void sub_0027BB78_gen(void);
extern void sub_0027BF8B_gen(void);
extern void sub_0027BF90_gen(void);
#define NET_ARG(i)   GZ_MEM32(g_esp + 4u + 4u * (i))
#define NET_PTR(va)  ((va) ? (void *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset) : NULL)
#define NET_RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4u + 4u * (nargs); } while (0)
#define NET_SHIM(addr, nargs, expr) \
    extern void sub_##addr##_gen(void); \
    void sub_##addr(void) \
    { \
        if (!gz_net_on()) { sub_##addr##_gen(); return; } \
        { uint32_t r_ = (uint32_t)(expr); NET_RET(r_, nargs); } \
    }

static uint32_t net_get_xnaddr(uint8_t *xna)
{
    gz_net_fill_xnaddr(xna, gz_net_local_ip());
    return 0x02u | 0x08u;                 /* XNET_GET_XNADDR_ETHERNET | _DHCP */
}

static uint32_t net_create_key(uint8_t *kid, uint8_t *key)
{
    if (kid) gz_net_random(kid, 8);
    if (key) gz_net_random(key, 16);
    return 0;
}

/* The "secure" address of a console is its real one. */
static uint32_t net_xnaddr_to_inaddr(const uint8_t *xna, uint32_t *ina)
{
    if (!xna || !ina) return 0x2726u;     /* WSAEINVAL */
    memcpy(ina, xna, 4);
    return 0;
}

static uint32_t net_inaddr_to_xnaddr(uint32_t ina, uint8_t *xna, uint8_t *kid)
{
    if (xna) gz_net_fill_xnaddr(xna, ina);
    if (kid) memset(kid, 0, 8);
    return 0;
}

NET_SHIM(0027BDE9, 1, 0)
NET_SHIM(0027BE00, 2, (NET_PTR(NET_ARG(1)) ? (memset(NET_PTR(NET_ARG(1)), 0, 0x18E), 0) : 0))
NET_SHIM(0027BEEE, 0, 0)
NET_SHIM(0027BEFB, 0, 0)
NET_SHIM(0027BABB, 1, net_get_xnaddr((uint8_t *)NET_PTR(NET_ARG(0))))
NET_SHIM(0027BAC6, 1, 0)
NET_SHIM(0027BA2B, 2, net_create_key((uint8_t *)NET_PTR(NET_ARG(0)), (uint8_t *)NET_PTR(NET_ARG(1))))
NET_SHIM(0027BA36, 2, 0)
NET_SHIM(0027BA41, 1, 0)
NET_SHIM(0027BA4C, 3, net_xnaddr_to_inaddr((const uint8_t *)NET_PTR(NET_ARG(0)), (uint32_t *)NET_PTR(NET_ARG(2))))
NET_SHIM(0027BBC1, 3, net_inaddr_to_xnaddr(NET_ARG(0), (uint8_t *)NET_PTR(NET_ARG(1)), (uint8_t *)NET_PTR(NET_ARG(2))))
NET_SHIM(0027BA85, 5, 0)
NET_SHIM(0027BAB0, 1, 0)
NET_SHIM(0027BAD1, 3, gz_net_socket(NET_ARG(0), NET_ARG(1), NET_ARG(2)))
NET_SHIM(0027BADC, 1, gz_net_closesocket(NET_ARG(0)))
NET_SHIM(0027BAF2, 3, gz_net_ioctlsocket(NET_ARG(0), NET_ARG(1), (uint32_t *)NET_PTR(NET_ARG(2))))
NET_SHIM(0027BAFD, 5, gz_net_setsockopt(NET_ARG(0), NET_ARG(1), NET_ARG(2), NET_PTR(NET_ARG(3)), NET_ARG(4)))
NET_SHIM(0027BB0C, 3, gz_net_bind(NET_ARG(0), NET_PTR(NET_ARG(1)), NET_ARG(2)))
NET_SHIM(0027BB22, 5, gz_net_select((uint32_t *)NET_PTR(NET_ARG(1)), (uint32_t *)NET_PTR(NET_ARG(2)), (uint32_t *)NET_PTR(NET_ARG(3)), (const uint32_t *)NET_PTR(NET_ARG(4))))
NET_SHIM(0027BB5A, 6, gz_net_recvfrom(NET_ARG(0), NET_PTR(NET_ARG(1)), NET_ARG(2), NET_ARG(3), NET_PTR(NET_ARG(4)), (uint32_t *)NET_PTR(NET_ARG(5))))
NET_SHIM(0027BB78, 6, gz_net_sendto(NET_ARG(0), NET_PTR(NET_ARG(1)), NET_ARG(2), NET_ARG(3), NET_PTR(NET_ARG(4)), NET_ARG(5)))
NET_SHIM(0027BF8B, 0, gz_net_get_error())
NET_SHIM(0027BF90, 1, (gz_net_set_error(NET_ARG(0)), 0))

/* -- Camera (widescreen) --------------------------------------------------
 *
 * The active camera block is at *(0x0041DB58) + 0x2BC: +0x30 field of view,
 * +0x34 zoom, +0x3C aspect ratio (width / height). sub_00057E00 (world to
 * screen) uses tan(fov / 2) across and tan(fov / 2 / aspect) down.
 * GZ_CAM_LOG=1 prints the block once a second. */
static void gz_camera_tick(void)
{
    static int log = -1;
    static DWORD last;
    uint32_t base = GZ_MEM32(0x0041DB58u), cam, k;
    if (log < 0) {
        log = getenv("GZ_CAM_LOG") != NULL;
        if (log) fprintf(stderr, "[CAM] logging on, world %08X\n", base);
    }
    {
        /* GZ_FIND_FLOAT=<hex bits>,<seconds>: that long after the first call, list every
         * guest address below 64 MB holding that 32-bit value, with the six
         * floats around it (finding the fight camera's aspect ratio). */
        static int armed = -1, at;
        static uint32_t want;
        static DWORD t_first;
        static int done;
        if (armed < 0) {
            const char *e = getenv("GZ_FIND_FLOAT");
            armed = e && sscanf(e, "%x,%d", &want, &at) == 2;
        }
        if (armed && !t_first) t_first = GetTickCount();
        if (armed && !done && GetTickCount() - t_first >= (DWORD)at * 1000u) {
            done = 1;
            uint32_t va = 0x10000u;
            int found = 0;
            while (va < 0x04000000u && found < 400) {
                MEMORY_BASIC_INFORMATION mi;
                const uint8_t *host = (const uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
                uint32_t end;
                if (!VirtualQuery(host, &mi, sizeof mi)) break;
                end = (uint32_t)((const uint8_t *)mi.BaseAddress + mi.RegionSize - (const uint8_t *)g_xbox_mem_offset);
                if (end > 0x04000000u || end <= va) end = 0x04000000u;
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                    uint32_t x;
                    for (x = (va + 3u) & ~3u; x + 4u <= end && found < 400; x += 4u) {
                        if (want == 0x50524F4Au) {
                            /* "PROJ": a 4x4 perspective matrix whose y scale is
                             * 4/3 of its x scale (row- or column-major w term). */
                            float m[16];
                            int j, ok;
                            if (x + 64u > end) break;
                            memcpy(m, (const void *)((uintptr_t)x + g_xbox_mem_offset), 64);
                            ok = m[0] > 0.3f && m[0] < 8.0f && m[5] > m[0] * 1.32f && m[5] < m[0] * 1.345f
                                 && m[1] == 0.0f && m[2] == 0.0f && m[4] == 0.0f && m[6] == 0.0f && m[15] == 0.0f
                                 && (m[11] == 1.0f || m[11] == -1.0f || m[14] == 1.0f || m[14] == -1.0f);
                            if (ok) {
                                fprintf(stderr, "[FIND] proj %08X:", x);
                                for (j = 0; j < 16; j++) fprintf(stderr, " %.4g", m[j]);
                                fprintf(stderr, "\n");
                                found++;
                            }
                            continue;
                        }
                        if (GZ_MEM32(x) == want) {
                            int j;
                            fprintf(stderr, "[FIND] %08X:", x);
                            for (j = -6; j <= 6; j++) {
                                uint32_t b = GZ_MEM32(x + 4 * j);
                                float f;
                                memcpy(&f, &b, 4);
                                fprintf(stderr, " %.4g", f);
                            }
                            fprintf(stderr, "\n");
                            found++;
                        }
                    }
                }
                va = end;
            }
            fprintf(stderr, "[FIND] %d hit(s) for %08X\n", found, want);
        }
    }
    if (!base) return;
    cam = base + 0x2BCu;
    if (log && GetTickCount() - last >= 1000u) {
        last = GetTickCount();
        fprintf(stderr, "[CAM] %08X:", cam);
        for (k = 0; k < 0x60u; k += 4) {
            uint32_t b = GZ_MEM32(cam + k);
            float f;
            memcpy(&f, &b, 4);
            fprintf(stderr, " %02X=%.4g", k, f);
        }
        fprintf(stderr, "\n");
    }
}

/* -- Widescreen ------------------------------------------------------------
 *
 * The game's cameras are left exactly as the game sets them (4:3), so
 * everything the game works out from a camera - where a 3D point lands on
 * the HUD, how to frame both monsters - stays consistent. The picture is
 * widened at two points only:
 *
 *  - sub_000A6820(this = renderer 0x004322D0, type, matrix), the renderer's
 *    set-transform (type 2 = projection): a perspective projection at 4:3
 *    gets its x scale x 3/4, in a copy below the stack. A third more is
 *    drawn on each side into the 640x480 frame the back end shows at 16:9
 *    (and the back end squeezes 2D by the same 3/4, so it keeps its shape
 *    and its place over the 3D scene).
 *
 *  - sub_00030840(this = frustum, camera, ...) (ret 12) and sub_00031470(this
 *    = frustum, camera) (ret 4) build the planes objects are culled against
 *    from the camera: position, axes and the projection scales +0x130
 *    (across) and +0x134 (up; across = up x aspect, aspect +0x148 = 3/4).
 *    While they run, a 4:3 camera's across scale is x 3/4 as well, so the
 *    frustum matches what is drawn.
 *
 * sub_0001F740(this = camera, fov, a2, a3, aspect) is the camera's
 * set-perspective (ret 16; aspect is height / width, 0 = "from the
 * viewport"). An earlier version widened the camera there instead. The game
 * hands a camera's stored aspect back to it, that code took the widened value
 * for a fresh one and undid it, and fights ended up with a 4:3 camera - 4:3
 * culling - under a 16:9 picture: scenery vanished in the outer sixths of the
 * screen. Menu cameras were never widened, which is how the main menu's
 * Godzilla head disappeared as the camera swung to the monster select.
 * GZ_WIDE_CULL=0: no frustum change (to compare). */
extern void sub_0001F740_gen(void);

/* A fight is on screen when a match (heap) camera was set up more recently
 * than any shell (static) camera. The match camera is only set up during
 * the intro, so this must not expire with time. The back end spreads the
 * HUD to the edges only then. */
static volatile LONG gz_cam_seq, gz_fight_cam_seq, gz_shell_cam_seq;
int gz_wide_fight_active(void)
{
    return gz_fight_cam_seq > gz_shell_cam_seq;
}

void sub_0001F740(void)
{
    if (g_ecx >= 0x01000000u) { extern void gz_rb_fight_camera(void); gz_rb_fight_camera(); }
    if (g_ecx >= 0x01000000u) gz_fight_cam_seq = ++gz_cam_seq;
    else gz_shell_cam_seq = ++gz_cam_seq;
    if (gz_opt_wide()) {
        static int nlog;
        if (getenv("GZ_WIDE_LOG") && nlog++ < 40) {
            uint32_t fb = GZ_MEM32(g_esp + 4), ab = GZ_MEM32(g_esp + 16);
            float fov, aspect;
            memcpy(&fov, &fb, 4); memcpy(&aspect, &ab, 4);
            fprintf(stderr, "[WIDE] camera %08X fov %.4g aspect arg %.4g viewport %ux%u\n", g_ecx, fov, aspect,
                    GZ_MEM32(g_ecx + 0x160), GZ_MEM32(g_ecx + 0x164));
        }
    }
    sub_0001F740_gen();
}

extern void sub_00030840_gen(void);
extern void sub_00031470_gen(void);
static void gz_frustum_build(void (*gen)(void))
{
    static int on = -1, nlog;
    uint32_t cam = GZ_MEM32(g_esp + 4), old = 0;
    int widen = 0;
    if (on < 0) { const char *e = getenv("GZ_WIDE_CULL"); on = !(e && *e == '0'); }
    if (on && cam >= 0x00010000u && gz_opt_wide()) {
        uint32_t vb = GZ_MEM32(cam + 0x134u);
        float h, v, r;
        old = GZ_MEM32(cam + 0x130u);
        memcpy(&h, &old, 4); memcpy(&v, &vb, 4);
        r = v != 0.0f ? h / v : 0.0f;
        /* the cameras whose projection sub_000A6820 widens: 4:3 ones */
        widen = r > 0.743f && r < 0.758f;
        if (nlog < 16 && getenv("GZ_WIDE_LOG")) {
            static uint32_t seen[16];
            int q, known = 0;
            for (q = 0; q < nlog; q++) if (seen[q] == (cam ^ (uint32_t)widen)) known = 1;
            if (!known) {
                seen[nlog++] = cam ^ (uint32_t)widen;
                fprintf(stderr, "[WIDE] frustum from camera %08X: across %.4g up %.4g (aspect %.4f) -> %s\n",
                        cam, h, v, r, widen ? "built 4/3 wider" : "as it is");
            }
        }
        if (widen) {
            uint32_t hb;
            h *= 0.75f;
            memcpy(&hb, &h, 4);
            GZ_MEM32(cam + 0x130u) = hb;
        }
    }
    gen();
    if (widen) GZ_MEM32(cam + 0x130u) = old;
}
void sub_00030840(void) { gz_frustum_build(sub_00030840_gen); }
void sub_00031470(void) { gz_frustum_build(sub_00031470_gen); }

extern void sub_000A6820_gen(void);

void sub_000A6820(void)
{
    uint32_t type = GZ_MEM32(g_esp + 4), src = GZ_MEM32(g_esp + 8);
    if (type == 2 && src && gz_opt_wide()) {
        float m[16], r;
        memcpy(m, (const void *)((uintptr_t)src + g_xbox_mem_offset), sizeof m);
        r = m[0] != 0.0f ? m[5] / m[0] : 0.0f;
        {
            static int nlog;
            if (getenv("GZ_WIDE_LOG") && nlog++ < 60)
                fprintf(stderr, "[WIDE] projection from %08X: x %.4g y %.4g m11 %.3g m15 %.3g ratio %.4g\n",
                        GZ_MEM32(g_esp), m[0], m[5], m[11], m[15], r);
        }
        /* only a perspective projection still at 4:3 (y scale / x scale) */
        if (m[15] == 0.0f && (m[11] == 1.0f || m[11] == -1.0f) && r > 1.32f && r < 1.345f) {
            uint32_t copy = g_esp - 0x100u;
            m[0] *= 0.75f;
            memcpy((void *)((uintptr_t)copy + g_xbox_mem_offset), m, sizeof m);
            GZ_MEM32(g_esp + 8) = copy;
            sub_000A6820_gen();
            return;
        }
    }
    sub_000A6820_gen();
}

/* -- Pad input override (rollback netplay, gz_rollback.c) -------------------
 *
 * The game polls every pad once a frame in sub_0001ECB0: a loop over the four
 * ports (index in ebx) that calls XInputGetState, sub_002D8D7E(hDevice,
 * pState) (ret 8), at 0x0001EE18. XINPUT_STATE is a packet number and the 18
 * byte pad (buttons, 8 analog buttons, 4 stick axes). For two machines to run
 * the same match tick for tick, a tick's input has to arrive exactly here,
 * not through the emulated USB port whose timing varies: gz_rb_pad() may
 * replace what the port read. */
extern void sub_002D8D7E_gen(void);
extern GZ_TLS uint32_t g_ebx;
int gz_rb_pad(int port, uint8_t *pad18);

/* -- Controllers in a rollback netplay session (gz_rbnet.c) ----------------
 *
 * The emulated USB controllers run on the host's clock: a pad arrives, and
 * its reports come, at moments that differ between two PCs. A session plays
 * the menus as well as the fight on both PCs from the same inputs, so there
 * the game's controller API answers for the gamepad type directly: two pads,
 * ports 1 and 2, present from the first call; their state is the session's
 * input for the tick (gz_rb_pad). The XDK functions (stdcall; the poller
 * sub_0001ECB0 and the setup sub_0001EBA0 call them):
 *   sub_002D8E87  XGetDevices(type)                         ret 4
 *   sub_002D8EA9  XGetDeviceChanges(type, *ins, *rem)        ret 12
 *   sub_002D8B44  XInputOpen(type, port, slot, params)       ret 16
 *   sub_002D8B9A  XInputClose(handle)                        ret 4
 *   sub_002D8BA6  XInputGetCapabilities(handle, *caps)       ret 8
 *   sub_002D8D7E  XInputGetState(handle, *state)             ret 8
 *   sub_002D8DF1  XInputSetState(handle, *feedback)          ret 8 (rumble)
 * Other device types (memory units, voice) go to the originals, which find
 * none: a session runs without the USB model (main.c). */
#define GZ_XDEV_GAMEPAD  0x002D7ABCu            /* XDEVICE_TYPE_GAMEPAD */
#define GZ_FAKE_PAD(p)   (0x47A50000u | (uint32_t)(p))
#define GZ_IS_FAKE_PAD(h) (((h) & 0xFFFFFFF0u) == 0x47A50000u)
int gz_rbn_session(void);
extern void sub_002D8E87_gen(void);
extern void sub_002D8EA9_gen(void);
extern void sub_002D8B44_gen(void);
extern void sub_002D8B9A_gen(void);
extern void sub_002D8BA6_gen(void);
extern void sub_002D8DF1_gen(void);
static int gz_pads_reported;                    /* XGetDevices or the first change said "2 pads" */

static void gz_stdcall_ret(uint32_t eax, unsigned args)
{
    g_eax = eax;
    g_esp += 4u + 4u * args;
}

void sub_002D8E87(void)
{
    if (gz_rbn_session() && GZ_MEM32(g_esp + 4) == GZ_XDEV_GAMEPAD) {
        gz_pads_reported = 1;
        gz_stdcall_ret(0x3u, 1);
        return;
    }
    sub_002D8E87_gen();
}

void sub_002D8EA9(void)
{
    if (gz_rbn_session() && GZ_MEM32(g_esp + 4) == GZ_XDEV_GAMEPAD) {
        uint32_t ins = GZ_MEM32(g_esp + 8), rem = GZ_MEM32(g_esp + 12);
        uint32_t now = gz_pads_reported ? 0u : 0x3u;
        gz_pads_reported = 1;
        if (ins) GZ_MEM32(ins) = now;
        if (rem) GZ_MEM32(rem) = 0;
        gz_stdcall_ret(now ? 1u : 0u, 3);
        return;
    }
    sub_002D8EA9_gen();
}

void sub_002D8B44(void)
{
    if (gz_rbn_session() && GZ_MEM32(g_esp + 4) == GZ_XDEV_GAMEPAD) {
        uint32_t port = GZ_MEM32(g_esp + 8);
        gz_stdcall_ret(port < 2u ? GZ_FAKE_PAD(port) : 0u, 4);
        return;
    }
    sub_002D8B44_gen();
}

void sub_002D8B9A(void)
{
    if (GZ_IS_FAKE_PAD(GZ_MEM32(g_esp + 4))) { gz_stdcall_ret(0, 1); return; }
    sub_002D8B9A_gen();
}

void sub_002D8BA6(void)
{
    uint32_t h = GZ_MEM32(g_esp + 4), caps = GZ_MEM32(g_esp + 8);
    if (GZ_IS_FAKE_PAD(h)) {
        /* XINPUT_CAPABILITIES: SubType (gamepad), then every button, axis
         * and motor supported */
        if (caps) {
            volatile uint8_t *c = (volatile uint8_t *)((uintptr_t)caps + g_xbox_mem_offset);
            int k;
            c[0] = 1; c[1] = 0; c[2] = 0; c[3] = 0;
            for (k = 4; k < 26; k++) c[k] = 0xFF;
        }
        gz_stdcall_ret(0, 2);
        return;
    }
    sub_002D8BA6_gen();
}

void sub_002D8DF1(void)
{
    uint32_t h = GZ_MEM32(g_esp + 4), fb = GZ_MEM32(g_esp + 8);
    if (GZ_IS_FAKE_PAD(h)) {
        if (fb) GZ_MEM32(fb) = 0;               /* the header: done, ERROR_SUCCESS */
        gz_stdcall_ret(0, 2);
        return;
    }
    sub_002D8DF1_gen();
}

/* The session's pad for a port, as XINPUT_STATE. The packet number is the
 * tick (rollback rewinds the game, not a counter kept here, and both PCs
 * must hand the game the same numbers). */
static void gz_fill_state(uint32_t state, int port)
{
    extern uint32_t gz_rb_cur_tick(void);
    extern int gz_rb_active(void);
    volatile uint8_t *d = (volatile uint8_t *)((uintptr_t)state + g_xbox_mem_offset);
    uint8_t pad[18];
    int k;
    if (!gz_rb_pad(port, pad)) memset(pad, 0, sizeof pad);
    GZ_MEM32(state) = gz_rb_active() ? gz_rb_cur_tick() + 1u : 0u;
    for (k = 0; k < 18; k++) d[4 + k] = pad[k];
}

void sub_002D8D7E(void)
{
    uint32_t ra = GZ_MEM32(g_esp), h = GZ_MEM32(g_esp + 4), state = GZ_MEM32(g_esp + 8), port = g_ebx;
    if (GZ_IS_FAKE_PAD(h)) {                    /* a session's pad */
        if (state) gz_fill_state(state, (int)(h & 0xFu));
        gz_stdcall_ret(0, 2);
        return;
    }
    sub_002D8D7E_gen();
    if (ra == 0x0001EE1Du && port < 4u && state) {
        static uint8_t last[4][18];
        static uint32_t packet[4];
        uint8_t pad[18];
        if (gz_rb_pad((int)port, pad)) {
            volatile uint8_t *d = (volatile uint8_t *)((uintptr_t)state + g_xbox_mem_offset);
            int k;
            if (memcmp(last[port], pad, 18)) { memcpy(last[port], pad, 18); packet[port]++; }
            GZ_MEM32(state) = packet[port];
            for (k = 0; k < 18; k++) d[4 + k] = pad[k];
            g_eax = 0;                              /* ERROR_SUCCESS: connected */
        }
    }
}

/* -- System Link timing -----------------------------------------------------
 *
 * A System Link match ticks once per frame in lockstep: the console runs it at
 * 30 frames a second (the game asks for frame interval 2) at the 60 Hz base,
 * and both machines must step the same way. The frame rate setting, the
 * precise pacing and the "no half-rate" override below all change that, and a
 * network match then ran fast on both PCs and desynced. While the game has a
 * System Link socket open (its System Link screens and the match), all three
 * step aside and the game times itself as on the console. */
int gz_net_active(void);
/* online_60fps (default on, gz_settings.c): network matches at 60 frames a
 * second. The match ticks by twice the base period (0x00442634): at 60 frames
 * with the base period halved, a tick is 1/60 s and the match runs at real
 * speed in lockstep. A PC ticking at 30 and one at 60 would desync, so 60
 * changes the build string the lobby hashes into its version number
 * ("Official Build (4.670)" at 0x00306A28, see DNetLobby, 0x000B4B07): the
 * two then see "the version of the game does not match" instead. */
int gz_opt_net60(void);
static int gz_net60(void)
{
    static int on = -1;
    if (on < 0) {
        on = gz_opt_net60();
        if (on) {
            volatile char *v = (volatile char *)((uintptr_t)0x00306A28u + g_xbox_mem_offset);
            if (v[16] == '4' && v[20] == '0') v[20] = '6';     /* 4.670 -> 4.676 */
            fprintf(stderr, "[RATE] network matches at 60 frames a second (online_60fps)\n");
        }
    }
    return on;
}

static int gz_net_session(void)
{
    static int was = -1;
    int now = gz_net_active();
    (void)gz_net60();                         /* the version string, before any lobby */
    if (now != was) {
        if (was >= 0 || now)
            fprintf(stderr, "[RATE] System Link session %s\n",
                    now ? (gz_net60() ? "started: 60 frames a second, 1/60 s ticks" : "started: the game's own frame timing (30 frames a second)")
                        : "ended: frame rate setting again");
        was = now;
    }
    return now;
}

/* -- No half-rate governor ------------------------------------------------
 *
 * The render function (sub_000B3A50) times each frame's GPU work and, when
 * frames run long, calls sub_000B2A00(this, 2): frame interval 2, i.e. the
 * frame period at 0x00400F2C becomes twice the base period at 0x00442634
 * (60 -> 30 fps on the console, where a missed vblank costs a whole frame).
 * Without vsync that only throws frames away: at GZ_RATE=144 it kept dropping
 * fights to a 72 fps target. The game steps by measured time, so a long frame
 * is simply a longer step. The interval stays 1 here; GZ_HALF_RATE=1 gives
 * the game's own behaviour back. */
extern void sub_000B2A00_gen(void);

void sub_000B2A00(void)
{
    static int half = -1;
    if (half < 0) half = getenv("GZ_HALF_RATE") && getenv("GZ_HALF_RATE")[0] == '1';
    if (!half && (!gz_net_session() || gz_net60())) GZ_MEM32(g_esp + 4) = 1;
    sub_000B2A00_gen();
}

/* ── The frame limiter's sleep (sub_00012240) ───────────────────────────
 *
 * A method (reached only through a vtable) taking a float of seconds: if
 * this+0x50 bit 0x20 is clear and the time exceeds 10 us it Sleeps
 * (seconds - 1e-5) * 1000 ms through sub_0017DF50 and returns the time
 * actually slept, in seconds, on the x87 stack; otherwise it returns 0.
 * The game calls it with the remainder of its frame budget.
 *
 * GZ_SLEEP_LOG=1: once a second, the requests, the time really slept and
 * the caller. GZ_UNCAP=1: never sleep (returns 0, as the original does for
 * a request under 10 us), so frames run as fast as they render. */
extern void sub_00012240_gen(void);
extern GZ_TLS double g_fp_stack[8];

/* GZ_PACE_LOG: a watchdog thread that, when a frame has run 7 ms past its
 * start, logs the guest return addresses on the game thread's stack: what
 * the game was doing in a slow frame. */
static volatile LONGLONG gz_slow_frame_start;      /* QPC ticks; 0 = not armed */
static uint32_t *volatile gz_slow_esp;
static DWORD WINAPI gz_slow_thread(LPVOID arg)
{
    LARGE_INTEGER f, q;
    LONGLONG reported = 0;
    int shown = 0;
    (void)arg;
    QueryPerformanceFrequency(&f);
    for (;;) {
        LONGLONG st = gz_slow_frame_start;
        Sleep(1);
        if (!st || st == reported || !gz_slow_esp || shown > 400) continue;
        QueryPerformanceCounter(&q);
        if ((q.QuadPart - st) * 1000 / f.QuadPart >= 7 && st == gz_slow_frame_start) {
            uint32_t esp = *gz_slow_esp, k, n = 0;
            reported = st; shown++;
            fprintf(stderr, "[SLOW] +%lld ms esp %08X:", (long long)((q.QuadPart - st) * 1000 / f.QuadPart), esp);
            for (k = 0; k < 400 && n < 14; k++) {
                uint32_t v = GZ_MEM32(esp + k * 4);
                if (v >= 0x00011000u && v < 0x002E0000u) { fprintf(stderr, " %08X", v); n++; }
            }
            fprintf(stderr, "\n");
        }
    }
}
static void gz_slow_watch(double now)
{
    static int started;
    LARGE_INTEGER q;
    (void)now;
    if (!started) {
        started = 1;
        gz_slow_esp = &g_esp;
        CreateThread(NULL, 0, gz_slow_thread, NULL, 0, NULL);
    }
    QueryPerformanceCounter(&q);
    gz_slow_frame_start = q.QuadPart;
}
extern GZ_TLS int g_fp_top;

/* Turbo (Options > Game speed): frames stay one period apart in real time,
 * but the world -- and the fight clock, which counts frames x the period at
 * 0x00442634 -- moves 1.2 periods per frame: the pace players liked from the
 * old speed bug. Never in System Link, where both sides must keep the
 * console's timing. GZ_TURBO_SCALE=<x> tries another factor. */
static double gz_turbo_scale(void)
{
    static double scale;
    if (!gz_opt_turbo() || gz_net_session() || gz_rb_deterministic()) return 1.0;  /* never in netplay */
    if (scale <= 0.0) {
        const char *e = getenv("GZ_TURBO_SCALE");
        scale = (e && atof(e) >= 0.5 && atof(e) <= 3.0) ? atof(e) : 1.2;
    }
    return scale;
}

void sub_00012240(void)
{
    static int uncap = -1, log = -1;
    uint32_t bits = GZ_MEM32(g_esp + 4), ra = GZ_MEM32(g_esp);
    float req;

    if (uncap < 0) {
        uncap = getenv("GZ_UNCAP") && getenv("GZ_UNCAP")[0] == '1';
        log = getenv("GZ_SLEEP_LOG") != NULL;
    }
    memcpy(&req, &bits, 4);
    gz_quick_fight_tick();
    gz_ko_tick();
    gz_tournament_tick();
    gz_opt_tick();
    gz_camera_tick();
    gz_biollante_slot_tick();
    gz_movie_tick();
    gz_movie_mute_tick();
    gz_ach_frame();
    gz_pad_activity_tick();
    if (ra == 0x0001D802u) gz_rb_tick();     /* gz_rollback.c: may put the game back a few ticks */
    {
        /* GZ_RATE=<n>: the game derives its frame period as 1 / (unsigned)
         * MEM32(0x4323A0) (sub_00012590). Overriding that rate is how to run
         * more frames per second at the game's own pace, if its logic steps
         * by that period. */
        int rate = gz_net_session() ? 60 : gz_opt_rate();   /* Options > frame rate, or GZ_RATE */
        if (rate > 0) {
            /* The game turns the rate at 0x004323A0 into frame periods only
             * now and then, so a rate changed in the menu is written to them
             * here: the main loop object's own period (+0x44, when this is
             * its call), the base period at 0x00442634 and period x frame
             * interval at 0x00400F2C (both only once a fight has set them). */
            static int logged_rate;
            float want = 1.0f / (float)rate, v, wstep = want * (float)gz_turbo_scale();
            uint32_t wb, sb, vb;
            int k;
            /* 0x00400F2C (the step) is set by the pacing below from real time */
            static const uint32_t globals[1] = { 0x00442634u };
            memcpy(&wb, &want, 4);
            memcpy(&sb, &wstep, 4);
            GZ_MEM32(0x4323A0) = (uint32_t)rate;
            if (ra == 0x0001D802u && g_ecx) GZ_MEM32(g_ecx + 0x44) = wb;   /* the limiter's real-time period */
            for (k = 0; k < 1; k++) {
                vb = GZ_MEM32(globals[k]);
                memcpy(&v, &vb, 4);
                if (v > 0.002f && v < 0.2f) GZ_MEM32(globals[k]) = sb;     /* the world's step (turbo: longer) */
            }
            if (logged_rate != rate) {
                logged_rate = rate;
                fprintf(stderr, "[RATE] %d fps, frame period %.2f ms\n", rate, want * 1000.0f);
            }
        }
    }
    if (uncap) {
        g_fp_top = (g_fp_top + 7) & 7;
        g_fp_stack[g_fp_top] = 0.0;
        g_esp += 8;                                 /* ret 4 */
        return;
    }
    {
        /* Precise pacing for the main loop's call (return address 0x0001D802).
         * The original sleeps the requested remainder in whole milliseconds
         * and always lands late (asked 1.9 ms, slept 2.3), and the loop has
         * about half a millisecond outside what its stopwatch measures; at
         * 144 Hz that is 15% of the frame. Instead: wake exactly one frame
         * period after the previous wake - sleep for the bulk,
         * spin the last stretch. A frame that is already late is not delayed
         * and restarts the schedule. GZ_PACE=0 keeps the original sleep. */
        static int pace = -1;
        static LARGE_INTEGER qf;
        static double last;
        if (pace < 0) {
            const char *e = getenv("GZ_PACE");
            pace = !(e && e[0] == '0');
            QueryPerformanceFrequency(&qf);
        }
        if (pace && ra == 0x0001D802u && (!gz_net_session() || gz_net60())) {
            LARGE_INTEGER q;
            float period = 1.0f / (float)(gz_opt_rate() > 0 ? gz_opt_rate() : 60);
            double now, start, target;
            if (period > 0.002f && period < 0.2f) {
                QueryPerformanceCounter(&q);
                start = now = (double)q.QuadPart / (double)qf.QuadPart;
                target = last + (double)period;
                if (gz_rb_replaying()) {
                    /* a rollback's replay (gz_rollback.c): no wait */
                    last = now;
                } else if (now >= target || target - now > 2.0 * (double)period) {
                    /* late, or no schedule yet: no wait */
                    last = (now - target < (double)period) ? target : now;
                    if (target - now > 2.0 * (double)period) last = now;
                } else {
                    for (;;) {
                        double left = target - now;
                        if (left <= 0.0) break;
                        if (left > 0.0022) Sleep((DWORD)((left - 0.0012) * 1000.0));
                        else if (left > 0.0003) SwitchToThread();
                        else YieldProcessor();
                        QueryPerformanceCounter(&q);
                        now = (double)q.QuadPart / (double)qf.QuadPart;
                    }
                    last = target;
                }
                {
                    /* The game advances its world by the step at 0x00400F2C each
                     * frame (normally the period, or 2 periods when it falls
                     * behind); see below for why it stays the period. The
                     * frame's real time (step, from one wake to the next) is
                     * only for the pacing log. */
                    static double prev_wake;
                    double step = prev_wake > 0.0 ? now - prev_wake : (double)period;
                    float f;
                    uint32_t fb;
                    prev_wake = now;
                    {
                        /* GZ_PACE_LOG=1: once a second, how the pacing went. */
                        static int plog = -1;
                        static double t0, worst;
                        static unsigned frames, late, waited;
                        if (plog < 0) plog = getenv("GZ_PACE_LOG") != NULL;
                        if (plog) gz_slow_watch(now);
                        if (plog) {
                            extern volatile LONG gz_stat_decode_us, gz_stat_decode_n, gz_stat_decode_px;
                            LONG du = InterlockedExchange(&gz_stat_decode_us, 0), dn = InterlockedExchange(&gz_stat_decode_n, 0);
                            LONG dp = InterlockedExchange(&gz_stat_decode_px, 0);
                            double fw = gz_fence_wait_s; gz_fence_wait_s = 0.0;
                            static double fw_sec, slept_sec;
                            fw_sec += fw; slept_sec += now - start;
                            if (step > 0.010)
                                fprintf(stderr, "[HITCH] %.1f ms: fence waits %.1f ms (longest %.1f from %08X %08X %08X %08X), texture decodes %ld (%ld px) %.1f ms\n",
                                        step * 1000.0, fw * 1000.0, gz_fence_wait_max * 1000.0, gz_fence_wait_ra[0], gz_fence_wait_ra[1],
                                        gz_fence_wait_ra[2], gz_fence_wait_ra[3], (long)dn, (long)dp, du / 1000.0);
                            gz_fence_wait_max = 0.0;
                            frames++;
                            if (now - start > 0.00005) waited++; else late++;
                            if (step > worst) worst = step;
                            if (now - t0 >= 1.0) {
                                fprintf(stderr, "[PACE] %u frames, %u waited, %u late, longest %.2f ms, waiting on GPU thread %.0f ms, idle in limiter %.0f ms\n", frames, waited, late, worst * 1000.0, fw_sec * 1000.0, slept_sec * 1000.0);
                                fw_sec = slept_sec = 0.0;
                                t0 = now; frames = late = waited = 0; worst = 0.0;
                            }
                        }
                    }
                    /* The step stays one period. The fight clock is not a sum
                     * of steps: sub_000C4150 sets it to (frames + c) x the
                     * period at 0x00442634, every frame. A step of the
                     * frame's real time (at least a period) used to be
                     * written here, and every late frame then rescaled the
                     * whole fight so far: on a PC holding ~105 of 120 fps
                     * the timer ran 1.23x real time, 1.31x at 144, and
                     * jumped about. With the period the game keeps real time
                     * at a rate the PC holds (60: 1.0x); one it cannot hold
                     * plays slower, as the console did when it dropped frames
                     * (the same PC at ~105-115 fps: 0.98x at 120, 0.85x at
                     * 144). */
                    step = (double)period * gz_turbo_scale();
                    /* A 60 fps network match: fixed values, never measured ones.
                     * The match ticks by twice the base period, and both machines
                     * must tick by exactly the same amount every frame. */
                    if ((gz_net60() && gz_net_active()) || gz_rb_deterministic()) step = (double)period;
                    f = (float)step;
                    memcpy(&fb, &f, 4);
                    GZ_MEM32(0x00400F2Cu) = fb;
                    if (gz_net60() && gz_net_active()) {
                        /* experiment: the network tick as twice the base period */
                        float h = f * 0.5f;
                        uint32_t hb;
                        memcpy(&hb, &h, 4);
                        GZ_MEM32(0x00442634u) = hb;
                    } else
                        GZ_MEM32(0x00442634u) = fb;   /* the render function copies this into the step */
                }
                g_fp_top = (g_fp_top + 7) & 7;
                g_fp_stack[g_fp_top] = now - start;     /* time slept, seconds */
                g_esp += 8;                             /* ret 4 */
                return;
            }
        }
    }
    if (log) {
        static LARGE_INTEGER f, t0;
        static double req_sum, got_sum, req_max, el_sum;
        static uint32_t n, last_ra;
        static float period;
        LARGE_INTEGER a, b;
        uint32_t app = g_ecx;
        if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
        QueryPerformanceCounter(&a);
        {
            /* The caller's object: frame period at +0x44, its stopwatch at
             * +0x18 (start count, 64-bit, at +0x38). */
            uint32_t pb = GZ_MEM32(app + 0x44);
            uint64_t start = (uint64_t)GZ_MEM32(app + 0x38) | ((uint64_t)GZ_MEM32(app + 0x3C) << 32);
            memcpy(&period, &pb, 4);
            el_sum += (double)((int64_t)a.QuadPart - (int64_t)start) / (double)f.QuadPart;
        }
        sub_00012240_gen();
        QueryPerformanceCounter(&b);
        n++;
        req_sum += req;
        if (req > req_max) req_max = req;
        got_sum += (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
        last_ra = ra;
        if ((double)(b.QuadPart - t0.QuadPart) / (double)f.QuadPart >= 1.0) {
            fprintf(stderr, "[SLEEP] %u calls/s, asked %.1f ms avg (max %.1f), slept %.1f ms avg, caller %08X, rate[0x4323A0]=%u, period %.2f ms, since stopwatch start %.1f ms avg\n",
                    n, n ? req_sum * 1000.0 / n : 0.0, req_max * 1000.0,
                    n ? got_sum * 1000.0 / n : 0.0, last_ra, GZ_MEM32(0x4323A0),
                    period * 1000.0, n ? el_sum * 1000.0 / n : 0.0);
            n = 0; req_sum = got_sum = req_max = el_sum = 0.0;
            t0 = b;
        }
        return;
    }
    sub_00012240_gen();
}
