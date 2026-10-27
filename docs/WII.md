# What the engine needs for a Wii game

LM and SMS are GameCube games. City Folk runs on the Wii, which keeps the GameCube's CPU and
graphics design but adds a second memory bank, a security/IO processor (Starlet running IOS) that
owns every device, the Wii Remote, the NAND flash and a newer SDK (RVL SDK + NintendoWare for
Revolution). This file lists each difference and how the port handles it. It is the plan; mark
items done as they land.

## What the disc tells us

- `main.dol` 5.4-5.6 MB plus **187 REL modules** (`files/rels/*NP.rel`, 4.1 MB; `RVFOREST1NP.str`
  lists their build paths). Most actors (`d_a_*`), menus (`d_menu_*`), insects and NPCs live in
  RELs that the game links at run time with OSLink.
- NintendoWare for Revolution formats: `.brres` (g3d models/textures/animations), `.brlyt/.arc`
  (lyt layouts), `.bmg` (messages, 5388 files), `.brsar/.brstm` (snd), `.breff/.breft` (effects),
  `.brfnt` (fonts). All of this is game-side code; only the SDK below it needs HLE.
- `HomeButton2/` = the SDK's Home Button menu, `DSApp/` = the Nintendo DS download app (Wild World
  character transfer), `Photo/` (SD-card photos).

## CPU and recompiler

- **Broadway = Gekko ISA** (same integer/FPU/paired-single instructions, same cache ops). The
  recompiler's instruction coverage carries over unchanged.
- **RELs.** New: the recompiler must translate the DOL *and* every REL. A REL's code addresses are
  only known when OSLink places it on the heap, so:
  - each REL is recompiled at its section offsets; branches between modules come from the REL's
    relocation table (`R_PPC_REL24` to module/section/offset) and are resolved to host functions
    at recompile time;
  - absolute address relocations (`ADDR16_HA/LO`, `ADDR32`) in code become loads of the module's
    runtime section base (filled in by our OSLink HLE); data sections are relocated in guest
    memory as the real OSLink does;
  - indirect calls (vtables, function pointers into REL code) go through a code-address table
    that OSLink registers per loaded module (base + offset → host function), unregistered on
    OSUnlink. N64Recomp's overlay handling is the model.
  - prolog/epilog/unresolved functions (`_prolog`, `_epilog`, `_unresolved`) called as usual.
- **Function discovery.** cf-decomp's symbols were for rev 0 and are offline; we have rev 1 discs.
  We generate `config/<id>/symbols.txt` (and one table per REL) ourselves: control-flow analysis
  from the entry point, every branch/`bl` target, REL relocation targets, `.ctors`/vtables and
  jump tables (decomp-toolkit, MIT, may be used as a dev tool for this). SDK functions need their
  real names for HLE: identify them from SDK assert/OSPanic file strings, version strings
  (`<< RVL_SDK - ... >>`), call patterns and documented signatures; then carry the names to PAL and
  Japan by function matching (as `gen_version_map.py` does for LM's PAL).

## Memory

- **MEM1** 24 MB at 0x80000000 (cached) / 0xC0000000 (uncached), as on the GameCube.
- **MEM2** 64 MB at 0x90000000 / 0xD0000000. The game's arena, NW4R heaps and many buffers live
  here. The guest memory window must cover 0x80000000-0x94000000 with both mirrors (LM: a 64 MiB
  window from 0x80000000; the HLE allocator at 0x81820000+ stays in the hole between the banks).
- IOS owns the top of MEM2 (~0x933E0000+); the low-memory globals Wii OS reads (0x80003100+:
  MEM2 size, IOS version, arena bounds) must be set the way the apploader/IOS leaves them.
- **Hardware registers** at 0xCD000000 (Hollywood: IPC, GPIO, timers) besides the classic
  0xCC000000 block. With the SDK HLE'd by name almost nothing reaches them; the GX FIFO
  (0xCC008000) works as in LM.

## Graphics (Hollywood)

- GX is Flipper's GX (same TEV, EFB 640x528, copies, Z textures): Aurora and every LM accuracy
  patch apply. VI gains a few registers; the HLE'd VI keeps LM's frame handoff.
- **Native 16:9**: the game reads the Wii's aspect setting (SCGetAspectRatio). Report 16:9 (and
  let F1 pick 4:3), then Hor+ ultrawide on top, as in LM.
- 480p / PAL60 / EuRGB60 from SC/VI settings.

## IOS (Starlet)

The game reaches every device through IPC to IOS (`IOS_Open/Ioctl(v)/Read/Write`). HLE at the SDK
level (by function name), not at IPC, wherever a library sits above it; IPC HLE only for what the
game calls directly.

| Device / library | City Folk uses it for | Port |
|---|---|---|
| DI (DVD) | disc reads | DVD HLE as LM (modelled latency), nod reads Wii partitions (decrypted) or an extracted folder |
| FS / ISFS / NAND | the save file, banner, settings | NAND HLE → `%APPDATA%\...\nand\title\00010000\<id>\data\` (latency modelled) |
| ES | title ID, data paths, ticket views | constant replies |
| SC (`/shared2/sys/SYSCONF`) | language, aspect, progressive, sensor bar, sound mode, EULA flags | HLE, values from F1 settings |
| STM | Home/Reset/Power buttons, shutdown | HLE: Power = quit |
| Bluetooth (WUD/WPAD/KPAD) | Wii Remote, Nunchuk, Classic Controller | HLE at KPAD/WPAD level (below) |
| USB | Wii Speak (online voice chat) | reported absent |
| SD (`/dev/sdio`) | photos, design/letter backup | HLE → `...\sd\` folder |
| Network (NCD/SO/SSL/NWC24/DWC) | Nintendo WFC online play, WiiConnect24 letters/events | host sockets + TLS under the IOS network HLE, revival servers (docs/ONLINE.md); real-time weather lookups are host-side |
| RTC / OS time | everything (the game runs on the real clock) | host clock + an F1 time offset (time travel) |

## Input: Wii Remote

- HLE `KPADRead`/`WPAD*`: buttons, pointer (DPD: position, valid, distance, angle), accelerometer,
  Nunchuk stick/buttons/accelerometer, extension attach/detach, battery, rumble (`WPADControlMotor`).
- **Pointer from mouse, right stick or gyro** (SDL sensor API) per port; recentre button; F1 >
  Controls like LM's bindings. Classic Controller (if the game supports it) and GameCube
  controller paths map a normal pad directly.
- **Real Wii Remotes** (and DolphinBar mode 4): connected over HID with SDL's hidapi, our own
  report handling (written from public Wii Remote protocol documentation): IR camera setup and
  dots → pointer, accelerometer, extensions (Nunchuk), rumble, speaker. The KPAD/WPAD HLE takes
  each port's input from a source: real remote, mouse, right stick, gyro pad. SDL's built-in Wii
  driver is not enough on its own (no IR pointer), so claim the device before SDL opens it.
- **Remote speaker** (`WPADSendStreamData`, 4-bit ADPCM): decode and mix into PC output (option:
  separate volume).
- Boot stand-in now (`src/recomp/hle_wpad.cpp`): WPADInit/KPADInit do nothing (the Bluetooth
  stack never starts: BTA_Init would wait forever for the USB Bluetooth device), WPADGetStatus =
  set up, channel 0 a core remote, KPADRead = buttons of host port 1 (`readHostPads`,
  hle_pad.cpp). The real HLE replaces `remoteButtons()` with a per-port source feeding one remote
  state (buttons, DPD pointer, acc, extension, rumble, speaker) as planned above.

## Audio

- **AX** (DSP microcode, different from LM's JAudio microcode): **done** (branch ax-audio,
  docs/AUDIO.md). `src/recomp/audio_dsp.cpp` stands in for the game's AX microcode (image at
  0x80542900, read with LM's DSP disassembler): the 3 ms command lists (SETUP, surround fold,
  PROCESS, aux A/B/C round trips incl. DPL2, compressor, OUTPUT, Wii Remote output) and the 0x140-
  byte PBs (accelerator ADPCM/PCM16/PCM8 with loop context, polyphase/linear SRC, envelope,
  low-pass, biquad, mixer with ramps and depop, ITD, remote SRC/sends) as the microcode does them;
  the DSP task HLE mixes at the list's mail and resumes AX through the task's resume callback.
  NW4R snd, AX and AXFX (reverb/chorus/delay on the aux buses) run unchanged as game code. Only the
  polyphase coefficient ROM is ours. Title stream and bus-scene music/speech play (pitch, levels,
  stereo checked; the stream bit-exact against an independent decode). Not done: music/SFX split
  for the volume sliders, discrete surround side data, playing the Wii Remote speaker sound on the
  PC (the 6 kHz remote buffers are filled; `audio::axRemoteFrame` is the hook).
- AI pacing by the SDL3 audio device as in LM (docs/AUDIO.md there); 0x180-byte (3 ms) AI blocks.

## Other SDK parts

- **Home Button menu** (HBM, game-side with its layouts): works with the pointer; option to open
  it from a key.
- **OSLink/OSUnlink**, OSThread/alarms/messages, cache ops: LM's HLE + REL support.
- **WiiConnect24**: some content was distributed by Nintendo through it (event letters, items):
  see docs/REGION_CONTENT.md for whether we can deliver it offline.
- **DS transfer** (DSApp, Wild World characters): needs a real DS; out of scope for now.

## Order of work

1. Repo scaffold from LM's generic code (as SMS did); Wii disc reading (nod: Wii partitions +
   extracted folders); MEM1/MEM2 guest memory. **Done** (branch bootstrap): LM's runtime/HLE/
   frame/settings/menu/controls/setup, Aurora at LM's commit 50e5be3 + LM's patch series; game
   hooks behind `src/recomp/game.h` (`src/game/cf_game.cpp`); `cfrecomp` (dev) and
   `CityFolkRecomp` (setup: images through nod's game partition, or extracted folders; DOL + all
   RELs recompiled) build with `tools/build.ps1`. Extracted folders: `src/port/disc_folder.*` (FST
   entry numbers from sys/fst.bin, data from files/). Guest memory: ctx.h (MEM1 + hole + MEM2, both
   mirrors, 0xCD MMIO). Still LM-shaped: settings.h fields (Hidden Mansion, ghost/world lights,
   gameplay), parts of the F1 menu, setup's USA/PAL slots.
2. Function discovery for the DOL and all RELs (USA rev 1), SDK names. **Done** (docs/DISCOVERY.md, config/; PAL and Japan too).
3. Recompiler REL support + OSLink HLE; boot through OS init (low-memory globals, IOS stubs).
   **Done** (docs/RECOMPILER.md "REL modules"; `src/recomp/rel.cpp`, `hle_os.cpp` OSInit,
   `hle_ios.cpp`). 28 RELs link and run up to the new-game bus scene.
4. NAND/ES/SC/DVD HLE → title screen; GX frames on screen. **Title screen reached** (2026-10-27):
   wrist-strap screen, loading screen, "Press A" title, then the new-game bus with Rover's
   dialogue. Stand-ins, not the final HLE: IOS at IPC level (NAND files from a host folder,
   `/dev/fs` ioctls, ES title id, STM event hook never answered, other devices ENOENT), no
   SYSCONF (SC reads fail: defaults), DSP task stand-in (AX command lists "finished" at once, no
   mixing; replaced by the AX stand-in on branch ax-audio), Wii Remote stand-in (`hle_wpad.cpp`: channel 0 buttons from the host pad, no pointer/
   Nunchuk, so the game shows "Communications with the Wii Remote have been interrupted").
   Open: the bus interior's 3D does not show (black behind the dialogue), a blue band under the
   456-line EFB copy (VIConfigure is a no-op: VI/XFB size not taken from the render mode), exit
   hangs in shutdown (killed after the test).
5. KPAD/WPAD HLE with mouse pointer → into the game; NAND save/load.
6. AX microcode stand-in → sound.
7. Enhancements (interpolation, widescreen, F1 menu) and game features (docs/ROADMAP.md).
8. PAL and Japan bases (function matching), then setup flow + packaging.
