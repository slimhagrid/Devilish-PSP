/* Runtime for the translated game (tools/z80_to_c.py writes the C files in extracted/gen/ against this header).
 *
 * Machine state is a global struct (`M`); the hardware model is the shared src/hw/hw.c (`G`). The translated code is a "switch machine": one case + label per
 * original instruction, calls and returns go through the emulated stack in RAM, so every `push de; ret` / `jp (hl)` idiom of the game works unchanged.
 * There is no CPU interpreter here: only the meaning of each instruction, expanded inline.
 *
 * Time model (identical to the oracle, so that frames / port logs match bit for bit): an instruction executes first (port accesses see the hardware as of
 * the start of the instruction), then its T-states are added to `M.acc`; when `M.acc` reaches `M.ev_left` (the end of the scanline) or an interrupt may be
 * taken, control returns to rt_slow(), which advances the hardware, delivers the frame callback and takes the interrupt.
 *
 * The same macros are compiled with -DZ80RT_CONF by the conformance harness (flat 64 KB memory, logged ports, one instruction per call).
 */
#ifndef Z80RT_H
#define Z80RT_H
#include <stdint.h>
#include "../hw/hw.h"

#define SF 0x80
#define ZF 0x40
#define YF 0x20
#define HF 0x10
#define XF 0x08
#define PF 0x04
#define NF 0x02
#define CF 0x01

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a2, f2, b2, c2, d2, e2, h2, l2;     /* shadow registers */
    uint16_t ix, iy, sp, pc;
    uint8_t i, iff1, iff2, im, ei_delay, irq_line, irq_ready, halted;
    uint8_t rbase;                              /* R = (rbase & 0x80) | ((rbase + rcnt) & 0x7F) */
    uint32_t rcnt;                              /* M1 cycles since rbase was set */
    int32_t acc;                                /* T-states executed but not yet given to the hardware */
    int32_t ev_left;                            /* T-states until the next hardware event (end of the scanline) */
    uint16_t cur;                               /* address of the instruction doing port I/O (for the port log) */
} Z80M;

extern Z80M M;
extern HW G;

/* ---- runtime entry points (runtime.c) -------------------------------------------------------------------------------------------------------- */
void rt_init(const uint8_t *rom, uint32_t size);
void rt_run(void);                       /* runs forever (until rt_stop() from a callback) */
void rt_stop(void);                      /* leave rt_run() at the next boundary */
void rt_slow(void);                      /* hardware events / interrupt at an instruction boundary */
void rt_sync(void);                      /* give the accumulated T-states to the hardware */
void rt_miss(uint16_t pc, const char *why) __attribute__((noreturn));
void rt_fatal(const char *msg) __attribute__((noreturn));
void rt_halt(uint16_t next);
extern void (*rt_frame_cb)(uint64_t frame);   /* called when a frame is complete (set by the host) */
void rt_back_filter(uint8_t *pad);       /* hosts call it at the end of rt_frame_cb with the pad of the next frame: PAD_BACK jumps the game back one menu screen (runtime.c) */
int rt_state_safe(void);                 /* inside rt_frame_cb: the program is at a plain instruction boundary, a save state can be taken or loaded now (state.c) */

#define CHK() (M.acc >= M.ev_left || M.irq_ready)
#define ACC(t) (M.acc += (t))
#define IRQ_REFRESH() (M.irq_ready = (uint8_t)(M.ei_delay | (M.irq_line & M.iff1)))
#define RINC(k) (M.rcnt += (k))
#define RGET() ((uint8_t)((M.rbase & 0x80) | ((M.rbase + M.rcnt) & 0x7F)))
#define RSET(v) (M.rbase = (uint8_t)(v), M.rcnt = 0)

/* ---- registers ------------------------------------------------------------------------------------------------------------------------------- */
#define A M.a
#define F M.f
#define B M.b
#define C M.c
#define D M.d
#define E M.e
#define H M.h
#define L M.l
#define BCv ((uint16_t)(M.b << 8 | M.c))
#define DEv ((uint16_t)(M.d << 8 | M.e))
#define HLv ((uint16_t)(M.h << 8 | M.l))
#define AFv ((uint16_t)(M.a << 8 | M.f))
#define SET_BC(v) do { uint16_t mt_ = (uint16_t)(v); M.b = (uint8_t)(mt_ >> 8); M.c = (uint8_t)mt_; } while (0)
#define SET_DE(v) do { uint16_t mt_ = (uint16_t)(v); M.d = (uint8_t)(mt_ >> 8); M.e = (uint8_t)mt_; } while (0)
#define SET_HL(v) do { uint16_t mt_ = (uint16_t)(v); M.h = (uint8_t)(mt_ >> 8); M.l = (uint8_t)mt_; } while (0)
#define SET_AF(v) do { uint16_t mt_ = (uint16_t)(v); M.a = (uint8_t)(mt_ >> 8); M.f = (uint8_t)mt_; } while (0)
#define IXH ((uint8_t)(M.ix >> 8))
#define IXL ((uint8_t)M.ix)
#define IYH ((uint8_t)(M.iy >> 8))
#define IYL ((uint8_t)M.iy)
#define SET_IXH(v) (M.ix = (uint16_t)((M.ix & 0x00FF) | (uint16_t)(v) << 8))
#define SET_IXL(v) (M.ix = (uint16_t)((M.ix & 0xFF00) | (uint8_t)(v)))
#define SET_IYH(v) (M.iy = (uint16_t)((M.iy & 0x00FF) | (uint16_t)(v) << 8))
#define SET_IYL(v) (M.iy = (uint16_t)((M.iy & 0xFF00) | (uint8_t)(v)))
#define EX_AF() do { uint8_t mt_; mt_ = M.a; M.a = M.a2; M.a2 = mt_; mt_ = M.f; M.f = M.f2; M.f2 = mt_; } while (0)
#define EXX() do { uint8_t mt_; mt_ = M.b; M.b = M.b2; M.b2 = mt_; mt_ = M.c; M.c = M.c2; M.c2 = mt_; mt_ = M.d; M.d = M.d2; M.d2 = mt_;                    mt_ = M.e; M.e = M.e2; M.e2 = mt_; mt_ = M.h; M.h = M.h2; M.h2 = mt_; mt_ = M.l; M.l = M.l2; M.l2 = mt_; } while (0)
#define EX_DE_HL() do { uint16_t mx_ = DEv; SET_DE(HLv); SET_HL(mx_); } while (0)

/* ---- memory ---------------------------------------------------------------------------------------------------------------------------------- */
#ifdef Z80RT_CONF
extern uint8_t conf_mem[65536];
void conf_port_out(uint16_t port, uint8_t v);
uint8_t conf_port_in(uint16_t port);
static inline uint8_t mem_rd(uint16_t a) { return conf_mem[a]; }
void conf_wlog(uint16_t a, uint8_t v);
static inline void mem_wr(uint16_t a, uint8_t v) { conf_wlog(a, v); conf_mem[a] = v; }
#define RDK(a) mem_rd(a)
#define WRK(a, v) mem_wr((a), (v))
#define RDK_RAM(a) mem_rd(a)
#define RAMB(a) mem_rd(a)
#elif defined(ROM_POISON)
/* diagnostic build (tools/native_build.sh POISON=1, native --poison FILE): a read of a ROM byte that the translated instructions occupy (FILE: one byte per ROM offset, 1 = such a byte) is
 * reported with the instruction that read it (needs TRACE=1 as well for the pc). Used to prove that the data image of M9 (the ROM without the translated code) is enough. */
extern const uint8_t *rt_poison;
extern uint16_t rt_last_pc;
void rt_poison_hit(uint16_t a, size_t off);
static inline uint8_t mem_rd(uint16_t a)
{
    const uint8_t *p;
    if (a < 0x400) p = G.rom + a;
    else if (a < 0x4000) p = G.page[0] + a;
    else if (a < 0x8000) p = G.page[1] + (a - 0x4000);
    else if (a < 0xC000) p = G.page[2] + (a - 0x8000);
    else return G.ram[a & 0x1FFF];
    if (rt_poison && rt_poison[(size_t)(p - G.rom)]) rt_poison_hit(a, (size_t)(p - G.rom));
    return *p;
}
static inline void mem_wr(uint16_t a, uint8_t v)
{
    if (a < 0xC000) return;
    G.ram[a & 0x1FFF] = v;
    if (a >= 0xFFFC) hw_wr(&G, a, v);
}
#define RDK(a) mem_rd(a)
#define WRK(a, v) mem_wr((a), (v))
#define RDK_RAM(a) (G.ram[(a) & 0x1FFF])
#define RAMB(a) (G.ram[(a) & 0x1FFF])
#else
static inline uint8_t mem_rd(uint16_t a)
{
    if (a < 0x400) return G.rom[a];
    if (a < 0x4000) return G.page[0][a];
    if (a < 0x8000) return G.page[1][a - 0x4000];
    if (a < 0xC000) return G.page[2][a - 0x8000];
    return G.ram[a & 0x1FFF];
}
static inline void mem_wr(uint16_t a, uint8_t v)
{
    if (a < 0xC000) return;
    G.ram[a & 0x1FFF] = v;
    if (a >= 0xFFFC) hw_wr(&G, a, v);        /* mapper registers: keep the model's banks in step (it re-stores the byte in RAM) */
}
#define RDK(a) ((a) < 0x8000 ? G.rom[(a)] : (a) < 0xC000 ? G.page[2][(a) - 0x8000] : G.ram[(a) & 0x1FFF])   /* constant address */
#define WRK(a, v) mem_wr((a), (v))
#define RDK_RAM(a) (G.ram[(a) & 0x1FFF])
#define RAMB(a) (G.ram[(a) & 0x1FFF])        /* byte of RAM at a constant address (patch cells of the RAM module) */
#endif
#define RD8(a) mem_rd((uint16_t)(a))
#define WR8(a, v) mem_wr((uint16_t)(a), (uint8_t)(v))
#define RD16(a) ((uint16_t)(mem_rd((uint16_t)(a)) | mem_rd((uint16_t)((a) + 1)) << 8))
#define WR16(a, v) do { uint16_t ma_ = (uint16_t)(a), mv_ = (uint16_t)(v); mem_wr(ma_, (uint8_t)mv_); mem_wr((uint16_t)(ma_ + 1), (uint8_t)(mv_ >> 8)); } while (0)
#define PUSH16(v) do { uint16_t mv_ = (uint16_t)(v); M.sp--; mem_wr(M.sp, (uint8_t)(mv_ >> 8)); M.sp--; mem_wr(M.sp, (uint8_t)mv_); } while (0)
static inline uint16_t pop16_(void) { uint8_t lo = mem_rd(M.sp); M.sp++; uint8_t hi = mem_rd(M.sp); M.sp++; return (uint16_t)(lo | hi << 8); }
#define POP16() pop16_()

/* ---- ports ----------------------------------------------------------------------------------------------------------------------------------- */
#ifdef Z80RT_CONF
static inline uint8_t z_in(uint16_t port) { return conf_port_in(port); }
static inline void z_out(uint16_t port, uint8_t v) { conf_port_out(port, v); }
#else
static inline uint8_t z_in(uint16_t port)
{
    rt_sync();
    uint8_t v = hw_in(&G, port);
    M.irq_line = (uint8_t)hw_irq(&G);
    IRQ_REFRESH();
    return v;
}
static inline void z_out(uint16_t port, uint8_t v)
{
    rt_sync();
    hw_out(&G, port, v);
    M.irq_line = (uint8_t)hw_irq(&G);
    IRQ_REFRESH();
}
#endif

/* ---- ALU helpers (same formulas as the oracle's core, which zexall validates) ----------------------------------------------------------------- */
static inline uint8_t z_add8(uint8_t a, uint8_t b, unsigned cy)
{
    unsigned r = (unsigned)a + b + cy;
    uint8_t v = (uint8_t)r;
    M.f = (uint8_t)((v & (SF | YF | XF)) | (v ? 0 : ZF) | ((a ^ b ^ v) & HF) | ((~(a ^ b) & (a ^ v) & 0x80) ? PF : 0) | (r > 0xFF ? CF : 0));
    return v;
}
static inline uint8_t z_sub8(uint8_t a, uint8_t b, unsigned cy)
{
    int r = (int)a - b - (int)cy;
    uint8_t v = (uint8_t)r;
    M.f = (uint8_t)((v & (SF | YF | XF)) | (v ? 0 : ZF) | ((a ^ b ^ v) & HF) | (((a ^ b) & (a ^ v) & 0x80) ? PF : 0) | NF | (r & 0x100 ? CF : 0));
    return v;
}
static inline void z_cp8(uint8_t a, uint8_t b)
{
    (void)z_sub8(a, b, 0);
    M.f = (uint8_t)((M.f & ~(YF | XF)) | (b & (YF | XF)));
}
static inline uint8_t z_par(uint8_t v) { return __builtin_parity(v) ? 0 : PF; }
static inline uint8_t z_and8(uint8_t a, uint8_t b) { uint8_t v = a & b; M.f = (uint8_t)((v & (SF | YF | XF)) | (v ? 0 : ZF) | HF | z_par(v)); return v; }
static inline uint8_t z_xor8(uint8_t a, uint8_t b) { uint8_t v = a ^ b; M.f = (uint8_t)((v & (SF | YF | XF)) | (v ? 0 : ZF) | z_par(v)); return v; }
static inline uint8_t z_or8(uint8_t a, uint8_t b) { uint8_t v = a | b; M.f = (uint8_t)((v & (SF | YF | XF)) | (v ? 0 : ZF) | z_par(v)); return v; }
static inline uint8_t z_inc8(uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    M.f = (uint8_t)((M.f & CF) | (r & (SF | YF | XF)) | (r ? 0 : ZF) | ((r & 0x0F) == 0 ? HF : 0) | (r == 0x80 ? PF : 0));
    return r;
}
static inline uint8_t z_dec8(uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    M.f = (uint8_t)((M.f & CF) | (r & (SF | YF | XF)) | (r ? 0 : ZF) | ((r & 0x0F) == 0x0F ? HF : 0) | (r == 0x7F ? PF : 0) | NF);
    return r;
}
static inline uint16_t z_add16(uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b;
    M.f = (uint8_t)((M.f & (SF | ZF | PF)) | ((r >> 8) & (YF | XF)) | (((a ^ b ^ r) >> 8) & HF) | (r > 0xFFFF ? CF : 0));
    return (uint16_t)r;
}
static inline uint16_t z_adc16(uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b + (M.f & CF);
    uint16_t v = (uint16_t)r;
    M.f = (uint8_t)(((v >> 8) & (SF | YF | XF)) | (v ? 0 : ZF) | (((a ^ b ^ v) >> 8) & HF) | ((~(a ^ b) & (a ^ v) & 0x8000) ? PF : 0) | (r > 0xFFFF ? CF : 0));
    return v;
}
static inline uint16_t z_sbc16(uint16_t a, uint16_t b)
{
    int32_t r = (int32_t)a - b - (M.f & CF);
    uint16_t v = (uint16_t)r;
    M.f = (uint8_t)(((v >> 8) & (SF | YF | XF)) | (v ? 0 : ZF) | (((a ^ b ^ v) >> 8) & HF) | (((a ^ b) & (a ^ v) & 0x8000) ? PF : 0) | NF | (r & 0x10000 ? CF : 0));
    return v;
}
static inline void z_daa(void)
{
    uint8_t a = M.a, corr = 0, cf = M.f & CF;
    if ((M.f & HF) || (a & 0x0F) > 9) corr |= 0x06;
    if (cf || a > 0x99) { corr |= 0x60; cf = CF; }
    uint8_t h;
    if (M.f & NF) { h = ((M.f & HF) && (a & 0x0F) < 6) ? HF : 0; a = (uint8_t)(a - corr); }
    else { h = (a & 0x0F) > 9 ? HF : 0; a = (uint8_t)(a + corr); }
    M.a = a;
    M.f = (uint8_t)((a & (SF | YF | XF)) | (a ? 0 : ZF) | h | z_par(a) | (M.f & NF) | cf);
}
static inline uint8_t z_rot(int op, uint8_t v)
{
    uint8_t cy, r;
    switch (op) {
    case 0: cy = v >> 7; r = (uint8_t)(v << 1 | cy); break;
    case 1: cy = v & 1; r = (uint8_t)(v >> 1 | cy << 7); break;
    case 2: cy = v >> 7; r = (uint8_t)(v << 1 | (M.f & CF)); break;
    case 3: cy = v & 1; r = (uint8_t)(v >> 1 | (M.f & CF) << 7); break;
    case 4: cy = v >> 7; r = (uint8_t)(v << 1); break;
    case 5: cy = v & 1; r = (uint8_t)(v >> 1 | (v & 0x80)); break;
    case 6: cy = v >> 7; r = (uint8_t)(v << 1 | 1); break;
    default: cy = v & 1; r = (uint8_t)(v >> 1); break;
    }
    M.f = (uint8_t)((r & (SF | YF | XF)) | (r ? 0 : ZF) | z_par(r) | cy);
    return r;
}
static inline void z_bit(int n, uint8_t v, uint8_t xy)
{
    uint8_t m = (uint8_t)(v & (1 << n));
    M.f = (uint8_t)((M.f & CF) | HF | (m ? 0 : (ZF | PF)) | (n == 7 && m ? SF : 0) | (xy & (YF | XF)));
}
/* rlca / rrca / rla / rra: S Z P kept, H N cleared, X Y from the result */
static inline void z_rlca(void) { uint8_t cy = M.a >> 7; M.a = (uint8_t)(M.a << 1 | cy); M.f = (uint8_t)((M.f & (SF | ZF | PF)) | (M.a & (YF | XF)) | cy); }
static inline void z_rrca(void) { uint8_t cy = M.a & 1; M.a = (uint8_t)(M.a >> 1 | cy << 7); M.f = (uint8_t)((M.f & (SF | ZF | PF)) | (M.a & (YF | XF)) | cy); }
static inline void z_rla(void) { uint8_t cy = M.a >> 7; M.a = (uint8_t)(M.a << 1 | (M.f & CF)); M.f = (uint8_t)((M.f & (SF | ZF | PF)) | (M.a & (YF | XF)) | cy); }
static inline void z_rra(void) { uint8_t cy = M.a & 1; M.a = (uint8_t)(M.a >> 1 | (M.f & CF) << 7); M.f = (uint8_t)((M.f & (SF | ZF | PF)) | (M.a & (YF | XF)) | cy); }
static inline void z_cpl(void) { M.a = (uint8_t)~M.a; M.f = (uint8_t)((M.f & (SF | ZF | PF | CF)) | HF | NF | (M.a & (YF | XF))); }
/* scf / ccf use the Q factor of the real chip; only the case "previous instruction did not write F" (Q = 0) is modelled here: X/Y = (F | A) & 0x28 */
static inline void z_scf(void) { M.f = (uint8_t)((M.f & (SF | ZF | PF)) | ((M.f | M.a) & (YF | XF)) | CF); }
static inline void z_ccf(void) { uint8_t oc = M.f & CF; M.f = (uint8_t)((M.f & (SF | ZF | PF)) | (oc ? HF : 0) | ((M.f | M.a) & (YF | XF)) | (oc ? 0 : CF)); }
static inline void z_ld_a_ir(uint8_t v) { M.a = v; M.f = (uint8_t)((M.f & CF) | (v & (SF | YF | XF)) | (v ? 0 : ZF) | (M.iff2 ? PF : 0)); }
static inline uint8_t z_rrd(uint8_t m) { uint8_t r = (uint8_t)((M.a << 4) | (m >> 4)); M.a = (uint8_t)((M.a & 0xF0) | (m & 0x0F)); M.f = (uint8_t)((M.f & CF) | (M.a & (SF | YF | XF)) | (M.a ? 0 : ZF) | z_par(M.a)); return r; }
static inline uint8_t z_rld(uint8_t m) { uint8_t r = (uint8_t)((m << 4) | (M.a & 0x0F)); M.a = (uint8_t)((M.a & 0xF0) | (m >> 4)); M.f = (uint8_t)((M.f & CF) | (M.a & (SF | YF | XF)) | (M.a ? 0 : ZF) | z_par(M.a)); return r; }
static inline void z_in_flags(uint8_t v) { M.f = (uint8_t)((M.f & CF) | (v & (SF | YF | XF)) | (v ? 0 : ZF) | z_par(v)); }

/* block instructions: one iteration; the return value is 1 when the instruction repeats (the caller adds the 21-T path and loops) */
static inline int z_ldx(int dir, int rep, uint16_t pcstart)
{
    uint8_t v = mem_rd(HLv);
    mem_wr(DEv, v);
    SET_HL(HLv + dir); SET_DE(DEv + dir); SET_BC(BCv - 1);
    uint8_t n = (uint8_t)(v + M.a);
    M.f = (uint8_t)((M.f & (SF | ZF | CF)) | ((n & 2) << 4) | (n & 8) | (BCv ? PF : 0));
    if (rep && BCv) { M.f = (uint8_t)((M.f & ~(YF | XF)) | ((pcstart >> 8) & (YF | XF))); return 1; }
    return 0;
}
static inline int z_cpx(int dir, int rep, uint16_t pcstart)
{
    uint8_t v = mem_rd(HLv);
    uint8_t keep = M.f;
    uint8_t res = z_sub8(M.a, v, 0);
    int h = (M.f & HF) != 0;
    (void)keep;
    SET_HL(HLv + dir); SET_BC(BCv - 1);
    uint8_t n = (uint8_t)(res - (h ? 1 : 0));
    M.f = (uint8_t)((keep & CF) | NF | (res & SF) | (res ? 0 : ZF) | (h ? HF : 0) | (n & 8) | ((n & 2) << 4) | (BCv ? PF : 0));
    if (rep && BCv && res) { M.f = (uint8_t)((M.f & ~(YF | XF)) | ((pcstart >> 8) & (YF | XF))); return 1; }
    return 0;
}
static inline int z_inx(int dir, int rep)
{
    uint8_t v = z_in(BCv);
    mem_wr(HLv, v);
    M.b--;
    SET_HL(HLv + dir);
    int k = v + ((M.c + dir) & 0xFF);
    M.f = (uint8_t)((M.b & (SF | YF | XF)) | (M.b ? 0 : ZF) | (k > 255 ? (HF | CF) : 0) | z_par((uint8_t)((k & 7) ^ M.b)) | (v & 0x80 ? NF : 0));
    return rep && M.b;
}
static inline int z_outx(int dir, int rep)
{
    uint8_t v = mem_rd(HLv);
    M.b--;
    z_out(BCv, v);
    SET_HL(HLv + dir);
    int k = v + M.l;
    M.f = (uint8_t)((M.b & (SF | YF | XF)) | (M.b ? 0 : ZF) | (k > 255 ? (HF | CF) : 0) | z_par((uint8_t)((k & 7) ^ M.b)) | (v & 0x80 ? NF : 0));
    return rep && M.b;
}

#endif
