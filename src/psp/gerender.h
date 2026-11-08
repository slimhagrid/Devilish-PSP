/* GE backend of the draw list (src/psp/gerender.c) */
#ifndef GERENDER_H
#define GERENDER_H
#include <stdint.h>
#include "../hw/scenedraw.h"

void ge_init(void);
void ge_set_screen(unsigned vram_offset);                    /* the display buffer (offset in VRAM) the next frames are stretched into; call before each frame's draw */
void ge_set_view(int x, int y, int w, int h, int smooth);   /* where the 160 x 144 picture goes on the 480 x 272 screen (options, M9); the rest of the screen is cleared to black for the next frames */
void ge_clear_screen(void);                                 /* clear the screen around the picture again (something was drawn over it by the front end) */
void ge_wait(void);                                         /* waits until the GE has finished what it was given */
void ge_draw(const SdList *l, const SdAtlas *a);            /* builds and submits the display list for one frame (asynchronous) */
void ge_readback(uint32_t *dst);                            /* waits, then copies the 160 x 144 target to dst (ABGR words) */
#endif
