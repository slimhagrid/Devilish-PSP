/* Game Gear hardware model: memory map and Sega mapper, I/O ports, VDP (Mode 4, 160x144 window, line / frame interrupts, per-scanline renderer), PSG register
 * state, joypad. No CPU in here: the caller (the dev-time oracle, the translated game, the PSP front end) provides the Z80 and reports elapsed T-states through
 * hw_advance(). Written once and shared; everything in it is a model of the Game Gear as documented (assumed until checked against a reference, see NOTES.md).
 */
#ifndef HW_H
#define HW_H
#include <stddef.h>
#include <stdint.h>

#define GG_W 160
#define GG_H 144
#define HW_CYC_LINE 228                 /* Z80 T-states per scanline (342 pixel clocks / 1.5) */
#define HW_LINES 262                    /* NTSC */
#define HW_CYC_FRAME (HW_CYC_LINE * HW_LINES)

#define PAD_UP 0x01
#define PAD_DOWN 0x02
#define PAD_LEFT 0x04
#define PAD_RIGHT 0x08
#define PAD_1 0x10
#define PAD_2 0x20
#define PAD_START 0x40
#define PAD_BACK 0x80                 /* not a console button: "back out of the menu" (Circle on the PSP, "back" in scripts); the hardware never sees it, rt_back_filter() acts on it */

/* PSG synthesis state (src/hw/psg.c): integer only, so every build produces the same samples */
#define HW_AUDIO_RATE 44100
#define HW_AUDIO_BUF 1024               /* stereo frames buffered before the sink is called */
typedef struct {
    uint64_t sample;                    /* output samples produced so far */
    uint64_t tick;                      /* chip clocks (T-states / 16) run so far */
    uint32_t rem;                       /* sample * 3579545 mod 705600: the fraction of a chip clock at the current sample boundary */
    uint16_t cnt[4];                    /* tone / noise down-counters */
    uint8_t out[4];                     /* flip-flop of each channel (3 = noise: the shift register's low bit is used instead) */
    uint16_t lfsr;
    int32_t lpl, lpr;                   /* DC-blocking filter state (16 fractional bits) */
    int nbuf;
    int16_t buf[HW_AUDIO_BUF * 2];
} HwPsg;

typedef struct HW HW;
struct HW {
    /* cartridge and memory */
    const uint8_t *rom;
    uint32_t rom_size, nbanks;
    uint8_t ram[0x2000];
    uint8_t map[4];                     /* last values written to $FFFC-$FFFF (the registers live in the RAM mirror) */
    uint8_t bank[3];                    /* ROM bank in slot 0, 1, 2 (mapper registers $FFFD-$FFFF) */
    const uint8_t *page[3];             /* start of the mapped 16 KB ROM page of each slot (kept in step with bank[]) */

    /* VDP */
    uint8_t vram[0x4000];
    uint8_t cram[64];                   /* 32 x 12-bit colours as little-endian words: GGGGRRRR, 0000BBBB */
    uint32_t pal_rgb[32];               /* the same colours as 0xAABBGGRR, kept in step with cram[] */
    uint8_t reg[16];
    uint16_t vaddr;
    uint8_t vcode;                      /* 0 = VRAM read, 1 = VRAM write, 2 = register write, 3 = CRAM write */
    uint8_t vlatch, vlatch_set;         /* first byte of a control word */
    uint8_t vbuf;                       /* read-ahead buffer */
    uint8_t cram_latch;
    uint8_t status;                     /* bit 7 frame interrupt, bit 6 sprite overflow, bit 5 sprite collision */
    uint8_t line_pending;
    uint8_t line_counter;
    uint8_t vscroll;                    /* vertical scroll, latched at the start of the frame */

    /* timing */
    int line;                           /* current scanline 0..261 */
    int line_cycle;                     /* T-state within the line */
    int line_drawn;                     /* the current line has been rendered (HW_RENDER_CYCLE reached) */
    /* sprite cache for the line renderer: per line the first 8 sprites that cover it (table order) and how many cover it (9 = more than 8); rebuilt on demand
     * when the sprite table (256 bytes at the table base), register 1 (size) or register 5 (base) changes, so that a line does not scan all 64 entries */
    int spr_dirty;
    uint8_t spr_cnt[192];
    uint8_t spr_list[192][8];
    uint64_t cycles;                    /* T-states since reset */
    uint64_t frame;                     /* completed frames */
    int frame_done;                     /* set when the last scanline ends; cleared by the caller */

#ifndef HW_NO_PIXELS                    /* the PSP build has no line renderer: the GE draws from the scene recorder's draw list */
    /* output of the reference line renderer: 160x144 pixels, 0xAABBGGRR (PSP-native order) */
    uint32_t fb[GG_H * GG_W];
    uint8_t line_ver[GG_H];             /* +1 whenever the line was re-rendered with different content */
#endif

    /* PSG (register state only here; synthesis lives in the audio module) */
    uint8_t psg_tone_lo[3], psg_tone_hi[3];
    uint16_t psg_tone[3];
    uint8_t psg_vol[4];                 /* 0 = loudest, 15 = off */
    uint8_t psg_noise;
    uint8_t psg_latch;
    uint8_t gg_stereo;                  /* port $06: bits 4-7 left, bits 0-3 right enable of tone 0-2 and noise */
    HwPsg psg;

    /* input and misc ports */
    uint8_t pad;                        /* PAD_* bits, 1 = pressed */
    uint8_t io_ctrl;                    /* port $3F */
    uint8_t mem_ctrl;                   /* port $3E */

    /* diagnostics (the oracle fills these in; the PSP build leaves them NULL) */
    void (*on_port)(HW *hw, int is_out, uint8_t port, uint8_t val, void *user);
    void (*on_mapper)(HW *hw, int reg, uint8_t val, void *user);
    void *user;
    uint32_t warn_flags;                /* HW_WARN_*: things the model does not implement but the game did */
};

#define HW_WARN_CARTRAM 0x01            /* $FFFC bit 3 / 2: cart RAM or RAM-over-ROM mapping requested */
#define HW_WARN_ZOOM 0x02               /* zoomed sprites (reg 1 bit 0) */
#define HW_WARN_PORT 0x04               /* access to a port the model does not know */
#define HW_WARN_MODE 0x08               /* VDP mode other than plain Mode 4 / 192 lines */

/* diagnostics only: skip the line renderer (the picture stays as it was; the PSP front end's A/B timing test uses it to split CPU time from video time) */
extern int hw_skip_render;

/* diagnostics only: when hw_prof_clock is set (microseconds) the time spent in the line renderer and in the PSG synthesis is accumulated here */
extern uint64_t (*hw_prof_clock)(void);
extern uint64_t hw_prof_render_us, hw_prof_audio_us;

void hw_init(HW *hw, const uint8_t *rom, uint32_t size);
void hw_reset(HW *hw);

uint8_t hw_rd(HW *hw, uint16_t a);
void hw_wr(HW *hw, uint16_t a, uint8_t v);
uint8_t hw_in(HW *hw, uint16_t port);
void hw_out(HW *hw, uint16_t port, uint8_t v);

/* audio: samples are produced up to T-state `cycles` and handed to hw_audio_sink (stereo s16 frames, left first) in chunks; without a sink nothing is synthesised.
 * The hardware model calls hw_audio_run() itself before every PSG / stereo register write; the front end calls it (and hw_audio_flush) once per frame. */
extern void (*hw_audio_sink)(const int16_t *lr, int frames);
void hw_audio_run(HW *hw, uint64_t cycles);
void hw_audio_flush(HW *hw);

/* account `t` T-states: run the scanline timeline (renders lines, raises the vblank / line interrupts) */
void hw_advance(HW *hw, int t);
/* after a save state was loaded (src/rt/state.c): rebuild what is derived from the loaded fields (palette, ROM page pointers, sprite cache) and tell the scene recorder that every tile may have changed */
void hw_state_restored(HW *hw);
/* level of the Z80's INT line */
int hw_irq(const HW *hw);

/* file offset of the ROM byte that CPU address `a` currently reads (-1 for RAM) */
int32_t hw_phys(const HW *hw, uint16_t a);

#endif
