/* Dev-time Z80 reference core, see z80.h. */
#include "z80.h"
#include <string.h>

#define SF 0x80
#define ZF 0x40
#define YF 0x20
#define HF 0x10
#define XF 0x08
#define PF 0x04
#define NF 0x02
#define CF 0x01

static uint8_t par[256];
static int tab_init;

static void init_tabs(void)
{
    for (int i = 0; i < 256; i++) {
        int p = 1;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        par[i] = p ? PF : 0;
    }
    tab_init = 1;
}

void z80_reset(Z80 *c)
{
    if (!tab_init) init_tabs();
    memset(c->r, 0, sizeof c->r);
    memset(c->r2, 0, sizeof c->r2);
    c->f = c->f2 = 0xFF;
    c->r[7] = 0xFF;
    c->ix = c->iy = 0xFFFF;
    c->sp = 0xFFFF;
    c->pc = 0;
    c->memptr = 0;
    c->i = c->rr = 0;
    c->iff1 = c->iff2 = c->im = 0;
    c->halted = c->ei_delay = c->irq = c->q = c->fmod = 0;
}

#define RD(a) (c->rd(c->u, (uint16_t)(a)))
#define WR(a, v) (c->wr(c->u, (uint16_t)(a), (uint8_t)(v)))
#define A c->r[7]
#define SETF(v) do { c->f = (uint8_t)(v); c->fmod = 1; } while (0)

static inline uint16_t get16(Z80 *c, int hi) { return (uint16_t)(c->r[hi] << 8 | c->r[hi + 1]); }
static inline void set16(Z80 *c, int hi, uint16_t v) { c->r[hi] = (uint8_t)(v >> 8); c->r[hi + 1] = (uint8_t)v; }
#define BC get16(c, 0)
#define DE get16(c, 2)
#define HL get16(c, 4)

static inline uint8_t fetch8(Z80 *c)
{
    uint8_t v = RD(c->pc);
    c->pc++;
    return v;
}
static inline uint16_t fetch16(Z80 *c)
{
    uint8_t lo = fetch8(c);
    uint8_t hi = fetch8(c);
    return (uint16_t)(lo | hi << 8);
}
static inline void inc_r(Z80 *c) { c->rr = (uint8_t)((c->rr & 0x80) | ((c->rr + 1) & 0x7F)); }
static inline void push16(Z80 *c, uint16_t v)
{
    c->sp--; WR(c->sp, v >> 8);
    c->sp--; WR(c->sp, v & 0xFF);
}
static inline uint16_t pop16(Z80 *c)
{
    uint8_t lo = RD(c->sp); c->sp++;
    uint8_t hi = RD(c->sp); c->sp++;
    return (uint16_t)(lo | hi << 8);
}

/* index-aware register pair / half access: idx 0 = HL, 1 = IX, 2 = IY */
static inline uint16_t getHLx(Z80 *c, int idx) { return idx == 0 ? HL : idx == 1 ? c->ix : c->iy; }
static inline void setHLx(Z80 *c, int idx, uint16_t v) { if (idx == 0) set16(c, 4, v); else if (idx == 1) c->ix = v; else c->iy = v; }
static inline uint8_t getr(Z80 *c, int idx, int i)
{
    if (idx && (i == 4 || i == 5)) { uint16_t v = idx == 1 ? c->ix : c->iy; return i == 4 ? (uint8_t)(v >> 8) : (uint8_t)v; }
    return c->r[i];
}
static inline void setr(Z80 *c, int idx, int i, uint8_t v)
{
    if (idx && (i == 4 || i == 5)) {
        uint16_t *p = idx == 1 ? &c->ix : &c->iy;
        *p = i == 4 ? (uint16_t)((*p & 0x00FF) | v << 8) : (uint16_t)((*p & 0xFF00) | v);
        return;
    }
    c->r[i] = v;
}

/* ---- 8-bit ALU ---------------------------------------------------------------------------------------------------------------------------------- */
static uint8_t alu_flags_add(uint8_t a, uint8_t b, int cy, uint8_t *res)
{
    int r = a + b + cy;
    uint8_t v = (uint8_t)r;
    *res = v;
    return (uint8_t)((v & (SF | YF | XF)) | (v ? 0 : ZF) | ((a ^ b ^ v) & HF) | ((~(a ^ b) & (a ^ v) & 0x80) ? PF : 0) | (r > 0xFF ? CF : 0));
}
static uint8_t alu_flags_sub(uint8_t a, uint8_t b, int cy, uint8_t *res)
{
    int r = a - b - cy;
    uint8_t v = (uint8_t)r;
    *res = v;
    return (uint8_t)((v & (SF | YF | XF)) | (v ? 0 : ZF) | ((a ^ b ^ v) & HF) | (((a ^ b) & (a ^ v) & 0x80) ? PF : 0) | NF | (r & 0x100 ? CF : 0));
}

static void alu(Z80 *c, int op, uint8_t b)
{
    uint8_t res, f;
    switch (op) {
    case 0: f = alu_flags_add(A, b, 0, &res); A = res; SETF(f); break;
    case 1: f = alu_flags_add(A, b, c->f & CF, &res); A = res; SETF(f); break;
    case 2: f = alu_flags_sub(A, b, 0, &res); A = res; SETF(f); break;
    case 3: f = alu_flags_sub(A, b, c->f & CF, &res); A = res; SETF(f); break;
    case 4: A &= b; SETF((A & (SF | YF | XF)) | (A ? 0 : ZF) | HF | par[A]); break;
    case 5: A ^= b; SETF((A & (SF | YF | XF)) | (A ? 0 : ZF) | par[A]); break;
    case 6: A |= b; SETF((A & (SF | YF | XF)) | (A ? 0 : ZF) | par[A]); break;
    default: f = alu_flags_sub(A, b, 0, &res); SETF((f & ~(YF | XF)) | (b & (YF | XF))); break;   /* cp: X/Y from the operand */
    }
}

static uint8_t inc8(Z80 *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    SETF((c->f & CF) | (r & (SF | YF | XF)) | (r ? 0 : ZF) | ((r & 0x0F) == 0 ? HF : 0) | (r == 0x80 ? PF : 0));
    return r;
}
static uint8_t dec8(Z80 *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    SETF((c->f & CF) | (r & (SF | YF | XF)) | (r ? 0 : ZF) | ((r & 0x0F) == 0x0F ? HF : 0) | (r == 0x7F ? PF : 0) | NF);
    return r;
}

static uint16_t add16(Z80 *c, uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b;
    SETF((c->f & (SF | ZF | PF)) | ((r >> 8) & (YF | XF)) | (((a ^ b ^ r) >> 8) & HF) | (r > 0xFFFF ? CF : 0));
    c->memptr = (uint16_t)(a + 1);
    return (uint16_t)r;
}
static uint16_t adc16(Z80 *c, uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b + (c->f & CF);
    uint16_t v = (uint16_t)r;
    SETF(((v >> 8) & (SF | YF | XF)) | (v ? 0 : ZF) | (((a ^ b ^ v) >> 8) & HF) | ((~(a ^ b) & (a ^ v) & 0x8000) ? PF : 0) | (r > 0xFFFF ? CF : 0));
    c->memptr = (uint16_t)(a + 1);
    return v;
}
static uint16_t sbc16(Z80 *c, uint16_t a, uint16_t b)
{
    int32_t r = (int32_t)a - b - (c->f & CF);
    uint16_t v = (uint16_t)r;
    SETF(((v >> 8) & (SF | YF | XF)) | (v ? 0 : ZF) | (((a ^ b ^ v) >> 8) & HF) | (((a ^ b) & (a ^ v) & 0x8000) ? PF : 0) | NF | (r & 0x10000 ? CF : 0));
    c->memptr = (uint16_t)(a + 1);
    return v;
}

static void daa(Z80 *c)
{
    uint8_t a = A, corr = 0, cf = c->f & CF;
    if ((c->f & HF) || (a & 0x0F) > 9) corr |= 0x06;
    if (cf || a > 0x99) { corr |= 0x60; cf = CF; }
    uint8_t h;
    if (c->f & NF) { h = ((c->f & HF) && (a & 0x0F) < 6) ? HF : 0; a = (uint8_t)(a - corr); }
    else { h = (a & 0x0F) > 9 ? HF : 0; a = (uint8_t)(a + corr); }
    A = a;
    SETF((a & (SF | YF | XF)) | (a ? 0 : ZF) | h | par[a] | (c->f & NF) | cf);
}

/* rotate / shift group (CB 00-3F): returns the result, sets all flags */
static uint8_t rot(Z80 *c, int op, uint8_t v)
{
    uint8_t cy, r;
    switch (op) {
    case 0: cy = v >> 7; r = (uint8_t)(v << 1 | cy); break;                    /* rlc */
    case 1: cy = v & 1; r = (uint8_t)(v >> 1 | cy << 7); break;                 /* rrc */
    case 2: cy = v >> 7; r = (uint8_t)(v << 1 | (c->f & CF)); break;            /* rl */
    case 3: cy = v & 1; r = (uint8_t)(v >> 1 | (c->f & CF) << 7); break;        /* rr */
    case 4: cy = v >> 7; r = (uint8_t)(v << 1); break;                          /* sla */
    case 5: cy = v & 1; r = (uint8_t)(v >> 1 | (v & 0x80)); break;              /* sra */
    case 6: cy = v >> 7; r = (uint8_t)(v << 1 | 1); break;                      /* sll (undocumented) */
    default: cy = v & 1; r = (uint8_t)(v >> 1); break;                          /* srl */
    }
    SETF((r & (SF | YF | XF)) | (r ? 0 : ZF) | par[r] | cy);
    return r;
}

static int cond(Z80 *c, int cc)
{
    switch (cc) {
    case 0: return !(c->f & ZF);
    case 1: return c->f & ZF;
    case 2: return !(c->f & CF);
    case 3: return c->f & CF;
    case 4: return !(c->f & PF);
    case 5: return c->f & PF;
    case 6: return !(c->f & SF);
    default: return c->f & SF;
    }
}

/* ---- CB group ---------------------------------------------------------------------------------------------------------------------------------- */
/* idx == 0: CB op on register / (HL); idx != 0: DD/FD CB d op with the effective address `ea` */
static int exec_cb(Z80 *c, int idx, uint16_t ea, uint8_t op)
{
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    int mem = idx || z == 6;
    uint16_t addr = idx ? ea : HL;
    uint8_t v = mem ? RD(addr) : c->r[z];
    if (x == 1) {
        uint8_t m = (uint8_t)(v & (1 << y));
        uint8_t xy = mem ? (uint8_t)(c->memptr >> 8) : v;
        if (idx) xy = (uint8_t)(ea >> 8);
        SETF((c->f & CF) | HF | (m ? 0 : (ZF | PF)) | (y == 7 && m ? SF : 0) | (xy & (YF | XF)));
        return idx ? 20 : mem ? 12 : 8;
    }
    uint8_t r;
    if (x == 0) r = rot(c, y, v);
    else if (x == 2) r = (uint8_t)(v & ~(1 << y));
    else r = (uint8_t)(v | (1 << y));
    if (mem) WR(addr, r);
    if (idx) { if (z != 6) c->r[z] = r; }
    else if (z != 6) c->r[z] = r;
    return idx ? 23 : mem ? 15 : 8;
}

/* ---- block instructions ------------------------------------------------------------------------------------------------------------------------ */
static int exec_block(Z80 *c, int op)
{
    int y = (op >> 3) & 7, z = op & 7;
    int dir = (y & 1) ? -1 : 1;           /* ldd / cpd / ind / outd go down */
    int rep = y >= 6;
    int t = 16;
    switch (z) {
    case 0: {   /* ldi / ldd / ldir / lddr */
        uint8_t v = RD(HL);
        WR(DE, v);
        set16(c, 4, (uint16_t)(HL + dir));
        set16(c, 2, (uint16_t)(DE + dir));
        set16(c, 0, (uint16_t)(BC - 1));
        uint8_t n = (uint8_t)(v + A);
        SETF((c->f & (SF | ZF | CF)) | ((n & 2) << 4) | (n & 8) | (BC ? PF : 0));
        if (rep && BC) {
            c->pc = (uint16_t)(c->pc - 2);
            c->memptr = (uint16_t)(c->pc + 1);
            SETF((c->f & ~(YF | XF)) | ((c->pc >> 8) & (YF | XF)));
            t = 21;
        }
        break;
    }
    case 1: {   /* cpi / cpd / cpir / cpdr */
        uint8_t v = RD(HL);
        uint8_t res;
        uint8_t f = alu_flags_sub(A, v, 0, &res);
        int h = (f & HF) != 0;
        set16(c, 4, (uint16_t)(HL + dir));
        set16(c, 0, (uint16_t)(BC - 1));
        uint8_t n = (uint8_t)(res - (h ? 1 : 0));
        SETF((c->f & CF) | NF | (res & SF) | (res ? 0 : ZF) | (h ? HF : 0) | (n & 8) | ((n & 2) << 4) | (BC ? PF : 0));
        c->memptr = (uint16_t)(c->memptr + dir);
        if (rep && BC && res) {
            c->pc = (uint16_t)(c->pc - 2);
            c->memptr = (uint16_t)(c->pc + 1);
            SETF((c->f & ~(YF | XF)) | ((c->pc >> 8) & (YF | XF)));
            t = 21;
        }
        break;
    }
    case 2: {   /* ini / ind / inir / indr */
        c->memptr = (uint16_t)(BC + dir);
        uint8_t v = c->in(c->u, BC);
        WR(HL, v);
        c->r[0]--;
        set16(c, 4, (uint16_t)(HL + dir));
        int k = v + ((c->r[1] + dir) & 0xFF);
        uint8_t b = c->r[0];
        SETF((b & (SF | YF | XF)) | (b ? 0 : ZF) | (k > 255 ? (HF | CF) : 0) | par[(uint8_t)((k & 7) ^ b)] | (v & 0x80 ? NF : 0));
        if (rep && b) { c->pc = (uint16_t)(c->pc - 2); t = 21; }
        break;
    }
    default: {  /* outi / outd / otir / otdr */
        uint8_t v = RD(HL);
        c->r[0]--;
        c->out(c->u, BC, v);
        c->memptr = (uint16_t)(BC + dir);
        set16(c, 4, (uint16_t)(HL + dir));
        int k = v + c->r[5];
        uint8_t b = c->r[0];
        SETF((b & (SF | YF | XF)) | (b ? 0 : ZF) | (k > 255 ? (HF | CF) : 0) | par[(uint8_t)((k & 7) ^ b)] | (v & 0x80 ? NF : 0));
        if (rep && b) { c->pc = (uint16_t)(c->pc - 2); t = 21; }
        break;
    }
    }
    return t;
}

/* ---- ED group ---------------------------------------------------------------------------------------------------------------------------------- */
static int exec_ed(Z80 *c)
{
    uint8_t op = fetch8(c);
    inc_r(c);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 2) {
        if (z <= 3 && y >= 4) return exec_block(c, op);
        return 8;
    }
    if (x != 1) return 8;
    switch (z) {
    case 0: {
        c->memptr = (uint16_t)(BC + 1);
        uint8_t v = c->in(c->u, BC);
        if (y != 6) c->r[y] = v;
        SETF((c->f & CF) | (v & (SF | YF | XF)) | (v ? 0 : ZF) | par[v]);
        return 12;
    }
    case 1:
        c->out(c->u, BC, y == 6 ? 0 : c->r[y]);
        c->memptr = (uint16_t)(BC + 1);
        return 12;
    case 2: {
        uint16_t hl = HL, rp = p == 0 ? BC : p == 1 ? DE : p == 2 ? HL : c->sp;
        set16(c, 4, q ? adc16(c, hl, rp) : sbc16(c, hl, rp));
        return 15;
    }
    case 3: {
        uint16_t a = fetch16(c);
        c->memptr = (uint16_t)(a + 1);
        if (q) {
            uint16_t v = (uint16_t)(RD(a) | RD(a + 1) << 8);
            if (p == 0) set16(c, 0, v); else if (p == 1) set16(c, 2, v); else if (p == 2) set16(c, 4, v); else c->sp = v;
        } else {
            uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? HL : c->sp;
            WR(a, v & 0xFF);
            WR(a + 1, v >> 8);
        }
        return 20;
    }
    case 4: {   /* neg */
        uint8_t res, f = alu_flags_sub(0, A, 0, &res);
        A = res; SETF(f);
        return 8;
    }
    case 5:     /* retn / reti */
        c->iff1 = c->iff2;
        c->pc = pop16(c);
        c->memptr = c->pc;
        return 14;
    case 6:
        c->im = (uint8_t)((y & 3) == 2 ? 1 : (y & 3) == 3 ? 2 : 0);
        return 8;
    default:
        switch (y) {
        case 0: c->i = A; return 9;
        case 1: c->rr = A; return 9;
        case 2: A = c->i; SETF((c->f & CF) | (A & (SF | YF | XF)) | (A ? 0 : ZF) | (c->iff2 ? PF : 0)); return 9;
        case 3: A = c->rr; SETF((c->f & CF) | (A & (SF | YF | XF)) | (A ? 0 : ZF) | (c->iff2 ? PF : 0)); return 9;
        case 4: {   /* rrd */
            uint8_t m = RD(HL);
            WR(HL, (A << 4) | (m >> 4));
            A = (uint8_t)((A & 0xF0) | (m & 0x0F));
            c->memptr = (uint16_t)(HL + 1);
            SETF((c->f & CF) | (A & (SF | YF | XF)) | (A ? 0 : ZF) | par[A]);
            return 18;
        }
        case 5: {   /* rld */
            uint8_t m = RD(HL);
            WR(HL, (m << 4) | (A & 0x0F));
            A = (uint8_t)((A & 0xF0) | (m >> 4));
            c->memptr = (uint16_t)(HL + 1);
            SETF((c->f & CF) | (A & (SF | YF | XF)) | (A ? 0 : ZF) | par[A]);
            return 18;
        }
        default: return 8;
        }
    }
}

/* ---- main decoder: unprefixed opcodes and DD / FD ---------------------------------------------------------------------------------------------- */
static int exec_main(Z80 *c, uint8_t op, int idx)
{
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    int ex = idx ? 4 : 0;
    uint16_t ea = 0;
    int d;

    if (x == 0) {
        switch (z) {
        case 0:
            switch (y) {
            case 0: return 4;
            case 1: { uint8_t t; t = A; A = c->r2[7]; c->r2[7] = t; t = c->f; c->f = c->f2; c->f2 = t; c->fmod = 1; return 4; }
            case 2: {   /* djnz */
                d = (int8_t)fetch8(c);
                c->r[0]--;
                if (c->r[0]) { c->pc = (uint16_t)(c->pc + d); c->memptr = c->pc; return 13; }
                return 8;
            }
            case 3: d = (int8_t)fetch8(c); c->pc = (uint16_t)(c->pc + d); c->memptr = c->pc; return 12;
            default:
                d = (int8_t)fetch8(c);
                if (cond(c, y - 4)) { c->pc = (uint16_t)(c->pc + d); c->memptr = c->pc; return 12; }
                return 7;
            }
        case 1:
            if (q == 0) {
                uint16_t v = fetch16(c);
                if (p == 0) set16(c, 0, v); else if (p == 1) set16(c, 2, v); else if (p == 2) setHLx(c, idx, v); else c->sp = v;
                return 10 + ex;
            } else {
                uint16_t rp = p == 0 ? BC : p == 1 ? DE : p == 2 ? getHLx(c, idx) : c->sp;
                setHLx(c, idx, add16(c, getHLx(c, idx), rp));
                return 11 + ex;
            }
        case 2:
            if (p == 0 || p == 1) {
                uint16_t a = p == 0 ? BC : DE;
                if (q) { A = RD(a); c->memptr = (uint16_t)(a + 1); }
                else { WR(a, A); c->memptr = (uint16_t)(((a + 1) & 0xFF) | A << 8); }
                return 7;
            } else {
                uint16_t a = fetch16(c);
                if (p == 2) {
                    if (q) { uint16_t v = (uint16_t)(RD(a) | RD(a + 1) << 8); setHLx(c, idx, v); }
                    else { uint16_t v = getHLx(c, idx); WR(a, v & 0xFF); WR(a + 1, v >> 8); }
                    c->memptr = (uint16_t)(a + 1);
                    return 16 + ex;
                }
                if (q) { A = RD(a); c->memptr = (uint16_t)(a + 1); }
                else { WR(a, A); c->memptr = (uint16_t)(((a + 1) & 0xFF) | A << 8); }
                return 13;
            }
        case 3: {
            uint16_t v;
            if (p == 0) { v = BC; v = (uint16_t)(v + (q ? -1 : 1)); set16(c, 0, v); }
            else if (p == 1) { v = DE; v = (uint16_t)(v + (q ? -1 : 1)); set16(c, 2, v); }
            else if (p == 2) { v = getHLx(c, idx); v = (uint16_t)(v + (q ? -1 : 1)); setHLx(c, idx, v); }
            else c->sp = (uint16_t)(c->sp + (q ? -1 : 1));
            return 6 + ex;
        }
        case 4:
        case 5:
            if (y == 6) {
                if (idx) { d = (int8_t)fetch8(c); ea = (uint16_t)(getHLx(c, idx) + d); c->memptr = ea; } else ea = HL;
                uint8_t v = RD(ea);
                v = z == 4 ? inc8(c, v) : dec8(c, v);
                WR(ea, v);
                return idx ? 23 : 11;
            } else {
                uint8_t v = getr(c, idx, y);
                v = z == 4 ? inc8(c, v) : dec8(c, v);
                setr(c, idx, y, v);
                return 4 + ex;
            }
        case 6:
            if (y == 6) {
                if (idx) { d = (int8_t)fetch8(c); ea = (uint16_t)(getHLx(c, idx) + d); c->memptr = ea; } else ea = HL;
                uint8_t n = fetch8(c);
                WR(ea, n);
                return idx ? 19 : 10;
            } else {
                uint8_t n = fetch8(c);
                setr(c, idx, y, n);
                return 7 + ex;
            }
        default:
            switch (y) {
            case 0: { uint8_t cy = A >> 7; A = (uint8_t)(A << 1 | cy); SETF((c->f & (SF | ZF | PF)) | (A & (YF | XF)) | cy); return 4; }
            case 1: { uint8_t cy = A & 1; A = (uint8_t)(A >> 1 | cy << 7); SETF((c->f & (SF | ZF | PF)) | (A & (YF | XF)) | cy); return 4; }
            case 2: { uint8_t cy = A >> 7; A = (uint8_t)(A << 1 | (c->f & CF)); SETF((c->f & (SF | ZF | PF)) | (A & (YF | XF)) | cy); return 4; }
            case 3: { uint8_t cy = A & 1; A = (uint8_t)(A >> 1 | (c->f & CF) << 7); SETF((c->f & (SF | ZF | PF)) | (A & (YF | XF)) | cy); return 4; }
            case 4: daa(c); return 4;
            case 5: A = (uint8_t)~A; SETF((c->f & (SF | ZF | PF | CF)) | HF | NF | (A & (YF | XF))); return 4;
            case 6: SETF((c->f & (SF | ZF | PF)) | (((c->q ^ c->f) | A) & (YF | XF)) | CF); return 4;
            default: {
                uint8_t oldc = c->f & CF;
                SETF((c->f & (SF | ZF | PF)) | (oldc ? HF : 0) | (((c->q ^ c->f) | A) & (YF | XF)) | (oldc ? 0 : CF));
                return 4;
            }
            }
        }
    }

    if (x == 1) {
        if (op == 0x76) { c->halted = 1; return 4; }
        int memop = (y == 6 || z == 6);
        if (memop) {
            if (idx) { d = (int8_t)fetch8(c); ea = (uint16_t)(getHLx(c, idx) + d); c->memptr = ea; } else ea = HL;
            if (y == 6) WR(ea, c->r[z]);            /* the register operand is the real H / L here */
            else c->r[y] = RD(ea);
            return idx ? 19 : 7;
        }
        setr(c, idx, y, getr(c, idx, z));
        return 4 + ex;
    }

    if (x == 2) {
        uint8_t v;
        if (z == 6) {
            if (idx) { d = (int8_t)fetch8(c); ea = (uint16_t)(getHLx(c, idx) + d); c->memptr = ea; } else ea = HL;
            v = RD(ea);
            alu(c, y, v);
            return idx ? 19 : 7;
        }
        alu(c, y, getr(c, idx, z));
        return 4 + ex;
    }

    /* x == 3 */
    switch (z) {
    case 0:
        if (cond(c, y)) { c->pc = pop16(c); c->memptr = c->pc; return 11; }
        return 5;
    case 1:
        if (q == 0) {
            uint16_t v = pop16(c);
            if (p == 0) set16(c, 0, v);
            else if (p == 1) set16(c, 2, v);
            else if (p == 2) setHLx(c, idx, v);
            else { A = (uint8_t)(v >> 8); c->f = (uint8_t)v; c->fmod = 1; }
            return 10 + ex;
        }
        if (p == 0) { c->pc = pop16(c); c->memptr = c->pc; return 10; }
        if (p == 1) {
            for (int k = 0; k < 6; k++) { uint8_t t = c->r[k]; c->r[k] = c->r2[k]; c->r2[k] = t; }
            return 4;
        }
        if (p == 2) { c->pc = getHLx(c, idx); return 4 + ex; }
        c->sp = getHLx(c, idx);
        return 6 + ex;
    case 2: {
        uint16_t a = fetch16(c);
        c->memptr = a;
        if (cond(c, y)) c->pc = a;
        return 10;
    }
    case 3:
        switch (y) {
        case 0: c->pc = fetch16(c); c->memptr = c->pc; return 10;
        case 2: { uint8_t n = fetch8(c); c->out(c->u, (uint16_t)(A << 8 | n), A); c->memptr = (uint16_t)(((n + 1) & 0xFF) | A << 8); return 11; }
        case 3: { uint8_t n = fetch8(c); uint16_t port = (uint16_t)(A << 8 | n); c->memptr = (uint16_t)(port + 1); A = c->in(c->u, port); return 11; }
        case 4: {
            uint16_t v = (uint16_t)(RD(c->sp) | RD(c->sp + 1) << 8);
            uint16_t h = getHLx(c, idx);
            WR(c->sp, h & 0xFF);
            WR(c->sp + 1, h >> 8);
            setHLx(c, idx, v);
            c->memptr = v;
            return 19 + ex;
        }
        case 5: { uint16_t t = DE; set16(c, 2, HL); set16(c, 4, t); return 4; }
        case 6: c->iff1 = c->iff2 = 0; return 4;
        default: c->iff1 = c->iff2 = 1; c->ei_delay = 1; return 4;
        }
    case 4: {
        uint16_t a = fetch16(c);
        c->memptr = a;
        if (cond(c, y)) { push16(c, c->pc); c->pc = a; return 17; }
        return 10;
    }
    case 5:
        if (q == 0) {
            uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? getHLx(c, idx) : (uint16_t)(A << 8 | c->f);
            push16(c, v);
            return 11 + ex;
        } else {
            uint16_t a = fetch16(c);
            c->memptr = a;
            push16(c, c->pc);
            c->pc = a;
            return 17;
        }
    case 6: alu(c, y, fetch8(c)); return 7;
    default:
        push16(c, c->pc);
        c->pc = (uint16_t)(y * 8);
        c->memptr = c->pc;
        return 11;
    }
}

/* does the unprefixed opcode refer to HL, H, L or (HL)? (then DD / FD change its meaning; otherwise the prefix is ignored) */
static int uses_hl(uint8_t op)
{
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 0) {
        if (z == 1) return q == 1 || p == 2;
        if (z == 2 || z == 3) return p == 2;
        if (z >= 4 && z <= 6) return y >= 4 && y <= 6;
        return 0;
    }
    if (x == 1) return op != 0x76 && ((y >= 4 && y <= 6) || (z >= 4 && z <= 6));
    if (x == 2) return z >= 4 && z <= 6;
    if (z == 1) return (q == 0 && p == 2) || (q == 1 && p >= 2);
    if (z == 3) return y == 4;
    if (z == 5) return q == 0 && p == 2;
    return 0;
}

int z80_step(Z80 *c)
{
    int delay = c->ei_delay;
    c->ei_delay = 0;
    c->last_irq = 0;
    c->last_pc = c->pc;
    if (c->irq && c->iff1 && !delay) {
        c->halted = 0;
        c->iff1 = c->iff2 = 0;
        inc_r(c);
        c->last_irq = 1;
        if (c->im == 2) {
            push16(c, c->pc);
            uint16_t a = (uint16_t)(c->i << 8 | 0xFF);
            c->pc = (uint16_t)(RD(a) | RD(a + 1) << 8);
            c->memptr = c->pc;
            c->q = 0;
            return 19;
        }
        push16(c, c->pc);
        c->pc = 0x38;
        c->memptr = c->pc;
        c->q = 0;
        return c->im == 0 ? 13 : 13;
    }
    if (c->halted) { inc_r(c); c->q = 0; return 4; }
    c->fmod = 0;
    uint8_t op = fetch8(c);
    inc_r(c);
    int t;
    if (op == 0xDD || op == 0xFD) {
        int idx = op == 0xDD ? 1 : 2;
        uint8_t op2 = RD(c->pc);
        if (op2 != 0xCB && !uses_hl(op2)) {
            c->q = 0;
            return 4;   /* the prefix is ignored (also before DD / FD / ED) and the next byte starts a new instruction */
        }
        c->pc++;
        inc_r(c);
        if (op2 == 0xCB) {
            int8_t d = (int8_t)fetch8(c);
            uint8_t op3 = fetch8(c);
            uint16_t ea = (uint16_t)(getHLx(c, idx) + d);
            c->memptr = ea;
            t = exec_cb(c, idx, ea, op3);
        } else {
            t = exec_main(c, op2, idx);   /* returns the total including the prefix */
        }
        c->q = c->fmod ? c->f : 0;
        return t;
    }
    if (op == 0xCB) {
        uint8_t op2 = fetch8(c);
        inc_r(c);
        t = exec_cb(c, 0, 0, op2);
    } else if (op == 0xED) {
        t = exec_ed(c);
    } else {
        t = exec_main(c, op, 0);
    }
    c->q = c->fmod ? c->f : 0;
    return t;
}
