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
#include "gz_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GZ_EXIT_BAD_COPY   2
#define GZ_EXIT_WRONG_XBE  3
#define GZ_EXIT_NO_WRITE   4

static char  s_game_dir_utf8[GZ_PATH_MAX * 3] = "game" GZ_SEP_A "GodzillaSTE";
static gzch s_exe_dir[GZ_PATH_MAX];

const char *gz_launch_game_dir(void) { return s_game_dir_utf8; }

#ifdef _WIN32
#define GZ_PLAY_SCRIPT GZT("Play.bat")
#else
#define GZ_PLAY_SCRIPT GZT("play.sh")
#endif

static void progress(double f, const char *text)
{
    printf("progress %.2f %s\n", f, text);
    fflush(stdout);
}

/* A short text identifying a file's contents well enough to notice a change. */
static void file_stamp(const gzch *path, char *out, size_t n)
{
    unsigned long long size, mtime;
    char p8[GZ_PATH_MAX * 3];
    gz_file_info(path, &size, &mtime);
    gz_to_utf8(path, p8, sizeof p8);
    snprintf(out, n, "%s|%llu|%016llX", p8, size, mtime);
}

static int stamp_matches(const gzch *stamp_file, const char *want)
{
    char have[GZ_PATH_MAX * 3 + 64] = "";
    FILE *f = gz_fopen(stamp_file, GZT("rb"));
    size_t got;
    if (!f) return 0;
    got = fread(have, 1, sizeof have - 1, f);
    have[got] = 0;
    fclose(f);
    return !strcmp(have, want);
}

static int stamp_write(const gzch *stamp_file, const char *text)
{
    FILE *f = gz_fopen(stamp_file, GZT("wb"));
    if (!f) return 0;
    fwrite(text, 1, strlen(text), f);
    return fclose(f) == 0;
}

/* ── default.xbe check ──────────────────────────────────────────────────
 * The program is default.xbe translated to C, so it only runs the exact
 * default.xbe it was made from. CMake records that file's SHA-1 (GZ_XBE_SHA1)
 * when it can; a build without one skips the check. */
static int xbe_ok(const void *data, size_t n)
{
#ifdef GZ_XBE_SHA1
    char got[41];
    gz_sha1_hex(data, n, got);
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

static void *read_whole(const gzch *path, size_t *n)
{
    FILE *f = gz_fopen(path, GZT("rb"));
    void *buf = NULL;
    long long sz;
    if (!f) return NULL;
    gz_fseek(f, 0, SEEK_END);
    sz = gz_ftell(f);
    gz_fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < (1LL << 30) && (buf = malloc((size_t)sz)) != NULL) {
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); buf = NULL; }
        else *n = (size_t)sz;
    }
    fclose(f);
    return buf;
}

static int write_whole(const gzch *path, const void *data, size_t n)
{
    FILE *f = gz_fopen(path, GZT("wb"));
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

static int xiso_open(Xiso *x, const gzch *path)
{
    unsigned char desc[0x1C];
    size_t i;
    memset(x, 0, sizeof *x);
    if ((x->f = gz_fopen(path, GZT("rb"))) == NULL) return 0;
    for (i = 0; i < sizeof k_bases / sizeof k_bases[0]; i++) {
        if (gz_fseek(x->f, k_bases[i] + 32LL * XSECTOR, SEEK_SET)) continue;
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
    if (gz_fseek(x->f, x->base + (long long)sector * XSECTOR, SEEK_SET) || fread(b, 1, size, x->f) != size) {
        free(b);
        return NULL;
    }
    return b;
}

typedef int (*xiso_fn)(Xiso *x, const gzch *rel, unsigned sector, unsigned size, void *ctx);

/* Calls fn for every file under the directory (sector, size); rel is the path
 * below the root, with backslashes. */
static int xiso_walk(Xiso *x, unsigned sector, unsigned size, const gzch *rel, xiso_fn fn, void *ctx)
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
            gzch sub[GZ_PATH_MAX];
            int ok;
            memcpy(&l, e, 2); memcpy(&r, e + 2, 2);
            memcpy(&sec, e + 4, 4); memcpy(&sz, e + 8, 4);
            if ((l == 0xFFFF && r == 0xFFFF) || nlen == 0 || off + 0x0E + nlen > lim) break;
            for (k = 0; k < nlen; k++) if (e[0x0E + k] >= 0x80) break;
            if (k < nlen) break;
            if (gz_strlen(rel) + nlen + 2 >= GZ_PATH_MAX) { free(d); return 0; }
            gz_sprintf(sub, GZ_PATH_MAX, GZT("%s%s"), rel, rel[0] ? GZ_SEP : GZT(""));
            for (k = 0; k < nlen; k++) { size_t q = gz_strlen(sub); sub[q] = (gzch)e[0x0E + k]; sub[q + 1] = 0; }
            ok = (e[0x0C] & 0x10) ? xiso_walk(x, sec, sz, sub, fn, ctx) : fn(x, sub, sec, sz, ctx);
            if (!ok) { free(d); return 0; }
            off += (0x0E + nlen + 3) & ~3u;
        }
    }
    free(d);
    return 1;
}

static int xiso_count(Xiso *x, const gzch *rel, unsigned sector, unsigned size, void *ctx)
{
    (void)rel; (void)sector; (void)ctx;
    x->total += size;
    return 1;
}

static int xiso_extract_one(Xiso *x, const gzch *rel, unsigned sector, unsigned size, void *ctx)
{
    const gzch *dest = (const gzch *)ctx;
    gzch out[GZ_PATH_MAX], *slash;
    static unsigned char buf[1 << 20];
    unsigned left = size;
    FILE *f;
    gz_sprintf(out, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("%s"), dest, rel);
    if ((slash = gz_strrchr(out, GZ_SEPC)) != NULL) { *slash = 0; gz_make_dirs(out); *slash = GZ_SEPC; }
    if ((f = gz_fopen(out, GZT("wb"))) == NULL) { x->failed = GZ_EXIT_NO_WRITE; return 0; }
    if (gz_fseek(x->f, x->base + (long long)sector * XSECTOR, SEEK_SET)) { fclose(f); x->failed = GZ_EXIT_BAD_COPY; return 0; }
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

typedef struct { const gzch *want; unsigned sector, size; int found; } XisoFind;
static int xiso_find_one(Xiso *x, const gzch *rel, unsigned sector, unsigned size, void *ctx)
{
    XisoFind *q = (XisoFind *)ctx;
    (void)x;
    if (!gz_stricmp(rel, q->want)) { q->sector = sector; q->size = size; q->found = 1; }
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
static int build_menu_mods(const gzch *game_dir)
{
    gzch src[GZ_PATH_MAX], dst[GZ_PATH_MAX], stamp[GZ_PATH_MAX];
    char want[GZ_PATH_MAX * 3 + 64];
    size_t n = 0;
    char *b;
    Buf menu = {0}, bio = {0};
    int rc = 0;
    gz_sprintf(src, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("ShellData") GZ_SEP GZT("ControlRoom.bdl"), game_dir);
    gz_sprintf(stamp, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("mods") GZ_SEP GZT("gz_built_from.txt"), s_exe_dir);
    file_stamp(src, want, sizeof want);
    /* The layout version: menus built by an older release (four Options
     * entries, before Game speed) are built again. */
    strncat(want, "|options5", sizeof want - strlen(want) - 1);
    gz_sprintf(dst, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("mods") GZ_SEP GZT("biollante_slot") GZ_SEP GZT("ShellData") GZ_SEP GZT("ControlRoom.bdl"), s_exe_dir);
    if (stamp_matches(stamp, want) && gz_file_exists(dst)) return 0;
    progress(0.96, "Adding the Options entries and the Biollante cell");
    if ((b = (char *)read_whole(src, &n)) == NULL) {
        fprintf(stderr, "[LAUNCH] can't read ShellData\\ControlRoom.bdl in the game's copy\n");
        return GZ_EXIT_BAD_COPY;
    }
    if (!patch_options(b, n, &menu) || !patch_biollante(menu.p, menu.n, -30, &bio)) {
        fprintf(stderr, "[LAUNCH] ControlRoom.bdl is not the one the menu additions were made for\n");
        rc = GZ_EXIT_BAD_COPY;
    } else {
        gz_sprintf(dst, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("mods") GZ_SEP GZT("menu") GZ_SEP GZT("ShellData"), s_exe_dir);
        gz_make_dirs(dst);
        gz_strcat(dst, GZ_PATH_MAX, GZ_SEP GZT("ControlRoom.bdl"));
        if (!write_whole(dst, menu.p, menu.n)) rc = GZ_EXIT_NO_WRITE;
        gz_sprintf(dst, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("mods") GZ_SEP GZT("biollante_slot") GZ_SEP GZT("ShellData"), s_exe_dir);
        gz_make_dirs(dst);
        gz_strcat(dst, GZ_PATH_MAX, GZ_SEP GZT("ControlRoom.bdl"));
        if (!rc && !write_whole(dst, bio.p, bio.n)) rc = GZ_EXIT_NO_WRITE;
        if (!rc && !stamp_write(stamp, want)) rc = GZ_EXIT_NO_WRITE;
    }
    free(b); free(menu.p); free(bio.p);
    return rc;
}

/* Gets the player's copy ready: picks the folder the game reads (unpacking a
 * disc image if needed), checks default.xbe, builds the menu additions. */
static int prepare_copy(const gzch *game)
{
    gzch dir[GZ_PATH_MAX], xbe[GZ_PATH_MAX], stamp[GZ_PATH_MAX];
    int rc;
    progress(0.0, "Checking your copy");
    if (gz_dir_exists(game)) {
        size_t n = 0;
        void *d;
        gz_strcpy(dir, GZ_PATH_MAX, game);
        gz_sprintf(xbe, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("default.xbe"), dir);
        if ((d = read_whole(xbe, &n)) == NULL) {
            printf("There is no default.xbe in that folder.\n");
            return GZ_EXIT_BAD_COPY;
        }
        rc = xbe_ok(d, n) ? 0 : GZ_EXIT_WRONG_XBE;
        free(d);
        if (rc) return rc;
    } else {
        Xiso x;
        XisoFind q = { GZT("default.xbe"), 0, 0, 0 };
        char want[GZ_PATH_MAX * 3 + 64];
        unsigned char *d;
        if (!xiso_open(&x, game)) {
            printf("That file is not an Xbox disc image.\n");
            return GZ_EXIT_BAD_COPY;
        }
        xiso_walk(&x, x.root_sector, x.root_size, GZT(""), xiso_find_one, &q);
        if (!q.found || (d = xiso_read(&x, q.sector, q.size)) == NULL) {
            fclose(x.f);
            printf("That disc image has no default.xbe.\n");
            return GZ_EXIT_BAD_COPY;
        }
        rc = xbe_ok(d, q.size) ? 0 : GZ_EXIT_WRONG_XBE;
        free(d);
        if (rc) { fclose(x.f); return rc; }
        gz_sprintf(dir, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("game") GZ_SEP GZT("GodzillaSTE"), s_exe_dir);
        gz_sprintf(stamp, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("game") GZ_SEP GZT("gz_unpacked_from.txt"), s_exe_dir);
        file_stamp(game, want, sizeof want);
        gz_sprintf(xbe, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("default.xbe"), dir);
        if (!(stamp_matches(stamp, want) && gz_file_exists(xbe))) {
            gz_delete_file(stamp);
            gz_make_dirs(dir);
            xiso_walk(&x, x.root_sector, x.root_size, GZT(""), xiso_count, NULL);
            x.p0 = 0.05; x.p1 = 0.95; x.last = 0.0;
            if (!xiso_walk(&x, x.root_sector, x.root_size, GZT(""), xiso_extract_one, dir)) {
                fclose(x.f);
                printf(x.failed == GZ_EXIT_NO_WRITE ? "Could not write the unpacked game next to the program (is the disk full?).\n"
                                                    : "The disc image ended early; it may be incomplete.\n");
                return x.failed ? x.failed : GZ_EXIT_BAD_COPY;
            }
            if (!stamp_write(stamp, want)) { fclose(x.f); return GZ_EXIT_NO_WRITE; }
        }
        fclose(x.f);
    }
    gz_to_utf8(dir, s_game_dir_utf8, sizeof s_game_dir_utf8);
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
static void open_log(const gzch *path)
{
    gzch p[GZ_PATH_MAX], a[GZ_PATH_MAX], b[GZ_PATH_MAX], dir[GZ_PATH_MAX], *c, *slash;
    FILE *f;
    /* RecompLauncher writes "{install}/logs/x.log": a backslash path with
     * forward slashes after it. One kind, so the folder is found. */
    gz_strcpy(p, GZ_PATH_MAX, path);
#ifdef _WIN32
    for (c = p; *c; c++) if (*c == L'/') *c = GZ_SEPC;
#else
    for (c = p; *c; c++) if (*c == '\\') *c = GZ_SEPC;
#endif
    gz_strcpy(dir, GZ_PATH_MAX, p);
    if ((slash = gz_strrchr(dir, GZ_SEPC)) != NULL) { *slash = 0; gz_make_dirs(dir); }
    gz_sprintf(a, GZ_PATH_MAX, GZT("%s.1"), p);
    gz_sprintf(b, GZ_PATH_MAX, GZT("%s.2"), p);
    gz_move_replace(a, b);
    gz_move_replace(p, a);
    /* Only redirect when the file opens: a failed freopen closes the stream,
     * and the next line written to it ends the program. */
    if ((f = gz_fopen(p, GZT("w"))) == NULL) return;
    fclose(f);
    gz_freopen(p, GZT("a"), stderr);            /* both streams append to one file */
    gz_freopen(p, GZT("a"), stdout);
}

typedef struct { const gzch *from, *to; unsigned *files; } CopyCtx;
static void copy_tree(const gzch *from, const gzch *to, unsigned *files);

static void copy_one(const gzch *name, int is_dir, void *ctx)
{
    const CopyCtx *c = (const CopyCtx *)ctx;
    gzch a[GZ_PATH_MAX], b[GZ_PATH_MAX];
    gz_sprintf(a, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("%s"), c->from, name);
    gz_sprintf(b, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("%s"), c->to, name);
    if (is_dir) copy_tree(a, b, c->files);
    else if (gz_copy_file_new(a, b)) (*c->files)++;
}

static void copy_tree(const gzch *from, const gzch *to, unsigned *files)
{
    CopyCtx c;
    if (!gz_dir_exists(from)) return;
    c.from = from; c.to = to; c.files = files;
    gz_make_dirs(to);
    gz_list_dir(from, copy_one, &c);
}

/* This game's folders (title id 49470073) from the toolkit's shared save
 * folder into its own. */
static void migrate_saves(const gzch *localappdata, const gzch *to)
{
    static const gzch *const parts[] = { GZT("UDATA") GZ_SEP GZT("49470073"), GZT("TDATA") GZ_SEP GZT("49470073") };
    gzch a[GZ_PATH_MAX], b[GZ_PATH_MAX];
    unsigned files = 0, i;
    for (i = 0; i < 2; i++) {
        gz_sprintf(a, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("xboxrecomp") GZ_SEP GZT("%s"), localappdata, parts[i]);
        gz_sprintf(b, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("%s"), to, parts[i]);
        if (gz_dir_exists(a)) copy_tree(a, b, &files);
    }
    if (files) fprintf(stderr, "[LAUNCH] copied %u save file(s) from %%LOCALAPPDATA%%\\xboxrecomp\n", files);
}

extern void gz_settings_apply_env(void);
extern void gz_controls_load(void);

/* Called first thing. Returns -1 to start the game, or an exit code. */
int gz_launch_init(void)
{
    int argc = 0, i, prepare = 0, window_opt = 0;
    gzch **argv = gz_host_args(&argc);
    const gzch *game = NULL, *log = NULL;
    gzch portable[GZ_PATH_MAX];
    const char *mode;

    gz_exe_dir(s_exe_dir, GZ_PATH_MAX);

    for (i = 1; argv && i < argc; i++) {
        const gzch *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        char v8[64] = "";
        if (v) gz_to_utf8(v, v8, sizeof v8);
        if (!gz_strcmp(a, GZT("--game")) && v) { game = v; i++; }
        else if (!gz_strcmp(a, GZT("--prepare"))) prepare = 1;
        else if (!gz_strcmp(a, GZT("--log")) && v) { log = v; i++; }
        else if (!gz_strcmp(a, GZT("--fullscreen"))) { _putenv_s("GZ_FULLSCREEN", "1"); window_opt = 1; }
        else if (!gz_strcmp(a, GZT("--windowed"))) { _putenv_s("GZ_FULLSCREEN", "0"); window_opt = 1; }
        else if (!gz_strcmp(a, GZT("--window-scale")) && v) { _putenv_s("GZ_WINDOW_SCALE", v8); i++; }
        else if (!gz_strcmp(a, GZT("--render-scale")) && v) { _putenv_s("GZ_RENDER_SCALE", v8); i++; }
        else if (!gz_strcmp(a, GZT("--aniso")) && v) { _putenv_s("GZ_ANISO", v8); i++; }
        else if (!gz_strcmp(a, GZT("--movies")) && v) { _putenv_s("GZ_MOVIES", v8); i++; }
    }

    /* Started on its own (the exe double-clicked: no options, and not from
     * play.bat or a test script, which set RECOMP_USB): play the copy
     * Play.bat remembered, or the unpacked one next to the program. Before,
     * this ran without the setup play.bat makes and never opened a window. */
    if (!game && !prepare && !getenv("RECOMP_USB")) {
        static gzch found[GZ_PATH_MAX], logp[GZ_PATH_MAX];
        gzch p[GZ_PATH_MAX];
        char line[GZ_PATH_MAX * 2];
        FILE *f;
        gz_sprintf(p, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("game_path.txt"), s_exe_dir);
        if ((f = gz_fopen(p, GZT("r"))) != NULL) {
            if (fgets(line, sizeof line, f)) {
                size_t n = strlen(line);
                while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
                if (n && gz_from_text(line, found, GZ_PATH_MAX) && gz_path_exists(found))
                    game = found;
            }
            fclose(f);
        }
        if (!game) {
            gz_sprintf(p, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("game") GZ_SEP GZT("GodzillaSTE") GZ_SEP GZT("default.xbe"), s_exe_dir);
            if (gz_file_exists(p)) {
                gz_sprintf(found, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("game") GZ_SEP GZT("GodzillaSTE"), s_exe_dir);
                game = found;
            }
        }
        if (!game) {
            gz_message_box(GZT("Drag your Godzilla: Save the Earth disc image or game folder onto ") GZ_PLAY_SCRIPT
                           GZT(" (next to this program), or start the game from RecompLauncher."),
                           GZT("Godzilla: Save the Earth"), 0);
            gz_host_free_args(argv);
            return 1;
        }
        if (!log) {
            gz_sprintf(logp, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("logs") GZ_SEP GZT("godzilla.log"), s_exe_dir);
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
        char ov[GZ_PATH_MAX * 3];
        gzch w[GZ_PATH_MAX];
        static gzch full[GZ_PATH_MAX];
        /* one spelling of the path (the unpack stamp compares it), resolved
         * against the folder the program was started in */
        if (gz_full_path(game, full, GZ_PATH_MAX)) game = full;
        gz_chdir(s_exe_dir);
        rc = prepare_copy(game);
        /* Started by Play.bat or a shortcut, a copy it can't use would
         * otherwise close without a word (RecompLauncher shows its own
         * message for these exit codes). */
        if (rc && !prepare && !getenv("RECOMPLAUNCHER")) {
            const gzch *why =
                rc == GZ_EXIT_WRONG_XBE ? GZT("Your copy's default.xbe is a different version from the one this build was ")
                                          GZT("made from. It needs Godzilla: Save the Earth for Xbox, USA/Europe release.")
              : rc == GZ_EXIT_NO_WRITE  ? GZT("The game couldn't unpack your disc next to the program. Put the game's folder ")
                                          GZT("somewhere you can write to (not Program Files) with about 1 GB free.")
              :                           GZT("Your copy couldn't be read, or it isn't Godzilla: Save the Earth for Xbox. ")
                                          GZT("Use the disc image (.iso / .xiso.iso) or the extracted folder with default.xbe.");
            gz_message_box(why, GZT("Godzilla: Save the Earth"), 1);
        }
        if (prepare || rc) {
            gz_host_free_args(argv);
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
        set_env_default("GZ_MOVIES", "0");
        gz_sprintf(w, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("mods") GZ_SEP GZT("biollante_slot"), s_exe_dir);
        gz_to_utf8(w, ov, sizeof ov);
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
            gzch w[GZ_PATH_MAX];
            char sv[GZ_PATH_MAX * 3];
            gz_sprintf(portable, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("portable.txt"), s_exe_dir);
            if (gz_file_exists(portable) || (game && gz_folder_writable(s_exe_dir))) {
                gz_sprintf(w, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("saves"), s_exe_dir);
            } else {
                gzch la[GZ_PATH_MAX];
                if (!gz_user_data_dir(la, GZ_PATH_MAX)) { gz_host_free_args(argv); gz_settings_apply_env(); gz_controls_load(); return -1; }
                gz_sprintf(w, GZ_PATH_MAX, GZT("%s") GZ_SEP GZT("GodzillaSTE"), la);
                if (!gz_dir_exists(w)) migrate_saves(la, w);
            }
            gz_to_utf8(w, sv, sizeof sv);
            _putenv_s("GZ_SAVE_DIR", sv);
        }
    }
    gz_settings_apply_env();
    gz_controls_load();
    gz_host_free_args(argv);
    return -1;
}
