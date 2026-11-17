/*
 * gz_rbnet.c - rollback netplay for two players (GZ_RB_NET=1).
 *
 * Both PCs play the same local Versus match, each with its own player's pad
 * read here and the other player's arriving over the network. The game runs
 * ahead on a prediction of the remote pad (the last one received); when the
 * real one arrives and differs, gz_rollback.c puts the game back to that tick
 * and plays the ticks again, without showing them, up to where it was.
 *
 * The match must play the same on both PCs given the same pads every tick.
 * Two things make it so:
 *  - the setup: the host (side 1) sends its GameInfo (monsters, city, seed)
 *    before the quick fight launches, and the client plays that;
 *  - tick 0: what the game holds when the fight starts still depends on how
 *    long each machine sat in the menus (clocks, the random generator). The
 *    client sends a hash of every low-memory page, the host answers with the
 *    pages that differ and its clock at tick 0, and the client takes them.
 *    From there the game reads only the virtual clock (kernel_hal.c), the
 *    worker threads run in lockstep, and the fight plays the same.
 *
 * Setup and tick 0 go over TCP (the host listens); pads go over UDP, each
 * packet carrying every pad the peer has not acknowledged.
 *
 *   GZ_RB_NET=1            on
 *   GZ_RB_SIDE=1|2         1 = host, player 1; 2 = client, player 2
 *   GZ_RB_PEER=ip[:port]   the other PC (the client needs it; the host learns
 *                          the client's address from its connection)
 *   GZ_RB_PORT=n           this side's port, TCP and UDP (default 7790)
 *   GZ_RB_DELAY=n          input delay in ticks (default 2)
 *   GZ_RB_WINDOW=n         ticks the game may run on predictions (default 8)
 *   GZ_RB_LAG_MS=n         hold each packet this long before sending (a test
 *                          of latency on one PC)
 *   GZ_RB_SCRIPT=1         with GZ_RB_NET: this side's pad from the script in
 *                          gz_rollback.c instead of the PC's controller, so
 *                          two copies can be compared tick for tick
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* gz_rollback.c */
uint32_t gz_rb_cur_tick(void);
int      gz_rb_go_back(uint32_t n);
void     gz_rb_replay_begin(uint32_t until);
int      gz_rb_page_count(void);
const uint8_t *gz_rb_shareable_page(int i);         /* NULL = not shared */
uint8_t *gz_rb_page_for_write(int i);
void     gz_rb_set_vbase(uint64_t v);
uint64_t gz_rb_tick0_clock(void);
void     gz_rb_park_workers(void);
int      gz_rb_script_pad(int port, uint32_t tick, uint8_t *pad18);
int      usb_gamepad_report(int pad, uint8_t *out, int max);  /* usb_gamepad.c */
size_t   xbox_HeapStateSize(void);                      /* the heap's tables (host side) */
size_t   xbox_HeapStateSave(void *buf);
void     xbox_HeapStateLoad(const void *buf);
size_t   xbox_DevicesStateSize(void);                   /* vblank, timers, DPCs, their clock */
void     xbox_DevicesStateSave(void *buf);
void     xbox_DevicesStateLoad(const void *buf);
size_t   xbox_ApuStateSize(void);                       /* the sound chip */
int      xbox_ApuStateSave(void *buf);
int      xbox_ApuStateLoad(const void *buf);
size_t   xbox_Nv2aStateSize(void);                      /* the GPU's method state */
void     xbox_Nv2aStateSave(void *buf);
void     xbox_Nv2aStateLoad(const void *buf);
int      xbox_Nv2aPause(int want_idle);
void     xbox_Nv2aResume(void);
void     xbox_DevicesStart(void);
int      gz_opt_wide(void);                              /* gz_settings.c */
void     gz_opt_force_wide(int v);
#define  OPTS_TAG 0x5354504Fu                           /* "OPTS" */
int      gz_rb_tick_digest(uint32_t t, uint64_t h[16]);  /* 16 parts: pages spread by number */
int      gz_rb_write_digests(uint32_t upto, const char *path);

/* The host-side states tick 0 sends after the pages, each tagged and sized:
 * what the pages were laid out by or run beside, which pages cannot carry. */
enum { ST_HEAP, ST_DEVICES, ST_APU, ST_GPU, ST_COUNT };
static const uint32_t s_st_tag[ST_COUNT] = { 0x50414548u, 0x53564544u, 0x20555041u, 0x20555047u };
static const char *s_st_name[ST_COUNT] = { "heap", "devices", "sound chip", "GPU" };

static size_t st_size(int k)
{
    switch (k) {
    case ST_HEAP: return xbox_HeapStateSize();
    case ST_DEVICES: return xbox_DevicesStateSize();
    case ST_APU: return xbox_ApuStateSize();
    default: return xbox_Nv2aStateSize();
    }
}

static int st_save(int k, void *buf)
{
    switch (k) {
    case ST_HEAP: xbox_HeapStateSave(buf); return 1;
    case ST_DEVICES: xbox_DevicesStateSave(buf); return 1;
    case ST_APU: return xbox_ApuStateSave(buf);
    default: xbox_Nv2aPause(1); xbox_Nv2aStateSave(buf); xbox_Nv2aResume(); return 1;
    }
}

static void st_load(int k, const void *buf)
{
    switch (k) {
    case ST_HEAP: xbox_HeapStateLoad(buf); break;
    case ST_DEVICES: xbox_DevicesStateLoad(buf); break;
    case ST_APU: xbox_ApuStateLoad(buf); break;
    default: xbox_Nv2aPause(1); xbox_Nv2aStateLoad(buf); xbox_Nv2aResume(); break;
    }
}

#define PAD      18
#define RING     512u                   /* ticks of pads kept */
#define MAXSEND  40u                    /* pads per packet at most */
#define MAGIC    0x5242475Bu            /* "[GBR": a build with the desync check */

enum { M_HELLO = 1, M_SETUP, M_HASHES, M_STATE, M_FILES, M_LOBBY };
#define MAX_TRANSFER 2000u              /* pages at tick 0, at most (about 8 MB) */
#define REFUSED      0xFFFFFFFFu

static int      s_on = -1, s_side, s_delay = 2, s_window = 8, s_lag_ms, s_script;
static SOCKET   s_tcp = INVALID_SOCKET, s_udp = INVALID_SOCKET;
static struct sockaddr_in s_peer;
static int      s_have_peer;
static uint16_t s_port = 7790;

/* pads: local by tick, remote by tick, what the game was given for the
 * remote player at each tick */
static uint8_t  s_local[RING][PAD], s_remote[RING][PAD], s_used[RING][PAD];
static uint8_t  s_used_pred[RING];      /* that tick ran on a prediction */
static int64_t  s_local_max = -1;       /* last tick with a local pad */
static int64_t  s_remote_max = -1;      /* last tick with every remote pad up to it */
static int64_t  s_peer_acked = -1;      /* last local tick the peer has */
static int64_t  s_first_bad = -1;       /* earliest tick that ran on a wrong guess */
static uint32_t s_peer_tick;            /* the peer's tick, from its last packet */
static double   s_peer_tick_at;         /* when that packet came */
static double   s_ahead_avg;
static unsigned s_stat_rb, s_stat_rb_ticks, s_stat_stall_ms;

/* The desync check: each packet carries this side's digest of its last
 * final tick (one that ran on both players' real pads and will not be
 * played again); the other side compares it with its own of that tick. */
static int64_t  s_final = -1;           /* this side's last final tick */
static struct { uint32_t tick; uint64_t h[16]; } s_rdig[64];  /* the other side's, to check */
static unsigned s_rdig_n;
static int      s_desynced;
/* checked; a few parts different (a word left on a stack or in a buffer that
 * follows this PC's GPU, seen between two copies on one PC); most different */
static unsigned s_stat_checked, s_stat_small, s_stat_bad;
#define DESYNC_PARTS 6                  /* parts that differ, at least, for a desync */

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart / (double)f.QuadPart;
}

int gz_rbn_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("GZ_RB_NET");
        s_on = e && *e == '1';
        if (s_on) {
            const char *v;
            s_side = (v = getenv("GZ_RB_SIDE")) && *v == '2' ? 2 : 1;
            if ((v = getenv("GZ_RB_DELAY")) && *v) s_delay = atoi(v);
            if ((v = getenv("GZ_RB_WINDOW")) && *v) s_window = atoi(v);
            if ((v = getenv("GZ_RB_LAG_MS")) && *v) s_lag_ms = atoi(v);
            if ((v = getenv("GZ_RB_PORT")) && *v) s_port = (uint16_t)atoi(v);
            s_script = (v = getenv("GZ_RB_SCRIPT")) && *v == '1';
            if (s_delay < 0) s_delay = 0;
            if (s_delay > 10) s_delay = 10;
            if (s_window < 1) s_window = 1;
            if (s_window > 60) s_window = 60;
            fprintf(stderr, "[RBN] rollback netplay: side %d (player %d), port %u, input delay %d, window %d%s\n",
                    s_side, s_side, (unsigned)s_port, s_delay, s_window, s_script ? ", scripted pad" : "");
        }
    }
    return s_on;
}

int gz_rbn_local_port(void) { return s_side - 1; }

/* A fight is on (from its camera set-up until the menus run again): only
 * then do ticks run on a guess of the other side's pad (see gz_rbn_tick). */
static int s_fight;
void gz_rbn_set_fight(int on)
{
    if (s_on > 0 && s_fight != on) {
        s_fight = on;
        fprintf(stderr, "[RBN] %s\n", on ? "fight: rollback on guesses" : "menus: waiting for both pads (lockstep)");
    }
}

/* ── straight to the Versus screen (GZ_RB_TO_VERSUS=1) ─────────────────
 *
 * A session started from the Multi-Player menu opens on the game's Versus
 * screen: the host's side presses the way there (Versus on the main menu,
 * then the host's profile), the picture stays black until it is up, and
 * player 2 joins on it with Start, as in the game's own Versus. The
 * presses are the host's pad, so both PCs get them; they are timed from
 * the frame the main menu builds its lists (its "Versus" entry), which in
 * lockstep is the same frame on both, however fast each PC is. Until then
 * neither PC's controller counts. */
#define TV_DOWN   120                   /* frames after the main menu: Down (to Versus) */
#define TV_A1     180                   /* A: Versus (the Profiles screen) */
#define TV_A2     260                   /* A: the host's profile */
#define TV_END    300                   /* the controllers count again */
#define TV_SHOW   400                   /* the picture is shown */
static int     s_to_versus = -1;
static int64_t s_tv_anchor = -1;

static int to_versus(void)
{
    if (s_to_versus < 0) {
        const char *e = getenv("GZ_RB_TO_VERSUS");
        s_to_versus = e && *e == '1' && gz_rbn_session();
    }
    return s_to_versus;
}

/* A menu list asked an entry's name (recomp_manual.c, sub_0009B0C0). */
void gz_rbn_menu_entry(const char *name)
{
    if (to_versus() && s_tv_anchor < 0 && !strcmp(name, "Versus")) {
        s_tv_anchor = gz_rb_cur_tick();
        fprintf(stderr, "[RBN] main menu at tick %lld: on to the Versus screen\n", (long long)s_tv_anchor);
    }
}

/* Side `side`'s pad on the way (1 = this tick's pad is set): the host
 * presses, player 2 nothing. Each PC works out the other's too, so neither
 * waits for the other's pads on the way (both run as fast as they can). */
static int tv_pad(int side, uint32_t tick, uint8_t *pad18)
{
    int64_t d;
    if (!to_versus()) return 0;
    if (s_tv_anchor < 0) { memset(pad18, 0, PAD); return 1; }
    d = (int64_t)tick - s_tv_anchor;
    if (d >= TV_END) return 0;
    memset(pad18, 0, PAD);
    if (side != 1) return 1;
    if (d >= TV_DOWN && d < TV_DOWN + 9) pad18[0] = 0x02;              /* D-pad down */
    if ((d >= TV_A1 && d < TV_A1 + 18) || (d >= TV_A2 && d < TV_A2 + 18))
        pad18[2] = 0xFF;                                                /* A */
    return 1;
}

/* The picture stays black until the Versus screen is up (gpu_d3d11.c). */
int gz_rbn_hide_picture(void)
{
    static int shown;
    int hide = to_versus() && (s_tv_anchor < 0 || (int64_t)gz_rb_cur_tick() < s_tv_anchor + TV_SHOW);
    if (to_versus() && !hide && !shown) {
        /* how long the restart took: since this process started */
        FILETIME c, e, k, u, now;
        shown = 1;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            ULARGE_INTEGER a, b;
            GetSystemTimeAsFileTime(&now);
            a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime;
            b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
            fprintf(stderr, "[RBN] the Versus screen is up: %.1f s after the game started again\n",
                    (double)(b.QuadPart - a.QuadPart) / 1e7);
        }
    }
    return hide;
}

/* A session (GZ_RB_SESSION=1, from the Multi-Player menu): the two PCs play
 * the menus as well as the fights together, from the first frame. */
int gz_rbn_session(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("GZ_RB_SESSION"); v = e && *e == '1'; }
    return v && gz_rbn_on();
}

/* ── TCP: whole messages ─────────────────────────────────────────────── */

static int tcp_all(SOCKET s, void *buf, int n, int send_)
{
    char *p = (char *)buf;
    while (n > 0) {
        int k = send_ ? send(s, p, n, 0) : recv(s, p, n, 0);
        if (k <= 0) return 0;
        p += k;
        n -= k;
    }
    return 1;
}

static int msg_send(uint32_t type, const void *a, uint32_t na, const void *b, uint32_t nb)
{
    uint32_t h[2];
    h[0] = type;
    h[1] = na + nb;
    return tcp_all(s_tcp, h, 8, 1) && (!na || tcp_all(s_tcp, (void *)a, (int)na, 1))
        && (!nb || tcp_all(s_tcp, (void *)b, (int)nb, 1));
}

/* The next message: its type, its payload in a malloc'd buffer. */
static uint8_t *msg_recv(uint32_t *type, uint32_t *len)
{
    uint32_t h[2];
    uint8_t *p;
    if (!tcp_all(s_tcp, h, 8, 0)) return NULL;
    if (h[1] > (256u << 20)) return NULL;
    p = (uint8_t *)malloc(h[1] ? h[1] : 1);
    if (!p) return NULL;
    if (h[1] && !tcp_all(s_tcp, p, (int)h[1], 0)) { free(p); return NULL; }
    *type = h[0];
    *len = h[1];
    return p;
}

static int parse_peer(const char *v, struct sockaddr_in *a)
{
    char host[128];
    const char *c = strrchr(v, ':');
    size_t n = c ? (size_t)(c - v) : strlen(v);
    if (n >= sizeof host) return 0;
    memcpy(host, v, n);
    host[n] = 0;
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons(c ? (uint16_t)atoi(c + 1) : (uint16_t)7790);
    return inet_pton(AF_INET, host, &a->sin_addr) == 1;
}

/* Sockets up and the TCP link made: the host waits for the client to
 * connect, the client connects to the host (both retry for a minute). */
static int s_link_done;                 /* connect_peer has run (or the lobby made the link) */
static int s_hello_taken;               /* the host has the client's hello already (the lobby) */

static int connect_peer(void)
{
#define done s_link_done
    WSADATA w;
    struct sockaddr_in me;
    u_long nb = 1;
    int one = 1;
    double t0 = now_s();
    if (done) return s_tcp != INVALID_SOCKET;
    done = 1;
    WSAStartup(MAKEWORD(2, 2), &w);
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_port = htons(s_port);
    me.sin_addr.s_addr = htonl(INADDR_ANY);

    s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp == INVALID_SOCKET || bind(s_udp, (struct sockaddr *)&me, sizeof me) != 0) {
        fprintf(stderr, "[RBN] UDP port %u: cannot bind (%d)\n", (unsigned)s_port, WSAGetLastError());
        return 0;
    }
    ioctlsocket(s_udp, FIONBIO, &nb);

    if (s_side == 1) {
        SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int alen = sizeof s_peer;
        setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        if (l == INVALID_SOCKET || bind(l, (struct sockaddr *)&me, sizeof me) != 0 || listen(l, 1) != 0) {
            fprintf(stderr, "[RBN] TCP port %u: cannot listen (%d)\n", (unsigned)s_port, WSAGetLastError());
            return 0;
        }
        fprintf(stderr, "[RBN] waiting for player 2 on port %u\n", (unsigned)s_port);
        s_tcp = accept(l, (struct sockaddr *)&s_peer, &alen);
        closesocket(l);
        if (s_tcp == INVALID_SOCKET) return 0;
        /* the client's UDP port is in its hello */
    } else {
        const char *v = getenv("GZ_RB_PEER");
        if (!v || !parse_peer(v, &s_peer)) {
            fprintf(stderr, "[RBN] GZ_RB_PEER=ip[:port] is needed on side 2\n");
            return 0;
        }
        for (;;) {
            s_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (connect(s_tcp, (struct sockaddr *)&s_peer, sizeof s_peer) == 0) break;
            closesocket(s_tcp);
            s_tcp = INVALID_SOCKET;
            if (now_s() - t0 > 60.0) {
                fprintf(stderr, "[RBN] no host at %s\n", v);
                return 0;
            }
            Sleep(250);
        }
        {
            uint32_t hello[2] = { MAGIC, s_port };
            msg_send(M_HELLO, hello, sizeof hello, NULL, 0);
        }
    }
    setsockopt(s_tcp, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    s_have_peer = 1;
    fprintf(stderr, "[RBN] connected (%.1f s)\n", now_s() - t0);
    return 1;
#undef done
}

/* ── a session's start: the host's profile for both ──────────────────── */

/* Files under dir (relative names), appended to buf as
 * [u16 name length][name][u32 size][bytes]. */
static void pack_dir(const char *root, const char *rel, uint8_t **buf, uint32_t *len, uint32_t *cap, uint32_t *count)
{
    char pat[MAX_PATH * 2], full[MAX_PATH * 2], sub[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\%s%s*", root, rel, *rel ? "\\" : "");
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "\\" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            pack_dir(root, sub, buf, len, cap, count);
            continue;
        }
        snprintf(full, sizeof full, "%s\\%s", root, sub);
        {
            FILE *f = fopen(full, "rb");
            long sz;
            uint16_t nl = (uint16_t)strlen(sub);
            uint32_t s32;
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz < 0 || sz > (16 << 20)) { fclose(f); continue; }
            while (*len + 2 + nl + 4 + (uint32_t)sz > *cap) {
                *cap = *cap ? *cap * 2 : (1u << 20);
                *buf = (uint8_t *)realloc(*buf, *cap);
            }
            s32 = (uint32_t)sz;
            memcpy(*buf + *len, &nl, 2); *len += 2;
            memcpy(*buf + *len, sub, nl); *len += nl;
            memcpy(*buf + *len, &s32, 4); *len += 4;
            *len += (uint32_t)fread(*buf + *len, 1, s32, f);
            fclose(f);
            (*count)++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void make_dirs_for(const char *path)
{
    char d[MAX_PATH * 2], *p;
    snprintf(d, sizeof d, "%s", path);
    for (p = d + 3; *p; p++)
        if (*p == '\\' || *p == '/') { char c = *p; *p = 0; CreateDirectoryA(d, NULL); *p = c; }
}

static void delete_tree(const char *dir)
{
    char pat[MAX_PATH * 2], one[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    if ((h = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(one, sizeof one, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(one);
            else DeleteFileA(one);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

/* Unpack [count] files into <save>\UDATA. */
static void unpack_files(const uint8_t *m, uint32_t len, const char *udata)
{
    uint32_t count, k, off = 4;
    if (len < 4) return;
    memcpy(&count, m, 4);
    for (k = 0; k < count && off + 2 <= len; k++) {
        uint16_t nl;
        uint32_t sz;
        char name[MAX_PATH], full[MAX_PATH * 2];
        FILE *f;
        memcpy(&nl, m + off, 2); off += 2;
        if (nl >= sizeof name || off + nl + 4 > len) break;
        memcpy(name, m + off, nl); name[nl] = 0; off += nl;
        memcpy(&sz, m + off, 4); off += 4;
        if (off + sz > len || strstr(name, "..")) break;
        snprintf(full, sizeof full, "%s\\%s", udata, name);
        make_dirs_for(full);
        if ((f = fopen(full, "wb")) != NULL) { fwrite(m + off, 1, sz, f); fclose(f); }
        off += sz;
    }
}

/* At boot, before the game starts (main.c): connect, and give both PCs the
 * host's profile, so the menus (profiles, unlocks) are the same on both.
 * 0 = no session (the game then starts on its own). */
static int exchange_profile(const char *udata, const char *from);

int gz_rbn_session_boot(void)
{
    const char *save = getenv("GZ_SAVE_DIR"), *from = getenv("GZ_RB_PROFILE_FROM");
    char udata[MAX_PATH * 2];
    if (!gz_rbn_session()) return 0;
    if (!save || !*save) { s_on = 0; return 0; }
    snprintf(udata, sizeof udata, "%s\\UDATA", save);
    if (!connect_peer()) {
        MessageBoxA(NULL, s_side == 2 ? "Could not reach the host. Check the IP address and that port 7790 reaches it."
                                      : "Player 2 did not connect.",
                    "Rollback Netplay", MB_ICONWARNING);
        s_on = 0;
        return 0;
    }
    if (!exchange_profile(udata, from)) { s_on = 0; return 0; }
    return 1;
}

/* After the link: the host takes the client's hello (its UDP port) and
 * sends its profile (UDATA) and the settings that change a match; both
 * sides keep the same copy in <GZ_SAVE_DIR>\UDATA. */
static int exchange_profile(const char *udata, const char *from)
{
    uint32_t type, len;
    uint8_t *m;
    delete_tree(udata);
    CreateDirectoryA(udata, NULL);
    if (s_side == 1) {
        uint8_t *buf = NULL;
        uint32_t blen = 4, cap = 0, count = 0;
        char src[MAX_PATH * 2];
        m = msg_recv(&type, &len);              /* the hello: its UDP port */
        if (!m || type != M_HELLO || len < 8 || ((uint32_t *)m)[0] != MAGIC) { free(m); return 0; }
        s_peer.sin_port = htons((uint16_t)((uint32_t *)m)[1]);
        s_hello_taken = 1;
        free(m);
        cap = 1u << 20;
        buf = (uint8_t *)malloc(cap);
        if (from && *from) {
            snprintf(src, sizeof src, "%s\\UDATA", from);
            pack_dir(src, "", &buf, &blen, &cap, &count);
        }
        memcpy(buf, &count, 4);
        /* after the files, the host's settings that change the match:
         * widescreen (what the game culls against) */
        if (blen + 8 > cap) { uint8_t *nb = (uint8_t *)realloc(buf, blen + 8); if (nb) { buf = nb; cap = blen + 8; } }
        if (blen + 8 <= cap) {
            uint32_t tag = OPTS_TAG, wide = (uint32_t)gz_opt_wide();
            memcpy(buf + blen, &tag, 4);
            memcpy(buf + blen + 4, &wide, 4);
            blen += 8;
        }
        msg_send(M_FILES, buf, blen, NULL, 0);
        unpack_files(buf, blen, udata);         /* the same copy here */
        free(buf);
        fprintf(stderr, "[RBN] session: %u profile file(s) sent to player 2\n", count);
    } else {
        m = msg_recv(&type, &len);
        if (!m || type != M_FILES) { free(m); return 0; }
        unpack_files(m, len, udata);
        if (len >= 12) {
            uint32_t tag, wide;
            memcpy(&tag, m + len - 8, 4);
            memcpy(&wide, m + len - 4, 4);
            if (tag == OPTS_TAG) gz_opt_force_wide((int)wide);
        }
        fprintf(stderr, "[RBN] session: the host's profile received (%u bytes)\n", len);
        free(m);
    }
    return 1;
}

/* ── the lobby's link (gz_lobby.c) ─────────────────────────────────────
 *
 * The lobby makes the link itself, on a thread of its own while it draws:
 * the host listens and takes one player at a time (again after one leaves),
 * the client connects. Each link starts with the hello and the host's
 * profile, as a session's; the lobby's messages (M_LOBBY) follow, and the
 * match that the lobby starts goes on over the same link. */
static SOCKET s_listen = INVALID_SOCKET;
void gz_rbn_lobby_drop(void);

static int lobby_sockets(void)
{
    static int up;
    WSADATA w;
    struct sockaddr_in me;
    u_long nb = 1;
    int one = 1;
    double t0 = now_s();
    if (up) return up > 0;
    up = -1;
    WSAStartup(MAKEWORD(2, 2), &w);
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_port = htons(s_port);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    /* (the run before, which started this one, may still be closing its
     * own: a few seconds' grace) */
    for (;;) {
        s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp != INVALID_SOCKET && bind(s_udp, (struct sockaddr *)&me, sizeof me) == 0) break;
        if (s_udp != INVALID_SOCKET) closesocket(s_udp);
        s_udp = INVALID_SOCKET;
        if (now_s() - t0 > 5.0) {
            fprintf(stderr, "[RBN] UDP port %u: cannot bind (%d)\n", (unsigned)s_port, WSAGetLastError());
            return 0;
        }
        Sleep(200);
    }
    SetHandleInformation((HANDLE)s_udp, HANDLE_FLAG_INHERIT, 0);
    ioctlsocket(s_udp, FIONBIO, &nb);
    if (s_side == 1) {
        s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
        if (s_listen == INVALID_SOCKET || bind(s_listen, (struct sockaddr *)&me, sizeof me) != 0
                || listen(s_listen, 1) != 0) {
            fprintf(stderr, "[RBN] TCP port %u: cannot listen (%d)\n", (unsigned)s_port, WSAGetLastError());
            return 0;
        }
        SetHandleInformation((HANDLE)s_listen, HANDLE_FLAG_INHERIT, 0);
    }
    up = 1;
    return 1;
}

/* Make the link (blocking; the lobby runs it on a thread). The host waits
 * for a player for as long as it takes; the client tries for `timeout_s`.
 * 1 = linked, 0 = no (the reason in *why). */
int gz_rbn_lobby_link(double timeout_s, const char **why)
{
    const char *save = getenv("GZ_SAVE_DIR"), *from = getenv("GZ_RB_PROFILE_FROM");
    char udata[MAX_PATH * 2];
    int one = 1;
    DWORD rcv = 60000;                          /* a silent peer: messages fail after a minute (the
                                                 * other PC may take a while to load the match) */
    double t0 = now_s();
    *why = "";
    if (!gz_rbn_on()) { *why = "netplay is off"; return 0; }
    if (!lobby_sockets()) { *why = s_side == 1 ? "the port is in use on this PC" : "cannot open the port on this PC"; return 0; }
    snprintf(udata, sizeof udata, "%s\\UDATA", save && *save ? save : ".");
    if (s_side == 1) {
        struct sockaddr_in a;
        int alen = sizeof a;
        fprintf(stderr, "[RBN] lobby: waiting for player 2 on port %u\n", (unsigned)s_port);
        s_tcp = accept(s_listen, (struct sockaddr *)&a, &alen);
        if (s_tcp == INVALID_SOCKET) { *why = "accept failed"; return 0; }
        s_peer = a;
    } else {
        const char *v = getenv("GZ_RB_PEER");
        if (!v || !parse_peer(v, &s_peer)) { *why = "no host address"; return 0; }
        for (;;) {
            s_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (connect(s_tcp, (struct sockaddr *)&s_peer, sizeof s_peer) == 0) break;
            closesocket(s_tcp);
            s_tcp = INVALID_SOCKET;
            if (now_s() - t0 > timeout_s) { *why = "no lobby at that address"; return 0; }
            Sleep(250);
        }
        {
            uint32_t hello[2] = { MAGIC, s_port };
            msg_send(M_HELLO, hello, sizeof hello, NULL, 0);
        }
    }
    setsockopt(s_tcp, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    setsockopt(s_tcp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv, sizeof rcv);
    SetHandleInformation((HANDLE)s_tcp, HANDLE_FLAG_INHERIT, 0);
    if (!exchange_profile(udata, from)) {
        *why = s_side == 1 ? "player 2 runs a different version" : "the lobby is full (or the host runs a different version)";
        gz_rbn_lobby_drop();
        return 0;
    }
    s_have_peer = 1;
    fprintf(stderr, "[RBN] lobby: linked (%.1f s)\n", now_s() - t0);
    return 1;
}

/* The other player left (or went silent): the link goes; the host can take
 * the next one. */
void gz_rbn_lobby_drop(void)
{
    if (s_tcp != INVALID_SOCKET) { closesocket(s_tcp); s_tcp = INVALID_SOCKET; }
    s_hello_taken = 0;
    s_have_peer = 0;
}

/* A lobby message out; 0 = the link is gone. */
int gz_rbn_lobby_send(const void *p, uint32_t n)
{
    return s_tcp != INVALID_SOCKET && msg_send(M_LOBBY, p, n, NULL, 0);
}

/* The next lobby message, if one has come (without waiting): 1 = one in
 * out (its size in *n), 0 = none yet, -1 = the link is gone. Other messages
 * are dropped. */
int gz_rbn_lobby_recv(void *out, uint32_t max, uint32_t *n)
{
    for (;;) {
        fd_set rd;
        struct timeval tv = { 0, 0 };
        uint32_t type, len;
        uint8_t *m;
        if (s_tcp == INVALID_SOCKET) return -1;
        FD_ZERO(&rd);
        FD_SET(s_tcp, &rd);
        if (select(0, &rd, NULL, NULL, &tv) <= 0) return 0;
        m = msg_recv(&type, &len);
        if (!m) return -1;
        if (type == M_LOBBY) {
            *n = len < max ? len : max;
            memcpy(out, m, *n);
            free(m);
            return 1;
        }
        free(m);
    }
}

/* The host's lobby is full: anyone else who connects is turned away at
 * once (their game then says so). Called every frame while linked. */
void gz_rbn_lobby_refuse_extra(void)
{
    fd_set rd;
    struct timeval tv = { 0, 0 };
    if (s_listen == INVALID_SOCKET) return;
    FD_ZERO(&rd);
    FD_SET(s_listen, &rd);
    if (select(0, &rd, NULL, NULL, &tv) > 0) {
        SOCKET x = accept(s_listen, NULL, NULL);
        if (x != INVALID_SOCKET) {
            fprintf(stderr, "[RBN] lobby: someone else tried to join; the lobby is full\n");
            closesocket(x);
        }
    }
}

/* Before the game starts again (gz_lobby_restart): every socket closed, so
 * the next run can take the port. */
void gz_rbn_close_all(void)
{
    if (s_tcp != INVALID_SOCKET) { shutdown(s_tcp, SD_BOTH); closesocket(s_tcp); s_tcp = INVALID_SOCKET; }
    if (s_udp != INVALID_SOCKET) { closesocket(s_udp); s_udp = INVALID_SOCKET; }
    if (s_listen != INVALID_SOCKET) { closesocket(s_listen); s_listen = INVALID_SOCKET; }
}

/* The lobby is done: the match goes on over this link. */
void gz_rbn_lobby_done(void)
{
    if (s_listen != INVALID_SOCKET) { closesocket(s_listen); s_listen = INVALID_SOCKET; }
    s_link_done = 1;
}

/* A match started from the lobby (GZ_RB_LOBBY_MATCH=1): when it ends the
 * game goes back to the lobby (gz_lobby.c). */
static int lobby_match(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("GZ_RB_LOBBY_MATCH"); v = e && *e == '1'; }
    return v && s_on > 0;
}
void gz_lobby_restart(int to_lobby, const char *notice);   /* gz_lobby.c */

/* The other player is gone mid-match: the host goes back to its lobby to
 * wait for the next, the client back to the game. */
static void lobby_match_lost(void)
{
    if (s_side == 1) gz_lobby_restart(1, "Player 2 left.");
    else gz_lobby_restart(0, "The host left.");
}

/* The match is over: the game's menus are back (their camera is set up
 * again after the fight's: after the results, or a quit from the pause
 * menu, which has no camera of its own). Both PCs see it at the same tick
 * and go back to the lobby together a moment later. */
int gz_wide_fight_active(void);                 /* recomp_manual.c */
static int64_t s_back_at = -1;
static int     s_fight_seen;
static void lobby_match_menus(uint32_t cur)
{
    static int menus_back;
    if (!lobby_match()) return;
    /* (the menus after the results set up a camera of the match's kind
     * again, so "the fight is on" is only looked at until they are seen) */
    if (gz_wide_fight_active()) s_fight_seen = 1;
    else if (s_fight_seen && !menus_back) {
        menus_back = 1;
        if (s_back_at < 0 || s_back_at > (int64_t)cur + 60) s_back_at = (int64_t)cur + 60;
        fprintf(stderr, "[RBN] lobby match: the menus are back at tick %u; back to the lobby\n", cur);
    }
    if (s_back_at >= 0 && (int64_t)cur >= s_back_at) gz_lobby_restart(1, NULL);
}

/* GameResults' victory (recomp_manual.c): if no menus follow in 20 s, go
 * back anyway. */
void gz_rbn_match_over(void)
{
    if (lobby_match() && s_back_at < 0) {
        s_back_at = (int64_t)gz_rb_cur_tick() + 1200;
        fprintf(stderr, "[RBN] lobby match: a winner; back to the lobby by tick %lld\n", (long long)s_back_at);
    }
}

/* ── the match setup (before the quick fight launches) ──────────────── */

/* The host's GameInfo for both: the host sends it, the client takes it.
 * Called with the setup about to launch; 0 = netplay could not start (the
 * match then plays locally). */
int gz_rbn_launch_sync(uint8_t *gameinfo, uint32_t size)
{
    uint32_t type, len;
    uint8_t *m;
    if (!gz_rbn_on()) return 1;
    if (!connect_peer()) { if (lobby_match()) lobby_match_lost(); s_on = 0; return 0; }
    if (s_side == 1 && !s_hello_taken) {
        m = msg_recv(&type, &len);              /* the hello: its UDP port */
        if (!m || type != M_HELLO || len < 8 || ((uint32_t *)m)[0] != MAGIC) {
            fprintf(stderr, "[RBN] bad hello\n");
            free(m);
            s_on = 0;
            return 0;
        }
        s_peer.sin_port = htons((uint16_t)((uint32_t *)m)[1]);
        free(m);
    }
    if (s_side == 1) {
        if (!msg_send(M_SETUP, gameinfo, size, NULL, 0)) { s_on = 0; return 0; }
        fprintf(stderr, "[RBN] match setup sent\n");
    } else {
        m = msg_recv(&type, &len);
        if (!m || type != M_SETUP || len != size) {
            fprintf(stderr, "[RBN] no match setup from the host\n");
            free(m);
            s_on = 0;
            return 0;
        }
        memcpy(gameinfo, m, size);
        free(m);
        fprintf(stderr, "[RBN] match setup received\n");
    }
    return 1;
}

/* ── tick 0: the host's memory for both ─────────────────────────────── */

static uint64_t page_hash(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)p;
    uint64_t h = 0x9E3779B97F4A7C15ull;
    int i;
    for (i = 0; i < 4096 / 8; i++) {
        h ^= q[i];
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    return h;
}

/* Called at the boundary that becomes tick 0, before history starts, with
 * the workers parked. */
int gz_rbn_tick0_sync(void)
{
    int n = gz_rb_page_count(), i;
    uint64_t *hash = (uint64_t *)calloc((size_t)n, 8);
    uint32_t type, len;
    uint8_t *m;
    double t0 = now_s();
    if (!hash) return 0;
    gz_rb_park_workers();
    /* the host: the frames' clock from here, for both (in the pages hashed
     * below and the devices' state sent after them) */
    if (s_side == 1) xbox_DevicesStart();
    for (i = 0; i < n; i++) {
        const uint8_t *p = gz_rb_shareable_page(i);
        hash[i] = p ? page_hash(p) : 0;
    }
    if (s_side == 2) {
        uint32_t k = 0;
        int took = 0;
        msg_send(M_HASHES, hash, (uint32_t)n * 8u, NULL, 0);
        m = msg_recv(&type, &len);
        if (!m || type != M_STATE || len < 12) { free(m); free(hash); return 0; }
        {
            uint64_t vb;
            uint32_t cnt, j;
            const uint8_t *q = m + 12;
            memcpy(&vb, m, 8);
            memcpy(&cnt, m + 8, 4);
            if (cnt == REFUSED) {
                fprintf(stderr, "[RBN] tick 0: the host refused: the two copies are too different "
                        "(different versions, menu mods or game files?). Playing locally.\n");
                free(m);
                free(hash);
                return -1;
            }
            const uint8_t *pages = q;
            /* The host-side states (the heap's tables, the devices, the
             * sound chip, the GPU), after the pages in the message but taken
             * first: the pages were laid out by the host's heap and run
             * beside its devices, and with this side's own a client that had
             * booted a little differently ran on garbage function pointers
             * from its first frame. First, so that nothing of this side's
             * (its KeTickCount keeper, say) writes over a page once taken. */
            q = pages + (size_t)cnt * (4 + 4096);
            if (q > m + len) q = m + len;
            while (q + 8 <= m + len) {
                uint32_t tag, hs;
                int s;
                memcpy(&tag, q, 4);
                memcpy(&hs, q + 4, 4);
                if (q + 8 + hs > m + len) break;
                for (s = 0; s < ST_COUNT; s++)
                    if (tag == s_st_tag[s]) {
                        if (hs == (uint32_t)st_size(s)) { st_load(s, q + 8); took |= 1 << s; }
                        else fprintf(stderr, "[RBN] tick 0: the host's %s is not this build's size\n", s_st_name[s]);
                    }
                q += 8 + hs;
            }
            for (j = 0, q = pages; j < cnt && q + 4 + 4096 <= m + len; j++, q += 4 + 4096) {
                uint32_t pi;
                uint8_t *dst;
                memcpy(&pi, q, 4);
                if ((dst = gz_rb_page_for_write((int)pi)) != NULL) { memcpy(dst, q + 4, 4096); k++; }
            }
            gz_rb_set_vbase(vb);
        }
        free(m);
        fprintf(stderr, "[RBN] tick 0: %u pages and the host's %s%s%s%s taken (%.2f s)\n", k,
                took & 1 ? "heap" : "-", took & 2 ? ", devices" : ", -", took & 4 ? ", sound chip" : ", -",
                took & 8 ? ", GPU" : ", -", now_s() - t0);
    } else {
        uint8_t *out;
        uint32_t cnt = 0, j, heap_bytes = 0;
        uint64_t vb = gz_rb_tick0_clock();
        m = msg_recv(&type, &len);
        if (!m || type != M_HASHES || len != (uint32_t)n * 8u) { free(m); free(hash); return 0; }
        for (i = 0; i < n; i++)
            if (hash[i] && ((uint64_t *)m)[i] != hash[i]) cnt++;
        if (cnt > MAX_TRANSFER) {
            /* More than a match's worth of clocks and counters: the two
             * loaded the match differently, and pages alone cannot make
             * them agree (the heap and file handles behind them differ). */
            uint8_t no[12];
            uint32_t r = REFUSED;
            memcpy(no, &vb, 8);
            memcpy(no + 8, &r, 4);
            msg_send(M_STATE, no, 12, NULL, 0);
            fprintf(stderr, "[RBN] tick 0: %u pages differ from player 2 (at most %u can be sent): "
                    "the two copies are too different. Playing locally.\n", cnt, MAX_TRANSFER);
            free(m);
            free(hash);
            return -1;
        }
        {
            size_t extra = 0;
            int s;
            for (s = 0; s < ST_COUNT; s++) extra += 8 + st_size(s);
            out = (uint8_t *)malloc(12 + (size_t)cnt * (4 + 4096) + extra);
            if (!out) { free(m); free(hash); return 0; }
            {
                uint8_t *hp = out + 12 + (size_t)cnt * 4100;
                for (s = 0; s < ST_COUNT; s++) {
                    uint32_t h32 = (uint32_t)st_size(s);
                    if (!st_save(s, hp + 8)) continue;
                    memcpy(hp, &s_st_tag[s], 4);
                    memcpy(hp + 4, &h32, 4);
                    hp += 8 + h32;
                }
                heap_bytes = (uint32_t)(hp - (out + 12 + (size_t)cnt * 4100));
            }
        }
        memcpy(out, &vb, 8);
        memcpy(out + 8, &cnt, 4);
        for (i = 0, j = 0; i < n; i++)
            if (hash[i] && ((uint64_t *)m)[i] != hash[i]) {
                uint32_t pi = (uint32_t)i;
                memcpy(out + 12 + (size_t)j * 4100, &pi, 4);
                memcpy(out + 12 + (size_t)j * 4100 + 4, gz_rb_shareable_page(i), 4096);
                j++;
            }
        msg_send(M_STATE, out, 12 + cnt * 4100u + heap_bytes, NULL, 0);
        free(out);
        free(m);
        gz_rb_set_vbase(vb);
        fprintf(stderr, "[RBN] tick 0: %u pages and the heap, devices, sound chip and GPU sent to player 2 (%.2f s)\n",
                cnt, now_s() - t0);
    }
    free(hash);
    return 1;
}

/* ── pads over UDP ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t magic;
    uint32_t tick;                      /* the sender's tick now */
    int32_t  ack;                       /* the last of our pads it has */
    uint32_t first;                     /* the tick of pads[0] */
    uint32_t n;
    uint32_t dtick;                     /* the desync check: a final tick + 1 (0 = none) */
    uint32_t dig[32];                   /* and the sender's digest of it (16 x 64 bits) */
    uint8_t  pads[MAXSEND][PAD];
} Packet;

/* Delayed sends (GZ_RB_LAG_MS). */
static struct { double at; int len; Packet p; } s_lagq[256];
static unsigned s_lag_head, s_lag_tail;

static void udp_out(const Packet *p, int len)
{
    sendto(s_udp, (const char *)p, len, 0, (struct sockaddr *)&s_peer, sizeof s_peer);
}

static void lag_flush(void)
{
    double t = now_s();
    while (s_lag_tail != s_lag_head && s_lagq[s_lag_tail % 256].at <= t) {
        udp_out(&s_lagq[s_lag_tail % 256].p, s_lagq[s_lag_tail % 256].len);
        s_lag_tail++;
    }
}

static void send_pads(uint32_t tick)
{
    Packet p;
    int64_t first = s_peer_acked + 1, last = s_local_max, t;
    if (first < last - (int64_t)MAXSEND + 1) first = last - (int64_t)MAXSEND + 1;
    if (first < 0) first = 0;
    p.magic = MAGIC;
    p.tick = tick;
    p.ack = (int32_t)s_remote_max;
    p.first = (uint32_t)first;
    p.n = last >= first ? (uint32_t)(last - first + 1) : 0;
    for (t = first; t <= last; t++)
        memcpy(p.pads[t - first], s_local[t % RING], PAD);
    {
        uint64_t h[16];
        p.dtick = 0;
        memset(p.dig, 0, sizeof p.dig);
        if (s_final >= 0 && gz_rb_tick_digest((uint32_t)s_final, h)) {
            p.dtick = (uint32_t)s_final + 1;
            memcpy(p.dig, h, sizeof p.dig);
        }
    }
    {
        int len = (int)(offsetof(Packet, pads) + p.n * PAD);
        if (s_lag_ms > 0 && s_lag_head - s_lag_tail < 256) {
            s_lagq[s_lag_head % 256].at = now_s() + s_lag_ms / 1000.0;
            s_lagq[s_lag_head % 256].len = len;
            s_lagq[s_lag_head % 256].p = p;
            s_lag_head++;
        } else
            udp_out(&p, len);
    }
}

/* Take in every packet waiting. A remote pad for a tick the game already ran
 * on a different guess marks that tick for a rollback. */
static void recv_pads(uint32_t cur)
{
    Packet p;
    for (;;) {
        int k = recv(s_udp, (char *)&p, sizeof p, 0), i;
        if (k < (int)offsetof(Packet, pads) || p.magic != MAGIC) {
            if (k < 0) break;
            continue;
        }
        if (p.n > MAXSEND || k < (int)(offsetof(Packet, pads) + p.n * PAD)) continue;
        if (p.ack > s_peer_acked) s_peer_acked = p.ack;
        if (p.tick >= s_peer_tick) { s_peer_tick = p.tick; s_peer_tick_at = now_s(); }
        if (p.dtick) {
            unsigned j;
            for (j = 0; j < s_rdig_n && s_rdig[j].tick != p.dtick - 1; j++) {}
            if (j == s_rdig_n && s_rdig_n < 64) {
                s_rdig[j].tick = p.dtick - 1;
                memcpy(s_rdig[j].h, p.dig, sizeof s_rdig[j].h);
                s_rdig_n++;
            }
        }
        for (i = 0; i < (int)p.n; i++) {
            int64_t t = (int64_t)p.first + i;
            if (t != s_remote_max + 1) continue;    /* in order only; the rest comes again */
            memcpy(s_remote[t % RING], p.pads[i], PAD);
            s_remote_max = t;
            if (t < (int64_t)cur && s_used_pred[t % RING]
                && memcmp(s_used[t % RING], p.pads[i], PAD) != 0
                && (s_first_bad < 0 || t < s_first_bad))
                s_first_bad = t;
        }
    }
}

/* The pad of `port` for `tick` (from the game's pad poll, gz_rb_pad). */
int gz_rbn_pad(int port, uint32_t tick, uint8_t *pad18)
{
    uint32_t r = tick % RING;
    if (port == gz_rbn_local_port()) {
        if ((int64_t)tick <= s_local_max) memcpy(pad18, s_local[r], PAD);
        else memset(pad18, 0, PAD);
        return 1;
    }
    if (port != 1 - gz_rbn_local_port()) return 0;
    if ((int64_t)tick <= s_remote_max) {
        memcpy(pad18, s_remote[r], PAD);
        s_used_pred[r] = 0;
    } else {
        /* a guess: the last pad the remote player had */
        if (s_remote_max >= 0) memcpy(pad18, s_remote[s_remote_max % RING], PAD);
        else memset(pad18, 0, PAD);
        s_used_pred[r] = 1;
    }
    memcpy(s_used[r], pad18, PAD);
    return 1;
}

/* This side's pad for a tick: the PC's controller (first pad), or the
 * script's when testing. */
static void read_local(uint32_t tick, uint8_t *pad18)
{
    if (tv_pad(s_side, tick, pad18)) return;
    if (s_script) {
        gz_rb_script_pad(gz_rbn_local_port(), tick, pad18);
        return;
    }
    {
        uint8_t rep[20];
        if (usb_gamepad_report(0, rep, sizeof rep) >= 20) memcpy(pad18, rep + 2, PAD);
        else memset(pad18, 0, PAD);
    }
}

/* ── per tick (the boundary that opens tick `cur`, not while replaying) ─ */

/* Returns how many ticks to go back (0: none). */
uint32_t gz_rbn_tick(uint32_t cur)
{
    double t0;
    static double last_report;
    lag_flush();
    lobby_match_menus(cur);
    /* this side's pads up to cur + delay. A session's menus wait for both
     * pads every tick (no guesses there), so they get a longer delay
     * (GZ_RB_MENU_DELAY, default 6 ticks, 100 ms): with the fight's 2 they
     * stalled on every tick a packet was late. (6 used to give the two PCs
     * different fights after the first rollbacks; the cause was the devices
     * running on each PC's own clock, see xbox_DevicesLock.) */
    {
        static int menu_delay = -1;
        int delay = s_delay;
        if (menu_delay < 0) {
            const char *v = getenv("GZ_RB_MENU_DELAY");
            menu_delay = v && *v ? atoi(v) : 6;
            if (menu_delay < s_delay) menu_delay = s_delay;
            if (menu_delay > 30) menu_delay = 30;
        }
        if (gz_rbn_session() && !s_fight) delay = menu_delay;
        while (s_local_max < (int64_t)cur + delay) {
            int64_t t = s_local_max + 1;
            read_local((uint32_t)t, s_local[t % RING]);
            s_local_max = t;
        }
    }
    while (s_local_max < (int64_t)cur + s_delay) {
        int64_t t = s_local_max + 1;
        read_local((uint32_t)t, s_local[t % RING]);
        s_local_max = t;
    }
    send_pads(cur);
    recv_pads(cur);
    {
        /* on the way to the Versus screen: the other side's pads are known
         * here (tv_pad), so there is nothing to wait for */
        while (s_remote_max < (int64_t)cur) {
            int64_t t = s_remote_max + 1;
            if (!tv_pad(3 - s_side, (uint32_t)t, s_remote[t % RING])) break;
            s_remote_max = t;
        }
    }

    /* Too far ahead of what the other side has confirmed: wait for it. Out
     * of a fight (a session's menus, loading) nothing runs on a guess: a
     * level load's file reads cannot be played again (the host file has
     * moved on), and a rollback across one gave the two PCs different
     * levels. Those ticks wait for the other side's pad instead. */
    t0 = now_s();
    while ((int64_t)cur - s_remote_max > (gz_rbn_session() && !s_fight ? 0 : s_window)) {
        Sleep(1);
        lag_flush();
        recv_pads(cur);
        if (now_s() - t0 > 0.25) send_pads(cur);     /* in case ours were lost */
        /* (out of a fight, a minute: the other PC may still be on its way
         * to the Versus screen, or loading) */
        if (now_s() - t0 > (s_fight ? 10.0 : 60.0)) {
            fprintf(stderr, "[RBN] nothing from the other side for %d s: playing on predictions\n", s_fight ? 10 : 60);
            if (lobby_match()) lobby_match_lost();
            s_remote_max = (int64_t)cur;            /* (the match is effectively over) */
            break;
        }
    }
    s_stat_stall_ms += (unsigned)((now_s() - t0) * 1000.0);

    /* Time sync: ahead of the other side (its tick now, allowing for the
     * time since its packet) by more than a tick on average: give back a
     * little time, so both sides run on predictions equally. */
    if (s_peer_tick_at > 0.0 && !gz_rbn_hide_picture()) {
        double peer_now = (double)s_peer_tick + (now_s() - s_peer_tick_at) * 60.0;
        double ahead = (double)cur - peer_now;
        s_ahead_avg = s_ahead_avg * 0.95 + ahead * 0.05;
        if (s_ahead_avg > 1.0) Sleep(s_ahead_avg > 3.0 ? 8 : 3);
    }

    /* The desync check. Final here: a tick that ran (before cur) on the
     * other side's real pad (up to s_remote_max), and not one a rollback
     * about to happen plays again (from s_first_bad). */
    {
        int64_t f = (int64_t)cur - 1;
        unsigned j = 0;
        if (f > s_remote_max) f = s_remote_max;
        if (s_first_bad >= 0 && f > s_first_bad - 1) f = s_first_bad - 1;
        if (f > s_final) s_final = f;
        while (j < s_rdig_n) {
            uint64_t ours[16];
            if ((int64_t)s_rdig[j].tick > s_final) { j++; continue; }
            /* (a lobby's match is over: what the menus after it do on each
             * PC, saving the profile and so on, is not the match) */
            if (lobby_match() && s_back_at >= 0) { s_rdig[j] = s_rdig[--s_rdig_n]; continue; }
            if (gz_rb_tick_digest(s_rdig[j].tick, ours)) {
                int k, parts = 0;
                for (k = 0; k < 16; k++) parts += ours[k] != s_rdig[j].h[k];
                s_stat_checked++;
                if (parts && parts < DESYNC_PARTS) {
                    static int said;
                    s_stat_small++;
                    if (!said) {
                        said = 1;
                        fprintf(stderr, "[RBN] tick %u: %d of 16 digest parts differ from the other PC "
                                "(a leftover, probably; a desync makes most of them differ)\n", s_rdig[j].tick, parts);
                    }
                } else if (parts) {
                    /* Kept on after the first, a few times 5 s apart: the
                     * pages written out show how it spreads. */
                    static uint32_t last_dump;
                    static int dumps;
                    s_stat_bad++;
                    if (!s_desynced || (dumps < 4 && s_rdig[j].tick >= last_dump + 300u)) {
                        char path[64];
                        int n;
                        snprintf(path, sizeof path, "rb_desync_p%d_%u.bin", s_side, dumps);
                        n = gz_rb_write_digests(s_rdig[j].tick, path);
                        fprintf(stderr, "[RBN] DESYNC: tick %u played differently on the two PCs (%d of 16 digest parts differ); "
                                "the %d ticks up to it are in %s (send them with the other PC's)\n",
                                s_rdig[j].tick, parts, n, path);
                        fflush(stderr);
                        last_dump = s_rdig[j].tick;
                        dumps++;
                    }
                    s_desynced = 1;
                }
            }
            s_rdig[j] = s_rdig[--s_rdig_n];     /* checked (or no longer kept) */
        }
    }

    if (now_s() - last_report > 10.0) {
        last_report = now_s();
        fprintf(stderr, "[RBN] tick %u: confirmed to %lld, %u rollbacks (%u ticks replayed), stalled %u ms, ahead %.1f, "
                "%u ticks checked against the other PC: %u alike, %u with a leftover, %u DESYNCED\n",
                cur, (long long)s_remote_max, s_stat_rb, s_stat_rb_ticks, s_stat_stall_ms, s_ahead_avg,
                s_stat_checked, s_stat_checked - s_stat_small - s_stat_bad, s_stat_small, s_stat_bad);
    }

    if (s_first_bad >= 0) {
        uint32_t back = cur - (uint32_t)s_first_bad;
        s_first_bad = -1;
        s_stat_rb++;
        if (getenv("GZ_RB_LOG_ROLLBACKS"))
            fprintf(stderr, "[RBN] rollback at tick %u: back %u to %u\n", cur, back, (uint32_t)(cur - back));
        s_stat_rb_ticks += back;
        return back;
    }
    return 0;
}

void gz_rbn_flush(void) { if (s_on > 0) lag_flush(); }

/* Netplay off for the rest of the run (tick 0 could not be agreed). */
void gz_rbn_off(void) { s_on = 0; }

/* ── Rollback Netplay from the Multi-Player menu ────────────────────────
 *
 * The entry that was Xbox Live (recomp_manual.c, sub_0009B0C0) calls this.
 * After the prompt the game starts again as a netplay session: the two PCs
 * connect at boot, both take the host's profile (gz_rbn_session_boot), and
 * from the first frame they run on the same inputs, rollback included: the
 * host's pad is player 1 and drives the menus (rules, city), the client's is
 * player 2, and each picks a monster. Two games that each went their own way
 * through the menus are too far apart in memory to agree on a match, so the
 * session starts both from the same point. */
void gz_rbn_menu_pick(void)
{
    char ip[128], peer[160], base[MAX_PATH], save[MAX_PATH + 16], exe[MAX_PATH];
    const char *qf = getenv("GZ_QF_FILE"), *sd = getenv("GZ_SAVE_DIR");
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int r = gz_rbn_prompt(ip, sizeof ip);
    (void)qf;
    if (!r) return;
    if (sd && *sd) snprintf(base, sizeof base, "%s", sd);
    else {
        const char *la = getenv("LOCALAPPDATA");
        snprintf(base, sizeof base, "%s\\xboxrecomp", la ? la : ".");
    }
    snprintf(save, sizeof save, "%s\\netplay", base);
    CreateDirectoryA(save, NULL);
    snprintf(peer, sizeof peer, "%s%s", ip, strchr(ip, ':') ? "" : ":7790");

    /* the session's settings, for the new process (this run's own first,
     * for going back to the game from the lobby: gz_lobby.c) */
    {
        extern void gz_lobby_keep_env(void);
        gz_lobby_keep_env();
    }
    SetEnvironmentVariableA("GZ_RB_NET", "1");
    SetEnvironmentVariableA("GZ_RB_SIDE", r == 1 ? "1" : "2");
    SetEnvironmentVariableA("GZ_RB_PEER", r == 2 ? peer : NULL);
    SetEnvironmentVariableA("GZ_SAVE_DIR", save);
    {
        /* The game's own menus played together (a session), opened
         * straight on the Versus screen; or with GZ_RB_LOBBY_UI=1 the
         * lobby (gz_lobby.c), whose matches start the game again each time. */
        const char *lv = getenv("GZ_RB_LOBBY_UI");
        int menus = !(lv && *lv == '1');
        SetEnvironmentVariableA("GZ_RB_SESSION", menus ? "1" : NULL);
        SetEnvironmentVariableA("GZ_RB_TO_VERSUS", menus ? "1" : NULL);
        SetEnvironmentVariableA("GZ_RB_LOBBY", menus ? NULL : "1");
        {
            extern void gz_lobby_keep_window_pos(void);
            gz_lobby_keep_window_pos();
        }
    }
    SetEnvironmentVariableA("GZ_RB_PROFILE_FROM", base);    /* the host's profile, for both */
    SetEnvironmentVariableA("GZ_QUICK_FIGHT", NULL);
    SetEnvironmentVariableA("GZ_MOVIES", "0");
    SetEnvironmentVariableA("GZ_RATE", "60");
    SetEnvironmentVariableA("GZ_TOURNAMENT", "1");
    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) return;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;          /* the same log */
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fprintf(stderr, "[RBN] Rollback Netplay: %s; starting again in the netplay %s\n",
            r == 1 ? "hosting" : peer, getenv("GZ_RB_LOBBY") ? "lobby" : "session");
    fflush(stderr);
    if (!CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxA(GetForegroundWindow(), "The game could not be started again for netplay.", "Rollback Netplay", MB_ICONWARNING);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    TerminateProcess(GetCurrentProcess(), 0);
}

/* ── the "Rollback Netplay" prompt (Multi-Player menu) ─────────────────
 *
 * A small window over the game: the friend's IP (the last one used is in
 * gz_netplay.txt next to the program), Host a match / Join / Cancel. */

#define ID_IP     101
#define ID_HOST   102
#define ID_JOIN   103
#define ID_CANCEL 104

static int  s_prompt_result;
static char s_prompt_ip[128];

static LRESULT CALLBACK prompt_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_HOST || LOWORD(wp) == ID_JOIN) {
            GetDlgItemTextA(w, ID_IP, s_prompt_ip, sizeof s_prompt_ip);
            if (LOWORD(wp) == ID_JOIN && !s_prompt_ip[0]) {
                MessageBoxA(w, "Type your friend's IP address to join their match.", "Rollback Netplay", MB_ICONINFORMATION);
                return 0;
            }
            s_prompt_result = LOWORD(wp) == ID_HOST ? 1 : 2;
            DestroyWindow(w);
            return 0;
        }
        if (LOWORD(wp) == ID_CANCEL) { s_prompt_result = 0; DestroyWindow(w); return 0; }
        break;
    case WM_CLOSE:
        s_prompt_result = 0;
        DestroyWindow(w);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

/* 0 = cancelled, 1 = host, 2 = join (ip filled in). */
int gz_rbn_prompt(char *ip, size_t n)
{
    static int registered;
    HINSTANCE hi = GetModuleHandleA(NULL);
    HWND owner = GetForegroundWindow(), w;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    MSG m;
    RECT r;
    int x, y, cw = 380, ch = 190;
    FILE *f;
    s_prompt_ip[0] = 0;
    if ((f = fopen("gz_netplay.txt", "r")) != NULL) {
        if (fgets(s_prompt_ip, sizeof s_prompt_ip, f)) s_prompt_ip[strcspn(s_prompt_ip, "\r\n")] = 0;
        fclose(f);
    }
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "GzRollbackPrompt";
        RegisterClassA(&wc);
        registered = 1;
    }
    if (!owner || !GetWindowRect(owner, &r)) SetRect(&r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    x = (r.left + r.right - cw) / 2;
    y = (r.top + r.bottom - ch) / 2;
    /* No owner: the game's window belongs to its render thread, and an owned
     * window shares that thread's input queue, which can stall the prompt
     * while the render thread waits on this one. */
    w = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, "GzRollbackPrompt", "Rollback Netplay",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, cw, ch, NULL, NULL, hi, NULL);
    if (!w) return 0;
#define GZ_CTL(cls, text, style, cx, cy, cwid, chgt, id) \
    SendMessageA(CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | (style), cx, cy, cwid, chgt, w, \
                                 (HMENU)(INT_PTR)(id), hi, NULL), WM_SETFONT, (WPARAM)font, TRUE)
    GZ_CTL("STATIC", "Host a match and give your friend your IP address, or type\r\n"
                     "your friend's IP address and join theirs. The host picks the\r\n"
                     "rules and the city. Port 7790 (TCP and UDP) must reach the host.",
           0, 14, 10, 350, 50, 0);
    GZ_CTL("STATIC", "Friend's IP:", 0, 14, 72, 80, 20, 0);
    GZ_CTL("EDIT", s_prompt_ip, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 96, 70, 260, 22, ID_IP);
    GZ_CTL("BUTTON", "Host a match", WS_TABSTOP | BS_PUSHBUTTON, 14, 112, 110, 28, ID_HOST);
    GZ_CTL("BUTTON", "Join", WS_TABSTOP | BS_DEFPUSHBUTTON, 134, 112, 110, 28, ID_JOIN);
    GZ_CTL("BUTTON", "Cancel", WS_TABSTOP | BS_PUSHBUTTON, 254, 112, 102, 28, ID_CANCEL);
#undef GZ_CTL
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetFocus(GetDlgItem(w, ID_IP));
    s_prompt_result = 0;
    while (IsWindow(w) && GetMessageA(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    if (s_prompt_result == 2 && (f = fopen("gz_netplay.txt", "w")) != NULL) {
        fprintf(f, "%s\n", s_prompt_ip);
        fclose(f);
    }
    if (ip && n) {
        strncpy(ip, s_prompt_ip, n - 1);
        ip[n - 1] = 0;
    }
    return s_prompt_result;
}
