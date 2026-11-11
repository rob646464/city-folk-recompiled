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

### Town interpolation (branch interp-town)

User report (USA town, 4K, 120 Hz, VSync, Enhanced lighting Ultra + Enhanced world): trees very
jumpy at high frame rates, sun shadows jumping when walking up / down the log, Enhanced world's
falling leaves jumping even at Original 60. Route `tools/routes/town_trees.txt` (user-town-clean,
warp to the plaza, walk / run in 8 directions); runs with a copy of the user's settings
(`run.ps1 -SettingsFile`, `-Exe` for a baseline build).

- **The game turns the camera, not the world**: walking north / south, the view matrix' translation
  stays (-x, -584.7, -751.7) and its rotation turns about world X (the log's axis, through y = z = 0
  in the user's town). Every placed object's Y axis is exactly radial from that axis (CF_SUN_LOG
  samples: 0.01 degrees), radius 670.7 on the main level, 614.7 on the lower terrace.
- **Trees mis-paired** (the main "jumpy"): the town's field objects (trees, bushes, flowers, fences,
  signs) are one ScnProc (d_fg_drawNP) that draws each visible object through fn_112_2184 (r3 =
  its record: +0x8 kind, +0x14 position), every one loading its model matrix from the same scratch
  MTX34 at 0x80763AD0. Under the one ScnProc scope the loads paired by order, so whenever an object
  entered or left the view all later ones blended with another object's matrix: present dumps show
  trees sliding halfway between two trees, or caught as a "jump" and shown at the game frame
  (judder summary: 840-1997 field-object jumps per 200 in-betweens). Each object is now its own
  tag scope keyed by its place and kind (hooks.txt `d_fg_drawNP::fn_112_2184 all`, cf_tags.cpp;
  A/B `CF_FG_TAG=0`): jumps 0, one-present pops on screen 151 -> 0 over 1200 presents.
- **Sun shadows**: the sun frame's "up" was the mean Y axis of the obj_ / sdw_ models drawn the
  frame before; it changed in a step whenever an object came into or left the view (steps up to
  11 degrees between game frames, 30 steps over 0.5 degrees in 2000 frames). Now the log is fitted
  (axis from the objects' up lines, ground radius = the most common radius; cf_light.cpp LogFit)
  and "up" is its normal where the view's forward ray meets the ground (`groundAhead`): max step
  0.03 degrees per frame. The cascades' texel grid is anchored in the world (Aurora 0441: world X
  as the reference axis, the world origin as the anchor; interpolation turns both with the blended
  camera); before, all of Aurora's inputs were view space, so the snapping never held. A/B
  `CF_SUN_UP=mean`, `CF_SUN_GRID=view`; `CF_SUN_LOG=<n>` every n frames.
- **Shadows gone at a low sun (branch shadow-fix, user report on main 077d9a1)**: the normal at
  the ground ahead leans 20-30 degrees further than the old mean of the visible objects' axes, so
  with the sun at its 12-degree floor (the user played at 16:57 in November) it grazed the ground
  there: long, faint shadows lost in the terminator, and only frames without a fit (old mean as the
  fallback) showed them, flickering in. Now the sun's up is the mean of the log's normals under
  rays through five rows of the view's centre column (a ray over the horizon takes its closest
  point: continuous), the look the old mean gave but without its steps, and a frame without it
  holds the last one (600 frames) instead of falling back. Same frames at 16:57: shadows as with
  the old mean (evidence in the agent scratch), steps max 0.015 degrees per frame outside the warp;
  10:00 unchanged. A/B `CF_SUN_UP=ahead` (interp-town's), `=mean` (the old).
- **Leaves on slopes (branch leaves-slope, user report on main c789775)**: the focus point itself
  is analytic (the fitted log, not the terrain) and steady, but Aurora's effects "metre" came from
  the focus distance, which changes as the camera rises or sinks on a slope (CF_WFX_FOCUS_LOG=1:
  440 -> 411 units, 7 %, going down south of the upper level). The particle field is laid out in
  that unit around a world position, so the whole field slid by its world offset x the scale
  change and zoomed. Aurora 0442: with a focus given and `worldScale` > 1.5, that fixed scale is
  used (CF: 14.7 = what the distance gave on flat ground); the log's ground radius only switches
  while the fit is young (a late switch moved the frame in one step). Route
  tools/routes/slope_probe.txt (upper level at 7 x positions, walking south down the slopes).
  Leaves tracked frame to frame on the slopes at 60 (TP frames excluded): p90 jerk 1.4-1.6 ->
  0.7-0.9 px, frames with every leaf jumping 4 -> 0; beach slope 9 -> 1 (the camera stopping);
  flat town p99 2.6 px, 0. 120 fps on the slopes: 0.
- **Looking at the sky (branch wfx-sky, user report on main 536ac8b)**: "the enhanced world
  particles freak out when pressing up on the D-pad to view the sky". D-pad Up is a view the
  game keeps until the player moves: the camera orbits its look-at target (~64 units above the
  player's feet, ~316 units along the forward ray: every forward ray of the transition passes
  within 5 units of one point), sinking from ~280-340 to ~180-240 units above the ground and
  its pitch from 52 to 33 degrees below the local horizontal. The log falls away 38+ degrees
  below the horizontal from that height, so the forward ray missed it: the centre (where that
  ray met the log) first raced off toward the horizon (80 units in 16 frames), then there was
  none, so Aurora fell back to the depth focus (the sky: 1500 units away, the effects' metre
  from that distance) and the effects frame to the snapped axis: the leaf field jumped, zoomed
  and came back with another jump when the player walked on (CF_WFX_FOCUS_LOG: 1016 frames
  without a centre in one look-up). The centre is now the ground under the look-at target
  (C + 316 forward, projected onto the log), moved 85 units along the ground in the camera's
  horizontal forward so the normal view keeps its old centre (cf_world.cpp effectsCentre); it
  moves ~5-9 units over a whole look-up. The particle box reaches 34 effect metres above the
  centre instead of 19 (Aurora 0443: count scaled with the height, a fade at the top), so the
  rays that rise over the horizon in the centre's frame still meet leaves against the sky.
  **At 120 fps** a second, older fault showed while the camera swung back down: the effects
  frame and centre are loaded at the HUD's first draw (cf_world.cpp endWorld), under the HUD's
  orthographic projection, so interpolation took them as 2D values (no camera factoring) and
  blended them in view space; the frame's origin lies thousands of units away and its view-space
  translation moved 70-100 units per game frame as the camera turned, so the in-betweens put
  the leaves a third of the way instead of halfway and every leaf caught up 4-6 px at each game
  frame. They are now noted as drawn from the scene's camera (`interp::setViewOverride`,
  BACKPORT 42; A/B `CF_WFX_VIEW_NOTE=0`); main had the same steps whenever the camera turned fast.
  Route tools/routes/look_up.txt (house door: Up, walk, Up; plaza), look_up_places.txt (slope,
  upper level's edge, beach). Leaves tracked frame to frame (GLOWDBG=8 dumps, main -> branch;
  "all" = frames with most leaves jumping > 4 px together). Original 60: house look-up p99 jerk
  9.6 -> 2.3 px, all 3 -> 0; walking on after it 6 -> 0; slope walk + 2 look-ups + run p99 9.0 ->
  3.4, all 13 -> 0; upper level's edge 8 -> 1 (the camera halting after the TP); beach 19 -> 1
  (the camera halting); normal view p99 1.5 / 1.35. 120 fps (CF_INTERP_TEST=2, CF_WFX_FREEZE=1,
  the in-betweens in order): slope look-up and return p99 11.8 -> 1.9, all 5 -> 0 (35 with only
  the new centre); house 2.2 / 0, slope walk 1.3 / 0, beach 1.6 / 0. The centre moves at most
  3.6 units per frame outside TPs / warps (main: 1079 frames without one on this route).
  Original 60 with default settings: frame dumps identical to main (66 of 66). A/B
  `CF_WFX_CENTRE=ahead` (the forward ray's ground point), `CF_WFX_TARGET=<distance>,<ahead>`,
  `CF_WFX_TOP=<metres>` (0 = the old box), `CF_WFX_VIEW_NOTE=0`.
- **Leaves** (not interpolation): Aurora's world effects centred their particle volume and scale on
  the scene depth under one pixel near the view's middle (the player, a tree, the ground behind as
  they passed under it), and the port's effects frame snapped the world's up to one of six axes,
  which flipped by 90 degrees as the camera turned about the log. Now the centre is the ground
  ahead (Aurora 0440 `AURORA_WORLD_FX_XF_FOCUS`, blended like a model matrix) and the frame is the
  log's own flat frame there (x = axis, y = normal, z = along the ground at -R theta, theta
  unwrapped), so the leaves stay on the ground as it turns. Leaves tracked frame to frame
  (GLOWDBG=8 dumps, agent scratch leaves.py): Original 60 p99 jerk 21.7 -> 5.2 px, frames with
  every leaf jumping together 35 -> 0; 120 fps p99 18.0 -> 1.7 px, 20 -> 0. A/B
  `CF_WFX_DEPTH_FOCUS=1`, `CF_WFX_AXIS=snap`.
- Original 60, default settings: frame dumps byte-identical to main (61 of 61); the changes act
  only while interpolating (tags), with Enhanced lighting (sun) or Enhanced world (frame, centre).
- **Judder detector**: `CF_JUDDER_SLOTCHECK=1` (perf-2) reported 0 defects while the trees were
  this wrong; the remaining ~16 "blend defects" per in-between with the check off are texture
  matrices from ResTev display lists (CallDisplayList +0x44) in PNMTX rows, as perf-2 found. Use
  present dumps (`CF_PRESENT_DUMP=1` + `CF_FRAME_DUMP`) and an on-screen check as well.
- The user's video (22 s, 4K/120): in-betweens were evenly spaced in the stretches measured, but a
  repeated frame came every ~12-35 refreshes: the log shows "variable refresh rate detected" and
  the governor limiting to 103-110 fps (GPU 8.8-9.1 ms median at 4K Ultra, over the 8.3 ms of
  120 Hz). On a display that really follows the presents that is smooth; captured at a fixed 120
  it repeats frames. Not changed here (pacing); worth asking whether G-Sync is on in windowed mode.

### The circle wipe (branch wipe-cover)

The circle that closes and opens around the player at doors, warps / stage changes and the
arrival in the town after "Preparing saved data" is `mWipeFader_c` (RTTI; vtable 0x804F0984,
`dWipeFader_c` 0x804F4DEC). Its draw `fn_802B6B48` sets `C_MTXOrtho(-228, 228, -304 * s, 304 * s)`
(s = +0x48) and draws ONE quad over exactly that frame with the circle texture (+0x4C, size
+0x50/+0x54, clamped: everything outside the texture is its black edge) placed by the texture
matrix at +0x14, which `dWipeFader_c`'s calc `fn_8017D0A4` builds as a scale about the centre
(+0x58/+0x5C) from the frame count (+0x44); at frame 0xFF a plain black quad. So the game's own
draw covers its whole 640x456 frame at 16:9 and 4:3 (checked at 1280x720: no gaps). In a window
wider than 16:9 the quad was classified as a full-screen picture and pillarboxed at 16:9, so the
town showed at both sides (any window even slightly wider than 16:9, e.g. one shortened by a title
bar, gets thin side gaps).

Fix: `HLE(fn_802B6B48)` (cf_hud.cpp, hooks.txt `all`) runs the draw in a `ws::WipeScope`. The
held quad (its GXBegin came inside the scope; it ends at the next GX call, after the scope, so
the flag is taken at GXBegin: `sHeldWipe`) stays centered in the game's frame (round circle) and
`widenWipeHeld` (hle_gx.cpp) widens it about the frame's centre by 1 / factor() with its texture
coordinates extrapolated along x, so the circle keeps its size and place and the added strips show
the texture's black edge. The plain black quad is widened the same way. At 16:9 and narrower
(factor 1) nothing changes. Verified (CF_SHOT, user-town-clean, route warps house1 at 7000 and
town.house1 at 8200): 21:9 entering/leaving the house and the town arrival cover the whole width
(before: town beside the 16:9 frame); 16:9 at 60 and 120 fps (interpolated in-betweens) and with
Enhanced lighting Ultra + world unchanged and gap-free; 4:3 unchanged. CF_SHOT is the game image
before post-processing: a gap the user sees at 16:9 that these runs do not show would come from
later (post / presentation) or a window slightly wider than 16:9 (fixed by the above). Log line
`[ws] circle wipe widened to the display` (once).

### Face seams at high frame rates (branch face-seams)

User report (4K, 120 Hz display with "Match display refresh rate", Enhanced lighting Ultra): a dark
line along the edge of Rover's face patch (across the forehead, down the sides) in the bus. **Not
Enhanced lighting**: the line is there with Enhanced lighting off as well and never at Original 60;
it comes from frame interpolation (any target above the game's 60). Route: `continue.txt` from
user-town-clean, `video.target_fps=120`, shots of game frames 1950-2550.

- The crack is the background seen between two shapes of the head: the face shape (one matrix,
  `LoadResShpPrimitive`) and the skinned head around it (envelope matrices in the PN slots), whose
  edge vertices meet. With every value shown as captured (`CF_INTERP_NOBLEND=0x3F`) it stayed: the
  tape replay itself drew the two shapes from different game frames.
- **Vertex arrays of g3d models were never copied into the tape** (interp_vtx.cpp). NW4R g3d sets
  each shape's vertex layout (VCD/VAT) and array bases with LOAD_CP_REG commands inside its shape
  display lists, with physical addresses (0x0xxxxxxx MEM1, 0x1xxxxxxx MEM2) and most model data in
  MEM2. The tape's vertex scan knew neither: the layout came only from the GXSetVtxDesc/AttrFmt HLEs
  (the shape lists' draws could not be sized: ~105 "unparsed streams" per log period in the bus),
  the arrays only from the GXSetArray HLE, and only MEM1 (0x8/0xC) arrays were copied. So Aurora
  read every g3d vertex array from guest memory at replay, a game frame late: shapes the CPU
  rebuilds every frame (the villagers' face expressions) came from the next frame while the rest of
  the head was drawn with this frame's matrices.
- Now, while a tape is captured, the layout written by a display list updates the port's VCD/VAT
  mirror (`gx::cpRegWrite`, hle_gx.cpp), array base/stride writes in a list are followed like
  GXSetArray and loaded again as a GXSetArray after the list (g3d's pre-primitive list), so the
  primitive lists after it read a copy taken at capture (and CPU-deformed position arrays can be
  paired and blended); MEM2 and physical addresses are accepted (`ramAvail`). Bus: 0 unparsed
  streams, ~20 arrays (12 KB) copied per tape. A/B `CF_DL_LAYOUT=0`, `CF_DL_ARRAYS=0`.
- Original 60 is unchanged by construction (all of it runs only while a tape is captured; the
  lockstep draw-time snapshots keep their MEM1-only check), and no Enhanced lighting code changed.
- Left: a faint dotted line (under a pixel at 1368p) at some in-betweens: the face's single matrix
  and the head's envelope matrices take slightly different in-between paths (gone with position
  matrices shown as captured, `CF_INTERP_NOBLEND=0x1`; entrywise blending of the instance does not
  remove it). The player's face (the town, 120 fps) had no seam before or after.

### Matching untagged draws (branch interp-match)

User report: "weirdness" at 120 fps on some animations, e.g. Mr. Resetti's dirt. Design (user-approved):
no effect-by-effect tags; the fallback for everything a library does not tag must work by itself.

- **What was paired by order**: a value's key is (scope, call site, occurrence). Where one scope draws
  many things from one call site (an emitter's non-billboard particles, a ScnProc's own draw loop,
  several shapes of one g3d model) the occurrence is the draw order, so whenever one of them entered
  or left the view every later one blended with another one's place. Mr. Resetti's dirt is
  `fn_8028FF0C` (an ef draw strategy other than the billboard one cf_tags.cpp tags per particle): 4
  clods born per game frame inside the emitter's list, all paired by order: the in-betweens showed
  the cloud reshuffled between game frames.
- **Fingerprint per draw** (`Tape::bundleFp`, interp.cpp `pushBundle`): what is drawn (display list
  address + size, or primitive / vertex layout / position array), the vertex count of immediate
  draws, the textures bound (maps 0-2); `bundleFp2` without the textures (flipbook textures change
  every game frame: the dirt clods alternate between two). Draw class = object scope (or the camera
  path for untagged draws) + fingerprint + pass.
- **World place per draw** (`bundleWorld`): its own position matrix (else the one current when it was
  drawn, or a material list's next shape in the same scope) applied to the centre of its indexed
  positions (or the centroid of its CPU-built vertices), through the inverse of the camera it was
  drawn under: camera motion is out. Each matched draw keeps its step (`bundleVel`) as the prediction
  for the next game frame.
- **Matching** (`matchDraws`, at pairing, guest thread): position / normal matrices and CPU-built
  vertex blocks whose bucket (key before occurrence) holds more than one value in either game frame,
  and untagged ones inside a view, lose their order pairing. Texture matrices and colours keep their
  keys (colours blend only while their draw pairs with the partner's draw, replayPair's existing
  check): g3d loads its materials' texture-SRT matrices into position matrix rows per material list,
  and matched by the place of the shape drawn next they paired across materials, so the city's
  villagers shook (city route: 4023 detector events with them, 453 without, 610-1208 old); their draws are matched within their class, first with the full fingerprint, then
  what is left without textures. One draw of a kind on both sides: the same thing unless it moved
  more than max(40, half its camera distance). Several: an order-preserving alignment (DP over the two
  draw orders, cost = distance to a's place or a's place moved on by its last step, whichever is
  nearer; gap cost = 2 x the class's median nearest distance, at least 0.05 + 2e-4 x camera distance, and at
  least 1.25 x the median step along the best constant shift of the two orders (a burst or stream whose
  list order holds and which moves more per game frame than its spacing);
  never beyond a fifth of the camera distance). Births and deaths in a particle list or an object
  loop are gaps, the survivors keep their partners; classes over 65536 pairs fall back to mutual
  nearest neighbours with a ratio test (draws at one place taken in order). The values of matched
  draws pair by bucket and their order within the draw. A draw without a partner is shown at the
  newest game frame for that interval (the replay's usual unpaired path). Existing tags are
  unchanged (unique keys still pair by key). A/B `CF_MATCH=0` (old draw-order pairing),
  `CF_MATCH_ALIGN=0` (mutual nearest only), `CF_MATCH_DIAG=0`, `CF_MATCH_GAP=<f>`, `CF_MATCH_LIMIT=<f>`,
  `CF_MATCH_RATIO=<f>`, `CF_MATCH_KINDS=<mask>` of Kind bits (default 0x23; 0x7F adds texture matrices and colours), `CF_MATCH_GROUPS=0` (model instances keep keys); `[match]` line every 5 s (draws needing a match, matched, new, left at the
  game frame, values paired, time); `CF_MATCH_LOG=1` per tape, `=2[,<first tape>]` every draw left
  without a partner and why, `CF_MATCH_DUMP=<site>` both frames' draws of a site in draw order, `CF_MATCH_TAPE=<seq>` every draw
  of one tape that needed a match with its values and partners;
  `CF_MATCH_SITES=<file>` per GX call site (draws, keyed uniquely, matched, left, untagged).
- **Detector that skips no draw** (`CF_MATCH_JUDGE=<file>[,<px>]`, `matchJudge`): Aurora's draw probe
  gives every draw of the main pass in the full view (first vertex through the matrix it was drawn
  with); each present's points are followed to the previous present's nearest point of the same
  fingerprint (texture-free) and slot, the previous point moved into the present's camera; velocity =
  screen step / game time between the presents (pacing does not count). Jerk = velocity change over
  <px> (default 16 px of a 2160-line picture per game frame), pop = a jerk that turns back, far = no
  point of its kind within 4 x <px>. Independent of every identity the interpolation uses (the old
  detector judged against the interpolator's own pairs and was blind to wrong pairs, BACKPORT 34).
  `[match-judge]` line every 5 s; `J`/`F`/`P` lines in the file with site and match state.
- **Static check** `py tools/interp/draw_coverage.py [--sites <CF_MATCH_SITES files>]`: every GX draw
  call site in the recompiled code (calls of GXBegin / GXCallDisplayList, direct call graph from the
  generated C++, library by name), classified tagged (reached from a per-thing tag hook or inside
  g3d / lyt), fingerprint (shared scope: matched by fingerprint + place, with the fingerprint
  quality), 2D/system, or needs help (a run shows most of its draws left without a partner).
- **Results** (120 fps, VSync off, 1280x720, loaded shared PC; whole route incl. boot; detector events
  per run, old = `CF_MATCH=0` same build; out/interp-match/judge_summary.txt): Mr. Resetti
  (tools/routes/resetti.txt on a NAND whose last session was not saved) 2438 -> 378 (dirt site
  `fn_8028FF0C` 2381 -> 322; far leaps within an interval 251 -> 19); town walk (town_trees.txt)
  1510 -> 403; town with the field-object tags off (`CF_FG_TAG=0`, the generic matcher alone) 7609 ->
  424 (trees 6273 -> 130, as good as the tags: 403); house 80 -> 81; city plaza 610-1208 (two runs) ->
  456; fireworks (2026-08-02 20:00) 12398 -> 10197, still high in both (the show is a sky backdrop
  with its own camera and dense radial bursts: the detector's nearest-point tracking and the world
  places are least reliable there; open). In-betweens (`CF_INTERP_TEST=4`, out/interp-match/
  rest_dirt2.png): with draw order the clods reshuffle between game frames, matched the cloud
  evolves smoothly. Matching cost (guest, per game frame): 0.04 (house) - 0.34 (city) ms average,
  max 1.5 ms (one 6 ms preemption spike in the city), 0 on the host per present. Original 60: no
  capture, so none of this runs (no `[match]` lines); frame dumps of the title and bus (game frames
  2000-3800) identical to main in both runs of each build; town frames differ only in the idle /
  water animation phase, as two main runs also differ from frame 5400 (out/interp-match/o60diff.png).
- Static check (out/interp-match/coverage.md): 94 draw call sites (GXBegin / GXCallDisplayList
  callers): tagged 13 (g3d model drawing 11, the ef billboard, lyt panes), fingerprint 63 (game code
  39, EGG 14, ef strategies 10), 2D/system 18 (RFL, HOME Menu, ut text); none flagged "needs help" by
  the runs (the most left at the game frame: ef `fn_8028E7A8` 3.5 %, `DrawQuad(VEC3,VEC3,VEC3)` (the
  fireworks) 2 %). Display lists written straight into the pipe (most g3d shapes) show up only in
  the runtime part, by the last GX call's site (ResTev::CallDisplayList, LoadResShpPrimitive).

### Matching untagged draws, round 2 (branch interp-match-2)

User: "still a lot of jumps, shouldn't there be 0?". The detector now classifies every event, so its
number of visible defects means what it says (`E <a|b|c> <reason>` lines, `CLASSIFIED` in the 5 s line):
- It follows each point along its **exact** path too: the same draw and slot within a game frame's
  in-betweens, the interpolator's partner draw (`Tape::bundlePartner`, now also for draws placed by
  another draw's matrix: `bundleOwner`) across the change.
- (a) visible defect: both the exact and the on-screen (nearest same-kind point) paths jerk, and either
  the speed changes inside one interval, or the point turns back where the game frames do not
  (`pop`), or a draw of its kind was much nearer in the previous game frame than its partner, also
  with the last step as prediction and also in the draw's own matrix space (`wrong-pair`).
- (b) by design: `motion` (the game's own velocity change at a game frame: logic at 60), `arc` (a
  rigid blend turning a part: same speed, turning direction), `game-pop` (the game frames themselves
  go there and back), `game-frame` / `born` (no partner: shown at the newest game frame / new),
  `appeared` (its partner was not drawn in the previous present), `cut`.
- (c) the detector's own mismatch: `detector` (its nearest point was another identical one; the exact
  path is smooth), `detector-new`, `exact-only` (the exact path jerks but on screen the point moves
  smoothly: a partner table that is not what is drawn there).
- Fixes found by it: draws without values of their own get the partner of the draw whose matrix
  places them; things drawn with a camera of their own (the fireworks show, a sky backdrop: view-space
  matrix the same in both game frames while the town camera moved) are blended as drawn
  (`cameraAttached`, Patch::ownCamera; `CF_CAM_ATTACHED=0`, `CF_CAM_ATTACHED_LOG=1`), matched in the
  space they are steadier in (`CF_MATCH_SPACE=0`), and the detector follows them on screen; the
  alignment now covers classes up to 200000 draw pairs (fireworks bursts).
- `SNAP <site> <n>` lines: point-presents shown at the game frame per draw site (effects at 60 fps).

### Matching untagged draws, round 3 (branch interp-match-3)

- **Every ef particle is its own identity** (cf_tags.cpp `cftags::efBegin`, hooked from cf_light's
  DrawOrder::Draw; interp `setEffectParticles` + gxCall): while an effect draws, all particles of its
  managers (Effect + 0x90 -> + 0x38 lists, birth = frame - age at + 0xDC) are known; a draw strategy's
  loop keeps the particle it draws in a callee-saved register, so each GX call there becomes that
  particle's scope (unless the billboard DrawQuad hook already opened one). This covers every strategy
  (fn_8028E7A8, fn_8028F414, fn_8028FF0C, DrawQuad(VEC3, VEC3, VEC3) ...): fireworks bursts
  (af_fld_firework, _00/_01/_02, _child, _st_child, _top), fish ripples and shadows (af_fsh_hamon,
  af_fsh_shadow), emote marks, Mr. Resetti's dirt and the running dust; the fireworks route had no
  fireflies. A/B `CF_EF_TAG_ALL=0`; `CF_EF_TAG_LOG=1` names each manager's emitter and draw sites.
- Third fingerprint level without the position array (`bundleFp3`: double-buffered CPU-built arrays).
- Equal counts in both frames, each draw within reach of the one at its place in the order and no
  outlier step: the order is the identity (several identical parts of one model; `CF_MATCH_ORDER=0`).
- Values of matched draws pair by matrix slot too (g3d loads only changed slots: by order alone a
  skinned villager's envelope matrices swapped slots and the part went there and back).
- Detector: wrong-pair only for draws matched by place (a tagged identity is the game's own).
- Results at 120 fps (quiet PC; visible defects / by design / detector mismatches): Resetti 0 / 129 /
  240, town walk 0 / 958 / 120, house 0 / 67 / 30, fireworks 1 / 9486 / 7965, city plaza 233-280
  (two runs) / ~950 / ~19. Open: the city's "pops" are walking villagers (skinned g3d draws, site
  ResTev::CallDisplayList) that go there and back at every in-between around game frames 6040-6880;
  their matrices pair correctly (same sources and slots, CF_MATCH_TAPE), and they also pop with
  `CF_MATCH=0` (94), so the cause is not the pairing (it was unnoted indexed matrix loads after a
  list's first draw: fixed, "Skinned villagers in the city" below).
  Resetti re-dumped (out/interp-match-3/rest3_dirt.png): the clod cloud evolves smoothly in both builds
  now (the per-particle ef tags apply with CF_MATCH=0 too).
- Running dust: the game changes its speed every frame; a curve through three game frames needs the
  next one (a frame more latency) or extrapolates backwards (C1 only at the start, overshoots on a
  turn), so the straight blend stays.

### Skinned villagers in the city (branch city-skin)

Round 3's open item: at 120 fps the walking villagers of the city plaza (city_plaza_warp.txt, game
frames ~5980-7000) went there and back at every in-between, visible defects 233-280 (1395 in a run
on a busy PC; 94 with `CF_MATCH=0`), all at the draw site ResTev::CallDisplayList (g3d shapes whose
primitive lists the game writes straight into the pipe).

- **Not the blend**: the popping point stood still on screen within each game frame while the
  camera blended (the same view-space matrix at alpha 0.25 and 0.75). With `CF_JUDGE_DIAG=1` (below)
  the matrix it was drawn with was neither the blended value the replay wrote for the noted load in
  that slot nor any noted value of either game frame: an unnoted matrix load, frame b's.
- **Cause**: NW4R g3d draws an envelope (skinned) shape as one display list that loads a group of
  envelope matrices with indexed loads (LOAD_INDX_A/B from the matrix array g3d rewrites every
  frame), draws the triangles of that group, loads the next group, draws, and so on.
  `convertListIndexed` (hle_gx.cpp; inlining the list with its indexed loads made immediate and
  noted, "Rendering status (branch render-3)" above) followed the list only up to its **first draw**, so every group
  after the first kept its indexed loads: replayed, they read the tape's copy of the matrix array
  (frame b's matrices, never blended) while the first group's matrices blended. The town's villagers
  and the player have one group per list in the shapes seen, so only the city's walkers (14 matrices,
  several groups) tore.
- **Fix**: the conversion steps over draws, sized with the vertex layout as the list itself sets it
  (its VCD/VAT LOAD_CP_REG writes applied to a copy of the layout mirror, which is put back
  afterwards: the list is parsed again from its start by vtxDisplayList), so the loads of every group
  are made immediate and noted (city: ~70 more converted loads per game frame). A list with a draw
  that cannot be sized is followed only up to it, as before. A/B `CF_DL_INDEXED=first` (old), `=0`
  (no conversion).
- **Results** at 120 fps, visible defects / by design / detector mismatches (same PC load, same day;
  main = b561412): city plaza 1395 / 2688 / 20 -> **0 / 139 / 16** (the ResTev::CallDisplayList
  "motion" events, 2489, were the same tearing and are gone too); a second pair of city runs 228 / 362
  / 16 (main) -> 0 / 136 / 19; city with `CF_MATCH=0` 0 / 205 / 196; fireworks (2026-08-02 20:00)
  0 / 9564 / 8547 (round 3: 1 / 9486 / 7965); town walk 0 / 912 / 111 (main) -> 0 / 926 / 99; house 0 / 73 / 30 (main) -> 0 / 72 / 35. The
  city's remaining (b) are things that appeared (culled the interval before), effects shown at the
  game frame (`fn_801C8828`, no sure partner) and game-frame velocity changes; (c) the detector's
  own nearest-point mismatches.
- Original 60: nothing is captured, so the conversion never runs (it is reached only from the
  capture paths, GXCallDisplayList's and hw::fifoWrite's `interp::capturing()` branches; an Original
  60 run of the city route logs no capture at all). Frame dumps of the city route every 100 game
  frames: 2000-3400 (title, bus) identical in main, this build and a second main run; from 3500 on
  even the two main runs differ (the city route is not deterministic run to run: its visitors and
  their animation phases), so they prove nothing there.
- Test aid `CF_JUDGE_DIAG=1` (with `CF_MATCH_JUDGE`): keeps the slot table and writes, after each
  visible defect, a `D` line (the patch its slot held, key / source / group / partner / policy,
  whether the drawn matrix is what the replay wrote for it, the translations in a, b and as drawn) and
  `DM` lines (the captured values of either game frame equal to the drawn matrix, or `none`: an
  unnoted load).

### Scripted camera moves at high frame rates (branch cam-jumps)

User report (main 0c37043, 4K, 120 Hz G-Sync): "big jumps during camera movement at high frame
rates (entering Nook's, catching a bug or fish, etc.)". Routes: tools/routes/cam_door.txt
(user-town-clean, `CF_FF=1-4400`: house -> Nook's Cranny by walking, the door camera, Tom Nook's
talk, the shop, out again, the pockets, a villager talk) and gameplay_fish_beach.txt (the sea bass
catch close-up); 120 fps, VSync off, 1280x720, and the user's own settings (Enhanced lighting Ultra,
4K window, a copy of the user's shader cache).

- **The camera blend is not it.** New test aid `CF_CAM_PRESENT=<file>`: per present the main camera
  as shown (eye, view direction, projection scale), its tapes, alpha, shown time and cut state;
  `py tools/interp/cam_steps.py <file>` turns each present's camera step into pixels of a 2160-line
  picture (turn x focal length + eye move / scene depth + zoom), as velocity per game frame (jerks
  within an interval / at a game-frame change) and per present (`leaps`: one present's step over 3x
  its neighbours'). The door camera, the walk into the shop, the shop camera (~10 units per game
  frame), the exit, the catch close-up and the talk: 1-4 jerks in 900-6000 moving presents, no false
  cut (cut presents only at scene changes and the route's teleports), the projection constant (City
  Folk does not zoom by FOV). Picture steps of `CF_INTERP_TEST=2` dumps (phase correlation per tile)
  even through the door camera: the in-betweens move halfway.
- **The jumps are host stalls the shown time jumped over.** A pipeline first met in play is
  compiled while the render worker waits for it (Aurora 0013): 150-200 ms each at default settings,
  662 ms for the three Enhanced-lighting pipelines of the catch with the user's settings and cache
  (`CF_PIPELINE_LOG` MISS frames = the present gaps, one for one: 6 misses = 6 gaps in a fresh-cache
  fishing run). They fall exactly on these moments: the rod in the hand, the first cast, the fish
  pulled out (5 game frames before the close-up camera starts), the first frames in a room. At the
  game's own rate the guest waits for the frame, so the picture pauses; interpolated, the game runs
  on (decoupled) and the next present showed where it was by then: 0.19-0.66 s of camera motion in
  one present (fish1 present 17745 -> 17746: tapes 10199 -> 10211). The user's last_run.log (52 min
  at 120): 25 such hitches of 220-430 ms render worker time ([perf] max).
- **Fix 1, the stall hold** (interp.cpp `pace`, `Pacer::debt`): when no present came for over 4
  periods (at least 40 ms, under 1 s) the shown time goes on one period after the last present, as
  at the game's own rate, and catches up by playing the tapes 25 % faster (`CF_STALL_CATCHUP`), at
  most 0.5 s behind and never behind the oldest tape kept (queue 8 -> 40 tapes, `kQueueTapes`); log
  line `[interp] no present for N ms: the picture goes on from where it stopped`. A/B
  `CF_STALL_HOLD=0`; test aid `CF_TEST_HOST_STALL=<game frame>:<ms>,...` (the host stops after that
  frame's first present). Door route with stalls of 200-300 ms in the town walk, the door camera,
  the shop camera and the exit: leaps with the camera moving 2 (318 / 444 px against 16 / 79 px
  neighbours) -> 0.
- **Fix 2, the precache list**: data/pipelines.bin merged with the configs these runs met and the
  user's own pipeline cache (2537 -> 3379 configs). Fresh-cache fishing: 6 misses -> 2 (both after
  the route's later teleports); door route 0. Startup hold with a seeded cache 7 s (cold
  compiles). Every config first met in play is still a hitch; the hold keeps it from becoming a jump.
- **Fix 3, the circle wipe** (cf_hud.cpp fn_802B6B48): its texture matrix was untagged (key 0, shown
  at the game frame), so the circle closed and opened in game-frame steps while the scene inside
  blended at every present (picture steps 0.3-1.6 within an interval vs 3-7 across). It is a HUD
  scope of its own now and pairs: the circle grows at every present (steps even). A/B `CF_WIPE_TAG=0`.
- Judge (`CF_MATCH_JUDGE`) visible defects after the town arrival (the rest are in the
  fast-forwarded title / bus): door route 0 (main 0), town walk town_trees.txt 0 (main 0; camera
  jerks 1 + 4, leaps 0; main 4 + 7, 0), city plaza (city_plaza_warp.txt) 0 (camera jerks 1 + 1, leaps 0). Fishing: 22 (main 9), all water
  effect particles (ef strategies 80291718 / 802905A0, splashes and ripples popping), the same
  sites on main; their number follows how many bites a run gets, not the camera.
  Original 60: none of this runs (no capture, no pacer; the wipe scope is bookkeeping): door route
  dumps every 50 frames 92 of 120 identical to main, the rest in the fast-forwarded part and event
  timing (a second main run matched neither).
- Not changed: camera steps from a standstill of 190-225 units in one game frame (the routes'
  `TP=` teleports) stay below the 250-unit cut threshold and blend over one interval; no such step
  was seen in play. With three or more game instances on this PC, game frames sometimes arrive two
  at once and the shown time skips a tape (60-70 px leaps in a town walk run beside other
  instances; none alone, main or branch).

### Splashes and ripples while the camera moves (branch fish-fx)

What branch cam-jumps left open: at 120 fps the fishing route (tools/routes/gameplay_fish_beach.txt)
still read 9-25 visible defects per run on main, all water effects (`af_fsh_hamon`,
`af_fsh_hamon_child` and the splash emitters; ef draw sites 80291718 = fn_80291120 and 802905A0 /
8029077C = fn_8028FF0C), in the frames after a bite, when the fish is pulled out and while the
catch close-up camera moves. Part of the user's "big jumps when catching a bug or fish".

- **Cause**: the nw4r::ef *directional* draw strategies (fn_8028FF0C, fn_802908D4, fn_80291120;
  their common setup fn_802918C0) build every particle's quad on the CPU **in view space** and
  draw it with an **identity position matrix** (`GXLoadPosMtxImm(identity)` at 80291974, once per
  particle manager). The particles are their own identities (round 3) and their vertices blend in
  view space, which is right. The identity matrix, though, is one value per manager under the
  effect's ScnProc, in a bucket with the other managers' ones, so it is matched by its draw: the
  manager's first drawn particle. Whenever that particle was new (born in this game frame, or the
  first one of the frame before had died) the matrix had no partner, and an unpaired position
  matrix is "placed under the in-between camera" (V(t) V(b)^-1 x its value). That moved **every
  particle of that manager** by the camera's remaining step for the interval, on top of their own
  blend, and back at the next game frame: 30-130 px (of a 2160-line picture) there and back, only
  while the camera moves. With a standing camera the product is the identity and nothing shows,
  which is why ripples under a still fishing camera were fine and the catch was not. Judge `D`
  lines (`CF_JUDGE_DIAG=1`): `site 80291974 ... partner -1 ... b 0.0 0.0 0.0 drawn -0.1 1.8 -1.0`
  (the slot's matrix is the identity in the tape and was drawn moved); the pops one interval later
  at 802905A0 with the matrix paired (`a 0.0 b 0.0 drawn 0.0`) are the way back.
- **Fix** (interp.cpp `identityMtx`, `placeVerts`): a position matrix that is exactly the identity
  places nothing, so it is never placed under the camera and never blended (as captured). The
  CPU-built vertex blocks drawn with it that have no partner (a particle's first game frame) or
  were placed anew (a jump over 200 units, a reversal) are the ones moved by V(t) V(b)^-1: they
  stand still in the world for that interval, like every other unpaired thing, instead of riding
  the camera. Stats: `view-space blocks without partner placed under the camera` in the
  `[interp] colour registers` line. A/B `CF_VIEW_SPACE_VERTS=0`.
- **`cameraAttached` (round 2's rule for the fireworks backdrop)**: (1) it took these identity
  matrices for things with a camera of their own (the same in view space while "their place", the
  eye, moved), so the judge followed their particles on screen without the camera; they are
  excluded. (2) Its mark came and went with every other game frame: a partner marked the frame
  before was skipped (`q.ownCamera`), so every second interval was blended camera-factored again
  and the judge saw the space flip (the 7 "pops" at 802905A0 left after the fix above, all with
  `own 1` / `own 0` alternating in the `D` lines). A mark now stays while the thing stands still in
  view space (`Patch::attached`). A/B `CF_CAM_ATTACHED_STAYS=0`.
- **Judge**: the game's own there-and-back is now also told by the tracked vertex itself, through
  the matrices its slot held at capture in the three game frames, seen on screen through the
  present's camera (the slot table is kept whenever the judge runs; `DG` line per visible defect).
  The old test used the world step of the draw's centre, which says little about a part that
  turns about an origin far from it: the float at the end of its flight (17 game frames after the
  cast: it falls 6 units a game frame and its sideways step turns round; 1 "pop" per fishing run)
  and a part of the player's model 6 game frames after the bite is answered (there and back at
  100-160 px per game frame between three game frames; 2 of 4 bites in the first main run) read
  as visible defects. Both are the game's own motion, shown as the game frames have it.
- **Checked and not defects**: the cast (rod wind-up and return, the float swinging on the line:
  in-between strips of `CF_INTERP_TEST=4` dumps over 16 game frames move evenly), the float on the
  water, the fish model pulled out, the caught-item pose and the text box: no visible event in any
  run. A particle's first game frame (`born` / `game-frame`, 1200-1600 per run, mostly the ripples'
  billboards at 80291FD8) is by design: it stands at its first place for one interval, since its
  velocity is not known (open: the ef particle's own velocity could feed `tagBackStep`).
- **Catching a bug** (new route tools/routes/gameplay_bug_net.txt, docs/TEST_COVERAGE.md): the net
  swing, the catch close-up and "I caught a robust cicada!" read 0 visible defects with the old
  behaviour too (0 / 711 / 312 old, 0 / 702 / 311 new): no view-space effect is alive while that
  camera moves.
- **Results** at 120 fps, VSync off, 1280x720, visible defects / by design / detector mismatches,
  game frames from 4500 on (after the fast-forwarded title and bus; two instances at a time on
  this PC). Old = the same build and judge with `CF_VIEW_SPACE_VERTS=0 CF_CAM_ATTACHED_STAYS=0`.
  Fishing (2-4 bites per run, which varies): main exe with its own judge 25; old 27 / 2807 / 1693
  and 23 / 3480 / 2019; new **0** / 2504 / 1688, **0** / 2404 / 1639, **0** / 1678 / 1114. Town
  walk (town_trees.txt) 0 / 449 / 75, city plaza (city_plaza_warp.txt) 0 / 226 / 14, house
  (house_furniture_warp.txt) 0 / 57 / 22, door route (cam_door.txt) 0 / 388 / 117. Fireworks
  (town_trees.txt on 2026-08-02 20:00): old 2 / 13271 / 9831, new 2 / 5874 / 8455: the bursts'
  "motion" events at 80292124 fall from 8709 to 2508 with the mark that stays; the 2 visible ones
  are the same in both (one g3d sky object 1300 units away at the top edge of the picture that
  steps 404 units sideways in one game frame, f11513, and is blended across: a wrap; on main too,
  open).
- Original 60: nothing here runs without a capture (replay, pairing and judge code; the test aids
  are behind their variables). Frame dumps of the fishing route every 100 game frames 4500-9000
  (the arrival, the pockets, two casts): this build against a main run 35 of 46 identical, the
  two main runs against each other 23 of 46 (the route is not deterministic run to run: load
  timing shifts the arrival), a second run of this build 0 of 46 against all three.
- Not done: a particle's first interval (above), PAL/JP/KR, the river and pond fish, a real 120 Hz
  display.

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
| USB (`/dev/usb/kbd`, `oh0`) | USB keyboard (letters), Wii Speak | the PC keyboard while F1 > Controls "Type with the PC keyboard" is on (docs/CONTROLS.md), else no keyboard; USB host with no devices (Wii Speak: docs/ONLINE.md 10e) |
| SD (`/dev/sdio`) | photos, design/letter backup | slot with no card for now; plan: SD host over a FAT image (docs/NAND.md "Not done") |
| Network (NCD/SO/SSL/NWC24/DWC/KD) | Nintendo WFC online play, WiiConnect24 letters/events | offline: `/dev/net/*` not running (opens fail); later host sockets + TLS, revival servers (docs/ONLINE.md); real-time weather lookups are host-side |
| Miis (RFL) | Mii masks | the game's own parts (`Face/RFLRes01.arc`); no `RFL_DB.dat` = RFL's empty-database fallback; F1 > Miis makes / imports the database and hands it to the running game (docs/MIIS.md) |
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
