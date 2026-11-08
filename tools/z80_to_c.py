"""Z80 -> C translator for the Devilish port (M3). Offline, from the user's own ROM; the output (extracted/gen/) is a derivative of the ROM and stays local.

    python tools/z80_to_c.py [--gen DIR]            # writes DIR/mod_lo.c mod_hi.c mod_b2.c mod_ram.c gen_info.c gen_manifest.json   (default extracted/gen)
    python tools/z80_to_c.py --conf DIR              # conformance cases: one C function per instruction encoding (see tools/conformance.py)

Pipeline: discovery (instruction starts from the static survey extracted/rom/insns.txt, the oracle's coverage extracted/oracle/*/cov.txt, the annotation file, closed
under direct control flow) -> one C `switch (M.pc)` machine per module with a case + label for every instruction -> modules:
    lo   $0000-$3FFF  bank 0                       hi   $4000-$7FFF  bank 1
    b2   $8000-$BFFF  slot-2 code, ROM bank 2 only ram  $C000-$FFFF  the sound driver the game unpacks into RAM at boot (image from the oracle)
Instruction semantics are expanded inline against src/rt/z80rt.h (flags exactly as the oracle's core, which zexdoc / zexall validate); control flow uses the macros
of src/rt/z80mod.h. R is counted per M1 cycle (the game's random number generator reads it). Not modelled (the translator refuses them, loudly): MEMPTR (BIT n,(HL)).

The RAM module is translated from the image extracted/oracle/ram_image.bin with tools/annotations_ram.json: bytes of the image that the program patches itself are
"patch cells": operand cells are read from RAM at run time, opcode cells are translated as the allowed alternatives; the runtime verifies the CRC of every other
code byte once per frame and stops loudly if it differs.
"""
import collections
import glob
import json
import os
import re
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import z80_dis as dis  # noqa: E402

ROOT = os.path.dirname(HERE)
BANK = 0x4000
SLOT2_CODE_BANK = 2
MODULES = [("lo", 0x0000, 0x4000), ("hi", 0x4000, 0x8000), ("b2", 0x8000, 0xC000), ("ram", 0xC000, 0x10000)]
CC = ["!(F & ZF)", "(F & ZF)", "!(F & CF)", "(F & CF)", "!(F & PF)", "(F & PF)", "!(F & SF)", "(F & SF)"]
R8N = ["B", "C", "D", "E", "H", "L", None, "A"]
ALUN = ["add", "adc", "sub", "sbc", "and", "xor", "or", "cp"]


EXECUTED = set()          # pcs the oracle has executed (filled by load_rom_seeds): an unsupported instruction there is fatal, elsewhere it is a run-time miss


class Unsupported(Exception):
    pass


class Ins:
    __slots__ = ("pc", "raw", "length", "prefix", "idx", "op", "d", "n8", "n16", "di", "toggle", "dyn_cells")

    def __init__(self):
        self.d = 0
        self.n8 = None
        self.n16 = None
        self.toggle = None
        self.dyn_cells = False


def u16(v):
    return "0x%04x" % (v & 0xFFFF)


def decode(rd, pc, ramcfg=None):
    """structured decode of the instruction at pc; rd(addr) -> byte"""
    di = dis.decode(rd, pc)
    ins = Ins()
    ins.pc, ins.raw, ins.length, ins.di = pc, di.raw, di.length, di
    b = list(di.raw)
    if di.text.endswith("(ignored)"):
        ins.prefix, ins.idx, ins.op = "ignored", 0, b[0]
        return ins
    idx = 0
    pos = 0
    if b[0] in (0xDD, 0xFD):
        idx = 1 if b[0] == 0xDD else 2
        pos = 1
    ins.idx = idx
    if b[pos] == 0xCB and not idx:
        ins.prefix, ins.op = "cb", b[1]
        return ins
    if b[pos] == 0xED:
        ins.prefix, ins.op = "ed", b[1]
        x, y, z = ins.op >> 6, (ins.op >> 3) & 7, ins.op & 7
        if x == 1 and z in (3,):
            ins.n16 = b[2] | b[3] << 8
        return _dyn(ins, ramcfg)
    if idx and b[pos] == 0xCB:
        ins.prefix, ins.d, ins.op = "ddcb", b[2] - 256 if b[2] > 127 else b[2], b[3]
        return ins
    op = b[pos]
    ins.prefix, ins.op = ("dd" if idx else "main"), op
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    pos += 1
    memop = False
    if x == 0 and z in (4, 5, 6) and y == 6:
        memop = True
    if x == 1 and op != 0x76 and (y == 6 or z == 6):
        memop = True
    if x == 2 and z == 6:
        memop = True
    if idx and memop:
        ins.d = b[pos] - 256 if b[pos] > 127 else b[pos]
        pos += 1
    if x == 0 and z == 6:
        ins.n8 = b[pos]
    elif x == 0 and z == 1 and q == 0:
        ins.n16 = b[pos] | b[pos + 1] << 8
    elif x == 0 and z == 2 and p >= 2:
        ins.n16 = b[pos] | b[pos + 1] << 8
    elif x == 3 and z == 6:
        ins.n8 = b[pos]
    elif x == 3 and z in (2, 4) or (x == 3 and z == 3 and y == 0) or (x == 3 and z == 5 and q == 1):
        ins.n16 = b[pos] | b[pos + 1] << 8
    elif x == 3 and z == 3 and y in (2, 3):
        ins.n8 = b[pos]
    return _dyn(ins, ramcfg)


def _dyn(ins, ramcfg):
    """RAM program: operand bytes that the program patches are read from RAM at run time"""
    if not ramcfg:
        return ins
    cells = ramcfg["operand_cells"]
    for off in range(1, ins.length):
        if ins.pc + off in cells:
            ins.dyn_cells = True
    if ins.dyn_cells:
        # the immediate / absolute operand is the last 1 or 2 bytes of the instruction
        if ins.n16 is not None:
            a = ins.pc + ins.length - 2
            ins.n16 = "(uint16_t)(RAMB(%s) | RAMB(%s) << 8)" % (u16(a), u16(a + 1))
        elif ins.n8 is not None:
            ins.n8 = "RAMB(%s)" % u16(ins.pc + ins.length - 1)
        else:
            raise Unsupported("patch cell in an instruction without an immediate: $%04x" % ins.pc)
    if ins.pc in ramcfg["opcode_cells"]:
        ins.toggle = ramcfg["opcode_cells"][ins.pc]
    return ins


def lit(v):
    """C text of an immediate that may be a literal or a run-time expression"""
    return v if isinstance(v, str) else "0x%x" % v


def is_const(v):
    return not isinstance(v, str)


class Emitter:
    def __init__(self, cfg, have_label):
        self.cfg = cfg                  # dict: need_r
        self.have_label = have_label    # function(target pc) -> label name or None (same module)

    # ---- control-flow terminators -------------------------------------------------------------------------------------------------------------
    def label(self, tgt):
        return self.have_label(tgt)

    def t_jp(self, t, tgt):
        l = self.label(tgt)
        return "IJP(%d, %s, %s);" % (t, u16(tgt), l) if l else "IJPX(%d, %s);" % (t, u16(tgt))

    def t_jpc(self, cond, tn, tt, tgt, nx):
        l = self.label(tgt)
        return ("IJPC(%s, %d, %d, %s, %s, %s);" % (cond, tn, tt, u16(tgt), l, u16(nx))) if l else \
               ("IJPCX(%s, %d, %d, %s, %s);" % (cond, tn, tt, u16(tgt), u16(nx)))

    def t_call(self, t, tgt, nx):
        l = self.label(tgt)
        return ("ICALL(%d, %s, %s, %s);" % (t, u16(tgt), l, u16(nx))) if l else ("ICALLX(%d, %s, %s);" % (t, u16(tgt), u16(nx)))

    def t_callc(self, cond, tn, tt, tgt, nx):
        l = self.label(tgt)
        return ("ICALLC(%s, %d, %d, %s, %s, %s);" % (cond, tn, tt, u16(tgt), l, u16(nx))) if l else \
               ("ICALLCX(%s, %d, %d, %s, %s);" % (cond, tn, tt, u16(tgt), u16(nx)))

    # ---- one instruction --------------------------------------------------------------------------------------------------------------------
    def emit(self, ins):
        """returns (statements, terminator or None, falls_through)"""
        S = []
        add = S.append
        di = ins.di
        nx = (ins.pc + ins.length) & 0xFFFF
        t, t2 = di.t, di.t2
        pc = ins.pc
        need_r = self.cfg["need_r"]

        def rinc(n):
            if need_r:
                add("RINC(%d);" % n)

        if ins.toggle is not None:
            return self.emit_toggle(ins, nx)
        if ins.prefix == "ignored":
            rinc(1)
            return S, "IEND(%d, %s);" % (t, u16(nx)), True
        if ins.prefix == "cb":
            rinc(2)
            self.cb(ins, S)
            return S, "IEND(%d, %s);" % (t, u16(nx)), True
        if ins.prefix == "ddcb":
            rinc(2)
            self.ddcb(ins, S)
            return S, "IEND(%d, %s);" % (t, u16(nx)), True
        if ins.prefix == "ed":
            rinc(2)
            return self.ed(ins, S, nx, t, t2)
        rinc(2 if ins.idx else 1)
        return self.main(ins, S, nx, t, t2)

    def emit_toggle(self, ins, nx):
        """opcode cell: the program stores either NOP ($00) or RET ($C9) here (a mute switch)"""
        S = []
        di = ins.di
        if self.cfg["need_r"]:
            S.append("RINC(1);")
        a = ins.pc & 0x1FFF
        allowed = ins.toggle
        if allowed != [0x00, 0xC9]:
            raise Unsupported("opcode cell $%04x with unsupported alternatives %s" % (ins.pc, allowed))
        S.append("if (RAMB(%s) == 0xC9) IRET(10);" % u16(ins.pc))
        S.append("if (RAMB(%s) != 0x00) rt_miss(%s, \"patched opcode cell holds an unexpected value\");" % (u16(ins.pc), u16(ins.pc)))
        return S, "IEND(4, %s);" % u16(nx), True

    # ---- helpers for operand text ------------------------------------------------------------------------------------------------------------
    def hlx(self, idx):
        return "HLv" if idx == 0 else "M.ix" if idx == 1 else "M.iy"

    def sethlx(self, idx, e):
        return "SET_HL(%s);" % e if idx == 0 else ("M.ix = (uint16_t)(%s);" % e if idx == 1 else "M.iy = (uint16_t)(%s);" % e)

    def ea(self, ins):
        if ins.idx == 0:
            return "HLv"
        return "(uint16_t)(M.%s + (%d))" % ("ix" if ins.idx == 1 else "iy", ins.d)

    def rget(self, ins, i, memop=False):
        if i == 6:
            return "RD8(%s)" % self.ea(ins)
        if ins.idx and i in (4, 5) and not memop:
            return ("IXH", "IXL", "IYH", "IYL")[(ins.idx - 1) * 2 + (i - 4)]
        return R8N[i]

    def rset(self, ins, i, e, memop=False):
        if i == 6:
            return "WR8(%s, %s);" % (self.ea(ins), e)
        if ins.idx and i in (4, 5) and not memop:
            return "SET_%s(%s);" % (("IXH", "IXL", "IYH", "IYL")[(ins.idx - 1) * 2 + (i - 4)], e)
        return "%s = (uint8_t)(%s);" % (R8N[i], e)

    def alu(self, y, v):
        return ["A = z_add8(A, %s, 0);", "A = z_add8(A, %s, F & CF);", "A = z_sub8(A, %s, 0);", "A = z_sub8(A, %s, F & CF);",
                "A = z_and8(A, %s);", "A = z_xor8(A, %s);", "A = z_or8(A, %s);", "z_cp8(A, %s);"][y] % v

    def rp(self, ins, p):
        return ["BCv", "DEv", self.hlx(ins.idx), "M.sp"][p]

    def setrp(self, ins, p, e):
        return ["SET_BC(%s);" % e, "SET_DE(%s);" % e, self.sethlx(ins.idx, e), "M.sp = (uint16_t)(%s);" % e][p]

    def rp2(self, ins, p):
        return ["BCv", "DEv", self.hlx(ins.idx), "AFv"][p]

    # ---- CB ------------------------------------------------------------------------------------------------------------------------------------
    def cb(self, ins, S):
        op = ins.op
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        if z == 6:
            if x == 1:
                if not self.cfg.get("approx_memptr"):
                    raise Unsupported("bit n,(hl): X/Y flags come from MEMPTR, which is not modelled (pc $%04x)" % ins.pc)
                S.append("z_bit(%d, RD8(HLv), H);   /* X/Y from H: MEMPTR is not modelled */" % y)
                return
            e = ("z_rot(%d, RD8(HLv))" % y) if x == 0 else ("(uint8_t)(RD8(HLv) & ~%d)" % (1 << y)) if x == 2 else ("(uint8_t)(RD8(HLv) | %d)" % (1 << y))
            S.append("{ uint16_t ea_ = HLv; WR8(ea_, %s); }" % e.replace("RD8(HLv)", "RD8(ea_)"))
            return
        r = R8N[z]
        if x == 0:
            S.append("%s = z_rot(%d, %s);" % (r, y, r))
        elif x == 1:
            S.append("z_bit(%d, %s, %s);" % (y, r, r))
        elif x == 2:
            S.append("%s = (uint8_t)(%s & ~%d);" % (r, r, 1 << y))
        else:
            S.append("%s = (uint8_t)(%s | %d);" % (r, r, 1 << y))

    def ddcb(self, ins, S):
        op = ins.op
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        ea = "(uint16_t)(M.%s + (%d))" % ("ix" if ins.idx == 1 else "iy", ins.d)
        if x == 1:
            S.append("{ uint16_t ea_ = %s; z_bit(%d, RD8(ea_), (uint8_t)(ea_ >> 8)); }" % (ea, y))
            return
        if x == 0:
            f = "z_rot(%d, RD8(ea_))" % y
        elif x == 2:
            f = "(uint8_t)(RD8(ea_) & ~%d)" % (1 << y)
        else:
            f = "(uint8_t)(RD8(ea_) | %d)" % (1 << y)
        cp = "" if z == 6 else " %s = r_;" % R8N[z]
        S.append("{ uint16_t ea_ = %s; uint8_t r_ = %s; WR8(ea_, r_);%s }" % (ea, f, cp))

    # ---- ED ------------------------------------------------------------------------------------------------------------------------------------
    def ed(self, ins, S, nx, t, t2):
        op = ins.op
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        p, q = y >> 1, y & 1
        end = "IEND(%d, %s);" % (t, u16(nx))
        if x == 2 and z <= 3 and y >= 4:
            dirn = -1 if (y & 1) else 1
            rep = 1 if y >= 6 else 0
            me = u16(ins.pc)
            fn = ["z_ldx(%d, %d, %s)" % (dirn, rep, me), "z_cpx(%d, %d, %s)" % (dirn, rep, me), "z_inx(%d, %d)" % (dirn, rep), "z_outx(%d, %d)" % (dirn, rep)][z]
            if z in (2, 3):
                S.append("M.cur = %s;" % me)
            if rep:
                S.append("if (%s) { IREP(%d, %s, L_%04x); }" % (fn, t2, me, ins.pc))
            else:
                S.append("(void)%s;" % fn)
            return S, end, True
        if x != 1:
            return S, "IEND(8, %s);" % u16(nx), True        # undefined ED opcode: two-byte NOP
        if z == 0:
            S.append("M.cur = %s;" % u16(ins.pc))
            if y == 6:
                S.append("{ uint8_t v_ = z_in(BCv); z_in_flags(v_); }")
            else:
                S.append("{ uint8_t v_ = z_in(BCv); %s = v_; z_in_flags(v_); }" % R8N[y])
        elif z == 1:
            S.append("M.cur = %s;" % u16(ins.pc))
            S.append("z_out(BCv, %s);" % ("0" if y == 6 else R8N[y]))
        elif z == 2:
            rp = ["BCv", "DEv", "HLv", "M.sp"][p]
            S.append("SET_HL(%s(HLv, %s));" % ("z_adc16" if q else "z_sbc16", rp))
        elif z == 3:
            nn = lit(ins.n16)
            rp = ["BCv", "DEv", "HLv", "M.sp"][p]
            if q:
                S.append({0: "SET_BC(RD16(%s));", 1: "SET_DE(RD16(%s));", 2: "SET_HL(RD16(%s));", 3: "M.sp = RD16(%s);"}[p] % nn)
            else:
                S.append("WR16(%s, %s);" % (nn, rp))
        elif z == 4:
            S.append("A = z_sub8(0, A, 0);")
        elif z == 5:
            S.append("M.iff1 = M.iff2; IRQ_REFRESH();")
            return S, "IRET(%d);" % t, False
        elif z == 6:
            S.append("M.im = %d;" % (1 if (y & 3) == 2 else 2 if (y & 3) == 3 else 0))
        else:
            if y == 0:
                S.append("M.i = A;")
            elif y == 1:
                S.append("RSET(A);")
            elif y == 2:
                S.append("z_ld_a_ir(M.i);")
            elif y == 3:
                S.append("z_ld_a_ir(RGET());")
            elif y == 4:
                S.append("{ uint16_t ea_ = HLv; WR8(ea_, z_rrd(RD8(ea_))); }")
            elif y == 5:
                S.append("{ uint16_t ea_ = HLv; WR8(ea_, z_rld(RD8(ea_))); }")
        return S, end, True

    # ---- unprefixed and DD / FD ------------------------------------------------------------------------------------------------------------------
    def main(self, ins, S, nx, t, t2):
        op, idx = ins.op, ins.idx
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        p, q = y >> 1, y & 1
        add = S.append
        end = "IEND(%d, %s);" % (t, u16(nx))
        hlx = self.hlx(idx)
        di = ins.di

        if x == 0:
            if z == 0:
                if y == 0:
                    return S, end, True
                if y == 1:
                    add("EX_AF();")
                    return S, end, True
                tgt = di.target
                if y == 2:
                    add("B--;")
                    return S, self.t_jpc("B != 0", t, t2, tgt, nx), True
                if y == 3:
                    return S, self.t_jp(t, tgt), False
                return S, self.t_jpc(CC[y - 4], t, t2, tgt, nx), True
            if z == 1:
                if q == 0:
                    add(self.setrp(ins, p, lit(ins.n16)))
                else:
                    add(self.sethlx(idx, "z_add16(%s, %s)" % (hlx, self.rp(ins, p))))
                return S, end, True
            if z == 2:
                if p < 2:
                    rp = "BCv" if p == 0 else "DEv"
                    add(("A = RD8(%s);" % rp) if q else ("WR8(%s, A);" % rp))
                    return S, end, True
                nn = ins.n16
                if p == 2:
                    if q:
                        add(self.sethlx(idx, "RD16(%s)" % lit(nn)))
                    else:
                        add("WR16(%s, %s);" % (lit(nn), hlx))
                else:
                    if is_const(nn):
                        add("A = RDK(%s);" % lit(nn) if q else "WRK(%s, A);" % lit(nn))
                    else:
                        add("A = RD8(%s);" % nn if q else "WR8(%s, A);" % nn)
                return S, end, True
            if z == 3:
                d = "- 1" if q else "+ 1"
                add(self.setrp(ins, p, "%s %s" % (self.rp(ins, p), d)))
                return S, end, True
            if z in (4, 5):
                fn = "z_inc8" if z == 4 else "z_dec8"
                if y == 6:
                    add("{ uint16_t ea_ = %s; WR8(ea_, %s(RD8(ea_))); }" % (self.ea(ins), fn))
                else:
                    add(self.rset(ins, y, "%s(%s)" % (fn, self.rget(ins, y))))
                return S, end, True
            if z == 6:
                if y == 6:
                    add("WR8(%s, %s);" % (self.ea(ins), lit(ins.n8)))
                else:
                    add(self.rset(ins, y, lit(ins.n8)))
                return S, end, True
            add(["z_rlca();", "z_rrca();", "z_rla();", "z_rra();", "z_daa();", "z_cpl();", "z_scf();", "z_ccf();"][y])
            if y in (6, 7) and not self.cfg.get("approx_scf"):
                raise Unsupported("scf / ccf: the Q factor is not modelled (pc $%04x)" % ins.pc)
            return S, end, True

        if x == 1:
            if op == 0x76:
                return S, "IHALT(%s);" % u16(nx), False
            memop = (y == 6 or z == 6)
            if memop:
                if y == 6:
                    add("WR8(%s, %s);" % (self.ea(ins), R8N[z]))
                else:
                    add("%s = RD8(%s);" % (R8N[y], self.ea(ins)))
            else:
                add(self.rset(ins, y, self.rget(ins, z)))
            return S, end, True

        if x == 2:
            add(self.alu(y, self.rget(ins, z, memop=(z == 6))))
            return S, end, True

        # x == 3
        if z == 0:
            return S, "IRETC(%s, %d, %d, %s);" % (CC[y], t, t2, u16(nx)), True
        if z == 1:
            if q == 0:
                if p == 3:
                    add("SET_AF(POP16());")
                else:
                    add({0: "SET_BC(POP16());", 1: "SET_DE(POP16());", 2: self.sethlx(idx, "POP16()")}[p])
                return S, end, True
            if p == 0:
                return S, "IRET(%d);" % t, False
            if p == 1:
                add("EXX();")
                return S, end, True
            if p == 2:
                return S, "IJPD(%d, %s);" % (t, hlx), False
            add("M.sp = %s;" % hlx)
            return S, end, True
        if z == 2:
            return S, self.t_jpc(CC[y], t, t, di.target, nx), True
        if z == 3:
            if y == 0:
                return S, self.t_jp(t, di.target), False
            if y == 2:
                add("M.cur = %s;" % u16(ins.pc))
                add("z_out((uint16_t)((uint16_t)A << 8 | %s), A);" % lit(ins.n8))
            elif y == 3:
                add("M.cur = %s;" % u16(ins.pc))
                add("A = z_in((uint16_t)((uint16_t)A << 8 | %s));" % lit(ins.n8))
            elif y == 4:
                add("{ uint16_t v_ = RD16(M.sp); WR16(M.sp, %s); %s }" % (hlx, self.sethlx(idx, "v_")))
            elif y == 5:
                add("EX_DE_HL();")
            elif y == 6:
                add("M.iff1 = M.iff2 = 0; IRQ_REFRESH();")
            else:
                add("M.iff1 = M.iff2 = 1; M.ei_delay = 1; IRQ_REFRESH();")
            return S, end, True
        if z == 4:
            return S, self.t_callc(CC[y], t, t2, di.target, nx), True
        if z == 5:
            if q == 0:
                add("PUSH16(%s);" % self.rp2(ins, p))
                return S, end, True
            return S, self.t_call(t, di.target, nx), True
        if z == 6:
            add(self.alu(y, lit(ins.n8)))
            return S, end, True
        return S, self.t_call(t, y * 8, nx), True   # rst


# ----------------------------------------------------------------------------------------------------------------------------------------------------
# discovery

FLOW_FALL = {"seq", "jpc", "jrc", "djnz", "callc", "retc", "call", "rst", "halt"}   # these continue at the next instruction (calls are assumed to return)


def phys_of(pc):
    if pc < 0x8000:
        return pc
    if pc < 0xC000:
        return SLOT2_CODE_BANK * BANK + (pc - 0x8000)
    return None


def discover(rom, seeds, ramcfg=None, ram=None):
    """closure of `seeds` (pcs) under direct control flow -> {pc: Ins}"""
    if ramcfg is None:
        rd = lambda a: rom[phys_of(a & 0xFFFF)] if phys_of(a & 0xFFFF) is not None and phys_of(a & 0xFFFF) < len(rom) else 0xFF
    else:
        rd = lambda a: ram[(a & 0xFFFF) & 0x1FFF]
    lo, hi = (0, 0xC000) if ramcfg is None else (0xC000, 0x10000)
    out = {}
    work = list(seeds)
    while work:
        pc = work.pop()
        if pc in out or not (lo <= pc < hi):
            continue
        ins = decode(rd, pc, ramcfg)
        out[pc] = ins
        di = ins.di
        k = di.kind
        if ins.prefix == "ignored":
            work.append((pc + 1) & 0xFFFF)
            continue
        if k in ("jp", "jr", "jpc", "jrc", "djnz", "call", "callc") and di.target is not None:
            work.append(di.target)
        if k == "rst":
            work.append(di.target)
        if ins.toggle is not None:
            work.append((pc + 1) & 0xFFFF)     # the NOP alternative continues; the RET alternative is handled in the emitted code
            continue
        if k in FLOW_FALL:
            work.append((pc + ins.length) & 0xFFFF)
    return out


def load_gap_seeds():
    """start addresses of the code-like gaps of the static survey (extracted/rom/gaps.txt, tools/gaps.py): uncovered runs of at least 8 plausible instructions that are
    anchored (call / jp into known code, RAM access or a known port). Code that no test run has executed yet (handlers reached through tables, rarely used paths)
    is mostly found here; a false positive costs only some unused C code. Only banks 0 and 1 (slots 0 / 1) and the slot-2 code bank are used."""
    seeds = set()
    path = os.path.join(ROOT, "extracted", "rom", "gaps.txt")
    if not os.path.exists(path):
        return seeds
    for line in open(path):
        m = re.match(r"([0-9a-f]+)\s+\$([0-9a-f]+)\s+(\d+) bytes", line)
        if not m:
            continue
        ph = int(m.group(1), 16)
        if ph < 0x8000:
            seeds.add(ph)
        elif ph // BANK == SLOT2_CODE_BANK:
            seeds.add(0x8000 + (ph & 0x3FFF))
    return seeds


def load_rom_seeds(rom):
    seeds = set()
    EXECUTED.clear()
    for line in open(os.path.join(ROOT, "extracted", "rom", "insns.txt")):
        ph, pc, ln, text = line.split(" ", 3)
        ph, pc = int(ph, 16), int(pc, 16)
        if ph < 0x8000 or ph // BANK == SLOT2_CODE_BANK:
            seeds.add(pc)
    ora = 0
    ram_pcs = set()
    for f in glob.glob(os.path.join(ROOT, "extracted", "oracle", "*", "cov.txt")):
        for line in open(f):
            t = line.split()
            if t[0] == "rom":
                ph = int(t[1], 16)
                pc = ph if ph < 0x8000 else 0x8000 + (ph & 0x3FFF)
                if ph < 0x8000 or ph // BANK == SLOT2_CODE_BANK:
                    seeds.add(pc)
                    EXECUTED.add(pc)
                    ora += 1
                else:
                    raise SystemExit("the oracle executed ROM code in bank %d (file offset $%05x): extend the translator's slot-2 handling" % (ph // BANK, ph))
            elif t[0] == "ram":
                ram_pcs.add(int(t[1], 16))
    return seeds, ram_pcs


# ----------------------------------------------------------------------------------------------------------------------------------------------------
# writing

def write_module(path, name, lo, hi, insns, cfg):
    keys = sorted(insns)
    inmod = set(keys)
    label = lambda tgt: ("L_%04x" % tgt) if tgt in inmod else None
    em = Emitter(cfg, label)
    out = []
    out.append("/* %s: generated by tools/z80_to_c.py from the user's ROM. Derived data: keep local. %d instructions. */" % (name, len(keys)))
    out.append('#include "../../src/rt/z80mod.h"')
    out.append("")
    out.append("void run_%s(void)" % name)
    out.append("{")
    out.append("dispatch:")
    out.append("    switch (M.pc) {")
    for k in keys:
        out.append("    case 0x%04x: goto L_%04x;" % (k, k))
    out.append("    default:")
    out.append("        if (M.pc >= 0x%04x && M.pc < 0x%05x) rt_miss(M.pc, \"jump to an address the translator did not find code at (module %s)\");" % (lo, hi, name))
    out.append("        return;")
    out.append("    }")
    errors = []
    for i, k in enumerate(keys):
        ins = insns[k]
        try:
            S, term, falls = em.emit(ins)
        except Unsupported as e:
            errors.append((k in EXECUTED or name == "ram", str(e)))
            S, term, falls = ['rt_miss(0x%04x, "untranslatable instruction");' % k], None, False
        out.append("L_%04x: TR(0x%04x); /* %s */" % (k, k, ins.di.text))
        for s in S:
            out.append("    " + s)
        if term:
            out.append("    " + term)
        if falls:
            nx = (k + ins.length) & 0xFFFF
            following = keys[i + 1] if i + 1 < len(keys) else None
            if following != nx:
                if nx in inmod:
                    out.append("    goto L_%04x;" % nx)
                else:
                    out.append("    M.pc = 0x%04x; return;" % nx)
    out.append("}")
    open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
    return errors


def main():
    args = sys.argv[1:]
    gen = os.path.join(ROOT, "extracted", "gen")
    conf = None
    i = 0
    while i < len(args):
        if args[i] == "--gen":
            gen = args[i + 1]; i += 2
        elif args[i] == "--conf":
            conf = args[i + 1]; i += 2
        elif args[i] == "--no-gaps" or args[i] == "--no-ptr":
            i += 1
        else:
            raise SystemExit(__doc__)
    if conf:
        raise SystemExit("--conf is implemented in tools/conformance.py")
    os.makedirs(gen, exist_ok=True)
    rom = dis.read_rom()
    seeds, ram_pcs = load_rom_seeds(rom)
    seeds |= {0x0000, 0x0038, 0x0066}
    for line in open(os.path.join(HERE, "annotations.txt"), encoding="utf-8"):
        t = line.split("#")[0].split()
        if len(t) == 4 and t[0] == "table":
            base, cnt = int(t[1], 0), int(t[2], 0)
            for k in range(cnt):
                v = rom[base + 2 * k] | rom[base + 2 * k + 1] << 8
                if v >= 0x100:
                    seeds.add(v)
    # tables of code addresses (tools/ptr_seeds.py) found from the solid seeds (survey, oracle coverage, annotations) and iterated: code that is only reached through a
    # table is what the oracle runs and the static descent cannot see
    ptr_new = set()
    if "--no-ptr" not in args:
        import ptr_seeds
        for _ in range(5):
            insns = discover(rom, seeds | ptr_new)
            covered = set()
            for pc, ins in insns.items():
                ph = phys_of(pc)
                for k in range(ins.length):
                    covered.add(ph + k)
            new, _tables = ptr_seeds.find(rom, covered, set(insns), SLOT2_CODE_BANK, phys_of)
            if new <= ptr_new:
                break
            ptr_new |= new
        seeds |= ptr_new
    gap_seeds = load_gap_seeds() if "--no-gaps" not in args else set()
    seeds |= gap_seeds
    tail_new = set()
    if "--no-ptr" not in args:
        import ptr_seeds
        for _ in range(4):
            insns = discover(rom, seeds | tail_new)
            covered = set()
            for pc, ins in insns.items():
                ph = phys_of(pc)
                for k in range(ins.length):
                    covered.add(ph + k)
            new = (ptr_seeds.find_tails(rom, covered, insns, phys_of) | ptr_seeds.find_immediates(rom, covered, insns, phys_of)) - tail_new - seeds
            if not new:
                break
            tail_new |= new
        seeds |= tail_new
    # a RAM program may be entered through jp (hl) targets recorded by the oracle (they are in ram_pcs already); the ROM side has its own seeds
    cfg = {"need_r": True, "approx_scf": True, "approx_memptr": True}   # undocumented X/Y flags of scf, ccf and bit n,(hl): approximated (NOTES M7)
    rom_insns = discover(rom, seeds)
    ramcfg = json.load(open(os.path.join(HERE, "annotations_ram.json")))
    ramcfg["operand_cells"] = {int(a, 0) for a in ramcfg["operand_cells"]}
    ramcfg["opcode_cells"] = {int(a, 0): v for a, v in ramcfg["opcode_cells"].items()}
    ram = open(os.path.join(ROOT, ramcfg["image"]), "rb").read()
    ram_seeds = set(ram_pcs) | {int(e, 0) for e in ramcfg["entries"]}
    ram_insns = discover(rom, ram_seeds, ramcfg, ram)
    ram_ptr = set()
    if "--no-ptr" not in args:
        # the driver's own command / handler tables: handlers that no test run has triggered yet
        import ptr_seeds
        ram_rd = lambda a: ram[(a & 0xFFFF) & 0x1FFF]
        ram_phys = lambda a: (a - 0xC000) if 0xC000 <= a < 0xE000 else None
        for _ in range(5):
            covered = set()
            for pc, ins in ram_insns.items():
                for k in range(ins.length):
                    covered.add(pc + k - 0xC000)
            new, _tables = ptr_seeds.find(ram, covered, set(ram_insns), None, ram_phys, ranges=((0xD500, 0xDE67),), rd=ram_rd)
            if new <= ram_ptr:
                break
            ram_ptr |= new
            ram_insns = discover(rom, ram_seeds | ram_ptr, ramcfg, ram)
    # self-modifying stores found statically: an instruction that writes (ld (nn),a / hl / de / bc / sp / ix / iy) into bytes of translated driver code. Operand bytes are
    # added to the patch cells; a store into an opcode byte needs its alternatives from a human (reported, the runtime CRC check stops the game if it ever happens)
    owner = {}
    for pc, ins in ram_insns.items():
        for k in range(ins.length):
            owner[pc + k] = (pc, k)
    static_cells, static_opcode = set(), []
    for pc, ins in ram_insns.items():
        di = ins.di
        if di.abs_addr is None or not di.text.startswith("ld (") or di.text.startswith("ld (hl)") or di.text.startswith("ld (ix") or di.text.startswith("ld (iy") or di.text.startswith("ld (bc)") or di.text.startswith("ld (de)"):
            continue
        width = 1 if di.text.endswith(",a") else 2
        for k in range(width):
            tgt = (di.abs_addr + k) & 0xFFFF
            if tgt in owner:
                if owner[tgt][1] == 0:
                    if tgt not in ramcfg["opcode_cells"]:
                        static_opcode.append((pc, tgt))
                elif tgt not in ramcfg["operand_cells"]:
                    static_cells.add(tgt)
    if static_cells:
        ramcfg["operand_cells"] = ramcfg["operand_cells"] | static_cells
        ram_insns = discover(rom, ram_seeds | ram_ptr, ramcfg, ram)
    allerr = []
    manifest = {"modules": {}}
    for name, lo, hi in MODULES:
        if name == "ram":
            insns = ram_insns
        else:
            insns = {k: v for k, v in rom_insns.items() if lo <= k < hi}
        errs = write_module(os.path.join(gen, "mod_%s.c" % name), name, lo, hi, insns, cfg)
        allerr += errs
        manifest["modules"][name] = {"instructions": len(insns), "bytes": sum(v.length for v in insns.values())}
    # facts for the runtime: the RAM program's executed code bytes (patch cells zeroed) and its CRC
    cells = ramcfg["operand_cells"] | set(ramcfg["opcode_cells"])
    spans = collections.OrderedDict()
    for k in sorted(ram_insns):
        for b in range(ram_insns[k].length):
            spans[k + b] = True
    img = bytearray(ram)
    for c in cells:
        img[c & 0x1FFF] = 0
    runs = []
    for a in sorted(spans):
        if runs and runs[-1][0] + runs[-1][1] == a:
            runs[-1][1] += 1
        else:
            runs.append([a, 1])
    crc = 0
    for a, n in runs:
        crc = zlib.crc32(bytes(img[a & 0x1FFF:(a & 0x1FFF) + n]), crc)
    info = ["/* generated by tools/z80_to_c.py: facts about the translation that the runtime checks. Derived data: keep local. */", "#include <stdint.h>",
            "const uint32_t gen_ram_crc = 0x%08x;" % crc,
            "const int gen_slot2_code_bank = %d;" % SLOT2_CODE_BANK,
            "const int gen_ram_nspans = %d;" % len(runs),
            "const uint16_t gen_ram_spans[][2] = {" + ", ".join("{0x%04x, %d}" % (a, n) for a, n in runs) + "};",
            "const int gen_ram_npatch = %d;" % len(cells),
            "const uint16_t gen_ram_patch[] = {" + ", ".join("0x%04x" % c for c in sorted(cells)) + "};",
            # the high-score tables as the game's start-up routine writes them (BEST LAP TIMES $CEC7 x 48, BEST PLAYERS $CF07 x 20), read from the oracle's RAM image: src/rt/hiscore.c
            # recognises the start-up state by them (the source holds no game data)
            "const uint8_t gen_hs_defaults[68] = {" + ", ".join("0x%02x" % b for b in (bytes(ram[0xEC7:0xEC7 + 48]) + bytes(ram[0xF07:0xF07 + 20]))) + "};"]
    open(os.path.join(gen, "gen_info.c"), "w").write("\n".join(info) + "\n")
    manifest["ram_crc"] = "%08x" % crc
    manifest["ram_code_bytes"] = len(spans)
    manifest["rom_seed_count"] = len(seeds)
    manifest["gap_seed_count"] = len(gap_seeds)
    manifest["ptr_seed_count"] = len(ptr_new)
    manifest["ram_ptr_seed_count"] = len(ram_ptr)
    manifest["tail_seed_count"] = len(tail_new)
    manifest["ram_static_patch_cells"] = sorted("%04x" % c for c in static_cells)
    manifest["ram_static_opcode_stores"] = ["%04x->%04x" % x for x in static_opcode]
    manifest["errors"] = [m for fatal, m in allerr if fatal]
    manifest["warnings"] = [m for fatal, m in allerr if not fatal]
    json.dump(manifest, open(os.path.join(gen, "gen_manifest.json"), "w"), indent=1)
    tot = sum(m["instructions"] for m in manifest["modules"].values())
    print("translated %d instructions: %s" % (tot, ", ".join("%s %d" % (n, m["instructions"]) for n, m in manifest["modules"].items())))
    print("RAM program: %d code bytes, crc %08x, %d patch cells (%d found by the static scan: %s), %d seeds from the driver's tables" % (
        len(spans), crc, len(cells), len(static_cells), " ".join("$%04x" % c for c in sorted(static_cells)), len(ram_ptr)))
    if static_opcode:
        print("ERROR: stores into an opcode byte of translated driver code (a false code seed over data, or a new self-modifying cell that needs alternatives in annotations_ram.json): %s" % " ".join("$%04x->$%04x" % x for x in static_opcode))
        sys.exit(1)
    print("seeds: %d (of which %d from pointer tables, %d from the survey's code-like gaps, %d short routines after a jp / jr / ret or in untranslated stretches)" % (
        len(seeds), len(ptr_new), len(gap_seeds), len(tail_new)))
    warn = [m for fatal, m in allerr if not fatal]
    if warn:
        print("not executed by any run and not supported (they stop with a miss if the game ever gets there): %d" % len(warn))
        for e in warn[:40]:
            print("  ", e)
    fatal = [m for f, m in allerr if f]
    if fatal:
        print("UNSUPPORTED in executed code (%d):" % len(fatal))
        for e in fatal[:20]:
            print("  ", e)
        sys.exit(1)


if __name__ == "__main__":
    main()
