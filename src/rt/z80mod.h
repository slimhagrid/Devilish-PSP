/* Control-flow macros of the translated modules (extracted/gen/mod_*.c). One instruction ends in exactly one of these; each adds the instruction's T-states and
 * checks whether the hardware / an interrupt needs attention (CHK) before control moves on. `return` leaves the module function: rt_run() then runs rt_slow()
 * if needed and picks the module that owns M.pc. L is the label of a static target inside the same module.
 */
#ifndef Z80MOD_H
#define Z80MOD_H
#include "z80rt.h"

#ifdef Z80TRACE
void rt_trace(uint16_t pc);
#define TR(a) rt_trace(a)
#elif defined(Z80COV)       /* coverage build (tools/native_build.sh COV=1): one bit per executed instruction start, written by the native host with --cov FILE */
extern uint8_t rt_cov[0x2000];
#define TR(a) (rt_cov[(a) >> 3] |= (uint8_t)(1u << ((a) & 7)))
#else
#define TR(a) ((void)0)
#endif

#define IEND(t, nx) do { ACC(t); if (CHK()) { M.pc = (nx); return; } } while (0)
#define IJP(t, tgt, L) do { ACC(t); if (CHK()) { M.pc = (tgt); return; } goto L; } while (0)
#define IJPX(t, tgt) do { ACC(t); M.pc = (tgt); return; } while (0)
#define IJPC(cond, tn, tt, tgt, L, nx) do { if (cond) { ACC(tt); if (CHK()) { M.pc = (tgt); return; } goto L; } ACC(tn); if (CHK()) { M.pc = (nx); return; } } while (0)
#define IJPCX(cond, tn, tt, tgt, nx) do { if (cond) { ACC(tt); M.pc = (tgt); return; } ACC(tn); if (CHK()) { M.pc = (nx); return; } } while (0)
#define ICALL(t, tgt, L, nx) do { PUSH16(nx); IJP(t, tgt, L); } while (0)
#define ICALLX(t, tgt, nx) do { PUSH16(nx); IJPX(t, tgt); } while (0)
#define ICALLC(cond, tn, tt, tgt, L, nx) do { if (cond) { PUSH16(nx); ACC(tt); if (CHK()) { M.pc = (tgt); return; } goto L; } ACC(tn); if (CHK()) { M.pc = (nx); return; } } while (0)
#define ICALLCX(cond, tn, tt, tgt, nx) do { if (cond) { PUSH16(nx); ACC(tt); M.pc = (tgt); return; } ACC(tn); if (CHK()) { M.pc = (nx); return; } } while (0)
#define IRET(t) do { M.pc = POP16(); ACC(t); if (CHK()) return; goto dispatch; } while (0)
#define IRETC(cond, tn, tt, nx) do { if (cond) { M.pc = POP16(); ACC(tt); if (CHK()) return; goto dispatch; } ACC(tn); if (CHK()) { M.pc = (nx); return; } } while (0)
#define IJPD(t, expr) do { M.pc = (uint16_t)(expr); ACC(t); if (CHK()) return; goto dispatch; } while (0)
#define IREP(t, selfpc, L) do { ACC(t); if (CHK()) { M.pc = (selfpc); return; } goto L; } while (0)
#define IHALT(nx) do { rt_halt(nx); M.pc = (nx); return; } while (0)

#endif
