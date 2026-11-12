/*
 * gz_settings.c - the recomp's own settings, in gz_settings.ini.
 *
 * Five are shown as extra entries on the game's Options screen (see "Options
 * menu toggles" in recomp_manual.c):
 *   tournament  every monster and city unlocked, whatever the profile holds
 *   ps_prompts  button pictures: 1 = cross/circle/square/triangle, 0 = Xbox
 *   widescreen  1 = 16:9 picture with a wider view, 0 = the original 4:3
 *   frame_rate  the game's frame rate setting (60 on the console); the menu
 *               entry steps through 60 / 120 / 144. Higher rates were too
 *               unstable in play; a saved or GZ_RATE value above 144 is
 *               treated as 144.
 *   turbo       1 = the game runs 1.2x faster (a player suggestion: the pace
 *               of the old high-frame-rate speed bug); never in System Link
 *               or rollback netplay
 * More are only in the file (and in RecompLauncher's settings):
 *   online       1 = System Link over the network (GZ_NET)
 *   online_peer  the other players' IP addresses (GZ_NET_PEER)
 *   online_60fps 1 = network matches at 60 frames a second (default), 0 = 30
 *                as on the console (GZ_NET60 overrides)
 *
 * Button icons default to the controller that is plugged in the first time
 * the game starts (RECOMPLAUNCHER_PAD when RecompLauncher started it; an Xbox
 * or other non-PlayStation pad SDL3 sees picks Xbox icons), else PlayStation.
 *
 * The file is in the working folder (GZ_SETTINGS_FILE overrides the path).
 * It is written out with every key and a comment for each the first time the
 * game starts; after that only the lines of changed keys are rewritten, so
 * comments and other lines stay. GZ_TOURNAMENT / GZ_PS_PROMPTS / GZ_WIDE /
 * GZ_TURBO (0 or 1) and GZ_RATE (a number) override the file for that run (tests use them); a
 * change made in the menu still applies and is saved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* the toolkit's input layer: which pad is plugged in */
int xbox_InputPadIsPlayStation(unsigned long port);

enum { OPT_TOURNAMENT, OPT_PROMPTS, OPT_RATE, OPT_WIDE, OPT_TURBO, OPT_COUNT };

static int s_val[OPT_COUNT] = { 0, 1, 60, 0, 0 };
static const int s_rates[] = { 60, 120, 144 };
#define GZ_RATE_MAX 144
static const char *const s_key[OPT_COUNT] = { "tournament", "ps_prompts", "frame_rate", "widescreen", "turbo" };
static const char *const s_env[OPT_COUNT] = { "GZ_TOURNAMENT", "GZ_PS_PROMPTS", "GZ_RATE", "GZ_WIDE", "GZ_TURBO" };
static int s_loaded;
static volatile unsigned s_generation = 1;

#define HEADER_MARK "; Godzilla: Save the Earth recomp settings"
static const char *const s_header[] = {
    HEADER_MARK " (also on the game's Options screen, last entries).",
    "; RecompLauncher edits this file too; comments and unknown lines are kept.",
    ";",
    "; tournament   1 = every monster and city unlocked, 0 = what your profile has earned",
    "; ps_prompts   button icons: 1 = PlayStation, 0 = Xbox",
    "; frame_rate   60 (the console's), 120 or 144; game speed stays real time",
    "; widescreen   1 = 16:9 with a wider view, 0 = the original 4:3",
    "; turbo        1 = the game runs 1.2x faster (not in System Link), 0 = normal speed",
    "; online       1 = System Link over the network (Multi-Player > System Link)",
    "; online_peer  the other player's IP address (several: comma separated);",
    ";              empty on the same home network. UDP ports 1000-1002 must be open.",
    "; online_60fps 1 = network matches at 60 frames a second, 0 = 30 as on the console;",
    ";              both players need the same (otherwise joining says the versions differ)",
    NULL
};

static const char *settings_path(void)
{
    const char *p = getenv("GZ_SETTINGS_FILE");
    return (p && *p) ? p : "gz_settings.ini";
}

/* "  key = value  \n" -> key and value without the spaces. 0 for comments,
 * section headers and lines without '='. Modifies line. */
static int split_line(char *line, char **key, char **val)
{
    char *eq, *e;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == ';' || *line == '#' || *line == '[' || (eq = strchr(line, '=')) == NULL) return 0;
    for (e = eq; e > line && isspace((unsigned char)e[-1]); e--) {}
    *e = 0;
    for (eq++; *eq == ' ' || *eq == '\t'; eq++) {}
    for (e = eq + strlen(eq); e > eq && isspace((unsigned char)e[-1]); e--) {}
    *e = 0;
    *key = line;
    *val = eq;
    return 1;
}

/* One key's value from the file, "" if it isn't there. */
static void read_key(const char *want, char *out, size_t n)
{
    FILE *f = fopen(settings_path(), "r");
    char line[512], *k, *v;
    out[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f))
        if (split_line(line, &k, &v) && !strcmp(k, want)) { snprintf(out, n, "%s", v); break; }
    fclose(f);
}

/* Writes the file back: lines of known keys get the current values, every
 * other line stays, and keys that are missing are added. A file without the
 * comment block gets it at the top. */
static void save(void)
{
    char lines[64][512], net[8], peer[256], buf[512], *k, *v;
    int n = 0, i, j, done[OPT_COUNT] = { 0 }, have_header = 0, have_net = 0, have_peer = 0, have_n60 = 0;
    FILE *f = fopen(settings_path(), "r");
    if (f) {
        while (n < 64 && fgets(lines[n], sizeof lines[n], f)) {
            size_t l = strlen(lines[n]);
            while (l && (lines[n][l - 1] == '\n' || lines[n][l - 1] == '\r')) lines[n][--l] = 0;
            if (!strncmp(lines[n], HEADER_MARK, sizeof HEADER_MARK - 1)) have_header = 1;
            n++;
        }
        fclose(f);
    }
    if ((f = fopen(settings_path(), "w")) == NULL) return;
    if (!have_header)
        for (i = 0; s_header[i]; i++) fprintf(f, "%s\n", s_header[i]);
    for (j = 0; j < n; j++) {
        snprintf(buf, sizeof buf, "%s", lines[j]);
        if (split_line(buf, &k, &v)) {
            for (i = 0; i < OPT_COUNT; i++)
                if (!strcmp(k, s_key[i])) {
                    if (!done[i]) fprintf(f, "%s=%d\n", s_key[i], s_val[i]);
                    done[i] = 1;
                    break;
                }
            if (i < OPT_COUNT) continue;
            if (!strcmp(k, "online")) have_net = 1;
            if (!strcmp(k, "online_peer")) have_peer = 1;
            if (!strcmp(k, "online_60fps")) have_n60 = 1;
        }
        fprintf(f, "%s\n", lines[j]);
    }
    for (i = 0; i < OPT_COUNT; i++)
        if (!done[i]) fprintf(f, "%s=%d\n", s_key[i], s_val[i]);
    if (!have_net) {
        const char *e = getenv("GZ_NET");
        snprintf(net, sizeof net, "%s", (e && *e == '1') ? "1" : "0");
        fprintf(f, "online=%s\n", net);
    }
    if (!have_n60) fprintf(f, "online_60fps=1\n");
    if (!have_peer) {
        const char *e = getenv("GZ_NET_PEER");
        snprintf(peer, sizeof peer, "%s", e ? e : "");
        fprintf(f, "online_peer=%s\n", peer);
    }
    fclose(f);
}

static void load(void)
{
    FILE *f;
    char line[512], *k, *v;
    int i, have_prompts = 0, need_write = 1;
    if (s_loaded) return;
    s_loaded = 1;
    if ((f = fopen(settings_path(), "r")) != NULL) {
        int have_header = 0, have_all = 0;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, HEADER_MARK, sizeof HEADER_MARK - 1)) have_header = 1;
            if (!split_line(line, &k, &v)) continue;
            for (i = 0; i < OPT_COUNT; i++)
                if (!strcmp(k, s_key[i])) {
                    s_val[i] = (i == OPT_RATE) ? atoi(v) : atoi(v) != 0;
                    if (i == OPT_PROMPTS) have_prompts = 1;
                    have_all |= 1 << i;
                }
            if (!strcmp(k, "online")) have_all |= 1 << OPT_COUNT;
            if (!strcmp(k, "online_peer")) have_all |= 2 << OPT_COUNT;
            if (!strcmp(k, "online_60fps")) have_all |= 4 << OPT_COUNT;
        }
        fclose(f);
        need_write = !have_header || have_all != (1 << (OPT_COUNT + 3)) - 1;
    }
    /* Started from RecompLauncher before the player picked button icons: show
     * the controller that is plugged in. */
    if (!have_prompts) {
        const char *pad = getenv("RECOMPLAUNCHER_PAD");
        if (pad && *pad) s_val[OPT_PROMPTS] = !strcmp(pad, "playstation");
        else {
            int ps = xbox_InputPadIsPlayStation(0);
            if (ps >= 0) s_val[OPT_PROMPTS] = ps;
        }
    }
    if (s_val[OPT_RATE] < 20 || s_val[OPT_RATE] > 1000) s_val[OPT_RATE] = 60;
    if (s_val[OPT_RATE] > GZ_RATE_MAX) {
        fprintf(stderr, "[SETTINGS] frame_rate %d is above the maximum; using %d\n", s_val[OPT_RATE], GZ_RATE_MAX);
        s_val[OPT_RATE] = GZ_RATE_MAX;
    }
    /* every key written out, with the comments (before the overrides below,
     * which are for this run only) */
    if (need_write) save();
    for (i = 0; i < OPT_COUNT; i++) {
        const char *e = getenv(s_env[i]);
        if (i == OPT_RATE) { if (e && atoi(e) > 0) s_val[i] = atoi(e); }
        else if (e && (*e == '0' || *e == '1')) s_val[i] = *e == '1';
    }
    if (s_val[OPT_RATE] > GZ_RATE_MAX) s_val[OPT_RATE] = GZ_RATE_MAX;
    fprintf(stderr, "[SETTINGS] tournament=%d ps_prompts=%d frame_rate=%d widescreen=%d turbo=%d (%s)\n",
            s_val[OPT_TOURNAMENT], s_val[OPT_PROMPTS], s_val[OPT_RATE], s_val[OPT_WIDE], s_val[OPT_TURBO],
            settings_path());
}

/* online / online_peer from the file into GZ_NET / GZ_NET_PEER, unless the
 * environment (play.bat) already sets them. Called at start-up. */
void gz_settings_apply_env(void)
{
    char v[256];
    const char *e = getenv("GZ_NET");
    if (!e || !*e) {
        read_key("online", v, sizeof v);
        if (v[0] == '1') _putenv_s("GZ_NET", "1");
    }
    e = getenv("GZ_NET_PEER");
    if (!e || !*e) {
        read_key("online_peer", v, sizeof v);
        if (v[0]) _putenv_s("GZ_NET_PEER", v);
    }
}

/* Network matches at 60 frames a second: online_60fps (default 1), GZ_NET60
 * overrides. Read once. */
int gz_opt_net60(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GZ_NET60");
        char t[16];
        if (e && (*e == '0' || *e == '1')) v = *e == '1';
        else { read_key("online_60fps", t, sizeof t); v = t[0] ? t[0] == '1' : 1; }
    }
    return v;
}

int gz_opt_get(int which)
{
    load();
    return (which >= 0 && which < OPT_COUNT) ? s_val[which] : 0;
}

void gz_opt_toggle(int which)
{
    load();
    if (which < 0 || which >= OPT_COUNT) return;
    if (which == OPT_RATE) {
        /* next entry of the list above the current value, wrapping to 60 */
        int i, n = (int)(sizeof s_rates / sizeof s_rates[0]), next = s_rates[0];
        for (i = 0; i < n; i++) if (s_rates[i] > s_val[which]) { next = s_rates[i]; break; }
        s_val[which] = next;
    } else
        s_val[which] = !s_val[which];
    if (which == OPT_PROMPTS) s_generation++;   /* textures decode again */
    save();
    fprintf(stderr, "[SETTINGS] %s=%d\n", s_key[which], s_val[which]);
}

int gz_opt_tournament(void) { return gz_opt_get(OPT_TOURNAMENT); }
int gz_opt_ps_prompts(void) { return gz_opt_get(OPT_PROMPTS); }
int gz_opt_rate(void)       { return gz_opt_get(OPT_RATE); }
int gz_opt_wide(void)       { return gz_opt_get(OPT_WIDE); }
int gz_opt_turbo(void)      { return gz_opt_get(OPT_TURBO); }

/* Changes whenever a setting changes (the texture cache re-checks on it). */
unsigned gz_opt_generation(void) { return s_generation; }
