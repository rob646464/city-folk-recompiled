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

### Rendering status (branch render-1)

- **Physical addresses in game-written GX data** (Aurora 0300, `GXSetAuroraPhysicalMemoryRange`,
  set in the GXInit HLE: MEM1 at 0, MEM2 at 0x10000000). NW4R g3d's shape display lists set the
  vertex array bases (CP 0xA0+) and EGG/g3d write `GX_CMD_CALL_DL` (0x40) straight into the FIFO,
  both with physical (often MEM2) addresses. Aurora ignored them ("Ignoring nested
  GX_CMD_CALL_DL", ~14 per frame), so every g3d model was invisible: the black bus scene, and the
  title town too. Now the arrays are mapped, called lists are processed in place (a call inside a
  called list is ignored, as on the hardware), and raw texture/TLUT loads (TX_SETIMAGE3 with its
  24-bit Wii address field, BP 0x64/0x65) are resolved as in SMS's 0101. Note: a called list is
  read when Aurora's FIFO thread reaches it (as on the console's GPU); a game that rewrites a list
  right after calling it would race (LM's vertex-snapshot issue), not seen yet.
- **VIConfigure** passes the render mode to Aurora (fbWidth x efbHeight): City Folk draws
  640x456 (viWidth 686 at 16:9, 670 at 4:3); the blue band under the picture was the rest of a
  640x480 EFB.
- **Aspect**: `SCGetAspectRatio` (src/recomp/hle_display.cpp) reports 16:9 unless F1 > Visuals >
  Aspect is Original 4:3 or `--aspect 4:3` (read at boot; main's generated SYSCONF says the same).
  The game then lays out its 2D for 16:9 and draws anamorphic into 640x456. widescreen.cpp's base
  aspect is the game's (16:9): a window at or below it gets the frame fitted at that aspect
  (Aurora 0301 `VISetDisplayAspect`, the fit used to go by 640/456), wider windows stretch + Hor+
  (factor 16:9 / window aspect; HUD anchoring hooks not written for City Folk yet). Default
  window 1280x720.
- **Frame hand-off**: EGG::AsyncDisplay calls VISetNextFrameBuffer from its retrace callback.
  Presenting there blocked the interrupt (audio stutter up to 2.4 s, the exit hang: the VI timer
  thread waited in present() while the main thread joined it) and ended the Aurora frame inside a
  draw when the interrupt came at a loop back-edge between GXBegin and the last vertex (malformed
  FIFO data on the town map). The first call from an interrupt switches to presenting at
  GXCopyDisp (`frame::presentOnCopy`, as LM's PAL release). Shutdown stops the frame hand-off
  before joining the timer thread.
- **Display lists**: recorded into a host buffer; a list that does not fit the game's buffer in
  Aurora's (longer) encoding stays in host memory and the game's buffer gets a 32-byte
  `GX_AURORA_CALL_HOST_DL` (Aurora 0302). City Folk's lists all fit since GXDrawSphere is
  implemented (EGG::DrawGX's two sphere lists were empty/overflowing). Test aids `CF_DL_LOG=1`,
  `CF_DL_FORCE_HOST=1`.
- RVL GX HLE added: GXGetProjectionv, GXGetTexObjAll/Fmt/MipMap/UserData, GXLoadTexObjPreLoaded,
  GXGetViewportv, GXGetScissor, GXGetVtxDescv/AttrFmt(v), draw-done callback, GXDrawSphere,
  FIFO/metric/poke/field stubs. GXSetZScaleOffset: applied since render-2 (below).
- Verified (screenshots, US disc, 1280x720): wrist strap, title town, Rover's spotlight stage,
  the bus interior (seats, windows, cab), name/town keyboards, town arrival at the gate (bus,
  bus stop, player, blob shadow), town map. Open: no console reference capture compared yet
  (only from memory of the game); GXSetZScaleOffset; Hor+ HUD anchoring; interpolation not tried.

### Rendering status (branch render-2)

- **Materials vs Dolphin**: Rover's intro spotlight scene matches Dolphin
  (`C:\Users\rmelv\Desktop\games\Dolphin-x64`, scratch user folder, EFB/XFB copies to RAM) pixel for
  pixel where nothing animates: face (181,188,161) vs (181,187,160), dialogue bubble identical. His
  white face with red slit eyes is his design: the eye/mouth/body textures are C4 with two palettes
  (`Npc/Special/Model/0.brres`, model `xct`: `skin` for eyes/mouth, `skin0` for the body), decoded
  independently from the disc they look like what is drawn; the greenish-white face is white x
  (ambient 49,66,99 + the spotlight's 255,239,99 x ~0.52). The town's ground materials (3-6 TEV
  stages, alpha splats with signed unclamped intermediates, COMP ops, indirect) were read stage by
  stage and need nothing Aurora lacks. The bus interior and town were not compared in Dolphin
  (its scripted input got the pointer only as far as Rover's first choice): unverified, nothing
  obviously off. Reference frames: Dolphin's "Dump Frames as Images" with a scripted DSU pad
  (`dsu_server.py` in the agent scratch area; Wii Remote buttons on DSU stick axes, IR on the right
  stick).
- **Ignored GX state**: Aurora 0303 reports every BP/XF register written without a decoder once
  (release builds too) and unimplemented indexed XF loads. City Folk writes only harmless ones:
  BP 0x44/0x45/0x58/0x68 (field mask, PE done, GXInit's 0x58, field mode), XF 0x1000/0x1005 (error,
  GXInit state) and XF 0x1012 = 1 (dual texture transform on, what Aurora always does).
  `GXSetZScaleOffset` is applied (hle_gx.cpp `zRange`: the SDK's __GXSetViewport writes
  sz = (far - near) S, oz = far S + O, i.e. depth x scale + offset, sent as the viewport's depth
  range clamped to 0..1); the game only ever calls it with (1, 0) (RFLiSetup2DCameraAndParam).
  `GXSetScissorBoxOffset` stays logged-if-non-zero (never seen).
- **Frame rate**: the game presents one frame per retrace in every scene measured (title, Rover's
  intro, bus, keyboards, town, town map: 59.8-60.0 game fps), so its native rate is 60 (50 for a
  50 Hz European game: `game::nativeFps`, from the VI rate). F1's frame-rate slider starts there
  ("Original (60)", drawn directly); above it frames are interpolated (tape stamps in VI fields of
  the current rate, so a slower scene is interpolated by its own time stamps).
- **Frame interpolation** (120 fps verified, 60 -> 120): it presented the right rate before, but
  every in-between was a copy of the newest frame. Two causes: (1) nothing was tagged (no City Folk
  tag sources: every value unpaired); (2) NW4R g3d loads its view-space model matrices with
  **indexed** XF loads from buffers it rewrites every frame, so a tape replayed a game frame later
  read the newest matrices (models drew with frame N+1's matrices). Now (hle_gx.cpp): while a tape
  is captured, indexed loads (GXLoadPosMtxIndx, GXLoadNrmMtxIndx3x3, raw 0x20/0x28/0x30 in the
  write-gather stream) are sent as immediate loads of the values in memory at that moment (what the
  GPU reads on the console) and noted; raw XF matrix loads are parsed and noted; tag sources in
  `src/game/cf_tags.cpp` (View: ScnRoot DrawOpa/DrawXlu; Object: ScnMdlSimple/ScnMdl/ScnProc
  G3dProc; Hud: lyt Pane::Draw; camera: G3DState::SetCameraProjMtx's CameraData). Town walk at
  120: 2558 paired values per tape, 519 blended per present, in-betweens halfway between the game
  frames (CF_INTERP_TEST=2), 120.0 fps presented, game 59.9-60.0, step error p95 0.3-5 ms (LM's
  `tools/interp/judder.py` on CF_INTERP_JUDDER logs). With VSync on a 60 Hz display the cap is the
  refresh rate (57 fps with the VRR cap when the offscreen test window looks like VRR). Open: lights,
  colours and effects (nw4r::ef particles) are not tagged yet (shown at the newest frame), text
  boxes not grouped, no cut detection hooks; matrices inside display lists that use indexed loads
  would still be read at replay (none counted so far).
- **Hor+ / ultrawide** (21:9 checked at 1680x720): the 3D widens (Hor+), the 2D stays a centred
  16:9 frame. With F1 > Visuals "Keep HUD at 16:9" off, `src/game/cf_hud.cpp` (hook on
  nw4r::lyt::Pane::Draw) anchors the clock to the left edge and the memo / camera-adjust buttons to
  the right edge; the toolbar, dialogue, keyboards and menus stay centred. The anchored buttons are
  clicked where they are drawn and the pointer covers the whole window (docs/CONTROLS.md "Pointer
  in ultrawide"); "Keep HUD at 16:9" stays on by default. Test aid `CF_LYT_LOG=<first>,<last>` (pane tree per frame).
- Test aids: `AURORA_DRAW_SKIP_SWEEP=1` (with AURORA_DRAW_LOG_FRAMES and CF_SHOT per frame: draw k
  dropped in frame first+k, which pixels each draw makes), `CF_LYT_LOG`, `[interp] raw XF matrix
  loads ...` line once a minute while interpolating. Scripted route: `tools/routes/gen_town.py`
  writes `town.txt` (boot -> town gate, ~frame 13300) and `town_walk.txt` (+ walking from 16700):
  `cfrecomp --input @tools/routes/town.txt` with `CF_INPUT_CLOCK=frames`.

### Rendering status (branch render-3)

- **Display lists read a frame late** (the main interpolation gap): NW4R g3d writes its material
  and shape display list calls (`GX_CMD_CALL_DL`, ~400 per frame in the town) straight into the
  write-gather pipe; Aurora 0300 processed them in place when the tape was replayed, so every
  material colour/texture animation the game patched into those lists came from the newest frame
  and nothing in them was noted. While a tape is captured such calls are now held back and made
  as a GXCallDisplayList (`rawCallWrite`, hle_gx.cpp): the list is inlined as it is now and parsed
  like any display list. Indexed matrix loads inside called lists (g3d shape pre-primitive lists)
  are rewritten into immediate loads of the matrices in memory now and noted (`convertListIndexed`;
  they were drawn with frame b's matrices from the array copy). Raw and list indexed loads are
  keyed by their matrix-array address (`interp::noteBySource`: g3d skips loads already in a slot,
  which shifted occurrence keys). A list's own colour writes belong to its own draw
  (`claimForLastDraw`), and draws with no matrices (g3d material lists) pair by list + scope:
  colour registers switched (shown at the game frame) per present 1300 -> about 1, blended ~2000.
  A/B: `CF_RAW_DL_INLINE=0`, `CF_DL_INDEXED=0`, `CF_KEY_BY_SOURCE=0`.
- **Camera**: `G3DState::SetCameraProjMtx` is called by `ScnRoot::SetGlbSettings` for all 32
  cameras of a scene; r5 is true only for the current one. The hook took the last stored (camera
  31, a fixed identity camera at z -15), so no camera motion was ever factored out of the blend and
  no cut could be found. Now only r5 = true sets the view source (`CF_CAM_LOG=1`). Cuts are also
  judged relative to the scene's depth (median view distance of the main pass, City Folk's sets
  are small): `CF_CUT_LOG=1` lists every tape's main camera step. The bus "cut" at ~frame 6250 is a
  pan (1-2 units per frame), scene changes come after black frames (no main view in a).
- **Particles**: nw4r::ef billboard particles (DrawQuad from the strategy at 0x80292388, particle
  in r27) are one Particle scope each, with a birth stamp from the age at +0xDC (+1 per frame in
  every particle followed, `CF_EF_PEEK=<words>`): a born/retired particle no longer shifts the
  pairing of the others (running dust, rain splashes). Lines and strips (rain streaks 0x80295174,
  0x8028E930, 0x8029077C) are still keyed by occurrence under their emitter.
- **Text boxes / HUD**: lyt panes are tagged per pane and blend; the dialogue box opening in the bus
  grows every present (region mean 54 -> 194 in 14 game frames, 2 distinct steps per game frame at
  120, `pdT` sheet); no grouping needed so far.
- **Capture cost**: the per-tape occurrence and pairing maps were std::unordered_map (a heap node
  per key, ~4500 values and ~400 lists per town tape): the guest spent ~18% of its time in the heap
  while capturing, and the town ran 31-36 game fps at 120. Flat epoch-cleared tables
  (`OccTable`) and reserved tape vectors: 42-60 game fps in the user's town under the same load.
  Aurora 0305: the texture-object cache sweep ran every frame (12% of the host thread at 120 fps),
  now every 32 frames.
- **Rain streaks** (Aurora 0304): the rain is GX_LINES (2 vertices, texcoords (0,0)-(0,1), line
  width 18/6 px, GXEnableTexOffsets + GX_TO_ONE). Aurora put the line texture offset on T, so S
  stayed 0, the clamped transparent edge of the 8x16 I4 streak texture: alpha 0, nothing drawn
  (AURORA_LINE_DEBUG=1 showed the lines in place). The offset goes on S now and the streaks show
  (faint, A0 25/255 x texture alpha by the game's TEV). Not compared with Dolphin (no way to force
  rain there yet); the game's own vertex data only makes sense with the offset on S. Test aids
  `CF_LINE_LOG=1` (first words of line draws), draw log line state.
- **PAL 50/60 Hz**: the VI retrace rate follows the TV format of the game's VIConfigure (EuRGB60 =
  59.94 Hz, PAL = 50 Hz); LM's separate "PAL refresh rate" option is gone, the console's EuRGB60
  (F1 > Game > Console, default on) is the only setting. The PAL game asked SCGetEuRgb60Mode 0.4 ms
  after SYSCONF was opened, before the read came back (a Wii is far slower in between), and always
  got 0: a 0 is now answered from the same setting the SYSCONF is generated from. PAL disc:
  `SCGetEuRgb60Mode = 1`, TV format 5, 59.5 presents/s (59.9 retraces/s); EuRGB60 off: format 1,
  50.0 retraces/s, the game one frame per retrace (its speed follows, as on a console).
- **Pacing** (user-town fixture, `play.txt` walk/run/pockets/map, 1280x720, VSync off; this PC is
  shared with other agents' game instances and builds: CPU 50-100% loaded): presented 108 / 117 /
  114 fps at targets 120 / 144 / 240 (presents p50 8.2-8.4 ms: the offscreen window here does not
  present faster than ~120 Hz), step error mean 2.6-3.0 ms p95 6.0-6.6, shown-time error p95 17-26
  ms, 0 reversals, 0-6 stalls; game 42-60 fps in the town (it also misses 60 at Original 60 under this
  load: 45-53, guest busy ~70% incl. the AX DSP stand-in ~19%). Identity check: 0 of 0.7-0.9 M
  tagged matrices jumped there and back. The judder detector's blend "defects" (16 per in-between,
  the player model) are its limitation with g3d skinned draws (per-vertex matrix indices; the
  present dumps show the model whole); not usable as a defect count for City Folk yet.
- **Original 60**: every change is behind interp::capturing() or the anchoring option: frame dumps
  identical to the render-2 build for every still frame of the title/Rover route (the frames that
  differ also differ between two runs of the same build: animation timing vs. load).
- **Ultrawide clicks** (fixed, branch ultrawide-pointer): anchoring moves the HUD elements' pane
  global matrices (`Pane::CalculateMtx` hook, dx = (1/f - 1) x half the layout width; clock -217 ->
  -312, camera icon 274 -> 369 at 21:9; `CF_HUD_ANCHOR=picture` the old picture-only shift), the
  pointer spans the whole window (KPAD x +-1/f) and the game's "pointer on the screen" tests are
  widened by the same factor (src/game/cf_pointer.cpp): the Save button at the window edge is
  hovered and clicked where it is drawn at 21:9 and 32:9. docs/CONTROLS.md "Pointer in ultrawide".

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
- **Remote speaker** (`WPADSendStreamData`, 4-bit ADPCM): decoded and mixed into the PC output
  with its own volume (branch audio-2, `src/recomp/audio_remote.cpp`, docs/AUDIO.md).
- **Done (branch wpad-1, docs/CONTROLS.md)**: `hle_wpad.cpp` answers the WPAD/KPAD calls the game
  makes (KPADRead with 200 Hz report timing, WPADProbe, connection/extension callbacks, battery
  info, motor, LED, DPD, speaker calls accepted, disconnect/reconnect); `wiimote.cpp` builds each
  port's Remote from a Source (emulated today: F1 > Controls bindings + mouse / right stick / gyro
  pointer + shake inputs). `VIGetDimmingCount` reports Remote radio activity (0 = the game's
  "Communications with the Wii Remote have been interrupted"). Speaker sound on the PC: branch
  audio-2 (audio_remote.cpp; real Remotes play it themselves, below). Not yet: tilt from a pad's
  accelerometer, Classic Controller (check whether the game reads it).
- **Real Wii Remotes (branch hid-remotes, docs/CONTROLS.md "Real Wii Remotes")**: `HidSource`
  (wiimote_hid.cpp) per port set to "Real Wii Remote" in F1 > Controls > Ports: SDL hidapi on
  057E:0306/0330 only (SDL's own Wii driver forced off meanwhile), report mode 0x37, the Wii's IR
  camera sequence with the console's sensitivity, calibration from EEPROM, Nunchuk (unencrypted
  init, calibration), IR dots -> KPAD pointer (roll-compensated midpoint, sensor bar above/below),
  rumble, speaker (WPADSendStreamData ADPCM as reports 0x18 at 6 kHz), battery, hot-plug, player
  LED = port. Protocol in wiimote_proto.cpp, tested against a fake device
  (`cf_wiimote_hid_test`); not yet tried with a real Remote.

### Scripted route to the town

`tools/routes/new_game.txt` (USA, `CF_INPUT_CLOCK=frames`, run with `tools/routes/run.ps1`):
wrist strap, title and Rover's lines (A every 80 from 1300), "Start fresh!" (pointer
`@0.41/0.42`), Rover's questions, the name keyboard ("rob": `@-0.339/0.017` r, `@0.367/0.017` o,
`@0.052/0.292` b, `@0.656/0.778` Confirm), the town-name keyboard ("Aa": `@-0.75/0.286` Shift,
`@-0.547/0.15` a twice, Confirm), "That's it!" (`@0.625/0.508`), "SO excited!", the town map
("OK!" at `@0.656/0.778`), bus, arrival, Rover's "Head on over to the town hall", the map hint;
the player stands at the gate, controllable, from about frame 14900 (~250 s). One key press per
key. Deterministic: with run.ps1's CF_RNG_SEED / CF_CLOCK_START / CF_CLOCK_FRAMES the town
(layout, villagers Butch, Jambette, Alfonso, Whitney, Patty, ...) and every frame up to the gate
are the same in every run (checked: identical frames 14000-19000 in two runs). `town_basics.txt`
adds walking (STICK*), running (B) and the pockets/map buttons; `continue.txt` continues a saved
town. Fast-forward (CF_FF) changes the game's timing and breaks the route.

Seen in play: the Nunchuk stick walks in all directions (camera-relative), B held runs, the HUD
Map button (`@0.578/0.82`) opens the town map with the player marker, B closes it; the town hall
door opens on stick-up (when exactly varies: route with `after link d_bgi_drawNP`).

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
  stereo checked; the stream bit-exact against an independent decode). Branch audio-2: volume
  sliders per NW4R sound player (music / effects / voices), the Wii Remote speaker stream
  (WPADSendStreamData ADPCM) decoded and played on the PC, discrete 5.1/7.1 from the DPL2 buses
  (docs/AUDIO.md).
- AI pacing by the SDL3 audio device as in LM (docs/AUDIO.md there); 0x180-byte (3 ms) AI blocks.

## Other SDK parts

- **Home Button menu** (HBM, game-side with its layouts): works with the pointer; option to open
  it from a key.
- **OSLink/OSUnlink**, OSThread/alarms/messages, cache ops: LM's HLE + REL support.
- **WiiConnect24**: some content was distributed by Nintendo through it (event letters, items):
  see docs/REGION_CONTENT.md for whether we can deliver it offline.
- **DS transfer** (DSApp, Wild World characters): the move-in works with a Wild World save file
  and a virtual DS behind the MP/MPDL libraries (docs/MOVE_IN.md); the DS Suitcase has no DS side.

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
   devices, step 5), DSP task stand-in (replaced by the AX stand-in, branch ax-audio),
   Wii Remote stand-in (`hle_wpad.cpp`: channel 0 buttons from the host pad, no pointer/
   Nunchuk, so the game shows "Communications with the Wii Remote have been interrupted").
   Fixed on branch render-1 (see "Rendering status"): the bus interior's 3D, the blue band, the
   exit hang. Was open: the bus interior's 3D does not show (black behind the dialogue), a blue band under the
   456-line EFB copy (VIConfigure is a no-op: VI/XFB size not taken from the render mode), exit
   hangs in shutdown (killed after the test; the main thread waits in `vi::stopTimer`'s join,
   seen 2026-10-27 with CF_HANG_DUMP). With Aurora's FIFO thread the title intro often ends in
   `FIFO processing range ... is outside buffered range` (AURORA_FIFO_INLINE=1 avoids it). (The
   title "not reacting to scripted A" was a test wrapper with a `$Input` parameter: PowerShell's
   automatic variable, so nothing reached `--input`; docs/CONTROLS.md.)
5. KPAD/WPAD HLE with mouse pointer → into the game; NAND save/load. **Wii Remote done** (branch
   wpad-1): scripted pointer play from the title through name/town entry into the town (route
   above). **System side done** (branch
   nand-sys, docs/NAND.md): IOS file system on a host NAND folder, SYSCONF/setting.txt from F1
   settings (language, 16:9, ... verified in the game: French text, 16:9 title), ES, STM, clock
   offset, devices absent cleanly (SD empty, no USB keyboard, offline). The game's NAND library
   passes a save-like self-test (CF_NAND_SELFTEST) incl. persistence across a restart; the game's
   own first save is still to be seen (no input past the title yet).
6. AX microcode stand-in → sound.
7. Enhancements (interpolation, widescreen, F1 menu) and game features (docs/ROADMAP.md).
8. PAL and Japan bases (function matching), then setup flow + packaging. **Setup done** (branch
   setup-1, docs/SETUP.md): CityFolkRecomp identifies USA/Europe/Japan rev 1 discs (ID, revision,
   main.dol and REL hashes), reads images (ISO encrypted/decrypted, WBFS, RVZ, WIA, CISO, GCZ) and
   extracted folders through one file system layer (port/disc_folder), builds each module with
   the bundled clang/lld in about a minute; the European and Japanese games boot through the
   title into Rover's bus dialogue with their own tables.
