/* High-score persistence (M8, D9). The Game Gear cartridge has no battery RAM: the BEST PLAYERS table and the time trial BEST LAP TIMES are set to defaults at every power-on
 * (the routine at $2123-$2167 runs once after reset). This PSP-side extra keeps them across runs. The game is not changed: the host looks at the two tables in the Z80 RAM at the
 * end of every frame, puts the saved values in as soon as the game has written its defaults, and asks for the file to be written a little after the tables change.
 *
 *   $CEC7-$CEF6   48 bytes  BEST LAP TIMES: 6 stages x (3 times as 16-bit words + a zero word)
 *   $CF07-$CF1A   20 bytes  BEST PLAYERS: 5 rows x (stage, 24-bit score in the game's own units)
 *
 * File: "DVHS", version byte 1, 68 table bytes, CRC32 of everything before it (little endian): 77 bytes. Pure logic, no file I/O: the hosts (native, PSP) read and write the file.
 */
#ifndef HISCORE_H
#define HISCORE_H
#include <stddef.h>
#include <stdint.h>

#define HS_BYTES 68
#define HS_FILE_BYTES 77

typedef struct {
    uint8_t saved[HS_BYTES];      /* what the file held */
    uint8_t last[HS_BYTES];       /* the tables at the end of the previous frame */
    int have;                     /* a valid file was loaded */
    int applied;                  /* the defaults were seen and the saved tables put in (or there was nothing to put in) */
    int dirty;                    /* the tables changed since they were last written */
    unsigned quiet, frames;
} HS;

void hs_init(HS *h);
int hs_load(HS *h, const uint8_t *file, size_t n);                 /* 1 if the file is valid */
void hs_image(const HS *h, uint8_t out[HS_FILE_BYTES]);             /* the file for the tables as they are now */
int hs_frame(HS *h, uint8_t *ram);                                  /* the end of a frame: 1 = write the file now (hs_image) */
void hs_restore(HS *h, uint8_t *ram);                               /* a save state was loaded: the tables of the file (or the current ones) go back into the RAM */
int hs_pending(const HS *h);                                        /* the tables changed and are not written yet (flush at exit) */

#endif
