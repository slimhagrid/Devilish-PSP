/* Devilish PSP - the translated game on the PSP (picture, controls and sound).
 *
 * Controls (D8, default): D-pad / analog nub = D-pad, CROSS or SQUARE = button 1, CIRCLE or TRIANGLE = button 2, START = Start.  HOME exits.
 * Data: none outside the EBOOT (M9): the 128 KB of game data the translated code reads is built in (extracted/gen/rom_data.c, tools/embed_rom.py); data/ only holds what the program writes
 * (high scores, save state, recordings, self-test output) and the self-test scripts.
 *
 * Self-test: if data/auto exists ("<frames>") the game runs that many frames unattended with the button script data/script.txt (same format as the oracle:
 * "FROM TO button[+button]" per line, buttons up down left right 1 2 start), writes data/out/hash.txt (CRC32 of every frame's picture, the same file the oracle
 * and the native host write with --hash), data/out/perf.txt (time per frame on the PSP's clock), a few frame dumps and data/out/done.txt, then exits.
 * It also writes data/out/audio_crc.txt ("samples N crc32 X", the same line `native --audio-crc` writes for the same scenario).
 * It does not wait for vsync and draws only every 60th frame so that it runs as fast as it can.  tools/game_check.py stages and runs it in the private PPSSPP.
 *
 * Recorder: if data/record exists, a played run writes the buttons per frame to data/rec_N.txt in the script format (see "input recorder" below); HOME closes it.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspaudio.h>
#include <pspge.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rt/z80rt.h"
#include "../rt/hiscore.h"
#include "../rt/state.h"
#include "config.h"
#include "gerender.h"

PSP_MODULE_INFO("DevilishPSP", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(-1024);

/* Where the program keeps its files: a data/ folder NEXT TO THE EBOOT, wherever the user put the game folder (the path comes from argv[0] = ".../<folder>/EBOOT.PBP"). Nothing is ever
 * created outside that folder: a folder in ms0:/PSP/GAME/ without an EBOOT.PBP shows up as "Corrupted data" in the XMB. The default is only used if the system gives no path. */
#define BASE_DEFAULT "ms0:/PSP/GAME/DevilishPSP"
static char g_base[256] = BASE_DEFAULT, g_data[288], g_out[320];
static int g_base_known;                                  /* the game folder came from argv[0] (it exists: the EBOOT is in it) */
static const char *pth(const char *dir, const char *name)    /* dir + "/" + name in one of a few static buffers (use at once) */
{
    static char buf[4][400];
    static unsigned k;
    char *o = buf[k++ & 3];
    snprintf(o, sizeof buf[0], "%s/%s", dir, name);
    return o;
}
static void set_paths(int argc, char **argv)
{
    if (argc > 0 && argv && argv[0]) {
        const char *a = argv[0], *sl = strrchr(a, '/');
        if (sl && sl > a && strchr(a, ':') && (size_t)(sl - a) < sizeof g_base) { memcpy(g_base, a, (size_t)(sl - a)); g_base[sl - a] = 0; g_base_known = 1; }
    }
    snprintf(g_data, sizeof g_data, "%s/data", g_base);
    snprintf(g_out, sizeof g_out, "%s/out", g_data);
}
#define AUTOFILE pth(g_data, "auto")
#define SCRIPTFILE pth(g_data, "script.txt")
#define CONFIGFILE pth(g_data, "config.ini")        /* options (M9): screen size, filter, volume, buttons; changed in the pause menu */
#define STATEFILE pth(g_data, "state1.sav")        /* the quick save slot (M8, src/rt/state.c): SELECT + R saves, SELECT + L loads */
#define SAVEFILE pth(g_data, "devilish.sav")     /* the high-score tables (M8, src/rt/hiscore.c) */

#define ROM_CRC32 0xC01293B0u              /* identifies the game in save states (the embedded image has this CRC, checked when it was generated) */
extern const uint8_t gen_rom_image[];
extern const uint32_t gen_rom_size, gen_rom_crc32;

#define SCR_W 480
#define SCR_H 272
#define BUF_W 512
#define FB_BYTES (BUF_W * SCR_H * 4)
#define GG_FRAME_US 16688.0       /* 228 * 262 T-states at 3.579545 MHz = 59.92 Hz */

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---- exit / fatal -------------------------------------------------------------------------------------------------------------------------- */
static Config cfg;
static HS hs;
static int hs_on;                         /* high scores are kept (not during a self-test or a recording: both must replay exactly) */
static volatile int quit_req;             /* HOME while recording: the game thread closes the recording at the end of its frame, then exits */
static int recording;
static void write_file(const char *path, const void *data, size_t n);
static void hs_write(void)
{
    u8 img[HS_FILE_BYTES];
    hs_image(&hs, img);
    write_file(SAVEFILE, img, sizeof img);
}
static int exit_cb(int a1, int a2, void *c)
{
    (void)a1; (void)a2; (void)c;
    if (hs_on && hs_pending(&hs)) hs_write();
    if (recording) { quit_req = 1; sceKernelDelayThread(400000); }     /* give the game thread a few frames to write the last lines */
    sceKernelExitGame();
    return 0;
}
static int cb_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", exit_cb, NULL));
    sceKernelSleepThreadCB();
    return 0;
}

static void write_file(const char *path, const void *data, size_t n)
{
    /* data/ (and data/out/ for diagnostics) are created when something is first written there: a normal player's data/ holds only what the game saves */
    if (!g_base_known) sceIoMkdir(g_base, 0777);          /* only the fallback folder; the real game folder is where the EBOOT already is */
    sceIoMkdir(g_data, 0777);
    if (strstr(path, "/out/")) sceIoMkdir(g_out, 0777);
    SceUID f = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (f < 0) return;
    sceIoWrite(f, data, n);
    sceIoClose(f);
}

static void ensure_dirs(void)
{
    if (!g_base_known) sceIoMkdir(g_base, 0777);          /* only the fallback folder; the real game folder is where the EBOOT already is */
    sceIoMkdir(g_data, 0777);
    sceIoMkdir(g_out, 0777);
}

/* overrides the runtime's weak default: the message goes to a file and to the screen (a translated-code miss, a changed RAM program, a bad ROM) */
void rt_fatal(const char *msg)
{
    ensure_dirs();
    write_file(pth(g_out, "fatal.txt"), msg, strlen(msg));
    pspDebugScreenInit();
    pspDebugScreenPrintf("Devilish PSP stopped:\n\n%s\n\nHOME exits.\n", msg);
    for (;;) sceDisplayWaitVblankStart();
}

static u8 *read_file(const char *path, size_t *size)
{
    SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (f < 0) return NULL;
    SceOff len = sceIoLseek(f, 0, PSP_SEEK_END);
    sceIoLseek(f, 0, PSP_SEEK_SET);
    u8 *buf = (u8 *)malloc((size_t)len + 1);
    if (!buf) { sceIoClose(f); return NULL; }
    int got = sceIoRead(f, buf, (unsigned)len);
    sceIoClose(f);
    if (got != (int)len) { free(buf); return NULL; }
    buf[len] = 0;
    *size = (size_t)len;
    return buf;
}

static u32 crc_table[256];
static u32 crc32_update(u32 crc, const u8 *p, size_t n)
{
    if (!crc_table[1]) for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static u64 now_us(void) { return sceKernelGetSystemTimeWide(); }

/* ---- video --------------------------------------------------------------------------------------------------------------------------------- */
/* The picture is drawn by the GE (src/psp/gerender.c) from the scene recorder's draw list (src/hw/scenedraw.c), stretched from 160x144 to 302x272 (nearest, the Game Gear's 10:9
 * picture at full screen height) straight into the frame buffer that is shown next. Three frame buffers rotate through sceDisplaySetFrameBuf: the buffer being drawn is never the
 * one on screen nor the one waiting for the next vblank, so a flip without a vblank wait cannot tear. The sides stay black: they are cleared once here and never drawn again. */
#define VRAM_CPU 0x44000000u   /* uncached CPU view of the 2 MB of VRAM */
#define VRAM_GE 0x04000000u    /* the address the display hardware wants */
#define NBUF 3
#define OUT_W 302
static int cv_cur;
static unsigned shown_off;
static u64 vwait_us, present_work_us;

static void video_init(void)
{
    sceDisplaySetMode(0, SCR_W, SCR_H);
    for (int b = 0; b < NBUF; b++) {
        u32 *p = (u32 *)(VRAM_CPU + (unsigned)b * FB_BYTES);
        for (int i = 0; i < BUF_W * SCR_H; i++) p[i] = 0xFF000000u;
    }
    sceDisplaySetFrameBuf((void *)VRAM_GE, BUF_W, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
}

/* ---- audio --------------------------------------------------------------------------------------------------------------------------------- */
/* The PSG model renders 44.1 kHz stereo (src/hw/psg.c) into a ring buffer: once per frame from on_frame and before every PSG register write. A high-priority thread
 * feeds the ring to the audio hardware in 512-frame blocks. Audio-clock pacing: on_frame waits while more than HIGH frames are queued, so the game runs at the audio
 * hardware's rate (the real 59.92 Hz, not the screen's 59.94 Hz). If the ring runs dry the thread plays silence for that block (counted as an underrun). */
#define ABUF 512                          /* stereo frames per hardware block (multiple of 64) */
#define RING_FRAMES (ABUF * 16)           /* 8192 frames = 186 ms */
#define HIGH_FRAMES (ABUF * 5)            /* about 58 ms queued ahead: a slow frame cannot drain it */
static int16_t __attribute__((aligned(64))) aring[RING_FRAMES * 2];
static volatile unsigned a_wr, a_rd;      /* frames written / read, ever increasing */
static volatile int audio_run, audio_ready;
static int audio_ch = -1;
static volatile unsigned audio_underruns;
/* underrun diagnostics (written by the audio thread, read when the perf report is written): the ring fill at the underrun, how long ago the game last handed over a frame, and the longest
 * time the audio hardware took for a block (a block is 11.6 ms; much more means the hardware thread itself was held up) */
static volatile unsigned ud_fill_min = 0xFFFFFFFFu, ud_n_window;
static volatile unsigned ud_since_frame_max_us, ud_block_max_us;
static volatile u64 ud_last_frame_us;
static unsigned fill_min = 0xFFFFFFFFu;
static u64 fill_sum, fill_n;

static void audio_push(const int16_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned w = a_wr;
        if (w - a_rd >= RING_FRAMES) break;   /* cannot happen with the pacing below; never overwrite unread audio */
        int v = cfg.volume;
        aring[(w % RING_FRAMES) * 2] = v >= 100 ? s[i * 2] : (int16_t)(s[i * 2] * v / 100);
        aring[(w % RING_FRAMES) * 2 + 1] = v >= 100 ? s[i * 2 + 1] : (int16_t)(s[i * 2 + 1] * v / 100);
        a_wr = w + 1;
    }
}

static int audio_thread(SceSize args, void *argp)
{
    static int16_t __attribute__((aligned(64))) blk[ABUF * 2];
    (void)args; (void)argp;
    while (audio_run) {
        u64 tb0 = sceKernelGetSystemTimeWide();
        if (a_wr - a_rd >= ABUF) {
            unsigned r = a_rd;
            for (int i = 0; i < ABUF; i++) { blk[i * 2] = aring[((r + (unsigned)i) % RING_FRAMES) * 2]; blk[i * 2 + 1] = aring[((r + (unsigned)i) % RING_FRAMES) * 2 + 1]; }
            a_rd = r + ABUF;
        } else {
            memset(blk, 0, sizeof blk);
            audio_underruns++;
            ud_n_window++;
            unsigned fl = a_wr - a_rd, since = (unsigned)(tb0 - ud_last_frame_us);
            if (fl < ud_fill_min) ud_fill_min = fl;
            if (since > ud_since_frame_max_us) ud_since_frame_max_us = since;
        }
        sceAudioOutputBlocking(audio_ch, PSP_AUDIO_VOLUME_MAX, blk);
        { unsigned bt = (unsigned)(sceKernelGetSystemTimeWide() - tb0); if (bt > ud_block_max_us) ud_block_max_us = bt; }
    }
    return 0;
}

static int audio_init(void)
{
    audio_ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, ABUF, PSP_AUDIO_FORMAT_STEREO);
    if (audio_ch < 0) return 0;
    audio_run = 1;
    SceUID th = sceKernelCreateThread("audio_thread", audio_thread, 0x12, 0x4000, 0, NULL);
    if (th < 0) return 0;
    sceKernelStartThread(th, 0, NULL);
    audio_ready = 1;
    return 1;
}

/* self-test: the CRC of everything the PSG produced (compare with the native host's --audio-crc) */
static u32 acrc;
static u64 asamples;
static void audio_crc_sink(const int16_t *s, int n)
{
    acrc = crc32_update(acrc, (const u8 *)s, (size_t)n * 4);
    asamples += (u64)n;
}

/* ---- input --------------------------------------------------------------------------------------------------------------------------------- */
static unsigned last_buttons;             /* raw PSP buttons of the last read (the recorder's hot keys look at L and R) */
static u8 read_pad(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned b = pad.Buttons;
    last_buttons = b;
    u8 m = 0;
    if ((b & PSP_CTRL_UP) || pad.Ly < 64) m |= PAD_UP;
    if ((b & PSP_CTRL_DOWN) || pad.Ly > 192) m |= PAD_DOWN;
    if ((b & PSP_CTRL_LEFT) || pad.Lx < 64) m |= PAD_LEFT;
    if ((b & PSP_CTRL_RIGHT) || pad.Lx > 192) m |= PAD_RIGHT;
    if (b & (PSP_CTRL_CROSS | PSP_CTRL_SQUARE)) m |= cfg.swapped ? PAD_2 : PAD_1;
    if (b & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) m |= cfg.swapped ? PAD_1 : PAD_2;
    if (b & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) m |= PAD_BACK;                 /* in the menus Circle / Triangle go back one screen (rt_back_filter); in play it does nothing extra */
    if ((b & PSP_CTRL_START) && !(b & PSP_CTRL_SELECT)) m |= PAD_START;       /* SELECT + START is the menu chord: the game does not see that Start */
    return m;
}

/* ---- self-test ------------------------------------------------------------------------------------------------------------------------------ */
typedef struct { u8 mask; long from, to; } Press;
#define PRESS_MAX 65536                   /* the longest bot scenarios have about 17,000 presses; a played recording of a whole game has a few thousand */
static Press presses[PRESS_MAX];
static int npress;
static int autotest;
static int diag;               /* diagnostics (perf.txt / perflog.txt, frame dumps): the self-test, or a normal run with an (empty) file data/diag; a normal player gets none of these files */
static long auto_frames;
static char *hashbuf;
static size_t hashlen;

static u8 button_mask(const char *tok)
{
    static const struct { const char *n; u8 m; } t[] = {{"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT}, {"1", PAD_1}, {"2", PAD_2}, {"start", PAD_START}, {"back", PAD_BACK}};
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(tok, t[i].n)) return t[i].m;
    return 0;
}

static void add_press(const char *names, long from, long to)
{
    u8 mask = 0;
    char buf[128], *save = NULL;
    strncpy(buf, names, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *tok = strtok_r(buf, "+ \t\r", &save); tok; tok = strtok_r(NULL, "+ \t\r", &save)) mask |= button_mask(tok);
    if (npress < PRESS_MAX) { presses[npress].mask = mask; presses[npress].from = from; presses[npress].to = to; npress++; }
}

static struct { long frame, to; int addr, val; } pokes[256];     /* "#! poke FRAME ADDR VAL" lines of the script: RAM writes at the end of a frame (as in the oracle and the native host) */
static int npokes;
static long script_state_save = -1, script_state_load = -1;   /* "#! state-save FRAME" / "#! state-load FRAME": the self-test takes a save state at the first safe frame boundary from FRAME on (kept in memory) and loads it back at the first safe one from FRAME2 on: the run rewinds, and every frame must still equal the oracle's */
static struct { long frame; char text[64]; int done; } script_cfg[16];     /* "#! cfg FRAME screen=full filter=smooth ...": the self-test changes the options at that frame (tests of the screen modes) */
static int n_script_cfg;
static long script_menu = -1;          /* "#! menu FRAME": the self-test draws the options menu once at that frame and dumps the screen (to see the panel without a console) */
static int script_save;                   /* "#! save" in a script: the high scores are kept in this self-test too (tools/save_check.sh, M8) */

static void parse_script(char *text)
{
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        long from, to;
        char names[128];
        long pf, pt; int pa, pv;
        if (!strncmp(line, "#! save", 7)) { script_save = 1; continue; }
        if (sscanf(line, "#! cfg %ld %63[^\n]", &pf, names) == 2 && n_script_cfg < 16) { script_cfg[n_script_cfg].frame = pf; snprintf(script_cfg[n_script_cfg].text, sizeof script_cfg[0].text, "%.63s", names); n_script_cfg++; continue; }
        if (sscanf(line, "#! menu %ld", &pf) == 1) { script_menu = pf; continue; }
        if (sscanf(line, "#! state-save %ld", &pf) == 1) { script_state_save = pf; continue; }
        if (sscanf(line, "#! state-load %ld", &pf) == 1) { script_state_load = pf; continue; }
        if (sscanf(line, "#! poke %ld %i %i", &pf, &pa, &pv) == 3 && npokes < 256) { pokes[npokes].frame = pokes[npokes].to = pf; pokes[npokes].addr = pa; pokes[npokes].val = pv; npokes++; continue; }
        if (sscanf(line, "#! hold %ld %ld %i %i", &pf, &pt, &pa, &pv) == 4 && npokes < 256) { pokes[npokes].frame = pf; pokes[npokes].to = pt; pokes[npokes].addr = pa; pokes[npokes].val = pv; npokes++; continue; }
        char *h = strchr(line, '#');
        if (h) *h = 0;
        if (sscanf(line, "%ld %ld %127s", &from, &to, names) == 3) add_press(names, from, to);
    }
}

static u8 script_pad(long frame)
{
    u8 held = 0;
    for (int i = 0; i < npress; i++) if (frame >= presses[i].from && frame <= presses[i].to) held |= presses[i].mask;
    return held;
}

/* ---- input recorder (M7.3) --------------------------------------------------------------------------------------------------------------- */
/* If data/record exists, a normal (played) run writes every change of the buttons to data/rec_N.txt (first N not in use) in the script format: one line
 * "FROM TO button[+button]" per stretch of frames with the same buttons held (frame numbers count from the start of the program, as in a self-test script),
 * and a final "# end FRAMES".  The file is appended to about once a minute (a few lines; a write to the memory stick can stall for a few ms) and when HOME is pressed.
 * Replaying it with data/auto + data/script.txt (or on the oracle / the native host) gives the same game, because the game only sees the buttons per frame.
 * Hot keys: data/record may contain lines "L ADDR VAL" / "R ADDR VAL" (ADDR as a Z80 RAM address $C000-$DFFF or offset, C-style numbers; several lines per key).
 * Pressing the key writes those bytes into the game's RAM at the end of that frame and logs it as "#! poke FRAME ADDR VAL", which every replayer applies at the same
 * frame; so a recording can use cheats (more lives, a boss at 1 hit point) and still be replayed exactly. */
typedef struct { int addr, val; } HotPoke;
static HotPoke hot[2][8];
static int nhot[2];
static int rec_synth;
static unsigned hot_prev;
static int hot_pending;                  /* bit 0 = L, bit 1 = R pressed since the last frame */
static char recpath[400];
static char recbuf[16384];
static size_t reclen;
static u8 rec_cur;                        /* buttons now held (what the game will see in the next frame) */
static long rec_start;                    /* first frame of the current stretch */
static long rec_frame;                    /* the frame being run */

/* The memory stick can stall a write for a second or more (the first recording had one burst of 83 audio underruns at a minute mark), so the periodic write
 * is done by a low-priority thread: the game thread only copies the few hundred bytes. HOME / the end of the run writes synchronously. */
static char recwr[sizeof recbuf];
static size_t recwr_len;
static volatile int rec_wr_busy;
static SceUID rec_sema = -1;

static void rec_write(const char *p, size_t n)
{
    SceUID f = sceIoOpen(recpath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (f >= 0) { sceIoWrite(f, p, n); sceIoClose(f); }
}

static int rec_writer(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(rec_sema, 1, NULL);
        rec_write(recwr, recwr_len);
        rec_wr_busy = 0;
    }
    return 0;
}

static void rec_flush(int sync)
{
    if (!reclen) return;
    if (rec_sema < 0 || sync) {
        while (rec_wr_busy) sceKernelDelayThread(1000);        /* keep the order of the lines */
        rec_write(recbuf, reclen);
        reclen = 0;
        return;
    }
    if (rec_wr_busy) return;                                   /* the last write is still going: try again at the next opportunity */
    memcpy(recwr, recbuf, reclen);
    recwr_len = reclen;
    reclen = 0;
    rec_wr_busy = 1;
    sceKernelSignalSema(rec_sema, 1);
}

static void rec_put(const char *s, int n)
{
    if (n <= 0) return;
    if (reclen + (size_t)n > sizeof recbuf) rec_flush(1);
    memcpy(recbuf + reclen, s, (size_t)n);
    reclen += (size_t)n;
}

static void rec_stretch(long from, long to, u8 m)
{
    static const struct { u8 m; const char *n; } t[] = {{PAD_UP, "up"}, {PAD_DOWN, "down"}, {PAD_LEFT, "left"}, {PAD_RIGHT, "right"}, {PAD_1, "1"}, {PAD_2, "2"}, {PAD_START, "start"}};
    char names[48] = "";
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (m & t[i].m) { if (names[0]) strcat(names, "+"); strcat(names, t[i].n); }
    char line[96];
    rec_put(line, snprintf(line, sizeof line, "%ld %ld %s\n", from, to, names));
}

static void rec_open(void)
{
    for (int n = 1; n < 1000; n++) {
        snprintf(recpath, sizeof recpath, "%s/rec_%d.txt", g_data, n);
        SceIoStat st;
        if (sceIoGetstat(recpath, &st) < 0) break;
    }
    static const char head[] = "# Devilish PSP input recording (script format; frames count from the start of the program)\n";
    rec_put(head, (int)sizeof head - 1);
    rec_flush(1);
    rec_sema = sceKernelCreateSema("rec", 0, 0, 1, NULL);
    SceUID th = rec_sema >= 0 ? sceKernelCreateThread("rec_writer", rec_writer, 0x30, 0x4000, 0, NULL) : -1;
    if (th >= 0) sceKernelStartThread(th, 0, NULL); else rec_sema = -1;     /* (without the thread every flush is a synchronous write) */
}

static void rec_parse_config(char *text)
{
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char key; int a, v;
        if (!strncmp(line, "TEST-PRESSES", 12)) rec_synth = 1;     /* test of the recorder without a controller (PPSSPP harness): left / right every 25 frames */
        if (sscanf(line, " %c %i %i", &key, &a, &v) == 3 && (key == 'L' || key == 'R')) {
            int k = key == 'R';
            if (nhot[k] < 8) { hot[k][nhot[k]].addr = a; hot[k][nhot[k]].val = v; nhot[k]++; }
        }
    }
}

/* start of on_frame(f), where the scripted pokes are applied: apply the hot keys pressed during the last frame */
static void rec_hotkeys(long f)
{
    rec_frame = f;
    if (!recording || !hot_pending) return;
    for (int k = 0; k < 2; k++) {
        if (!(hot_pending & (1 << k))) continue;
        for (int i = 0; i < nhot[k]; i++) {
            G.ram[hot[k][i].addr & 0x1FFF] = (uint8_t)hot[k][i].val;
            char line[64];
            rec_put(line, snprintf(line, sizeof line, "#! poke %ld 0x%04x 0x%02x\n", f, hot[k][i].addr & 0x1FFF, hot[k][i].val & 0xFF));
        }
    }
    hot_pending = 0;
}

/* end of on_frame(f): read the buttons for frame f+1 and log a change */
static u8 rec_pad(long f)
{
    u8 m = read_pad();
    if (rec_synth) m = (f / 25) % 3 == 0 ? PAD_LEFT : (f / 25) % 3 == 1 ? PAD_RIGHT : 0;
    unsigned lr = last_buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER);
    unsigned fresh = lr & ~hot_prev;
    hot_prev = lr;
    if (fresh & PSP_CTRL_LTRIGGER) hot_pending |= 1;
    if (fresh & PSP_CTRL_RTRIGGER) hot_pending |= 2;
    if (m != rec_cur) {
        if (rec_cur) rec_stretch(rec_start, f, rec_cur);
        rec_cur = m;
        rec_start = f + 1;
    }
    if (quit_req) {
        if (rec_cur && rec_start <= f) rec_stretch(rec_start, f, rec_cur);     /* (a stretch that started with this read has not been used by any frame yet) */
        char line[48];
        rec_put(line, snprintf(line, sizeof line, "# end %ld\n", f + 1));
        rec_flush(1);
        sceKernelExitGame();
    }
    static int due;
    if ((f + 1) % 3600 == 0) due = 1;
    if (due && !rec_wr_busy) { rec_flush(0); due = 0; }
    return m;
}

static u32 ge_pic[GG_W * GG_H];         /* the GE's 160 x 144 target of the frame, read back (self-test only) */
static u64 ge_t_pre, ge_acc_build, ge_acc_submit, ge_acc_stall, ge_n;   /* GE path timing in the current perf window (us): builder, submitting the list, waiting for the GE */
static u64 chk_frames, chk_frames_bad_b, chk_side_bad;

/* called by the scene builder at line 192, before it rewrites the atlas: the GE must be done with the previous frame (it normally is long since) */
static void ge_pre(void)
{
    u64 t = now_us();
    ge_wait();
    ge_acc_stall += now_us() - t;
    ge_t_pre = now_us();
}

/* called when the draw list is complete: the time since ge_pre is the recorder's undo / builder work; then the GE draws this frame into the buffer that the next flip shows */
static void ge_frame(const SdList *l, const SdAtlas *a)
{
    u64 t = now_us();
    ge_acc_build += t - ge_t_pre;
    ge_set_screen((unsigned)cv_cur * FB_BYTES);
    ge_draw(l, a);
    ge_acc_submit += now_us() - t;
    ge_n++;
}

/* shows the frame the GE drew (it normally finished long ago: the list was submitted at line 192 and the frame ends about 4 ms of emulation later) */
static void osd_draw(int buf);
static void present_ge(int wait_vblank)
{
    u64 ts = now_us();
    int b = cv_cur;
    ge_wait();
    ge_acc_stall += now_us() - ts;
    osd_draw(b);
    vwait_us = 0;
    if (wait_vblank) { u64 t1 = now_us(); sceDisplayWaitVblankStart(); vwait_us = now_us() - t1; }
    sceDisplaySetFrameBuf((void *)(VRAM_GE + (unsigned)b * FB_BYTES), BUF_W, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    shown_off = (unsigned)b * FB_BYTES;
    cv_cur = (b + 1) % NBUF;
    present_work_us = (now_us() - ts) - vwait_us;
}

/* self-test: is the stretched picture on the screen what the 160 x 144 target says? Every pixel of the 302 x 272 area must show the target pixel at the centre-sampled source position
 * (exact), or at most one source pixel away in x and / or y (the GE steps through the texture in fixed point, so a few boundary columns and rows round differently from the
 * formula: the picture is still a faithful nearest-neighbour scale). Anything else is an error; the side bars must be black. */
static u64 chk_exact, chk_near, chk_far;
static void screen_check(void)
{
    const u32 *scr = (const u32 *)(VRAM_CPU + shown_off);
    int vx, vy, vw, vh;
    cfg_view(cfg.screen, &vx, &vy, &vw, &vh);
    unsigned far = 0, side = 0;
    for (int dy = 0; dy < SCR_H; dy++) {
        const u32 *row = scr + dy * BUF_W;
        int inside_y = dy >= vy && dy < vy + vh;
        for (int dx = 0; dx < SCR_W; dx++) {
            u32 p = row[dx] & 0xFFFFFFu;
            if (!inside_y || dx < vx || dx >= vx + vw) { if (p) side++; continue; }
            int sy = (2 * (dy - vy) + 1) * GG_H / (2 * vh), sx = (2 * (dx - vx) + 1) * GG_W / (2 * vw);
            if (!cfg.smooth) {
                if (p == (ge_pic[sy * GG_W + sx] & 0xFFFFFFu)) { chk_exact++; continue; }
                int ok = 0;
                for (int yy = sy - 1; yy <= sy + 1 && !ok; yy++)
                    for (int xx = sx - 1; xx <= sx + 1 && !ok; xx++)
                        if (xx >= 0 && xx < GG_W && yy >= 0 && yy < GG_H && p == (ge_pic[yy * GG_W + xx] & 0xFFFFFFu)) ok = 1;
                if (ok) chk_near++; else { chk_far++; far++; }
            } else {                    /* bilinear: every colour channel lies between the smallest and the largest of the source pixels around (the GE blends in fixed point: 2 levels of slack) */
                int ok = 1;
                for (int c = 0; c < 24 && ok; c += 8) {
                    int lo = 255, hi = 0;
                    for (int yy = sy - 1; yy <= sy + 1; yy++)
                        for (int xx = sx - 1; xx <= sx + 1; xx++)
                            if (xx >= 0 && xx < GG_W && yy >= 0 && yy < GG_H) { int v = (int)((ge_pic[yy * GG_W + xx] >> c) & 0xFF); if (v < lo) lo = v; if (v > hi) hi = v; }
                    int v = (int)((p >> c) & 0xFF);
                    if (v < lo - 2 || v > hi + 2) ok = 0;
                }
                if (ok) chk_near++; else { chk_far++; far++; }
            }
        }
    }
    chk_frames++;
    chk_side_bad += side;
    if (far || side) chk_frames_bad_b++;
}

static void dump_rgb(long f)
{
    static u8 rgb[GG_W * GG_H * 3];
    char name[400];
    for (int i = 0; i < GG_W * GG_H; i++) {
        u32 p = ge_pic[i];
        rgb[i * 3] = (u8)p; rgb[i * 3 + 1] = (u8)(p >> 8); rgb[i * 3 + 2] = (u8)(p >> 16);
    }
    snprintf(name, sizeof name, "%s/frame_%05ld.rgb", g_out, f);
    write_file(name, rgb, sizeof rgb);
}

/* the 480x272 picture that is on screen (read back from VRAM) */
static void dump_screen(long f)
{
    static u8 row[SCR_W * 3];
    char name[400];
    const u32 *fbv = (const u32 *)(VRAM_CPU + shown_off);
    snprintf(name, sizeof name, "%s/screen_%05ld.rgb", g_out, f);
    SceUID fd = sceIoOpen(name, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            u32 p = fbv[y * BUF_W + x];
            row[x * 3] = (u8)p; row[x * 3 + 1] = (u8)(p >> 8); row[x * 3 + 2] = (u8)(p >> 16);
        }
        sceIoWrite(fd, row, sizeof row);
    }
    sceIoClose(fd);
}

/* ---- the frame hook ------------------------------------------------------------------------------------------------------------------------ */
#define PERF_WINDOW 300
static u64 t_last_end, t_acc_emu, t_acc_present, t_max_emu, t_win_start, t_acc_tail, t_max_work;
static u64 p_render, p_audio;           /* sprite-flag logic / PSG synthesis time on the profiled frames (every 8th) of this window */
static long perf_frames, late_frames, p_frames;
#define PROF_EVERY 8

static void write_perf(long frames)
{
    if (!diag) return;
    char text[3072];
    double n = (double)(perf_frames ? perf_frames : 1);
    double wall_s = (double)(now_us() - t_win_start) / 1e6, fps = wall_s > 0 ? perf_frames / wall_s : 0;
    int len = snprintf(text, sizeof text,
                       "frames %ld  window %ld frames   achieved %.1f fps (a real Game Gear runs at 59.92)\n"
                       "emulate  avg %.2f ms  max %.2f ms   (budget %.2f ms per frame)\n"
                       "present  avg %.2f ms   (showing the GE's frame without the vsync wait; in the self-test it is mostly the frame CRC)\n"
                       "work per frame (emulate + audio tail + present): avg %.2f ms  max %.2f ms\n"
                       "  profiled on %ld of the frames: sprite flags avg %.2f ms, PSG synthesis avg %.2f ms (its last call of the frame, which 'emulate' leaves out, avg %.2f ms over all frames)\n"
                       "frames over budget (emulate + audio tail + present) in this window: %ld\n"
                       "audio: underruns so far %u, ring fill in this window min %u avg %u frames (pacing target %u)\n",
                       frames, perf_frames, fps, (double)t_acc_emu / n / 1000.0, (double)t_max_emu / 1000.0, GG_FRAME_US / 1000.0, (double)t_acc_present / n / 1000.0,
                       (double)(t_acc_emu + t_acc_tail + t_acc_present) / n / 1000.0, (double)t_max_work / 1000.0,
                       p_frames, p_frames ? (double)p_render / (double)p_frames / 1000.0 : 0.0, p_frames ? (double)p_audio / (double)p_frames / 1000.0 : 0.0, (double)t_acc_tail / n / 1000.0,
                       late_frames, audio_underruns, fill_min == 0xFFFFFFFFu ? 0u : fill_min, fill_n ? (unsigned)(fill_sum / fill_n) : 0u, (unsigned)HIGH_FRAMES);
    len += snprintf(text + len, sizeof text - (size_t)len, "audio underruns in this window: %u (ring fill at an underrun: smallest %u frames; longest time since the game last handed over a frame: %.1f ms; longest audio block: %.1f ms, normal 11.6)\n",
                    ud_n_window, ud_fill_min == 0xFFFFFFFFu ? 0u : ud_fill_min, ud_since_frame_max_us / 1000.0, ud_block_max_us / 1000.0);
    ud_n_window = 0; ud_fill_min = 0xFFFFFFFFu; ud_since_frame_max_us = 0; ud_block_max_us = 0;
    {
        double gn = (double)(ge_n ? ge_n : 1);
        len += snprintf(text + len, sizeof text - (size_t)len,
                        "GE picture path (%llu frames): scene builder avg %.2f ms (undoing the log, atlas, cells, sprites), submitting the draw list avg %.2f ms, waiting for the GE avg %.2f ms\n"
                        "  (all three are CPU time inside 'emulate' / 'present'; the GE itself runs in parallel with the emulation)\n",
                        (unsigned long long)ge_n, (double)ge_acc_build / gn / 1000.0, (double)ge_acc_submit / gn / 1000.0, (double)ge_acc_stall / gn / 1000.0);
    }
    write_file(autotest ? pth(g_out, "perf.txt") : pth(g_data, "perf.txt"), text, (size_t)len);
    {   /* timeline: one line per window, appended (frame, emulate avg / max, sprite flags, audio, audio tail, GE builder / submit, work avg / max, late frames, underruns, ring min) */
        char line[256];
        double gn = (double)(ge_n ? ge_n : 1);
        int ll = snprintf(line, sizeof line, "%ld emu %.2f/%.2f flags %.2f audio %.2f tail %.2f build %.2f submit %.2f work %.2f/%.2f late %ld underruns %u ringmin %u\n",
                          frames, (double)t_acc_emu / n / 1000.0, (double)t_max_emu / 1000.0, p_frames ? (double)p_render / (double)p_frames / 1000.0 : 0.0,
                          p_frames ? (double)p_audio / (double)p_frames / 1000.0 : 0.0, (double)t_acc_tail / n / 1000.0, (double)ge_acc_build / gn / 1000.0, (double)ge_acc_submit / gn / 1000.0,
                          (double)(t_acc_emu + t_acc_tail + t_acc_present) / n / 1000.0, (double)t_max_work / 1000.0, late_frames, audio_underruns, fill_min == 0xFFFFFFFFu ? 0u : fill_min);
        SceUID lf = sceIoOpen(autotest ? pth(g_out, "perflog.txt") : pth(g_data, "perflog.txt"), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (lf >= 0) { sceIoWrite(lf, line, (size_t)ll); sceIoClose(lf); }
    }
}

/* ---- save states (M8) ------------------------------------------------------------------------------------------------------------------------- */
static u8 st_img[ST_MAX + 64];
static size_t st_len;                      /* size of the image in st_img (0 = none) */
static int st_saved_test, st_loaded_test;  /* self-test: taken / loaded */
static unsigned st_prev;                   /* buttons of the previous frame (edge detection of the hot keys) */
static int osd_frames;         /* a coloured square in the left side bar tells what happened: green = saved, blue = loaded, red = nothing done */
static u32 osd_color;
static void osd_show(u32 color) { osd_color = color; osd_frames = 90; }

static void osd_draw(int buf)
{
    if (osd_frames <= 0) return;
    u32 *p = (u32 *)(VRAM_CPU + (unsigned)buf * FB_BYTES);
    for (int y = 8; y < 32; y++) for (int x = 8; x < 32; x++) p[y * BUF_W + x] = osd_color;
    if (--osd_frames == 0) ge_clear_screen();            /* the GE clears the area around the picture again for the next frames (the square sits in the left bar) */
}

static int state_save_now(const char *file)
{
    st_len = st_save(st_img, sizeof st_img, ROM_CRC32);
    if (!st_len) return 0;
    if (file) write_file(file, st_img, st_len);
    return 1;
}

static int state_load_now(const char *file)
{
    if (file) {
        size_t n = 0;
        u8 *b = read_file(file, &n);
        if (!b) return 0;
        int ok = n <= sizeof st_img && memcpy(st_img, b, n) && st_load(st_img, n, ROM_CRC32);
        free(b);
        if (!ok) return 0;
    } else if (!st_len || !st_load(st_img, st_len, ROM_CRC32)) return 0;
    if (hs_on) hs_restore(&hs, G.ram);
    return 1;
}

/* ---- options menu (M9) ------------------------------------------------------------------------------------------------------------------------ */
static unsigned menu_prev;                 /* buttons of the previous frame (edge detection of the SELECT + START chord) */

static void apply_cfg(void)
{
    int x, y, w, h;
    cfg_view(cfg.screen, &x, &y, &w, &h);
    ge_set_view(x, y, w, h, cfg.smooth);
}

static char cfg_last[512];        /* the options text as it was loaded or last written: config.ini is only written when the options changed */
static size_t cfg_last_n;
static void cfg_save(void)
{
    char buf[512];
    size_t n = cfg_format(&cfg, buf, sizeof buf);
    if (!n || (n == cfg_last_n && !memcmp(buf, cfg_last, n))) return;
    write_file(CONFIGFILE, buf, n);
    memcpy(cfg_last, buf, n);
    cfg_last_n = n;
}

static void menu_text(int x, int y, u32 color, const char *str)
{
    for (; *str; str++, x += 8) pspDebugScreenPutChar(x, y, color, (u8)*str);
}

static void menu_center(int y, u32 color, const char *str)      /* centred on the panel (x 100..379) */
{
    menu_text(240 - 4 * (int)strlen(str), y, color, str);
}

#define MENU_ITEMS 5            /* Screen, Picture, Buttons, Resume game, Exit game (no volume: the PSP has its own volume control) */
#define MENU_Y0 44
#define MENU_Y1 192

static void menu_draw(int sel)
{
    static int text_ready;
    if (!text_ready) {      /* pspDebugScreenPutChar draws nothing until the debug screen is initialised (setup = 0: it must not change the display mode or the frame buffer, the GE owns them) */
        pspDebugScreenInitEx(NULL, PSP_DISPLAY_PIXEL_FORMAT_8888, 0);
        text_ready = 1;
    }
    u32 *base = (u32 *)(VRAM_CPU + shown_off);
    for (int y = MENU_Y0; y < MENU_Y1; y++) for (int x = 100; x < 380; x++) base[y * BUF_W + x] = 0xFF402010u;     /* dark blue panel (ABGR) */
    for (int x = 100; x < 380; x++) { base[MENU_Y0 * BUF_W + x] = 0xFFFFFFFFu; base[(MENU_Y1 - 1) * BUF_W + x] = 0xFFFFFFFFu; }
    for (int y = MENU_Y0; y < MENU_Y1; y++) { base[y * BUF_W + 100] = 0xFFFFFFFFu; base[y * BUF_W + 379] = 0xFFFFFFFFu; }
    pspDebugScreenSetBase(base);
    pspDebugScreenEnableBackColor(0);
    menu_center(54, 0xFF00D0FFu, "OPTIONS");
    static const char *const names[SCREEN_MODES] = { "fit height", "full width", "1x", "1.5x" };
    const char *items[MENU_ITEMS];
    char l0[48], l1[48], l2[48];
    snprintf(l0, sizeof l0, "Screen    < %s >", names[cfg.screen]);
    snprintf(l1, sizeof l1, "Picture   < %s >", cfg.smooth ? "smooth" : "sharp");
    snprintf(l2, sizeof l2, "Buttons   < %s >", cfg.swapped ? "1/2 swapped" : "normal");
    items[0] = l0; items[1] = l1; items[2] = l2; items[3] = "Resume game"; items[4] = "Exit game";
    for (int i = 0; i < MENU_ITEMS; i++) menu_text(124, 78 + i * 18, i == sel ? 0xFF40FF40u : 0xFFFFFFFFu, items[i]);
    for (int i = 0; i < MENU_ITEMS; i++) if (i == sel) menu_text(108, 78 + i * 18, 0xFF40FF40u, ">");
    menu_center(166, 0xFFC0C0C0u, "UP/DOWN choose  LEFT/RIGHT change");
    menu_center(178, 0xFFC0C0C0u, "CROSS select  CIRCLE back");
}

/* The pause menu: the game stands still (this runs inside the frame callback), the panel is drawn over the picture that is on the screen. Changes apply at once and are written to
 * data/config.ini when the menu closes. */
static void options_menu(void)
{
    int sel = 0, done = 0;
    unsigned prev = 0xFFFFFFFFu;
    while (!done) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned b = pad.Buttons, e = b & ~prev;
        prev = b;
        if (pad.Ly < 64) b |= PSP_CTRL_UP;
        if (pad.Ly > 192) b |= PSP_CTRL_DOWN;
        if (e & PSP_CTRL_UP) sel = (sel + MENU_ITEMS - 1) % MENU_ITEMS;
        if (e & PSP_CTRL_DOWN) sel = (sel + 1) % MENU_ITEMS;
        int d = ((e & PSP_CTRL_RIGHT) ? 1 : 0) - ((e & PSP_CTRL_LEFT) ? 1 : 0);
        if (e & PSP_CTRL_CROSS) { if (sel < 3) d = 1; }
        if (d) {
            if (sel == 0) cfg.screen = (cfg.screen + SCREEN_MODES + d) % SCREEN_MODES;
            else if (sel == 1) cfg.smooth = !cfg.smooth;
            else if (sel == 2) cfg.swapped = !cfg.swapped;
            apply_cfg();
        }
        if ((e & PSP_CTRL_CROSS) && sel == 3) done = 1;
        if ((e & PSP_CTRL_CROSS) && sel == 4) { cfg_save(); if (hs_on && hs_pending(&hs)) hs_write(); sceKernelExitGame(); }
        if (e & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) done = 1;
        if ((b & PSP_CTRL_SELECT) && (e & PSP_CTRL_START)) done = 1;
        menu_draw(sel);
        sceDisplayWaitVblankStart();
    }
    cfg_save();
    ge_clear_screen();
    for (int k = 0; k < 60; k++) {                 /* wait until the keys that closed the menu are up, so that the game does not see them */
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        if (!(pad.Buttons & (PSP_CTRL_START | PSP_CTRL_SELECT | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE))) break;
        sceDisplayWaitVblankStart();
    }
}

static void on_frame(uint64_t frame)
{
    u64 t0 = now_us();
    ud_last_frame_us = t0;
    long f = (long)frame;
    for (int k = 0; k < npokes; k++) if (f >= pokes[k].frame && f <= pokes[k].to) G.ram[pokes[k].addr & 0x1FFF] = (uint8_t)pokes[k].val;
    for (int k = 0; k < n_script_cfg; k++) if (!script_cfg[k].done && f >= script_cfg[k].frame) { script_cfg[k].done = 1; cfg_parse(&cfg, script_cfg[k].text); apply_cfg(); }
    if (hs_on && hs_frame(&hs, G.ram)) hs_write();
    rec_hotkeys(f);
    u64 emu = t_last_end ? t0 - t_last_end : 0;

    u64 ta0 = now_us();
    hw_audio_run(&G, G.cycles);   /* the rest of this frame's samples */
    hw_audio_flush(&G);
    u64 tail = now_us() - ta0;
    if (audio_ready) {            /* audio-clock pacing */
        while (a_wr - a_rd > HIGH_FRAMES) sceKernelDelayThread(500);
        unsigned fl = a_wr - a_rd;
        if (fl < fill_min) fill_min = fl;
        fill_sum += fl; fill_n++;
    }

    if (autotest) {
        ge_readback(ge_pic);
        if (hashbuf && hashlen + 24 < (size_t)auto_frames * 24 * 2 + 64) hashlen += (size_t)snprintf(hashbuf + hashlen, 24, "%ld %08x\n", f, (unsigned)crc32_update(0, (const u8 *)ge_pic, sizeof ge_pic));
        present_ge(0);
        if (f == script_menu) { menu_draw(2); dump_screen(f); }
        if (f % 50 == 0) screen_check();
        if (f % 600 == 0) { dump_rgb(f); dump_screen(f); }
    } else {
        present_ge(!audio_ready);   /* with audio pacing the screen flips at the next vblank by itself; without audio the vblank paces the game */
    }

    u64 t1 = now_us();
    u64 pres = autotest ? t1 - t0 : present_work_us;
    if (hw_prof_clock) { p_render += hw_prof_render_us; p_audio += hw_prof_audio_us; p_frames++; }
    hw_prof_render_us = hw_prof_audio_us = 0;
    hw_prof_clock = ((f + 1) % PROF_EVERY == 0) ? now_us : NULL;
    if (f > 0) {   /* frame 0 includes the start-up */
        t_acc_emu += emu;
        t_acc_present += pres;
        t_acc_tail += tail;
        if (emu + tail + pres > t_max_work) t_max_work = emu + tail + pres;
        if (emu > t_max_emu) t_max_emu = emu;
        if ((double)(emu + tail + pres) > GG_FRAME_US) late_frames++;
        perf_frames++;
        if (perf_frames == PERF_WINDOW) {
            write_perf(f + 1);
            fill_min = 0xFFFFFFFFu; fill_sum = 0; fill_n = 0;
            ge_acc_build = ge_acc_submit = ge_acc_stall = ge_n = 0;
            perf_frames = 0; t_acc_emu = t_acc_present = t_max_emu = t_acc_tail = t_max_work = 0; late_frames = 0;
            p_render = p_audio = 0; p_frames = 0;
            t_win_start = now_us();
        }
    }

    if (autotest && f + 1 >= auto_frames) {
        write_perf(f + 1);
        write_file(pth(g_out, "hash.txt"), hashbuf, hashlen);
        char at[96];
        int al = snprintf(at, sizeof at, "samples %lu crc32 %08x\n", (unsigned long)asamples, (unsigned)acrc);
        write_file(pth(g_out, "audio_crc.txt"), at, (size_t)al);
        {
            char sc[500];
            int sl = snprintf(sc, sizeof sc, "screen check of %llu frames (every 50th), %llu pixels each: %llu exactly the centre-sampled source pixel, %llu one source pixel off (fixed-point rounding of the stretch), %llu WRONG; non-black side-bar pixels %llu; frames with an error %llu\n",
                              (unsigned long long)chk_frames, (unsigned long long)(chk_frames ? (chk_exact + chk_near + chk_far) / chk_frames : 0), (unsigned long long)chk_exact, (unsigned long long)chk_near, (unsigned long long)chk_far,
                              (unsigned long long)chk_side_bad, (unsigned long long)chk_frames_bad_b);
            write_file(pth(g_out, "screen_check.txt"), sc, (size_t)sl);
        }
        if (hs_on && hs_pending(&hs)) hs_write();
        write_file(pth(g_out, "done.txt"), "ok\n", 3);
        sceIoRemove(AUTOFILE);          /* a self-test run started by hand (data/auto) switches itself off, so the next start is a normal one */
        sceKernelExitGame();
    }
    G.pad = autotest ? script_pad(f + 1) : recording ? rec_pad(f) : read_pad();
    if (!autotest) {                                    /* SELECT + START: the options menu */
        unsigned b = last_buttons, e = b & ~menu_prev;
        menu_prev = b;
        if ((b & PSP_CTRL_SELECT) && (e & PSP_CTRL_START)) { options_menu(); G.pad = read_pad(); menu_prev = last_buttons; }
    }
    if (rt_state_safe()) {
        if (autotest) {
            if (script_state_save >= 0 && !st_saved_test && f >= script_state_save) { st_saved_test = state_save_now(NULL); }
            else if (st_saved_test && script_state_load >= 0 && !st_loaded_test && f >= script_state_load) {
                st_loaded_test = state_load_now(NULL) ? 1 : -1;
                if (st_loaded_test > 0) G.pad = script_pad((long)G.frame);
            }
        } else if (!recording) {
            unsigned b = last_buttons, edge = b & ~st_prev;
            st_prev = b;
            if ((b & PSP_CTRL_SELECT) && (edge & PSP_CTRL_RTRIGGER)) osd_show(state_save_now(STATEFILE) ? 0xFF00C000u : 0xFF2020E0u);
            else if ((b & PSP_CTRL_SELECT) && (edge & PSP_CTRL_LTRIGGER)) {
                if (state_load_now(STATEFILE)) { osd_show(0xFFE08020u); G.pad = read_pad(); }
                else osd_show(0xFF2020E0u);
            }
        }
    }
    rt_back_filter(&G.pad);
    t_last_end = now_us();
}

int main(int argc, char **argv)
{
    set_paths(argc, argv);
    SceUID th = sceKernelCreateThread("update_thread", cb_thread, 0x11, 0xFA0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    size_t asz = 0;
    char *autotxt = (char *)read_file(AUTOFILE, &asz);
    { SceIoStat dst; diag = autotxt != NULL || sceIoGetstat(pth(g_data, "diag"), &dst) >= 0; }
    if (diag) ensure_dirs();
    if (autotxt) {
        autotest = 1;
        auto_frames = atol(autotxt);
        if (auto_frames <= 0) auto_frames = 600;
        free(autotxt);
        hashbuf = (char *)malloc((size_t)auto_frames * 24 * 2 + 64);       /* (twice: a self-test with a state load repeats frames) */
        size_t ssz = 0;
        char *st = (char *)read_file(SCRIPTFILE, &ssz);
        if (st) { parse_script((char *)st); free(st); }
    }

    if (!autotest) {
        size_t rsz = 0;
        char *rc = (char *)read_file(pth(g_data, "record"), &rsz);
        if (rc) { recording = 1; rec_parse_config(rc); free(rc); rec_open(); }
    }

    if ((!autotest && !recording) || script_save) {
        hs_init(&hs);
        size_t ssz = 0;
        u8 *sv = read_file(SAVEFILE, &ssz);
        if (sv) { hs_load(&hs, sv, ssz); free(sv); }
        hs_on = 1;
    }

    if (gen_rom_crc32 != ROM_CRC32) rt_fatal("The built-in game data is not Devilish (USA) (CRC32 C01293B0):\nrun tools/embed_rom.py and rebuild.");

    cfg_defaults(&cfg);
    if (!autotest) {                                    /* (a self-test always runs with the defaults) */
        size_t csz = 0;
        char *ct = (char *)read_file(CONFIGFILE, &csz);
        if (ct) { char *c2 = (char *)realloc(ct, csz + 1); if (c2) { c2[csz] = 0; cfg_parse(&cfg, c2); ct = c2; } free(ct); }
    }
    cfg_last_n = cfg_format(&cfg, cfg_last, sizeof cfg_last);       /* what is on disk (or the defaults, nothing on disk) */
    video_init();
    hw_scene_on = 1;
    ge_init();
    apply_cfg();
    hw_scene_pre_build = ge_pre;
    hw_scene_frame_cb = ge_frame;
    if (autotest) hw_audio_sink = audio_crc_sink;
    else if (audio_init()) hw_audio_sink = audio_push;
    rt_init(gen_rom_image, gen_rom_size);
    rt_frame_cb = on_frame;
    G.pad = autotest ? script_pad(0) : 0;
    t_win_start = now_us();
    rt_run();
    return 0;
}
