#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

static const char *const screen_names[SCREEN_MODES] = { "fit", "full", "1x", "1.5x" };

const char *cfg_screen_name(int screen)
{
    return screen >= 0 && screen < SCREEN_MODES ? screen_names[screen] : screen_names[0];
}

void cfg_defaults(Config *c)
{
    c->screen = SCREEN_FIT;
    c->smooth = 0;
    c->volume = 100;
    c->swapped = 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

void cfg_parse(Config *c, const char *text)
{
    char line[96];
    const char *p = text;
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '\n') n++;
        size_t m = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, m);
        line[m] = 0;
        p += n;
        if (*p == '\n') p++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *val = trim(eq + 1);
        if (!strcmp(key, "screen")) {
            for (int i = 0; i < SCREEN_MODES; i++) if (!strcmp(val, screen_names[i])) c->screen = i;
        } else if (!strcmp(key, "filter")) {
            if (!strcmp(val, "sharp")) c->smooth = 0;
            else if (!strcmp(val, "smooth")) c->smooth = 1;
        } else if (!strcmp(key, "volume")) {
            char *end;
            long v = strtol(val, &end, 10);
            if (end != val && *end == 0 && v >= 0 && v <= 100) c->volume = (int)v;
        } else if (!strcmp(key, "buttons")) {
            if (!strcmp(val, "normal")) c->swapped = 0;
            else if (!strcmp(val, "swapped")) c->swapped = 1;
        }
    }
}

size_t cfg_format(const Config *c, char *out, size_t cap)
{
    int n = snprintf(out, cap,
                     "# Devilish PSP options (changed in the game with SELECT + START)\n"
                     "screen=%s          # fit | full | 1x | 1.5x\n"
                     "filter=%s         # sharp | smooth\n"
                     "volume=%d          # 0 - 100\n"
                     "buttons=%s       # normal | swapped\n",
                     cfg_screen_name(c->screen), c->smooth ? "smooth" : "sharp", c->volume, c->swapped ? "swapped" : "normal");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

void cfg_view(int screen, int *x, int *y, int *w, int *h)
{
    int vw, vh;
    switch (screen) {
    case SCREEN_FULL: vw = 480; vh = 272; break;
    case SCREEN_1X: vw = 160; vh = 144; break;
    case SCREEN_15X: vw = 240; vh = 216; break;
    default: vw = 302; vh = 272; break;
    }
    *x = (480 - vw) / 2; *y = (272 - vh) / 2; *w = vw; *h = vh;
}
