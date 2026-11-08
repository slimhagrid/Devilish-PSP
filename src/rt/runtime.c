/* Runtime of the translated game: machine state, scanline time, interrupt delivery, dispatch between the translated modules, loud misses. */
#include "z80rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* translated modules (extracted/gen/mod_*.c) and the facts the translator knew about them (extracted/gen/gen_info.c) */
void run_lo(void);
void run_hi(void);
void run_b2(void);
extern const int gen_slot2_code_bank;          /* the only ROM bank in slot 2 that has translated code */
void run_ram(void);                            /* the sound driver the game unpacks into RAM at boot ($D500-$DE66), translated from the oracle's RAM image */
extern const uint32_t gen_ram_crc;             /* CRC32 of the RAM program's code bytes (patch cells zeroed) at the time of translation */
extern const uint16_t gen_ram_spans[][2];      /* {start, length} of every executed code byte range of the RAM program */
extern const int gen_ram_nspans;
extern const uint16_t gen_ram_patch[];         /* addresses of the self-modifying cells: not part of the CRC */
extern const int gen_ram_npatch;
static uint64_t ram_checked_frame = (uint64_t)-1;

Z80M M;
HW G;
#ifdef Z80TRACE
/* instruction trace (build with -DZ80TRACE; the generated code calls rt_trace(pc) at the start of every instruction): same line format as the oracle's --trace */
FILE *rt_trace_f;
uint64_t rt_trace_from, rt_trace_to;
uint16_t rt_last_pc;
void rt_trace(uint16_t pc)
{
    rt_last_pc = pc;
    uint64_t cyc = G.cycles + (uint64_t)M.acc;
    if (rt_trace_f && cyc >= rt_trace_from && cyc < rt_trace_to)
        fprintf(rt_trace_f, "%llu %04x af=%02x%02x bc=%02x%02x de=%02x%02x hl=%02x%02x sp=%04x ix=%04x iy=%04x\n", (unsigned long long)cyc, pc, M.a, M.f, M.b, M.c, M.d, M.e, M.h, M.l,
                M.sp, M.ix, M.iy);
}
#endif
#ifdef Z80COV
uint8_t rt_cov[0x2000];
#endif
#ifdef ROM_POISON
const uint8_t *rt_poison;
void rt_poison_hit(uint16_t a, size_t off)
{
    static uint8_t seen[131072];
    static int n;
    extern uint16_t rt_last_pc;
    if (seen[off]) return;
    seen[off] = 1;
    if (n++ < 200) fprintf(stderr, "poison: ROM offset $%05zx (CPU $%04x) read as data by the instruction at $%04x, frame %llu\n", off, a, rt_last_pc, (unsigned long long)G.frame);
}
#endif
void (*rt_frame_cb)(uint64_t frame);
static int stopped;

/* weak: the PSP front end shows the message on the screen instead */
__attribute__((weak)) void rt_fatal(const char *msg)
{
    fprintf(stderr, "FATAL: %s\n", msg);
    fflush(stderr);
    exit(3);
}

void rt_miss(uint16_t pc, const char *why)
{
    char buf[256];
    snprintf(buf, sizeof buf, "MISS at pc=$%04x (slot banks %d/%d/%d, sp=$%04x, frame %llu): %s", pc, G.bank[0], G.bank[1], G.bank[2], M.sp,
             (unsigned long long)G.frame, why);
    rt_fatal(buf);
}

static void on_mapper(HW *hw, int reg, uint8_t val, void *user)
{
    (void)hw; (void)user;
    /* the translated code assumes slots 0 and 1 hold banks 0 and 1 for ever (the game writes them once, with exactly these values) */
    if ((reg == 1 && val != 0) || (reg == 2 && val != 1) || (reg == 0 && val != 0)) rt_fatal("the game changed a mapper register the translation assumes constant ($FFFC-$FFFE)");
}

void rt_sync(void)
{
    if (M.acc) {
        hw_advance(&G, M.acc);
        M.acc = 0;
        M.ev_left = HW_CYC_LINE - G.line_cycle;
    }
}

static uint32_t crc32_(uint32_t c, const uint8_t *p, size_t n)
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* the RAM program may only differ from the image the translator saw in its patch cells */
static void ram_verify(void)
{
    uint8_t img[0x2000];
    memcpy(img, G.ram, sizeof img);
    for (int i = 0; i < gen_ram_npatch; i++) img[gen_ram_patch[i] & 0x1FFF] = 0;
    uint32_t crc = 0;
    for (int i = 0; i < gen_ram_nspans; i++) crc = crc32_(crc, img + (gen_ram_spans[i][0] & 0x1FFF), gen_ram_spans[i][1]);
    if (crc != gen_ram_crc) {
        char buf[160];
        snprintf(buf, sizeof buf, "the RAM program ($D500-) differs from the translated image (crc %08x, expected %08x) at frame %llu", (unsigned)crc, (unsigned)gen_ram_crc, (unsigned long long)G.frame);
        rt_fatal(buf);
    }
}

void rt_init(const uint8_t *rom, uint32_t size)
{
    hw_init(&G, rom, size);
    G.on_mapper = on_mapper;
    memset(&M, 0, sizeof M);
    /* Z80 power-on state as the oracle's core has it */
    M.a = 0xFF; M.f = 0xFF; M.a2 = 0; M.f2 = 0xFF;
    M.ix = M.iy = 0xFFFF; M.sp = 0xFFFF; M.pc = 0;
    M.ev_left = HW_CYC_LINE;
    stopped = 0;
    ram_checked_frame = (uint64_t)-1;
}

void rt_stop(void) { stopped = 1; }

static void service_irq(void)
{
    M.halted = 0;
    M.iff1 = M.iff2 = 0;
    RINC(1);
    PUSH16(M.pc);
    if (M.im == 2) {
        uint16_t a = (uint16_t)(M.i << 8 | 0xFF);
        M.pc = RD16(a);
        ACC(19);
    } else {
        M.pc = 0x38;
        ACC(13);
    }
    if (M.acc >= M.ev_left) rt_sync();
}

/* Save states (src/rt/state.c) can be taken and loaded only where the program is at a plain instruction boundary: inside the frame callback called from the first site below. rt_state_safe() says so; the callback asks. */
static int cb_site;
int rt_state_safe(void) { return cb_site == 1; }

/* Backing out of the menus (not in the original game, which has no way back once NORMAL GAME / TIME TRIAL is chosen). The menu flow is in the game's main loop ($12CE-$13DB): it calls
 * SELECT GAMES ($237B, returns to $12D4), then SELECT STAGE ($217B, returns to $12DE; also called from $13D8, returns to $13DB) for TIME TRIAL, then OPTIONS ($256C, returns to $12F0)
 * for NORMAL GAME. The main loop runs with SP = $DFEF, so while one of those calls is active its return address is the word at $DFED. A back request (PAD_BACK pressed, edge) while
 * the screen is OPTIONS or SELECT STAGE restarts the main loop at $12CE (SELECT GAMES), from SELECT GAMES at $12A6 (the title); the Z80 stack and the interrupt state are reset like at
 * those points of the original flow, everything else (RAM, VDP, sound) is left as it is, as each screen sets up its own. Nothing happens anywhere else (in play, on the title). */
static int back_pending, back_hold;
static uint8_t back_prev;

static int menu_back_now(void)
{
    if (M.sp > 0xDFEDu) return 0;                                 /* not inside a call made by the main loop */
    uint16_t ret = (uint16_t)(G.ram[0x1FED] | G.ram[0x1FEE] << 8);
    uint16_t to;
    switch (ret) {
    case 0x12F0: case 0x12DE: case 0x13DB: to = 0x12CE; break;    /* OPTIONS, SELECT STAGE -> SELECT GAMES */
    case 0x12D4: to = 0x12A6; break;                              /* SELECT GAMES -> title */
    default: return 0;
    }
    M.sp = 0xDFEF;
    M.pc = to;
    M.iff1 = M.iff2 = 1;
    M.ei_delay = 0;
    M.halted = 0;
    return 1;
}

void rt_back_filter(uint8_t *pad)
{
    uint8_t p = *pad, edge = (uint8_t)(p & ~back_prev);
    back_prev = p;
    if (edge & PAD_BACK) back_pending = 4;                          /* a press counts for 4 frames (a frame callback inside the interrupt acknowledge is not a safe place), not for as long as the key is held:
                                                                     * Circle is also the game's button 2, held a lot in play, and must not back out of a menu that appears later */
    else if (back_pending) back_pending--;
    if (!(p & PAD_BACK)) back_hold = 0;
    if (back_pending && rt_state_safe() && menu_back_now()) { back_pending = 0; back_hold = 1; }
    if (back_hold) *pad = (uint8_t)(p & ~(PAD_1 | PAD_2));         /* the key that backed out is not a ball launch in the next screen */
}

/* an instruction boundary where the hardware has an event due or an interrupt may be taken */
void rt_slow(void)
{
    if (M.acc >= M.ev_left) {
        rt_sync();
        M.irq_line = (uint8_t)hw_irq(&G);
        if (G.frame_done) {
            G.frame_done = 0;
            cb_site = 1;
            if (rt_frame_cb) rt_frame_cb(G.frame - 1);
            cb_site = 0;
        }
    }
    int delay = M.ei_delay;
    M.ei_delay = 0;
    if (M.irq_line && M.iff1 && !delay) {
        service_irq();
        M.irq_line = (uint8_t)hw_irq(&G);
        if (G.frame_done) {                       /* the acknowledge cycles can end a frame */
            G.frame_done = 0;
            cb_site = 2;
            if (rt_frame_cb) rt_frame_cb(G.frame - 1);
            cb_site = 0;
        }
    }
    IRQ_REFRESH();
}

/* HALT: 4 T-states per cycle until an interrupt can be taken (the game does not use HALT; kept for completeness and the conformance tests) */
void rt_halt(uint16_t next)
{
    M.pc = next;
    for (;;) {
        ACC(4);
        RINC(1);
        if (M.acc >= M.ev_left) {
            rt_sync();
            M.irq_line = (uint8_t)hw_irq(&G);
            if (G.frame_done) { G.frame_done = 0; if (rt_frame_cb) rt_frame_cb(G.frame - 1); }
        }
        if (M.irq_line && M.iff1) return;
        if (stopped) return;
    }
}

/* ---- the sound driver ---- *
 * The game unpacks a Z80 sound driver into RAM and calls it once per frame. It is translated like the rest of the game (module ram) and runs on the same timeline: its port writes
 * reach the hardware at the T-states the Z80 code would have used. */
int64_t rt_force_iff_frame = -1;

void rt_run(void)
{
    while (!stopped) {
        if (CHK()) { rt_slow(); if (stopped) break; }
        uint16_t p = M.pc;
        if (p < 0x4000) run_lo();
        else if (p < 0x8000) run_hi();
        else if (p < 0xC000) {
            if (G.bank[2] % G.nbanks != (uint32_t)gen_slot2_code_bank) rt_miss(p, "code executed in slot 2 with a bank that has no translation");
            run_b2();
        } else {
            if (G.frame == (uint64_t)rt_force_iff_frame && p == 0xD500) M.iff1 = M.iff2 = 1;     /* test hook (--force-iff FRAME): run this frame's driver call with interrupts enabled */
            if (ram_checked_frame != G.frame) { ram_verify(); ram_checked_frame = G.frame; }
            run_ram();
        }
    }
}
