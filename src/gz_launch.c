/*
 * gz_launch.c - command-line options for launchers (RecompLauncher, scripts,
 * Steam shortcuts). With no options the game starts exactly as before: the
 * extracted disc in game\GodzillaSTE under the working folder, settings from
 * the environment (play.bat).
 *
 *   --game <path>     the player's copy: an Xbox disc image (.iso / .xiso) or
 *                     the extracted folder with default.xbe. Also switches to
 *                     the program's folder, so gz_settings.ini, mods\,
 *                     shader_cache\ and screenshots\ are found next to it, and
 *                     turns on what play.bat sets for normal play (unless the
 *                     environment already sets it). A disc image is unpacked
 *                     once into game\GodzillaSTE next to the program.
 *   --prepare         with --game: no window. Check the copy, unpack a disc
 *                     image, build the menu additions in mods\ from the
 *                     player's ControlRoom.bdl, print "progress <0..1> <text>"
 *                     lines, exit 0 when done.
 *   --log <file>      write the log there (the last two are kept as
 *                     <file>.1 / <file>.2).
 *   --fullscreen, --windowed, --window-scale N, --render-scale N, --aniso N,
 *   --movies 0|1      the same as GZ_FULLSCREEN, GZ_WINDOW_SCALE,
 *                     GZ_RENDER_SCALE, GZ_ANISO and GZ_MOVIES.
 *
 * Also: RECOMPLAUNCHER_MODE=bigpicture starts full screen unless a window
 * option says otherwise. Saves (unless GZ_SAVE_DIR is set) go to the
 * program's saves\ folder with a portable.txt next to it or with --game and a
 * writable folder, else to %LOCALAPPDATA%\GodzillaSTE (copied once from the
 * toolkit's shared %LOCALAPPDATA%\xboxrecomp). gz_settings.ini's online keys
 * and gz_controls.ini are read here too.
 *
 * Exit codes (see recomplauncher.json): 2 the copy can't be read or is not
 * this game, 3 default.xbe is not the one this build was made from, 4 the
 * program's folder can't be written.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define GZ_EXIT_BAD_COPY   2
#define GZ_EXIT_WRONG_XBE  3
#define GZ_EXIT_NO_WRITE   4

static char  s_game_dir_utf8[MAX_PATH * 3] = "game\\GodzillaSTE";
static WCHAR s_exe_dir[MAX_PATH];

const char *gz_launch_game_dir(void) { return s_game_dir_utf8; }

static void to_utf8(const WCHAR *w, char *out, int n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL)) out[0] = 0;
}

static int file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void make_dirs(const WCHAR *path)       /* every folder in path */
{
    WCHAR tmp[MAX_PATH], *p;
    wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);
    for (p = tmp + 3; *p; p++)
        if (*p == L'\\' || *p == L'/') { WCHAR c = *p; *p = 0; CreateDirectoryW(tmp, NULL); *p = c; }
    CreateDirectoryW(tmp, NULL);
}

static void progress(double f, const char *text)
{
    printf("progress %.2f %s\n", f, text);
    fflush(stdout);
}

/* A short text identifying a file's contents well enough to notice a change. */
static void file_stamp(const WCHAR *path, char *out, size_t n)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    char p8[MAX_PATH * 3];
    memset(&fa, 0, sizeof fa);
    GetFileAttributesExW(path, GetFileExInfoStandard, &fa);
    to_utf8(path, p8, sizeof p8);
    snprintf(out, n, "%s|%llu|%08lX%08lX", p8,
             ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow,
             (unsigned long)fa.ftLastWriteTime.dwHighDateTime, (unsigned long)fa.ftLastWriteTime.dwLowDateTime);
}

static int stamp_matches(const WCHAR *stamp_file, const char *want)
{
    char have[MAX_PATH * 3 + 64] = "";
    FILE *f = _wfopen(stamp_file, L"rb");
    size_t got;
    if (!f) return 0;
    got = fread(have, 1, sizeof have - 1, f);
    have[got] = 0;
    fclose(f);
    return !strcmp(have, want);
}

static int stamp_write(const WCHAR *stamp_file, const char *text)
{
    FILE *f = _wfopen(stamp_file, L"wb");
    if (!f) return 0;
    fwrite(text, 1, strlen(text), f);
    return fclose(f) == 0;
}

/* ── default.xbe check ──────────────────────────────────────────────────
 * The program is default.xbe translated to C, so it only runs the exact
 * default.xbe it was made from. CMake records that file's SHA-1 (GZ_XBE_SHA1)
 * when it can; a build without one skips the check. */
static void sha1_hex(const void *data, size_t n, char out[41])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char d[20];
    int i;
    out[0] = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) != 0) return;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        if (BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0) == 0 && BCryptFinishHash(h, d, 20, 0) == 0)
            for (i = 0; i < 20; i++) sprintf(out + i * 2, "%02x", d[i]);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
}

static int xbe_ok(const void *data, size_t n)
{
#ifdef GZ_XBE_SHA1
    char got[41];
    sha1_hex(data, n, got);
    if (_stricmp(got, GZ_XBE_SHA1)) {
        fprintf(stderr, "[LAUNCH] default.xbe SHA-1 %s, this build needs %s "
                "(Godzilla: Save the Earth, USA/Europe)\n", got, GZ_XBE_SHA1);
        printf("This copy's default.xbe is not the one this build was made from "
               "(it needs the USA/Europe release).\n");
        return 0;
    }
#else
    (void)data; (void)n;
#endif
    return 1;
}

static void *read_whole(const WCHAR *path, size_t *n)
{
    FILE *f = _wfopen(path, L"rb");
    void *buf = NULL;
    long long sz;
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (sz > 0 && sz < (1LL << 30) && (buf = malloc((size_t)sz)) != NULL) {
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); buf = NULL; }
        else *n = (size_t)sz;
    }
    fclose(f);
    return buf;
}

static int write_whole(const WCHAR *path, const void *data, size_t n)
{
    FILE *f = _wfopen(path, L"wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(data, 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}

/* ── Xbox disc images (XDVDFS) ─────────────────────────────────────────
 * The same reader as xboxrecomp's tools/xiso: find the game partition, then
 * walk the directory tables (linearly per 2048-byte sector, so a bad tree link
 * can't hide files). */
#define XSECTOR 2048
static const long long k_bases[] = { 0x00000000LL, 0x0000FD90LL, 0x00030600LL, 0x0FD90000LL, 0x18300000LL };

typedef struct {
    FILE *f;
    long long base;
    unsigned root_sector, root_size;
    unsigned long long total, done;   /* bytes, for progress */
    double p0, p1, last;   /* progress range, last value printed */
    int counting, failed;
} Xiso;

static int xiso_open(Xiso *x, const WCHAR *path)
{
    unsigned char desc[0x1C];
    size_t i;
    memset(x, 0, sizeof *x);
    if ((x->f = _wfopen(path, L"rb")) == NULL) return 0;
    for (i = 0; i < sizeof k_bases / sizeof k_bases[0]; i++) {
        if (_fseeki64(x->f, k_bases[i] + 32LL * XSECTOR, SEEK_SET)) continue;
        if (fread(desc, 1, sizeof desc, x->f) == sizeof desc && !memcmp(desc, "MICROSOFT*XBOX*MEDIA", 20)) {
            x->base = k_bases[i];
            memcpy(&x->root_sector, desc + 0x14, 4);
            memcpy(&x->root_size, desc + 0x18, 4);
            return 1;
        }
    }
    fclose(x->f);
    x->f = NULL;
    return 0;
}

static unsigned char *xiso_read(Xiso *x, unsigned sector, unsigned size)
{
    unsigned char *b = (unsigned char *)malloc(size ? size : 1);
    if (!b) return NULL;
    if (_fseeki64(x->f, x->base + (long long)sector * XSECTOR, SEEK_SET) || fread(b, 1, size, x->f) != size) {
        free(b);
        return NULL;
    }
    return b;
}

typedef int (*xiso_fn)(Xiso *x, const WCHAR *rel, unsigned sector, unsigned size, void *ctx);

/* Calls fn for every file under the directory (sector, size); rel is the path
 * below the root, with backslashes. */
static int xiso_walk(Xiso *x, unsigned sector, unsigned size, const WCHAR *rel, xiso_fn fn, void *ctx)
{
    unsigned char *d = xiso_read(x, sector, size);
    unsigned base;
    if (!d) return 0;
    for (base = 0; base < size; base += XSECTOR) {
        unsigned lim = size - base < XSECTOR ? size - base : XSECTOR, off = 0;
        while (off + 0x0E <= lim) {
            const unsigned char *e = d + base + off;
            unsigned short l, r;
            unsigned sec, sz, nlen = e[0x0D], k;
            WCHAR sub[MAX_PATH];
            int ok;
            memcpy(&l, e, 2); memcpy(&r, e + 2, 2);
            memcpy(&sec, e + 4, 4); memcpy(&sz, e + 8, 4);
            if ((l == 0xFFFF && r == 0xFFFF) || nlen == 0 || off + 0x0E + nlen > lim) break;
            for (k = 0; k < nlen; k++) if (e[0x0E + k] >= 0x80) break;
            if (k < nlen) break;
            if (wcslen(rel) + nlen + 2 >= MAX_PATH) { free(d); return 0; }
            swprintf_s(sub, MAX_PATH, L"%s%s", rel, rel[0] ? L"\\" : L"");
            for (k = 0; k < nlen; k++) { size_t q = wcslen(sub); sub[q] = (WCHAR)e[0x0E + k]; sub[q + 1] = 0; }
            ok = (e[0x0C] & 0x10) ? xiso_walk(x, sec, sz, sub, fn, ctx) : fn(x, sub, sec, sz, ctx);
            if (!ok) { free(d); return 0; }
            off += (0x0E + nlen + 3) & ~3u;
        }
    }
    free(d);
    return 1;
}

static int xiso_count(Xiso *x, const WCHAR *rel, unsigned sector, unsigned size, void *ctx)
{
    (void)rel; (void)sector; (void)ctx;
    x->total += size;
    return 1;
}

static int xiso_extract_one(Xiso *x, const WCHAR *rel, unsigned sector, unsigned size, void *ctx)
{
    const WCHAR *dest = (const WCHAR *)ctx;
    WCHAR out[MAX_PATH], *slash;
    static unsigned char buf[1 << 20];
    unsigned left = size;
    FILE *f;
    swprintf_s(out, MAX_PATH, L"%s\\%s", dest, rel);
    if ((slash = wcsrchr(out, L'\\')) != NULL) { *slash = 0; make_dirs(out); *slash = L'\\'; }
    if ((f = _wfopen(out, L"wb")) == NULL) { x->failed = GZ_EXIT_NO_WRITE; return 0; }
    if (_fseeki64(x->f, x->base + (long long)sector * XSECTOR, SEEK_SET)) { fclose(f); x->failed = GZ_EXIT_BAD_COPY; return 0; }
    while (left) {
        unsigned n = left < sizeof buf ? left : (unsigned)sizeof buf;
        double p;
        if (fread(buf, 1, n, x->f) != n) { fclose(f); x->failed = GZ_EXIT_BAD_COPY; return 0; }
        if (fwrite(buf, 1, n, f) != n) { fclose(f); x->failed = GZ_EXIT_NO_WRITE; return 0; }
        left -= n;
        x->done += n;
        p = x->p0 + (x->p1 - x->p0) * (double)x->done / (double)(x->total ? x->total : 1);
        if (p - x->last >= 0.02) { progress(p, "Unpacking your disc"); x->last = p; }
    }
    if (fclose(f)) { x->failed = GZ_EXIT_NO_WRITE; return 0; }
    return 1;
}

typedef struct { const WCHAR *want; unsigned sector, size; int found; } XisoFind;
static int xiso_find_one(Xiso *x, const WCHAR *rel, unsigned sector, unsigned size, void *ctx)
{
    XisoFind *q = (XisoFind *)ctx;
    (void)x;
    if (!_wcsicmp(rel, q->want)) { q->sector = sector; q->size = size; q->found = 1; }
    return 1;
}

/* ── Menu additions (the same edits as recomp_config/tools/make_options_items.py
 * and make_biollante_slot.py, which setup.ps1 runs) ─────────────────────
 * Both change XML inside ShellData/ControlRoom.bdl and keep its length. */
typedef struct { char *p; size_t n, cap; } Buf;

static int buf_put(Buf *b, const void *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t c = (b->cap ? b->cap * 2 : 4096);
        char *q;
        while (c < b->n + n + 1) c *= 2;
        if ((q = (char *)realloc(b->p, c)) == NULL) return 0;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}

static const char *mem_find(const char *h, size_t hn, const char *nd)
{
    size_t nn = strlen(nd), i;
    if (nn > hn) return NULL;
    for (i = 0; i + nn <= hn; i++) if (h[i] == nd[0] && !memcmp(h + i, nd, nn)) return h + i;
    return NULL;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* At s: "<position> 0 " [0-9.]+ " 0". Returns the number's start and length. */
static int match_position(const char *s, const char *end, const char **num, size_t *nlen)
{
    static const char head[] = "<position> 0 ";
    const char *p = s + sizeof head - 1, *q;
    if (end - s < (ptrdiff_t)sizeof head || memcmp(s, head, sizeof head - 1)) return 0;
    for (q = p; q < end && ((*q >= '0' && *q <= '9') || *q == '.'); q++) {}
    if (q == p || end - q < 2 || q[0] != ' ' || q[1] != '0') return 0;
    *num = p; *nlen = (size_t)(q - p);
    return 1;
}

/* At s: <text n="NAME">, whitespace, then a position. */
static int match_text_pos(const char *s, const char *end, const char *name, const char **num, size_t *nlen)
{
    char tag[64];
    size_t tl;
    snprintf(tag, sizeof tag, "<text n=\"%s\">", name);
    tl = strlen(tag);
    if (end - s < (ptrdiff_t)tl || memcmp(s, tag, tl)) return 0;
    s += tl;
    while (s < end && is_ws(*s)) s++;
    return match_position(s, end, num, nlen);
}

static int patch_options(const char *b, size_t n, Buf *out)
{
    static const char *const names[5] = { "GzTournament", "GzPrompts", "GzRate", "GzWide", "GzSpeed" };
    const char *i = mem_find(b, n, "<interface n=\"Options\">"), *j, *m, *me, *s, *num;
    size_t xn, nl, k, need;
    double ys[3];
    int ny = 0;
    Buf tpl = {0}, x = {0}, nw = {0}, y = {0};
    if (!i || (j = mem_find(i, n - (size_t)(i - b), "</interface>")) == NULL) return 0;
    xn = (size_t)(j - i);
    if (mem_find(i, xn, "GzTournament")) return 0;
    if ((m = mem_find(i, xn, "<text n=\"TopTen\">")) == NULL || (me = mem_find(m, (size_t)(j - m), "</text>")) == NULL) return 0;
    me += 7;
    for (s = m; s < me; s++) if (*s != '\t' && *s != '\r' && *s != '\n') buf_put(&tpl, s, 1);
    for (s = i; s < j && ny <= 3; s++) {
        static const char *const ynames[3] = { "AudioAdjust", "ControllerAdjust", "TopTen" };
        for (k = 0; k < 3; k++)
            if (match_text_pos(s, j, ynames[k], &num, &nl)) {
                char t[32];
                if (ny < 3) { snprintf(t, sizeof t, "%.*s", (int)nl, num); ys[ny] = strtod(t, NULL); }
                ny++;
                break;
            }
    }
    if (ny != 3) return 0;
    for (k = 0; k < 5; k++) {           /* the new entries, from the TopTen one */
        Buf t = {0};
        char nm[48], pos[48];
        const char *e = tpl.p + tpl.n;
        snprintf(nm, sizeof nm, "n=\"%s\"", names[k]);
        snprintf(pos, sizeof pos, "<position> 0 %.2f 0", ys[0] + 24.0 * (double)(k + 3));
        for (s = tpl.p; s < e;) {
            if (e - s >= 10 && !memcmp(s, "n=\"TopTen\"", 10)) { buf_put(&t, nm, strlen(nm)); s += 10; }
            else { buf_put(&t, s, 1); s++; }
        }
        for (s = t.p, e = t.p + t.n; s < e;) {
            if (match_position(s, e, &num, &nl)) { buf_put(&nw, pos, strlen(pos)); s = num + nl + 2; }
            else { buf_put(&nw, s, 1); s++; }
        }
        free(t.p);
    }
    buf_put(&x, i, (size_t)(me - i));
    buf_put(&x, nw.p, nw.n);
    buf_put(&x, me, (size_t)(j - me));
    for (k = 0; k < 2; k++) {           /* ControllerAdjust and TopTen move up */
        const char *nm = k ? "TopTen" : "ControllerAdjust", *e = x.p + x.n;
        char v[32];
        int hits = 0;
        Buf r = {0};
        snprintf(v, sizeof v, "%.2f", ys[0] + 24.0 * (double)(k + 1));
        for (s = x.p; s < e;) {
            if (match_text_pos(s, e, nm, &num, &nl)) {
                buf_put(&r, s, (size_t)(num - s)); buf_put(&r, v, strlen(v));
                s = num + nl; hits++;
            } else { buf_put(&r, s, 1); s++; }
        }
        free(x.p); x = r;
        if (hits != 1) { free(x.p); free(tpl.p); free(nw.p); return 0; }
    }
    if (x.n < xn) return 0;
    need = x.n - xn;                    /* paid back with indentation tabs */
    for (s = x.p; s < x.p + x.n; s++) {
        if (need && *s == '\t') { need--; continue; }
        buf_put(&y, s, 1);
    }
    free(x.p); free(tpl.p); free(nw.p);
    if (need || y.n != xn) { free(y.p); return 0; }
    buf_put(out, b, (size_t)(i - b));
    buf_put(out, y.p, y.n);
    buf_put(out, j, n - (size_t)(j - b));
    free(y.p);
    return 1;
}

static int patch_biollante(const char *b, size_t n, int shift, Buf *out)
{
    static const char Q[] = "SOURCE\\MONSTERSELECT_EN_QUESTION";
    static const char *const nudges[2] = { "<p> -1 -1 0</p>", "<p> 0 -1 0</p>" };
    const char *i = mem_find(b, n, "<interface n=\"Monster Select\">"), *j, *r1, *g;
    char cell[1024], grp[64];
    size_t xn, k;
    long long need;
    Buf x = {0}, head = {0}, grid = {0};
    if (!i || (j = mem_find(i, n - (size_t)(i - b), "</interface>")) == NULL) return 0;
    xn = (size_t)(j - i);
    if (mem_find(i, xn, "<o n=\"biollante\">")) return 0;
    snprintf(cell, sizeof cell,
             "<o n=\"biollante\"><p> 571 0 0</p>"
             "<q n=\"disabled\"><f>0x1</f><b>%s</b><c>0x406040FF</c></q>"
             "<q n=\"enabled\"><b>%s</b><c>0x60FF60FF</c></q>"
             "<q n=\"infocus\"><f>0x1</f><b>%s</b><c>0xC0FFC0FF</c></q>"
             "<o n=\"locked\"><f>0x1</f><q n=\"color\"><b>%s</b><c>0x80FF80FF</c></q>"
             "<q n=\"question_mark\"><b>SOURCE\\OUTLINE</b></q></o></o>", Q, Q, Q, Q);
    if ((r1 = mem_find(i, xn, "</o><o n=\"Row1\">")) == NULL) return 0;
    buf_put(&x, i, (size_t)(r1 - i));
    buf_put(&x, cell, strlen(cell));
    buf_put(&x, r1, (size_t)(j - r1));
    {   /* move the grid left so ten columns fit */
        static const char old_grp[] = "<o n=\"Thumbnails\"><p> 3 -7 0</p>";
        const char *a = mem_find(x.p, x.n, old_grp);
        Buf y = {0};
        if (!a || mem_find(a + 1, x.n - (size_t)(a + 1 - x.p), old_grp)) { free(x.p); return 0; }
        snprintf(grp, sizeof grp, "<o n=\"Thumbnails\"><p> %d -7 0</p>", 3 + shift);
        buf_put(&y, x.p, (size_t)(a - x.p));
        buf_put(&y, grp, strlen(grp));
        buf_put(&y, a + sizeof old_grp - 1, x.n - (size_t)(a - x.p) - (sizeof old_grp - 1));
        free(x.p); x = y;
    }
    need = (long long)x.n - (long long)xn;
    if ((g = mem_find(x.p, x.n, "<o n=\"Thumbnails\">")) == NULL) { free(x.p); return 0; }
    buf_put(&head, x.p, (size_t)(g - x.p));
    buf_put(&grid, g, x.n - (size_t)(g - x.p));
    free(x.p);
    for (k = 0; k < 2; k++) {
        size_t tl = strlen(nudges[k]);
        const char *t;
        while (need >= (long long)tl && (t = mem_find(grid.p, grid.n, nudges[k])) != NULL) {
            size_t at = (size_t)(t - grid.p);
            memmove(grid.p + at, grid.p + at + tl, grid.n - at - tl + 1);
            grid.n -= tl;
            need -= (long long)tl;
        }
    }
    while (need > 0) {
        const char *t = mem_find(head.p, head.n, "<p> ");
        size_t at;
        if (!t) break;
        at = (size_t)(t - head.p) + 3;          /* drop the space after <p> */
        memmove(head.p + at, head.p + at + 1, head.n - at);
        head.n--;
        need--;
    }
    x.p = NULL; x.n = x.cap = 0;
    buf_put(&x, head.p, head.n);
    buf_put(&x, grid.p, grid.n);
    free(head.p); free(grid.p);
    if (need < 0) {                     /* removed a little too much: pad a number */
        const char *c = mem_find(x.p, x.n, "<o n=\"biollante\">"), *t;
        Buf y = {0};
        size_t at;
        if (!c || (t = mem_find(c, x.n - (size_t)(c - x.p), "<p>")) == NULL) { free(x.p); return 0; }
        at = (size_t)(t - x.p) + 3;
        buf_put(&y, x.p, at);
        while (need < 0) { buf_put(&y, " ", 1); need++; }
        buf_put(&y, x.p + at, x.n - at);
        free(x.p); x = y;
    }
    if (need || x.n != xn) { free(x.p); return 0; }
    buf_put(out, b, (size_t)(i - b));
    buf_put(out, x.p, x.n);
    buf_put(out, j, n - (size_t)(j - b));
    free(x.p);
    return 1;
}

/* Builds mods\menu and mods\biollante_slot next to the program from the copy's
 * ControlRoom.bdl, unless they were already built from that file. */
static int build_menu_mods(const WCHAR *game_dir)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH], stamp[MAX_PATH];
    char want[MAX_PATH * 3 + 64];
    size_t n = 0;
    char *b;
    Buf menu = {0}, bio = {0};
    int rc = 0;
    swprintf_s(src, MAX_PATH, L"%s\\ShellData\\ControlRoom.bdl", game_dir);
    swprintf_s(stamp, MAX_PATH, L"%s\\mods\\gz_built_from.txt", s_exe_dir);
    file_stamp(src, want, sizeof want);
    /* The layout version: menus built by an older release (four Options
     * entries, before Game speed) are built again. */
    strncat_s(want, sizeof want, "|options5", _TRUNCATE);
    swprintf_s(dst, MAX_PATH, L"%s\\mods\\biollante_slot\\ShellData\\ControlRoom.bdl", s_exe_dir);
    if (stamp_matches(stamp, want) && file_exists(dst)) return 0;
    progress(0.96, "Adding the Options entries and the Biollante cell");
    if ((b = (char *)read_whole(src, &n)) == NULL) {
        fprintf(stderr, "[LAUNCH] can't read ShellData\\ControlRoom.bdl in the game's copy\n");
        return GZ_EXIT_BAD_COPY;
    }
    if (!patch_options(b, n, &menu) || !patch_biollante(menu.p, menu.n, -30, &bio)) {
        fprintf(stderr, "[LAUNCH] ControlRoom.bdl is not the one the menu additions were made for\n");
        rc = GZ_EXIT_BAD_COPY;
    } else {
        swprintf_s(dst, MAX_PATH, L"%s\\mods\\menu\\ShellData", s_exe_dir);
        make_dirs(dst);
        wcscat_s(dst, MAX_PATH, L"\\ControlRoom.bdl");
        if (!write_whole(dst, menu.p, menu.n)) rc = GZ_EXIT_NO_WRITE;
        swprintf_s(dst, MAX_PATH, L"%s\\mods\\biollante_slot\\ShellData", s_exe_dir);
        make_dirs(dst);
        wcscat_s(dst, MAX_PATH, L"\\ControlRoom.bdl");
        if (!rc && !write_whole(dst, bio.p, bio.n)) rc = GZ_EXIT_NO_WRITE;
        if (!rc && !stamp_write(stamp, want)) rc = GZ_EXIT_NO_WRITE;
    }
    free(b); free(menu.p); free(bio.p);
    return rc;
}

/* Gets the player's copy ready: picks the folder the game reads (unpacking a
 * disc image if needed), checks default.xbe, builds the menu additions. */
static int prepare_copy(const WCHAR *game)
{
    WCHAR dir[MAX_PATH], xbe[MAX_PATH], stamp[MAX_PATH];
    int rc;
    progress(0.0, "Checking your copy");
    if (dir_exists(game)) {
        size_t n = 0;
        void *d;
        wcsncpy_s(dir, MAX_PATH, game, _TRUNCATE);
        swprintf_s(xbe, MAX_PATH, L"%s\\default.xbe", dir);
        if ((d = read_whole(xbe, &n)) == NULL) {
            printf("There is no default.xbe in that folder.\n");
            return GZ_EXIT_BAD_COPY;
        }
        rc = xbe_ok(d, n) ? 0 : GZ_EXIT_WRONG_XBE;
        free(d);
        if (rc) return rc;
    } else {
        Xiso x;
        XisoFind q = { L"default.xbe", 0, 0, 0 };
        char want[MAX_PATH * 3 + 64];
        unsigned char *d;
        if (!xiso_open(&x, game)) {
            printf("That file is not an Xbox disc image.\n");
            return GZ_EXIT_BAD_COPY;
        }
        xiso_walk(&x, x.root_sector, x.root_size, L"", xiso_find_one, &q);
        if (!q.found || (d = xiso_read(&x, q.sector, q.size)) == NULL) {
            fclose(x.f);
            printf("That disc image has no default.xbe.\n");
            return GZ_EXIT_BAD_COPY;
        }
        rc = xbe_ok(d, q.size) ? 0 : GZ_EXIT_WRONG_XBE;
        free(d);
        if (rc) { fclose(x.f); return rc; }
        swprintf_s(dir, MAX_PATH, L"%s\\game\\GodzillaSTE", s_exe_dir);
        swprintf_s(stamp, MAX_PATH, L"%s\\game\\gz_unpacked_from.txt", s_exe_dir);
        file_stamp(game, want, sizeof want);
        swprintf_s(xbe, MAX_PATH, L"%s\\default.xbe", dir);
        if (!(stamp_matches(stamp, want) && file_exists(xbe))) {
            DeleteFileW(stamp);
            make_dirs(dir);
            xiso_walk(&x, x.root_sector, x.root_size, L"", xiso_count, NULL);
            x.p0 = 0.05; x.p1 = 0.95; x.last = 0.0;
            if (!xiso_walk(&x, x.root_sector, x.root_size, L"", xiso_extract_one, dir)) {
                fclose(x.f);
                printf(x.failed == GZ_EXIT_NO_WRITE ? "Could not write the unpacked game next to the program (is the disk full?).\n"
                                                    : "The disc image ended early; it may be incomplete.\n");
                return x.failed ? x.failed : GZ_EXIT_BAD_COPY;
            }
            if (!stamp_write(stamp, want)) { fclose(x.f); return GZ_EXIT_NO_WRITE; }
        }
        fclose(x.f);
    }
    to_utf8(dir, s_game_dir_utf8, sizeof s_game_dir_utf8);
    if ((rc = build_menu_mods(dir)) != 0) {
        if (rc == GZ_EXIT_NO_WRITE) printf("Could not write the menu additions next to the program.\n");
        else printf("The copy's menu file is not the one this build expects.\n");
        return rc;
    }
    progress(1.0, "Ready");
    return 0;
}

static void set_env_default(const char *name, const char *value)
{
    const char *e = getenv(name);
    if (!e || !*e) _putenv_s(name, value);
}

/* The last log is renamed <file>.1, the one before <file>.2. */
static void open_log(const WCHAR *path)
{
    WCHAR p[MAX_PATH], a[MAX_PATH], b[MAX_PATH], dir[MAX_PATH], *c, *slash;
    FILE *f;
    /* RecompLauncher writes "{install}/logs/x.log": a backslash path with
     * forward slashes after it. One kind, so the folder is found. */
    wcsncpy_s(p, MAX_PATH, path, _TRUNCATE);
    for (c = p; *c; c++) if (*c == L'/') *c = L'\\';
    wcsncpy_s(dir, MAX_PATH, p, _TRUNCATE);
    if ((slash = wcsrchr(dir, L'\\')) != NULL) { *slash = 0; make_dirs(dir); }
    swprintf_s(a, MAX_PATH, L"%s.1", p);
    swprintf_s(b, MAX_PATH, L"%s.2", p);
    MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(p, a, MOVEFILE_REPLACE_EXISTING);
    /* Only redirect when the file opens: a failed freopen closes the stream,
     * and the next line written to it ends the program. */
    if ((f = _wfopen(p, L"w")) == NULL) return;
    fclose(f);
    _wfreopen(p, L"a", stderr);            /* both streams append to one file */
    _wfreopen(p, L"a", stdout);
}

static int folder_writable(const WCHAR *dir)
{
    WCHAR p[MAX_PATH];
    HANDLE h;
    swprintf_s(p, MAX_PATH, L"%s\\gz_write_test.tmp", dir);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

static void copy_tree(const WCHAR *from, const WCHAR *to, unsigned *files)
{
    WCHAR pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\*", from);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    make_dirs(to);
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        swprintf_s(a, MAX_PATH, L"%s\\%s", from, fd.cFileName);
        swprintf_s(b, MAX_PATH, L"%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(a, b, files);
        else if (CopyFileW(a, b, TRUE)) (*files)++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* This game's folders (title id 49470073) from the toolkit's shared save
 * folder into its own. */
static void migrate_saves(const WCHAR *localappdata, const WCHAR *to)
{
    static const WCHAR *const parts[] = { L"UDATA\\49470073", L"TDATA\\49470073" };
    WCHAR a[MAX_PATH], b[MAX_PATH];
    unsigned files = 0, i;
    for (i = 0; i < 2; i++) {
        swprintf_s(a, MAX_PATH, L"%s\\xboxrecomp\\%s", localappdata, parts[i]);
        swprintf_s(b, MAX_PATH, L"%s\\%s", to, parts[i]);
        if (dir_exists(a)) copy_tree(a, b, &files);
    }
    if (files) fprintf(stderr, "[LAUNCH] copied %u save file(s) from %%LOCALAPPDATA%%\\xboxrecomp\n", files);
}

extern void gz_settings_apply_env(void);
extern void gz_controls_load(void);

/* Called first thing. Returns -1 to start the game, or an exit code. */
int gz_launch_init(void)
{
    int argc = 0, i, prepare = 0, window_opt = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const WCHAR *game = NULL, *log = NULL;
    WCHAR *slash, portable[MAX_PATH];
    const char *mode;

    GetModuleFileNameW(NULL, s_exe_dir, MAX_PATH);
    if ((slash = wcsrchr(s_exe_dir, L'\\')) != NULL) *slash = 0;

    for (i = 1; argv && i < argc; i++) {
        const WCHAR *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        char v8[64] = "";
        if (v) to_utf8(v, v8, sizeof v8);
        if (!wcscmp(a, L"--game") && v) { game = v; i++; }
        else if (!wcscmp(a, L"--prepare")) prepare = 1;
        else if (!wcscmp(a, L"--log") && v) { log = v; i++; }
        else if (!wcscmp(a, L"--fullscreen")) { _putenv_s("GZ_FULLSCREEN", "1"); window_opt = 1; }
        else if (!wcscmp(a, L"--windowed")) { _putenv_s("GZ_FULLSCREEN", "0"); window_opt = 1; }
        else if (!wcscmp(a, L"--window-scale") && v) { _putenv_s("GZ_WINDOW_SCALE", v8); i++; }
        else if (!wcscmp(a, L"--render-scale") && v) { _putenv_s("GZ_RENDER_SCALE", v8); i++; }
        else if (!wcscmp(a, L"--aniso") && v) { _putenv_s("GZ_ANISO", v8); i++; }
        else if (!wcscmp(a, L"--movies") && v) { _putenv_s("GZ_MOVIES", v8); i++; }
    }

    /* Started on its own (the exe double-clicked: no options, and not from
     * play.bat or a test script, which set RECOMP_USB): play the copy
     * Play.bat remembered, or the unpacked one next to the program. Before,
     * this ran without the setup play.bat makes and never opened a window. */
    if (!game && !prepare && !getenv("RECOMP_USB")) {
        static WCHAR found[MAX_PATH], logp[MAX_PATH];
        WCHAR p[MAX_PATH];
        char line[MAX_PATH * 2];
        FILE *f;
        swprintf_s(p, MAX_PATH, L"%s\\game_path.txt", s_exe_dir);
        if ((f = _wfopen(p, L"r")) != NULL) {
            if (fgets(line, sizeof line, f)) {
                size_t n = strlen(line);
                while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
                if (n && MultiByteToWideChar(CP_OEMCP, 0, line, -1, found, MAX_PATH)
                        && GetFileAttributesW(found) != INVALID_FILE_ATTRIBUTES)
                    game = found;
            }
            fclose(f);
        }
        if (!game) {
            swprintf_s(p, MAX_PATH, L"%s\\game\\GodzillaSTE\\default.xbe", s_exe_dir);
            if (file_exists(p)) {
                swprintf_s(found, MAX_PATH, L"%s\\game\\GodzillaSTE", s_exe_dir);
                game = found;
            }
        }
        if (!game) {
            MessageBoxW(NULL, L"Drag your Godzilla: Save the Earth disc image or game folder onto Play.bat "
                              L"(next to this program), or start the game from RecompLauncher.",
                        L"Godzilla: Save the Earth", MB_ICONINFORMATION);
            LocalFree(argv);
            return 1;
        }
        if (!log) {
            swprintf_s(logp, MAX_PATH, L"%s\\logs\\godzilla.log", s_exe_dir);
            log = logp;
        }
    }

    if (log) open_log(log);
    if (prepare && !game) {
        printf("--prepare needs --game <disc image or folder>\n");
        return GZ_EXIT_BAD_COPY;
    }

    /* Big Picture: full screen unless the player picked a window option. */
    mode = getenv("RECOMPLAUNCHER_MODE");
    if (!window_opt && mode && !strcmp(mode, "bigpicture")) set_env_default("GZ_FULLSCREEN", "1");

    if (game) {
        int rc;
        char ov[MAX_PATH * 3];
        WCHAR w[MAX_PATH];
        static WCHAR full[MAX_PATH];
        /* one spelling of the path (the unpack stamp compares it), resolved
         * against the folder the program was started in */
        if (GetFullPathNameW(game, MAX_PATH, full, NULL)) game = full;
        SetCurrentDirectoryW(s_exe_dir);
        rc = prepare_copy(game);
        /* Started by Play.bat or a shortcut, a copy it can't use would
         * otherwise close without a word (RecompLauncher shows its own
         * message for these exit codes). */
        if (rc && !prepare && !getenv("RECOMPLAUNCHER")) {
            const WCHAR *why =
                rc == GZ_EXIT_WRONG_XBE ? L"Your copy's default.xbe is a different version from the one this build was "
                                          L"made from. It needs Godzilla: Save the Earth for Xbox, USA/Europe release."
              : rc == GZ_EXIT_NO_WRITE  ? L"The game couldn't unpack your disc next to the program. Put the game's folder "
                                          L"somewhere you can write to (not Program Files) with about 1 GB free."
              :                           L"Your copy couldn't be read, or it isn't Godzilla: Save the Earth for Xbox. "
                                          L"Use the disc image (.iso / .xiso.iso) or the extracted folder with default.xbe.";
            MessageBoxW(NULL, why, L"Godzilla: Save the Earth", MB_ICONWARNING);
        }
        if (prepare || rc) {
            LocalFree(argv);
            return rc;
        }
        /* what play.bat sets for normal play (see the comments there) */
        set_env_default("RECOMP_USB", "1");
        set_env_default("RECOMP_PB_EXEC", "1");
        set_env_default("RECOMP_PB_WALK", "dma");
        set_env_default("RECOMP_AC97_READY", "1");
        set_env_default("RECOMP_HEAP_RECLAIM", "1");
        set_env_default("RECOMP_PB_INVENTORY", "0");
        set_env_default("RECOMP_DISPATCH_GATE", "1");
        set_env_default("RECOMP_KERNEL_LOG_BUDGET", "200");
        set_env_default("RECOMP_AUDIO_BUF_SAMPLES", "1024");
        set_env_default("RECOMP_AUDIO_BUF_COUNT", "4");
        set_env_default("RECOMP_AUDIO_HOST", "1");
        set_env_default("GZ_BIOLLANTE_SLOT", "1");
        {
            /* rollback netplay (gz_rbnet.c): both players' ports have a pad,
             * or the game never polls the remote player's */
            const char *rn = getenv("GZ_RB_NET");
            if (rn && *rn == '1') set_env_default("RECOMP_USB_PADS", "2");
        }
        set_env_default("GZ_MOVIES", "0");
        swprintf_s(w, MAX_PATH, L"%s\\mods\\biollante_slot", s_exe_dir);
        to_utf8(w, ov, sizeof ov);
        set_env_default("RECOMP_OVERLAY_DIR", ov);
    }

    /* Where saves and the hard-disk cache go, unless GZ_SAVE_DIR says:
     *  - <program>\saves with a portable.txt next to the program, or when
     *    started with --game and the program's folder can be written (a
     *    launcher install keeps everything in its own folder);
     *  - otherwise %LOCALAPPDATA%\GodzillaSTE. Before this was the toolkit's
     *    shared %LOCALAPPDATA%\xboxrecomp, which other xboxrecomp games use
     *    too: the first time, this game's saves are copied from there (the
     *    old ones stay where they are). */
    {
        const char *sd = getenv("GZ_SAVE_DIR");
        if (!sd || !*sd) {
            WCHAR w[MAX_PATH];
            char sv[MAX_PATH * 3];
            swprintf_s(portable, MAX_PATH, L"%s\\portable.txt", s_exe_dir);
            if (file_exists(portable) || (game && folder_writable(s_exe_dir))) {
                swprintf_s(w, MAX_PATH, L"%s\\saves", s_exe_dir);
            } else {
                WCHAR la[MAX_PATH];
                DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
                if (!n || n >= MAX_PATH) { LocalFree(argv); gz_settings_apply_env(); gz_controls_load(); return -1; }
                swprintf_s(w, MAX_PATH, L"%s\\GodzillaSTE", la);
                if (!dir_exists(w)) migrate_saves(la, w);
            }
            to_utf8(w, sv, sizeof sv);
            _putenv_s("GZ_SAVE_DIR", sv);
        }
    }
    gz_settings_apply_env();
    gz_controls_load();
    LocalFree(argv);
    return -1;
}
