"""M1 ROM survey: bank statistics and a bank-aware recursive-descent code map.

    python tools/survey.py [ROM.gg]     # writes extracted/rom/{bank_stats.txt, bank_map.txt, code_map.json, insns.txt, unresolved.txt, listing_bankNN.txt}

Memory model (verified in NOTES.md "M1 results"): the game writes the mapper registers `$FFFD` = 0 and `$FFFE` = 1 once (init at `$0122` / `$012C`), `$FFFC` = 0
once (no cart RAM) and `$FFFF` at 11 more places. So `$0000-$7FFF` is always banks 0 and 1 (flat file offsets) and only slot 2 (`$8000-$BFFF`) switches.
Code locations are (phys, pc): `phys` is the file offset, `pc` the CPU address it ran at.

The descent follows states (pc, slot-2 bank, A): a constant tracker follows `ld a, imm` / `xor a` and applies the slot-2 bank to `ld ($ffff), a` and to calls of small
"bank setter" routines (found automatically: a straight-line routine that stores A to `$FFFF` before any other use of A). Assumption (checked later by the oracle's
coverage trace): a called routine leaves the slot-2 bank as it found it unless it is such a setter. The game keeps a "base" slot-2 bank in RAM `$C011` (set by `ld ($c011),a`, restored by routines that reload it); it is modelled as the symbolic bank "B" whose
possible values are collected over the whole run (set of constants stored to `$C011`) and the descent is repeated until that set stops growing.
Calls into `$C000+` are code in RAM: listed, not followed.
Hand annotations (tools/annotations.txt: handler tables, extra entry points, each with its evidence) are added as seeds.
Anything the tracker cannot resolve (slot-2 code reached with an unknown bank, `jp (hl)`, ...) goes to unresolved.txt, never guessed.
"""
import collections
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import z80_dis as dis  # noqa: E402

ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "extracted", "rom")
BANK = 0x4000
MAPPER = {0xFFFC: "ctrl", 0xFFFD: "slot0", 0xFFFE: "slot1", 0xFFFF: "slot2"}
# Hand annotations (each one verified in the listing, see NOTES.md "M1 results"):
#   $0850 enqueues the 16-bit code address in HL (it starts with `ex de,hl`) on the game's task queue ($C21A = end pointer, queue at $C21C); $0839 pops and
#   runs the entries with `push hl; jp (hl)`. The handler addresses are the constants loaded into HL before `call $0850`.
ENQUEUE_HL = {0x0850}
SEEDS = [(0x0000, 2, "reset"), (0x0038, None, "irq (im 1)"), (0x0066, None, "nmi")] + [(v, None, "rst $%02x" % v) for v in (0x08, 0x10, 0x18, 0x20, 0x28, 0x30)]


def load_annotations(rom):
    """seeds from tools/annotations.txt: [(pc, bank spec, why)]; bank spec is "B", an int, or None (fixed region)"""
    path = os.path.join(HERE, "annotations.txt")
    seeds = []
    if not os.path.exists(path):
        return seeds

    def spec(w):
        return "B" if w == "B" else None if w == "-" else int(w, 0)
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.split("#")[0].split()
        if not line:
            continue
        if line[0] == "table" and len(line) == 4:
            base, count, bank = int(line[1], 0), int(line[2], 0), spec(line[3])
            for k in range(count):
                v = rom[base + 2 * k] | rom[base + 2 * k + 1] << 8
                if v >= 0x100:
                    seeds.append((v, bank, "annotation table $%04x[%d] (annotations.txt:%d)" % (base, k, n)))
        elif line[0] == "seed" and len(line) == 3:
            seeds.append((int(line[1], 0), spec(line[2]), "annotation seed (annotations.txt:%d)" % n))
        else:
            raise SystemExit("annotations.txt:%d: cannot parse %r" % (n, line))
    return seeds


def entropy(b):
    if not b:
        return 0.0
    c = collections.Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def bank_stats(rom):
    lines = ["bank  entropy  %00   %ff   distinct  first16"]
    for b in range(len(rom) // BANK):
        d = rom[b * BANK:(b + 1) * BANK]
        lines.append("%2d    %5.2f   %4.1f %4.1f   %3d       %s" % (b, entropy(d), 100 * d.count(0) / len(d), 100 * d.count(255) / len(d), len(set(d)), d[:16].hex(" ")))
    return lines


def touches_hl(i):
    """does the instruction (possibly) change H, L or HL?  (conservative: calls and rsts count)"""
    mn, a = i.mn, i.args
    if i.kind in ("call", "callc", "rst"):
        return True
    if mn in ("ld", "in"):
        return a.split(",")[0] in ("h", "l", "hl")
    if mn in ("inc", "dec", "pop", "rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"):
        return a in ("h", "l", "hl")
    if mn in ("res", "set"):
        return a.split(",")[1] in ("h", "l") if "," in a else False
    if mn in ("add", "adc", "sbc") and a.startswith("hl,"):
        return True
    if mn in ("ex", "exx", "ldi", "ldd", "ldir", "lddr", "cpi", "cpd", "cpir", "cpdr", "ini", "ind", "inir", "indr", "outi", "outd", "otir", "otdr", "rrd", "rld"):
        return not (mn == "ex" and a == "af,af'")
    return False


def writes_a(i):
    """does the instruction (possibly) change A?  (conservative: True when unsure)"""
    mn, a = i.mn, i.args
    if mn in ("add", "adc", "sub", "sbc", "and", "or", "xor"):
        return True
    if mn == "ld":
        return a.startswith("a,")
    if mn in ("inc", "dec"):
        return a == "a"
    if mn in ("cpl", "neg", "daa", "rlca", "rrca", "rla", "rra", "rrd", "rld"):
        return True
    if mn in ("rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"):
        return a == "a" or a.endswith(",a")
    if mn in ("res", "set"):
        return a.endswith(",a")
    if mn == "pop":
        return a == "af"
    if mn == "ex":
        return a == "af,af'"
    if mn == "in":
        return a.startswith("a,")
    if i.kind in ("call", "callc", "rst"):
        return True
    if mn in ("ldir", "lddr", "ldi", "ldd", "cpi", "cpd", "cpir", "cpdr", "ini", "ind", "inir", "indr", "otir", "otdr", "outi", "outd"):
        return mn.startswith("cp")
    if mn.startswith("dd prefix") or mn.startswith("fd prefix"):
        return False
    return False


class Survey:
    def __init__(self, rom):
        self.rom = rom
        self.nbanks = len(rom) // BANK
        self.code = {}                                   # (phys, pc) -> Insn
        self.visited = set()
        self.setter_cache = {}
        self.mapper_writes = []                          # (phys, pc, reg, how, bank or None)
        self.unresolved = []                             # (kind, phys/pc, text)
        self.ram_calls = collections.OrderedDict()       # target -> [site pcs]
        self.ports = collections.defaultdict(list)       # (mn, port) -> [pc]
        self.cports = collections.defaultdict(list)      # mn -> [pc]  (in/out/block io through C)
        self.indirect = []                               # (pc, text)
        self.sites = collections.defaultdict(list)       # misc instruction classes -> [pc]
        self.slot2_calls = collections.defaultdict(set)  # (target, bank) -> {site pcs}
        self.targets = set()                             # (phys, pc) of every branch / call target
        self.queue_seeds = {}                            # handler address -> [enqueue site pcs]
        self.setter_body = set()                         # pcs inside bank-setter routines (A is their parameter there)
        self.base = {2}                                  # possible values of the base slot-2 bank ($C011); 2 is installed by the init code at $0139
        self.base_sources = collections.defaultdict(set)  # base value -> {pcs that store it}

    # --- address model --------------------------------------------------------------------------------------------------------------------------
    def phys(self, pc, b2):
        if pc < 0x8000:
            return pc
        if pc < 0xC000 and b2 is not None and b2 < self.nbanks:
            return b2 * BANK + (pc - 0x8000)
        return None

    def rd_at(self, b2):
        rom = self.rom
        n = len(rom)

        def rd(a):
            a &= 0xFFFF
            if a < 0x8000:
                return rom[a] if a < n else 0xFF
            if a < 0xC000 and b2 is not None:
                o = b2 * BANK + (a - 0x8000)
                return rom[o] if o < n else 0xFF
            return 0xFF
        return rd

    # --- bank setter detection -------------------------------------------------------------------------------------------------------------------
    def setter_kind(self, target):
        """(kind, info): ('set', writes_base) if the routine at `target` stores A (unchanged) to $FFFF (writes_base: it also stores A to $C011),
        ('load', ram_addr) if it stores a value loaded from RAM, else (None, None)"""
        if target in self.setter_cache:
            return self.setter_cache[target]
        res = (None, None)
        if target < 0x8000:
            rd = self.rd_at(None)
            pc = target
            a_ok = True
            loaded = None
            base = False
            for _ in range(12):
                i = dis.decode(rd, pc)
                if i.kind != "seq":
                    break
                self.setter_body.add(pc)
                if i.mn == "ld" and i.args.endswith(",a") and i.abs_addr == 0xFFFF:
                    if loaded is not None:
                        res = ("load", loaded)
                    elif a_ok:
                        res = ("set", base)
                    break
                if i.mn == "ld" and i.args.endswith(",a") and i.abs_addr == 0xC011:
                    base = True
                if writes_a(i):
                    if i.mn == "ld" and i.args.startswith("a,(") and i.abs_addr is not None and loaded is None:
                        loaded = i.abs_addr
                    else:
                        a_ok = False
                pc += i.length
        self.setter_cache[target] = res
        return res

    # --- descent ---------------------------------------------------------------------------------------------------------------------------------
    def reset_collectors(self):
        self.code = {}
        self.visited = set()
        self.mapper_writes = []
        self.unresolved = []
        self.ram_calls = collections.OrderedDict()
        self.ports = collections.defaultdict(list)
        self.cports = collections.defaultdict(list)
        self.indirect = []
        self.sites = collections.defaultdict(list)
        self.slot2_calls = collections.defaultdict(set)
        self.targets = set()
        self.slot2_unknown = {}                          # pc -> why: slot-2 code reached with an unknown bank
        self.slot2_ok = set()                            # pcs of slot-2 code reached with a known bank

    def run_all(self):
        """repeat the descent until the set of possible base banks stops growing"""
        rounds = 0
        while True:
            before = set(self.base)
            self.reset_collectors()
            self.run()
            rounds += 1
            if self.base == before:
                break
        for pc, why in sorted(self.slot2_unknown.items()):
            if pc not in self.slot2_ok:
                self.unresolved.append(("slot2-code-unknown-bank", pc, "reached $%04x with slot-2 bank unknown (%s)" % (pc, why)))
        return rounds

    def note_unresolved(self, kind, pc, text):
        if pc not in self.setter_body:
            self.unresolved.append((kind, pc, text))

    def run(self):
        work = [(pc, b2, None, why) for pc, b2, why in SEEDS + load_annotations(self.rom)]
        while work:
            pc, b2, a, why = work.pop()
            hist = []
            while True:
                if b2 == "B":   # the base bank: every value it can take
                    for v in sorted(self.base):
                        work.append((pc, v, a, why))
                    break
                key = (pc, b2, a)
                if key in self.visited:
                    break
                self.visited.add(key)
                ph = self.phys(pc, b2)
                if ph is None:
                    if pc >= 0xC000:
                        self.ram_calls.setdefault(pc, []).append(-1)
                    else:
                        self.slot2_unknown[pc] = why
                    break
                if pc >= 0x8000:
                    self.slot2_ok.add(pc)
                i = dis.decode(self.rd_at(b2), pc)
                self.code[(ph, pc)] = i
                self.classify(i, ph)
                hist = (hist + [i])[-10:]
                nb2, na = b2, a
                # constant tracker for A
                if i.mn == "ld" and i.args.startswith("a,") and i.imm is not None and i.abs_addr is None and not i.args.endswith(")"):
                    na = i.imm
                elif (i.mn == "xor" or i.mn == "sub") and i.args == "a":
                    na = 0
                elif i.mn in ("inc", "dec") and i.args == "a" and a is not None:
                    na = (a + (1 if i.mn == "inc" else -1)) & 0xFF
                elif writes_a(i):
                    na = None
                if i.mn == "ld" and i.args.endswith(",a") and i.abs_addr == 0xC011:
                    if a is None:
                        self.note_unresolved("base-bank-unknown-value", pc, "ld ($c011),a with A not a known constant")
                    else:
                        self.base.add(a)
                        self.base_sources[a].add(pc)
                if i.mn == "ld" and i.abs_addr in MAPPER and i.args.endswith(",a"):
                    reg = MAPPER[i.abs_addr]
                    self.mapper_writes.append((ph, pc, reg, "ld (nn),a", a))
                    if reg == "slot2":
                        nb2 = a
                        if a is None:
                            self.note_unresolved("bank-write-unknown-value", pc, "ld ($ffff),a with A not a known constant")
                nxt = (pc + i.length) & 0xFFFF
                k = i.kind
                if k in ("call", "callc"):
                    t = i.target
                    kind, info = self.setter_kind(t) if t < 0x8000 else (None, None)
                    if t in ENQUEUE_HL:
                        self.enqueue_site(pc, hist[:-1], b2, work)
                    if t >= 0xC000:
                        self.ram_calls.setdefault(t, []).append(pc)
                    else:
                        if t >= 0x8000:
                            self.slot2_calls[(t, b2)].add(pc)
                        work.append((t, b2, None, "call from $%04x with bank %s" % (pc, b2)))
                        self.targets.add((self.phys(t, b2) if t >= 0x8000 else t, t))
                    if kind == "set":
                        self.mapper_writes.append((ph, pc, "slot2", "call setter $%04x" % t, a))
                        nb2 = a
                        if a is None:
                            self.note_unresolved("bank-setter-unknown-value", pc, "call $%04x (bank setter) with A not a known constant" % t)
                        if info:   # it also stores A as the base bank
                            if a is None:
                                self.note_unresolved("base-bank-unknown-value", pc, "call $%04x (base bank setter) with A not a known constant" % t)
                            else:
                                self.base.add(a)
                                self.base_sources[a].add(pc)
                    elif kind == "load":
                        nb2 = "B" if info == 0xC011 else None   # restores the base bank / the last bank used from RAM
                        self.mapper_writes.append((ph, pc, "slot2", "call restore $%04x (from $%04x)" % (t, info), None))
                    pc, b2, a = nxt, nb2, None
                    continue
                if k == "rst":
                    work.append((i.target, b2, None, "rst from $%04x" % pc))
                    self.targets.add((i.target, i.target))
                    pc, a = nxt, None
                    continue
                if k in ("jp", "jr", "jpc", "jrc", "djnz"):
                    t = i.target
                    if t >= 0xC000:
                        self.ram_calls.setdefault(t, []).append(pc)
                    else:
                        work.append((t, nb2, na, "branch from $%04x with bank %s" % (pc, nb2)))
                        self.targets.add((self.phys(t, nb2) if t >= 0x8000 else t, t))
                    if k in ("jp", "jr"):
                        break
                    pc, b2, a = nxt, nb2, na
                    continue
                if k in ("ret", "reti", "retn"):
                    break
                if k == "jpind":
                    self.indirect.append((pc, ph, i.text))
                    self.note_unresolved("indirect-jump", pc, "%s at phys $%05x (jump table / computed jump: needs annotation or oracle)" % (i.text, ph))
                    break
                pc, b2, a = nxt, nb2, na

    def enqueue_site(self, pc, hist, b2, work):
        """`call enqueue` at pc: the handler is the last `ld hl, nn` before it with nothing touching HL in between"""
        for j in reversed(hist):
            if j.mn == "ld" and j.args.startswith("hl,") and j.imm is not None and j.abs_addr is None and not j.args.endswith(")"):
                h = j.imm
                self.queue_seeds.setdefault(h, []).append(pc)
                work.append((h, "B", None, "task queue: enqueued at $%04x" % pc))
                self.targets.add((self.phys(h, b2) if h >= 0x8000 else h, h))
                return
            if touches_hl(j):
                break
        self.note_unresolved("enqueue-unknown-handler", pc, "call $0850 with HL not a known constant (handler address built at run time)")

    def classify(self, i, ph):
        if i.port is not None:
            self.ports[(i.mn, i.port)].append(i.addr)
        elif i.mn in ("in", "out") and "(c)" in i.args:
            self.cports[i.mn].append(i.addr)
        elif i.mn in ("otir", "otdr", "outi", "outd", "inir", "indr", "ini", "ind"):
            self.cports[i.mn].append(i.addr)
        for name in ("halt", "ei", "di", "im", "retn", "reti", "ldir", "lddr", "exx", "daa"):
            if i.mn == name:
                self.sites[name].append(i.addr)
        if i.mn == "ex" and i.args == "af,af'":
            self.sites["ex af,af'"].append(i.addr)
        if "ix" in i.args or "iy" in i.args:
            self.sites["ix/iy"].append(i.addr)
        if i.mn in ("ldir", "lddr", "ldi", "ldd") :
            pass


def phys_to_cpu(ph):
    """canonical CPU address of a file offset: bank 0 at $0000, bank 1 at $4000, banks 2.. at $8000"""
    b = ph // BANK
    return (ph & 0x3FFF) + (0x0000 if b == 0 else 0x4000 if b == 1 else 0x8000)


def covered_ranges(code, nbanks):
    """per bank: sorted list of [start_off, end_off) of decoded instruction bytes (offsets inside the bank)"""
    cov = collections.defaultdict(set)
    for (ph, pc), i in code.items():
        for k in range(i.length):
            cov[ph // BANK].add((ph + k) % (BANK * nbanks))
    out = {}
    for b, s in cov.items():
        offs = sorted(s)
        rs = []
        for o in offs:
            if rs and rs[-1][1] == o:
                rs[-1][1] = o + 1
            else:
                rs.append([o, o + 1])
        out[b] = rs
    return out


def write_listing(sv, b, path):
    rom = sv.rom
    lo, hi = b * BANK, (b + 1) * BANK
    insns = {ph: i for (ph, pc), i in sv.code.items() if lo <= ph < hi and pc == phys_to_cpu(ph)}
    alias = sum(1 for (ph, pc) in sv.code if lo <= ph < hi and pc != phys_to_cpu(ph))
    labels = {ph for (ph, pc) in sv.targets if ph is not None and lo <= ph < hi}
    lines = ["; bank %d  file offset $%05x-$%05x  CPU $%04x-$%04x%s" % (b, lo, hi - 1, phys_to_cpu(lo), phys_to_cpu(lo) + BANK - 1,
                                                                     "  (%d instructions executed at another CPU address: not listed)" % alias if alias else "")]
    o = lo
    while o < hi:
        if o in insns:
            i = insns[o]
            if o in labels:
                lines.append("L%04x:" % phys_to_cpu(o))
            lines.append("  %04x  %-11s %s" % (phys_to_cpu(o), i.raw.hex(" "), i.text))
            o += i.length
        else:
            e = o
            while e < hi and e not in insns and e - o < 16 and (e == o or e not in labels):
                e += 1
            lines.append("  %04x  .db %s" % (phys_to_cpu(o), " ".join("%02x" % x for x in rom[o:e])))
            o = e
    open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


def main():
    args = sys.argv[1:]
    rom = dis.read_rom(args[0] if args else None)
    os.makedirs(OUT, exist_ok=True)
    sv = Survey(rom)
    sv.reset_collectors()
    rounds = sv.run_all()
    print("descent rounds:", rounds, "base banks:", sorted(sv.base))
    nb = sv.nbanks
    cov = covered_ranges(sv.code, nb)

    stats = bank_stats(rom)
    open(os.path.join(OUT, "bank_stats.txt"), "w").write("\n".join(stats) + "\n")

    # bank map
    bm = ["bank  CPU range   code bytes (% of bank)  ranges  reached from"]
    for b in range(nb):
        n = sum(e - s for s, e in cov.get(b, []))
        bm.append("%2d    $%04x-$%04x  %5d (%4.1f%%)          %3d" % (b, phys_to_cpu(b * BANK), phys_to_cpu(b * BANK) + BANK - 1, n, 100.0 * n / BANK, len(cov.get(b, []))))
    bm.append("")
    bm.append("slot-2 calls (target, bank at the call): sites")
    for (t, b2), sites in sorted(sv.slot2_calls.items(), key=lambda kv: (kv[0][0], -1 if kv[0][1] is None else kv[0][1])):
        bm.append("  call $%04x with slot-2 bank %s from %s" % (t, "?" if b2 is None else b2, " ".join("$%04x" % s for s in sorted(sites))))
    open(os.path.join(OUT, "bank_map.txt"), "w").write("\n".join(bm) + "\n")

    # unresolved
    seen = set()
    ur = []
    for kind, pc, text in sv.unresolved:
        if (kind, pc, text) not in seen:
            seen.add((kind, pc, text))
            ur.append("%-28s $%04x  %s" % (kind, pc, text))
    open(os.path.join(OUT, "unresolved.txt"), "w").write("\n".join(sorted(ur)) + "\n")

    for b in range(nb):
        write_listing(sv, b, os.path.join(OUT, "listing_bank%02d.txt" % b))

    mw = [{"phys": ph, "pc": pc, "reg": reg, "how": how, "value": v} for ph, pc, reg, how, v in sv.mapper_writes]
    meta = {
        "rom_size": len(rom), "banks": nb,
        "instructions": len(sv.code), "code_bytes": sum(e - s for rs in cov.values() for s, e in rs),
        "code_ranges": {str(b): rs for b, rs in cov.items()},
        "mapper_writes": mw,
        "setters": {"$%04x" % t: list(k) for t, k in sv.setter_cache.items() if k[0]},
        "ports": {"%s $%02x" % k: ["$%04x" % a for a in sorted(set(v))] for k, v in sorted(sv.ports.items())},
        "port_via_c": {k: ["$%04x" % a for a in sorted(set(v))] for k, v in sv.cports.items()},
        "queue_handlers": {"$%04x" % h: ["$%04x" % x for x in sorted(set(v))] for h, v in sorted(sv.queue_seeds.items())},
        "ram_calls": {"$%04x" % t: ["$%04x" % s for s in sorted(set(v)) if s >= 0] for t, v in sv.ram_calls.items()},
        "indirect_jumps": [{"pc": pc, "phys": ph, "text": t} for pc, ph, t in sv.indirect],
        "sites": {k: ["$%04x" % a for a in sorted(set(v))] for k, v in sv.sites.items()},
        "unresolved_count": len(ur),
    }
    json.dump(meta, open(os.path.join(OUT, "code_map.json"), "w"), indent=1)
    with open(os.path.join(OUT, "insns.txt"), "w") as f:   # every decoded instruction: file offset, CPU address, length, text (input of gaps.py)
        for (ph, pc), i in sorted(sv.code.items()):
            f.write("%05x %04x %d %s\n" % (ph, pc, i.length, i.text))

    print("instructions: %d, code bytes: %d of %d (%.1f%%)" % (meta["instructions"], meta["code_bytes"], len(rom), 100.0 * meta["code_bytes"] / len(rom)))
    print("bank setters found:", meta["setters"])
    print("queue handlers:", len(meta["queue_handlers"]))
    print("mapper writes:", len(mw), "(slot-2 values:", sorted({str(v) for *_, reg, how, v in [(0, 0, w["reg"], w["how"], w["value"]) for w in mw] if reg == "slot2"}), ")")
    print("ram calls:", meta["ram_calls"])
    print("unresolved entries:", len(ur))
    print("\n".join(bm[:nb + 1]))


if __name__ == "__main__":
    main()
