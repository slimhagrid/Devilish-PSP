#!/bin/bash
# Devilish PSP build kit: builds EBOOT.PBP from YOUR OWN copy of the game ROM "Devilish (USA).gg" (128 KB, CRC32 C01293B0).
#
#   bash build.sh [path/to/Devilish\ \(USA\).gg]
#
# Needs a Linux shell (or WSL on Windows) with gcc, python3 and the PSP toolchain (psp-cmake, see README.md). Takes about 10-20 minutes, most of it compiling.
# Everything the steps produce (extracted/, build/, out/) is derived from the ROM: it stays on your machine (the .gitignore of this folder keeps it out of git).
set -e
cd "$(dirname "$0")"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
say() { echo; echo "=== $*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1 ($2)"; exit 1; }; }

# --- the ROM -------------------------------------------------------------------------------------------------------------------------------------
ROM="$1"
if [ -z "$ROM" ]; then
    n=$(ls ./*.gg 2>/dev/null | wc -l)
    [ "$n" = 1 ] || { echo "put your ROM (Devilish (USA).gg) in this folder, or pass its path:  bash build.sh path/to/rom.gg"; exit 1; }
    ROM=$(ls ./*.gg)
fi
[ -f "$ROM" ] || { echo "no such file: $ROM"; exit 1; }
# the tools look for the ROM as the only .gg file in this folder
if [ "$(ls ./*.gg 2>/dev/null | wc -l)" != 1 ]; then cp "$ROM" "./Devilish (USA).gg"; fi
ROM=$(ls ./*.gg)

need gcc "sudo apt install build-essential"
need python3 "sudo apt install python3"
export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PATH:$PSPDEV/bin"
need psp-cmake "install the PSP toolchain (pspdev), see README.md"

say "1/6 checking the ROM"
python3 tools/embed_rom.py "$ROM" --check

# --- the reference interpreter: used only here, to find out which code the game runs --------------------------------------------------------------
say "2/6 building the reference interpreter (a build tool, not part of the PSP program)"
mkdir -p work/oracle
gcc -O2 -w -Isrc -DHW_REF_FLAGS -o work/oracle/oracle tools/oracle/oracle.c tools/oracle/z80.c src/hw/hw.c src/hw/psg.c

say "3/6 playing the scripted sessions in it (records which code the game runs; $(wc -l < tools/kit_scenarios.txt) sessions, $JOBS at a time)"
mkdir -p extracted/oracle
run_one() {
    n="$1"
    frames=$(grep -m1 '^#! frames' "tests/scripts/$n.txt" | awk '{print $3}')
    mkdir -p "extracted/oracle/$n"
    extra=""
    [ "$n" = "new_game_stage1" ] && extra="--ramdump 100 extracted/oracle/ram_image.bin"      # the sound driver the game unpacks into RAM at boot
    ./work/oracle/oracle "$ROM" --frames "$frames" --script "tests/scripts/$n.txt" --cov "extracted/oracle/$n/cov.txt" --ind "extracted/oracle/$n/ind.txt" $extra > /dev/null
    echo "  $n ($frames frames)"
}
export -f run_one
export ROM
xargs -a tools/kit_scenarios.txt -P "$JOBS" -I{} bash -c 'run_one {}'
[ -f extracted/oracle/ram_image.bin ] || { echo "the sound driver image was not written"; exit 1; }

say "4/6 translating the game (Z80 -> C)"
python3 tools/survey.py > /dev/null
python3 tools/gaps.py > /dev/null
python3 tools/z80_to_c.py
python3 tools/embed_rom.py "$ROM"
python3 tools/check_generated.py

say "5/6 compiling for the PSP (the big generated modules take a few minutes each)"
mkdir -p build
(cd build && psp-cmake .. > cmake.log 2>&1 && make -j"$JOBS")

say "6/6 packaging"
mkdir -p out/DevilishPSP
cp build/EBOOT.PBP out/DevilishPSP/EBOOT.PBP
cp pbp/README.txt out/DevilishPSP/README.txt
echo
ls -l out/DevilishPSP
echo
echo "Done. Copy the folder out/DevilishPSP to ms0:/PSP/GAME/ on your Memory Stick."
echo "The EBOOT contains the game's data: it is for your own use, do not share it."
