/* Draw list of one frame (M6c / V3 in PLAN.md): what a tile / sprite renderer (the GE on the PSP, tests/psg-style reference rasteriser on the PC) needs to draw the 160x144 picture
 * of a frame that the line-by-line VDP renderer drew. Built at line 192 from the scene recorder's bands (src/hw/scene.inc, src/hw/scenedraw.inc).
 *
 * The picture is a set of BANDS (window rows y0..y1-1, 0 = line 24 of the VDP picture). Each band has its own colour tables and its own cells and sprites, and is drawn with a
 * scissor on its rows. In a band: the backdrop colour only (display off), or the background cells (8x8 tiles from the atlas, with flips and one of two palettes), then the
 * sprites (rows of 8-pixel wide tile slices, colour 0 transparent, the last in the list drawn first so that the lowest sprite number ends up on top), then the cells that have
 * the background-priority flag again (colour 0 transparent), which hides the sprite pixels under them.
 *
 * The tile atlas holds 1024 tiles of 8x8 pixels, 4 bits per pixel, linear, 256 pixels wide (32 tiles per row): slots 0-511 are the VDP's tiles as of the last band of the frame,
 * slots 512-1023 hold the older versions of tiles that were rewritten during the frame (a band before the rewrite draws the old version from an extra slot).
 */
#ifndef SCENEDRAW_H
#define SCENEDRAW_H
#include <stdint.h>

#define SD_ATLAS_TILES 1024
#define SD_ATLAS_W 256                  /* pixels */
#define SD_ATLAS_BYTES (SD_ATLAS_TILES * 32)
#define SD_MAX_BANDS 144
#define SD_MAX_CELLS 20000
#define SD_MAX_SPR 8192

#define SD_CLUT_BG0 0                   /* background palette 0 (colours 0-15 of the VDP's colour RAM), opaque */
#define SD_CLUT_BG1 1                   /* background palette 1 (16-31), opaque */
#define SD_CLUT_SPR 2                   /* sprite palette (16-31), colour 0 transparent */
#define SD_CLUT_BG0T 3                  /* background palette 0 with colour 0 transparent (the priority cells) */
#define SD_CLUT_BG1T 4

#define SD_FLIP_H 1
#define SD_FLIP_V 2

typedef struct { int16_t x, y; uint16_t slot; uint8_t flags, pad; } SdCell;                   /* an 8x8 cell at window position (x, y); may lie partly outside */
typedef struct { int16_t x, y; uint16_t slot; uint8_t r0, nrows; } SdSpr;                     /* tile rows r0 .. r0 + nrows - 1 of the slot, 8 pixels wide, at window position (x, y) */

typedef struct {
    uint8_t y0, y1;                     /* window rows [y0, y1) */
    uint8_t display_on;
    uint32_t backdrop;                  /* ABGR, used when the display is off */
    uint32_t clut[5][16] __attribute__((aligned(16)));
    uint16_t cell_first[4], cell_n[4];  /* [0] palette 0, [1] palette 1 (opaque cells), [2] palette 0, [3] palette 1 (priority cells, drawn after the sprites) */
    uint16_t spr_first, spr_n;
} SdBand;

typedef struct {
    int nbands, ncells, nspr;
    SdBand band[SD_MAX_BANDS];
    SdCell cell[SD_MAX_CELLS];
    SdSpr spr[SD_MAX_SPR];
} SdList;

typedef struct {
    uint8_t data[SD_ATLAS_BYTES] __attribute__((aligned(64)));
    uint32_t conv_seq[512];             /* the pattern-write sequence number each base tile was converted at (see scene.inc) */
    uint8_t ready;
} SdAtlas;

/* converts one VDP tile (32 bytes of bit planes) into atlas slot `slot` */
void sd_convert_tile(const uint8_t *vram_tile, SdAtlas *a, int slot);
/* reference rasteriser: draws the list into a 160 x 144 buffer of ABGR words (src/hw/scenedraw_ref.c); the bands cover rows the buffer must not otherwise be assumed to hold */
void sd_ref_raster(const SdList *l, const SdAtlas *a, uint32_t *fb);

/* hooks (src/hw/hw.c): called at line 192 of every frame when the scene recorder is on (hw_scene_on != 0). pre_build: the atlas that is about to be rewritten must not be in
 * use any more (the GE backend waits here); frame_cb: the finished list and its atlas. */
struct HW;
extern int hw_scene_on;
extern void (*hw_scene_pre_build)(void);
extern void (*hw_scene_frame_cb)(const SdList *l, const SdAtlas *a);
#endif
