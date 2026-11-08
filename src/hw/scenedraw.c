/* Tile conversion and the reference rasteriser of the draw list (scenedraw.h). The rasteriser is what the GE is supposed to do, written as plain C: tests compare its output with
 * the picture of the line-by-line renderer (the PC), and the GE's output is compared with the same pictures in PPSSPP. */
#include <string.h>
#include "scenedraw.h"

/* a VDP tile is 8 rows of 4 bytes (bit planes 0-3 of the row, bit 7 = leftmost pixel); the atlas keeps 4 bits per pixel, the left pixel in the low nibble (the PSP's 4-bit texture order) */
void sd_convert_tile(const uint8_t *t, SdAtlas *a, int slot)
{
    uint8_t *d = a->data + ((slot >> 5) * 8 * (SD_ATLAS_W / 2)) + (slot & 31) * 4;
    for (int r = 0; r < 8; r++, t += 4, d += SD_ATLAS_W / 2) {
        for (int k = 0; k < 4; k++) {
            int x0 = 2 * k, x1 = 2 * k + 1;
            unsigned c0 = (t[0] >> (7 - x0) & 1) | (t[1] >> (7 - x0) & 1) << 1 | (t[2] >> (7 - x0) & 1) << 2 | (t[3] >> (7 - x0) & 1) << 3;
            unsigned c1 = (t[0] >> (7 - x1) & 1) | (t[1] >> (7 - x1) & 1) << 1 | (t[2] >> (7 - x1) & 1) << 2 | (t[3] >> (7 - x1) & 1) << 3;
            d[k] = (uint8_t)(c0 | c1 << 4);
        }
    }
}

static inline unsigned texel(const SdAtlas *a, int slot, int tx, int ty)     /* pixel (tx, ty) of a tile slot */
{
    int ax = (slot & 31) * 8 + tx, ay = (slot >> 5) * 8 + ty;
    uint8_t b = a->data[ay * (SD_ATLAS_W / 2) + ax / 2];
    return (ax & 1) ? (unsigned)(b >> 4) : (unsigned)(b & 15);
}

static void cells(const SdList *l, const SdAtlas *a, const SdBand *B, int first, int n, int clut, int transparent, uint32_t *fb)
{
    for (int i = first; i < first + n; i++) {
        const SdCell *c = &l->cell[i];
        for (int ty = 0; ty < 8; ty++) {
            int y = c->y + ty;
            if (y < B->y0 || y >= B->y1) continue;
            for (int tx = 0; tx < 8; tx++) {
                int x = c->x + tx;
                if (x < 0 || x >= 160) continue;
                unsigned ix = texel(a, c->slot, (c->flags & SD_FLIP_H) ? 7 - tx : tx, (c->flags & SD_FLIP_V) ? 7 - ty : ty);
                if (transparent && !ix) continue;
                fb[y * 160 + x] = B->clut[clut][ix];
            }
        }
    }
}

void sd_ref_raster(const SdList *l, const SdAtlas *a, uint32_t *fb)
{
    for (int b = 0; b < l->nbands; b++) {
        const SdBand *B = &l->band[b];
        if (!B->display_on) {
            for (int y = B->y0; y < B->y1; y++)
                for (int x = 0; x < 160; x++) fb[y * 160 + x] = B->backdrop;
            continue;
        }
        cells(l, a, B, B->cell_first[0], B->cell_n[0], SD_CLUT_BG0, 0, fb);
        cells(l, a, B, B->cell_first[1], B->cell_n[1], SD_CLUT_BG1, 0, fb);
        for (int i = B->spr_first; i < B->spr_first + B->spr_n; i++) {
            const SdSpr *s = &l->spr[i];
            for (int r = 0; r < s->nrows; r++) {
                int y = s->y + r;
                if (y < B->y0 || y >= B->y1) continue;
                for (int tx = 0; tx < 8; tx++) {
                    int x = s->x + tx;
                    if (x < 0 || x >= 160) continue;
                    unsigned ix = texel(a, s->slot, tx, s->r0 + r);
                    if (ix) fb[y * 160 + x] = B->clut[SD_CLUT_SPR][ix];
                }
            }
        }
        cells(l, a, B, B->cell_first[2], B->cell_n[2], SD_CLUT_BG0T, 1, fb);
        cells(l, a, B, B->cell_first[3], B->cell_n[3], SD_CLUT_BG1T, 1, fb);
    }
}
