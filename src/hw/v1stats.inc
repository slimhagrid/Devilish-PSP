/* Video feasibility statistics (M6c / V1 in PLAN.md): included into hw.c only in a build with -DHW_V1_STATS (tools/native_build.sh with V1=1). Nothing here changes what the
 * hardware model does; it counts what the game does to the picture so that the design of a GE renderer can be judged from facts:
 *   - writes to the VDP registers, VRAM (by region) and CRAM, by phase of the frame (lines 0-23 = active but not shown on the Game Gear, 24-167 = shown, 168-191 = active but
 *     not shown, 192-261 = vertical blank) and whether the value changed;
 *   - "bands": between two consecutive SHOWN lines of a frame, did anything the picture depends on change (registers that matter, CRAM, pattern / name table / sprite table
 *     contents)? A GE renderer would have to cut its drawing at every such boundary; the report counts them by kind, by line and per frame;
 *   - sprites: how many cover each line (the VDP draws 8), lines with more than 8, collisions, 8x16 / zoom use, tile-priority sprites hidden behind background;
 *   - reads of the status port and what the sprite flags (bits 5, 6) were when read.
 */
#include <stdio.h>
#include <stdlib.h>

static void render_line_impl(HW *hw, int y);
static int v1_quiet;                    /* set while a copy of the model is rendered for comparison: hooks stay silent */

enum { V1_PH_TOP, V1_PH_SHOWN, V1_PH_BOTTOM, V1_PH_VBLANK };
enum { V1_R_PAT, V1_R_NT, V1_R_SAT };
/* band kind bits: 0-10 = register n, 11 = CRAM, 12 = pattern data, 13 = name table, 14 = sprite table, 15 = display on / off (register 1 bit 6, also counted in bit 1 of reg) */
#define V1_KB_CRAM 11
#define V1_KB_PAT 12
#define V1_KB_NT 13
#define V1_KB_SAT 14
#define V1_NKINDS 15

typedef struct {
    uint8_t reg[11];
    uint32_t cram, pat, nt, sat;
} V1Sig;

static struct {
    uint64_t frames, frames_with_shown_lines;
    uint64_t reg_w[16][4], reg_chg[16][4];
    uint64_t vram_w[3][4], vram_chg[3][4];
    uint64_t cram_w[4], cram_chg[4];
    uint64_t status_reads, status_read_ovf, status_read_col, status_read_vbl;
    uint64_t cover_shown[10], cover_all[10];
    uint64_t lines_shown_on, lines_shown_off;
    uint64_t lines_ovf_shown, lines_ovf_hidden, lines_col_shown, lines_col_hidden;
    uint64_t frames_ovf_shown, frames_ovf_any, frames_col_any;
    uint64_t lines_prio_cells, frames_prio_cells;           /* shown lines whose name-table row has a cell with the priority bit; frames with any */
    int cur_prio_cells;
    uint64_t lines_tall, lines_zoom, lines_hlock, lines_vlock, lines_prio_hides, frames_prio_hides, lines_xshift;
    uint64_t bands_hist[12];
    uint64_t kind_total[V1_NKINDS];
    uint64_t kind_by_line[192];
    uint64_t pat_bytes_per_frame_max, nt_bytes_per_frame_max;
    uint64_t sprites_total_max_frame;
    /* greedy bands: a band's picture is drawn with the state at its first line; a new band starts at the first line where that does not give the line the game really drew */
    uint64_t gb_hist[12], gb_kind[V1_NKINDS], gb_by_line[192], gb_lines_total;
    uint64_t end_ok, end_bad, end_bad_lines;                 /* frames a single drawing with the state at line 192 (before the vblank work) reproduces exactly */
    uint64_t first_ok, first_bad;                            /* the same with the state at the first shown line (= one band) */
    /* per frame */
    int cur_gbands, gb_valid;
    V1Sig gb_sig;
    int cur_bands, cur_started, cur_ovf_shown, cur_ovf_any, cur_col_any, cur_prio, cur_shown_lines;
    V1Sig last;
    uint32_t cram_ver, pat_ver, nt_ver, sat_ver;
    uint64_t cur_pat_bytes, cur_nt_bytes;
} V1S;

static int v1_phase(const HW *hw)
{
    int l = hw->line;
    return l < 24 ? V1_PH_TOP : l < 168 ? V1_PH_SHOWN : l < 192 ? V1_PH_BOTTOM : V1_PH_VBLANK;
}

static void v1_reg_write(HW *hw, int r, uint8_t old, uint8_t nv)
{
    int ph = v1_phase(hw);
    V1S.reg_w[r][ph]++;
    if (old != nv) V1S.reg_chg[r][ph]++;
}

static void v1_vram_write(HW *hw, unsigned a, uint8_t old, uint8_t nv)
{
    unsigned sat = (unsigned)(hw->reg[5] & 0x7E) << 7, nt = (unsigned)(hw->reg[2] & 0x0E) << 10;
    int region = ((a - sat) & 0x3FFF) < 0x100 ? V1_R_SAT : ((a - nt) & 0x3FFF) < 0x700 ? V1_R_NT : V1_R_PAT;
    int ph = v1_phase(hw);
    V1S.vram_w[region][ph]++;
    if (old != nv) {
        V1S.vram_chg[region][ph]++;
        if (region == V1_R_PAT) V1S.pat_ver++; else if (region == V1_R_NT) V1S.nt_ver++; else V1S.sat_ver++;
    }
    if (region == V1_R_PAT) V1S.cur_pat_bytes++;
    if (region == V1_R_NT) V1S.cur_nt_bytes++;
}

static void v1_cram_write(HW *hw, unsigned old, unsigned nv)
{
    int ph = v1_phase(hw);
    V1S.cram_w[ph]++;
    if (old != nv) { V1S.cram_chg[ph]++; V1S.cram_ver++; }
}

static void v1_status_read(HW *hw, uint8_t s)
{
    (void)hw;
    V1S.status_reads++;
    if (s & 0x40) V1S.status_read_ovf++;
    if (s & 0x20) V1S.status_read_col++;
    if (s & 0x80) V1S.status_read_vbl++;
}

static void v1_frame_end(void)
{
    if (V1S.cur_started) {
        V1S.frames_with_shown_lines++;
        int b = V1S.cur_bands + 1;
        V1S.bands_hist[b > 11 ? 11 : b]++;
        V1S.gb_hist[V1S.cur_gbands > 11 ? 11 : V1S.cur_gbands]++;
        if (V1S.cur_gbands == 1) V1S.first_ok++; else V1S.first_bad++;
        if (V1S.cur_ovf_shown) V1S.frames_ovf_shown++;
        if (V1S.cur_ovf_any) V1S.frames_ovf_any++;
        if (V1S.cur_col_any) V1S.frames_col_any++;
        if (V1S.cur_prio) V1S.frames_prio_hides++;
        if (V1S.cur_prio_cells) V1S.frames_prio_cells++;
    }
    V1S.frames++;
    if (V1S.cur_pat_bytes > V1S.pat_bytes_per_frame_max) V1S.pat_bytes_per_frame_max = V1S.cur_pat_bytes;
    if (V1S.cur_nt_bytes > V1S.nt_bytes_per_frame_max) V1S.nt_bytes_per_frame_max = V1S.cur_nt_bytes;
    V1S.cur_gbands = 0; V1S.gb_valid = 0;
    V1S.cur_bands = V1S.cur_started = V1S.cur_ovf_shown = V1S.cur_ovf_any = V1S.cur_col_any = V1S.cur_prio = 0; V1S.cur_prio_cells = 0;
    V1S.cur_pat_bytes = V1S.cur_nt_bytes = 0;
}

static void v1_sig(const HW *hw, V1Sig *s)
{
    s->reg[0] = hw->reg[0] & 0xEC;            /* scroll locks, left blank, sprite shift, mode bit */
    s->reg[1] = hw->reg[1] & 0x43;            /* display on, 8x16 sprites, zoom */
    s->reg[2] = hw->reg[2] & 0x0E;
    s->reg[3] = 0; s->reg[4] = 0;
    s->reg[5] = hw->reg[5] & 0x7E;
    s->reg[6] = hw->reg[6] & 0x04;
    s->reg[7] = hw->reg[7] & 0x0F;
    s->reg[8] = hw->reg[8];
    s->reg[9] = hw->vscroll;                  /* the model latches the vertical scroll once per frame */
    s->reg[10] = 0;
    s->cram = V1S.cram_ver; s->pat = V1S.pat_ver; s->nt = V1S.nt_ver; s->sat = V1S.sat_ver;
}

/* called before the line renderer runs for line y (y < 262 is only called for y < 192 by the model) */
static uint8_t v1_line_pre(HW *hw, int y)
{
    uint8_t st = hw->status;
    hw->status &= (uint8_t)~0x60;              /* so that the flags this line sets can be told from older ones */
    if (y >= 24 && y < 168) {
        V1Sig s;
        v1_sig(hw, &s);
        if (V1S.cur_started) {
            unsigned mask = 0;
            for (int r = 0; r < 11; r++) if (s.reg[r] != V1S.last.reg[r]) mask |= 1u << r;
            if (s.cram != V1S.last.cram) mask |= 1u << V1_KB_CRAM;
            if (s.pat != V1S.last.pat) mask |= 1u << V1_KB_PAT;
            if (s.nt != V1S.last.nt) mask |= 1u << V1_KB_NT;
            if (s.sat != V1S.last.sat) mask |= 1u << V1_KB_SAT;
            if (mask) {
                V1S.cur_bands++;
                V1S.kind_by_line[y]++;
                for (int k = 0; k < V1_NKINDS; k++) if (mask >> k & 1) V1S.kind_total[k]++;
            }
        }
        V1S.cur_started = 1;
        V1S.last = s;
    }
    return st;
}

static void v1_line_post(HW *hw, int y, uint8_t st_before)
{
    uint8_t flags = hw->status & 0x60;
    hw->status = (uint8_t)((hw->status & ~0x60) | (st_before & 0x60) | flags);
    int shown = y >= 24 && y < 168;
    if (shown) {
        static HW *snap;
        if (!snap) snap = malloc(sizeof *snap);
        int fresh = !V1S.gb_valid;
        if (!fresh) {
            snap->spr_dirty = 1;
            v1_quiet = 1;
            render_line_impl(snap, y);
            v1_quiet = 0;
            if (memcmp(&snap->fb[(y - 24) * GG_W], &hw->fb[(y - 24) * GG_W], GG_W * sizeof(uint32_t)) != 0) {
                V1Sig now;
                v1_sig(hw, &now);
                for (int r = 0; r < 11; r++) if (now.reg[r] != V1S.gb_sig.reg[r]) V1S.gb_kind[r]++;
                if (now.cram != V1S.gb_sig.cram) V1S.gb_kind[V1_KB_CRAM]++;
                if (now.pat != V1S.gb_sig.pat) V1S.gb_kind[V1_KB_PAT]++;
                if (now.nt != V1S.gb_sig.nt) V1S.gb_kind[V1_KB_NT]++;
                if (now.sat != V1S.gb_sig.sat) V1S.gb_kind[V1_KB_SAT]++;
                V1S.gb_by_line[y]++;
                V1S.gb_lines_total++;
                fresh = 1;
                V1S.cur_gbands++;
            }
        } else {
            V1S.cur_gbands = 1;
        }
        if (fresh) {
            memcpy(snap, hw, sizeof *snap);
            v1_sig(hw, &V1S.gb_sig);
            V1S.gb_valid = 1;
        }
    }
    int on = (hw->reg[1] & 0x40) && (hw->reg[0] & 0x04);
    if (shown) { if (on) V1S.lines_shown_on++; else V1S.lines_shown_off++; }
    if (!on || y >= 192) return;
    int cover = hw->spr_cnt[y];
    V1S.cover_all[cover]++;
    if (shown) V1S.cover_shown[cover]++;
    if (flags & 0x40) { if (shown) { V1S.lines_ovf_shown++; V1S.cur_ovf_shown = 1; } else V1S.lines_ovf_hidden++; V1S.cur_ovf_any = 1; }
    if (flags & 0x20) { if (shown) V1S.lines_col_shown++; else V1S.lines_col_hidden++; V1S.cur_col_any = 1; }
    if (shown) {
        unsigned nt = (unsigned)(hw->reg[2] & 0x0E) << 10;
        int ys = (y + hw->vscroll) % 224;
        for (int c = 0; c < 32; c++) {
            unsigned ea = nt + (unsigned)(ys >> 3) * 64 + (unsigned)c * 2;
            if (hw->vram[(ea + 1) & 0x3FFF] & 0x10) { V1S.lines_prio_cells++; V1S.cur_prio_cells = 1; break; }
        }
    }
    if (hw->reg[1] & 2) V1S.lines_tall++;
    if (hw->reg[1] & 1) V1S.lines_zoom++;
    if (hw->reg[0] & 0x08) V1S.lines_xshift++;
    if (shown && (hw->reg[0] & 0x40) && y < 16 + 24) V1S.lines_hlock++;
    if (shown && (hw->reg[0] & 0x80)) V1S.lines_vlock++;
}

/* at the start of line 192 (before any work of the vertical blank): would ONE drawing of the 144 shown lines with the state of this moment give the frame the game drew? */
static void v1_line192(const HW *hw)
{
    static HW *endst;
    if (!endst) endst = malloc(sizeof *endst);
    memcpy(endst, hw, sizeof *endst);
    endst->spr_dirty = 1;
    v1_quiet = 1;
    for (int y = 24; y < 168; y++) render_line_impl(endst, y);
    v1_quiet = 0;
    int bad = 0;
    for (int y = 0; y < GG_H; y++)
        if (memcmp(&endst->fb[y * GG_W], &hw->fb[y * GG_W], GG_W * sizeof(uint32_t)) != 0) bad++;
    if (bad) { V1S.end_bad++; V1S.end_bad_lines += (uint64_t)bad; } else V1S.end_ok++;
}

/* a sprite pixel hidden by a background priority tile (counted by the renderer through this hook) */
static void v1_prio_hide(void) { if (v1_quiet) return; V1S.lines_prio_hides++; V1S.cur_prio = 1; }

static const char *v1_kind_name[V1_NKINDS] = {"reg0 (scroll locks / mode)", "reg1 (display, sprite size)", "reg2 (name table base)", "reg3", "reg4", "reg5 (sprite table base)", "reg6 (sprite patterns)",
                                              "reg7 (backdrop colour)", "reg8 (horizontal scroll)", "reg9 (vertical scroll, latched)", "reg10", "CRAM (palette)", "pattern data (tiles)", "name table", "sprite table"};
static const char *v1_phase_name[4] = {"lines 0-23", "lines 24-167 (shown)", "lines 168-191", "lines 192-261 (vblank)"};

void hw_v1_report(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    v1_frame_end();
    fprintf(f, "frames %llu, with shown lines %llu\n", (unsigned long long)V1S.frames, (unsigned long long)V1S.frames_with_shown_lines);
    fprintf(f, "shown lines with display on %llu, off %llu\n", (unsigned long long)V1S.lines_shown_on, (unsigned long long)V1S.lines_shown_off);
    fprintf(f, "bands per frame (picture-relevant state change between two shown lines starts a new band): ");
    for (int i = 1; i < 12; i++) fprintf(f, "%d:%llu ", i, (unsigned long long)V1S.bands_hist[i]);
    fprintf(f, "(11 = 11 or more)\n");
    fprintf(f, "greedy bands per frame (a new band only where drawing the line with the state at the band's first line gives different pixels): ");
    for (int i = 1; i < 12; i++) fprintf(f, "%d:%llu ", i, (unsigned long long)V1S.gb_hist[i]);
    fprintf(f, "(11 = 11 or more)\n");
    fprintf(f, "greedy band starts by cause (state that differs from the band's first line):");
    for (int k = 0; k < V1_NKINDS; k++) if (V1S.gb_kind[k]) fprintf(f, " [%s %llu]", v1_kind_name[k], (unsigned long long)V1S.gb_kind[k]);
    fprintf(f, "\ngreedy band starts by line:");
    for (int y = 0; y < 192; y++) if (V1S.gb_by_line[y]) fprintf(f, " %d:%llu", y, (unsigned long long)V1S.gb_by_line[y]);
    fprintf(f, "\none drawing with the state of line 192 reproduces the frame exactly: %llu frames, not exactly: %llu frames (%llu wrong lines in total)\n", (unsigned long long)V1S.end_ok,
            (unsigned long long)V1S.end_bad, (unsigned long long)V1S.end_bad_lines);
    fprintf(f, "band boundaries (any state change, upper bound) by kind:");
    for (int k = 0; k < V1_NKINDS; k++) if (V1S.kind_total[k]) fprintf(f, " [%s %llu]", v1_kind_name[k], (unsigned long long)V1S.kind_total[k]);
    fprintf(f, "\nband boundaries by line (line: count):");
    for (int y = 0; y < 192; y++) if (V1S.kind_by_line[y]) fprintf(f, " %d:%llu", y, (unsigned long long)V1S.kind_by_line[y]);
    fprintf(f, "\nregister writes (changed / written) by phase:\n");
    for (int r = 0; r < 11; r++) {
        uint64_t any = 0;
        for (int p = 0; p < 4; p++) any += V1S.reg_w[r][p];
        if (!any) continue;
        fprintf(f, "  reg%-2d", r);
        for (int p = 0; p < 4; p++) fprintf(f, "  %s: %llu/%llu", v1_phase_name[p], (unsigned long long)V1S.reg_chg[r][p], (unsigned long long)V1S.reg_w[r][p]);
        fprintf(f, "\n");
    }
    static const char *rn[3] = {"pattern data", "name table", "sprite table"};
    fprintf(f, "VRAM writes (changed / written) by region and phase:\n");
    for (int r = 0; r < 3; r++) {
        fprintf(f, "  %-13s", rn[r]);
        for (int p = 0; p < 4; p++) fprintf(f, "  %s: %llu/%llu", v1_phase_name[p], (unsigned long long)V1S.vram_chg[r][p], (unsigned long long)V1S.vram_w[r][p]);
        fprintf(f, "\n");
    }
    fprintf(f, "  CRAM         ");
    for (int p = 0; p < 4; p++) fprintf(f, "  %s: %llu/%llu", v1_phase_name[p], (unsigned long long)V1S.cram_chg[p], (unsigned long long)V1S.cram_w[p]);
    fprintf(f, "\nmost pattern bytes written in one frame %llu, most name-table bytes %llu\n", (unsigned long long)V1S.pat_bytes_per_frame_max, (unsigned long long)V1S.nt_bytes_per_frame_max);
    fprintf(f, "sprites per line (lines 0-191, display on), count of lines by sprites covering the line (9 = more than 8):");
    for (int c = 0; c < 10; c++) fprintf(f, " %d:%llu", c, (unsigned long long)V1S.cover_all[c]);
    fprintf(f, "\n  same for shown lines 24-167:");
    for (int c = 0; c < 10; c++) fprintf(f, " %d:%llu", c, (unsigned long long)V1S.cover_shown[c]);
    fprintf(f, "\noverflow (more than 8 on a line): shown lines %llu, hidden lines %llu; frames with it on a shown line %llu, anywhere %llu\n", (unsigned long long)V1S.lines_ovf_shown,
            (unsigned long long)V1S.lines_ovf_hidden, (unsigned long long)V1S.frames_ovf_shown, (unsigned long long)V1S.frames_ovf_any);
    fprintf(f, "sprite collision: lines on shown lines %llu, on hidden lines %llu; frames with any %llu\n", (unsigned long long)V1S.lines_col_shown, (unsigned long long)V1S.lines_col_hidden,
            (unsigned long long)V1S.frames_col_any);
    fprintf(f, "lines (0-191, display on) with 8x16 sprites %llu, zoomed %llu, sprite x shift %llu; shown lines with horizontal lock (top rows) %llu, vertical lock %llu\n",
            (unsigned long long)V1S.lines_tall, (unsigned long long)V1S.lines_zoom, (unsigned long long)V1S.lines_xshift, (unsigned long long)V1S.lines_hlock, (unsigned long long)V1S.lines_vlock);
    fprintf(f, "sprite pixels hidden behind a priority background tile: lines %llu, frames %llu\n", (unsigned long long)V1S.lines_prio_hides, (unsigned long long)V1S.frames_prio_hides);
    fprintf(f, "status port reads %llu: with the overflow bit %llu, with the collision bit %llu, with the frame bit %llu\n", (unsigned long long)V1S.status_reads, (unsigned long long)V1S.status_read_ovf,
            (unsigned long long)V1S.status_read_col, (unsigned long long)V1S.status_read_vbl);
    fprintf(f, "K frames %llu\nK shown_on %llu\nK shown_off %llu\n", (unsigned long long)V1S.frames, (unsigned long long)V1S.lines_shown_on, (unsigned long long)V1S.lines_shown_off);
    for (int i = 1; i < 12; i++) fprintf(f, "K gb%d %llu\n", i, (unsigned long long)V1S.gb_hist[i]);
    for (int i = 1; i < 12; i++) fprintf(f, "K ub%d %llu\n", i, (unsigned long long)V1S.bands_hist[i]);
    fprintf(f, "K end_ok %llu\nK end_bad %llu\nK end_bad_lines %llu\n", (unsigned long long)V1S.end_ok, (unsigned long long)V1S.end_bad, (unsigned long long)V1S.end_bad_lines);
    fprintf(f, "K ovf_lines_shown %llu\nK ovf_frames_shown %llu\nK ovf_frames_any %llu\n", (unsigned long long)V1S.lines_ovf_shown, (unsigned long long)V1S.frames_ovf_shown, (unsigned long long)V1S.frames_ovf_any);
    fprintf(f, "K col_lines_shown %llu\nK col_frames_any %llu\n", (unsigned long long)V1S.lines_col_shown, (unsigned long long)V1S.frames_col_any);
    fprintf(f, "K tall %llu\nK zoom %llu\nK xshift %llu\nK hlock %llu\nK vlock %llu\nK prio_lines %llu\nK prio_frames %llu\n", (unsigned long long)V1S.lines_tall, (unsigned long long)V1S.lines_zoom,
            (unsigned long long)V1S.lines_xshift, (unsigned long long)V1S.lines_hlock, (unsigned long long)V1S.lines_vlock, (unsigned long long)V1S.lines_prio_hides, (unsigned long long)V1S.frames_prio_hides);
    for (int c = 0; c < 10; c++) fprintf(f, "K cover%d %llu\n", c, (unsigned long long)V1S.cover_shown[c]);
    fprintf(f, "K prio_cell_lines %llu\nK prio_cell_frames %llu\n", (unsigned long long)V1S.lines_prio_cells, (unsigned long long)V1S.frames_prio_cells);
    fprintf(f, "K sread %llu\nK sread_ovf %llu\nK sread_col %llu\nK pat_max %llu\nK nt_max %llu\n", (unsigned long long)V1S.status_reads, (unsigned long long)V1S.status_read_ovf,
            (unsigned long long)V1S.status_read_col, (unsigned long long)V1S.pat_bytes_per_frame_max, (unsigned long long)V1S.nt_bytes_per_frame_max);
    fclose(f);
}
