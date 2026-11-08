# Devilish PSP - technical overview

How the port is put together, why it is built this way, and how it was checked. This document describes the *method* and the source code in this repository. It contains nothing taken from the
game; where a fact about the game matters (for instance that it patches its own sound driver at run time) it is stated in general terms, and the exact places are found by the tools from your
own ROM.

## 1. The pipeline

```
 Devilish (USA).gg  (your own dump, never leaves your machine)
        |
        |  tools/embed_rom.py --check            size + CRC32
        v
 reference interpreter  (tools/oracle/, gcc)       plays 27 scripted button sessions, records
        |                                          which instruction starts ran (cov.txt) and a RAM image
        v
 static survey          (survey.py, gaps.py)       code the ROM reaches by direct control flow, code-like gaps, tables of code addresses
        |
        v
 translator             (z80_to_c.py)              every instruction found -> one C case; sound driver from the RAM image
        |
        v
 extracted/gen/*.c      (generated, local)         mod_lo.c  mod_hi.c  mod_b2.c  mod_ram.c  gen_info.c  rom_data.c
        |
        +--- src/hw, src/rt, src/psp               hand-written: hardware model, runtime, PSP front end
        v
 psp-cmake / make  ->  EBOOT.PBP                   the game's data is inside the EBOOT, no ROM file is read at run time
```

Everything below `extracted/` is derived from the game and stays on your machine. Everything above it, and all of `src/` and `tools/`, is source code.

## 2. What has to be provided

The Game Gear is a Z80 at 3.58 MHz with 8 KB of RAM, a Sega mapper (registers at the top of the address space, three 16 KB ROM windows of which the third is switchable), a VDP in its Mode 4
(a 160 x 144 window of the Master System's picture, shown on lines 24-167 of 262 lines of 228 T-states each), an SN76489-style sound chip with four voices and a stereo mask, a pad port and
a Start button. A native port has to give the translated code each of these, with the *timing* the game relies on: the game polls the VDP's line counter, reads the sprite overflow and
collision flags, changes scroll and colours in the middle of a frame, and counts instructions' T-states and its own refresh register for a random number generator.

## 3. Finding the code

Z80 code is not marked. The translator needs the address of every instruction the game can execute, and it cannot guess: a jump into the middle of an instruction that was translated
differently would be a wrong game. It collects instruction starts from four sources:

1. **Static survey** (`survey.py`): a recursive descent from the reset and interrupt vectors that follows jumps, calls and returns, with a small constant tracker for the mapper's bank
   register so that code in the switchable window is decoded with the right bank.
2. **Code-like gaps and code-address tables** (`gaps.py`, `ptr_seeds.py`): stretches the descent did not reach that decode as a plausible instruction run and are *anchored* (they call into
   known code, touch RAM or use a known port), and runs of three or more little-endian words that all point at code. A false positive costs only some unused C.
3. **Recorded sessions**: the reference interpreter runs the game under 27 scripted button sessions and records every instruction start that executes (`cov.txt`). This is what finds the code
   behind computed jumps (`jp (hl)`, `push de` / `ret` dispatch) that no static analysis can follow.
4. **Annotations** (`annotations.txt`): the few entry points and tables that were found by reading the game, as addresses.

The translator closes the union under direct control flow and translates it. **There is no fallback interpreter.** If the finished game ever jumps to an address that was not translated, the
runtime stops with `MISS at pc=$XXXX` and a short reason instead of guessing. That is deliberate: a miss is reported, never silently wrong. The scripted sessions were chosen (and the miss
messages used) to drive the number of misses over all the game's stages to zero.

The game's data lies between and after its code, and some of it is read at run time like code bytes would be. The kit therefore embeds the whole 128 KB image as a C array (`rom_data.c`) and
the translated code reads its data from it; the translated instructions themselves are C, not bytes.

## 4. The translation: a switch machine

Each ROM region (`lo` and `hi` = the two fixed 16 KB banks, `b2` = the one switchable bank that holds code) becomes one C function with a `switch (M.pc)` and a label per instruction.
`src/rt/z80rt.h` holds the meaning of every instruction as macros; the generated code expands them inline. A few properties matter:

* **The machine state is a global struct** (`M`: registers, shadow registers, IX / IY, SP, PC, interrupt flags, the refresh counter, and the T-states not yet given to the hardware).
* **Calls and returns use the emulated stack in RAM**, so `push` / `ret` tricks, `jp (hl)` dispatch and self-made jump tables work unchanged.
* **Every instruction ends in one control-flow macro** (`IEND`, `IJP`, `ICALL`, `IRET`, ... in `z80mod.h`) that adds its T-states and checks whether the hardware or an interrupt needs
  attention. If so the function returns to the runtime with `M.pc` set; otherwise control flows to the next case. Between any two instructions the machine can therefore be paused, which
  is what makes interrupts, the frame callback and save states exact.
* **Flags are exact**, including the undocumented bits, and the **R register** is advanced per M1 cycle (the game's random numbers come from it). The macros were validated against a
  reference Z80 core that passes the standard zexdoc / zexall exercisers.
* Instructions the translator cannot model exactly (a few rarely used corners) are refused at translation time, loudly.

## 5. The sound driver

The game unpacks a Z80 sound driver into RAM during start-up and calls it once per frame; its code exists only in RAM, so the ROM survey cannot see it. The build therefore captures the RAM
image from the reference interpreter (`--ramdump`) and translates it as a fourth module, `mod_ram.c`, with `annotations_ram.json`:

* The driver **patches itself** at a handful of places: operand bytes that it overwrites (read from RAM at run time instead of being baked into the C) and a few opcode bytes that it
  toggles between `nop` and `ret` as a mute switch (translated as both alternatives).
* The runtime checks a CRC of every other code byte of the driver once per frame and stops loudly if the game ever changes them.

`CMakeLists.txt` builds this driver automatically; the build needs no other sound code.

## 6. The runtime (`src/rt/`)

* `rt_run()` dispatches on `M.pc` to the module that owns it and runs until the host stops it. `rt_slow()` is the one place where hardware time advances: at the end of each scanline (or
  when an interrupt can be taken) it gives the hardware the accumulated T-states, raises the line and frame interrupts, calls the host's frame callback when a frame is complete and
  delivers the interrupt.
* **Time is the reference's time**: an instruction runs first (port accesses see the hardware as of its start), then its T-states are added. The picture, the port-access log and the audio
  of the translated game are therefore bit for bit those of the reference interpreter.
* **Save states** (`state.c`) store the whole machine at a frame boundary: Z80 registers and timing counters, the 8 KB RAM (which holds the sound driver's variables too), the VDP with VRAM,
  colour RAM, registers and counters, the sound chip, the mapper, the pad. The image is little-endian bytes in a fixed order with a magic, a version, the ROM CRC32 and a trailing CRC.
  A state can only be taken or loaded inside the frame callback, where the program is at a plain instruction boundary (`rt_state_safe()`).
* **High scores** (`hiscore.c`): the game keeps its two score tables in RAM and resets them at every power-on (the cartridge has no battery). The host watches the tables at the end of
  each frame, puts the saved values in as soon as the game has written its defaults, and writes the file a couple of seconds after the tables change. The game is not modified.
  The defaults used to recognise the start-up state are generated from your ROM (`gen_info.c`), not stored in the source.
* **Menu back** (Circle / Triangle): the original has no way back once a game mode is chosen. The runtime works out which menu screen is active from the return address of the call the main
  loop is inside, and restarts the main loop at the previous screen with the stack and interrupt state reset to what that point of the original flow has; nothing else is touched. A request
  counts for four frames and is edge-triggered, so that Circle, which is also the game's button 2, never backs out of a menu that appears later.

## 7. Video (`src/hw/`, `src/psp/gerender.c`)

The first design drew the picture line by line on the CPU, like the VDP does. The final one lets the PSP's graphics engine draw whole frames:

1. **The VDP model** (`hw.c`) keeps VRAM, colour RAM, registers, the line counter and the interrupt logic, and answers port reads, as it would on the Game Gear. It is shared with the
   reference interpreter.
2. **The scene recorder** (`scene.inc`) watches what the game does to the picture between lines 24 and 191. Every change to something the picture depends on (a colour, a scroll or display
   register, a name-table or sprite-table byte, a tile that something refers to) is written to an *undo log*, and a *band* is cut at the first line that can see it. A frame with no mid-frame
   effects is one band. The state of an earlier band is obtained by undoing the log backwards, so quiet frames cost almost nothing.
3. **The draw-list builder** (`scenedraw.inc`) turns the bands into a list of background cells, sprite runs (with the VDP's eight-sprites-per-line rule and its draw order) and four colour
   tables, plus a tile atlas that also keeps the old version of tiles rewritten during the frame. What the builder does not support and the game never needs (zoomed sprites, the vertical
   scroll lock) is refused loudly.
4. **The GE backend** (`gerender.c`) draws each band with a scissor into a 160 x 144 render target as textured quads from the 4-bit atlas through 16-entry colour tables; sprites use a table
   whose colour 0 is transparent and an alpha test. One display list per frame is submitted asynchronously, so the GE draws while the CPU runs the next frame. The target is then stretched to the
   480 x 272 screen: *fit* (302 x 272, the Game Gear's 10:9 at full height), *full width*, *1x* or *1.5x*, sharp or bilinear.
5. The sprite overflow and collision flags the game polls are computed by a small routine in `hw.c` (the PSP build compiles the CPU line renderer out).

The line-by-line renderer still exists as the *reference*: in the author's tests the GE's picture is compared with it pixel for pixel on every frame of every scripted session.

## 8. Sound

* `psg.c` is a closed-form mixer for the chip's three square voices and one noise voice: 2 dB volume steps, the Game Gear stereo mask, 44.1 kHz output, integer arithmetic only so that the PC
  and PSP builds produce identical samples. A voice flips at most once per output sample, so the number of "on" chip ticks in a sample is computed, not stepped; the output equals the
  tick-by-tick synthesiser it replaced bit for bit.
* The PSP front end feeds the samples to a ring buffer read by the PSP's audio thread, and **the audio clock paces the game**: the game waits when the ring is full, so there is no drift
  between sound and picture.

## 9. The PSP front end (`src/psp/`)

* Controls and the options menu (`main.c`, `config.c`); the menu draws over the picture and stops the game while it is open. Options live in `data/config.ini` (`screen`, `filter`,
  `volume`, `buttons`; unknown keys are ignored).
* The program uses `MEMSIZE 1` (the extra RAM of the 2000 / 3000 / Go models); about 49 MB was measured free on a PSP-3000.
* **Self-test mode.** If `data/auto` exists (a frame count) the game runs that many frames unattended, driven by `data/script.txt` (lines `FROM TO button[+button]`, buttons `up down left
  right 1 2 start`, `#!` header lines for options), and writes to `data/out/`: `hash.txt` (a CRC of every frame's picture), `audio_crc.txt` (sample count and CRC of the audio),
  `perf.txt` (time per frame on the PSP's clock, audio underruns), a few frame dumps, `screen_check.txt` (the stretched screen against the source picture) and `done.txt`. The same file
  formats come out of the reference interpreter and a native PC build, which is how the author compared all three. The scripts in `tests/scripts/` are input only.
* **Recorder.** If `data/record` exists, a played session writes its buttons per frame in the script format, so a real play can be replayed anywhere.

## 10. How it was checked (the author's tests, not part of the kit)

* The translator's output was run against the reference core instruction by instruction (over 2 million generated cases: every encoding with many operand and flag combinations) and results, flags and timing agree.
* Twenty long scripted sessions (menus, every time trial stage, normal game stages, a recorded play, a bot that plays the paddle game to clear stages) run on the reference interpreter,
  a native PC build of the translated game and PPSSPP: **the per-frame picture CRCs and the audio CRC are identical on all twenty**. Several of them (stage 3, a bot-played game, a
  long normal-game run) were also run on a PSP-3000 and matched.
* Save, save state and options were tested by scripts that restore the state or change the options in the middle of a session and compare against a run without the interruption.
* Timing on the PSP-3000 at 333 MHz: 60 fps, the game itself takes about 2 to 6 ms of the 16.7 ms frame depending on the scene; the console runs the code about 1.2 times slower than PPSSPP.

## 11. Source map

| Path | What |
|---|---|
| `build.sh` | the whole build |
| `CMakeLists.txt` | PSP build (with the translated sound driver) |
| `tools/z80_dis.py` | Z80 decoder shared by the tools |
| `tools/survey.py`, `gaps.py`, `ptr_seeds.py` | code discovery |
| `tools/z80_to_c.py` | the translator |
| `tools/annotations.txt`, `annotations_ram.json` | entry points, tables and self-modifying cells, as addresses |
| `tools/embed_rom.py` | ROM check and the embedded data array |
| `tools/check_generated.py` | hashes of the generated code |
| `tools/oracle/` | the reference interpreter (a plain Z80 core with the shared hardware model) |
| `src/rt/` | runtime: machine state, dispatch, scanline clock, interrupts, menu back, save states, high scores |
| `src/hw/` | VDP model, scene recorder, draw-list builder, sound mixer |
| `src/psp/` | PSP front end: GE renderer, input, audio output, options menu, self-test and recorder |
| `tests/scripts/` | scripted button sessions (input only) |

## 12. Not done, and ideas

* XMB music (`SND0.AT3`); an optional Game Gear LCD look for the picture.
* More than one save state slot.
* The PSP-1000 (the program wants the extra RAM of the later models).
