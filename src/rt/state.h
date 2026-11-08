/* Save states (M8): the whole machine at a frame boundary as a byte image (Z80 registers and timing, the 8 KB RAM which holds the sound driver's state too, the VDP with its VRAM /
 * CRAM / registers / line counters, the PSG registers and synthesiser state, the mapper, the pad). Taken and loaded inside the frame callback, where rt_state_safe() says the program is
 * at a plain instruction boundary. Every field is written as little-endian bytes in a fixed order (not a dump of the structs), so the layout does not depend on the compiler; the native host and the PSP build each load their own images (an image made by one loaded by the other is not tested).
 *
 * Image: "DVST", version byte, ROM CRC32 (so a state of another game is refused), payload length, payload, CRC32 of everything before it.
 */
#ifndef STATE_H
#define STATE_H
#include <stddef.h>
#include <stdint.h>

#define ST_MAX 28000                       /* a safe upper bound of the image size */

size_t st_save(uint8_t *out, size_t cap, uint32_t rom_crc);       /* the image, or 0 if cap is too small */
int st_load(const uint8_t *in, size_t n, uint32_t rom_crc);       /* 1 = loaded; 0 = refused (nothing was changed) */

#endif
