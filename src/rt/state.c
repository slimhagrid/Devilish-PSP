#include <string.h>
#include "z80rt.h"
#include "state.h"

#define ST_VERSION 1

typedef struct {
    uint8_t *p;              /* the next byte */
    uint8_t *end;
    int save;                /* 1 = write the fields, 0 = read them, 2 = only measure */
    int bad;
} W;

static void f8(W *w, uint8_t *v)
{
    if (w->p + 1 > w->end) { w->bad = 1; return; }
    if (w->save == 2) { w->p += 1; return; }
    if (w->save) *w->p = *v; else *v = *w->p;
    w->p++;
}
static void f16(W *w, uint16_t *v)
{
    if (w->p + 2 > w->end) { w->bad = 1; return; }
    if (w->save == 2) { w->p += 2; return; }
    if (w->save) { w->p[0] = (uint8_t)*v; w->p[1] = (uint8_t)(*v >> 8); } else *v = (uint16_t)(w->p[0] | w->p[1] << 8);
    w->p += 2;
}
static void f32(W *w, uint32_t *v)
{
    if (w->p + 4 > w->end) { w->bad = 1; return; }
    if (w->save == 2) { w->p += 4; return; }
    if (w->save) { for (int i = 0; i < 4; i++) w->p[i] = (uint8_t)(*v >> (8 * i)); }
    else { uint32_t x = 0; for (int i = 0; i < 4; i++) x |= (uint32_t)w->p[i] << (8 * i); *v = x; }
    w->p += 4;
}
static void f64(W *w, uint64_t *v)
{
    if (w->p + 8 > w->end) { w->bad = 1; return; }
    if (w->save == 2) { w->p += 8; return; }
    if (w->save) { for (int i = 0; i < 8; i++) w->p[i] = (uint8_t)(*v >> (8 * i)); }
    else { uint64_t x = 0; for (int i = 0; i < 8; i++) x |= (uint64_t)w->p[i] << (8 * i); *v = x; }
    w->p += 8;
}
#define FI32(w, v) do { uint32_t x_ = (uint32_t)(v); f32(w, &x_); (v) = (int32_t)x_; } while (0)
static void fb(W *w, uint8_t *v, size_t n)
{
    if (w->p + n > w->end) { w->bad = 1; return; }
    if (w->save == 2) { w->p += n; return; }
    if (w->save) memcpy(w->p, v, n); else memcpy(v, w->p, n);
    w->p += n;
}

/* every field of the machine, in a fixed order; the same function writes and reads */
static void fields(W *w)
{
    uint8_t *r = &M.a;                                   /* a f b c d e h l */
    fb(w, r, 8);
    fb(w, &M.a2, 8);                                     /* a2 f2 b2 c2 d2 e2 h2 l2 */
    f16(w, &M.ix); f16(w, &M.iy); f16(w, &M.sp); f16(w, &M.pc);
    f8(w, &M.i); f8(w, &M.iff1); f8(w, &M.iff2); f8(w, &M.im); f8(w, &M.ei_delay); f8(w, &M.irq_line); f8(w, &M.irq_ready); f8(w, &M.halted);
    f8(w, &M.rbase); f32(w, &M.rcnt); FI32(w, M.acc); FI32(w, M.ev_left); f16(w, &M.cur);

    fb(w, G.ram, sizeof G.ram);
    fb(w, G.map, 4);
    fb(w, G.bank, 3);
    fb(w, G.vram, sizeof G.vram);
    fb(w, G.cram, sizeof G.cram);
    fb(w, G.reg, sizeof G.reg);
    f16(w, &G.vaddr);
    f8(w, &G.vcode); f8(w, &G.vlatch); f8(w, &G.vlatch_set); f8(w, &G.vbuf); f8(w, &G.cram_latch);
    f8(w, &G.status); f8(w, &G.line_pending); f8(w, &G.line_counter); f8(w, &G.vscroll);
    FI32(w, G.line); FI32(w, G.line_cycle); FI32(w, G.line_drawn);
    f64(w, &G.cycles); f64(w, &G.frame);

    fb(w, G.psg_tone_lo, 3); fb(w, G.psg_tone_hi, 3);
    for (int i = 0; i < 3; i++) f16(w, &G.psg_tone[i]);
    fb(w, G.psg_vol, 4);
    f8(w, &G.psg_noise); f8(w, &G.psg_latch); f8(w, &G.gg_stereo);
    f64(w, &G.psg.sample); f64(w, &G.psg.tick); f32(w, &G.psg.rem);
    for (int i = 0; i < 4; i++) f16(w, &G.psg.cnt[i]);
    fb(w, G.psg.out, 4);
    f16(w, &G.psg.lfsr);
    FI32(w, G.psg.lpl); FI32(w, G.psg.lpr);

    f8(w, &G.pad); f8(w, &G.io_ctrl); f8(w, &G.mem_ctrl);
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

#define HDR 17                                           /* "DVST", version, rom crc (4), payload length (4), spare (4) */

size_t st_save(uint8_t *out, size_t cap, uint32_t rom_crc)
{
    if (cap < ST_MAX) return 0;
    memcpy(out, "DVST", 4);
    out[4] = ST_VERSION;
    for (int i = 0; i < 4; i++) out[5 + i] = (uint8_t)(rom_crc >> (8 * i));
    W w = { out + HDR, out + cap - 4, 1, 0 };
    fields(&w);
    if (w.bad) return 0;
    uint32_t len = (uint32_t)(w.p - (out + HDR));
    for (int i = 0; i < 4; i++) out[9 + i] = (uint8_t)(len >> (8 * i));
    memset(out + 13, 0, 4);
    uint32_t crc = crc32_of(out, HDR + len);
    for (int i = 0; i < 4; i++) out[HDR + len + i] = (uint8_t)(crc >> (8 * i));
    return HDR + len + 4;
}

int st_load(const uint8_t *in, size_t n, uint32_t rom_crc)
{
    if (n < HDR + 4 || memcmp(in, "DVST", 4) != 0 || in[4] != ST_VERSION) return 0;
    uint32_t rc = 0, len = 0, crc = 0;
    for (int i = 0; i < 4; i++) { rc |= (uint32_t)in[5 + i] << (8 * i); len |= (uint32_t)in[9 + i] << (8 * i); }
    if (rc != rom_crc || len > ST_MAX || (size_t)HDR + len + 4 != n) return 0;
    for (int i = 0; i < 4; i++) crc |= (uint32_t)in[HDR + len + i] << (8 * i);
    if (crc != crc32_of(in, HDR + len)) return 0;
    uint8_t *p = (uint8_t *)in + HDR;                    /* the read side never writes to the image */
    W m = { NULL, (uint8_t *)(size_t)-1, 2, 0 };           /* measure first: an image of another layout is refused before anything is changed */
    W *mw = &m; mw->p = p;
    fields(mw);
    if ((uint32_t)(mw->p - p) != len) return 0;
    W w = { p, p + len, 0, 0 };
    fields(&w);
    if (w.bad) rt_fatal("save state: internal error while reading");
    hw_state_restored(&G);
    return 1;
}
