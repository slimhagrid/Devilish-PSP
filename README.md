# Devilish PSP

A native port of **Devilish** (Sage's Creation, 1991, licensed by Sega, Sega Game Gear, USA release) to the Sony PSP.

It is **not an emulator**: the game's own Z80 machine code is translated to C ahead of time and compiled for the PSP's MIPS CPU, and the translated game runs on a small runtime that
provides what the Game Gear hardware did (memory map, video, sound chip, input, timers). The picture is drawn by the PSP's graphics engine, the sound is synthesised natively, and the
game's own data is taken from **your own copy of the cartridge**.

> ## Read this first - what this repository is, and is not
>
> * **This repository contains no part of the game.** No ROM, no program code, graphics, music, text or any other data of Devilish are included, and none may ever be added
>   (see `.gitignore`).
> * **You need your own copy of Devilish for the Sega Game Gear (USA)**, as a ROM dump of a cartridge you own, made by yourself: `Devilish (USA).gg`, 131,072 bytes, CRC32 `C01293B0`.
>   The tools in `tools/` read that file and write the translated game to your own computer. Other dumps and other regions are refused.
> * The generated code, the data embedded in it and the finished `EBOOT.PBP` **are derived from the game's copyrighted work. Do not upload, share or redistribute them.** Only the
>   source code of this repository is free to use, and it is released into the public domain (see `LICENSE`: the Unlicense).
> * This is an unofficial, non-commercial fan project. It is not affiliated with, authorised or endorsed by Sega or Sage's Creation. *Devilish*, *Game Gear*, *Sega* and all
>   related names, characters and artwork are the property of their respective owners.
> * Nothing here is legal advice. Check what is allowed where you live.

## What you need

| | |
|---|---|
| **The game** | Devilish (USA) for the Sega Game Gear as a ROM dump of your own cartridge: `Devilish (USA).gg`, 128 KB, CRC32 `C01293B0`. Other releases are not supported |
| **A PSP** with custom firmware (PRO, ARK-4 ...) that runs homebrew, or **PPSSPP** on PC / Android | tested on a PSP-3000 and in PPSSPP; built for the extra RAM of the PSP-2000 / 3000 / Go (`MEMSIZE 1`), the PSP-1000 has not been tried |
| **A Memory Stick** | about 5 MB free |
| **Linux, or WSL2 (Ubuntu) on Windows** | to run the build; keep the kit on the Linux side (your home folder), builds on `/mnt/c/...` are much slower |
| **Python 3.9+** | no extra packages: the tools use the standard library only |
| **gcc** | builds the small reference interpreter that the build uses to find out which code the game runs (`sudo apt install build-essential`) |
| **The PSP toolchain** | [pspdev](https://pspdev.github.io/) (`psp-cmake`, `psp-gcc`, `pack-pbp` ...) |

About 100 MB of disk and 10 minutes for the build, most of it compiling the big generated modules.

## Step by step

### 1. Get your ROM

Dump your own Devilish cartridge to a `.gg` file with any Game Gear dumper. The build checks the size and the CRC32 and stops if they do not match (`Devilish (USA).gg`, 131,072 bytes, CRC32
`C01293B0`). Put the file in the kit folder, or pass its path to the build script.

### 2. Install the tools

```bash
sudo apt install build-essential python3

# PSP toolchain: follow https://pspdev.github.io/installation.html, then
export PSPDEV=$HOME/pspdev            # wherever you installed it
export PATH=$PATH:$PSPDEV/bin
```

**Windows (the route this was developed and tested on):** install WSL2 with Ubuntu (`wsl --install -d Ubuntu`) and do everything, the tools and the build, inside it.

**Linux:** the same commands run natively. This route has not been tested by the author yet, please report problems.

### 3. Build

```bash
cp "/path/to/Devilish (USA).gg" .        # or: bash build.sh "/path/to/Devilish (USA).gg"
bash build.sh
```

This runs, in order:

1. checks the ROM (size and CRC32);
2. builds the reference interpreter (`tools/oracle/`), a development tool that is not part of the PSP program;
3. plays 27 scripted button sessions in it (menus, the time trial stages, normal game stages, a long recorded play), recording which instructions of the game run (a few minutes,
   sessions run in parallel);
4. translates: a static survey of the ROM (`survey.py`, `gaps.py`), then `z80_to_c.py` turns every instruction found by the survey, the recorded sessions and the game's own jump tables
   into C, and does the same for the sound driver the game unpacks into RAM at start-up; `embed_rom.py` puts the game's data into a C array; `check_generated.py` compares hashes of the
   results with the ones the kit was released with;
5. compiles everything for the PSP with `psp-cmake` / `make` (the generated modules take a few minutes each);
6. assembles the game folder `out/DevilishPSP/` (`EBOOT.PBP` and `README.txt`).

If step 4 reports a different hash, the translation is not the one the author tested (a different ROM dump, or the sessions did not cover the same code). The build may still work.

Optional: put your own `ICON0.png` (144x80), `PIC1.png` (480x272), `PIC0.png`, `SND0.AT3` into `pbp/` before building for the XMB icon and background (see `pbp/ART.txt`). No artwork is
included here, and the game's own art must not be committed.

### 4. Put it on the PSP

1. Copy the folder `out/DevilishPSP` to the Memory Stick as `ms0:/PSP/GAME/DevilishPSP` (so that `EBOOT.PBP` is in that folder).
2. Start **Devilish PSP** from *Game -> Memory Stick* in the XMB.
3. Saves, the settings file and a few logs are written to a `data/` folder next to `EBOOT.PBP` (`devilish.sav`, `state1.sav`, `config.ini`, `out/`).

**PPSSPP:** copy the same folder to `<PPSSPP memstick>/PSP/GAME/DevilishPSP`, or open `EBOOT.PBP` with *File -> Load*.

## Controls

| Button | |
|---|---|
| D-pad / analog nub | move |
| CROSS or SQUARE | button 1 (launch the ball, confirm) |
| CIRCLE or TRIANGLE | button 2; **in the game's menus: back one screen** (OPTIONS / SELECT STAGE -> SELECT GAMES -> title) |
| START | Start |
| SELECT + START | **options menu** (the game stands still) |
| SELECT + R | **quick save** (saves at once into `data/state1.sav`) |
| SELECT + L | quick load (also after a restart) |
| HOME | exit |

A green square in the left black bar says a state was saved, a blue one that it was loaded, a red one that nothing was done.

The options menu (SELECT + START) has **Screen** (fit height / full width / 1x / 1.5x), **Picture** (sharp / smooth) and **Buttons** (1 and 2 swapped), then Resume game and Exit game.
UP/DOWN choose, LEFT/RIGHT change, CROSS select, CIRCLE or SELECT + START closes. Your choices are kept in `data/config.ini`. For the volume use the PSP's own volume buttons.

## What is in the game

* NORMAL GAME and TIME TRIAL with the game's own menus, title and attract screens, continue and game over screens, and its own OPTIONS screen.
* BEST PLAYERS and the time trial BEST LAP TIMES are kept on the Memory Stick between runs (`data/devilish.sav`; delete the file to start again). The Game Gear cartridge itself has no
  battery.
* A save state (one slot) that works anywhere in play and also after a restart.
* Back out of the game's menus with Circle: the original has no way back once NORMAL GAME or TIME TRIAL is chosen.

## How it works, in short

* **Translator** (`tools/`, Python): `z80_dis.py` decodes Z80 code; `survey.py`, `gaps.py` and `ptr_seeds.py` find the code in the ROM (a static descent, code-like gaps, tables of code
  addresses) and the recorded sessions add what actually runs; `z80_to_c.py` writes one C `switch (pc)` machine per ROM region with a case for every instruction, flags and timing exactly as
  the reference interpreter has them. The sound driver, which exists only in RAM after start-up, is translated from the RAM image the interpreter captured.
  `annotations.txt` and `annotations_ram.json` hold the few entry points and self-modifying cells that cannot be found automatically, as addresses.
* **Runtime** (`src/rt/`, C): the machine state, the scanline clock, interrupts, dispatch between the translated modules, the menu back action, save states and high scores. A jump to
  code the translator did not find stops the game loudly (`MISS at pc=$XXXX`) instead of guessing.
* **Hardware layer** (`src/hw/`): a model of the Game Gear's video chip state, a scene recorder that turns a frame's VDP activity into a list of drawing bands, a builder that turns
  those into GE draw lists, and a mixer for the sound chip's four voices.
* **PSP front end** (`src/psp/`): the GE renderer and its scaling modes, input, audio output, the options menu, and a self-test that replays a button script and checks the picture and the
  audio against the reference.
* **Reference interpreter** (`tools/oracle/`): a plain Z80 interpreter with the same hardware model. The build uses it to learn which code the game runs; it is a development tool and
  is not part of the PSP program.

See `docs/TECHNICAL.md` for more: the pipeline, how the code is found, how the sound driver and the video are handled, how it was verified, and a source map.

## Testing (optional)

```bash
python3 tools/check_generated.py       # the generated code has the hashes of the author's build (build.sh runs this)
```

The author's own checks compare the translated game with the reference interpreter picture by picture and sample by sample over 20 long scripted sessions, natively and in PPSSPP. They
are not part of this kit.

## Known limits

* Tested on a PSP-3000 and in PPSSPP; the PSP-1000 has not been tried (the game is built for the extra RAM of the 2000 / 3000 models).
* The sound chip is synthesised by a native mixer that is checked sample for sample against a reference synthesiser; it was judged by ear on the PSP, not compared with a real Game Gear.
* No XMB music (`SND0.AT3` needs an AT3 encoder, which is not part of this kit).
* One save state slot.

## Contributing

Pull requests are welcome for the source code. **Never commit anything derived from the game** (the ROM, `extracted/`, the built `EBOOT.PBP`, dumps, disassembly listings, extracted text,
graphics or sound; whole-screen screenshots are fine in issues but not in the repository). `.gitignore` already excludes the usual places; please check `git status` before every commit.

## License

The source code in this repository is released into the public domain under the [Unlicense](https://unlicense.org): use it, change it, sell it, no attribution needed. That covers this
code only, never the game or anything made from your copy of it.

## Credits

Devilish is a game by Sage's Creation, licensed by Sega (as its title screen says). This port is a fan project that needs, and respects, the original. Built with the
[pspdev](https://github.com/pspdev) toolchain. It was tested on a real PSP.
