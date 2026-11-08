"""M1 gap analysis: what the static descent did not cover.

    python tools/gaps.py [ROM.gg]       # reads extracted/rom/insns.txt (tools/survey.py), writes extracted/rom/gaps.txt

Two lists for the oracle (M2) and the annotations to settle:
  1. code-like gaps: uncovered byte ranges that decode as a plausible instruction run (>= 8 instructions ending in jp / jr / ret, no `rst $38` floods,
     no undefined ED opcodes, no ignored prefixes) AND anchored: a call / jp into known code, a reference to RAM ($C000-$DFFF), or a use of a known port.
     A heuristic: it finds candidates, it proves nothing.
  2. pointer tables: runs of >= 3 little-endian 16-bit words in $0100-$7fff (>= 3 distinct, none inside an already decoded instruction), each pointing at a
     known instruction start or at an anchored code-like run, at an address that code already reached loads with `ld rp,nn`. Targets the descent has not reached
     are the seeds it was missing. (Slot-2 targets are not checked: their bank is unknown.) These are the
     jump-table / handler-table seeds that static descent cannot see. Slot-2 targets (>= $8000) are checked against the instructions of the bank they sit in.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import z80_dis as dis  # noqa: E402

ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "extracted", "rom")
BANK = 0x4000
BAD_MN = ("dd prefix (ignored)", "fd prefix (ignored)")


def cpu(ph):
    b = ph // BANK
    return (ph & 0x3FFF) + (0x0000 if b == 0 else 0x4000 if b == 1 else 0x8000)


def anchored(run, starts_cpu):
    """evidence that a decoded run is code and not data that happens to decode: it calls / jumps to known code, touches RAM or uses a port"""
    for _, i in run:
        if i.kind in ("call", "jp") and i.target is not None and i.target in starts_cpu:
            return True
        if i.abs_addr is not None and 0xC000 <= i.abs_addr < 0xE000:
            return True
        if i.port is not None and i.port in (0xBE, 0xBF, 0x7F, 0x00, 0xDC, 0xDD, 0x3E, 0x3F):
            return True
    return False


def plausible(i):
    if i.text in BAD_MN or i.mn.endswith("prefix (ignored)"):
        return False
    if i.mn == "rst" and i.args == "$38":
        return False
    if i.mn == "nop" and i.length == 2:       # undefined ED opcode
        return False
    if i.mn in ("halt",):
        return False
    if i.mn == "ld" and i.args in ("b,b", "c,c", "d,d", "e,e", "h,h", "l,l", "a,a"):
        return False
    return True


def main():
    args = sys.argv[1:]
    rom = dis.read_rom(args[0] if args else None)
    nb = len(rom) // BANK
    starts = {}
    covered = bytearray(len(rom))
    for line in open(os.path.join(OUT, "insns.txt")):
        ph, pc, ln, text = line.split(" ", 3)
        ph, ln = int(ph, 16), int(ln)
        starts[ph] = (int(pc, 16), ln, text.strip())
        for k in range(ln):
            covered[ph + k] = 1

    def rd(a):
        return rom[a] if 0 <= a < len(rom) else 0xFF

    starts_cpu = {pc for pc, _, _ in starts.values() if pc < 0x8000}
    # 1. code-like gaps (decode in file-offset space; a branch target is only a hint and is shown as written)
    out = ["# code-like gaps (uncovered, >= 8 plausible instructions, ending in jp / jr / ret, anchored). file offset, CPU address, bytes, first instructions"]
    cands = []
    o = 0
    while o < len(rom):
        if covered[o]:
            o += 1
            continue
        # decode a run starting here
        p = o
        run = []
        ok = True
        while p < len(rom) and not covered[p] and p // BANK == o // BANK:
            # decode with addresses relative to the slot so that branch targets look right
            i = dis.decode(lambda a: rd(a - cpu(o) + o), cpu(o) + (p - o))
            if not plausible(i) or p + i.length > len(rom) or any(covered[p + k] for k in range(i.length)):
                ok = False
                break
            run.append((p, i))
            p += i.length
            if i.kind in ("jp", "jr", "ret", "reti", "retn", "jpind"):
                break
        if ok and len(run) >= 8 and run[-1][1].kind in ("jp", "jr", "ret", "reti", "retn", "jpind") and anchored(run, starts_cpu):
            cands.append((o, p, run))
            o = p
        else:
            o += 1
    total = 0
    for o, p, run in cands:
        total += p - o
        out.append("%05x  $%04x  %3d bytes  %s" % (o, cpu(o), p - o, " | ".join(i.text for _, i in run[:5]) + (" ..." if len(run) > 5 else "")))
    out.append("# %d candidates, %d bytes" % (len(cands), total))
    cand_starts = {o for o, _, _ in cands}

    # 2. pointer tables
    out.append("")
    out.append("# pointer tables: >= 3 consecutive LE words ($0100-$7fff, >= 3 distinct, none inside an already decoded instruction) at an address that reached code loads with `ld rp,nn`; each word is a known instruction start or an anchored code-like run. Not-yet-covered entries are new seeds.")
    known_cpu = {}
    for ph, (pc, ln, text) in starts.items():
        known_cpu.setdefault(pc, set()).add(ph)
    code_cpu = set(known_cpu)

    cache = {}

    def code_like_at(v):
        """does the fixed-region address v decode as a plausible run (>= 4 instructions, ends in jp / jr / ret within 60) that is anchored?"""
        if v in cache:
            return cache[v]
        res = False
        run = []
        pc = v
        while pc < 0x8000 and len(run) < 60:
            i = dis.decode(lambda a: rd(a), pc)
            if not plausible(i):
                break
            run.append((pc, i))
            pc += i.length
            if i.kind in ("jp", "jr", "ret", "reti", "retn", "jpind"):
                res = len(run) >= 4 and anchored(run, starts_cpu)
                break
        cache[v] = res
        return res

    def good(v):
        if v < 0x100 or v >= 0x8000:
            return False
        if v in code_cpu:
            return True
        if covered[v]:      # inside an instruction the descent already decoded
            return False
        return code_like_at(v)

    # data addresses that reached code loads with `ld rp, nn`: a table is only believed when code points at it
    refs = {}
    for ph, (pc, ln, text) in starts.items():
        if text.startswith("ld ") and "," in text and text.split()[1].split(",")[0] in ("hl", "de", "bc", "ix", "iy") and "(" not in text:
            v = int(text.split("$")[-1], 16)
            if 0x100 <= v < 0x8000 and not covered[v]:
                refs.setdefault(v, []).append(pc)
    tables = 0
    for r in sorted(refs):
        run = []
        p = r
        while p + 2 <= len(rom) and p < 0x8000 and len(run) < 48:
            v = rom[p] | rom[p + 1] << 8
            if not good(v):
                break
            run.append(v)
            p += 2
        if len(run) >= 3 and len(set(run)) >= 3:
            tables += 1
            new = [v for v in run if v not in code_cpu]
            out.append("%05x  $%04x  %2d entries (%d not yet covered)  loaded at %s  %s" % (
                r, r, len(run), len(new), " ".join("$%04x" % x for x in refs[r][:4]), " ".join("$%04x" % v for v in run[:12]) + (" ..." if len(run) > 12 else "")))
    out.append("# %d tables" % tables)
    # per-bank summary: covered by the descent + heuristic code-like gap bytes, against the bank's byte statistics
    import collections
    import math
    gapbytes = collections.Counter()
    for o, p, run in cands:
        gapbytes[o // BANK] += p - o
    out.append("")
    out.append("# per-bank summary (covered = decoded by the descent; gap = heuristic code-like bytes outside it; class is a guess for the oracle to confirm)")
    out.append("bank  covered  gap   %code  entropy  %00   %ff   class")
    for b in range(nb):
        d = rom[b * BANK:(b + 1) * BANK]
        cov = sum(covered[b * BANK:(b + 1) * BANK])
        c = collections.Counter(d)
        ent = -sum(v / len(d) * math.log2(v / len(d)) for v in c.values())
        pct = 100.0 * (cov + gapbytes[b]) / BANK
        cls = "code (+data)" if pct >= 50 else "mixed" if pct >= 20 else "data (graphics / tables / music)"
        out.append("%2d    %6d  %5d  %4.0f%%  %5.2f   %4.1f %4.1f   %s" % (b, cov, gapbytes[b], pct, ent, 100.0 * d.count(0) / BANK, 100.0 * d.count(255) / BANK, cls))
    open(os.path.join(OUT, "gaps.txt"), "w").write("\n".join(out) + "\n")
    print("code-like gaps: %d (%d bytes), pointer tables: %d" % (len(cands), total, tables))


if __name__ == "__main__":
    main()
