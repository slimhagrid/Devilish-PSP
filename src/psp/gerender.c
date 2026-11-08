/* GE backend of the draw list (M6c / V3): draws a frame's bands (src/hw/scenedraw.h) into a 160 x 144 render target in video memory with the PSP's graphics engine.
 * Background cells and sprite rows are textured quads (GU_SPRITES) from the 4-bit tile atlas (RAM, linear, 256 x 256 texels) through 16-entry colour tables; the sprite and
 * priority passes use tables whose colour 0 has alpha 0 and an alpha test. Everything is submitted as one display list; the GE runs while the CPU emulates the next frame.
 * The reference for what it must produce is sd_ref_raster() (src/hw/scenedraw.c): tests compare the target with the line renderer's picture, pixel for pixel.
 */
#include <pspkernel.h>
#include <pspgu.h>
#include <stdint.h>
#include <string.h>
#include "gerender.h"

#define VRAM_ABS 0x04000000u
#define VRAM_CPU 0x44000000u
#define TARGET_OFF 0x198000u            /* after three 512 x 272 x 4 frame buffers (3 x 0x88000) */
#define TARGET_STRIDE 256
/* the Game Gear picture on the 480 x 272 screen (default: 302 x 272 centred, the CPU blit's geometry); ge_set_view() changes it */
static int view_x = 89, view_y = 0, view_w = 302, view_h = 272, view_smooth, view_clear = 4;
#define DEPTH_OFF 0x1C0000u             /* never written: depth test and depth writes are off */

typedef struct { uint16_t u, v; int16_t x, y, z; } TV;      /* GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D */
#define TVTYPE (GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D)

static uint32_t list[524288] __attribute__((aligned(64)));  /* 2 MB: the most cells (20,000) and sprite runs (8,192) the draw list can hold take about 0.6 MB */
static int ge_busy;
static unsigned screen_off = 0xFFFFFFFFu;      /* VRAM offset of the display buffer the stretched picture goes to (0xFFFFFFFF: draw the 160 x 144 target only) */

void ge_init(void)
{
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)TARGET_OFF, TARGET_STRIDE);
    sceGuDepthBuffer((void *)DEPTH_OFF, 64);
    sceGuOffset(2048 - 80, 2048 - 72);
    sceGuViewport(2048, 2048, 160, 144);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, 160, 144);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();
    sceGuSync(0, 0);
}

void ge_set_screen(unsigned vram_offset) { screen_off = vram_offset; }
void ge_set_view(int x, int y, int w, int h, int smooth) { view_x = x; view_y = y; view_w = w; view_h = h; view_smooth = smooth; view_clear = 4; }
void ge_clear_screen(void) { view_clear = 4; }

void ge_wait(void)
{
    if (ge_busy) { sceGuSync(0, 0); ge_busy = 0; }
}

static void quads_cells(const SdCell *c, int n)
{
    if (!n) return;
    TV *v = (TV *)sceGuGetMemory((unsigned)n * 2 * sizeof(TV));
    for (int i = 0; i < n; i++, c++) {
        int u0 = (c->slot & 31) * 8, v0 = (c->slot >> 5) * 8;
        int ua = u0, ub = u0 + 8, va = v0, vb = v0 + 8, t;
        if (c->flags & SD_FLIP_H) { t = ua; ua = ub; ub = t; }
        if (c->flags & SD_FLIP_V) { t = va; va = vb; vb = t; }
        v[2 * i].u = (uint16_t)ua; v[2 * i].v = (uint16_t)va; v[2 * i].x = c->x; v[2 * i].y = c->y; v[2 * i].z = 0;
        v[2 * i + 1].u = (uint16_t)ub; v[2 * i + 1].v = (uint16_t)vb; v[2 * i + 1].x = (int16_t)(c->x + 8); v[2 * i + 1].y = (int16_t)(c->y + 8); v[2 * i + 1].z = 0;
    }
    sceGuDrawArray(GU_SPRITES, TVTYPE, n * 2, 0, v);
}

static void quads_sprites(const SdSpr *s, int n)
{
    if (!n) return;
    TV *v = (TV *)sceGuGetMemory((unsigned)n * 2 * sizeof(TV));
    for (int i = 0; i < n; i++, s++) {
        int u0 = (s->slot & 31) * 8, v0 = (s->slot >> 5) * 8 + s->r0;
        v[2 * i].u = (uint16_t)u0; v[2 * i].v = (uint16_t)v0; v[2 * i].x = s->x; v[2 * i].y = s->y; v[2 * i].z = 0;
        v[2 * i + 1].u = (uint16_t)(u0 + 8); v[2 * i + 1].v = (uint16_t)(v0 + s->nrows); v[2 * i + 1].x = (int16_t)(s->x + 8); v[2 * i + 1].y = (int16_t)(s->y + s->nrows); v[2 * i + 1].z = 0;
    }
    sceGuDrawArray(GU_SPRITES, TVTYPE, n * 2, 0, v);
}

void ge_draw(const SdList *l, const SdAtlas *a)
{
    sceKernelDcacheWritebackRange(a->data, SD_ATLAS_BYTES);
    sceKernelDcacheWritebackRange(l->band, (unsigned)l->nbands * sizeof(SdBand));
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBufferList(GU_PSM_8888, (void *)TARGET_OFF, TARGET_STRIDE);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_T4, 0, 0, 0);
    sceGuTexImage(0, SD_ATLAS_W, SD_ATLAS_W, SD_ATLAS_W, a->data);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    for (int b = 0; b < l->nbands; b++) {
        const SdBand *B = &l->band[b];
        sceGuScissor(0, B->y0, 160, B->y1 - B->y0);
        if (!B->display_on) {
            sceGuClearColor(B->backdrop);
            sceGuClear(GU_COLOR_BUFFER_BIT);
            continue;
        }
        sceGuDisable(GU_ALPHA_TEST);
        for (int k = 0; k < 2; k++) {
            if (!B->cell_n[k]) continue;
            sceGuClutLoad(2, B->clut[SD_CLUT_BG0 + k]);
            quads_cells(&l->cell[B->cell_first[k]], B->cell_n[k]);
        }
        sceGuEnable(GU_ALPHA_TEST);
        if (B->spr_n) {
            sceGuClutLoad(2, B->clut[SD_CLUT_SPR]);
            quads_sprites(&l->spr[B->spr_first], B->spr_n);
        }
        for (int k = 2; k < 4; k++) {
            if (!B->cell_n[k]) continue;
            sceGuClutLoad(2, B->clut[SD_CLUT_BG0T + (k - 2)]);
            quads_cells(&l->cell[B->cell_first[k]], B->cell_n[k]);
        }
    }
    sceGuDisable(GU_ALPHA_TEST);
    if (screen_off != 0xFFFFFFFFu) {
        /* the finished 160 x 144 picture, stretched to the view rectangle (ge_set_view; nearest or bilinear); the area around it is cleared for 4 frames after a change, never otherwise */
        sceGuTexFlush();                                 /* the target was just written by this list and is now read as a texture */
        sceGuTexSync();
        if (view_smooth) {                               /* bilinear sampling at the right / bottom edge would blend with what lies beyond the picture: repeat the last column and row one texel further */
            sceGuDrawBufferList(GU_PSM_8888, (void *)TARGET_OFF, TARGET_STRIDE);
            sceGuScissor(0, 0, 161, 145);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, 256, 256, TARGET_STRIDE, (void *)(VRAM_ABS + TARGET_OFF));
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
            TV *e = (TV *)sceGuGetMemory(2 * sizeof(TV));
            e[0].u = 159; e[0].v = 0; e[0].x = 160; e[0].y = 0; e[0].z = 0;
            e[1].u = 160; e[1].v = 144; e[1].x = 161; e[1].y = 144; e[1].z = 0;
            sceGuDrawArray(GU_SPRITES, TVTYPE, 2, 0, e);
            sceGuTexFlush();
            sceGuTexSync();
            e = (TV *)sceGuGetMemory(2 * sizeof(TV));
            e[0].u = 0; e[0].v = 143; e[0].x = 0; e[0].y = 144; e[0].z = 0;
            e[1].u = 161; e[1].v = 144; e[1].x = 161; e[1].y = 145; e[1].z = 0;
            sceGuDrawArray(GU_SPRITES, TVTYPE, 2, 0, e);
            sceGuTexFlush();
            sceGuTexSync();
        }
        sceGuDrawBufferList(GU_PSM_8888, (void *)screen_off, 512);
        sceGuScissor(0, 0, 480, 272);
        if (view_clear > 0) {                           /* the view or something drawn over the bars changed: clear the whole buffer first (the next 4 frames reach every buffer) */
            view_clear--;
            sceGuClearColor(0xFF000000u);
            sceGuClear(GU_COLOR_BUFFER_BIT);
        }
        sceGuTexFilter(view_smooth ? GU_LINEAR : GU_NEAREST, view_smooth ? GU_LINEAR : GU_NEAREST);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, 256, 256, TARGET_STRIDE, (void *)(VRAM_ABS + TARGET_OFF));
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        TV *v = (TV *)sceGuGetMemory(2 * sizeof(TV));
        v[0].u = 0; v[0].v = 0; v[0].x = (int16_t)view_x; v[0].y = (int16_t)view_y; v[0].z = 0;
        v[1].u = 160; v[1].v = 144; v[1].x = (int16_t)(view_x + view_w); v[1].y = (int16_t)(view_y + view_h); v[1].z = 0;
        sceGuDrawArray(GU_SPRITES, TVTYPE, 2, 0, v);
    }
    sceGuFinish();
    ge_busy = 1;
}

void ge_readback(uint32_t *dst)
{
    ge_wait();
    const uint32_t *src = (const uint32_t *)(VRAM_CPU + TARGET_OFF);
    /* the GE does not keep a destination alpha in cleared areas and the screen ignores it: compare the colour (the line renderer's pictures have alpha $FF everywhere) */
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) dst[y * 160 + x] = src[y * TARGET_STRIDE + x] | 0xFF000000u;
}
