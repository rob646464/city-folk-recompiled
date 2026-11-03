# Texture packs

Replacement textures in Dolphin's format, a Dolphin-compatible texture dumper for pack makers.
Code: `src/recomp/texture_packs.cpp` (packs folder, menu, settings, test aids) on top of Aurora's
texture replacement (`extern/aurora/lib/gfx/texture_replacement.cpp`, upstream) and Aurora patch
**0360** (dumper, cache budget, statistics, use log; generic, docs/BACKPORT.md).

## For players: installing a pack

1. F1 > Visuals > Texture packs > **Open texture packs folder**. It is `texture_packs` in the data
   folder: `%APPDATA%\Animal Crossing City Folk Recompiled\texture_packs` (Linux:
   `~/.local/share/Animal Crossing City Folk Recompiled/texture_packs`; portable mode:
   `<program folder>\portable\texture_packs`).
2. Put each pack in its own folder there. Packs made for Dolphin are copied as they are; any of
   these layouts work (the game ID is the running version's: RUUE01 USA, RUUP01 Europe, RUUJ01
   Japan, RUUK01 Korea):
   - `texture_packs\<pack>\RUUE01\...` (what Dolphin packs usually ship: the folder you would put
     in Dolphin's `Load\Textures`)
   - `texture_packs\RUUE01\...` (that folder dropped in directly)
   - `texture_packs\<pack>\Load\Textures\RUUE01\...`, `...\Textures\RUUE01\...`,
     `...\User\Load\Textures\RUUE01\...` (a copy of a Dolphin user folder)
   - `RUU` instead of the full ID, or a marker file `RUUE01.txt` / `RUU.txt` in the pack folder
     (Dolphin's conventions for "this game, every region" and "the whole folder")
   - a folder with the texture files and no ID at all: used for every region.
   A pack whose ID folders are all for other regions is listed greyed out ("other region") and not
   loaded. Subfolders inside a pack are fine (searched recursively).
3. Press **Reload packs** (or restart). No restart is needed for adding, removing, editing,
   enabling or reordering packs.

The list shows each pack with an on/off box, its number of textures and up/down arrows: where two
packs replace the same texture, the one higher in the list wins. "Use texture packs" turns them all
off (every texture is the game's own again). Settings: `[texture_packs]` in settings.ini
(`enabled`, `budget_mb`, `dump_mips`, `pack<N>=<0|1>,<folder>` in priority order; new packs are
added enabled at the bottom).

Files: `tex1_<w>x<h>[_m]_<texture hash>[_<palette hash>]_<format>.png` or `.dds` (DDS: BC1-BC7
and uncompressed RGBA8; mip levels inside the DDS or as `<name>_mip<N>.png/.dds` sidecars;
`_arb` arbitrary-mipmap suffix accepted; `$` wildcards for either hash). Replacements may be any
size (upscaled packs are the usual case). They stream in on background threads: for the first
frames after a texture appears the original is drawn.

### The RUUE01HD project pack

Not downloaded or tested here (no copy on this PC). If you have it: it is a Dolphin pack, so
unpack it and copy its `RUUE01` folder (the one that goes into Dolphin's `Load\Textures`) into
`texture_packs\RUUE01HD\` (giving `texture_packs\RUUE01HD\RUUE01\...`), then Reload packs. Its
DDS files (BC7) are uploaded as they are; large PNG packs take 4 bytes per texel of graphics
memory, so raise "Texture memory for packs" (below) on cards with 8 GB or more.

### Memory

"Texture memory for packs" (512 MB .. 8 GB, default 2 GB) bounds the GPU memory replacement
textures may hold; past it the least recently drawn ones are released and stream in again when
they are next drawn (a short pop-in of the original). Every registered DDS keeps a small
thumbnail (mip tail up to 64x64) in memory for instant pop-in; PNGs have none. The `[mem]` lines
in the log (every 60 s) have a `texture packs:` line: registrations, replacements on the GPU and
their MB against the budget, pending loads, failed files, texture loads replaced, dump counts.

## For pack makers: dumping

F1 > Visuals > Texture packs > For pack makers > **Dump textures** writes every texture the game
loads, once each, as PNG into `<data folder>\texture_dump\<game id>\` (Open dump folder). The
textures already on screen are written at once (the texture cache is re-resolved when dumping is
switched on). The file names are exactly Dolphin's "Dump Textures" names, so an edited (recoloured,
upscaled, saved as DDS) copy dropped into a pack replaces that texture here and in Dolphin.
"Include mipmaps" also writes `<name>_mip<N>.png` for mipmapped textures (Dolphin's "Dump Mip
Maps"). Dumping is not saved: it is off at the next start. Existing files are never overwritten.
EFB copies (screen copies, shadows, the map's render targets) are not textures and are not dumped,
as in Dolphin.

## How a texture is matched (and why Dolphin's names work)

Aurora computes the same key Dolphin does when a GX texture is first resolved (not per draw; the
texture object and content caches keep it): XXH64 (seed 0) of the **base level** as the game stores
it in memory (block-padded size of the format), the size, the GX format number, and for C4 / C8 /
C14X2 an XXH64 of the palette entries the texture actually uses (the range between its smallest and
largest index). `_m` is Dolphin's "the min filter uses mipmaps" flag (TexMode0 mip bits); the
loader ignores it, the dumper writes it as Dolphin does.

**Checked against Dolphin** (2026-10-28, US rev 1, `Dolphin-x64` with a scratch user folder,
GFX.ini `DumpTextures = True`, booted from `US DATA/DATA/sys/main.dol`, title screen, no input,
75 s; our port the same scene with `CF_TEXDUMP`): Dolphin dumped 126 textures, the port 120; **98
names are identical and all 98 files are pixel-identical** (formats I4, I8, IA4, IA8, RGB565,
RGB5A3, RGBA8, C4, C8, CMPR). The rest differ by scene content between the two runs (Dolphin's
real clock vs the run's fixed clock and RNG seed: other title-town objects, and palettes animated
by the time of day: two textures appear in both with another palette hash, among them the only
C14X2 one). City Folk uses no mipmapped textures in the title or the town (no `_m` names from
either emulator).

## Interactions with the port's other features

- **Skin tone** (F1 > Enhancements > Gameplay): player skin/face textures are C4 with one palette
  per tone; the palette hash is part of the name, so a pack replacing `..._<tlut of tone 3>_8`
  changes only that tone and the picker keeps working. A pack that uses `$` for the palette hash
  replaces the texture for every tone (the picker then has no visible effect on it): pack makers
  should keep the palette hashes for skin textures.
- **EFB copies / copies to RAM**: never replaced (resolved before replacements, as in Dolphin).
- **Interpolation**: frames replayed at high frame rates resolve textures from a capture-time copy
  of the texels; the hash is of the content, so the same replacement is chosen (no tags involved).
- **Textures the game rewrites** (patterns, designs, the town map): their hash changes with their
  content, so only a pack entry for that exact content matches.

## Test aids

- `CF_TEXPACK_DIR=<dir>` packs folder instead of `<data folder>\texture_packs`
- `CF_TEXPACKS=0|1` master switch
- `CF_TEXDUMP=1|<dir>` dump from the start (into `<dir>`)
- `CF_TEXPACK_LOG=<s>` the `[mem] texture packs:` line every s seconds (1 = 5 s) and an Aurora info
  line for the first use of each replacement (`texture_replacement: using <file> for <key>`)
- `CF_TEXPACK_AT=<frame>:<action>[,...][;<frame>:...]` at guest frames: `reload`, `on`, `off`,
  `dump`, `nodump`, `budget=<MB>`, `pack=<folder>:<0|1>`, `top=<folder>` (live changes as the menu
  makes them)
- Scratch tools used for the checks (not in the repo): a dump comparer (names + pixels) and a pack
  builder (recolour + 4x upscale from a dump, one DDS, a second pack for priority, a RUUP01-only
  pack).

## Verified (2026-10-28, USA, fixture user-town + continue.txt, scratch packs never committed)

- **Small pack** (recoloured 4x PNGs of the town ground, dirt (C8), a C4 gradient, a player face
  skin texture (C4 + skin palette), and the HUD clock (RGB5A3) as an uncompressed RGBA8 DDS; a
  second pack `Override` with no ID folder replacing the clock in magenta; `PalOnly/RUUP01`): every
  replacement used (log), Override above TestPack showed the magenta clock, `top=TestPack` at frame
  7600 switched it live to TestPack's DDS clock, PalOnly listed "not for RUUE01" and not loaded.
  Skin tone fixed to 8 at frame 6400 (CF_SKIN_TONE_AT) with the pack loaded: the face rebound to
  the dark palette as without a pack (its palette hash differs, so the replacement made for the
  previous tone no longer applies, as intended).
- **Big pack** (all 191 textures of the town dump, colour-swapped and 4x upscaled, plus 5000
  never-drawn 256x256 PNGs): scan + registration 72-87 ms on the loader thread, the whole town and
  HUD replaced, interpolated at 120 fps target (CF_SETTINGS_AT video.target_fps=120) with normal
  pairing; `budget=64` at frame 7000: resident replacements held at 61-64 MB (LRU pop-in visible on
  the forest backdrop, as expected for a budget below the scene's 140 MB), `off` at 8000 restored
  every original texture, `on` at 8600 brought the pack back without a restart. Memory: private
  ~1760 MB with the big pack vs ~1500-1555 MB without (town, same route), flat over the run;
  replacements on the GPU 118-142 MB at the default 2 GB budget.
- **Dumper**: 182-191 textures from title to town (PNG, 1.3 MB), names identical to Dolphin's
  (above); no mipmapped textures in these scenes.
- **Menu**: F1 > Visuals > Texture packs page passes the CF_MENU_ID_CHECK sweep (no ID conflicts).
- Not tested: a real third-party pack (RUUE01HD not on this PC), DDS BC7 files, PAL/Japan/Korea
  (same code path: the ID comes from the running disc), Linux/macOS folder opening.
