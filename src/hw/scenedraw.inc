/* Draw-list builder (M6c / V3): included into hw.c after scene.inc. At line 192 it turns the scene recorder's bands into the draw list of scenedraw.h.
 *
 * The state of each band is reached by undoing the recorder's log backwards in place (the last band uses the live state minus the changes after line 167; an earlier band the state
 * with the log undone down to the band's start) and put back afterwards. For every band: the background cells that touch its rows (21 columns x 19 rows at most), the sprites (in
 * reverse table order) cut into runs of lines where the VDP draws them (a line draws at most 8 sprites, the first 8 in table order), the four colour tables. Tiles that were
 * rewritten between two bands get an extra atlas slot with the old data for the earlier band.
 *
 * What the GE renderer does not do and the game never needs is refused loudly: zoomed sprites and the vertical scroll lock (the VDP model supports them, this does not).
 */
#include "scenedraw.h"

static void sprites_build(HW *hw);
__attribute__((weak)) void rt_fatal(const char *msg);

void (*hw_scene_pre_build)(void);
void (*hw_scene_frame_cb)(const SdList *l, const SdAtlas *a);
int hw_scene_on = 1;               /* record and build (the PSP front end clears it unless the GE renderer is wanted) */

static struct {
    SdList list;
    SdAtlas atlas[2];
    int cur;
    uint16_t remap[512];                /* tile -> atlas slot for the band being built */
    uint8_t pending[512];               /* the tile's data changed since remap[] was set: the next band that draws it needs a slot made from the current state */
    SdAtlas *atlas_cur;
    int nextra;
    uint64_t frames, cells, sprites, extras, max_cells, max_extras;
} SD;

static void sd_fail(const char *msg)
{
    char buf[200];
    snprintf(buf, sizeof buf, "video: %s (frame %llu)", msg, (unsigned long long)SC.frames);
    if (rt_fatal) rt_fatal(buf);
    fprintf(stderr, "%s\n", buf);
    abort();
}

static void sd_redo(HW *t, const ScEv *e)
{
    if (e->kind == SC_VRAM) t->vram[e->a & 0x3FFF] = (uint8_t)e->nw;
    else if (e->kind == SC_CRAM) {
        t->cram[(e->a & 31) * 2] = (uint8_t)(e->nw & 0xFF);
        t->cram[(e->a & 31) * 2 + 1] = (uint8_t)(e->nw >> 8);
        t->pal_rgb[e->a & 31] = cram_to_rgb(e->nw);
    } else t->reg[e->a & 15] = (uint8_t)e->nw;
}

/* convert the tiles of the base atlas that changed since this atlas was last converted (state: the live VRAM of the last band) */
static void sd_sync_base(HW *hw, SdAtlas *a)
{
    for (int t = 0; t < 512; t++) {
        if (a->conv_seq[t] != SC.tile_seq[t] || !a->ready) {
            sd_convert_tile(hw->vram + t * 32, a, t);
            a->conv_seq[t] = SC.tile_seq[t];
        }
    }
    a->ready = 1;
}

/* the atlas slot that holds tile t as it is in the current (band) state: the base slot, or an extra slot made on demand when the tile was rewritten after the band */
#ifndef SD_BREAK
#define SD_BREAK 0                       /* negative controls of the draw-list check: 1 = no 8-sprites-per-line rule, 2 = no extra tile slots, 3 = sprites in table order, 4 = priority cells not redrawn */
#endif
static inline int sd_slot(HW *hw, int t)
{
    t &= 0x1FF;
    if (SD.pending[t] && SD_BREAK != 2) {
        if (SD.nextra >= 512) sd_fail("too many tile versions in one frame");
        int slot = 512 + SD.nextra++;
        sd_convert_tile(hw->vram + t * 32, SD.atlas_cur, slot);
        SD.remap[t] = (uint16_t)slot;
        SD.pending[t] = 0;
    }
    return SD.remap[t];
}

static void sd_band(HW *hw, SdList *l, int y0, int y1)
{
    SdBand *B = &l->band[l->nbands++];
    B->y0 = (uint8_t)y0; B->y1 = (uint8_t)y1;
    B->display_on = (uint8_t)(((hw->reg[1] & 0x40) && (hw->reg[0] & 0x04)) ? 1 : 0);
    for (int i = 0; i < 16; i++) {
        uint32_t p0 = hw->pal_rgb[i], p1 = hw->pal_rgb[16 + i];
        B->clut[SD_CLUT_BG0][i] = p0; B->clut[SD_CLUT_BG1][i] = p1;
        B->clut[SD_CLUT_SPR][i] = i ? p1 : 0u;
        B->clut[SD_CLUT_BG0T][i] = i ? p0 : 0u; B->clut[SD_CLUT_BG1T][i] = i ? p1 : 0u;
    }
    B->backdrop = hw->pal_rgb[16 + (hw->reg[7] & 0x0F)];
    for (int k = 0; k < 4; k++) { B->cell_first[k] = (uint16_t)l->ncells; B->cell_n[k] = 0; }
    B->spr_first = (uint16_t)l->nspr; B->spr_n = 0;
    if (!B->display_on) return;
    if (hw->reg[1] & 0x01) sd_fail("zoomed sprites are not supported by the GE renderer");
    if (hw->reg[0] & 0x80) sd_fail("the vertical scroll lock is not supported by the GE renderer");

    /* background: the cells that touch rows y0 .. y1 - 1, in four groups (palette, priority) */
    static SdCell grp[4][21 * 19];
    int ng[4] = {0, 0, 0, 0};
    unsigned nt = sc_nt_base(hw);
    int ys0 = (24 + hw->vscroll) % 224;
    int b0 = (48 - hw->reg[8]) & 255, col0 = b0 >> 3, fx = b0 & 7;
    for (int j = 0; j < 19; j++) {
        int py = -(ys0 & 7) + 8 * j;
        if (py + 8 <= y0 || py >= y1) continue;
        int row = ((ys0 >> 3) + j) % 28;
        for (int i = 0; i < 21; i++) {
            int px = -fx + 8 * i;
            if (px + 8 <= 0 || px >= 160) continue;
            unsigned ea = nt + (unsigned)(row * 32 + ((col0 + i) & 31)) * 2;
            unsigned ent = hw->vram[ea & 0x3FFF] | hw->vram[(ea + 1) & 0x3FFF] << 8;
            int pal = (int)((ent >> 11) & 1);
            SdCell cc;
            cc.x = (int16_t)px; cc.y = (int16_t)py; cc.slot = (uint16_t)sd_slot(hw, (int)(ent & 0x1FF));
            cc.flags = (uint8_t)(((ent >> 9) & 1) | ((ent >> 10) & 1) << 1); cc.pad = 0;
            grp[pal][ng[pal]++] = cc;                                          /* every cell goes into the opaque pass of its palette ... */
            if (((ent >> 12) & 1) && SD_BREAK != 4) grp[2 + pal][ng[2 + pal]++] = cc;             /* ... and a priority cell again into the pass after the sprites (colour 0 transparent) */
        }
    }
    for (int k = 0; k < 4; k++) {
        if (l->ncells + ng[k] > SD_MAX_CELLS) sd_fail("too many background cells in one frame");
        B->cell_first[k] = (uint16_t)l->ncells;
        memcpy(&l->cell[l->ncells], grp[k], (size_t)ng[k] * sizeof(SdCell));
        l->ncells += ng[k];
        B->cell_n[k] = (uint16_t)ng[k];
    }
    /* sprites */
    hw->spr_dirty = 1;
    sprites_build(hw);
    int tall = (hw->reg[1] >> 1) & 1, h = tall ? 16 : 8;
    unsigned sat = sc_sat_base(hw), spg = (hw->reg[6] & 0x04) ? 256u : 0;
    int xshift = (hw->reg[0] & 0x08) ? 8 : 0, nspr = 0;
    while (nspr < 64 && hw->vram[(sat + (unsigned)nspr) & 0x3FFF] != 0xD0) nspr++;
    for (int nn = 0; nn < nspr; nn++) {
        int n = SD_BREAK == 3 ? nn : nspr - 1 - nn;
        int sy = hw->vram[(sat + (unsigned)n) & 0x3FFF];
        int sx = hw->vram[(sat + 0x80 + 2 * (unsigned)n) & 0x3FFF] - xshift;
        unsigned tile = hw->vram[(sat + 0x81 + 2 * (unsigned)n) & 0x3FFF];
        int wx = sx - 48;
        if (wx <= -8 || wx >= 160) continue;
        int run_start = -1;                                                       /* k of the first line of the current run */
        for (int k = 0; k <= h; k++) {
            int drawn = 0;
            if (k < h) {
                int ly = (sy + 1 + k) & 255;
                if (ly >= 24 + y0 && ly < 24 + y1 && ly < 168) {
                    int cnt = hw->spr_cnt[ly] > 8 ? 8 : hw->spr_cnt[ly];
                    for (int q = 0; q < cnt; q++) if (hw->spr_list[ly][q] == n) { drawn = 1; break; }
                    if (SD_BREAK == 1) drawn = 1;
                }
            }
            int split = drawn && run_start >= 0 && tall && k == 8;                  /* a run does not cross from the first to the second tile of a tall sprite */
            if (drawn && !split) { if (run_start < 0) run_start = k; continue; }
            if (run_start >= 0) {
                int half = tall && run_start >= 8;
                unsigned t = tall ? ((tile & 0xFE) | (unsigned)half) : tile;
                if (l->nspr >= SD_MAX_SPR) sd_fail("too many sprite runs in one frame");
                SdSpr *s = &l->spr[l->nspr++];
                s->x = (int16_t)wx;
                s->y = (int16_t)(((sy + 1 + run_start) & 255) - 24);
                s->slot = (uint16_t)sd_slot(hw, (int)((spg + t) & 0x1FF));
                s->r0 = (uint8_t)(run_start & 7);
                s->nrows = (uint8_t)(k - run_start);
                B->spr_n++;
                run_start = -1;
            }
            if (split) run_start = k;                                                  /* the second tile's run starts on this line */
        }
    }
    hw->spr_dirty = 1;
}

/* build the list for the frame that just ended (line 192); hw is restored exactly as it was */
static void sd_build(HW *hw)
{
    SdList *l = &SD.list;
    if (hw_scene_pre_build) hw_scene_pre_build();
    SD.cur ^= 1;
    SdAtlas *a = &SD.atlas[SD.cur];
    l->nbands = l->ncells = l->nspr = 0;
    SD.nextra = 0;
    int pos = SC.nev, suffix = 0;
    while (pos > 0 && SC.ev[pos - 1].line >= 168) { sc_undo(hw, &SC.ev[--pos]); suffix++; }       /* changes after the last shown line */
    int pos_s = pos;
    sd_sync_base(hw, a);
    for (int t = 0; t < 512; t++) { SD.remap[t] = (uint16_t)t; SD.pending[t] = 0; }
    SD.atlas_cur = a;
    for (int b = SC.nbands - 1; b >= 0; b--) {
        int y0 = SC.band[b].start - 24, y1 = (b + 1 < SC.nbands ? SC.band[b + 1].start : 168) - 24;
        /* the state at the start of band b: undo every change logged after it (also the ones after the last cut that did not cut: they do not touch this band's pixels at the
         * band's start, but the state at the END of the frame would show them on the lines above them); a tile rewritten by an undone change needs a slot of its own if drawn */
        while (pos > SC.band[b].ev_end) {
            const ScEv *e = &SC.ev[--pos];
            sc_undo(hw, e);
            if (e->kind == SC_VRAM) SD.pending[(e->a >> 5) & 0x1FF] = 1;
        }
        sd_band(hw, l, y0, y1);
    }
    /* bands were built last to first; the list is order independent (disjoint rows), only the log has to be put back */
    for (int i = pos; i < SC.nev; i++) sd_redo(hw, &SC.ev[i]);
    hw->spr_dirty = 1;
    /* tiles rewritten after the last shown line were converted from their old data: convert them again next time */
    for (int i = pos_s; i < SC.nev; i++)
        if (SC.ev[i].kind == SC_VRAM) a->conv_seq[(SC.ev[i].a >> 5) & 0x1FF] = 0xFFFFFFFFu;
    (void)suffix;
    SD.frames++;
    SD.cells += (uint64_t)l->ncells; SD.sprites += (uint64_t)l->nspr; SD.extras += (uint64_t)SD.nextra;
    if ((uint64_t)l->ncells > SD.max_cells) SD.max_cells = (uint64_t)l->ncells;
    if ((uint64_t)SD.nextra > SD.max_extras) SD.max_extras = (uint64_t)SD.nextra;
#ifdef HW_SCENE_DRAWCHECK
    {
        static uint32_t pic[160 * 144];
        for (int i = 0; i < 160 * 144; i++) pic[i] = 0x00FF00FFu;
        sd_ref_raster(l, a, pic);
        for (int y = 0; y < 144; y++)
            if (memcmp(&pic[y * 160], &hw->fb[y * 160], 160 * sizeof(uint32_t)) != 0) {
                int x = 0;
                while (pic[y * 160 + x] == hw->fb[y * 160 + x]) x++;
                fprintf(stderr, "draw list check: frame %llu, window row %d (line %d) differs at x %d: list %08x, picture %08x; %d bands, %d cells, %d sprite runs, %d extra tile slots\n", (unsigned long long)hw->frame,
                        y, y + 24, x, pic[y * 160 + x], hw->fb[y * 160 + x], l->nbands, l->ncells, l->nspr, SD.nextra);
                {
                    int nb = 0;
                    for (int i = 0; i < SC.nev; i++) if (SC.ev[i].kind == SC_VRAM && ((SC.ev[i].a >> 5) & 0x1FF) == 209) nb++;
                    fprintf(stderr, "  tile 209: conv_seq %u tile_seq %u, %d logged writes to it; palette 0 colours:", a->conv_seq[209], SC.tile_seq[209], nb);
                    for (int i = 0; i < 4; i++) fprintf(stderr, " %08x", l->band[0].clut[0][i]);
                    fprintf(stderr, "\n  live vram tile 209 row 4:");
                    for (int i = 0; i < 4; i++) fprintf(stderr, " %02x", hw->vram[209 * 32 + 16 + i]);
                    fprintf(stderr, "; atlas row 4:");
                    for (int i = 0; i < 4; i++) fprintf(stderr, " %02x", a->data[((209 >> 5) * 8 + 4) * 128 + (209 & 31) * 4 + i]);
                    fprintf(stderr, "\n");
                }
                for (int b = 0; b < l->nbands; b++) {      /* which band and which draws touch the first wrong pixel */
                    const SdBand *B = &l->band[b];
                    if (y < B->y0 || y >= B->y1) continue;
                    fprintf(stderr, "  band %d rows %d..%d display %d;", b, B->y0, B->y1 - 1, B->display_on);
                    for (int k = 0; k < 4; k++)
                        for (int i = B->cell_first[k]; i < B->cell_first[k] + B->cell_n[k]; i++) {
                            const SdCell *c = &l->cell[i];
                            if (x >= c->x && x < c->x + 8 && y >= c->y && y < c->y + 8) fprintf(stderr, " cell[group %d] at (%d,%d) slot %d flags %d;", k, c->x, c->y, c->slot, c->flags);
                        }
                    for (int i = B->spr_first; i < B->spr_first + B->spr_n; i++) {
                        const SdSpr *s = &l->spr[i];
                        if (x >= s->x && x < s->x + 8 && y >= s->y && y < s->y + s->nrows) fprintf(stderr, " sprite run at (%d,%d) slot %d rows %d+%d;", s->x, s->y, s->slot, s->r0, s->nrows);
                    }
                    fprintf(stderr, "\n");
                }
                abort();
            }
        SC.draw_checked++;
    }
#endif
    if (hw_scene_frame_cb) hw_scene_frame_cb(l, a);
}
