/* Oscillator mixer for the game's sound chip commands (M6b / S4): three square-wave tone voices, one noise voice, 2 dB volume steps, Game Gear stereo mask on port $06.
 * Integer only (no floating point), so the PC and PSP builds produce identical samples; src/hw/hw.c holds the voice parameters (tone periods, volumes, noise mode),
 * this file turns them into 44.1 kHz audio.
 *
 * Model (documented SN76489 behaviour, assumed until compared with a reference, NOTES.md):
 *  - time is counted in chip clocks ("ticks", the Z80 clock / 16 = 223,722 Hz). A tone voice's counter reloads from its 10-bit period and flips the output every
 *    `period` ticks, so f = 3,579,545 / (32 * period). A period of 0 or 1 holds the output high (games use this to play sampled sound through the volume register).
 *  - noise: a flip-flop with a period of 16 / 32 / 64 ticks, or the period of tone 2 (noise register bits 0-1 = 3); the 16-bit shift register shifts on every rising
 *    edge of the flip-flop; white noise feeds back bit 0 xor bit 3, periodic noise bit 0; the output is the register's low bit; a noise register write resets it to $8000.
 *  - every output sample covers the 5 or 6 ticks between two sample boundaries and is the average of the voice levels over them (a box filter); register writes take
 *    effect at the next sample boundary (22 us).
 *  - volume 0..14 = 2 dB steps below the maximum, 15 = off; Game Gear stereo: port $06 bits 4-7 enable tone 0-2 and noise on the left, bits 0-3 on the right.
 *  - a one-pole high-pass (about 7 Hz) removes the DC offset of the unipolar voices.
 *
 * How it is computed: the mixer does not step the voices tick by tick. A sample covers at most 6 ticks, so a voice with a period of 6 or more flips at most once per
 * sample: the tick of the flip follows from its counter and the number of 'on' ticks is a subtraction. Voices with shorter periods (above 37 kHz, nobody hears them, but
 * the game could program them) fall back to the tick loop. The output is bit for bit what the tick-by-tick synthesiser it replaced produced (tests/psg_ref.c,
 * compared by tests/psg_diff.c on random register traffic and by the picture / audio CRCs of every scenario).
 */
#include "hw.h"

#define PSG_CLK 3579545ull

void (*hw_audio_sink)(const int16_t *lr, int frames);

/* 6554 * 10^(-v / 10), rounded; three voices at full volume sum to 19,662 and four to 26,216 (no clipping) */
static const uint16_t amp_tab[16] = {6554, 5206, 4135, 3285, 2609, 2073, 1646, 1308, 1039, 825, 655, 520, 413, 328, 261, 0};

void hw_audio_flush(HW *hw)
{
    HwPsg *p = &hw->psg;
    if (p->nbuf && hw_audio_sink) hw_audio_sink(p->buf, p->nbuf);
    p->nbuf = 0;
}

static inline int16_t clamp16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* chip clocks per output sample: PSG_CLK / 16 / HW_AUDIO_RATE = 3579545 / 705600 = 5.07, i.e. 5 ticks, or 6 when the fraction carries over */
#define TICK_DEN (HW_AUDIO_RATE * 16u)

/* a tone voice over the next n ticks (n = 5 or 6): the number of ticks its output is high; advances the counter and the flip-flop */
static inline int tone_run(uint16_t *cnt, uint8_t *out, unsigned per, int n)
{
    unsigned c = *cnt, o = *out;
    if (per <= 1) { *out = 1; return n; }
    if (c > (unsigned)n) { *cnt = (uint16_t)(c - (unsigned)n); return o ? n : 0; }          /* no flip in this sample */
    if (per >= (unsigned)n) {                                                              /* exactly one flip, at tick f */
        int f = c ? (int)c : 1;
        *out = (uint8_t)(o ^ 1u);
        *cnt = (uint16_t)(per - (unsigned)(n - f));
        return o ? f - 1 : n - f + 1;
    }
    int on = 0;                                                                            /* a period shorter than the sample: tick by tick */
    for (int k = 0; k < n; k++) {
        if (c > 0) c--;
        if (c == 0) { c = per; o ^= 1u; }
        on += (int)o;
    }
    *cnt = (uint16_t)c;
    *out = (uint8_t)o;
    return on;
}

/* the noise voice over the next n ticks: the number of ticks the shift register's low bit is high; `white` selects the feedback */
static inline int noise_run(uint16_t *cnt, uint8_t *out, uint16_t *lfsr, unsigned nper, unsigned white, int n)
{
    unsigned c = *cnt, o = *out, l = *lfsr;
    if (c > (unsigned)n) { *cnt = (uint16_t)(c - (unsigned)n); return (l & 1u) ? n : 0; }
    if (nper >= (unsigned)n) {
        int f = c ? (int)c : 1;
        o ^= 1u;
        *out = (uint8_t)o;
        *cnt = (uint16_t)(nper - (unsigned)(n - f));
        if (!o) return (l & 1u) ? n : 0;                       /* falling edge: the register does not move */
        int before = (l & 1u) ? f - 1 : 0;
        unsigned fb = white ? ((l ^ (l >> 3)) & 1u) : (l & 1u);
        l = (l >> 1) | fb << 15;
        *lfsr = (uint16_t)l;
        return before + ((l & 1u) ? n - f + 1 : 0);
    }
    int on = 0;
    for (int k = 0; k < n; k++) {
        if (c > 0) c--;
        if (c == 0) {
            c = nper;
            o ^= 1u;
            if (o) {
                unsigned fb = white ? ((l ^ (l >> 3)) & 1u) : (l & 1u);
                l = (l >> 1) | fb << 15;
            }
        }
        on += (int)(l & 1u);
    }
    *cnt = (uint16_t)c;
    *out = (uint8_t)o;
    *lfsr = (uint16_t)l;
    return on;
}

static void audio_run(HW *hw, uint64_t cycles)
{
    HwPsg *p = &hw->psg;
    uint64_t end = cycles * HW_AUDIO_RATE / PSG_CLK;      /* samples completed at T-state `cycles` */
    if (!hw_audio_sink) {                                 /* nobody listens: keep the clock, skip the work */
        if (end > p->sample) { p->sample = end; p->tick = end * PSG_CLK / TICK_DEN; p->rem = (uint32_t)(end * PSG_CLK % TICK_DEN); }
        return;
    }
    if (p->sample >= end) return;
    /* the voice parameters only change between runs (a register write runs the mixer up to the write first) */
    unsigned per0 = hw->psg_tone[0], per1 = hw->psg_tone[1], per2 = hw->psg_tone[2];
    unsigned nper, white = hw->psg_noise & 4;
    {
        unsigned r = hw->psg_noise & 3;
        nper = r == 3 ? (per2 ? per2 : 1) : 16u << r;
    }
    int32_t amp[4];
    int32_t lmask[4], rmask[4];
    for (int c = 0; c < 4; c++) {
        amp[c] = amp_tab[hw->psg_vol[c] & 15];
        lmask[c] = (hw->gg_stereo & (0x10 << c)) ? -1 : 0;
        rmask[c] = (hw->gg_stereo & (0x01 << c)) ? -1 : 0;
    }
    uint32_t rem = p->rem;
    uint64_t sample = p->sample, tick = p->tick;
    int32_t lpl = p->lpl, lpr = p->lpr;
    int nbuf = p->nbuf;
    while (sample < end) {
        rem += (uint32_t)PSG_CLK;
        int n = rem >= 6u * TICK_DEN ? 6 : 5;
        rem -= (uint32_t)n * TICK_DEN;
        tick += (uint64_t)n;
        int on0 = tone_run(&p->cnt[0], &p->out[0], per0, n);
        int on1 = tone_run(&p->cnt[1], &p->out[1], per1, n);
        int on2 = tone_run(&p->cnt[2], &p->out[2], per2, n);
        int on3 = noise_run(&p->cnt[3], &p->out[3], &p->lfsr, nper, white, n);
        int32_t a0 = amp[0] * on0, a1 = amp[1] * on1, a2 = amp[2] * on2, a3 = amp[3] * on3;
        if (n == 5) { a0 /= 5; a1 /= 5; a2 /= 5; a3 /= 5; } else { a0 /= 6; a1 /= 6; a2 /= 6; a3 /= 6; }
        int32_t l = (a0 & lmask[0]) + (a1 & lmask[1]) + (a2 & lmask[2]) + (a3 & lmask[3]);
        int32_t r = (a0 & rmask[0]) + (a1 & rmask[1]) + (a2 & rmask[2]) + (a3 & rmask[3]);
        lpl += ((l << 16) - lpl) >> 10;
        lpr += ((r << 16) - lpr) >> 10;
        p->buf[nbuf * 2] = clamp16(l - (lpl >> 16));
        p->buf[nbuf * 2 + 1] = clamp16(r - (lpr >> 16));
        sample++;
        if (++nbuf == HW_AUDIO_BUF) {
            p->nbuf = nbuf;
            hw_audio_flush(hw);
            nbuf = 0;
        }
    }
    p->rem = rem;
    p->sample = sample;
    p->tick = tick;
    p->lpl = lpl;
    p->lpr = lpr;
    p->nbuf = nbuf;
}

void hw_audio_run(HW *hw, uint64_t cycles)
{
    if (hw_prof_clock) {
        uint64_t t = hw_prof_clock();
        audio_run(hw, cycles);
        hw_prof_audio_us += hw_prof_clock() - t;
    } else {
        audio_run(hw, cycles);
    }
}
