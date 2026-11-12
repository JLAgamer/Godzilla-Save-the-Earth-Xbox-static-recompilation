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

#define PAD      18
#define RING     512u                   /* ticks of pads kept */
#define MAXSEND  40u                    /* pads per packet at most */
#define MAGIC    0x5242475Au            /* "ZGBR" */

enum { M_HELLO = 1, M_SETUP, M_HASHES, M_STATE };
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
static int connect_peer(void)
{
    static int done;
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
    if (!connect_peer()) { s_on = 0; return 0; }
    if (s_side == 1) {
        m = msg_recv(&type, &len);              /* the hello: its UDP port */
        if (!m || type != M_HELLO || len < 8 || ((uint32_t *)m)[0] != MAGIC) {
            fprintf(stderr, "[RBN] bad hello\n");
            free(m);
            s_on = 0;
            return 0;
        }
        s_peer.sin_port = htons((uint16_t)((uint32_t *)m)[1]);
        free(m);
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
    for (i = 0; i < n; i++) {
        const uint8_t *p = gz_rb_shareable_page(i);
        hash[i] = p ? page_hash(p) : 0;
    }
    if (s_side == 2) {
        uint32_t k = 0;
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
            for (j = 0; j < cnt && q + 4 + 4096 <= m + len; j++, q += 4 + 4096) {
                uint32_t pi;
                uint8_t *dst;
                memcpy(&pi, q, 4);
                if ((dst = gz_rb_page_for_write((int)pi)) != NULL) { memcpy(dst, q + 4, 4096); k++; }
            }
            gz_rb_set_vbase(vb);
        }
        free(m);
        fprintf(stderr, "[RBN] tick 0: %u pages taken from the host (%.2f s)\n", k, now_s() - t0);
    } else {
        uint8_t *out;
        uint32_t cnt = 0, j;
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
        out = (uint8_t *)malloc(12 + (size_t)cnt * (4 + 4096));
        if (!out) { free(m); free(hash); return 0; }
        memcpy(out, &vb, 8);
        memcpy(out + 8, &cnt, 4);
        for (i = 0, j = 0; i < n; i++)
            if (hash[i] && ((uint64_t *)m)[i] != hash[i]) {
                uint32_t pi = (uint32_t)i;
                memcpy(out + 12 + (size_t)j * 4100, &pi, 4);
                memcpy(out + 12 + (size_t)j * 4100 + 4, gz_rb_shareable_page(i), 4096);
                j++;
            }
        msg_send(M_STATE, out, 12 + cnt * 4100u, NULL, 0);
        free(out);
        free(m);
        gz_rb_set_vbase(vb);
        fprintf(stderr, "[RBN] tick 0: %u pages sent to player 2 (%.2f s)\n", cnt, now_s() - t0);
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
    /* this side's pads up to cur + delay */
    while (s_local_max < (int64_t)cur + s_delay) {
        int64_t t = s_local_max + 1;
        read_local((uint32_t)t, s_local[t % RING]);
        s_local_max = t;
    }
    send_pads(cur);
    recv_pads(cur);

    /* Too far ahead of what the other side has confirmed: wait for it. */
    t0 = now_s();
    while ((int64_t)cur - s_remote_max > s_window) {
        Sleep(1);
        lag_flush();
        recv_pads(cur);
        if (now_s() - t0 > 0.25) send_pads(cur);     /* in case ours were lost */
        if (now_s() - t0 > 10.0) {
            fprintf(stderr, "[RBN] nothing from the other side for 10 s: playing on predictions\n");
            s_remote_max = (int64_t)cur;            /* (the match is effectively over) */
            break;
        }
    }
    s_stat_stall_ms += (unsigned)((now_s() - t0) * 1000.0);

    /* Time sync: ahead of the other side (its tick now, allowing for the
     * time since its packet) by more than a tick on average: give back a
     * little time, so both sides run on predictions equally. */
    if (s_peer_tick_at > 0.0) {
        double peer_now = (double)s_peer_tick + (now_s() - s_peer_tick_at) * 60.0;
        double ahead = (double)cur - peer_now;
        s_ahead_avg = s_ahead_avg * 0.95 + ahead * 0.05;
        if (s_ahead_avg > 1.0) Sleep(s_ahead_avg > 3.0 ? 8 : 3);
    }

    if (now_s() - last_report > 10.0) {
        last_report = now_s();
        fprintf(stderr, "[RBN] tick %u: confirmed to %lld, %u rollbacks (%u ticks replayed), stalled %u ms, ahead %.1f\n",
                cur, (long long)s_remote_max, s_stat_rb, s_stat_rb_ticks, s_stat_stall_ms, s_ahead_avg);
    }

    if (s_first_bad >= 0) {
        uint32_t back = cur - (uint32_t)s_first_bad;
        s_first_bad = -1;
        s_stat_rb++;
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
 * After the prompt the game starts again as a netplay session: both PCs boot
 * the same way, deterministic from the first frame (main.c), straight into a
 * match set up from the host's last Versus setup (the quick fight's
 * gz_quickfight.bin; the client takes the host's). Two games that each went
 * their own way through the menus are too far apart in memory to agree on a
 * match. */
void gz_rbn_menu_pick(void)
{
    char ip[128], peer[160], base[MAX_PATH], save[MAX_PATH + 16], exe[MAX_PATH];
    const char *qf = getenv("GZ_QF_FILE"), *sd = getenv("GZ_SAVE_DIR");
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int r = gz_rbn_prompt(ip, sizeof ip);
    FILE *f;
    if (!r) return;
    if (r == 1) {
        if ((f = fopen(qf && *qf ? qf : "gz_quickfight.bin", "rb")) == NULL) {
            MessageBoxA(GetForegroundWindow(),
                        "Play one Versus match from the menus first: the host's last Versus setup "
                        "(monsters, city, rules) is the match you play.", "Rollback Netplay", MB_ICONINFORMATION);
            return;
        }
        fclose(f);
    }
    if (sd && *sd) snprintf(base, sizeof base, "%s", sd);
    else {
        const char *la = getenv("LOCALAPPDATA");
        snprintf(base, sizeof base, "%s\\xboxrecomp", la ? la : ".");
    }
    snprintf(save, sizeof save, "%s\\netplay", base);
    CreateDirectoryA(save, NULL);
    snprintf(peer, sizeof peer, "%s%s", ip, strchr(ip, ':') ? "" : ":7790");

    /* the session's settings, for the new process */
    SetEnvironmentVariableA("GZ_RB_NET", "1");
    SetEnvironmentVariableA("GZ_RB_SIDE", r == 1 ? "1" : "2");
    SetEnvironmentVariableA("GZ_RB_PEER", r == 2 ? peer : NULL);
    SetEnvironmentVariableA("GZ_SAVE_DIR", save);
    SetEnvironmentVariableA("GZ_QUICK_FIGHT", "1");
    SetEnvironmentVariableA("GZ_MOVIES", "0");
    SetEnvironmentVariableA("GZ_QF_DELAY", "60");
    SetEnvironmentVariableA("RECOMP_USB_PADS", "2");
    SetEnvironmentVariableA("GZ_RATE", "60");
    SetEnvironmentVariableA("GZ_TOURNAMENT", "1");
    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) return;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;          /* the same log */
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fprintf(stderr, "[RBN] Rollback Netplay: %s; starting again as a netplay session\n",
            r == 1 ? "hosting" : peer);
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
