Animal Crossing: City Folk Recompiled - Windows x64
==================================================

A native PC port of Animal Crossing: City Folk (Wii). No game data is included: the game is
built on your PC from your own disc the first time you start it.

REQUIREMENTS
- Windows 10 or 11, 64-bit, a Direct3D 12 graphics card.
- About 2 GB of free disk space (the game module built at the first start, shader caches).
- Your own City Folk disc image (ISO, WBFS, RVZ, WIA, CISO, GCZ) or a Dolphin
  "Extract Entire Disc" folder: USA (RUUE01), Europe (RUUP01), Japan (RUUJ01) or
  Korea (RUUK01), revision 1.

HOW TO RUN
1. Unpack the zip anywhere (not inside Program Files).
2. Start CityFolkRecomp.exe.
3. The first start shows the setup screen: pick your disc, then "Install". The game code is
   translated and compiled for your PC (about a minute) and the shaders are prepared.

In the game, F1 opens the options menu; Esc closes it.

WHERE FILES GO
    %APPDATA%\Animal Crossing City Folk Recompiled\   settings.ini, install.ini, game module,
                                                     Wii NAND (saves), shader caches, last_run.log
Portable mode: create portable.txt next to the game; everything then goes into .\portable.

REPORTING PROBLEMS
Attach last_run.log (and previous_run.log) from the folder above.

The bundled clang/lld-link are from the LLVM project (llvm\LLVM-LICENSE.TXT).
