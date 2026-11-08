/* Dev-time reference run of a Game Gear ROM: Z80 core (z80.c) + shared hardware model (src/hw/hw.c). Never part of the PSP build.
 *
 *   oracle ROM.gg [options]
 *     --frames N             run N frames (default 600)
 *     --script FILE          input script: lines "FIRST LAST BUTTONS" (frames, inclusive; BUTTONS joined with '+': up down left right 1 2 start); '#' comments
 *     --dump DIR EVERY       write DIR/frame_NNNNN.rgb (160x144x3) every EVERY frames
 *     --dump-at DIR F1,F2..  write frame dumps at those frames
 *     --hash FILE            CRC32 of every frame's picture, one "frame crc" line per frame
 *     --cov FILE             coverage: every executed ROM instruction start ("rom PHYS"), code executed in RAM ("ram PC CRC16BYTES"), aliased addresses ("alias PHYS PC")
 *     --ind FILE             indirect flow: "jp (hl)" targets and `ret` dispatch (return address that does not match a call): "kind site target count"
 *     --iolog FILE           every port access and mapper write with frame / line / cycle
 *     --ramdump FRAME FILE   RAM ($C000-$DFFF, 8 KB) after that frame
 *     --rdmap FILE           one byte per ROM offset: 1 = read at least once (data reads and instruction fetches)
 *     --trace FILE FROM TO   one line per instruction (cycle, pc, registers) for T-states in [FROM, TO)
 *     --watch LO HI FILE     unique (writer pc, address, count, last value) of every write into [LO, HI]
 *     --mbox LO HI FILE      ordered log of the GAME's writes (writer pc outside $D500-$DEFF) into [LO, HI] tagged with the number of sound-driver calls so far, plus
 *                            one line per call of the driver (pc reaches $D500) and for the init call ($D503): the input of tools/snd/ (S1 of PLAN.md M6b)
 *     --initram FILE         RAM ($C000-$DFFF, 8 KB) just before the first call of $D503 (the driver unpacked, its variables as at boot)
 *     --quiet
 * Prints a summary (frames, T-states, warnings the hardware model raised).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/hw/hw.h"
#include "z80.h"

static HW hw;
static Z80 cpu;
static FILE *iolog;
static uint8_t *cov;
static uint32_t crc_tab[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_tab[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v >> 1) ^ (0xEDB88320u & (0u - (v & 1u)));
            crc_tab[i] = v;
        }
    }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* ---- flow tables ------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t site, target; char kind; uint32_t count; } Flow;
static Flow *flows;
static int nflows, capflows;

static void flow_add(char kind, uint32_t site, uint32_t target)
{
    for (int i = 0; i < nflows; i++)
        if (flows[i].kind == kind && flows[i].site == site && flows[i].target == target) { flows[i].count++; return; }
    if (nflows == capflows) { capflows = capflows ? capflows * 2 : 256; flows = (Flow *)realloc(flows, (size_t)capflows * sizeof(Flow)); }
    flows[nflows++] = (Flow){site, target, kind, 1};
}

typedef struct { uint16_t pc; uint32_t crc; uint32_t count; } RamExec;
static RamExec *ramx;
static int nramx, capramx;

typedef struct { uint32_t phys; uint16_t pc; } Alias;
static Alias *aliases;
static int naliases, capaliases;

static uint16_t shadow[4096];
static int sdepth;

/* ---- hardware glue ------------------------------------------------------------------------------------------------------------------------------ */
/* write watch: unique (writer pc, address) pairs for writes into [watch_lo, watch_hi] (--watch LO HI FILE), used to find self-modifying code and code loaders */
static int watch_on;
static uint16_t watch_lo, watch_hi;
typedef struct { uint16_t pc, addr; uint32_t count; uint8_t last; } WatchW;
static WatchW *wwr;
static int nwwr, capwwr;
static void watch_add(uint16_t pc, uint16_t a, uint8_t v)
{
    for (int i = 0; i < nwwr; i++)
        if (wwr[i].pc == pc && wwr[i].addr == a) { wwr[i].count++; wwr[i].last = v; return; }
    if (nwwr == capwwr) { capwwr = capwwr ? capwwr * 2 : 256; wwr = (WatchW *)realloc(wwr, (size_t)capwwr * sizeof(WatchW)); }
    wwr[nwwr++] = (WatchW){pc, a, 1, v};
}

/* ROM read tracking (--rdmap FILE): one byte per ROM offset, 1 = read at least once (instruction fetches included; subtract the covered instructions) */
static uint8_t *rdmap;
/* --mbox: the game's writes into a RAM range, in order, tagged with the number of driver calls so far */
static FILE *mbox_f;
static uint16_t mbox_lo, mbox_hi;
static uint64_t drv_calls;
static uint8_t c_rd(void *u, uint16_t a)
{
    if (rdmap && a < 0xC000) { int32_t ph = hw_phys((HW *)u, a); if (ph >= 0) rdmap[ph] = 1; }
    return hw_rd((HW *)u, a);
}
static void c_wr(void *u, uint16_t a, uint8_t v)
{
    if (watch_on && a >= watch_lo && a <= watch_hi) watch_add(cpu.last_pc, a, v);
    if (mbox_f && a >= mbox_lo && a <= mbox_hi && (cpu.last_pc < 0xD500 || cpu.last_pc > 0xDEFF)) fprintf(mbox_f, "W %llu %04x %02x %04x\n", (unsigned long long)drv_calls, a, v, cpu.last_pc);
    hw_wr((HW *)u, a, v);
}
static uint8_t c_in(void *u, uint16_t p) { return hw_in((HW *)u, p); }
static void c_out(void *u, uint16_t p, uint8_t v) { hw_out((HW *)u, p, v); }

static void on_port(HW *h, int is_out, uint8_t port, uint8_t val, void *user)
{
    (void)user;
    if (iolog) fprintf(iolog, "%llu f%llu l%d c%d %s %02x %02x pc=%04x\n", (unsigned long long)h->cycles, (unsigned long long)h->frame, h->line, h->line_cycle,
                       is_out ? "out" : "in ", port, val, cpu.last_pc);
}
static void on_mapper(HW *h, int reg, uint8_t val, void *user)
{
    (void)user;
    if (iolog) fprintf(iolog, "%llu f%llu l%d c%d map %c %02x pc=%04x\n", (unsigned long long)h->cycles, (unsigned long long)h->frame, h->line, h->line_cycle,
                       "CDEF"[reg] , val, cpu.last_pc);
}

/* ---- input script ------------------------------------------------------------------------------------------------------------------------------ */
typedef struct { int first, last; uint8_t mask; } Press;
static Press *presses;
static int npress;

static uint8_t parse_buttons(const char *s)
{
    uint8_t m = 0;
    char buf[128];
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *t = strtok(buf, "+ \t\r\n"); t; t = strtok(NULL, "+ \t\r\n")) {
        if (!strcmp(t, "up")) m |= PAD_UP;
        else if (!strcmp(t, "down")) m |= PAD_DOWN;
        else if (!strcmp(t, "left")) m |= PAD_LEFT;
        else if (!strcmp(t, "right")) m |= PAD_RIGHT;
        else if (!strcmp(t, "1")) m |= PAD_1;
        else if (!strcmp(t, "2")) m |= PAD_2;
        else if (!strcmp(t, "start")) m |= PAD_START;
        else { fprintf(stderr, "script: unknown button '%s'\n", t); exit(2); }
    }
    return m;
}

static struct { int frame, to, addr, val; } pokes[256];
static int npokes;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int pf, pa, pv, pt;
        if (sscanf(line, "#! poke %d %i %i", &pf, &pa, &pv) == 3 && npokes < 256) { pokes[npokes].frame = pokes[npokes].to = pf; pokes[npokes].addr = pa; pokes[npokes].val = pv; npokes++; continue; }     /* RAM poke at the end of a frame, as in the native host */
        if (sscanf(line, "#! hold %d %d %i %i", &pf, &pt, &pa, &pv) == 4 && npokes < 256) { pokes[npokes].frame = pf; pokes[npokes].to = pt; pokes[npokes].addr = pa; pokes[npokes].val = pv; npokes++; continue; }     /* the byte is rewritten at the end of every frame from pf to pt (infinite balls, time) */
        char *h = strchr(line, '#');
        if (h) *h = 0;
        int a, b;
        char btn[128];
        if (sscanf(line, "%d %d %127s", &a, &b, btn) == 3) {
            presses = (Press *)realloc(presses, (size_t)(npress + 1) * sizeof(Press));
            presses[npress++] = (Press){a, b, parse_buttons(btn)};
        }
    }
    fclose(f);
}

static uint8_t pad_at(int frame)
{
    uint8_t m = 0;
    for (int i = 0; i < npress; i++)
        if (frame >= presses[i].first && frame <= presses[i].last) m |= presses[i].mask;
    return m;
}

/* ---- tracing of one instruction ------------------------------------------------------------------------------------------------------------------ */
static void trace_before(uint16_t pc)
{
    int32_t ph = hw_phys(&hw, pc);
    if (ph >= 0) {
        if (!cov[ph]) {
            cov[ph] = 1;
            uint16_t canon = (uint16_t)(ph < 0x8000 ? ph : 0x8000 + (ph & 0x3FFF));
            if (canon != pc) {
                if (naliases == capaliases) { capaliases = capaliases ? capaliases * 2 : 64; aliases = (Alias *)realloc(aliases, (size_t)capaliases * sizeof(Alias)); }
                aliases[naliases++] = (Alias){(uint32_t)ph, pc};
            }
        }
    } else {
        uint8_t b[16];
        for (int i = 0; i < 16; i++) b[i] = hw_rd(&hw, (uint16_t)(pc + i));
        uint32_t crc = crc32_update(0, b, 16);
        int i;
        for (i = 0; i < nramx; i++) if (ramx[i].pc == pc && ramx[i].crc == crc) { ramx[i].count++; break; }
        if (i == nramx) {
            if (nramx == capramx) { capramx = capramx ? capramx * 2 : 64; ramx = (RamExec *)realloc(ramx, (size_t)capramx * sizeof(RamExec)); }
            ramx[nramx++] = (RamExec){pc, crc, 1};
        }
    }
}

static void trace_after(uint16_t pc, uint8_t *op)
{
    /* op: the instruction's first bytes before it ran */
    uint8_t o = op[0];
    uint16_t npc = cpu.pc;
    int ed = (o == 0xED);
    if (cpu.last_irq) {
        if (sdepth < 4096) shadow[sdepth++] = pc;
        return;
    }
    if (o == 0xCD || (o & 0xC7) == 0xC4) {
        if (npc != (uint16_t)(pc + 3) && sdepth < 4096) shadow[sdepth++] = (uint16_t)(pc + 3);
    } else if ((o & 0xC7) == 0xC7) {
        if (sdepth < 4096) shadow[sdepth++] = (uint16_t)(pc + 1);
    } else if (o == 0xC9 || (o & 0xC7) == 0xC0 || (ed && (op[1] == 0x4D || op[1] == 0x45))) {
        int taken = ed ? 1 : (o == 0xC9) ? 1 : (npc != (uint16_t)(pc + 1));
        if (taken && sdepth > 0) {
            if (shadow[sdepth - 1] == npc) sdepth--;
            else flow_add('R', pc, npc);       /* return address that was not pushed by a call: table / ret dispatch */
        }
    } else if (o == 0xE9 || ((o == 0xDD || o == 0xFD) && op[1] == 0xE9)) {
        flow_add('J', pc, npc);
    }
}

/* ---- output helpers ---------------------------------------------------------------------------------------------------------------------------- */
static void write_rgb(const char *dir, int frame)
{
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%05d.rgb", dir, frame);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    static uint8_t buf[GG_W * GG_H * 3];
    for (int i = 0; i < GG_W * GG_H; i++) {
        uint32_t p = hw.fb[i];
        buf[i * 3] = (uint8_t)p; buf[i * 3 + 1] = (uint8_t)(p >> 8); buf[i * 3 + 2] = (uint8_t)(p >> 16);
    }
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: oracle ROM.gg [options] (see the top of oracle.c)\n"); return 2; }
    const char *rom_path = argv[1], *dump_dir = NULL, *dump_at_dir = NULL, *hash_path = NULL, *cov_path = NULL, *ind_path = NULL, *io_path = NULL, *ram_path = NULL, *watch_path = NULL, *rdmap_path = NULL, *trace_path = NULL;
    uint64_t trace_from = 0, trace_to = 0;
    const char *initram_path = NULL;
    int init_done = 0;
    FILE *trace_f = NULL;
    int frames = 600, dump_every = 0, ram_frame = -1, quiet = 0;
    int dump_at[256], ndump_at = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames")) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script")) load_script(argv[++i]);
        else if (!strcmp(argv[i], "--dump")) { dump_dir = argv[++i]; dump_every = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--dump-at")) {
            dump_at_dir = argv[++i];
            for (char *t = strtok(argv[++i], ","); t && ndump_at < 256; t = strtok(NULL, ",")) dump_at[ndump_at++] = atoi(t);
        }
        else if (!strcmp(argv[i], "--hash")) hash_path = argv[++i];
        else if (!strcmp(argv[i], "--cov")) cov_path = argv[++i];
        else if (!strcmp(argv[i], "--ind")) ind_path = argv[++i];
        else if (!strcmp(argv[i], "--iolog")) io_path = argv[++i];
        else if (!strcmp(argv[i], "--ramdump")) { ram_frame = atoi(argv[++i]); ram_path = argv[++i]; }
        else if (!strcmp(argv[i], "--watch")) { watch_lo = (uint16_t)strtoul(argv[++i], NULL, 0); watch_hi = (uint16_t)strtoul(argv[++i], NULL, 0); watch_path = argv[++i]; watch_on = 1; }
        else if (!strcmp(argv[i], "--rdmap")) rdmap_path = argv[++i];
        else if (!strcmp(argv[i], "--trace")) { trace_path = argv[++i]; trace_from = strtoull(argv[++i], NULL, 0); trace_to = strtoull(argv[++i], NULL, 0); }
        else if (!strcmp(argv[i], "--mbox")) { mbox_lo = (uint16_t)strtoul(argv[++i], NULL, 0); mbox_hi = (uint16_t)strtoul(argv[++i], NULL, 0); mbox_f = fopen(argv[++i], "w"); }
        else if (!strcmp(argv[i], "--initram")) initram_path = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    FILE *rf = fopen(rom_path, "rb");
    if (!rf) { perror(rom_path); return 2; }
    fseek(rf, 0, SEEK_END);
    long size = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    uint8_t *rom = (uint8_t *)malloc((size_t)size);
    if (fread(rom, 1, (size_t)size, rf) != (size_t)size) { fprintf(stderr, "short read\n"); return 2; }
    fclose(rf);
    cov = (uint8_t *)calloc((size_t)size, 1);
    if (rdmap_path) rdmap = (uint8_t *)calloc((size_t)size, 1);

    hw_init(&hw, rom, (uint32_t)size);
    hw.on_port = on_port;
    hw.on_mapper = on_mapper;
    memset(&cpu, 0, sizeof cpu);
    cpu.rd = c_rd; cpu.wr = c_wr; cpu.in = c_in; cpu.out = c_out; cpu.u = &hw;
    z80_reset(&cpu);
    if (io_path) iolog = fopen(io_path, "w");
    FILE *hf = hash_path ? fopen(hash_path, "w") : NULL;

    if (trace_path) trace_f = fopen(trace_path, "w");
    uint64_t steps = 0;
    for (int frame = 0; frame < frames; frame++) {
        hw.pad = pad_at(frame);
        hw.frame_done = 0;
        while (!hw.frame_done) {
            uint16_t pc = cpu.pc;
            uint8_t op[2];
            op[0] = hw_rd(&hw, pc);
            op[1] = hw_rd(&hw, (uint16_t)(pc + 1));
            cpu.irq = (uint8_t)hw_irq(&hw);
            if (trace_f && hw.cycles >= trace_from && hw.cycles < trace_to && !(cpu.irq && cpu.iff1 && !cpu.ei_delay))   /* interrupt acknowledge steps are not instructions */
                fprintf(trace_f, "%llu %04x af=%02x%02x bc=%02x%02x de=%02x%02x hl=%02x%02x sp=%04x ix=%04x iy=%04x\n", (unsigned long long)hw.cycles, pc, cpu.r[7], cpu.f, cpu.r[0], cpu.r[1],
                        cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5], cpu.sp, cpu.ix, cpu.iy);
            if (pc == 0xD500 && !cpu.halted) { drv_calls++; if (mbox_f) fprintf(mbox_f, "K %llu %d\n", (unsigned long long)drv_calls, frame); }
            if (pc == 0xD503 && !init_done && !cpu.halted) {
                init_done = 1;
                if (mbox_f) fprintf(mbox_f, "I %d bank2=%d\n", frame, hw.bank[2]);
                if (initram_path) { FILE *f = fopen(initram_path, "wb"); if (f) { fwrite(hw.ram, 1, sizeof hw.ram, f); fclose(f); } }
            }
            if (!cpu.halted) trace_before(pc);
            int t = z80_step(&cpu);
            hw_advance(&hw, t);
            trace_after(pc, op);
            steps++;
        }
        for (int k = 0; k < npokes; k++) if (frame >= pokes[k].frame && frame <= pokes[k].to) hw.ram[pokes[k].addr & 0x1FFF] = (uint8_t)pokes[k].val;
        if (hf) fprintf(hf, "%d %08x\n", frame, crc32_update(0, (const uint8_t *)hw.fb, sizeof hw.fb));
        if (dump_dir && dump_every > 0 && frame % dump_every == 0) write_rgb(dump_dir, frame);
        for (int i = 0; i < ndump_at; i++) if (dump_at[i] == frame) write_rgb(dump_at_dir, frame);
        if (ram_path && frame == ram_frame) {
            FILE *f = fopen(ram_path, "wb");
            if (f) { fwrite(hw.ram, 1, sizeof hw.ram, f); fclose(f); }
        }
    }
    if (hf) fclose(hf);
    if (mbox_f) fclose(mbox_f);
    if (iolog) fclose(iolog);
    if (trace_f) fclose(trace_f);

    if (cov_path) {
        FILE *f = fopen(cov_path, "w");
        for (long i = 0; i < size; i++) if (cov[i]) fprintf(f, "rom %05lx\n", i);
        for (int i = 0; i < nramx; i++) fprintf(f, "ram %04x %08x %u\n", ramx[i].pc, ramx[i].crc, ramx[i].count);
        for (int i = 0; i < naliases; i++) fprintf(f, "alias %05x %04x\n", aliases[i].phys, aliases[i].pc);
        fclose(f);
    }
    if (ind_path) {
        FILE *f = fopen(ind_path, "w");
        for (int i = 0; i < nflows; i++) fprintf(f, "%s %04x %04x %u\n", flows[i].kind == 'J' ? "jp(hl)" : "ret", flows[i].site, flows[i].target, flows[i].count);
        fclose(f);
    }
    if (rdmap_path) {
        FILE *f = fopen(rdmap_path, "wb");
        if (f) { fwrite(rdmap, 1, (size_t)size, f); fclose(f); }
    }
    if (watch_path) {
        FILE *f = fopen(watch_path, "w");
        for (int i = 0; i < nwwr; i++) fprintf(f, "%04x %04x %u %02x\n", wwr[i].pc, wwr[i].addr, wwr[i].count, wwr[i].last);
        fclose(f);
    }
    if (!quiet) {
        long ncov = 0;
        for (long i = 0; i < size; i++) ncov += cov[i];
        printf("frames %d, %llu instructions, %.1f million T-states, %ld ROM instruction starts covered, %d RAM code sites, %d indirect flows, hw warnings 0x%x\n",
               frames, (unsigned long long)steps, (double)hw.cycles / 1e6, ncov, nramx, nflows, hw.warn_flags);
        printf("final: pc=%04x sp=%04x iff1=%d im=%d halted=%d bank=%d/%d/%d vdp regs0-1=%02x %02x status=%02x\n", cpu.pc, cpu.sp, cpu.iff1, cpu.im, cpu.halted,
               hw.bank[0], hw.bank[1], hw.bank[2], hw.reg[0], hw.reg[1], hw.status);
    }
    return 0;
}
