/*
 * gz_lobby.c - the Rollback Netplay lobby.
 *
 * Multi-Player > Rollback Netplay > Host / Join starts the game again in the
 * lobby (GZ_RB_LOBBY=1, gz_rbnet.c gz_rbn_menu_pick). The game does not boot
 * yet: the lobby draws its own picture in the game's window (GDI into a
 * bitmap, shown by gpu_d3d11.c) and makes the link to the other PC
 * (gz_rbnet.c gz_rbn_lobby_link). The host waits for a player, the other
 * joins; each picks a monster on their own controller, the host picks the
 * city and the rules, both press Start to be ready, and the host's Start
 * then counts down from 3 on both screens. The game boots straight into
 * that match (a netplay quick fight, recomp_manual.c gz_quick_fight_tick),
 * over the same link. When the match is over both PCs start again in the
 * lobby with the same picks (gz_lobby_restart, from gz_rbnet.c).
 *
 * Leaving: B (or the Leave row) asks first, then the game starts again as
 * it was before netplay (the settings kept by gz_lobby_keep_env). When the
 * host leaves, player 2 goes back to the game; when player 2 leaves, the
 * host's lobby waits for the next player. Ten seconds without a word from
 * the other side counts as leaving.
 *
 * Everything is unlocked in netplay (tournament mode), so the lobby offers
 * the whole roster and every city. Of the Versus rules, the match setup is
 * known to hold the rounds (+0x128) and the round time (+0x120); those are
 * the two the host sets here.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gz_lobby_template.h"

/* gz_rbnet.c */
int  gz_rbn_on(void);
int  gz_rbn_local_port(void);
int  gz_rbn_lobby_link(double timeout_s, const char **why);
void gz_rbn_lobby_drop(void);
int  gz_rbn_lobby_send(const void *p, uint32_t n);
int  gz_rbn_lobby_recv(void *out, uint32_t max, uint32_t *n);
void gz_rbn_lobby_done(void);
/* gpu_d3d11.c */
int  gz_gpu_lobby_open(void);
void gz_gpu_lobby_present(const uint32_t *bgra, int w, int h);
void gz_gpu_lobby_close(void);
HWND gz_gpu_window(void);
int  gz_gpu_window_pos(int *x, int *y);
/* usb_gamepad.c, gz_launch.c */
int  usb_gamepad_report(int pad, uint8_t *out, int max);
const char *gz_launch_game_dir(void);

/* ── the roster, the cities, the rules ───────────────────────────────── */

static const struct { const char *id, *name; } k_mon[] = {
    { "godzilla2k", "Godzilla 2000" },   { "godzilla90s", "Godzilla 90s" },
    { "anguirus", "Anguirus" },          { "rodan", "Rodan" },
    { "megalon", "Megalon" },            { "gigan", "Gigan" },
    { "jetjaguar", "Jet Jaguar" },       { "ghidorah", "King Ghidorah" },
    { "mghidorah", "Mecha-King Ghidorah" }, { "mechagodzilla2", "Mechagodzilla 2" },
    { "mechagodzilla3", "Mechagodzilla 3" }, { "megaguirus", "Megaguirus" },
    { "moguera2", "Moguera" },           { "orga", "Orga" },
    { "destoroyah", "Destoroyah" },      { "baragon", "Baragon" },
    { "spacegodzilla", "SpaceGodzilla" }, { "mothra_larvae", "Mothra" },
    { "biollante", "Biollante" },
};
#define NMON   ((int)(sizeof k_mon / sizeof k_mon[0]))
#define MON_RANDOM NMON                 /* the grid's last cell */
#define GRID_COLS 5
#define GRID_ROWS 4

static const struct { const char *id, *name; } k_city[] = {
    { "New_Tokyo_Main_DAY", "Tokyo" },        { "New_Osaka_Main_DAY", "Osaka" },
    { "New_Seattle_Main_DAY", "Seattle" },    { "New_SF_Main_DAY", "San Francisco" },
    { "New_LA_Main_DAY", "Los Angeles" },     { "New_London_Main_DAY", "London" },
    { "New_Boston_Main_DAY", "Boston" },      { "NewYork_DAY", "New York" },
    { "MonsterIsland_DAY", "Monster Island" }, { "Boxing_Ring_PS2_DAY", "Boxing Ring" },
    { "New_Mothership", "Mothership" },
};
#define NCITY  ((int)(sizeof k_city / sizeof k_city[0]))
#define CITY_RANDOM NCITY

static const int k_rounds[] = { 1, 2, 3 };
#define NROUNDS ((int)(sizeof k_rounds / sizeof k_rounds[0]))
static const int k_time[] = { 60, 90, 120, 180, 300 };   /* seconds */
#define NTIME  ((int)(sizeof k_time / sizeof k_time[0]))

/* An environment variable for this process's C runtime (getenv) and for
 * Windows (the next process), together; NULL or "" removes it. */
static void env_set(const char *name, const char *val)
{
    SetEnvironmentVariableA(name, val && *val ? val : NULL);
    _putenv_s(name, val ? val : "");
}

/* ── keeping this run's settings, and starting again ─────────────────── */

/* What netplay changes in the environment, kept as GZ_RB_ORIG_<name>
 * ("=value", or "-" for unset) the first time, for going back. */
static const char *const k_env[] = {
    "GZ_RB_NET", "GZ_RB_SIDE", "GZ_RB_PEER", "GZ_SAVE_DIR", "GZ_RB_SESSION", "GZ_RB_LOBBY",
    "GZ_RB_PROFILE_FROM", "GZ_QUICK_FIGHT", "GZ_MOVIES", "GZ_RATE", "GZ_TOURNAMENT",
    "GZ_QF_P1", "GZ_QF_P2", "GZ_QF_CITY", "GZ_QF_ROUNDS", "GZ_QF_ROUND_TIME", "GZ_QF_DELAY",
    "GZ_QF_FILE", "GZ_QF_SEED", "GZ_RB_LOBBY_MATCH", "GZ_RB_LOBBY_LAST", "GZ_RB_TO_VERSUS",
};
#define NENV ((int)(sizeof k_env / sizeof k_env[0]))

void gz_lobby_keep_env(void)
{
    char name[64], val[1024];
    int k;
    if (GetEnvironmentVariableA("GZ_RB_ORIG_KEPT", val, sizeof val)) return;
    for (k = 0; k < NENV; k++) {
        DWORD n = GetEnvironmentVariableA(k_env[k], val + 1, sizeof val - 1);
        snprintf(name, sizeof name, "GZ_RB_ORIG_%s", k_env[k]);
        if (n && n < sizeof val - 1) { val[0] = '='; env_set(name, val); }
        else env_set(name, "-");
    }
    env_set("GZ_RB_ORIG_KEPT", "1");
}

static void restore_env(void)
{
    char name[64], val[1024];
    int k;
    for (k = 0; k < NENV; k++) {
        snprintf(name, sizeof name, "GZ_RB_ORIG_%s", k_env[k]);
        if (GetEnvironmentVariableA(name, val, sizeof val) && val[0] == '=')
            env_set(k_env[k], val + 1);
        else
            env_set(k_env[k], NULL);
        env_set(name, NULL);
    }
    env_set("GZ_RB_ORIG_KEPT", NULL);
}

/* The window's place, for the next run (GZ_WIN_POS, gpu_d3d11.c). */
void gz_lobby_keep_window_pos(void)
{
    int x, y;
    char v[32];
    if (gz_gpu_window_pos(&x, &y)) {
        snprintf(v, sizeof v, "%d,%d", x, y);
        env_set("GZ_WIN_POS", v);
    }
}

/* Start the game again: in the lobby (to_lobby), or as it was before
 * netplay, with `notice` shown at its start. Does not return. */
void gz_lobby_restart(int to_lobby, const char *notice)
{
    char exe[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    static const char *const match_env[] = {
        "GZ_QUICK_FIGHT", "GZ_QF_P1", "GZ_QF_P2", "GZ_QF_CITY", "GZ_QF_ROUNDS",
        "GZ_QF_ROUND_TIME", "GZ_QF_DELAY", "GZ_QF_FILE", "GZ_QF_SEED", "GZ_RB_LOBBY_MATCH",
    };
    int k;
    gz_lobby_keep_window_pos();
    {
        extern void gz_rbn_close_all(void);     /* gz_rbnet.c: the port for the next run */
        gz_rbn_close_all();
    }
    if (to_lobby) {
        for (k = 0; k < (int)(sizeof match_env / sizeof match_env[0]); k++)
            env_set(match_env[k], NULL);
        env_set("GZ_RB_LOBBY", "1");
    } else
        restore_env();
    env_set("GZ_RB_NOTICE", notice && *notice ? notice : NULL);
    fprintf(stderr, "[LOBBY] starting again %s%s%s\n", to_lobby ? "in the lobby" : "out of netplay",
            notice ? ": " : "", notice ? notice : "");
    fflush(stderr);
    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) ExitProcess(1);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;          /* the same log */
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    TerminateProcess(GetCurrentProcess(), 0);
}

/* ── the picture (GDI into a bitmap) ─────────────────────────────────── */

#define W 1280
#define H 960
static HDC      s_dc;
static uint32_t *s_px;
static HFONT    s_font[5];                  /* 24, 30, 40, 56, 160 px */
enum { F_SMALL, F_TEXT, F_BIG, F_TITLE, F_HUGE };

static void gfx_init(void)
{
    BITMAPINFO bi;
    HBITMAP bmp;
    char path[MAX_PATH * 2];
    static const int size[5] = { 24, 30, 40, 56, 160 };
    int k;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;                 /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    s_dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void **)&s_px, NULL, 0);
    SelectObject(s_dc, bmp);
    SetBkMode(s_dc, TRANSPARENT);
    /* the game's own font (its disc has Arial Unicode), this run only */
    snprintf(path, sizeof path, "%s\\media\\ArialUni.Ttf", gz_launch_game_dir());
    AddFontResourceExA(path, FR_PRIVATE, NULL);
    for (k = 0; k < 5; k++)
        s_font[k] = CreateFontA(-size[k], 0, 0, 0, k >= F_BIG ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                                "Arial Unicode MS");
}

static void fill(int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(s_dc, &r, b);
    DeleteObject(b);
}

static void frame(int x, int y, int w, int h, int t, COLORREF c)
{
    fill(x, y, w, t, c);
    fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c);
    fill(x + w - t, y, t, h, c);
}

/* Text in a box: align 0 left, 1 centre, 2 right; vertically centred. */
static void text(int x, int y, int w, int h, int font, COLORREF c, int align, const char *s)
{
    RECT r = { x, y, x + w, y + h };
    wchar_t ws[256];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 256);
    SelectObject(s_dc, s_font[font]);
    SetTextColor(s_dc, c);
    DrawTextW(s_dc, ws, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS
              | (align == 1 ? DT_CENTER : align == 2 ? DT_RIGHT : DT_LEFT));
}

#define C_BG     RGB(14, 16, 26)
#define C_PANEL  RGB(30, 34, 50)
#define C_CELL   RGB(40, 46, 66)
#define C_DIM    RGB(140, 146, 170)
#define C_TEXT   RGB(236, 238, 246)
#define C_P1     RGB(236, 76, 64)
#define C_P2     RGB(70, 146, 246)
#define C_READY  RGB(84, 214, 120)
#define C_GOLD   RGB(246, 200, 80)

/* ── input: this PC's first controller and the keyboard ──────────────── */

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_START, K_N };

static void read_keys(int down[K_N])
{
    uint8_t rep[20];
    int fg = GetForegroundWindow() == gz_gpu_window();
    memset(down, 0, sizeof(int) * K_N);
    if (usb_gamepad_report(0, rep, sizeof rep) >= 20) {
        int16_t lx, ly;
        memcpy(&lx, rep + 12, 2);
        memcpy(&ly, rep + 14, 2);
        down[K_UP]    = (rep[2] & 0x01) || ly > 16000;
        down[K_DOWN]  = (rep[2] & 0x02) || ly < -16000;
        down[K_LEFT]  = (rep[2] & 0x04) || lx < -16000;
        down[K_RIGHT] = (rep[2] & 0x08) || lx > 16000;
        down[K_START] = (rep[2] & 0x10) != 0;
        down[K_A]     = rep[4] > 32;
        down[K_B]     = rep[5] > 32;
    }
    if (fg) {
        down[K_UP]    |= (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
        down[K_DOWN]  |= (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
        down[K_LEFT]  |= (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        down[K_RIGHT] |= (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        down[K_A]     |= (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        down[K_B]     |= (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        down[K_START] |= (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    }
}

/* Presses this frame: a direction held repeats after 300 ms, every 110 ms. */
static void poll_presses(int press[K_N])
{
    static int was[K_N];
    static DWORD next[K_N];
    int down[K_N], k;
    DWORD now = GetTickCount();
    read_keys(down);
    for (k = 0; k < K_N; k++) {
        press[k] = down[k] && !was[k];
        if (press[k]) next[k] = now + 300;
        else if (down[k] && k <= K_RIGHT && (LONG)(now - next[k]) >= 0) { press[k] = 1; next[k] = now + 110; }
        was[k] = down[k];
    }
}

/* ── the lobby's state ───────────────────────────────────────────────── */

/* One side's part (each side sends its own; the host's carries the city
 * and the rules). Rows: 0-3 the monster grid, then the host's City,
 * Rounds, Round time and Leave, or player 2's Leave. */
typedef struct {
    int32_t mon;                        /* picked: 0..NMON-1, MON_RANDOM */
    int32_t cur;                        /* grid cursor */
    int32_t row;                        /* 0-3 grid, 4.. below it */
    int32_t ready;
    int32_t city, rounds, time;         /* host: indices */
} Part;

enum { L_STATE = 1, L_START, L_ACK, L_LEAVE };
typedef struct {
    uint32_t kind;
    Part     part;
    int32_t  f_p1, f_p2, f_city, f_rounds, f_time;     /* L_START: the match */
} LMsg;

static Part s_me, s_them;
static int  s_is_host;                     /* this PC is the host */
static volatile LONG s_link;            /* 0 linking, 1 linked, 2 failed */
static const char *s_link_why;
static int  s_have_them;                /* player 2 is in (host) / linked (client) */
static DWORD s_last_rx, s_last_tx;
static int  s_confirm;                  /* "Leave the lobby?" */
static char s_notice[160];
static DWORD s_notice_until;
static int  s_notice_then;              /* after the notice: 0 stay, 1 out of netplay */
static int  s_counting;                 /* the countdown is on */
static DWORD s_count_t0;
static int  s_final[5];                 /* p1, p2, city, rounds, time */
static char s_host_ip[64];
static char s_peer_txt[160];

static DWORD WINAPI link_thread(LPVOID p)
{
    const char *why = "";
    (void)p;
    if (gz_rbn_lobby_link(s_is_host ? 1e12 : 20.0, &why)) InterlockedExchange(&s_link, 1);
    else { s_link_why = why; InterlockedExchange(&s_link, 2); }
    return 0;
}

static void start_link(void)
{
    HANDLE th;
    InterlockedExchange(&s_link, 0);
    th = CreateThread(NULL, 0, link_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
}

static void notice(const char *msg, DWORD ms, int then)
{
    snprintf(s_notice, sizeof s_notice, "%s", msg);
    s_notice_until = GetTickCount() + ms;
    s_notice_then = then;
}

static void send_part(void)
{
    LMsg m;
    memset(&m, 0, sizeof m);
    m.kind = L_STATE;
    m.part = s_me;
    if (s_link == 1 && s_have_them) gz_rbn_lobby_send(&m, sizeof m);
    s_last_tx = GetTickCount();
}

/* Player 2 left the host's lobby (or went silent): wait for the next. */
static void them_gone(const char *why)
{
    if (s_is_host) {
        gz_rbn_lobby_drop();
        s_have_them = 0;
        memset(&s_them, 0, sizeof s_them);
        s_me.ready = 0;
        notice(why, 3000, 0);
        start_link();
    } else
        notice("The host left.", 3000, 1);
}

static int idx_by_id(const char *want, int city)
{
    int k, n = city ? NCITY : NMON;
    for (k = 0; k < n; k++) {
        const char *id = city ? k_city[k].id : k_mon[k].id;
        size_t l = strlen(want);
        /* the menus store a city without "_DAY": a prefix is enough */
        if (!_stricmp(id, want) || (city && l >= 6 && !_strnicmp(id, want, l))) return k;
    }
    return -1;
}

/* The host's match setup to start from: its last Versus match from the
 * menus (gz_quickfight.bin, or GZ_QF_FILE as it was before netplay), or the
 * one built in. */
static void base_setup(unsigned char blob[0x640])
{
    char name[MAX_PATH], v[MAX_PATH];
    FILE *f;
    uint32_t players = 0;
    int ok = 0;
    if (GetEnvironmentVariableA("GZ_RB_ORIG_GZ_QF_FILE", v, sizeof v) && v[0] == '=')
        snprintf(name, sizeof name, "%s", v + 1);
    else
        snprintf(name, sizeof name, "gz_quickfight.bin");
    if ((f = fopen(name, "rb")) != NULL) {
        ok = fread(blob, 1, 0x640, f) == 0x640;
        fclose(f);
        if (ok) memcpy(&players, blob + 0x118, 4);
        ok = ok && players >= 2 && blob[0x238];
    }
    if (!ok) memcpy(blob, gz_lobby_template, 0x640);
}

/* The picks to start with: the last lobby's (GZ_RB_LOBBY_LAST), or the
 * host's last Versus match. */
static void initial_picks(void)
{
    unsigned char blob[0x640];
    char last[128];
    int v[5];
    float t;
    int k, best = 0;
    memset(&s_me, 0, sizeof s_me);
    base_setup(blob);
    blob[0x160 + 0x1E] = blob[0x238 + 0x1E] = blob[0xD0 + 0x3F] = 0;
    s_me.mon = s_is_host ? idx_by_id((const char *)blob + 0x160, 0) : idx_by_id("gigan", 0);
    if (s_me.mon < 0) s_me.mon = 0;
    s_me.city = idx_by_id((const char *)blob + 0xD0, 1);
    if (s_me.city < 0) s_me.city = 0;
    {
        uint32_t r;
        memcpy(&r, blob + 0x128, 4);
        for (k = 0; k < NROUNDS; k++) if ((uint32_t)k_rounds[k] == r) s_me.rounds = k;
    }
    memcpy(&t, blob + 0x120, 4);
    for (k = 1; k < NTIME; k++)
        if (abs(k_time[k] - (int)t) < abs(k_time[best] - (int)t)) best = k;
    s_me.time = best;
    if (GetEnvironmentVariableA("GZ_RB_LOBBY_LAST", last, sizeof last)
            && sscanf(last, "%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
        int mine = s_is_host ? v[0] : v[1];
        if (mine >= 0 && mine <= MON_RANDOM) s_me.mon = mine;
        if (v[2] >= 0 && v[2] <= CITY_RANDOM) s_me.city = v[2];
        if (v[3] >= 0 && v[3] < NROUNDS) s_me.rounds = v[3];
        if (v[4] >= 0 && v[4] < NTIME) s_me.time = v[4];
    }
    s_me.cur = s_me.mon;
    s_me.row = s_me.cur / GRID_COLS;
}

/* This PC's address on its network, for the host to tell player 2. */
static void find_host_ip(void)
{
    char name[256];
    struct addrinfo hints, *res = NULL, *a;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    snprintf(s_host_ip, sizeof s_host_ip, "?");
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (gethostname(name, sizeof name) == 0 && getaddrinfo(name, NULL, &hints, &res) == 0) {
        for (a = res; a; a = a->ai_next) {
            char ip[64];
            struct sockaddr_in *sa = (struct sockaddr_in *)a->ai_addr;
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
            if (strncmp(ip, "127.", 4) && strncmp(ip, "169.254.", 8)) { snprintf(s_host_ip, sizeof s_host_ip, "%s", ip); break; }
        }
        freeaddrinfo(res);
    }
}

/* ── drawing ─────────────────────────────────────────────────────────── */

static const char *mon_name(int m) { return m == MON_RANDOM ? "Random" : (m >= 0 && m < NMON ? k_mon[m].name : "?"); }
static const char *city_name(int c) { return c == CITY_RANDOM ? "Random" : (c >= 0 && c < NCITY ? k_city[c].name : "?"); }

static void time_txt(char *out, size_t n, int t) { snprintf(out, n, "%d:%02d", k_time[t] / 60, k_time[t] % 60); }

static const Part *host_part(void) { return s_is_host ? &s_me : &s_them; }
static const Part *p2_part(void) { return s_is_host ? &s_them : &s_me; }

static void draw_player(int x, int y, int w, int h, int pn, const Part *p, int present)
{
    COLORREF col = pn == 1 ? C_P1 : C_P2;
    char lab[64];
    int mine = (pn == 1) == (s_is_host != 0);
    fill(x, y, w, h, C_PANEL);
    fill(x, y, w, 8, col);
    snprintf(lab, sizeof lab, "PLAYER %d%s%s", pn, pn == 1 ? "  -  HOST" : "", mine ? "  (you)" : "");
    text(x + 24, y + 16, w - 48, 40, F_SMALL, col, 0, lab);
    if (!present) {
        text(x + 24, y + 60, w - 48, 60, F_BIG, C_DIM, 0, s_is_host ? "Waiting for player 2..." : "Connecting...");
        return;
    }
    text(x + 24, y + 60, w - 48, 60, F_BIG, C_TEXT, 0,
         s_counting ? mon_name(pn == 1 ? s_final[0] : s_final[1]) : mon_name(p->mon));
    text(x + 24, y + 124, w - 48, 40, F_TEXT, p->ready ? C_READY : C_DIM, 0,
         p->ready ? "READY" : "choosing...");
}

static void draw(void)
{
    const Part *hp = host_part(), *pp = p2_part();
    int present2 = s_have_them || !s_is_host;
    int linked = s_link == 1 && s_have_them;
    int gx = (W - (GRID_COLS * 220 + (GRID_COLS - 1) * 16)) / 2, gy = 330, k;
    char line[200];

    fill(0, 0, W, H, C_BG);
    text(0, 20, W, 70, F_TITLE, C_TEXT, 1, "ROLLBACK NETPLAY");
    if (s_is_host)
        snprintf(line, sizeof line, linked ? "Hosting on %s (port 7790)  -  player 2 is in"
                                           : "Hosting  -  player 2 joins with your IP: %s (port 7790)", s_host_ip);
    else
        snprintf(line, sizeof line, linked ? "In %s's lobby" : "Joining %s...", s_peer_txt);
    text(0, 86, W, 36, F_TEXT, C_DIM, 1, line);

    draw_player(60, 140, 560, 170, 1, hp, s_is_host || linked);
    draw_player(660, 140, 560, 170, 2, pp, linked && present2);

    /* the monster grid: each player's pick filled in their colour, each
     * cursor framed in it (yours thick) */
    for (k = 0; k < GRID_COLS * GRID_ROWS && k <= MON_RANDOM; k++) {
        int x = gx + (k % GRID_COLS) * 236, y = gy + (k / GRID_COLS) * 84;
        int p1 = hp->mon == k, p2 = linked && pp->mon == k;
        fill(x, y, 220, 72, C_CELL);
        if (p1 && p2) { fill(x, y, 110, 72, RGB(96, 40, 40)); fill(x + 110, y, 110, 72, RGB(36, 56, 100)); }
        else if (p1) fill(x, y, 220, 72, RGB(96, 40, 40));
        else if (p2) fill(x, y, 220, 72, RGB(36, 56, 100));
        text(x + 8, y, 204, 72, F_SMALL, k == MON_RANDOM ? C_GOLD : C_TEXT, 1, mon_name(k));
        if (linked && s_them.row < GRID_ROWS && s_them.cur == k)
            frame(x - 4, y - 4, 228, 80, 3, s_is_host ? C_P2 : C_P1);
        if (!s_counting && s_me.row < GRID_ROWS && s_me.cur == k)
            frame(x - 7, y - 7, 234, 86, 5, s_is_host ? C_P1 : C_P2);
    }

    /* the host's city and rules, then Leave */
    {
        int y = gy + GRID_ROWS * 84 + 24, row = GRID_ROWS;
        char v[64];
        const char *lab[3] = { "City", "Rounds", "Round time" };
        for (k = 0; k < 3; k++, row++, y += 54) {
            int sel = s_is_host && s_me.row == row && !s_counting;
            if (k == 0) snprintf(v, sizeof v, "%s", s_counting ? city_name(s_final[2]) : city_name(hp->city));
            else if (k == 1) snprintf(v, sizeof v, "%d", k_rounds[hp->rounds]);
            else time_txt(v, sizeof v, hp->time);
            fill(240, y, 800, 46, sel ? RGB(60, 44, 44) : C_PANEL);
            if (sel) frame(240, y, 800, 46, 3, C_P1);
            text(264, y, 300, 46, F_TEXT, C_DIM, 0, lab[k]);
            snprintf(line, sizeof line, s_is_host && !s_counting ? "<  %s  >" : "%s", v);
            text(560, y, 460, 46, F_TEXT, C_TEXT, 1, line);
        }
        {
            int leave_row = s_is_host ? GRID_ROWS + 3 : GRID_ROWS;
            int sel = s_me.row == leave_row && !s_counting;
            fill(540, y + 6, 200, 42, sel ? RGB(70, 40, 40) : C_PANEL);
            if (sel) frame(540, y + 6, 200, 42, 3, s_is_host ? C_P1 : C_P2);
            text(540, y + 6, 200, 42, F_TEXT, C_TEXT, 1, "Leave");
        }
    }

    /* what to press */
    if (!s_counting) {
        const char *hint;
        if (s_is_host && s_me.ready && linked && s_them.ready) hint = "Both ready  -  press START to begin the match";
        else if (s_me.ready) hint = s_is_host ? "Ready  -  waiting for player 2 (B: not ready)" : "Ready  -  waiting for the host to start (B: not ready)";
        else hint = "A / Enter: pick     START / Space: ready     B / Esc: back";
        text(0, H - 64, W, 40, F_TEXT, C_GOLD, 1, hint);
    }

    if (s_counting) {
        DWORD el = GetTickCount() - s_count_t0;
        int n = 3 - (int)(el / 1000);
        char num[8];
        snprintf(num, sizeof num, "%d", n < 1 ? 1 : n);
        fill(W / 2 - 160, 380, 320, 240, RGB(20, 22, 34));
        frame(W / 2 - 160, 380, 320, 240, 4, C_GOLD);
        text(W / 2 - 160, 380, 320, 200, F_HUGE, C_GOLD, 1, num);
        text(W / 2 - 160, 570, 320, 40, F_SMALL, C_DIM, 1, "the match is starting");
    }
    if (s_confirm) {
        fill(340, 380, 600, 200, RGB(24, 26, 40));
        frame(340, 380, 600, 200, 4, C_TEXT);
        text(340, 400, 600, 70, F_BIG, C_TEXT, 1, "Leave the lobby?");
        text(340, 490, 600, 50, F_TEXT, C_DIM, 1, "A / Enter: leave      B / Esc: stay");
    }
    if (s_notice[0] && GetTickCount() < s_notice_until) {
        fill(240, 400, 800, 140, RGB(24, 26, 40));
        frame(240, 400, 800, 140, 4, C_GOLD);
        text(260, 400, 760, 140, F_BIG, C_TEXT, 1, s_notice);
    }
    GdiFlush();
}

/* ── one frame of the lobby ──────────────────────────────────────────── */

static int leave_row(void) { return s_is_host ? GRID_ROWS + 3 : GRID_ROWS; }

static void handle_input(void)
{
    int pr[K_N], changed = 0;
    poll_presses(pr);
    if (s_counting || (s_notice[0] && GetTickCount() < s_notice_until && s_notice_then)) return;
    if (s_confirm) {
        if (pr[K_A]) {
            LMsg m;
            memset(&m, 0, sizeof m);
            m.kind = L_LEAVE;
            if (s_link == 1 && s_have_them) gz_rbn_lobby_send(&m, sizeof m);
            gz_lobby_restart(0, NULL);
        }
        if (pr[K_B]) s_confirm = 0;
        return;
    }
    if (pr[K_B]) {
        if (s_me.ready) { s_me.ready = 0; changed = 1; }
        else s_confirm = 1;
    }
    if (s_me.row < GRID_ROWS) {
        int c = s_me.cur % GRID_COLS, r = s_me.row;
        if (pr[K_LEFT])  { c = (c + GRID_COLS - 1) % GRID_COLS; changed = 1; }
        if (pr[K_RIGHT]) { c = (c + 1) % GRID_COLS; changed = 1; }
        if (pr[K_UP] && r > 0) { r--; changed = 1; }
        if (pr[K_DOWN]) { r++; changed = 1; }
        if (r < GRID_ROWS) {
            int cell = r * GRID_COLS + c;
            if (cell > MON_RANDOM) cell = MON_RANDOM;
            s_me.cur = cell;
            s_me.row = r;
        } else
            s_me.row = GRID_ROWS;
        if (pr[K_A] && s_me.row < GRID_ROWS) {
            s_me.mon = s_me.cur;
            s_me.ready = 0;
            changed = 1;
        }
    } else {
        int row = s_me.row;
        if (pr[K_UP]) { s_me.row = row == GRID_ROWS ? GRID_ROWS - 1 : row - 1; changed = 1; }
        if (pr[K_DOWN] && row < leave_row()) { s_me.row = row + 1; changed = 1; }
        if (s_is_host && row < leave_row()) {
            int d = pr[K_LEFT] ? -1 : pr[K_RIGHT] ? 1 : 0;
            if (d) {
                if (row == GRID_ROWS) s_me.city = (s_me.city + d + NCITY + 1) % (NCITY + 1);
                else if (row == GRID_ROWS + 1) s_me.rounds = (s_me.rounds + d + NROUNDS) % NROUNDS;
                else s_me.time = (s_me.time + d + NTIME) % NTIME;
                s_me.ready = 0;
                changed = 1;
            }
        }
        if (pr[K_A] && row == leave_row()) s_confirm = 1;
    }
    if (pr[K_START]) {
        int linked = s_link == 1 && s_have_them;
        if (s_is_host && s_me.ready && linked && s_them.ready) {
            /* the match: randoms drawn here, for both */
            LMsg m, a;
            uint32_t n;
            DWORD t0 = GetTickCount();
            int got = 0;
            srand(GetTickCount());
            s_final[0] = s_me.mon == MON_RANDOM ? rand() % NMON : s_me.mon;
            s_final[1] = s_them.mon == MON_RANDOM ? rand() % NMON : s_them.mon;
            s_final[2] = s_me.city == CITY_RANDOM ? rand() % NCITY : s_me.city;
            s_final[3] = s_me.rounds;
            s_final[4] = s_me.time;
            memset(&m, 0, sizeof m);
            m.kind = L_START;
            m.part = s_me;
            m.f_p1 = s_final[0]; m.f_p2 = s_final[1]; m.f_city = s_final[2];
            m.f_rounds = s_final[3]; m.f_time = s_final[4];
            if (gz_rbn_lobby_send(&m, sizeof m)) {
                /* player 2's answer, after anything it sent before it */
                while (GetTickCount() - t0 < 8000) {
                    int r = gz_rbn_lobby_recv(&a, sizeof a, &n);
                    if (r < 0) break;
                    if (r > 0 && n >= sizeof(uint32_t) && a.kind == L_ACK) { got = 1; break; }
                    if (r > 0 && a.kind == L_LEAVE) break;
                    if (r == 0) Sleep(5);
                }
            }
            if (got) { s_counting = 1; s_count_t0 = GetTickCount(); }
            else them_gone("Player 2 did not answer.");
            return;
        }
        if (!s_me.ready) { s_me.ready = 1; changed = 1; }
    }
    if (changed) send_part();
}

static void handle_net(void)
{
    LMsg m;
    uint32_t n;
    int r;
    DWORD now = GetTickCount();
    if (s_link == 2) {
        if (!s_notice_then || !s_notice[0]) {
            char msg[160];
            snprintf(msg, sizeof msg, "Could not %s: %s.", s_is_host ? "host" : "join", s_link_why ? s_link_why : "?");
            notice(msg, 4000, 1);
        }
        return;
    }
    if (s_link != 1) return;
    if (s_is_host) {
        extern void gz_rbn_lobby_refuse_extra(void);
        gz_rbn_lobby_refuse_extra();
    }
    if (!s_have_them) {
        s_have_them = 1;
        s_last_rx = now;
        memset(&s_them, 0, sizeof s_them);
        s_them.mon = -1;
        send_part();
    }
    if (s_counting) return;
    while ((r = gz_rbn_lobby_recv(&m, sizeof m, &n)) > 0) {
        s_last_rx = now;
        if (n < sizeof m) continue;
        if (m.kind == L_STATE) s_them = m.part;
        else if (m.kind == L_LEAVE) { them_gone("Player 2 left."); return; }
        else if (m.kind == L_START && !s_is_host) {
            LMsg a;
            s_them = m.part;
            s_final[0] = m.f_p1; s_final[1] = m.f_p2; s_final[2] = m.f_city;
            s_final[3] = m.f_rounds; s_final[4] = m.f_time;
            memset(&a, 0, sizeof a);
            a.kind = L_ACK;
            gz_rbn_lobby_send(&a, sizeof a);
            s_counting = 1;
            s_count_t0 = GetTickCount();
            return;
        }
    }
    if (r < 0 || now - s_last_rx > 10000) { them_gone("Player 2 left."); return; }
    if (now - s_last_tx > 500) send_part();             /* "still here" */
}

/* The match the lobby agreed on, for the game that boots next. */
static void set_match_env(void)
{
    unsigned char blob[0x640];
    char path[MAX_PATH * 2], v[64];
    const char *save = getenv("GZ_SAVE_DIR");
    FILE *f;
    if (s_is_host) {
        /* the host's setup to start from (the quick fight reads it) */
        base_setup(blob);
        snprintf(path, sizeof path, "%s\\lobby_match.bin", save && *save ? save : ".");
        if ((f = fopen(path, "wb")) != NULL) { fwrite(blob, 1, sizeof blob, f); fclose(f); }
        env_set("GZ_QF_FILE", path);
    }
    env_set("GZ_QUICK_FIGHT", "1");
    env_set("GZ_QF_P1", k_mon[s_final[0]].id);
    env_set("GZ_QF_P2", k_mon[s_final[1]].id);
    env_set("GZ_QF_CITY", k_city[s_final[2]].id);
    snprintf(v, sizeof v, "%d", k_rounds[s_final[3]]);
    env_set("GZ_QF_ROUNDS", v);
    snprintf(v, sizeof v, "%d.9", k_time[s_final[4]]);  /* as the menus store it (5:00 is 300.9) */
    env_set("GZ_QF_ROUND_TIME", v);
    env_set("GZ_QF_DELAY", "60");
    env_set("GZ_QF_SEED", NULL);
    env_set("GZ_RB_LOBBY_MATCH", "1");
    {
        /* the picks as made (a random stays random), for the next lobby */
        const Part *h = host_part(), *p = p2_part();
        snprintf(v, sizeof v, "%d,%d,%d,%d,%d", h->mon, p->mon, h->city, h->rounds, h->time);
        env_set("GZ_RB_LOBBY_LAST", v);
    }
    fprintf(stderr, "[LOBBY] match: %s vs %s in %s, %d round(s) of %d s\n", k_mon[s_final[0]].id,
            k_mon[s_final[1]].id, k_city[s_final[2]].id, k_rounds[s_final[3]], k_time[s_final[4]]);
}

/* GZ_SHOT_FILE=<path> (the test harnesses): once a file of that name
 * appears, the lobby's picture goes to <path>.bmp and the file is removed,
 * as the game's own pictures are taken. */
static void shot_poll(void)
{
    static const char *path = (const char *)1;
    char out[MAX_PATH + 8];
    FILE *f;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    if (path == (const char *)1) path = getenv("GZ_SHOT_FILE");
    if (!path || !*path || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(path);
    snprintf(out, sizeof out, "%s.bmp", path);
    if (!(f = fopen(out, "wb"))) return;
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + W * H * 4;
    ih.biSize = sizeof ih;
    ih.biWidth = W;
    ih.biHeight = -H;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(s_px, 4, (size_t)W * H, f);
    fclose(f);
}

static HANDLE s_done_ev;

static DWORD WINAPI lobby_thread(LPVOID p)
{
    MSG msg;
    (void)p;
    if (!gz_gpu_lobby_open()) {
        notice("The window could not be opened.", 1, 1);
        gz_lobby_restart(0, "The netplay lobby could not open its window.");
    }
    gfx_init();
    for (;;) {
        handle_net();
        handle_input();
        if (s_notice[0] && GetTickCount() >= s_notice_until) {
            if (s_notice_then) gz_lobby_restart(0, s_notice);
            s_notice[0] = 0;
        }
        if (s_counting && GetTickCount() - s_count_t0 >= 3000) break;
        draw();
        shot_poll();
        gz_gpu_lobby_present(s_px, W, H);
    }
    draw();
    gz_gpu_lobby_present(s_px, W, H);
    set_match_env();
    gz_rbn_lobby_done();
    SetEvent(s_done_ev);
    /* the window stays this thread's: keep its messages moving while the
     * game runs (its frames come from the game's render thread) */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* At boot (main.c), before the game reads its saves: a notice left by the
 * run before, then the lobby if this run is one. Returns once a match is
 * agreed (the quick fight's settings are then in the environment). */
void gz_lobby_boot(void)
{
    char v[256];
    const char *e = getenv("GZ_RB_LOBBY");
    if (GetEnvironmentVariableA("GZ_RB_NOTICE", v, sizeof v) && v[0]) {
        env_set("GZ_RB_NOTICE", NULL);
        MessageBoxA(NULL, v, "Rollback Netplay", MB_ICONINFORMATION);
    }
    if (!e || *e != '1' || !gz_rbn_on()) return;
    s_is_host = gz_rbn_local_port() == 0;
    {
        const char *peer = getenv("GZ_RB_PEER");
        snprintf(s_peer_txt, sizeof s_peer_txt, "%s", peer ? peer : "?");
        {
            char *c = strrchr(s_peer_txt, ':');
            if (c && !strcmp(c, ":7790")) *c = 0;
        }
    }
    if (s_is_host) find_host_ip();
    initial_picks();
    fprintf(stderr, "[LOBBY] %s\n", s_is_host ? "hosting" : "joining");
    s_done_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    start_link();
    CloseHandle(CreateThread(NULL, 0, lobby_thread, NULL, 0, NULL));
    WaitForSingleObject(s_done_ev, INFINITE);
}
