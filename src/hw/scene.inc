/* Scene recorder (M6c / V2 in PLAN.md): included into hw.c in a build with -DHW_SCENE. It records, for the shown lines of every frame (the Game Gear shows lines 24-167),
 * what a renderer that draws a whole frame at once (the GE, drawing at line 192) needs in order to give every line the picture the line-by-line renderer gave it:
 *
 *   - an UNDO LOG of every change to the picture state during lines 24-191: VRAM bytes (old value), colour words, the registers the picture depends on;
 *   - a list of BANDS: runs of shown lines with the same state. A band starts at the first line that can see a change that matters. The state of the LAST band is the live
 *     state at line 192; the state of an earlier band is obtained by undoing the log backwards (so a frame without a cut costs nothing but the log of what changed).
 *
 * What cuts a band (this is the rule the renderer relies on; HW_SCENE_CHECK proves it): a change of a colour, of a picture register (scroll, display enable, table bases,
 * sprite size / pattern bank, locks), of a name-table byte or sprite-table byte, or of a tile pattern that a name-table cell or sprite refers to. Writes to tiles that nothing
 * refers to do not cut (but are logged, because a later name-table write can make them matter). A change that happens during line L before its render point affects line
 * L; after the render point, line L + 1.
 *
 * HW_SCENE_CHECK (test builds): at line 192 the live state is copied, rolled back band by band with the undo log, each band's lines are drawn with the ordinary line renderer
 * into a cleared frame buffer, and the result must equal the frame that was really drawn. Any difference stops the program with the frame, the band list and the first line.
 */
#include <stdio.h>
#include <stdlib.h>
#include "scenedraw.h"

#ifndef HW_NO_PIXELS
static void render_line_impl(HW *hw, int y);
#endif

#define SC_MAX_EV 32768
#define SC_MAX_BANDS 144                    /* one per shown line at most: a cut on the line a band already starts at only extends it */
enum { SC_VRAM, SC_CRAM, SC_REG };
enum { SC_K_NT, SC_K_SAT, SC_K_PAT, SC_K_CRAM, SC_K_REG, SC_NK };
typedef struct { uint8_t kind, line; uint16_t a; uint16_t old, nw; } ScEv;
typedef struct { uint8_t start; uint16_t ev_end; } ScBand;       /* first line; log entries applied at its start */

static struct {
    int active;
    int nev, nbands, overflow;
    ScEv ev[SC_MAX_EV];
    ScBand band[SC_MAX_BANDS];
    uint8_t last_line[512];              /* per tile: the last shown line that a visible name-table cell or a visible sprite draws it on (0 = not used); valid only when refs_valid */
    int refs_valid;
    uint64_t frames, bands_hist[12], cuts[SC_NK], max_ev, overflows, checked, max_bands, draw_checked;
    uint32_t tile_seq[512], seq;         /* per VDP tile: the sequence number of the last value-changing write to its 32 bytes (the atlas converts tiles whose number moved) */
} SC;
static void sd_build(HW *hw);
static void sc_state_restored(void)      /* a save state was loaded: VRAM changed without writes the recorder saw, so every tile counts as rewritten */
{
    for (int i = 0; i < 512; i++) SC.tile_seq[i] = ++SC.seq;
    SC.refs_valid = 0;
}

static const char *sc_kind_name[SC_NK] = {"name table", "sprite table", "tile pattern in use", "colour", "register"};

static unsigned sc_reg_mask(int r)
{
    switch (r) {
    case 0: return 0xEC;                  /* scroll locks, left column blank, sprite shift, mode 4 */
    case 1: return 0x43;                  /* display on, 8x16 sprites, zoom */
    case 2: return 0x0E;
    case 5: return 0x7E;
    case 6: return 0x04;
    case 7: return 0x0F;
    case 8: return 0xFF;
    default: return 0;                    /* register 9 is latched at the start of the frame by the model; 3, 4, 10 and the rest do not draw */
    }
}

static inline unsigned sc_nt_base(const HW *hw) { return (unsigned)(hw->reg[2] & 0x0E) << 10; }
static inline unsigned sc_sat_base(const HW *hw) { return (unsigned)(hw->reg[5] & 0x7E) << 7; }

/* the tiles that something visible refers to: the name-table cells that cover the shown lines at the current scroll (21 columns, 19 rows at most) and the sprites that are in
 * the table before its end marker and overlap the shown window; computed on demand (a name-table, sprite-table or register change invalidates it) */
static void sc_recount(const HW *hw)
{
    memset(SC.last_line, 0, sizeof SC.last_line);
    unsigned nt = sc_nt_base(hw), sat = sc_sat_base(hw), spg = (hw->reg[6] & 0x04) ? 256u : 0;
    int ys = (24 + hw->vscroll) % 224, rows = ((ys & 7) + 143) / 8 + 1;
    int b0 = (48 - hw->reg[8]) & 255, cols = ((b0 & 7) + 159) / 8 + 1;
    if (hw->reg[0] & 0x80) { ys = 0; rows = 28; b0 = 0; cols = 32; }        /* vertical scroll lock (the game never uses it): the right columns ignore the scroll, so take every cell */
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            unsigned ea = nt + (unsigned)(((ys >> 3) + r) % 28 * 32 + (((b0 >> 3) + c) & 31)) * 2;
            int last = 24 + r * 8 - (ys & 7) + 7;                       /* the last line this cell row covers */
            if (last > 167 || (hw->reg[0] & 0x80)) last = 167;                  /* with the scroll lock every cell may be on any line */
            SC.last_line[(hw->vram[ea & 0x3FFF] | hw->vram[(ea + 1) & 0x3FFF] << 8) & 0x1FF] = (uint8_t)last;   /* rows grow downward: the later row has the later last line */
        }
    int tall = (hw->reg[1] >> 1) & 1, zoom = hw->reg[1] & 1, h = (tall ? 16 : 8) * (zoom ? 2 : 1), w = zoom ? 16 : 8, xshift = (hw->reg[0] & 0x08) ? 8 : 0;
    for (unsigned n = 0; n < 64; n++) {
        int sy = hw->vram[(sat + n) & 0x3FFF];
        if (sy == 0xD0) break;
        int sx = hw->vram[(sat + 0x80 + 2 * n) & 0x3FFF] - xshift;
        int last = 0;
        for (int k = 0; k < h; k++) { int ly = (sy + 1 + k) & 255; if (ly >= 24 && ly < 168) last = ly; }
        if (!last || sx + w <= 48 || sx >= 208) continue;
        unsigned t = hw->vram[(sat + 0x81 + 2 * n) & 0x3FFF];
        unsigned t0 = tall ? (t & 0xFE) : t;
        for (unsigned q = 0; q < (tall ? 2u : 1u); q++) { uint8_t *u = &SC.last_line[(spg + t0 + q) & 0x1FF]; if (*u < last) *u = (uint8_t)last; }
    }
    SC.refs_valid = 1;
}

static void sc_start(const HW *hw)       /* line 24 begins */
{
    SC.active = hw_scene_on;
    if (!SC.active) return;
    SC.nev = 0;
    SC.nbands = 1;
    SC.band[0].start = 24;
    SC.band[0].ev_end = 0;
    sc_recount(hw);
}

/* a change that matters: a band starts at the first line that can see it */
static void sc_cut(const HW *hw, int kind)
{
    int cl = hw->line + (hw->line_drawn ? 1 : 0);
    if (cl >= 168) return;
    if (cl < 24) cl = 24;
    ScBand *b = &SC.band[SC.nbands - 1];
    if (cl == b->start) b->ev_end = (uint16_t)SC.nev;
    else if (SC.nbands < SC_MAX_BANDS) { SC.band[SC.nbands].start = (uint8_t)cl; SC.band[SC.nbands].ev_end = (uint16_t)SC.nev; SC.nbands++; }
    else SC.overflow = 1;
    SC.cuts[kind]++;
}

static inline int sc_log(const HW *hw, int kind, unsigned a, unsigned old, unsigned nw)
{
    if (SC.nev >= SC_MAX_EV) { SC.overflow = 1; return 0; }
    ScEv *e = &SC.ev[SC.nev++];
    e->kind = (uint8_t)kind; e->line = (uint8_t)(hw->line > 255 ? 255 : hw->line); e->a = (uint16_t)a; e->old = (uint16_t)old; e->nw = (uint16_t)nw;
    return 1;
}

/* a VRAM byte changes value (called before the write, only when the value differs) */
static void sc_vram_write(const HW *hw, unsigned a, uint8_t old, uint8_t nw)
{
    if (!hw_scene_on) return;
    SC.tile_seq[(a >> 5) & 0x1FF] = ++SC.seq;
    if (!SC.active) return;
    unsigned nt = sc_nt_base(hw), sat = sc_sat_base(hw);
    int kind = ((a - sat) & 0x3FFF) < 0x100 ? SC_K_SAT : ((a - nt) & 0x3FFF) < 0x700 ? SC_K_NT : SC_K_PAT;
    if (!sc_log(hw, SC_VRAM, a, old, nw)) return;
    if (kind != SC_K_PAT) SC.refs_valid = 0;
    /* SC_BREAK (negative controls of the check): 1 = patterns never cut, 2 = name-table writes never cut, 3 = pattern writes cut for every tile, 4 = sprite-table writes never cut, 5 = colour changes never cut, 6 = register changes never cut */
#ifndef SC_BREAK
#define SC_BREAK 0
#endif
    if (!((hw->reg[1] & 0x40) && (hw->reg[0] & 0x04))) return;      /* display off: the line renderer draws only the backdrop colour, no table or tile is read (turning the display on is a register change, which cuts) */
    if (kind == SC_K_PAT) {
        if (SC_BREAK == 1) return;
        if (!SC.refs_valid) sc_recount(hw);
        int cl = hw->line + (hw->line_drawn ? 1 : 0);
        if (SC_BREAK == 3 || SC.last_line[(a >> 5) & 0x1FF] >= (cl < 24 ? 24 : cl)) sc_cut(hw, SC_K_PAT);
    } else if (!(SC_BREAK == 2 && kind == SC_K_NT) && !(SC_BREAK == 4 && kind == SC_K_SAT)) {
        sc_cut(hw, kind);
    }
}

static void sc_cram_write(const HW *hw, unsigned idx, unsigned old, unsigned nw)
{
    if (!SC.active) return;
    if (sc_log(hw, SC_CRAM, idx, old, nw)) { if (SC_BREAK != 5) sc_cut(hw, SC_K_CRAM); }
}

/* a register write: only a change of a bit the picture depends on is a change that matters (the value is stored before this is called) */
static void sc_reg_write(const HW *hw, int r, uint8_t old, uint8_t nw)
{
    if (!SC.active) return;
    unsigned m = sc_reg_mask(r);
    if (!m || ((old ^ nw) & m) == 0) return;
    if (sc_log(hw, SC_REG, (unsigned)r, old, nw)) {
        if (SC_BREAK != 6) sc_cut(hw, SC_K_REG);
        SC.refs_valid = 0;
    }
}

static void sc_undo(HW *t, const ScEv *e)
{
    if (e->kind == SC_VRAM) t->vram[e->a & 0x3FFF] = (uint8_t)e->old;
    else if (e->kind == SC_CRAM) {
        t->cram[(e->a & 31) * 2] = (uint8_t)(e->old & 0xFF);
        t->cram[(e->a & 31) * 2 + 1] = (uint8_t)(e->old >> 8);
        t->pal_rgb[e->a & 31] = cram_to_rgb(e->old);
    } else t->reg[e->a & 15] = (uint8_t)e->old;
}

#ifdef HW_SCENE_CHECK
static void sc_check(const HW *hw)
{
    static HW *tmp;
    if (!tmp) tmp = malloc(sizeof *tmp);
    memcpy(tmp, hw, sizeof *tmp);
    for (int i = 0; i < GG_W * GG_H; i++) tmp->fb[i] = 0x00FF00FFu;           /* a line the replay does not draw stays visibly wrong */
    int pos = SC.nev;
    while (pos > 0 && SC.ev[pos - 1].line >= 168) sc_undo(tmp, &SC.ev[--pos]);      /* changes after the last shown line */
    for (int b = SC.nbands - 1; b >= 0; b--) {
        int y0 = SC.band[b].start, y1 = b + 1 < SC.nbands ? SC.band[b + 1].start : 168;
        while (pos > SC.band[b].ev_end) sc_undo(tmp, &SC.ev[--pos]);
        tmp->spr_dirty = 1;
        for (int y = y0; y < y1; y++) render_line_impl(tmp, y);
    }
    for (int y = 0; y < GG_H; y++)
        if (memcmp(&tmp->fb[y * GG_W], &hw->fb[y * GG_W], GG_W * sizeof(uint32_t)) != 0) {
            fprintf(stderr, "scene check: frame %llu, shown line %d differs; %d bands:", (unsigned long long)hw->frame, y + 24, SC.nbands);
            for (int b = 0; b < SC.nbands; b++) fprintf(stderr, " [%d..%d ev%d]", SC.band[b].start, b + 1 < SC.nbands ? SC.band[b + 1].start - 1 : 167, SC.band[b].ev_end);
            fprintf(stderr, "; %d logged changes:", SC.nev);
            for (int i = 0; i < SC.nev && i < 8; i++) fprintf(stderr, " [%s a=%04x %x->%x line %d]", SC.ev[i].kind == SC_VRAM ? "vram" : SC.ev[i].kind == SC_CRAM ? "cram" : "reg", SC.ev[i].a, SC.ev[i].old, SC.ev[i].nw, SC.ev[i].line);
            {   /* what draws on the failing line: tiles of the background cells and of the sprites, to compare with the tiles of the logged pattern writes */
                int ly = y + 24, vs = hw->vscroll, ys = (ly + vs) % 224;
                unsigned nt = sc_nt_base(hw), sat = sc_sat_base(hw), spg = (hw->reg[6] & 4) ? 256u : 0;
                fprintf(stderr, " | line %d draws bg tiles:", ly);
                for (int c = 0; c < 32; c++) { unsigned ea = nt + (unsigned)((ys >> 3) * 32 + c) * 2; fprintf(stderr, " %x", (hw->vram[ea & 0x3FFF] | hw->vram[(ea + 1) & 0x3FFF] << 8) & 0x1FF); }
                fprintf(stderr, "; sprites:");
                for (unsigned n = 0; n < 64; n++) {
                    int sy = hw->vram[(sat + n) & 0x3FFF];
                    if (sy == 0xD0) break;
                    int h = (((hw->reg[1] >> 1) & 1) ? 16 : 8) * ((hw->reg[1] & 1) ? 2 : 1);
                    if (((ly - (sy + 1)) & 255) < h) fprintf(stderr, " #%u(tile %x x %d)", n, spg + hw->vram[(sat + 0x81 + 2 * n) & 0x3FFF], hw->vram[(sat + 0x80 + 2 * n) & 0x3FFF]);
                }
                fprintf(stderr, " |");
            }
            fprintf(stderr, " regs %02x %02x %02x %02x %02x %02x %02x %02x %02x vscroll %d\n", hw->reg[0], hw->reg[1], hw->reg[2], hw->reg[5], hw->reg[6], hw->reg[7], hw->reg[8], hw->reg[9], hw->reg[10], hw->vscroll);
            abort();
        }
    SC.checked++;
}
#endif

static void sc_end(HW *hw)               /* line 192 begins: the shown picture is complete */
{
    if (!SC.active) return;
    SC.active = 0;
    SC.frames++;
    int b = SC.nbands;
    SC.bands_hist[b > 11 ? 11 : b]++;
#ifdef SC_TRACE
    if (SC.nbands >= SC_TRACE) {
        fprintf(stderr, "scene: frame %llu, %d bands, %d changes, display %s, bands start at:", (unsigned long long)hw->frame, SC.nbands, SC.nev, ((hw->reg[1] & 0x40) && (hw->reg[0] & 0x04)) ? "on" : "off");
        for (int i = 0; i < SC.nbands && i < 24; i++) fprintf(stderr, " %d", SC.band[i].start);
        fprintf(stderr, "\n");
    }
#endif
    if ((uint64_t)SC.nev > SC.max_ev) SC.max_ev = (uint64_t)SC.nev;
    if ((uint64_t)SC.nbands > SC.max_bands) SC.max_bands = (uint64_t)SC.nbands;
    if (SC.overflow) {
        SC.overflows++;
        fprintf(stderr, "scene recorder overflow in frame %llu (%d changes, %d bands)\n", (unsigned long long)hw->frame, SC.nev, SC.nbands);
        abort();
    }
#ifdef HW_SCENE_CHECK
    sc_check(hw);
#endif
#ifdef HW_SCENE_DRAWCHECK
    sd_build(hw);
#else
    if (hw_scene_frame_cb) sd_build(hw);
#endif
}

void hw_scene_report(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "frames %llu (checked against the line renderer: %llu), overflows %llu, most logged changes in one frame %llu\n", (unsigned long long)SC.frames, (unsigned long long)SC.checked,
            (unsigned long long)SC.overflows, (unsigned long long)SC.max_ev);
    fprintf(f, "bands per frame:");
    for (int i = 1; i < 12; i++) fprintf(f, " %d:%llu", i, (unsigned long long)SC.bands_hist[i]);
    fprintf(f, " (11 = 11 or more)\ncuts by cause:");
    for (int k = 0; k < SC_NK; k++) fprintf(f, " [%s %llu]", sc_kind_name[k], (unsigned long long)SC.cuts[k]);
    fprintf(f, "\n");
    for (int i = 1; i < 12; i++) fprintf(f, "K sb%d %llu\n", i, (unsigned long long)SC.bands_hist[i]);
    fprintf(f, "K max_bands %llu\nK draw_checked %llu\n", (unsigned long long)SC.max_bands, (unsigned long long)SC.draw_checked);
    fprintf(f, "K frames %llu\nK checked %llu\nK max_ev %llu\n", (unsigned long long)SC.frames, (unsigned long long)SC.checked, (unsigned long long)SC.max_ev);
    fclose(f);
}
