/*
 * gz_net.c - System Link over real sockets (GZ_NET=1).
 *
 * The game links the XDK's XNET library (its own TCP/IP stack on the console's
 * network chip, which the recomp does not emulate) and uses only a small part
 * of it, all through one socket wrapper class (sub_0018E8A0..0018EAF0):
 * socket / bind / ioctlsocket / setsockopt(SO_BROADCAST) / select / sendto /
 * recvfrom / closesocket on UDP (and VDP, protocol 254, which is UDP with a
 * console-specific payload split), plus XNetGetTitleXnAddr, key create /
 * register and XnAddr <-> InAddr translation. src/recomp_manual.c replaces
 * those library entry points with the functions here, which run them on
 * Winsock. Addresses are plain IPv4: an XNADDR carries the machine's real
 * address and "secure" addresses are that same address; there is no key
 * exchange or encryption, so this talks to other recomp instances only.
 *
 *   GZ_NET=1            turn it on (off: the original library runs and the
 *                       game sees no network cable).
 *   GZ_NET_IP=a.b.c.d   address to bind and to advertise (default: the
 *                       machine's primary address). Two instances on one PC
 *                       can use 127.0.0.1 and 127.0.0.2.
 *   GZ_NET_PEER=ip[,ip] other players. Broadcasts (how System Link finds
 *                       games) are sent to these addresses instead of the
 *                       LAN broadcast address, so play works across the
 *                       internet or a VPN as long as the UDP port reaches
 *                       the other side.
 *   GZ_NET_LOG=1        log every call.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SOCK 32
#define HANDLE_BASE 0x100u

static SOCKET   s_sock[MAX_SOCK];
static int      s_used[MAX_SOCK];
static int      s_on = -1, s_log;
static int      s_err;
static uint32_t s_ip;                 /* network byte order */
static int      s_ip_forced;
static uint32_t s_peer[8];
static int      s_npeer;
static int      s_lag_ms;             /* simulated delay, defined with its queue below */
static void     lag_flush(void);

static void nlog(const char *fmt, ...)
{
    va_list ap;
    if (!s_log) return;
    va_start(ap, fmt);
    fprintf(stderr, "[NET] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static const char *ipstr(uint32_t ip_be, char *buf)
{
    const uint8_t *b = (const uint8_t *)&ip_be;
    sprintf(buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

int gz_net_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("GZ_NET"), *ip = getenv("GZ_NET_IP"), *peer = getenv("GZ_NET_PEER");
        WSADATA wd;
        char b[32];
        s_on = e && *e == '1';
        if (!s_on) return 0;
        s_log = getenv("GZ_NET_LOG") != NULL;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
            fprintf(stderr, "[NET] WSAStartup failed; networking stays off\n");
            return s_on = 0;
        }
        if (ip && *ip) {
            s_ip = inet_addr(ip);
            s_ip_forced = 1;
        } else {
            /* The address a UDP socket would use to reach the internet
             * (nothing is sent by connect() on a datagram socket). */
            SOCKET t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in a;
            int n = sizeof a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = inet_addr("8.8.8.8");
            if (t != INVALID_SOCKET && connect(t, (struct sockaddr *)&a, sizeof a) == 0
                    && getsockname(t, (struct sockaddr *)&a, &n) == 0)
                s_ip = a.sin_addr.s_addr;
            else
                s_ip = inet_addr("127.0.0.1");
            if (t != INVALID_SOCKET) closesocket(t);
        }
        while (peer && *peer && s_npeer < 8) {
            char one[64];
            size_t n = strcspn(peer, ",; ");
            if (n && n < sizeof one) {
                memcpy(one, peer, n); one[n] = 0;
                s_peer[s_npeer] = inet_addr(one);
                if (s_peer[s_npeer] != INADDR_NONE) s_npeer++;
            }
            peer += n;
            while (*peer == ',' || *peer == ';' || *peer == ' ') peer++;
        }
        fprintf(stderr, "[NET] System Link on real sockets: local %s, %d peer(s)%s\n",
                ipstr(s_ip, b), s_npeer, s_npeer ? "" : " (LAN broadcast)");
    }
    return s_on;
}

uint32_t gz_net_local_ip(void) { return s_ip; }

static SOCKET host(uint32_t h)
{
    uint32_t i = h - HANDLE_BASE;
    return (i < MAX_SOCK && s_used[i]) ? s_sock[i] : INVALID_SOCKET;
}

static uint32_t fail(void) { s_err = WSAGetLastError(); return 0xFFFFFFFFu; }

uint32_t gz_net_socket(uint32_t af, uint32_t type, uint32_t proto)
{
    SOCKET s;
    BOOL one = TRUE;
    int i;
    if (proto == 254) proto = IPPROTO_UDP;          /* VDP */
    s = socket((int)af, (int)type, (int)proto);
    if (s == INVALID_SOCKET) return fail();
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    for (i = 0; i < MAX_SOCK; i++)
        if (!s_used[i]) {
            s_used[i] = 1; s_sock[i] = s;
            nlog("socket(type %u, proto %u) -> %X", type, proto, HANDLE_BASE + i);
            return HANDLE_BASE + (uint32_t)i;
        }
    closesocket(s);
    s_err = WSAEMFILE;
    return 0xFFFFFFFFu;
}

uint32_t gz_net_closesocket(uint32_t h)
{
    uint32_t i = h - HANDLE_BASE;
    if (i >= MAX_SOCK || !s_used[i]) { s_err = WSAENOTSOCK; return 0xFFFFFFFFu; }
    closesocket(s_sock[i]);
    s_used[i] = 0;
    nlog("closesocket(%X)", h);
    return 0;
}

uint32_t gz_net_bind(uint32_t h, const void *addr, uint32_t len)
{
    struct sockaddr_in a;
    char b[32];
    if (len < sizeof a) { s_err = WSAEFAULT; return 0xFFFFFFFFu; }
    memcpy(&a, addr, sizeof a);
    a.sin_family = AF_INET;                          /* the game writes it as a u16 already */
    if (s_ip_forced && a.sin_addr.s_addr == INADDR_ANY) a.sin_addr.s_addr = s_ip;
    if (bind(host(h), (struct sockaddr *)&a, sizeof a) != 0) {
        fail();
        fprintf(stderr, "[NET] bind %s:%u failed (%d)\n", ipstr(a.sin_addr.s_addr, b), ntohs(a.sin_port), s_err);
        return 0xFFFFFFFFu;
    }
    nlog("bind(%X, %s:%u)", h, ipstr(a.sin_addr.s_addr, b), ntohs(a.sin_port));
    return 0;
}

uint32_t gz_net_ioctlsocket(uint32_t h, uint32_t cmd, uint32_t *arg)
{
    u_long v = arg ? *arg : 0;
    if (ioctlsocket(host(h), (long)cmd, &v) != 0) return fail();
    if (arg) *arg = (uint32_t)v;
    return 0;
}

uint32_t gz_net_setsockopt(uint32_t h, uint32_t level, uint32_t name, const void *val, uint32_t len)
{
    if (setsockopt(host(h), (int)level, (int)name, (const char *)val, (int)len) != 0) {
        nlog("setsockopt(%X, %X, %X) not supported by the host; ignored", h, level, name);
    }
    return 0;
}

/* Guest fd_set: u32 count, u32 handles[64]. */
static void fds_in(const uint32_t *g, fd_set *f)
{
    uint32_t i;
    FD_ZERO(f);
    if (!g) return;
    for (i = 0; i < g[0] && i < 64; i++) {
        SOCKET s = host(g[1 + i]);
        if (s != INVALID_SOCKET) FD_SET(s, f);
    }
}

static void fds_out(uint32_t *g, const fd_set *f)
{
    uint32_t i, n = 0, keep[64];
    if (!g) return;
    for (i = 0; i < g[0] && i < 64; i++) {
        SOCKET s = host(g[1 + i]);
        if (s != INVALID_SOCKET && FD_ISSET(s, f)) keep[n++] = g[1 + i];
    }
    g[0] = n;
    for (i = 0; i < n; i++) g[1 + i] = keep[i];
}

uint32_t gz_net_select(uint32_t *r, uint32_t *w, uint32_t *e, const uint32_t *timeout)
{
    fd_set fr, fw, fe;
    struct timeval tv;
    int n;
    if (s_lag_ms > 0) lag_flush();
    fds_in(r, &fr); fds_in(w, &fw); fds_in(e, &fe);
    if (timeout) { tv.tv_sec = (long)timeout[0]; tv.tv_usec = (long)timeout[1]; }
    n = select(0, r ? &fr : NULL, w ? &fw : NULL, e ? &fe : NULL, timeout ? &tv : NULL);
    if (n < 0) return fail();
    fds_out(r, &fr); fds_out(w, &fw); fds_out(e, &fe);
    return (uint32_t)n;
}

uint32_t gz_net_recvfrom(uint32_t h, void *buf, uint32_t len, uint32_t flags, void *from, uint32_t *fromlen)
{
    struct sockaddr_in a;
    int al = sizeof a, n;
    char b[32];
    if (s_lag_ms > 0) lag_flush();
    n = recvfrom(host(h), (char *)buf, (int)len, (int)flags, (struct sockaddr *)&a, &al);
    if (n < 0) return fail();
    if (from && fromlen && *fromlen >= sizeof a) { memcpy(from, &a, sizeof a); *fromlen = sizeof a; }
    nlog("recvfrom(%X) %d bytes from %s:%u", h, n, ipstr(a.sin_addr.s_addr, b), ntohs(a.sin_port));
    return (uint32_t)n;
}

void gz_net_random(uint8_t *p, uint32_t n);
/* GZ_NET_LAG_MS / GZ_NET_LOSS_PCT: hold every outgoing datagram for that long
 * and drop that share of them, to try a match under internet-like conditions
 * with two instances on one PC. The queue is flushed from every socket call. */
#define LAGQ 512
static struct { DWORD due; SOCKET s; struct sockaddr_in to; int len; char data[1500]; } s_lagq[LAGQ];
static int s_lag_head, s_lag_tail, s_lag_ms = -1, s_loss_pct;
static unsigned s_lag_dropped, s_lag_sent;

static void lag_init(void)
{
    const char *l = getenv("GZ_NET_LAG_MS"), *p = getenv("GZ_NET_LOSS_PCT");
    s_lag_ms = l ? atoi(l) : 0;
    s_loss_pct = p ? atoi(p) : 0;
    if (s_lag_ms || s_loss_pct)
        fprintf(stderr, "[NET] simulating %d ms delay and %d%% loss on outgoing packets\n", s_lag_ms, s_loss_pct);
}

static void lag_flush(void)
{
    DWORD now = GetTickCount();
    while (s_lag_head != s_lag_tail && (int)(now - s_lagq[s_lag_head].due) >= 0) {
        sendto(s_lagq[s_lag_head].s, s_lagq[s_lag_head].data, s_lagq[s_lag_head].len, 0,
               (struct sockaddr *)&s_lagq[s_lag_head].to, sizeof s_lagq[0].to);
        s_lag_head = (s_lag_head + 1) % LAGQ;
    }
}

/* 1 = handled here (queued or dropped). */
static int lag_send(SOCKET so, const void *buf, int len, const struct sockaddr_in *to)
{
    uint8_t r;
    int next;
    if (s_lag_ms < 0) lag_init();
    if (!s_lag_ms && !s_loss_pct) return 0;
    lag_flush();
    gz_net_random(&r, 1);
    if (s_loss_pct && (int)(r * 100u / 256u) < s_loss_pct) { s_lag_dropped++; return 1; }
    next = (s_lag_tail + 1) % LAGQ;
    if (len > (int)sizeof s_lagq[0].data || next == s_lag_head) return 0;     /* too big / full: send now */
    s_lagq[s_lag_tail].due = GetTickCount() + (DWORD)s_lag_ms;
    s_lagq[s_lag_tail].s = so; s_lagq[s_lag_tail].to = *to; s_lagq[s_lag_tail].len = len;
    memcpy(s_lagq[s_lag_tail].data, buf, (size_t)len);
    s_lag_tail = next;
    s_lag_sent++;
    return 1;
}

uint32_t gz_net_sendto(uint32_t h, const void *buf, uint32_t len, uint32_t flags, const void *to, uint32_t tolen)
{
    struct sockaddr_in a;
    int n, i;
    char b[32];
    if (!to || tolen < sizeof a) { s_err = WSAEFAULT; return 0xFFFFFFFFu; }
    memcpy(&a, to, sizeof a);
    a.sin_family = AF_INET;
    if (a.sin_addr.s_addr == INADDR_BROADCAST && s_npeer) {
        /* Game discovery: reach each configured peer directly. */
        for (i = 0; i < s_npeer; i++) {
            a.sin_addr.s_addr = s_peer[i];
            if (!lag_send(host(h), buf, (int)len, &a))
                sendto(host(h), (const char *)buf, (int)len, (int)flags, (struct sockaddr *)&a, sizeof a);
        }
        nlog("sendto(%X) %u bytes broadcast -> %d peer(s), port %u", h, len, s_npeer, ntohs(a.sin_port));
        return len;
    }
    if (lag_send(host(h), buf, (int)len, &a)) return len;
    n = sendto(host(h), (const char *)buf, (int)len, (int)flags, (struct sockaddr *)&a, sizeof a);
    if (n < 0) return fail();
    nlog("sendto(%X) %d bytes to %s:%u", h, n, ipstr(a.sin_addr.s_addr, b), ntohs(a.sin_port));
    return (uint32_t)n;
}

uint32_t gz_net_get_error(void) { return (uint32_t)s_err; }
void gz_net_set_error(uint32_t e) { s_err = (int)e; }

/* XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20] = 36 bytes. */
void gz_net_fill_xnaddr(uint8_t *xna, uint32_t ip_be)
{
    memset(xna, 0, 36);
    memcpy(xna, &ip_be, 4);
    xna[10] = 0x00; xna[11] = 0x50; xna[12] = 0xF2;         /* a made-up MAC from the address */
    memcpy(xna + 13, (const uint8_t *)&ip_be + 1, 3);
}

void gz_net_random(uint8_t *p, uint32_t n)
{
    static uint64_t s;
    uint32_t i;
    if (!s) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        s = (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ 0x9E3779B97F4A7C15ull;
    }
    for (i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        p[i] = (uint8_t)(s >> 24);
    }
}
