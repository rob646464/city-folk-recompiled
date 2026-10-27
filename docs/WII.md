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

The game reaches every device through IPC to IOS (`IOS_Open/Ioctl(v)/Read/Write`). Decision
(branch nand-sys, docs/NAND.md): the **IPC client is HLE'd and IOS's devices are host code**;
every library above IPC (ISFS, NAND, SC, ES, VF, RFL, NWC24, KBD) runs as recompiled game code,
so its own logic, error handling and request order stay exact. Requests are performed when issued
and answered after a modelled IOS latency (async: callback interrupt; sync: the thread waits with
the CPU given up).

| Device / library | City Folk uses it for | Port (docs/NAND.md) |
|---|---|---|
| DI (DVD) | disc reads | DVD HLE as LM (modelled latency), nod reads Wii partitions (decrypted) or an extracted folder; `/dev/di` accepted |
| FS / ISFS / NAND / VF | the save file, banner, settings, Mii database | **done**: `/dev/fs` + NAND files on a host folder `%APPDATA%\...\nand\` with the Wii's paths (`/title/00010000/52555545/data/`, `/shared2/...`, `/tmp` emptied at start), IOS result codes, path rules, owners/permissions, usage/quotas for NANDCheck, latency |
| ES | title ID, data paths, ticket views | **done**: title id, data dir, device id, title list, ticket views, consumption; a title launch (Wii Menu) ends the program |
| SC (`/shared2/sys/SYSCONF`, setting.txt) | language, aspect, progressive, sensor bar, sound mode, product area | **done**: both files generated from F1 > Game > Console + video/audio settings (SC library reads them unchanged); setting.txt also at 0x80003800 |
| STM | Home/Reset/Power buttons, shutdown | **done**: power/reset events call the game's callbacks (`ios::stmEvent`); shutdown / hot reset / standby and OSShutdownSystem / OSRestart / OSReturnToMenu end the program like "Quit game" |
| Bluetooth (WUD/WPAD/KPAD) | Wii Remote, Nunchuk, Classic Controller | HLE at KPAD/WPAD level (below) |
| USB (`/dev/usb/kbd`, `oh0`) | USB keyboard (letters), Wii Speak | absent (no keyboard; USB host with no devices) |
| SD (`/dev/sdio`) | photos, design/letter backup | slot with no card for now; plan: SD host over a FAT image (docs/NAND.md "Not done") |
| Network (NCD/SO/SSL/NWC24/DWC/KD) | Nintendo WFC online play, WiiConnect24 letters/events | offline: `/dev/net/*` not running (opens fail); later host sockets + TLS, revival servers (docs/ONLINE.md); real-time weather lookups are host-side |
| Miis (RFL) | Mii masks | the game's own parts (`Face/RFLRes01.arc`); no `RFL_DB.dat` = RFL's empty-database fallback; a player's own database can be copied in |
| RTC / OS time | everything (the game runs on the real clock) | **done**: host local time + F1 clock offset (time travel), applied at start |

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

- **AX** (DSP microcode, different from LM's JAudio microcode): command lists and voice parameter
  blocks in RAM; ADPCM/PCM voices, sample-rate conversion, volume ramps, low-pass, main/aux A-C
  sends with surround (Dolby Pro Logic II), remote-speaker outputs. Plan: an AX microcode stand-in
  (like LM's audio_dsp.cpp) so the game's own AX/AXFX/NW4R snd code runs unchanged; written from
  public documentation, not Dolphin's code. AXFX reverb/chorus/delay are game-side code.
- AI pacing by the SDL3 audio device as in LM (docs/AUDIO.md there).

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
   dialogue. Stand-ins, not the final HLE: IOS at IPC level (replaced by the nand-sys IOS
   devices, step 5), DSP task stand-in (AX command lists "finished" at once, no
   mixing), Wii Remote stand-in (`hle_wpad.cpp`: channel 0 buttons from the host pad, no pointer/
   Nunchuk, so the game shows "Communications with the Wii Remote have been interrupted").
   Open: the bus interior's 3D does not show (black behind the dialogue), a blue band under the
   456-line EFB copy (VIConfigure is a no-op: VI/XFB size not taken from the render mode), exit
   hangs in shutdown (killed after the test; the main thread waits in `vi::stopTimer`'s join,
   seen 2026-10-27 with CF_HANG_DUMP). With Aurora's FIFO thread the title intro often ends in
   `FIFO processing range ... is outside buffered range` (AURORA_FIFO_INLINE=1 avoids it), and the
   title does not react to scripted A/START (also on main).
5. KPAD/WPAD HLE with mouse pointer → into the game; NAND save/load. **System side done** (branch
   nand-sys, docs/NAND.md): IOS file system on a host NAND folder, SYSCONF/setting.txt from F1
   settings (language, 16:9, ... verified in the game: French text, 16:9 title), ES, STM, clock
   offset, devices absent cleanly (SD empty, no USB keyboard, offline). The game's NAND library
   passes a save-like self-test (CF_NAND_SELFTEST) incl. persistence across a restart; the game's
   own first save is still to be seen (no input past the title yet).
6. AX microcode stand-in → sound.
7. Enhancements (interpolation, widescreen, F1 menu) and game features (docs/ROADMAP.md).
8. PAL and Japan bases (function matching), then setup flow + packaging.
