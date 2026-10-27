# Game features: grass wear, skin tone

F1 > Enhancements > Gameplay. Both default to the original game. Research behind them:
docs/FEATURE_RESEARCH.md sections 2 and 3 (web sources, untrusted); this file is what the game
does, as traced in the running USA game, and what the port changes.

## Grass wear

Code: `src/game/cf_grass.cpp`; hooks `fn_8008D6B4` (step wear) and `fn_800A7F9C` (daily
regrowth) in `tools/recomp/hooks.txt`, with their PAL (`fn_8008D6D4`, `fn_800A7FBC`) and Japan
(`fn_8008D63C`, `fn_800A7F24`) names (`config/<id>/us_map.txt`, hash matches: same code, only the
addresses differ). Setting `[gameplay] grass_wear` = 0 Original, 1 New Leaf style, 2 Off.

### What the game does (USA rev 1)

| Address | What |
|---|---|
| `fn_800A835C` | returns the grass manager, a static object (USA 0x80591C00, PAL 0x80594880, Japan 0x80596080) |
| manager +0x58 | 128 x 128 tile health array (8 x 8 acres of 16 x 16 tiles, one byte per tile, 0 bare .. 255 full grass), stored in 8 x 4 blocks like an I8 texture (buffer at manager +0x80, pointer at +0x7C). A generic "tiled 2D array": `fn_802B6034` dimensions, `fn_802B6338` init, `fn_802B637C` get, `fn_802B63CC` set |
| manager +0x4080 | 128 x 128 bit map: tiles already worn since it was last cleared (`fn_800A77FC` test, `fn_800A7830` set, `fn_800A773C` clear) |
| town data +0x38A2 | the saved copy: 25 acres x 0x100 bytes (acre stride 0x100, row of acres 0x500; `fn_80110E7C(town, ax, ay)`), each acre 16 x 16 in 8 x 4 blocks. The town data is the save image (`fn_8010E308` returns its base) + 0x68414, so the grass is at save + 0x6BCB6, as ACSE has it |
| `fn_800A7C84(mgr, x, y, value, flush)` | writes one tile, clamped to the tile's minimum / maximum (`fn_800A7DA0`, `fn_800A7E10`: 0 under buildings, plaza, paths), into the save copy and the manager's array |
| `fn_800A7E80(mgr, x, y, delta, force, flush)` | adds `delta`; unless `force`, only if the tile is not in the worn bit map, which it then sets: **a tile wears once per load** |
| `fn_800A7F14(mgr, pos, delta, ...)` | world position to tile (`fn_8006CC64`: 32 world units per tile), then `fn_800A7E80` |
| `fn_8008D6B4(field, pos, amount)` | `fn_800A7F14` with `-amount` |
| d_a_playerNP `fn_80_2334` | the player's step: whenever the player enters a new tile (last tile at player +0x2284/+0x2288), `fn_8008D6B4(field, &player->pos, amount)` with amount = 4 + rand(4) (4..7), or 8 + rand(4) (8..11) while player +0x2270 bit 0x00800000 is set (not running: running steps were 4..7 too; snow is the likely meaning, per the web sources' "winter 8-11"). Walking and running wear the same |
| `fn_8008D658` | town generation: `fn_8008D6B4` with 255 (bare ground under new buildings) |
| `fn_800A7F9C(mgr, days, ?, flush)` | the **daily regrowth** (called through `fn_8008D70C`): tiles with a plant or object get its bonus (item data +0x0E), then every tile gets `1 + (sum of its 8 neighbours / 8) / 85`, i.e. +4 in healthy grass, less beside bare ground; both forced; then the worn bit map is cleared. It runs at the new day while playing (the 6 AM "start of a new day" scene, verified with a clock jump) |
| `fn_800A822C(mgr)` | clamps every tile to its minimum / maximum (load) |

Player speed (player +0x88, read next to the step code): walking at full stick 2.0 units per frame,
running (B held) 2.7 (3.0 on the first step). Villagers never called the step wear in the runs.

### What the port does

- **Original**: hooks call the originals; byte-identical.
- **Off**: player steps never wear (the step call returns without calling the game). Town
  generation still clears the ground under new buildings. Regrowth unchanged, so bare patches heal.
- **New Leaf style** (our estimate; no source gives New Leaf's numbers):
  - only steps taken while running (speed above 2.35) wear; walking never does;
  - a tile wears at most once per game day (a day runs from 6 AM to 6 AM on the console clock), on
    top of the game's own once-per-load rule (whichever is stricter);
  - the wear is halved, rounded up: 2..4 (snow flag: 4..6) instead of 4..7 (8..11);
  - the daily regrowth runs twice: about +8 per day in healthy grass.
  So a path run along every day loses about 3 and regains about 8: no permanent bald paths, while
  heavy running across one day still leaves a visible trail. Not implemented: New Leaf's extra
  regrowth after rain/snow days (the weather state is not known to the port yet).
- **Regrow all grass now** (button): every worn tile (health 1..254) of the 5 x 5 town acres set
  to 255 through the game's own `fn_800A7C84` (clamped to the tile's maximum), into both copies;
  saved with the town at the next save. Tiles at 0 are left alone: the town generation sets the
  ground under buildings, objects and the plaza to 0, and the setter's clamp does not protect it (a
  first version raised every tile and grew grass over the bus-stop plaza).

### Verification

Route `tools/routes/grass_walk.txt` (`gen_town.py`): new game to the town gate, A through the map
tutorial, then a square from the bus stop run (B held) and walked in turns. Runs fast-forwarded to
the town with `CF_FF=0-16600`. Every new game makes a new random town, but the bus-stop area the
route runs around is the same, so the tiles stepped on match between runs.

- Original, `CF_GRASS_LOG=1`: running and walking steps both wear 4..7 (e.g. `step 53,21 amount 5
  speed 2.70: original (255 -> 250)`), a second step on the same tile in the same load does
  nothing (`187 -> 187`); the new day (clock jump of one day, `CF_CLOCK_JUMP`) shows the game's
  "start of a new day" scene and runs the regrowth live (`regrowth (days 1 ...): sum 3844647 ->
  3849694, tiles under 64: 586 -> 567`).
- Original vs Off vs New Leaf with `CF_GRASS_WEAR_X=40` (so one lap shows) and `CF_GRASS_MAP`
  tile maps at frames 17150 / 19150 / 20990 (each mode its own town): Original 40 tiles lost
  health (-7877 in total) and the square shows as a bare outline in the map and the screenshots;
  Off 0 tiles; New Leaf 35 tiles, -3781 (running legs only, halved; the walked legs log `new leaf:
  walking, no wear`, a second running lap the same day `new leaf: already worn today`). After the
  day jump New Leaf's double regrowth gave +10502 against Original's +5047.
- Regrow all (`CF_GRASS_REGROW_AT`): 1781 worn tiles raised, the 326 tiles at 0 unchanged.

## Skin tone

Code: `src/game/cf_skin.cpp`; hooks `d_a_playerNP::fn_80_2080` (the player's skin palette index;
same REL offset in all three versions) and `fn_80137D34` (daily tan update; PAL `fn_801379E4`,
Japan `fn_80137924`). These are the first REL hooks: `callOriginal` for `<module>::<name>` hooks
was added for them (the recompiler now puts hooked REL functions' original bodies into the
`orig` table under `relfmt::relOrigKey`, `src/recompiler/rel_format.h`; no ABI change).

### What the game does (USA rev 1)

| Address | What |
|---|---|
| player manager, USA 0x805D26C0 (PAL 0x805D5340, Japan 0x805D6B40) | +0x34 + 4i: save slot of player number i (`fn_80100454`; 7 = none) |
| `fn_801017B8(i)` | player data ("pd") of player number i: slots 0..3 are `save + 0x20 + slot * 0x86C0` (`fn_8013B33C`), others (visitors) live elsewhere |
| pd + 0x83F8 | **tan, 0..15**; the tone drawn is tan >> 1 (0..7). = save + 0x8418 for slot 0 (the ACSE offset) |
| pd + 0x83EC | face 0..7 (pd + 0x83EA hair style, 0x83F6/0x83F7 colours, 0x7EEC gender) |
| pd + 0x7EC2 / + 0x7ED8 | town id (u16) + town name (8 UTF-16), player id (u16) + player name: the author record patterns copy |
| d_a_playerNP `fn_80_2080(player)` | skin palette index = tan >> 1 (0 outside 0..7); palette files `Plyr/Pltt/Palette.arc` 0..7.brplt "skin" (16 RGB5A3 colours; entry 6 the main skin colour, FF34 lightest .. E1C6 darkest) |
| d_a_playerNP `fn_80_20D0(player)` | face palette = 8 + face * 8 + skin index (8..71: every face has its own eight tones) |
| player actor +0x221C / +0x2218 | the two indices, set once when the actor is built; `fn_80_29E0C` (model palette setup: `fn_80_29558` release, `fn_80_29330` + `fn_80_28BDC` bind via `fn_800B9A04` -> `fn_800B9718` = `"%d.brplt"` from the palette archive) runs once when the model is set up. A tan change in the data therefore shows at the next scene change (verified: a poked tan showed only after the new-day scene) |
| `fn_80137D34(pd, delta)` | the daily tan update at the new day: moves pd + 0x83F8 by an amount from `delta` and the "in the sun" flag 0x50 (`fn_801386A4` test, `fn_80138714` clear), clamped 0..15 (October, after a clock jump: 0x0E -> 0x0D) |

### What the port does

- F1 > Enhancements > Gameplay > Skin tone shows the player being played (player number 0:
  name, town, the game's tan level) with "Game (tanning)" and eight swatches (1 = lightest ..
  8 = darkest), coloured from the game's own palette files (entry 6 of 0..7.brplt, read through
  `fn_800B9718` in game).
- A fixed tone replaces `fn_80_2080`'s result for that resident's actor (skin and face palettes
  both follow, since `fn_80_20D0` calls it), and the tan update leaves that resident's saved tan
  unchanged (called as usual, then the byte is put back), so tanning is off while the tone is
  fixed and the game's own tan returns with "Game". Nothing is written to the save.
- The choice is per resident, stored in settings.ini `[skin_tone]` as `<town id>-<player id>`
  (both hex, from the player data) `= 1..8`; absent = game. Keyed by the ids instead of the save
  slot, so it follows the resident (another slot, a copied save), and other residents of the same
  town have their own entry (or none). Visitors (player data outside slots 0..3) are never
  changed.
- A change in the menu applies at once: the current player's actor is rebound with the game's
  `fn_80_29E0C` after its +0x221C / +0x2218 are set from `fn_80_2080` / `fn_80_20D0` (only if the
  actor is still the one the hook last saw: same player number and index). Calling only
  `fn_80_28BDC` a second time crashed (a null virtual call: the previous bindings must be released
  first, which `fn_80_29E0C` does).

### Verification

- `CF_SKIN_TONE_AT=17300:8,17700:4,18100:1,18500:0` (sets the choice as the menu does) on the
  town route: the player's face and hands change at once to the darkest, a middle and the lightest
  tone, then back to the game's (screenshots in the report; log `[skin] rebound player palettes:
  skin 7 face 15`, `skin 3 face 11`, `skin 0 face 8`).
- Tanning: without a fixed tone a poked tan of 0x0E became 0x0D at the next new day (and the
  darker face showed after the new-day scene rebuilt the model); with tone 8 fixed the update ran
  and the byte was kept (`tan update ... (fixed tone: kept)`), the actor rebuilt after the scene
  still got palette 7.
- F1 > Enhancements > Gameplay in the town: both options, the resident's name and town, the eight
  swatches in the game's colours.
- Not tested: a second resident. The scripted route only reaches a first, unsaved resident (the
  game's first save is not reached yet), so the "other residents unaffected" rule rests on the
  code: the hook compares each actor's own player data ids with the stored keys, the tan update
  receives the player data pointer of the resident it updates, and only keys that were set are
  fixed. Visitors from other towns (player numbers whose slot is not 0..3) are skipped.
- PAL / Japan: addresses taken from the version maps (all hash matches), both versions recompiled
  with the hooks (`cfrecompile` accepted every hook name and emitted the REL original); not run.

## Test aids

- `CF_GRASS_LOG=1`: every player step wear (tile, amount, speed, decision, health before ->
  after) and every regrowth (sum of all tiles and tiles under 64, before -> after).
- `CF_GRASS_WEAR_X=<n>`: player steps wear n times as much (shows a path in one run).
- `CF_GRASS_REGROW_AT=<frame>`: presses "Regrow all grass now" at that guest frame.
- `CF_GRASS_MAP=<dir>,<frame>,...`: the 80 x 80 town tiles' health as `<dir>/grass_<frame>.pgm`.
- `CF_SKIN_LOG=1`: every skin palette index the player actor asks for (actor, player data, key,
  tan, result), every tan update (before -> after), the swatch palettes.
- `CF_SKIN_TONE_AT=<frame>:<tone 0-8>,...`: sets the current resident's choice at that frame as
  the menu does (0 = game).
- `CF_FF=<from>-<to>[,...][:<speed>]` (src/game/cf_devaids.cpp): fast-forward during those guest
  frames (retraces/alarms <speed> x, default 16; one frame in 12 shown). The scripted route to the
  town (16600 frames, 4.6 min at 60 fps) takes about 1.5-2.5 min, depending on the PC's load.
- `CF_MEM_DUMP=<dir>,<frame>,...`: MEM1 + MEM2 written to `<dir>/mem_<frame>.bin` at those frames
  (offline diffs of game state).
- `CF_CLOCK_JUMP=<frame>:<seconds>,...`: moves the console clock forward at that guest frame (a
  day change while playing).
- `CF_POKE=<frame>:<hex addr>:<hex byte>,...`: writes a guest byte at that frame.
- `CF_WATCH_FROM=<frame>`: the `CF_WATCH` store report starts at that frame; `CF_FRAME_LOG=1`
  logs every 1000th guest frame (to place other log lines in time).
