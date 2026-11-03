Animal Crossing: City Folk Recompiled - Linux x86_64
===================================================

A native PC port of Animal Crossing: City Folk (Wii). No game data is included: the game is
built on your computer from your own disc the first time you start it.

REQUIREMENTS
- A 64-bit x86 PC running Linux with glibc 2.34 or newer (Ubuntu 22.04+, Debian 12+,
  Fedora 36+, Arch, SteamOS 3.5+, ...). Check with:  ldd --version
- A Vulkan graphics driver (Mesa for AMD/Intel, the proprietary NVIDIA driver).
- X11 or Wayland, PulseAudio or PipeWire.
- About 2 GB of free disk space (the game module built at the first start, shader caches).
- Your own City Folk disc image (ISO, WBFS, RVZ, WIA, CISO, GCZ) or a Dolphin
  "Extract Entire Disc" folder: USA (RUUE01), Europe (RUUP01), Japan (RUUJ01) or
  Korea (RUUK01), revision 1.

HOW TO RUN
1. Unpack:  tar xzf CityFolkRecompiled-<version>-linux-x86_64.tar.gz
2. Start:   ./CityFolkRecompiled.sh   (or double-click it in your file manager)
3. The first start shows the setup screen: pick your disc, then "Install". The game code is
   translated and compiled for your PC (about a minute) and the shaders are prepared.

Options: --setup (setup screen again), --fullscreen.
In the game, F1 opens the options menu; Esc closes it.

WHERE FILES GO
    ~/.config/Animal Crossing City Folk Recompiled/       settings.ini, install.ini
    ~/.local/share/Animal Crossing City Folk Recompiled/  game module, Wii NAND (saves),
                                                          last_run.log
    ~/.cache/Animal Crossing City Folk Recompiled/        shader caches
Portable mode: create portable.txt next to the game; everything then goes into ./portable.

REPORTING PROBLEMS
Attach last_run.log (and previous_run.log) from the data folder above.

The bundled clang/lld are from the LLVM project (llvm/LLVM-LICENSE.TXT).
