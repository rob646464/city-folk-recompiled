Animal Crossing: City Folk Recompiled - macOS (Apple silicon)
============================================================

A native port of Animal Crossing: City Folk (Wii). No game data is included: the game is
built on your Mac from your own disc the first time you start it.

REQUIREMENTS
- macOS 13 or newer on Apple silicon (M1 or newer).
- About 2 GB of free disk space.
- Your own City Folk disc image (ISO, WBFS, RVZ, WIA, CISO, GCZ) or an extracted disc folder,
  revision 1 of any region.

THIS BUILD IS NOT SIGNED OR NOTARIZED
It is built by GitHub Actions and has not been run by the developers. Before the first start,
in Terminal, inside the unpacked folder:
    xattr -dr com.apple.quarantine .
or right-click CityFolkRecomp > Open.

HOW TO RUN
    ./CityFolkRecomp
The first start shows the setup screen: pick your disc, then "Install". F1 opens the options
menu in the game; Esc closes it.

WHERE FILES GO
    ~/Library/Application Support/Animal Crossing City Folk Recompiled/  settings, game module, saves
    ~/Library/Caches/Animal Crossing City Folk Recompiled/               shader caches

The bundled clang/ld64.lld are from the LLVM project (llvm/LLVM-LICENSE.TXT).
