/* Game Gear hardware model, see hw.h. */
#include "hw.h"
#include <string.h>
#if defined(HW_SPRITE_CHECK) || defined(HW_BG_CHECK) || defined(HW_FLAGS_CHECK)
#include <stdio.h>
#include <stdlib.h>
#endif
/* the sprite flags come from sprite_flags(); the reference renderer's own flags are used only in HW_REF_FLAGS builds (the oracle) and compared in HW_FLAGS_CHECK builds */
#if (defined(HW_REF_FLAGS) || defined(HW_FLAGS_CHECK)) && !defined(HW_NO_SPRITE_FLAGS)
#define HW_RENDERER_FLAGS
#endif
#if defined(HW_RENDERER_FLAGS) && defined(HW_NO_PIXELS)
#error "the reference flags need the reference renderer"
#endif

/* The vertical scroll register is latched once per frame (assumed from the VDP documentation, to be checked against a reference emulator, NOTES.md). */
#ifndef HW_LATCH_VSCROLL
#define HW_LATCH_VSCROLL 1
#endif
/* T-state within the line at which the line is drawn with the registers as they are then (228 = end of the line). */
#ifndef HW_RENDER_CYCLE
#define HW_RENDER_CYCLE 228
#endif

int hw_skip_render;
#ifdef HW_RENDER_STATS
uint64_t hw_stat[8];                    /* debug: [0] lines rendered, [1] background runs, [2] sprite entries drawn, [3] sprite pixels, [4] lines with sprites, [5] lines changed */
#define STAT(i, n) (hw_stat[i] += (n))
#else
#define STAT(i, n) ((void)0)
#endif
uint64_t (*hw_prof_clock)(void);
uint64_t hw_prof_render_us, hw_prof_audio_us;
#ifdef HW_FLAGS_CHECK
uint64_t hw_flags_checked, hw_flags_set[4];     /* lines compared; lines where neither / overflow / collision / both flags were raised (index = bits 6, 5 of the status) */
#endif
#ifndef HW_NO_PIXELS
static uint32_t px_lut[2][256][2];      /* see the line renderer */
#endif
static int px_lut_ready;
static void px_lut_init(void);
#ifdef HW_V1_STATS
#include "v1stats.inc"
#define V1(x) x
#else
#define V1(x) ((void)0)
#endif

void hw_reset(HW *hw)
{
    memset(hw->ram, 0, sizeof hw->ram);
    memset(hw->vram, 0, sizeof hw->vram);
    memset(hw->cram, 0, sizeof hw->cram);
    for (int i = 0; i < 32; i++) hw->pal_rgb[i] = 0xFF000000u;
    memset(hw->reg, 0, sizeof hw->reg);
    hw->map[0] = 0; hw->map[1] = 0; hw->map[2] = 1; hw->map[3] = 2;   /* power-on state of $FFFC-$FFFF */
    hw->bank[0] = 0; hw->bank[1] = 1; hw->bank[2] = 2;
    for (int i = 0; i < 3; i++) hw->page[i] = hw->rom + (size_t)(hw->bank[i] % hw->nbanks) * 0x4000;
    hw->vaddr = 0; hw->vcode = 0; hw->vlatch = 0; hw->vlatch_set = 0; hw->vbuf = 0; hw->cram_latch = 0;
    hw->status = 0; hw->line_pending = 0; hw->line_counter = 0; hw->vscroll = 0;
    hw->line = 0; hw->line_cycle = 0; hw->line_drawn = 0; hw->cycles = 0; hw->frame = 0; hw->frame_done = 0;
#ifndef HW_NO_PIXELS
    memset(hw->fb, 0, sizeof hw->fb);
    memset(hw->line_ver, 0, sizeof hw->line_ver);
#endif
    memset(hw->psg_tone, 0, sizeof hw->psg_tone);
    memset(hw->psg_vol, 0x0F, sizeof hw->psg_vol);
    hw->psg_noise = 0; hw->psg_latch = 0; hw->gg_stereo = 0xFF;
    memset(&hw->psg, 0, sizeof hw->psg);
    hw->psg.lfsr = 0x8000;
    hw->spr_dirty = 1;
    hw->io_ctrl = 0xFF; hw->mem_ctrl = 0;
    hw->warn_flags = 0;
}

void hw_init(HW *hw, const uint8_t *rom, uint32_t size)
{
    memset(hw, 0, sizeof *hw);
    hw->rom = rom;
    hw->rom_size = size;
    hw->nbanks = size / 0x4000;
    if (!px_lut_ready) px_lut_init();
    if (hw->nbanks == 0) hw->nbanks = 1;
    hw_reset(hw);
}

/* ---- memory ------------------------------------------------------------------------------------------------------------------------------------- */
static inline const uint8_t *rom_page(const HW *hw, int slot)
{
    return hw->page[slot];
}

uint8_t hw_rd(HW *hw, uint16_t a)
{
    if (a < 0x400) return hw->rom[a];                               /* the first 1 KB is always bank 0 */
    if (a < 0x4000) return rom_page(hw, 0)[a];
    if (a < 0x8000) return rom_page(hw, 1)[a - 0x4000];
    if (a < 0xC000) {
        if (hw->map[0] & 0x08) { hw->warn_flags |= HW_WARN_CARTRAM; return 0xFF; }
        return rom_page(hw, 2)[a - 0x8000];
    }
    return hw->ram[a & 0x1FFF];
}

void hw_wr(HW *hw, uint16_t a, uint8_t v)
{
    if (a < 0xC000) return;                                          /* ROM: ignored (no cart RAM in this game) */
    hw->ram[a & 0x1FFF] = v;
    if (a >= 0xFFFC) {
        int r = a - 0xFFFC;
        hw->map[r] = v;
        if (r == 0) { if (v & 0x1C) hw->warn_flags |= HW_WARN_CARTRAM; }
        else { hw->bank[r - 1] = v; hw->page[r - 1] = hw->rom + (size_t)(v % hw->nbanks) * 0x4000; }
        if (hw->on_mapper) hw->on_mapper(hw, r, v, hw->user);
    }
}

int32_t hw_phys(const HW *hw, uint16_t a)
{
    if (a < 0x400) return a;
    if (a < 0x4000) return (int32_t)((size_t)(hw->bank[0] % hw->nbanks) * 0x4000 + a);
    if (a < 0x8000) return (int32_t)((size_t)(hw->bank[1] % hw->nbanks) * 0x4000 + (a - 0x4000));
    if (a < 0xC000) return (int32_t)((size_t)(hw->bank[2] % hw->nbanks) * 0x4000 + (a - 0x8000));
    return -1;
}

static inline uint32_t cram_to_rgb(unsigned w);

#ifdef HW_SCENE
#include "scene.inc"
#include "scenedraw.inc"
#define SC_HOOK(x) x
#else
#define SC_HOOK(x) ((void)0)
#endif

void hw_state_restored(HW *hw)
{
    for (int i = 0; i < 32; i++) hw->pal_rgb[i] = cram_to_rgb(hw->cram[2 * i] | hw->cram[2 * i + 1] << 8);
    for (int i = 0; i < 3; i++) hw->page[i] = hw->rom + (size_t)(hw->bank[i] % hw->nbanks) * 0x4000;
    hw->spr_dirty = 1;
    hw->frame_done = 0;
    SC_HOOK(sc_state_restored());
}

/* ---- VDP ---------------------------------------------------------------------------------------------------------------------------------------- */
int hw_irq(const HW *hw)
{
    return ((hw->status & 0x80) && (hw->reg[1] & 0x20)) || (hw->line_pending && (hw->reg[0] & 0x10));
}

static uint8_t vdp_status_read(HW *hw)
{
    uint8_t s = hw->status;
    V1(v1_status_read(hw, s));
    hw->status &= 0x1F;
    hw->line_pending = 0;
    hw->vlatch_set = 0;
    return s;
}

static void vdp_control_write(HW *hw, uint8_t v)
{
    if (!hw->vlatch_set) {
        hw->vlatch = v;
        hw->vlatch_set = 1;
        return;
    }
    hw->vlatch_set = 0;
    hw->vcode = v >> 6;
    hw->vaddr = (uint16_t)((hw->vlatch | (v & 0x3F) << 8) & 0x3FFF);
    if (hw->vcode == 0) {
        hw->vbuf = hw->vram[hw->vaddr];
        hw->vaddr = (hw->vaddr + 1) & 0x3FFF;
    } else if (hw->vcode == 2) {
        V1(v1_reg_write(hw, v & 0x0F, hw->reg[v & 0x0F], hw->vlatch));
        SC_HOOK(uint8_t sc_old = hw->reg[v & 0x0F]);
        hw->reg[v & 0x0F] = hw->vlatch;
        SC_HOOK(sc_reg_write(hw, v & 0x0F, sc_old, hw->vlatch));
        if ((v & 0x0F) == 1 || (v & 0x0F) == 5) hw->spr_dirty = 1;
    }
}

static void vdp_data_write(HW *hw, uint8_t v)
{
    hw->vlatch_set = 0;
    if (hw->vcode == 3) {
        /* Game Gear CRAM: 64 bytes; an even-address write is latched, the odd-address write commits the 12-bit word */
        if ((hw->vaddr & 1) == 0) hw->cram_latch = v;
        else {
            V1(v1_cram_write(hw, hw->cram[hw->vaddr & 0x3E] | hw->cram[(hw->vaddr & 0x3E) + 1] << 8, hw->cram_latch | (v & 0x0F) << 8));
#ifdef HW_SCENE
            {
                unsigned o16 = hw->cram[hw->vaddr & 0x3E] | hw->cram[(hw->vaddr & 0x3E) + 1] << 8, n16 = hw->cram_latch | (v & 0x0F) << 8;
                if (o16 != n16) sc_cram_write(hw, (hw->vaddr & 0x3E) >> 1, o16, n16);
            }
#endif
            hw->cram[(hw->vaddr & 0x3E)] = hw->cram_latch;
            hw->cram[(hw->vaddr & 0x3E) + 1] = v & 0x0F;
            hw->pal_rgb[(hw->vaddr & 0x3E) >> 1] = cram_to_rgb(hw->cram[hw->vaddr & 0x3E] | hw->cram[(hw->vaddr & 0x3E) + 1] << 8);
        }
    } else {
        V1(v1_vram_write(hw, hw->vaddr, hw->vram[hw->vaddr], v));
#ifdef HW_SCENE
        if (hw->vram[hw->vaddr] != v) sc_vram_write(hw, hw->vaddr, hw->vram[hw->vaddr], v);
#endif
        hw->vram[hw->vaddr] = v;
        if (((hw->vaddr - ((unsigned)(hw->reg[5] & 0x7E) << 7)) & 0x3FFF) < 0x100) hw->spr_dirty = 1;
    }
    hw->vbuf = v;
    hw->vaddr = (hw->vaddr + 1) & 0x3FFF;
}

static uint8_t vdp_data_read(HW *hw)
{
    hw->vlatch_set = 0;
    uint8_t v = hw->vbuf;
    hw->vbuf = hw->vram[hw->vaddr];
    hw->vaddr = (hw->vaddr + 1) & 0x3FFF;
    return v;
}

/* ---- sprite flags ------------------------------------------------------------------------------------------------------------------------------- */
/* The VDP's sprite overflow (status bit 6) and collision (bit 5) flags are the only part of the picture logic the game itself reads (5.6 million status reads in the scenarios),
 * so they are produced here, from the sprite table and the pattern rows alone, at the point of the line where the picture used to be drawn (HW_RENDER_CYCLE). The pictures
 * come from the GE (scenedraw); the CPU line renderer below exists only in the test builds (it is compiled out with HW_NO_PIXELS, which the PSP build sets) and is
 * the reference these flags are checked against (HW_FLAGS_CHECK: every line, both ways; HW_REF_FLAGS: the renderer's own flags, for the oracle). */
static uint8_t rev8[256];                       /* bit-reversed byte: a pattern row's leftmost pixel is bit 7, the mask below has it as bit 0 */

static void px_lut_init(void)
{
    for (int v = 0; v < 256; v++) {
        int r = 0;
        for (int b = 0; b < 8; b++) r |= (v >> b & 1) << (7 - b);
        rev8[v] = (uint8_t)r;
    }
#ifndef HW_NO_PIXELS
    /* Pattern bytes expanded to eight pixels (one byte each, 0 or 1), indexed [hflip][plane byte]; a tile row is the four plane bytes combined with shifts. */
    for (int f = 0; f < 2; f++)
        for (int v = 0; v < 256; v++) {
            uint8_t b[8];
            for (int x = 0; x < 8; x++) b[x] = (uint8_t)(v >> (f ? x : 7 - x) & 1);
            memcpy(px_lut[f][v], b, 8);
        }
#endif
    px_lut_ready = 1;
}

static inline uint32_t cram_to_rgb(unsigned w)
{
    unsigned r = (w & 0xF) * 17, g = ((w >> 4) & 0xF) * 17, b = ((w >> 8) & 0xF) * 17;
    return 0xFF000000u | b << 16 | g << 8 | r;
}

static void sprites_build(HW *hw);

/* The flags the sprites of line y (0-191) raise: bit 6 when more than 8 sprites cover the line, bit 5 when two of the first 8 have an opaque pixel in the same column
 * (all 256 columns of the picture count, not only the 160 shown; sprites are 8 or 16 pixels wide, so a row is one or two small masks put into a 256-bit line). */
static unsigned sprite_flags(HW *hw, int y)
{
    int cover = hw->spr_cnt[y];
#if defined(FLAGS_BREAK) && FLAGS_BREAK == 4
    unsigned fl = cover >= 8 ? 0x40u : 0;
#else
    unsigned fl = cover > 8 ? 0x40u : 0;
#endif
    if (cover < 2) return fl;
    int tall = (hw->reg[1] >> 1) & 1, zoom = hw->reg[1] & 1;
    unsigned sat = (unsigned)(hw->reg[5] & 0x7E) << 7;
    unsigned spg = (hw->reg[6] & 0x04) ? 0x2000u : 0;
    int xshift = (hw->reg[0] & 0x08) ? 8 : 0;
    uint32_t occ[9] = {0};
    for (int i = 0; i < cover && i < 8; i++) {
        int n = hw->spr_list[y][i];
        int dy = (y - (hw->vram[(sat + (unsigned)n) & 0x3FFF] + 1)) & 255;
        int sx = hw->vram[(sat + 0x80 + 2 * (unsigned)n) & 0x3FFF] - xshift;
        unsigned tile = hw->vram[(sat + 0x81 + 2 * (unsigned)n) & 0x3FFF];
        int srow = zoom ? dy >> 1 : dy;
        if (tall) { tile &= 0xFE; if (srow >= 8) { tile |= 1; srow -= 8; } }
        const uint8_t *p = hw->vram + ((spg + tile * 32 + (unsigned)srow * 4) & 0x3FFF);
        uint32_t m = rev8[p[0] | p[1] | p[2] | p[3]];
        if (!m) continue;
#if defined(FLAGS_BREAK) && FLAGS_BREAK == 3           /* deliberate faults for tests/flags_test.sh: each must be caught */
        if (0)
#endif
        if (zoom) { uint32_t z = 0; for (int k = 0; k < 8; k++) if (m >> k & 1) z |= 3u << (2 * k); m = z; }
#if defined(FLAGS_BREAK) && FLAGS_BREAK == 1
        if (sx < 0) { m = -sx >= 16 ? 0 : m; sx = 0; }
#else
        if (sx < 0) { m = -sx >= 16 ? 0 : m >> -sx; sx = 0; }          /* pixels left of column 0 are not drawn */
#endif
        if (sx > 255) continue;
#if !(defined(FLAGS_BREAK) && FLAGS_BREAK == 2)
        if (sx + 16 > 256) m &= (1u << (256 - sx)) - 1;                /* nor are those right of column 255 */
#endif
        if (!m) continue;
        int wd = sx >> 5;
        uint64_t mm = (uint64_t)m << (sx & 31);
        uint32_t lo = (uint32_t)mm, hi = (uint32_t)(mm >> 32);
        if ((occ[wd] & lo) || (occ[wd + 1] & hi)) { fl |= 0x20; break; }
        occ[wd] |= lo; occ[wd + 1] |= hi;
    }
    return fl;
}

/* Everything the sprite logic does for line y at the render point: the table cache, the warnings, and the flags (returned; the renderer does its own in the reference builds). */
static unsigned sprite_line(HW *hw, int y)
{
    if (!(hw->reg[1] & 0x40) || !(hw->reg[0] & 0x04)) {
        if ((hw->reg[1] & 0x40) && !(hw->reg[0] & 0x04)) hw->warn_flags |= HW_WARN_MODE;   /* display on but not Mode 4 */
        return 0;
    }
    if (hw->spr_dirty) sprites_build(hw);
    if (hw->reg[1] & 1) hw->warn_flags |= HW_WARN_ZOOM;
#ifdef HW_NO_SPRITE_FLAGS                              /* debug build for V1: never report overflow / collision, to see whether the game notices */
    (void)y; (void)sprite_flags;
    return 0;
#else
    return sprite_flags(hw, y);
#endif
}

#ifndef HW_NO_PIXELS
/* ---- line renderer (reference, test builds only) ------------------------------------------------------------------------------------------------ */

/* one 8-pixel row of a 4-bitplane tile: colour index 0-15 per pixel, left to right */
static inline void tile_row32(const HW *hw, unsigned tile_addr, int row, int hflip, uint32_t o[2])
{
    const uint8_t *p = hw->vram + ((tile_addr + (unsigned)row * 4) & 0x3FFF);
    const uint32_t *l0 = px_lut[hflip][p[0]], *l1 = px_lut[hflip][p[1]], *l2 = px_lut[hflip][p[2]], *l3 = px_lut[hflip][p[3]];
    o[0] = l0[0] | l1[0] << 1 | l2[0] << 2 | l3[0] << 3;
    o[1] = l0[1] | l1[1] << 1 | l2[1] << 2 | l3[1] << 3;
}

static inline void tile_row(const HW *hw, unsigned tile_addr, int row, int hflip, uint8_t out[8])
{
    uint32_t o[2];
    tile_row32(hw, tile_addr, row, hflip, o);
    memcpy(out, o, 8);
}

/* The visible 160 background pixels of a line when the vertical scroll lock is off: whole background tiles (21 of them cover 160 pixels at any fine scroll), written
 * eight pixels at a time into an aligned strip and copied out at the fine-scroll offset. idx: CRAM index; bgc / bgp (colour 0-15, priority flag) only when `pri`. */
static void bg_strip(const HW *hw, int y, int scx, int vs, unsigned nt, uint8_t *idx, uint8_t *bgc, uint8_t *bgp, int pri)
{
    uint8_t ti[176], tc[176], tp[176];
    int ys = (y + vs) % 224, row = ys & 7;
    unsigned row_base = nt + (unsigned)(ys >> 3) * 64;
    int b0 = (48 - scx) & 255, col = b0 >> 3, fx = b0 & 7;
    for (int k = 0; k < 21; k++, col = (col + 1) & 31) {
        unsigned ea = row_base + (unsigned)col * 2;
        unsigned ent = hw->vram[ea & 0x3FFF] | hw->vram[(ea + 1) & 0x3FFF] << 8;
        uint32_t o[2];
        tile_row32(hw, (ent & 0x1FF) * 32, (ent >> 10) & 1 ? 7 - row : row, (ent >> 9) & 1, o);
        uint32_t add = (ent >> 11) & 1 ? 0x10101010u : 0;
        uint32_t c[2] = {o[0] + add, o[1] + add};
        memcpy(tc + k * 8, o, 8);
        memcpy(ti + k * 8, c, 8);
        if (pri) {
            uint32_t pv[2] = {(ent >> 12) & 1 ? 0x01010101u : 0, (ent >> 12) & 1 ? 0x01010101u : 0};
            memcpy(tp + k * 8, pv, 8);
        }
    }
    memcpy(idx + 48, ti + fx, 160);
    if (pri) { memcpy(bgc + 48, tc + fx, 160); memcpy(bgp + 48, tp + fx, 160); }
}

#endif /* HW_NO_PIXELS */

/* the sprites on each line, in table order: a sprite covers the lines (y + 1) .. (y + height) of its table entry; the table ends at the first entry with y = $D0 */
static void sprites_build(HW *hw)
{
    int tall = (hw->reg[1] >> 1) & 1, zoom = hw->reg[1] & 1;
    int h = (tall ? 16 : 8) * (zoom ? 2 : 1);
    unsigned sat = (unsigned)(hw->reg[5] & 0x7E) << 7;
    memset(hw->spr_cnt, 0, sizeof hw->spr_cnt);
    for (int n = 0; n < 64; n++) {
        int sy = hw->vram[(sat + (unsigned)n) & 0x3FFF];
        if (sy == 0xD0) break;
        for (int k = 0; k < h; k++) {
            int ly = (sy + 1 + k) & 255;
            if (ly >= 192) continue;
            int c = hw->spr_cnt[ly];
            if (c < 9) {
                if (c < 8) hw->spr_list[ly][c] = (uint8_t)n;
                hw->spr_cnt[ly] = (uint8_t)(c + 1);
            }
        }
    }
    hw->spr_dirty = 0;
}

#ifndef HW_NO_PIXELS
/* The picture is 256 wide, the Game Gear shows x 48..207 of lines 24..167. Lines outside the window are still run through the sprite logic, because the
 * sprite overflow / collision flags depend on them; the background is only drawn where it can be seen. */
static void render_line_impl(HW *hw, int y)
{
    uint8_t idx[256];                  /* CRAM index of each pixel */
    uint8_t bgc[256], bgp[256];          /* background colour 0-15, background priority flag */
    int backdrop = 16 + (hw->reg[7] & 0x0F);
    int vis = (y >= 24 && y < 24 + GG_H);
    uint32_t *d = vis ? &hw->fb[(y - 24) * GG_W] : NULL;
    int changed = 0;

    if (!(hw->reg[1] & 0x40) || !(hw->reg[0] & 0x04)) {
        if (vis) {
            uint32_t c = hw->pal_rgb[backdrop];
            for (int x = 0; x < GG_W; x++)
                if (d[x] != c) { d[x] = c; changed = 1; }
        }
    } else {
        if (hw->spr_dirty) sprites_build(hw);
        int cover = y < 192 ? hw->spr_cnt[y] : 0;
        /* ---- background */
        if (vis) {
            unsigned nt = (unsigned)(hw->reg[2] & 0x0E) << 10;
            int scx = hw->reg[8];
            if (y < 16 && (hw->reg[0] & 0x40)) scx = 0;              /* horizontal scroll lock for the top two tile rows */
            int vs = HW_LATCH_VSCROLL ? hw->vscroll : hw->reg[9];
            int lock = hw->reg[0] & 0x80;
            int sx = 48;
            if (!lock) { bg_strip(hw, y, scx, vs, nt, idx, bgc, bgp, cover != 0); sx = 208; }
            while (sx < 208) {
                int tx = sx >> 3;
                int vscroll_here = (tx >= 24 && lock) ? 0 : vs;      /* vertical scroll lock for the right 8 columns */
                int ys = (y + vscroll_here) % 224;
                int bx = (sx - scx) & 255;
                int col = bx >> 3, fx = bx & 7;
                int n = 8 - fx;
                if (sx + n > (tx + 1) * 8) n = (tx + 1) * 8 - sx;    /* a run ends at a background tile or a screen tile boundary */
                unsigned ent_addr = nt + (unsigned)((ys >> 3) * 32 + col) * 2;
                unsigned ent = hw->vram[ent_addr & 0x3FFF] | hw->vram[(ent_addr + 1) & 0x3FFF] << 8;
                int row = ys & 7;
                if ((ent >> 10) & 1) row = 7 - row;
                uint8_t pix[8];
                tile_row(hw, (ent & 0x1FF) * 32, row, (ent >> 9) & 1, pix);
                STAT(1, 1);
                uint8_t palbase = (uint8_t)(((ent >> 11) & 1) * 16), pri = (uint8_t)((ent >> 12) & 1);
                for (int k = 0; k < n; k++) {
                    bgc[sx + k] = pix[fx + k];
                    bgp[sx + k] = pri;
                    idx[sx + k] = (uint8_t)(palbase + pix[fx + k]);
                }
                sx += n;
            }
#ifdef HW_BG_CHECK                                      /* debug build: the strip renderer must equal the run-by-run renderer it replaced */
            if (!lock) {
                uint8_t ri[256], rc[256], rp[256];
                for (int sx2 = 48; sx2 < 208;) {
                    int tx = sx2 >> 3, ys = (y + vs) % 224, bx = (sx2 - scx) & 255, col = bx >> 3, fx = bx & 7, n = 8 - fx;
                    if (sx2 + n > (tx + 1) * 8) n = (tx + 1) * 8 - sx2;
                    unsigned ent_addr = nt + (unsigned)((ys >> 3) * 32 + col) * 2;
                    unsigned ent = hw->vram[ent_addr & 0x3FFF] | hw->vram[(ent_addr + 1) & 0x3FFF] << 8;
                    int row = ys & 7;
                    if ((ent >> 10) & 1) row = 7 - row;
                    uint8_t pix[8];
                    tile_row(hw, (ent & 0x1FF) * 32, row, (ent >> 9) & 1, pix);
                    for (int k = 0; k < n; k++) { rc[sx2 + k] = pix[fx + k]; rp[sx2 + k] = (uint8_t)((ent >> 12) & 1); ri[sx2 + k] = (uint8_t)(((ent >> 11) & 1) * 16 + pix[fx + k]); }
                    sx2 += n;
                }
                for (int x = 48; x < 208; x++)
                    if (ri[x] != idx[x] || (cover && (rc[x] != bgc[x] || rp[x] != bgp[x]))) { fprintf(stderr, "background mismatch at line %d x %d\n", y, x); abort(); }
            }
#endif
        }
        /* ---- sprites */
        int tall = (hw->reg[1] >> 1) & 1, zoom = hw->reg[1] & 1;
        int w = zoom ? 16 : 8;
        unsigned sat = (unsigned)(hw->reg[5] & 0x7E) << 7;
        unsigned spg = (hw->reg[6] & 0x04) ? 0x2000u : 0;
        int xshift = (hw->reg[0] & 0x08) ? 8 : 0;
#ifdef HW_SPRITE_CHECK                                  /* debug build: the cache must equal a scan of all 64 table entries */
        {
            int tall_c = (hw->reg[1] >> 1) & 1, zoom_c = hw->reg[1] & 1, hh = (tall_c ? 16 : 8) * (zoom_c ? 2 : 1), count = 0;
            for (int n = 0; n < 64; n++) {
                int sy = hw->vram[(sat + (unsigned)n) & 0x3FFF];
                if (sy == 0xD0) break;
                if (((y - (sy + 1)) & 255) >= hh) continue;
                if (++count > 8) break;
                if (count > cover || hw->spr_list[y][count - 1] != n) { fprintf(stderr, "sprite cache mismatch at line %d\n", y); abort(); }
            }
            if (count != (cover > 8 ? 9 : cover)) { fprintf(stderr, "sprite cache count mismatch at line %d: %d vs %d\n", y, count, cover); abort(); }
        }
#endif
        uint8_t drawn[256];
        if (cover) memset(drawn, 0, sizeof drawn);
#ifdef HW_RENDERER_FLAGS                                /* the reference flags: more than 8 sprites on the line */
        if (cover > 8) hw->status |= 0x40;
#endif
        STAT(0, 1); STAT(4, cover != 0);
        for (int i = 0; i < cover && i < 8; i++) {
            int n = hw->spr_list[y][i];
            STAT(2, 1);
            int sy = hw->vram[(sat + n) & 0x3FFF];
            int dy = (y - (sy + 1)) & 255;
            int sx = hw->vram[(sat + 0x80 + 2 * n) & 0x3FFF] - xshift;
            unsigned tile = hw->vram[(sat + 0x81 + 2 * n) & 0x3FFF];
            int srow = zoom ? dy >> 1 : dy;
            if (tall) { tile &= 0xFE; if (srow >= 8) { tile |= 1; srow -= 8; } }
            uint8_t pix[8];
            tile_row(hw, spg + tile * 32, srow, 0, pix);
            for (int px = 0; px < w; px++) {
                int x = sx + px;
                if (x < 0 || x > 255) continue;
                int c = pix[zoom ? px >> 1 : px];
                if (!c) continue;
                if (drawn[x]) {
#ifdef HW_RENDERER_FLAGS
                    hw->status |= 0x20;
#endif
                    continue;
                }
                STAT(3, 1);
                drawn[x] = 1;
                if (!vis || x < 48 || x >= 208) continue;
                if (bgp[x] && bgc[x]) { V1(v1_prio_hide()); continue; }   /* background priority tile covers the sprite */
                idx[x] = (uint8_t)(16 + c);
            }
        }
    }

    /* ---- the 160x144 window: x 48..207, y 24..167 of the 256x192 picture */
    if (vis && (hw->reg[1] & 0x40) && (hw->reg[0] & 0x04)) {
        for (int x = 0; x < GG_W; x++) {
            uint32_t c = hw->pal_rgb[idx[48 + x]];
            if (d[x] != c) { d[x] = c; changed = 1; }
        }
    }
    if (changed) { hw->line_ver[y - 24]++; STAT(5, 1); }
}

#endif /* HW_NO_PIXELS */

/* What happens at a line's render point: the sprite logic (cache, warnings, flags), and in the test builds the reference picture. */
static void render_line_work(HW *hw, int y)
{
    unsigned fl = sprite_line(hw, y);
#if defined(HW_RENDERER_FLAGS) && !defined(HW_FLAGS_CHECK)
    (void)fl;
#endif
#ifdef HW_RENDERER_FLAGS
    /* reference flags: the renderer raises them itself; HW_FLAGS_CHECK also compares them, line by line, with the routine's */
    unsigned st = hw->status;
    hw->status = (uint8_t)(st & ~0x60u);
    render_line_impl(hw, y);
    unsigned ref = hw->status & 0x60u;
    hw->status = (uint8_t)(st | ref);
#ifdef HW_FLAGS_CHECK
    if (ref != fl) {
        fprintf(stderr, "sprite flags mismatch at frame %llu line %d: routine %02x, renderer %02x\n", (unsigned long long)hw->frame, y, fl, ref);
        abort();
    }
    hw_flags_checked++;
    hw_flags_set[ref >> 5 & 3]++;
#endif
#else
    hw->status |= (uint8_t)fl;
#ifndef HW_NO_PIXELS
    if (!hw_skip_render) render_line_impl(hw, y);
#endif
#endif
}

static void render_line(HW *hw, int y)
{
#ifdef HW_V1_STATS
    uint8_t v1_st = v1_line_pre(hw, y);
    render_line_work(hw, y);
    v1_line_post(hw, y, v1_st);
    return;
#endif
    if (hw_prof_clock) {
        uint64_t t = hw_prof_clock();
        render_line_work(hw, y);
        hw_prof_render_us += hw_prof_clock() - t;
    } else {
        render_line_work(hw, y);
    }
}

static void line_start(HW *hw)
{
    int l = hw->line;
    if (l == 192) { V1(v1_line192(hw)); SC_HOOK(sc_end(hw)); }
    if (l == 24) SC_HOOK(sc_start(hw));
    if (l == 0) { V1(v1_frame_end()); hw->vscroll = hw->reg[9]; }
    if (l <= 192) {
        if (hw->line_counter == 0) { hw->line_counter = hw->reg[10]; hw->line_pending = 1; }
        else hw->line_counter--;
    } else {
        hw->line_counter = hw->reg[10];
    }
    if (l == 193) hw->status |= 0x80;
}

void hw_advance(HW *hw, int t)
{
    hw->cycles += (uint64_t)t;
    hw->line_cycle += t;
#ifndef HW_NO_RENDER
    if (!hw->line_drawn && hw->line_cycle >= HW_RENDER_CYCLE && hw->line < 192) { render_line(hw, hw->line); hw->line_drawn = 1; }
#endif
    while (hw->line_cycle >= HW_CYC_LINE) {
        hw->line_cycle -= HW_CYC_LINE;
#ifndef HW_NO_RENDER
        if (!hw->line_drawn && hw->line < 192) render_line(hw, hw->line);
#endif
        hw->line_drawn = 0;
        if (++hw->line >= HW_LINES) {
            hw->line = 0;
            hw->frame++;
            hw->frame_done = 1;
        }
        line_start(hw);
#ifndef HW_NO_RENDER
        if (hw->line_cycle >= HW_RENDER_CYCLE && hw->line < 192) { render_line(hw, hw->line); hw->line_drawn = 1; }
#endif
    }
}

/* ---- PSG ---------------------------------------------------------------------------------------------------------------------------------------- */
static void psg_write(HW *hw, uint8_t v)
{
    if (v & 0x80) {
        hw->psg_latch = (v >> 4) & 7;
        int ch = hw->psg_latch >> 1;
        if (hw->psg_latch & 1) hw->psg_vol[ch] = v & 0x0F;
        else if (ch < 3) { hw->psg_tone_lo[ch] = v & 0x0F; hw->psg_tone[ch] = (uint16_t)((hw->psg_tone[ch] & 0x3F0) | (v & 0x0F)); }
        else { hw->psg_noise = v & 0x07; hw->psg.lfsr = 0x8000; }
    } else {
        int ch = hw->psg_latch >> 1;
        if (hw->psg_latch & 1) hw->psg_vol[ch] = v & 0x0F;
        else if (ch < 3) { hw->psg_tone_hi[ch] = v & 0x3F; hw->psg_tone[ch] = (uint16_t)((hw->psg_tone[ch] & 0x00F) | (v & 0x3F) << 4); }
        else { hw->psg_noise = v & 0x07; hw->psg.lfsr = 0x8000; }
    }
}

/* ---- ports -------------------------------------------------------------------------------------------------------------------------------------- */
static uint8_t vcounter(const HW *hw)
{
    int l = hw->line;
    return (uint8_t)(l <= 0xDA ? l : l - 6);          /* NTSC 192-line counter sequence */
}

uint8_t hw_in(HW *hw, uint16_t port)
{
    uint8_t p = (uint8_t)port, v;
    if (p <= 0x06) {                                   /* Game Gear specific ports */
        switch (p) {
        case 0: v = (uint8_t)(0x40 | ((hw->pad & PAD_START) ? 0x00 : 0x80)); break;   /* bit 7 Start (0 = pressed), bit 6 = export, bit 5 = NTSC */
        case 1: v = 0x7F; break;
        case 2: v = 0xFF; break;
        case 3: v = 0x00; break;
        case 4: v = 0xFF; break;
        case 5: v = 0x00; break;
        default: v = 0xFF; break;
        }
    } else if (p < 0x40) {
        v = 0xFF;                                      /* $3E/$3F and mirrors are write-only */
    } else if (p < 0x80) {
        v = (p & 1) ? (uint8_t)((hw->line_cycle * 342 / HW_CYC_LINE) >> 1) : vcounter(hw);
    } else if (p < 0xC0) {
        v = (p & 1) ? vdp_status_read(hw) : vdp_data_read(hw);
    } else if ((p & 1) == 0) {                         /* $DC (and mirrors, e.g. $F2): joypad port A + B up / down */
        uint8_t k = hw->pad;
        v = (uint8_t)(0xFF & ~((k & PAD_UP ? 0x01 : 0) | (k & PAD_DOWN ? 0x02 : 0) | (k & PAD_LEFT ? 0x04 : 0) | (k & PAD_RIGHT ? 0x08 : 0) |
                               (k & PAD_1 ? 0x10 : 0) | (k & PAD_2 ? 0x20 : 0)));
    } else {                                           /* $DD: port B, reset, and the TH lines (read back the output levels when configured as outputs) */
        v = 0x3F;
        if (hw->io_ctrl & 0x02) v |= 0x40; else v |= (uint8_t)((hw->io_ctrl & 0x20) << 1);
        if (hw->io_ctrl & 0x08) v |= 0x80; else v |= (uint8_t)(hw->io_ctrl & 0x80);
    }
    if (hw->on_port) hw->on_port(hw, 0, p, v, hw->user);
    return v;
}

void hw_out(HW *hw, uint16_t port, uint8_t v)
{
    uint8_t p = (uint8_t)port;
    if (hw->on_port) hw->on_port(hw, 1, p, v, hw->user);
    if (p <= 0x06) {
        if (p == 6) { hw_audio_run(hw, hw->cycles); hw->gg_stereo = v; }
        return;                                        /* $00-$05: serial / EXT ports, not modelled */
    }
    if (p < 0x40) {
        if (p & 1) hw->io_ctrl = v; else hw->mem_ctrl = v;
        return;
    }
    if (p < 0x80) { hw_audio_run(hw, hw->cycles); psg_write(hw, v); return; }
    if (p < 0xC0) {
        if (p & 1) vdp_control_write(hw, v); else vdp_data_write(hw, v);
        return;
    }
    /* $C0-$FF: joypad ports are read-only; $F0-$F2 would be the YM2413 on a Japanese Master System: ignored */
}
