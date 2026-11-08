#include <string.h>
#include "hiscore.h"

#define LAP_ADDR 0xCEC7
#define LAP_LEN 48
#define PLY_ADDR 0xCF07
#define PLY_LEN 20
#define QUIET_FRAMES 120          /* the tables unchanged for 2 seconds: write */
#define GIVE_UP_FRAMES 3600       /* the defaults were never seen (the game started differently): leave the tables alone */

/* the tables as the game's start-up routine writes them: generated from the user's own ROM (tools/z80_to_c.py -> extracted/gen/gen_info.c, read from the RAM image the oracle captured),
 * so that this source holds no game data */
extern const uint8_t gen_hs_defaults[HS_BYTES];
#define defaults gen_hs_defaults
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

static void get(const uint8_t *ram, uint8_t *out)
{
    memcpy(out, ram + (LAP_ADDR & 0x1FFF), LAP_LEN);
    memcpy(out + LAP_LEN, ram + (PLY_ADDR & 0x1FFF), PLY_LEN);
}

static void put(uint8_t *ram, const uint8_t *in)
{
    memcpy(ram + (LAP_ADDR & 0x1FFF), in, LAP_LEN);
    memcpy(ram + (PLY_ADDR & 0x1FFF), in + LAP_LEN, PLY_LEN);
}

void hs_init(HS *h)
{
    memset(h, 0, sizeof *h);
}

int hs_load(HS *h, const uint8_t *file, size_t n)
{
    if (n != HS_FILE_BYTES || memcmp(file, "DVHS", 4) != 0 || file[4] != 1) return 0;
    uint32_t crc = (uint32_t)file[73] | (uint32_t)file[74] << 8 | (uint32_t)file[75] << 16 | (uint32_t)file[76] << 24;
    if (crc != crc32_of(file, 73)) return 0;
    memcpy(h->saved, file + 5, HS_BYTES);
    h->have = 1;
    return 1;
}

void hs_image(const HS *h, uint8_t out[HS_FILE_BYTES])
{
    memcpy(out, "DVHS", 4);
    out[4] = 1;
    memcpy(out + 5, h->last, HS_BYTES);
    uint32_t crc = crc32_of(out, 73);
    out[73] = (uint8_t)crc; out[74] = (uint8_t)(crc >> 8); out[75] = (uint8_t)(crc >> 16); out[76] = (uint8_t)(crc >> 24);
}

int hs_frame(HS *h, uint8_t *ram)
{
    uint8_t cur[HS_BYTES];
    get(ram, cur);
    if (!h->applied) {
        h->frames++;
        if (memcmp(cur, defaults, HS_BYTES) == 0) {
            if (h->have) { put(ram, h->saved); memcpy(cur, h->saved, HS_BYTES); }
            memcpy(h->last, cur, HS_BYTES);
            h->applied = 1;
        } else if (h->frames > GIVE_UP_FRAMES) {
            memcpy(h->last, cur, HS_BYTES);
            h->applied = 1;
        }
        return 0;
    }
    if (memcmp(cur, h->last, HS_BYTES) != 0) {
        memcpy(h->last, cur, HS_BYTES);
        h->dirty = 1;
        h->quiet = 0;
        return 0;
    }
    if (h->dirty && ++h->quiet >= QUIET_FRAMES) {
        h->dirty = 0;
        return 1;
    }
    return 0;
}

int hs_pending(const HS *h)
{
    return h->dirty;
}

void hs_restore(HS *h, uint8_t *ram)
{
    uint8_t cur[HS_BYTES];
    if (h->applied) {
        put(ram, h->last);
    } else if (h->have) {
        put(ram, h->saved);
        memcpy(h->last, h->saved, HS_BYTES);
        h->applied = 1;
    } else {
        get(ram, cur);
        memcpy(h->last, cur, HS_BYTES);
        h->applied = 1;
    }
}
