# Animal Crossing: City Folk Recompiled

**Discord:** news, help and bug reports for all my projects: https://discord.gg/cM4chZtgh7

A native PC port of **Animal Crossing: City Folk** (Wii; *Let's Go to the City* in Europe and Australia) for Windows, Linux and macOS. The game's PowerPC code is translated into C++ by a build-time recompiler and compiled for your computer, so it runs natively with no emulator. Wii system libraries are reimplemented (HLE), and graphics, audio and input run on the [Aurora](https://github.com/encounter/aurora) engine (WebGPU/Dawn + SDL3).

**No game files are included, and none are downloaded.** You need your own City Folk disc. On first start the program asks for a Wii disc image (ISO, WBFS, RVZ, WIA, CISO, GCZ) or a Dolphin "Extract Entire Disc" folder, revision 1 of the USA (RUUE01), European (RUUP01), Japanese (RUUJ01) or Korean (RUUK01) release, and builds the game for your PC with the bundled compiler (about a minute). Nothing derived from the game is stored in this repository or in the release packages.

## Download
Packages for Windows, Linux and macOS are on the [Releases](https://github.com/rob646464/city-folk-recompiled/releases) page. Unpack, start `CityFolkRecomp.exe` (Linux: `CityFolkRecompiled.sh`, macOS: `CityFolkRecomp`), pick your disc and click "Install". The README.txt in each package covers requirements and where files go. The game updates itself from the same page (F1 > Game > Updates).

## Features
All options are in the **F1** menu, and everything that changes the game is off by default.
- **Any resolution, widescreen and ultrawide,** MSAA, and **high frame rates** up to 320 fps through interpolation, while the game logic keeps its original rate.
- **Accurate Wii rendering** (EFB copies including to RAM, depth textures, fog, TEV/indirect effects, copy filters), checked against Dolphin, with a shader precache.
- **Optional Enhanced lighting and Enhanced world:** sun and moon shadows, coloured point lights, furniture lights, ambient occlusion, water reflections, rain puddles, seasonal particles. Also a CRT filter and Dolphin-format texture packs.
- **Controls:** the Wii pointer from the mouse, a stick or gyro, menus with a normal controller, real Wii Remotes (Bluetooth or DolphinBar), PC keyboard typing, DSU motion controllers.
- **Online play restored** between players of this port: a room list with one-click join, passwords, voice chat from your microphone, keyboard chat and letters between towns.
- **Nintendo events** (the WiiConnect24 distribution items, designs and letters, from WiiLink's archive, downloaded at run time), content from other regions, hemisphere choice, real-time weather, grass wear and skin tone options, unused items at Nook's.
- **Save import/export** with Dolphin and Wii backups, a **Mii maker**, moving in from **Wild World** with a DS save file, and in-game photos saved to a `screenshots` folder (Photos > SD Card).

## Building from source
The repository holds only original code: the recompiler, the runtime and HLE, hooks, function tables (addresses and names) and tools. Building the shipped program needs no game data.

Requirements (Windows): Visual Studio 2022 build tools (for the Windows SDK), LLVM clang, CMake 3.25+, Ninja and Git. Linux and macOS use the `linux-clang` and `macos-clang` presets (see `.github/workflows/build.yml` for the packages each needs).

```
git clone --recursive https://github.com/rob646464/city-folk-recompiled.git
cd city-folk-recompiled
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Configure -Target CityFolkRecomp
cmake --install build/windows-clang --prefix <folder>
```

`cmake --install` produces the same layout as a release package, including the bundled clang and lld-link. The developer build `cfrecomp` recompiles the game at build time and needs your own extracted disc (`-Disc "<folder or image>"`). Details: [docs/SETUP.md](docs/SETUP.md) (setup, packages, releases), [docs/RECOMPILER.md](docs/RECOMPILER.md), [docs/WII.md](docs/WII.md), [docs/RENDERING.md](docs/RENDERING.md), [docs/ONLINE.md](docs/ONLINE.md) and the other files in `docs/`.

## Legal
Animal Crossing is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo. It contains no Nintendo code, assets or keys: the game is rebuilt on your machine from a disc you own. Third-party software used by the program is listed in [packaging/THIRD-PARTY.txt](packaging/THIRD-PARTY.txt).

## Credits
WiiLink for the Nintendo event archive (used with their permission), Open-Meteo for the weather data (CC BY 4.0), Luke Street (encounter) for Aurora, nod and decomp-toolkit, the public Wii decompilation projects whose symbol maps helped name the system libraries, and Dolphin as the rendering reference (no code taken from either).
