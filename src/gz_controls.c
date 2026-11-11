/*
 * gz_controls.c - controller remapping and deadzones, from gz_controls.ini
 * in the working folder (GZ_CONTROLS_FILE overrides the path).
 *
 * Each Xbox control takes one control of the player's pad, named by its
 * position (SDL's names: "south" is the bottom face button, an Xbox A or a
 * PlayStation cross). The file is written out with the defaults and comments
 * the first time the game starts. The toolkit's input layer applies it to
 * every pad (xbox_InputSetPadMap, input_map.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "input_map.h"

void xbox_InputSetPadMap(const struct InputPadMap *m);

static const char *controls_path(void)
{
    const char *p = getenv("GZ_CONTROLS_FILE");
    return (p && *p) ? p : "gz_controls.ini";
}

static void write_defaults(const char *path, const InputPadMap *m)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return;
    fputs("; Godzilla: Save the Earth recomp controller layout.\n"
          ";\n"
          "; Each Xbox control below takes one control of your pad, named by where it is:\n"
          ";   south, east, west, north   face buttons (Xbox A B X Y; PlayStation cross circle square triangle)\n"
          ";   left_shoulder, right_shoulder, left_trigger, right_trigger\n"
          ";   back, start, guide, left_stick_click, right_stick_click, touchpad, misc1, paddle1-4\n"
          ";   dpad_up, dpad_down, dpad_left, dpad_right\n"
          ";   left_stick_up / _down / _left / _right, right_stick_up / ... (a stick pushed one way)\n"
          ";   none                       nothing\n"
          "; black and white are the Xbox pad's two small buttons (the shoulders by default).\n"
          "; left_stick / right_stick: which of your sticks moves it (left, right or none).\n"
          "; Deadzones and the trigger threshold are percent of full travel (0 = none, as the console).\n"
          "; Delete this file to get the defaults back.\n\n", f);
    for (i = 0; i < INPUT_PAD_CONTROL_COUNT; i++)
        fprintf(f, "%s=%s\n", xbox_InputMap_ControlName(i), xbox_InputMap_SourceName(m->source[i]));
    fprintf(f, "left_stick=%s\n", m->left_stick == INPUT_STICK_LEFT ? "left" : m->left_stick == INPUT_STICK_RIGHT ? "right" : "none");
    fprintf(f, "right_stick=%s\n", m->right_stick == INPUT_STICK_LEFT ? "left" : m->right_stick == INPUT_STICK_RIGHT ? "right" : "none");
    fprintf(f, "deadzone_left=%d\ndeadzone_right=%d\ntrigger_threshold=%d\n",
            m->deadzone_left, m->deadzone_right, m->trigger_threshold);
    fclose(f);
}

static int stick_from(const char *v)
{
    if (!_stricmp(v, "left")) return INPUT_STICK_LEFT;
    if (!_stricmp(v, "right")) return INPUT_STICK_RIGHT;
    if (!_stricmp(v, "none")) return INPUT_STICK_NONE;
    return -1;
}

static int pct(const char *v)
{
    int n = atoi(v);
    return n < 0 ? 0 : n > 95 ? 95 : n;
}

void gz_controls_load(void)
{
    InputPadMap m;
    FILE *f;
    char line[256];
    int bad = 0, changed = 0;
    xbox_InputMap_PadMapDefaults(&m);
    m.deadzone_left = m.deadzone_right = m.trigger_threshold = 0;   /* as the console */
    if ((f = fopen(controls_path(), "r")) == NULL) {
        write_defaults(controls_path(), &m);
        xbox_InputSetPadMap(&m);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *k = line, *v, *e;
        int c, s;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == ';' || *k == '#' || *k == '[' || (v = strchr(k, '=')) == NULL) continue;
        for (e = v; e > k && isspace((unsigned char)e[-1]); e--) {}
        *e = 0;
        for (v++; *v == ' ' || *v == '\t'; v++) {}
        for (e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); e--) {}
        *e = 0;
        if ((c = xbox_InputMap_ControlFromName(k)) >= 0 && c < INPUT_PAD_CONTROL_COUNT) {
            if ((s = xbox_InputMap_SourceFromName(v)) >= 0) {
                changed |= m.source[c] != (uint8_t)s;
                m.source[c] = (uint8_t)s;
            } else bad++;
        } else if (!strcmp(k, "left_stick") || !strcmp(k, "right_stick")) {
            if ((s = stick_from(v)) >= 0) {
                uint8_t *dst = k[0] == 'l' ? &m.left_stick : &m.right_stick;
                changed |= *dst != (uint8_t)s;
                *dst = (uint8_t)s;
            } else bad++;
        } else if (!strcmp(k, "deadzone_left")) { m.deadzone_left = (uint8_t)pct(v); changed |= m.deadzone_left != 0; }
        else if (!strcmp(k, "deadzone_right")) { m.deadzone_right = (uint8_t)pct(v); changed |= m.deadzone_right != 0; }
        else if (!strcmp(k, "trigger_threshold")) { m.trigger_threshold = (uint8_t)pct(v); changed |= m.trigger_threshold != 0; }
        else bad++;
    }
    fclose(f);
    if (bad) fprintf(stderr, "[INPUT] %s: %d line(s) not understood, left at the default\n", controls_path(), bad);
    if (changed) fprintf(stderr, "[INPUT] controller layout from %s\n", controls_path());
    xbox_InputSetPadMap(&m);
}
