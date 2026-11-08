/* Options of the PSP port (M9): kept in data/config.ini, changed in the pause menu (SELECT + START). Pure text handling, no file I/O (the front end reads and writes the file).
 *
 *   screen=fit | full | 1x | 1.5x     fit = 302 x 272 (the Game Gear's 10:9 at full height), full = 480 x 272 stretched, 1x = 160 x 144, 1.5x = 240 x 216 (all centred)
 *   filter=sharp | smooth             nearest neighbour or bilinear scaling
 *   volume=0..100                     percent
 *   buttons=normal | swapped          normal: Cross / Square = button 1, Circle / Triangle = button 2; swapped: the other way round
 * Unknown keys and bad values are ignored (the default stays); '#' starts a comment.
 */
#ifndef CONFIG_H
#define CONFIG_H
#include <stddef.h>

enum { SCREEN_FIT, SCREEN_FULL, SCREEN_1X, SCREEN_15X, SCREEN_MODES };

typedef struct {
    int screen;         /* SCREEN_* */
    int smooth;         /* 0 = nearest, 1 = bilinear */
    int volume;         /* 0..100 */
    int swapped;        /* 1 = buttons 1 and 2 swapped */
} Config;

void cfg_defaults(Config *c);
void cfg_parse(Config *c, const char *text);                       /* applies what it understands on top of *c */
size_t cfg_format(const Config *c, char *out, size_t cap);          /* the file text; returns its length (0 if cap is too small) */
void cfg_view(int screen, int *x, int *y, int *w, int *h);          /* the picture rectangle on the 480 x 272 screen */
const char *cfg_screen_name(int screen);

#endif
