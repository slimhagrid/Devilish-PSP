Devilish PSP - native port of the Game Gear game "Devilish"

Install:  copy the folder DevilishPSP to  ms0:/PSP/GAME/  on the Memory Stick (so that EBOOT.PBP is at ms0:/PSP/GAME/DevilishPSP/EBOOT.PBP).
          Nothing else is needed: the game's data is inside EBOOT.PBP, there is no ROM file to copy and none is read.
          The program creates data/ next to EBOOT.PBP for its own files (devilish.sav, state1.sav, config.ini, out/).

What it is: the game's own Z80 code translated to C (no CPU emulator), with PSP-native video (drawn by the GE) and a native sound engine.

Controls:  D-pad / analog nub = move, Cross or Square = button 1, Circle or Triangle = button 2, Start = Start, HOME exits.
Menus:     Circle or Triangle goes back one screen (OPTIONS / SELECT STAGE -> SELECT GAMES -> title). The original game has no way back; during play nothing changes.
Options:   SELECT + START opens a pause menu (the game stands still): Screen (fit height / full width / 1x / 1.5x), Picture (sharp / smooth),
           Buttons (1 and 2 swapped), Resume, Exit. UP/DOWN choose, LEFT/RIGHT change, Cross select, Circle or SELECT + START closes.
           (Volume: use the PSP's own volume buttons.)
           The choices are kept in data/config.ini.
Save state: SELECT + R saves the game at once into data/state1.sav, SELECT + L loads it back (also after a restart).
           A green square in the left black bar says it was saved, a blue one that it was loaded, a red one that nothing was done.
High scores: BEST PLAYERS and the time trial BEST LAP TIMES are kept in data/devilish.sav between runs; delete the file to start again.

If the game stops with "MISS at pc=$XXXX" the text names code the translator did not find: report it.
data/out/ gets perf.txt / fatal.txt for the hardware checks.

This EBOOT.PBP contains the game's data and code derived from the user's own Game Gear ROM: it is for private use only, do not share it.
