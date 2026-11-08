"""Z80 decoder / disassembler with T-state counts, shared by every tool (survey, oracle, translator).

    python tools/z80_dis.py --selftest
    python tools/z80_dis.py [ROM.gg] BANK:ADDR [COUNT]       # list COUNT instructions (default 32) from a slot address in a bank

`decode(rd, pc)` returns an `Insn` for the instruction at `pc`; `rd(addr)` gives the byte at an address (the caller decides what the address means:
file offset, bank-qualified, RAM...). Complete: all 252 base opcodes, CB, ED (including the 8-T NOP aliases), DD / FD (including the undocumented
IXH / IXL / IYH / IYL forms) and DDCB / FDCB (including the undocumented "also copy to register" forms), SLL, `IN F,(C)`, `OUT (C),0`.

Rules the translator relies on:
  * A DD / FD prefix in front of an opcode that does not use HL / H / L / (HL) (or in front of DD / FD / ED) is an ignored 4-T prefix: it is decoded as its own 1-byte
    instruction (`kind` "seq", text "dd/fd prefix (ignored)") and the next byte is decoded as a normal instruction.
  * `t` is the T-state count for the normal path, `t2` the count when a conditional instruction is taken / repeats (None for unconditional ones);
    for `jr cc` / `jp cc` ... see the comments at `kind`: `t` = not taken, `t2` = taken (`djnz`: t = 8 not taken, t2 = 13 taken; block repeats: t = 16 done, t2 = 21 repeat).
  * `kind` is one of: seq, jp, jr, jpc, jrc, djnz, call, callc, ret, retc, reti, retn, rst, jpind (JP (HL) / (IX) / (IY)), halt.
    `target` is the branch / call / rst target (None for ret, jpind and seq).
  * `mn` is the mnemonic, `args` the operand text (lower case, hex as `$xx`), `text` both together.
  * `port` is the immediate port of `in a,(n)` / `out (n),a` (None otherwise), `imm` the immediate operand (8 or 16 bit) if any, `abs_addr` the absolute memory
    address operand of `ld (nn),r` / `ld r,(nn)` forms.

The tests (`--selftest`) come from the same author as the tables: the independent validation is the oracle run against public test ROMs (M2).
"""
import os
import sys
from collections import namedtuple

Insn = namedtuple("Insn", "addr length raw text mn args t t2 kind target port imm abs_addr")

R8 = ["b", "c", "d", "e", "h", "l", "(hl)", "a"]
RP = ["bc", "de", "hl", "sp"]
RP2 = ["bc", "de", "hl", "af"]
CC = ["nz", "z", "nc", "c", "po", "pe", "p", "m"]
ALU = ["add", "adc", "sub", "sbc", "and", "xor", "or", "cp"]
ALU_A = ["a,", "a,", "", "a,", "", "", "", ""]   # the "a," operand prefix of add / adc / sbc
ROT = ["rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"]
BLOCK = [["ldi", "ldd", "ldir", "lddr"], ["cpi", "cpd", "cpir", "cpdr"], ["ini", "ind", "inir", "indr"], ["outi", "outd", "otir", "otdr"]]
IM_MODE = ["0", "0", "1", "2", "0", "0", "1", "2"]


def hx8(v):
    return "$%02x" % v


def hx16(v):
    return "$%04x" % v


def sgn(v):
    return v - 256 if v & 0x80 else v


def _uses_hl(op):
    """does the unprefixed opcode `op` refer to HL, H, L or (HL)? (then DD / FD change its meaning)"""
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    if x == 0:
        if z == 1:
            return q == 1 or p == 2
        if z in (2, 3):
            return p == 2
        if z in (4, 5, 6):
            return y in (4, 5, 6)
        return False
    if x == 1:
        return op != 0x76 and (y in (4, 5, 6) or z in (4, 5, 6))
    if x == 2:
        return z in (4, 5, 6)
    if z == 1:
        return (q == 0 and p == 2) or (q == 1 and p in (2, 3))
    if z == 3:
        return y == 4
    if z == 5:
        return q == 0 and p == 2
    return False


def decode(rd, pc):
    """decode the instruction at pc (addresses wrap at 16 bits)"""
    p = pc & 0xFFFF
    start = p
    raw = []

    def fetch():
        nonlocal p
        v = rd(p) & 0xFF
        raw.append(v)
        p = (p + 1) & 0xFFFF
        return v

    def make(mn, args="", t=4, t2=None, kind="seq", target=None, port=None, imm=None, abs_addr=None):
        text = (mn + " " + args) if args else mn
        return Insn(start, len(raw), bytes(raw), text, mn, args, t, t2, kind, target, port, imm, abs_addr)

    b = fetch()
    idx = None
    if b in (0xDD, 0xFD):
        nb = rd(p) & 0xFF
        if nb in (0xDD, 0xFD, 0xED) or (nb != 0xCB and not _uses_hl(nb)):
            return make("dd prefix (ignored)" if b == 0xDD else "fd prefix (ignored)", "", 4)
        idx = "ix" if b == 0xDD else "iy"
        b = fetch()
        if b == 0xCB:
            d = fetch()
            op = fetch()
            return _decode_ddcb(make, idx, d, op)
    if b == 0xCB:
        return _decode_cb(make, fetch())
    if b == 0xED:
        return _decode_ed(make, fetch, p)
    return _decode_base(make, fetch, b, idx, start)


def _reg(i, idx, disp, hl_mem_in_insn):
    """operand name of 8-bit register index i; with an index prefix H / L become IXH / IXL unless the instruction also has an (IX+d) operand"""
    if i == 6:
        if idx:
            return "(%s%s$%02x)" % (idx, "-" if disp & 0x80 else "+", (-sgn(disp)) & 0xFF if disp & 0x80 else disp)
        return "(hl)"
    if idx and i in (4, 5) and not hl_mem_in_insn:
        return idx + ("h" if i == 4 else "l")
    return R8[i]


def _decode_base(make, fetch, op, idx, start):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    hl = idx or "hl"
    extra = 4 if idx else 0           # T-states added to the register forms by a DD / FD prefix
    disp = 0

    def mem_disp():
        nonlocal disp
        disp = fetch()
        return disp

    if x == 0:
        if z == 0:
            if y == 0:
                return make("nop")
            if y == 1:
                return make("ex", "af,af'")
            d = fetch()
            tgt = (start + 2 + sgn(d)) & 0xFFFF
            if y == 2:
                return make("djnz", hx16(tgt), 8, 13, "djnz", tgt)
            if y == 3:
                return make("jr", hx16(tgt), 12, None, "jr", tgt)
            return make("jr", "%s,%s" % (CC[y - 4], hx16(tgt)), 7, 12, "jrc", tgt)
        if z == 1:
            if q == 0:
                lo = fetch()
                hi = fetch()
                v = lo | hi << 8
                return make("ld", "%s,%s" % (hl if p == 2 else RP[p], hx16(v)), 10 + extra, imm=v)
            return make("add", "%s,%s" % (hl, hl if p == 2 else RP[p]), 11 + extra)
        if z == 2:
            if p == 0 or p == 1:
                rp = RP[p]
                return make("ld", ("a,(%s)" if q else "(%s),a") % rp, 7)
            lo = fetch()
            hi = fetch()
            v = lo | hi << 8
            if p == 2:
                return make("ld", ("%s,(%s)" if q else "(%s),%s") % ((hl, hx16(v)) if q else (hx16(v), hl)), 16 + extra, imm=v, abs_addr=v)
            return make("ld", ("a,(%s)" % hx16(v)) if q else ("(%s),a" % hx16(v)), 13, imm=v, abs_addr=v)
        if z == 3:
            return make("dec" if q else "inc", hl if p == 2 else RP[p], 6 + extra)
        if z in (4, 5):
            mem = y == 6
            if mem and idx:
                mem_disp()
            name = _reg(y, idx, disp, mem and bool(idx))
            t = (11 + (12 if idx else 0)) if mem else 4 + extra
            return make("dec" if z == 5 else "inc", name, t)
        if z == 6:
            mem = y == 6
            if mem and idx:
                mem_disp()
            n = fetch()
            name = _reg(y, idx, disp, mem and bool(idx))
            t = (10 + (9 if idx else 0)) if mem else 7 + extra
            return make("ld", "%s,%s" % (name, hx8(n)), t, imm=n)
        names = ["rlca", "rrca", "rla", "rra", "daa", "cpl", "scf", "ccf"]
        return make(names[y])

    if x == 1:
        if op == 0x76:
            return make("halt", "", 4, None, "halt")
        mem = y == 6 or z == 6
        if mem and idx:
            mem_disp()
        dst = _reg(y, idx, disp, mem and bool(idx))
        src = _reg(z, idx, disp, mem and bool(idx))
        if mem:
            t = 7 + (12 if idx else 0)
        else:
            t = 4 + (4 if idx else 0)
        return make("ld", "%s,%s" % (dst, src), t)

    if x == 2:
        mem = z == 6
        if mem and idx:
            mem_disp()
        src = _reg(z, idx, disp, mem and bool(idx))
        return make(ALU[y], ALU_A[y] + src, (7 + (12 if idx else 0)) if mem else 4 + extra)

    # x == 3
    if z == 0:
        return make("ret", CC[y], 5, 11, "retc")
    if z == 1:
        if q == 0:
            return make("pop", hl if p == 2 else RP2[p], 10 + extra)
        if p == 0:
            return make("ret", "", 10, None, "ret")
        if p == 1:
            return make("exx")
        if p == 2:
            return make("jp", "(%s)" % hl, 4 + extra, None, "jpind")
        return make("ld", "sp,%s" % hl, 6 + extra)
    if z == 2:
        lo = fetch()
        hi = fetch()
        v = lo | hi << 8
        return make("jp", "%s,%s" % (CC[y], hx16(v)), 10, None, "jpc", v, imm=v)
    if z == 3:
        if y == 0:
            lo = fetch()
            hi = fetch()
            v = lo | hi << 8
            return make("jp", hx16(v), 10, None, "jp", v, imm=v)
        if y == 2:
            n = fetch()
            return make("out", "(%s),a" % hx8(n), 11, port=n, imm=n)
        if y == 3:
            n = fetch()
            return make("in", "a,(%s)" % hx8(n), 11, port=n, imm=n)
        if y == 4:
            return make("ex", "(sp),%s" % hl, 19 + extra)
        if y == 5:
            return make("ex", "de,hl")
        if y == 6:
            return make("di")
        return make("ei")
    if z == 4:
        lo = fetch()
        hi = fetch()
        v = lo | hi << 8
        return make("call", "%s,%s" % (CC[y], hx16(v)), 10, 17, "callc", v, imm=v)
    if z == 5:
        if q == 0:
            return make("push", hl if p == 2 else RP2[p], 11 + extra)
        lo = fetch()
        hi = fetch()
        v = lo | hi << 8
        return make("call", hx16(v), 17, None, "call", v, imm=v)
    if z == 6:
        n = fetch()
        return make(ALU[y], ALU_A[y] + hx8(n), 7, imm=n)
    return make("rst", hx8(y * 8), 11, None, "rst", y * 8)


def _decode_cb(make, op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    mem = z == 6
    if x == 0:
        return make(ROT[y], R8[z], 15 if mem else 8)
    mn = ["bit", "res", "set"][x - 1]
    return make(mn, "%d,%s" % (y, R8[z]), (12 if x == 1 else 15) if mem else 8)


def _decode_ddcb(make, idx, d, op):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    mem = "(%s%s$%02x)" % (idx, "-" if d & 0x80 else "+", (-sgn(d)) & 0xFF if d & 0x80 else d)
    if x == 1:
        return make("bit", "%d,%s" % (y, mem), 20)
    copy = "" if z == 6 else "," + R8[z]   # undocumented: the result is also stored in register z
    if x == 0:
        return make(ROT[y], mem + copy, 23)
    return make(["res", "set"][x - 2], "%d,%s%s" % (y, mem, copy), 23)


def _decode_ed(make, fetch, p_after):
    op = fetch()
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    if x == 1:
        if z == 0:
            return make("in", "%s,(c)" % ("f" if y == 6 else R8[y]), 12)
        if z == 1:
            return make("out", "(c),%s" % ("0" if y == 6 else R8[y]), 12)
        if z == 2:
            return make("adc" if q else "sbc", "hl,%s" % RP[p], 15)
        if z == 3:
            lo = fetch()
            hi = fetch()
            v = lo | hi << 8
            if q:
                return make("ld", "%s,(%s)" % (RP[p], hx16(v)), 20, imm=v, abs_addr=v)
            return make("ld", "(%s),%s" % (hx16(v), RP[p]), 20, imm=v, abs_addr=v)
        if z == 4:
            return make("neg", "", 8)
        if z == 5:
            return make("reti", "", 14, None, "reti") if y == 1 else make("retn", "", 14, None, "retn")
        if z == 6:
            return make("im", IM_MODE[y], 8)
        names = ["ld i,a", "ld r,a", "ld a,i", "ld a,r", "rrd", "rld", "nop", "nop"]
        n = names[y]
        mn, _, a = n.partition(" ")
        return make(mn, a, 9 if y < 4 else 18 if y < 6 else 8)
    if x == 2 and z <= 3 and y >= 4:
        return make(BLOCK[z][y - 4], "", 16, 21 if y >= 6 else None)   # only the repeating forms (ldir, cpir, inir, otir...) have the 21-T path
    return make("nop", "", 8)   # undefined ED opcode: two-byte NOP


def read_rom(path=None):
    if path is None:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(here)
        cands = [f for f in os.listdir(root) if f.lower().endswith(".gg")]
        path = os.path.join(root, cands[0])
    return open(path, "rb").read()


def slot_offset(bank, addr):
    """file offset of slot address `addr` ($0000-$BFFF) with `bank` mapped in the slot that holds it (first 1 KB always bank 0)"""
    if addr < 0x400:
        return addr
    return bank * 0x4000 + (addr & 0x3FFF)


# ---------------------------------------------------------------------------------------------------------------------------------------------------
# self-test

# (bytes, text, length, t, t2)
VECTORS = [
    ("00", "nop", 1, 4, None), ("01 34 12", "ld bc,$1234", 3, 10, None), ("02", "ld (bc),a", 1, 7, None), ("03", "inc bc", 1, 6, None),
    ("04", "inc b", 1, 4, None), ("06 55", "ld b,$55", 2, 7, None), ("07", "rlca", 1, 4, None), ("08", "ex af,af'", 1, 4, None),
    ("09", "add hl,bc", 1, 11, None), ("0a", "ld a,(bc)", 1, 7, None), ("10 fe", "djnz $0000", 2, 8, 13), ("18 05", "jr $0007", 2, 12, None),
    ("20 05", "jr nz,$0007", 2, 7, 12), ("38 fc", "jr c,$fffe", 2, 7, 12), ("21 00 c0", "ld hl,$c000", 3, 10, None), ("22 00 c0", "ld ($c000),hl", 3, 16, None),
    ("2a 00 c0", "ld hl,($c000)", 3, 16, None), ("27", "daa", 1, 4, None), ("31 ef df", "ld sp,$dfef", 3, 10, None), ("32 0a c0", "ld ($c00a),a", 3, 13, None),
    ("34", "inc (hl)", 1, 11, None), ("35", "dec (hl)", 1, 11, None), ("36 12", "ld (hl),$12", 2, 10, None), ("37", "scf", 1, 4, None),
    ("3a 0a c0", "ld a,($c00a)", 3, 13, None), ("3e 01", "ld a,$01", 2, 7, None), ("40", "ld b,b", 1, 4, None), ("46", "ld b,(hl)", 1, 7, None),
    ("70", "ld (hl),b", 1, 7, None), ("76", "halt", 1, 4, None), ("7e", "ld a,(hl)", 1, 7, None), ("80", "add a,b", 1, 4, None), ("86", "add a,(hl)", 1, 7, None),
    ("90", "sub b", 1, 4, None), ("98", "sbc a,b", 1, 4, None), ("a8", "xor b", 1, 4, None), ("af", "xor a", 1, 4, None), ("b8", "cp b", 1, 4, None),
    ("be", "cp (hl)", 1, 7, None), ("c0", "ret nz", 1, 5, 11), ("c1", "pop bc", 1, 10, None), ("c2 00 10", "jp nz,$1000", 3, 10, None), ("c3 10 01", "jp $0110", 3, 10, None),
    ("c4 00 10", "call nz,$1000", 3, 10, 17), ("c5", "push bc", 1, 11, None), ("c6 05", "add a,$05", 2, 7, None), ("c7", "rst $00", 1, 11, None),
    ("c9", "ret", 1, 10, None), ("cd 00 10", "call $1000", 3, 17, None), ("d3 bf", "out ($bf),a", 2, 11, None), ("d9", "exx", 1, 4, None),
    ("db dc", "in a,($dc)", 2, 11, None), ("e3", "ex (sp),hl", 1, 19, None), ("e9", "jp (hl)", 1, 4, None), ("eb", "ex de,hl", 1, 4, None), ("f1", "pop af", 1, 10, None),
    ("f3", "di", 1, 4, None), ("f9", "ld sp,hl", 1, 6, None), ("fb", "ei", 1, 4, None), ("fe 10", "cp $10", 2, 7, None), ("ff", "rst $38", 1, 11, None),
    ("cb 00", "rlc b", 2, 8, None), ("cb 06", "rlc (hl)", 2, 15, None), ("cb 36", "sll (hl)", 2, 15, None), ("cb 3f", "srl a", 2, 8, None),
    ("cb 47", "bit 0,a", 2, 8, None), ("cb 46", "bit 0,(hl)", 2, 12, None), ("cb 86", "res 0,(hl)", 2, 15, None), ("cb ff", "set 7,a", 2, 8, None),
    ("ed 40", "in b,(c)", 2, 12, None), ("ed 70", "in f,(c)", 2, 12, None), ("ed 41", "out (c),b", 2, 12, None), ("ed 71", "out (c),0", 2, 12, None),
    ("ed 42", "sbc hl,bc", 2, 15, None), ("ed 4a", "adc hl,bc", 2, 15, None), ("ed 43 00 c0", "ld ($c000),bc", 4, 20, None), ("ed 7b 00 c0", "ld sp,($c000)", 4, 20, None),
    ("ed 44", "neg", 2, 8, None), ("ed 45", "retn", 2, 14, None), ("ed 4d", "reti", 2, 14, None), ("ed 55", "retn", 2, 14, None), ("ed 46", "im 0", 2, 8, None),
    ("ed 56", "im 1", 2, 8, None), ("ed 5e", "im 2", 2, 8, None), ("ed 4e", "im 0", 2, 8, None), ("ed 47", "ld i,a", 2, 9, None), ("ed 57", "ld a,i", 2, 9, None),
    ("ed 67", "rrd", 2, 18, None), ("ed 6f", "rld", 2, 18, None), ("ed a0", "ldi", 2, 16, None), ("ed b0", "ldir", 2, 16, 21), ("ed b8", "lddr", 2, 16, 21),
    ("ed a1", "cpi", 2, 16, None), ("ed b3", "otir", 2, 16, 21), ("ed a3", "outi", 2, 16, None), ("ed b2", "inir", 2, 16, 21), ("ed 00", "nop", 2, 8, None),
    ("ed ff", "nop", 2, 8, None), ("ed a4", "nop", 2, 8, None),
    ("dd 21 34 12", "ld ix,$1234", 4, 14, None), ("fd 21 34 12", "ld iy,$1234", 4, 14, None), ("dd 22 00 c0", "ld ($c000),ix", 4, 20, None),
    ("dd 2a 00 c0", "ld ix,($c000)", 4, 20, None), ("dd 23", "inc ix", 2, 10, None), ("dd 09", "add ix,bc", 2, 15, None), ("dd 29", "add ix,ix", 2, 15, None),
    ("dd 34 05", "inc (ix+$05)", 3, 23, None), ("dd 35 fb", "dec (ix-$05)", 3, 23, None), ("dd 36 05 99", "ld (ix+$05),$99", 4, 19, None),
    ("dd 46 05", "ld b,(ix+$05)", 3, 19, None), ("dd 66 05", "ld h,(ix+$05)", 3, 19, None), ("dd 74 05", "ld (ix+$05),h", 3, 19, None), ("dd 77 05", "ld (ix+$05),a", 3, 19, None),
    ("dd 7e 05", "ld a,(ix+$05)", 3, 19, None), ("dd 86 05", "add a,(ix+$05)", 3, 19, None), ("dd be 05", "cp (ix+$05)", 3, 19, None),
    ("dd e1", "pop ix", 2, 14, None), ("dd e5", "push ix", 2, 15, None), ("dd e3", "ex (sp),ix", 2, 23, None), ("dd e9", "jp (ix)", 2, 8, None), ("fd e9", "jp (iy)", 2, 8, None),
    ("dd f9", "ld sp,ix", 2, 10, None), ("dd 26 12", "ld ixh,$12", 3, 11, None), ("dd 2e 12", "ld ixl,$12", 3, 11, None), ("dd 24", "inc ixh", 2, 8, None),
    ("dd 7c", "ld a,ixh", 2, 8, None), ("dd 65", "ld ixh,ixl", 2, 8, None), ("dd 84", "add a,ixh", 2, 8, None), ("fd 6d", "ld iyl,iyl", 2, 8, None),
    ("dd 00", "dd prefix (ignored)", 1, 4, None), ("dd eb", "dd prefix (ignored)", 1, 4, None), ("dd dd 21 00 00", "dd prefix (ignored)", 1, 4, None), ("dd ed 44", "dd prefix (ignored)", 1, 4, None),
    ("dd cb 05 06", "rlc (ix+$05)", 4, 23, None), ("dd cb 05 00", "rlc (ix+$05),b", 4, 23, None), ("dd cb fb 46", "bit 0,(ix-$05)", 4, 20, None),
    ("fd cb 05 86", "res 0,(iy+$05)", 4, 23, None), ("dd cb 05 c7", "set 0,(ix+$05),a", 4, 23, None), ("dd cb 05 47", "bit 0,(ix+$05)", 4, 20, None),
]


def selftest():
    bad = 0
    for hexs, text, length, t, t2 in VECTORS:
        data = bytes.fromhex(hexs.replace(" ", "")) + b"\x00" * 4
        i = decode(lambda a: data[a], 0)
        got = (i.text, i.length, i.t, i.t2)
        if got != (text, length, t, t2):
            bad += 1
            print("FAIL", hexs, "got", got, "want", (text, length, t, t2))
    # control flow fields
    i = decode(lambda a: bytes.fromhex("10fe")[a - 0x100], 0x100)
    assert (i.kind, i.target) == ("djnz", 0x100), i
    i = decode(lambda a: bytes.fromhex("cd3412")[a], 0)
    assert (i.kind, i.target) == ("call", 0x1234), i
    i = decode(lambda a: bytes.fromhex("f7")[a], 0)
    assert (i.kind, i.target) == ("rst", 0x30), i
    i = decode(lambda a: bytes.fromhex("d3bf")[a], 0)
    assert i.port == 0xBF
    # coverage: every opcode in every space decodes, lengths sane, and the length does not depend on trailing bytes
    n = 0
    for pre in ([], [0xCB], [0xED], [0xDD], [0xFD], [0xDD, 0xCB, 0x01], [0xFD, 0xCB, 0x01]):
        for op in range(256):
            for fill in (0x00, 0xFF):
                data = bytes(pre + [op]) + bytes([fill] * 6)
                i = decode(lambda a: data[a], 0)
                assert 1 <= i.length <= 4 and i.t >= 4, (pre, op, i)
                if i.length != len(i.raw):
                    bad += 1
                n += 1
    # total length of the 256 unprefixed opcodes (known value: 3 x 3-byte groups etc. is checked indirectly): count of 1/2/3-byte opcodes
    cnt = {1: 0, 2: 0, 3: 0}
    for op in range(256):
        if op in (0xCB, 0xDD, 0xED, 0xFD):
            continue
        data = bytes([op, 0, 0, 0])
        cnt[decode(lambda a: data[a], 0).length] += 1
    # 2-byte: 8 x LD r,n + 8 x ALU n + DJNZ + JR + 4 JR cc + IN/OUT (n) = 8+8+1+1+4+2 = 24 ; 3-byte: 4 LD rp,nn + LD (nn),HL/A + LD HL/A,(nn) + 8 JP cc + JP + 8 CALL cc + CALL = 4+4+8+1+8+1 = 26
    assert cnt == {1: 252 - 24 - 26, 2: 24, 3: 26}, cnt
    print("z80_dis selftest: %d vectors, %d opcode-space checks, %s" % (len(VECTORS), n, "ALL OK" if not bad else "%d FAILURES" % bad))
    return bad == 0


def main():
    args = sys.argv[1:]
    if "--selftest" in args:
        sys.exit(0 if selftest() else 1)
    rom_path = None
    if args and args[0].lower().endswith(".gg"):
        rom_path = args.pop(0)
    rom = read_rom(rom_path)
    if not args:
        print(__doc__)
        return
    bank_s, _, addr_s = args[0].partition(":")
    bank, addr = int(bank_s, 0), int(addr_s, 0)
    count = int(args[1], 0) if len(args) > 1 else 32
    rd = lambda a: rom[slot_offset(bank, a & 0xFFFF)] if (a & 0xFFFF) < 0xC000 else 0xFF
    a = addr
    for _ in range(count):
        i = decode(rd, a)
        print("%d:%04x  %-12s %s" % (bank, a, i.raw.hex(" "), i.text))
        a = (a + i.length) & 0xFFFF


if __name__ == "__main__":
    main()
