/*
 * gz_rollback.c - groundwork for rollback netcode: state history by undo log.
 *
 * Rollback needs the game put back a few ticks and run again to exactly the
 * same result. A tick boundary is the frame limiter call from the main loop
 * (sub_00012240, return address 0x0001D802): the host stack is the same there
 * every frame, so guest memory + the main thread's guest registers + a little
 * runtime state, restored at that point, is the whole game.
 *
 * History is kept as an undo log. Guest memory (the low 64 MB window, the
 * "contiguous" window at 0x80000000 with the pushbuffer / vertex buffers /
 * textures, and the graphics chip's register block) is write-protected; the
 * first write to a page in a tick faults into rb_veh(), which saves the page's
 * old contents to the current tick's log and unprotects it. At the next
 * boundary the touched pages are protected again. Going back n ticks replays
 * the logs backwards: the cost follows the pages touched (about 100 a tick in
 * a fight), not the 128 MB.
 *
 * Besides memory, a tick's record holds: the main thread's guest registers,
 * the virtual clock's tick (the game reads the time stamp counter into its
 * state; while history is on, the main thread's clock is a function of the
 * tick number), and the runtime's heap tables (xbox_HeapStateSave). The
 * graphics chip's poll thread is parked, caught up, at every boundary.
 *
 * Not covered yet: other guest threads (their stacks are in the windows and
 * do get put back), the sound chip, kernel objects, the back end's render
 * state (a restore can leave the picture's fog / blend stale until the game
 * sets it again).
 *
 *   GZ_RB_TEST=<seconds>  sync test, that long after the quick fight launches:
 *                         run GZ_RB_TICKS ticks (default 10), take a picture,
 *                         roll back, run them again, take a picture
 *                         (<GZ_SHOT_FILE>_r<round>_run<1|2>.bmp); GZ_RB_ROUNDS
 *                         times (default 3). The two pictures should match.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gen/recomp_types.h"

/* toolkit (xbox_memory_layout.c, kernel_hal.c) */
size_t xbox_HeapStateSize(void);
size_t xbox_HeapStateSave(void *buf);
void xbox_HeapStateLoad(const void *buf);
void *xbox_Nv2aRegs(uint32_t *size);
int  xbox_ThreadStacks(uint32_t *bases, int max, uint32_t *size);
void xbox_OhciPause(int on);
void xbox_DispatchGateEnter(void);   /* kernel_hal.c: no DPC or interrupt handler runs while held */
void xbox_DispatchGateLeave(void);
int  xbox_Nv2aPause(int want_idle);
void xbox_Nv2aResume(void);
void xbox_VirtualTscSet(uint64_t value);
void xbox_VirtualTscOff(void);
uint64_t xbox_ReadTimeStampCounter(void);
#define RB_TSC_HZ 733333333ull

#define RB_PAGE   4096u
#define RB_WIN    (64u * 1024u * 1024u)
#define RB_WPAGES (RB_WIN / RB_PAGE)            /* pages per guest window */
#define RB_GPAGES (16u * 1024u * 1024u / RB_PAGE)
#define RB_NPAGES (2u * RB_WPAGES + RB_GPAGES)  /* low window, contiguous window, GPU registers */

typedef struct {
    uint32_t eax, ecx, edx, esp, ebx, esi, edi, ebp, fs_base, seh_ebp, eflags, mxcsr;
    int df, fp_top, fp_cmp;
    uint16_t fp_cw, fp_cc;
    double fp[8];
    RecompXmm xmm[8];
} RbRegs;

/* One tick's record. Its pages follow each other in the arena from `first`. */
typedef struct {
    uint32_t tick;
    uint32_t vtick;
    RbRegs   regs;
    uint64_t heap;              /* arena offset of the heap tables at this boundary */
    uint64_t first;             /* arena offset of the first page entry */
    uint32_t npages;
} RbFrame;

#define RB_ENTRY  (8u + RB_PAGE)                /* page index (padded) + old contents */
#define RB_FRAMES 1024u
#define RB_ARENA  (1ull << 30)                  /* reserved; committed as it fills */

static uint8_t   *s_base[3];                    /* host address of each window */
static uint8_t    s_tracked[RB_NPAGES], s_dirty[RB_NPAGES];
static RbFrame    s_frames[RB_FRAMES];
static uint32_t   s_cur;                        /* tick being logged */
static uint32_t   s_oldest;                     /* oldest tick that can be gone back to */
static uint8_t   *s_arena;
static uint64_t   s_arena_used, s_arena_committed;
static size_t     s_heap_max;
static SRWLOCK    s_lock = SRWLOCK_INIT;
static volatile LONG s_tracking;
static uint64_t   s_vbase;
static uint32_t   s_vtick;
static uint64_t   s_stat_faults, s_stat_pages_max;

int gz_rb_active(void) { return s_tracking; }

static uint8_t *page_ptr(uint32_t i)
{
    if (i < RB_WPAGES) return s_base[0] + (size_t)i * RB_PAGE;
    if (i < 2u * RB_WPAGES) return s_base[1] + (size_t)(i - RB_WPAGES) * RB_PAGE;
    return s_base[2] + (size_t)(i - 2u * RB_WPAGES) * RB_PAGE;
}

static int page_of(const uint8_t *p)
{
    if (p >= s_base[0] && p < s_base[0] + RB_WIN) return (int)((p - s_base[0]) / RB_PAGE);
    if (p >= s_base[1] && p < s_base[1] + RB_WIN) return (int)(RB_WPAGES + (p - s_base[1]) / RB_PAGE);
    if (s_base[2] && p >= s_base[2] && p < s_base[2] + (size_t)RB_GPAGES * RB_PAGE)
        return (int)(2u * RB_WPAGES + (p - s_base[2]) / RB_PAGE);
    return -1;
}

static void regs_save(RbRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->esp = g_esp;
    r->ebx = g_ebx; r->esi = g_esi; r->edi = g_edi; r->ebp = g_ebp;
    r->fs_base = g_fs_base; r->seh_ebp = g_seh_ebp; r->eflags = g_eflags; r->mxcsr = g_mxcsr;
    r->df = g_df; r->fp_top = g_fp_top; r->fp_cmp = g_fp_cmp;
    r->fp_cw = g_fp_control_word; r->fp_cc = g_fp_cc;
    memcpy(r->fp, g_fp_stack, sizeof r->fp);
    r->xmm[0] = g_xmm0; r->xmm[1] = g_xmm1; r->xmm[2] = g_xmm2; r->xmm[3] = g_xmm3;
    r->xmm[4] = g_xmm4; r->xmm[5] = g_xmm5; r->xmm[6] = g_xmm6; r->xmm[7] = g_xmm7;
}

static void regs_load(const RbRegs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_esp = r->esp;
    g_ebx = r->ebx; g_esi = r->esi; g_edi = r->edi; g_ebp = r->ebp;
    g_fs_base = r->fs_base; g_seh_ebp = r->seh_ebp; g_eflags = r->eflags; g_mxcsr = r->mxcsr;
    g_df = r->df; g_fp_top = r->fp_top; g_fp_cmp = r->fp_cmp;
    g_fp_control_word = r->fp_cw; g_fp_cc = r->fp_cc;
    memcpy(g_fp_stack, r->fp, sizeof r->fp);
    g_xmm0 = r->xmm[0]; g_xmm1 = r->xmm[1]; g_xmm2 = r->xmm[2]; g_xmm3 = r->xmm[3];
    g_xmm4 = r->xmm[4]; g_xmm5 = r->xmm[5]; g_xmm6 = r->xmm[6]; g_xmm7 = r->xmm[7];
}

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart / (double)f.QuadPart;
}

/* Room for n more bytes in the arena (under s_lock). 0 = full. */
static int arena_room(uint64_t n)
{
    if (s_arena_used + n > RB_ARENA) return 0;
    while (s_arena_used + n > s_arena_committed) {
        if (!VirtualAlloc(s_arena + s_arena_committed, 16u << 20, MEM_COMMIT, PAGE_READWRITE)) return 0;
        s_arena_committed += 16u << 20;
    }
    return 1;
}

/* The first write to a protected page this tick: keep its old contents. */
static LONG CALLBACK rb_veh(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    int i;
    DWORD old;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!s_tracking || (i = page_of((const uint8_t *)er->ExceptionInformation[1])) < 0 || !s_tracked[i])
        return EXCEPTION_CONTINUE_SEARCH;
    AcquireSRWLockExclusive(&s_lock);
    if (!s_dirty[i]) {
        RbFrame *f = &s_frames[s_cur % RB_FRAMES];
        uint8_t *p = page_ptr((uint32_t)i);
        if (arena_room(RB_ENTRY)) {
            uint8_t *e = s_arena + s_arena_used;
            memcpy(e, &i, 4);
            memcpy(e + 8, p, RB_PAGE);
            s_arena_used += RB_ENTRY;
            f->npages++;
        } else
            s_oldest = s_cur + 1;                   /* history lost up to here */
        s_dirty[i] = 1;
        s_stat_faults++;
        VirtualProtect(p, RB_PAGE, PAGE_READWRITE, &old);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Start a tick's record at the arena's end (under s_lock, at a boundary). */
static void frame_begin(uint32_t tick)
{
    RbFrame *f = &s_frames[tick % RB_FRAMES];
    if (!arena_room(s_heap_max + 64u * RB_ENTRY)) {     /* arena full: start over */
        s_arena_used = 0;
        s_oldest = tick;
        arena_room(s_heap_max);
    }
    f->tick = tick;
    f->vtick = s_vtick;
    regs_save(&f->regs);
    f->heap = s_arena_used;
    s_arena_used += (xbox_HeapStateSave(s_arena + s_arena_used) + 15u) & ~(size_t)15u;
    f->first = s_arena_used;
    f->npages = 0;
    if (tick - s_oldest >= RB_FRAMES - 1) s_oldest = tick - (RB_FRAMES - 2);
}

/* Protect again what the tick that just ended touched. */
static void reprotect(const RbFrame *f)
{
    uint32_t k;
    DWORD old;
    for (k = 0; k < f->npages; k++) {
        uint32_t i;
        memcpy(&i, s_arena + f->first + (uint64_t)k * RB_ENTRY, 4);
        if (s_dirty[i]) {
            VirtualProtect(page_ptr(i), RB_PAGE, PAGE_READONLY, &old);
            s_dirty[i] = 0;
        }
    }
}

/* Write-protect a window, allocation by allocation, and note its pages. */
static void track_window(int w, uint32_t first, uint32_t pages)
{
    uint8_t *base = s_base[w];
    size_t off = 0, size = (size_t)pages * RB_PAGE;
    if (!base) return;
    while (off < size) {
        MEMORY_BASIC_INFORMATION mbi;
        size_t end;
        DWORD old;
        if (!VirtualQuery(base + off, &mbi, sizeof mbi)) break;
        end = (size_t)((uint8_t *)mbi.BaseAddress + mbi.RegionSize - base);
        if (end > size || end <= off) end = size;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD)) {
            /* tracked first: another thread's write the instant after the
             * protect must find the page known to rb_veh */
            memset(s_tracked + first + off / RB_PAGE, 1, (end - off) / RB_PAGE);
            if (!VirtualProtect(base + off, end - off, PAGE_READONLY, &old))
                memset(s_tracked + first + off / RB_PAGE, 0, (end - off) / RB_PAGE);
        }
        off = end;
    }
}

/* History on: from the next boundary every tick can be gone back to. Called at
 * a boundary, on the main thread. */
static int history_start(void)
{
    uint32_t i, n = 0, gsize = 0;
    if (s_tracking) return 1;
    s_heap_max = (xbox_HeapStateSize() + 15u) & ~(size_t)15u;
    if (!s_arena) s_arena = (uint8_t *)VirtualAlloc(NULL, RB_ARENA, MEM_RESERVE, PAGE_READWRITE);
    if (!s_arena) return 0;
    s_base[0] = (uint8_t *)((uintptr_t)0 + g_xbox_mem_offset);
    s_base[1] = (uint8_t *)((uintptr_t)0x80000000u + g_xbox_mem_offset);
    s_base[2] = (uint8_t *)xbox_Nv2aRegs(&gsize);
    AddVectoredExceptionHandler(1, rb_veh);
    /* The USB controller model stops: pads come through gz_rb_pad() now, and
     * the driver's queues in guest memory must not go back in time under a
     * controller that did not. */
    xbox_OhciPause(1);
    xbox_Nv2aPause(1);
    AcquireSRWLockExclusive(&s_lock);
    memset(s_tracked, 0, sizeof s_tracked);
    memset(s_dirty, 0, sizeof s_dirty);
    s_cur = s_oldest = 0;
    s_arena_used = 0;
    s_vbase = xbox_ReadTimeStampCounter();
    s_vtick = 0;
    frame_begin(0);
    InterlockedExchange(&s_tracking, 1);        /* before protecting: a write now must reach rb_veh */
    ReleaseSRWLockExclusive(&s_lock);           /* (rb_veh takes the lock itself) */
    track_window(0, 0, RB_WPAGES);
    track_window(1, RB_WPAGES, RB_WPAGES);
    if (s_base[2] && gsize >= RB_GPAGES * RB_PAGE) track_window(2, 2u * RB_WPAGES, RB_GPAGES);
    {
        /* Worker threads' stacks stay out of the history: they are those
         * threads' present, and putting one back in time under a running
         * thread crashes it (the music thread did). */
        uint32_t bases[32], ssize = 0;
        int k, ns = xbox_ThreadStacks(bases, 32, &ssize);
        DWORD old;
        for (k = 0; k < ns; k++) {
            uint32_t p0 = bases[k] / RB_PAGE, p1 = (bases[k] + ssize + RB_PAGE - 1) / RB_PAGE;
            if (p1 > RB_WPAGES) continue;
            VirtualProtect(page_ptr(p0), (size_t)(p1 - p0) * RB_PAGE, PAGE_READWRITE, &old);
            memset(s_tracked + p0, 0, p1 - p0);
        }
        /* and the interrupt / DPC handlers' stacks, at the bottom of the
         * stack region (0x00780000 up; the main thread's is at its top) */
        VirtualProtect(page_ptr(0x00780000u / RB_PAGE), 0x00100000u, PAGE_READWRITE, &old);
        memset(s_tracked + 0x00780000u / RB_PAGE, 0, 0x00100000u / RB_PAGE);
        fprintf(stderr, "[RB] %d worker stack(s) and the handler stacks left out\n", ns);
    }
    for (i = 0; i < RB_NPAGES; i++) n += s_tracked[i];
    xbox_Nv2aResume();
    fprintf(stderr, "[RB] history on: %u pages (%.0f MB) write-protected\n", n, n * (double)RB_PAGE / 1048576.0);
    return 1;
}

/* A boundary: close the tick that ran, open the next. */
static void boundary(void)
{
    RbFrame *f = &s_frames[s_cur % RB_FRAMES];
    xbox_DispatchGateEnter();           /* sound and other device code: not in flight */
    xbox_Nv2aPause(1);
    AcquireSRWLockExclusive(&s_lock);
    if (f->npages > s_stat_pages_max) s_stat_pages_max = f->npages;
    reprotect(f);
    s_cur++;
    s_vtick++;
    frame_begin(s_cur);
    ReleaseSRWLockExclusive(&s_lock);
    xbox_Nv2aResume();
    xbox_DispatchGateLeave();
}

/* Back n ticks: the game is as it was at that boundary (this thread's
 * registers included, so the caller must return to the main loop as if it had
 * just arrived there). Returns 0 if the history does not reach that far. */
static int rollback(uint32_t n)
{
    uint32_t target = s_cur - n, t, pages = 0;
    DWORD old;
    double t0 = now_s();
    if (n > s_cur || target < s_oldest) return 0;
    xbox_DispatchGateEnter();
    xbox_Nv2aPause(1);
    AcquireSRWLockExclusive(&s_lock);
    /* Oldest tick first: a page's contents at the target boundary are in the
     * first record from there on that holds it (each record has a page once),
     * so every page is written back once. */
    {
        static uint8_t done[RB_NPAGES];
        memset(done, 0, sizeof done);
        for (t = target; ; t++) {
            const RbFrame *f = &s_frames[t % RB_FRAMES];
            uint32_t k;
            for (k = 0; k < f->npages; k++) {
                const uint8_t *e = s_arena + f->first + (uint64_t)k * RB_ENTRY;
                uint32_t i;
                uint8_t *p;
                memcpy(&i, e, 4);
                if (done[i]) continue;
                done[i] = 1;
                p = page_ptr(i);
                if (!s_dirty[i]) VirtualProtect(p, RB_PAGE, PAGE_READWRITE, &old);
                memcpy(p, e + 8, RB_PAGE);
                VirtualProtect(p, RB_PAGE, PAGE_READONLY, &old);
                s_dirty[i] = 0;
                pages++;
            }
            if (t == s_cur) break;
        }
        /* pages only the current tick touched are in its record, handled above;
         * a page dirty now but in no record cannot exist (rb_veh logs first) */
    }
    {
        RbFrame *f = &s_frames[target % RB_FRAMES];
        regs_load(&f->regs);
        s_vtick = f->vtick;
        xbox_HeapStateLoad(s_arena + f->heap);
        s_arena_used = f->first;                /* the later records are gone */
        f->npages = 0;
        s_cur = target;
    }
    ReleaseSRWLockExclusive(&s_lock);
    xbox_Nv2aResume();
    xbox_DispatchGateLeave();
    {
        static unsigned nlog;
        if (nlog++ < 12 || (nlog & 63u) == 0)
            fprintf(stderr, "[RB] back %u ticks to %u: %u pages put back in %.2f ms\n", n, target, pages, (now_s() - t0) * 1000.0);
    }
    return 1;
}

/* ── scripted match (GZ_RB_SCRIPT=1) ────────────────────────────────────
 *
 * The two-machine question, asked on one PC: do two copies of the game, given
 * the same match setup and the same input for every tick, play the same
 * match? Tick 0 is the boundary after the match camera is first set up (the
 * start of the fighters' intro; gz_rb_fight_camera, from sub_0001F740), the
 * pads of players 1 and 2 are a fixed pseudo-random function of the tick, and
 * pictures are taken at the ticks in GZ_RB_SHOT_AT ("610 1210", written as
 * <GZ_SHOT_FILE>_t<tick>.bmp). GZ_RB_SCRIPT_RB=<n> also goes back n ticks
 * every 30 and plays them again: a copy doing that must show the same
 * pictures as one that does not, or rollback is not exact. */
#define RB_SHOT_TICKS 40
static void shot_request(void);
static int      s_script = -1, s_script_rb, s_marker;
static uint32_t s_shot_at[8], s_nshot, s_last_rb = 0xFFFFFFFFu;

void gz_rb_fight_camera(void)
{
    if (!s_marker && s_script > 0) fprintf(stderr, "[RB] match camera set up: tick 0 at the next boundary\n");
    s_marker = 1;
}

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

/* The pad for a port this tick, or 0 to leave what the port read (called from
 * the game's pad poll, see "Pad input override" in recomp_manual.c). */
int gz_rb_pad(int port, uint8_t *pad18)
{
    uint32_t r;
    int16_t x, y;
    if (s_script <= 0 || !s_tracking || port > 1) return 0;
    /* a new decision every 12 ticks: walk somewhere, press one or two of the
     * attack buttons (never Start or Back) */
    r = mix((s_vtick / 12u) * 2u + (uint32_t)port + 0x9E3779B9u);
    memset(pad18, 0, 18);
    x = (r & 3u) == 0 ? 0 : (int16_t)((int)((r >> 2) & 0xFFFFu) - 32768);
    y = (r & 3u) == 0 ? 0 : (int16_t)((int)((r >> 14) & 0xFFFFu) - 32768);
    if (r & 0x00010000u) pad18[2] = 255;        /* A */
    if (r & 0x00020000u) pad18[3] = 255;        /* B */
    if (r & 0x00040000u) pad18[4] = 255;        /* X */
    if ((r & 0x00380000u) == 0) pad18[5] = 255; /* Y, less often */
    if ((r & 0x01C00000u) == 0) pad18[9] = 255; /* right trigger */
    memcpy(pad18 + 10, &x, 2);
    memcpy(pad18 + 12, &y, 2);
    return 1;
}

static void script_tick(void)
{
    uint32_t k;
    if (!s_tracking) {
        const char *e = getenv("GZ_RB_SHOT_AT"), *r = getenv("GZ_RB_SCRIPT_RB");
        if (!s_marker) return;
        while (e && *e && s_nshot < 8) {
            s_shot_at[s_nshot++] = (uint32_t)strtoul(e, (char **)&e, 10);
            while (*e == ' ' || *e == ',') e++;
        }
        s_script_rb = r ? atoi(r) : 0;
        if (!history_start()) { s_script = 0; return; }
        fprintf(stderr, "[RB] scripted match: tick 0%s\n", s_script_rb ? ", with rollbacks" : "");
    } else
        boundary();
    for (k = 0; k < s_nshot; k++) {
        if (s_vtick == s_shot_at[k]) shot_request();
        if (s_vtick == s_shot_at[k] + RB_SHOT_TICKS) {
            const char *f = getenv("GZ_SHOT_FILE");
            char a[600], b[600];
            if (f && *f) {
                snprintf(a, sizeof a, "%s.bmp", f);
                snprintf(b, sizeof b, "%s_t%u.bmp", f, s_shot_at[k]);
                MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
                fprintf(stderr, "[RB] tick %u: picture kept\n", s_shot_at[k]);
            }
        }
    }
    if (s_script_rb > 0 && s_vtick && s_vtick % 30u == 0 && s_vtick != s_last_rb && s_cur >= (uint32_t)s_script_rb) {
        s_last_rb = s_vtick;                    /* once: the replay comes through here again */
        rollback((uint32_t)s_script_rb);
    }
    xbox_VirtualTscSet(s_vbase + (uint64_t)s_vtick * (RB_TSC_HZ / 60ull));
}

/* ── the sync test ─────────────────────────────────────────────────────── */
static int    s_state = -1, s_count, s_round, s_armed;
static int    s_ticks = 10, s_rounds = 3;
static double s_at;

void gz_rb_arm(void) { s_armed = 1; }       /* the quick fight launched */

static void shot_request(void)
{
    const char *f = getenv("GZ_SHOT_FILE");
    FILE *h;
    if (f && *f && (h = fopen(f, "w")) != NULL) fclose(h);
}

static void shot_keep(int run)
{
    const char *f = getenv("GZ_SHOT_FILE");
    char a[600], b[600];
    if (!f || !*f) return;
    snprintf(a, sizeof a, "%s.bmp", f);
    snprintf(b, sizeof b, "%s_r%d_run%d.bmp", f, s_round + 1, run);
    MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
}

/* Once per frame, from the limiter's main-loop call. */
void gz_rb_tick(void)
{
    static double t0, tick_s;
    static unsigned tick_n;
    if (s_script < 0) { const char *e = getenv("GZ_RB_SCRIPT"); s_script = e && *e == '1'; }
    if (s_script > 0) { if (s_armed) script_tick(); return; }
    if (s_state == 9) return;
    if (s_state < 0) {
        const char *e = getenv("GZ_RB_TEST"), *k = getenv("GZ_RB_TICKS"), *r = getenv("GZ_RB_ROUNDS");
        if (!e || atof(e) <= 0.0) { s_state = 9; return; }
        s_at = atof(e);
        if (k && atoi(k) > 0) s_ticks = atoi(k);
        if (r && atoi(r) > 0) s_rounds = atoi(r);
        s_state = 0;
        fprintf(stderr, "[RB] sync test %.0f s after the quick fight launches: %d ticks, %d rounds\n", s_at, s_ticks, s_rounds);
    }
    if (s_state == 0) {
        if (!s_armed) return;
        if (t0 == 0.0) t0 = now_s();
        if (now_s() - t0 < s_at - 2.0) return;
        if (!history_start()) { s_state = 9; return; }      /* two seconds of plain history first */
        s_state = 1;
    } else {
        double a = now_s();
        boundary();
        tick_s += now_s() - a; tick_n++;
    }
    switch (s_state) {
    case 1:
        if (now_s() - t0 < s_at) break;
        fprintf(stderr, "[RB] round %d: boundaries cost %.3f ms each so far (%u ticks, %llu page faults, busiest tick %llu pages)\n",
                s_round + 1, tick_n ? tick_s * 1000.0 / tick_n : 0.0, tick_n, (unsigned long long)s_stat_faults,
                (unsigned long long)s_stat_pages_max);
        s_count = 0;
        s_state = 2;
        break;
    case 2:                                             /* first run */
        if (++s_count == s_ticks) shot_request();
        if (s_count < s_ticks + RB_SHOT_TICKS) break;
        shot_keep(1);
        if (!rollback((uint32_t)s_count)) { fprintf(stderr, "[RB] history too short\n"); s_state = 8; break; }
        s_count = 0;
        s_state = 3;
        break;
    case 3:                                             /* the same ticks again */
        if (++s_count == s_ticks) shot_request();
        if (s_count < s_ticks + RB_SHOT_TICKS) break;
        shot_keep(2);
        fprintf(stderr, "[RB] round %d done\n", s_round + 1);
        s_count = 0;
        s_state = ++s_round < s_rounds ? 2 : 8;
        break;
    }
    if (s_state == 8) {
        fprintf(stderr, "[RB] sync test done; history stays on\n");
        s_state = 7;
    }
    /* the main thread's clock is the tick number */
    xbox_VirtualTscSet(s_vbase + (uint64_t)s_vtick * (RB_TSC_HZ / 60ull));
}
