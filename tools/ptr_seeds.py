"""Pointer-table finder for the translator: code that is only reached through a table of code addresses (handler tables, state tables, tables of tables).

    python tools/ptr_seeds.py            # prints what it finds on top of the translator's current seeds

A table is a run of at least 3 consecutive little-endian words, at least 3 distinct, every word of which is a code address: the start of an instruction the translator
already has, or the start of a plausible run (at least 4 instructions, no undefined / ignored opcodes, ends in jp / jr / ret within 60 instructions) that is anchored
(it calls or jumps into known code, touches RAM $C000-$DFFF or uses a known port). The search covers the ROM positions the translated code does not occupy, in the
fixed region ($0100-$7FFF) and in the slot-2 code bank (CPU $8000-$BFFF); target addresses are CPU addresses in the same space. The translator calls find() with its
seeds, translates what is new, and repeats until nothing more is found (tables reached only from newly found code, targets that become anchored by new code).
A false positive costs only some unused C code; the miss that the table would have produced at run time is the thing being avoided (NOTES.md "M7").
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gaps  # noqa: E402
import z80_dis as dis  # noqa: E402

BANK = 0x4000
TERMINATORS = ("jp", "jr", "ret", "reti", "retn", "jpind")


def find(rom, covered, starts, slot2_bank, phys_of, min_len=3, ranges=((0x100, 0x8000), (0x8000, 0xC000)), rd=None):
    """covered: set of ROM file offsets occupied by translated instructions; starts: set of CPU addresses where translated instructions start.
    Returns (set of new seed CPU addresses, list of (table CPU address, [targets]))."""
    code_lo = min(a for a, b in ranges)
    code_hi = max(b for a, b in ranges)
    if rd is None:
        def rd(a):
            ph = phys_of(a & 0xFFFF)
            return rom[ph] if ph is not None and ph < len(rom) else 0xFF

    cache = {}

    def run_at(v):
        """the plausible instruction run from CPU address v: (instructions, ends in a terminator)"""
        run = []
        pc = v
        while len(run) < 400 and pc < code_hi:
            i = dis.decode(rd, pc)
            if not gaps.plausible(i):
                return run, False
            run.append((pc, i))
            pc += i.length
            if i.kind in TERMINATORS:
                return run, True
        return run, False

    def classify(v):
        """2 = clearly code (a known instruction start, or an anchored run of >= 4 instructions), 1 = plausible code (a run of >= 2 instructions ending in a terminator),
        0 = not code"""
        if v in cache:
            return cache[v]
        res = 0
        if code_lo <= v < code_hi:
            ph = phys_of(v)
            if v in starts:
                res = 2
            elif ph is not None and ph not in covered and rd(v) != 0:      # a routine does not start with a nop: zero-filled data decodes as a run of them
                run, ended = run_at(v)
                if ended and len(run) >= 4 and gaps.anchored(run, starts):
                    res = 2
                elif ended and len(run) >= 2:
                    res = 1
        cache[v] = res
        return res

    new_seeds, tables = set(), []
    positions = [a for lo, hi in ranges for a in range(lo, hi)]
    skip_until = [0, 0]                   # per alignment: a table is searched at both byte parities, and one parity may hold the real table
    for p in positions:
        if p < skip_until[p & 1]:
            continue
        ph = phys_of(p)
        if ph is None or ph in covered or phys_of(p + 1) is None or phys_of(p + 1) in covered:
            continue
        if rd(p) | rd(p + 1) << 8 == 0:
            continue
        run = []
        q = p
        q_end = p                           # just after the last real entry
        zeros = 0
        while q + 1 < code_hi and len(run) < 96:
            if phys_of(q) in covered or phys_of(q + 1) in covered:
                break
            v = rd(q) | rd(q + 1) << 8
            if v == 0:                      # tables of per-state / per-level handlers have empty slots: up to 4 in a row do not end the table
                zeros += 1
                if zeros > 4:
                    break
                q += 2
                continue
            if classify(v) == 0:
                break
            zeros = 0
            run.append(v)
            q += 2
            q_end = q
        q = q_end
        strong = sum(1 for v in run if classify(v) == 2)
        if len(run) >= 4 and len(set(run)) >= 4 and strong >= max(3, len(run) // 3):
            tables.append((p, run))
            new_seeds.update(v for v in run if v not in starts)
            skip_until[p & 1] = q
    return new_seeds, tables


def find_tails(rom, covered, insns, phys_of, ranges=((0x100, 0x8000), (0x8000, 0xC000))):
    """Short routines that nothing translated reaches (helpers called from code the survey never saw): candidate starts are the address right after a translated
    unconditional jp / jr / ret (a routine that follows another) and the start of every untranslated stretch; kept when a run of at least 3 plausible instructions
    that ends in jp / jr / ret and is anchored (calls known code, touches RAM or a known port) begins there. insns: {pc: translator Ins}. Returns a set of CPU addresses."""
    def rd(a):
        ph = phys_of(a & 0xFFFF)
        return rom[ph] if ph is not None and ph < len(rom) else 0xFF

    starts = set(insns)
    cands = set()
    for pc, ins in insns.items():
        if ins.di.kind in TERMINATORS:
            n = (pc + ins.length) & 0xFFFF
            if any(lo <= n < hi for lo, hi in ranges) and phys_of(n) not in covered:
                cands.add(n)
    for lo, hi in ranges:
        a = lo
        while a < hi:
            if phys_of(a) in covered:
                a += 1
                continue
            cands.add(a)
            while a < hi and phys_of(a) not in covered:
                a += 1
    out = set()
    for c in cands:
        run, pc, ended = [], c, False
        while len(run) < 80 and pc < 0xC000:
            i = dis.decode(rd, pc)
            if not gaps.plausible(i):
                break
            run.append((pc, i))
            pc += i.length
            if i.kind in TERMINATORS:
                ended = True
                break
        if ended and len(run) >= 3 and gaps.anchored(run, starts):
            out.add(c)
    return out


def find_immediates(rom, covered, insns, phys_of, ranges=((0x100, 0x8000), (0x8000, 0xC000))):
    """Code addresses that are loaded as a 16-bit immediate (ld hl,$9958 ... a callback stored in RAM, or reached through jp (hl)): the value of every translated
    ld rr,nn that lies in an untranslated stretch and starts a plausible run (at least 2 instructions, ends in jp / jr / ret, anchored or at least 4 instructions
    long). insns: {pc: translator Ins}. Returns a set of CPU addresses."""
    def rd(a):
        ph = phys_of(a & 0xFFFF)
        return rom[ph] if ph is not None and ph < len(rom) else 0xFF

    starts = set(insns)
    out = set()
    seen = set()
    for pc, ins in insns.items():
        di = ins.di
        if di.mn != "ld" or di.imm is None or di.abs_addr is not None or di.length != 3 and di.length != 4:
            continue
        if di.args.split(",")[0] not in ("hl", "de", "bc", "ix", "iy"):
            continue
        v = di.imm & 0xFFFF
        if v in seen or v in starts or not any(lo <= v < hi for lo, hi in ranges) or phys_of(v) in covered or rd(v) == 0:
            continue
        seen.add(v)
        run, q, ended = [], v, False
        while len(run) < 80 and q < 0xC000:
            i = dis.decode(rd, q)
            if not gaps.plausible(i):
                break
            run.append((q, i))
            q += i.length
            if i.kind in TERMINATORS:
                ended = True
                break
        if ended and len(run) >= 2 and (len(run) >= 4 or gaps.anchored(run, starts)):
            out.add(v)
    return out


if __name__ == "__main__":
    import z80_to_c as t
    rom = dis.read_rom()
    seeds = t.load_rom_seeds(rom)
    seeds |= {0, 0x38, 0x66}      # solid seeds only: code the gap heuristic invented may overlap the tables
    insns = t.discover(rom, seeds)
    covered = set()
    for pc, ins in insns.items():
        ph = t.phys_of(pc)
        for k in range(ins.length):
            covered.add(ph + k)
    new, tables = find(rom, covered, set(insns), t.SLOT2_CODE_BANK, t.phys_of)
    print("tables: %d, new seeds: %d" % (len(tables), len(new)))
    for p, run in tables[:60]:
        print("  $%04x  %2d words  %s" % (p, len(run), " ".join("%04x" % v for v in run[:10]) + (" ..." if len(run) > 10 else "")))
    for miss in (0x2b8c, 0x2c34, 0x2cb0, 0x2d4b):
        print("miss $%04x found: %s" % (miss, miss in new or miss in insns))
