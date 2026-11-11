/*
 * gz_settings.c - the recomp's own settings that can be changed in game.
 *
 * Three settings, shown as extra entries on the game's Options screen (see
 * "Options menu toggles" in recomp_manual.c):
 *   tournament  every monster and city unlocked, whatever the profile holds
 *   prompts     button pictures: 1 = cross/circle/square/triangle, 0 = Xbox
 *   widescreen  1 = 16:9 picture with a wider view, 0 = the original 4:3
 *   frame_rate  the game's frame rate setting (60 on the console); the menu
 *               entry steps through 60 / 120 / 144. Higher rates were too
 *               unstable in play; a saved or GZ_RATE value above 144 is
 *               treated as 144.
 *
 * They are kept in gz_settings.ini in the working folder (GZ_SETTINGS_FILE
 * overrides the path). GZ_TOURNAMENT / GZ_PS_PROMPTS / GZ_WIDE (0 or 1) and GZ_RATE
 * (a number) override the file for that run (tests use them); a change
 * made in the menu still applies and is saved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OPT_TOURNAMENT, OPT_PROMPTS, OPT_RATE, OPT_WIDE, OPT_COUNT };

static int s_val[OPT_COUNT] = { 0, 1, 60, 0 };
static const int s_rates[] = { 60, 120, 144 };
#define GZ_RATE_MAX 144
static const char *const s_key[OPT_COUNT] = { "tournament", "ps_prompts", "frame_rate", "widescreen" };
static const char *const s_env[OPT_COUNT] = { "GZ_TOURNAMENT", "GZ_PS_PROMPTS", "GZ_RATE", "GZ_WIDE" };
static int s_loaded;
static volatile unsigned s_generation = 1;

static const char *settings_path(void)
{
    const char *p = getenv("GZ_SETTINGS_FILE");
    return (p && *p) ? p : "gz_settings.ini";
}

static void load(void)
{
    FILE *f;
    char line[128];
    int i;
    if (s_loaded) return;
    s_loaded = 1;
    if ((f = fopen(settings_path(), "r")) != NULL) {
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            for (i = 0; i < OPT_COUNT; i++)
                if (!strcmp(line, s_key[i])) s_val[i] = (i == OPT_RATE) ? atoi(eq + 1) : atoi(eq + 1) != 0;
        }
        fclose(f);
    }
    for (i = 0; i < OPT_COUNT; i++) {
        const char *e = getenv(s_env[i]);
        if (i == OPT_RATE) { if (e && atoi(e) > 0) s_val[i] = atoi(e); }
        else if (e && (*e == '0' || *e == '1')) s_val[i] = *e == '1';
    }
    if (s_val[OPT_RATE] < 20 || s_val[OPT_RATE] > 1000) s_val[OPT_RATE] = 60;
    if (s_val[OPT_RATE] > GZ_RATE_MAX) {
        fprintf(stderr, "[SETTINGS] frame_rate %d is above the maximum; using %d\n", s_val[OPT_RATE], GZ_RATE_MAX);
        s_val[OPT_RATE] = GZ_RATE_MAX;
    }
    fprintf(stderr, "[SETTINGS] tournament=%d ps_prompts=%d frame_rate=%d widescreen=%d (%s)\n",
            s_val[OPT_TOURNAMENT], s_val[OPT_PROMPTS], s_val[OPT_RATE], s_val[OPT_WIDE], settings_path());
}

static void save(void)
{
    FILE *f = fopen(settings_path(), "w");
    int i;
    if (!f) return;
    for (i = 0; i < OPT_COUNT; i++) fprintf(f, "%s=%d\n", s_key[i], s_val[i]);
    fclose(f);
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

/* Changes whenever a setting changes (the texture cache re-checks on it). */
unsigned gz_opt_generation(void) { return s_generation; }
