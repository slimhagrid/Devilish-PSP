/* Dev-time Z80 reference core (M2 oracle). Host only, never part of the PSP build.
 *
 * Complete instruction set including the undocumented parts (X/Y flags, IXH/IXL, SLL, DDCB register copies, MEMPTR, the SCF/CCF Q quirk); T-states per
 * instruction (no per-cycle bus timing). Validated against zexdoc / zexall (tools/oracle/zex.c).
 */
#ifndef Z80_H
#define Z80_H
#include <stdint.h>

typedef struct Z80 Z80;
struct Z80 {
    uint8_t (*rd)(void *u, uint16_t a);
    void (*wr)(void *u, uint16_t a, uint8_t v);
    uint8_t (*in)(void *u, uint16_t port);
    void (*out)(void *u, uint16_t port, uint8_t v);
    void *u;

    uint8_t r[8];           /* B C D E H L (unused) A */
    uint8_t f;
    uint8_t r2[8], f2;      /* shadow set */
    uint16_t ix, iy, sp, pc, memptr;
    uint8_t i, rr;          /* I and R (bit 7 of R is only changed by LD R,A) */
    uint8_t iff1, iff2, im;
    uint8_t halted;
    uint8_t ei_delay;       /* an EI was just executed: no interrupt before the next instruction */
    uint8_t irq;            /* level of the INT line, set by the hardware model */
    uint8_t q, fmod;        /* the "Q" factor of SCF / CCF: F as written by the previous instruction (0 if it did not write F) */
    uint16_t last_pc;       /* address of the instruction just executed (trace) */
    int last_irq;           /* the last step serviced an interrupt */
};

void z80_reset(Z80 *c);
/* execute one instruction (or one interrupt acknowledge, or one HALT cycle); returns the T-states used */
int z80_step(Z80 *c);

#define Z80_B(c) ((c)->r[0])
#define Z80_A(c) ((c)->r[7])

#endif
