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
- **Regrow all grass now** (button): every tile of the 5 x 5 town acres set to 255 through the
  game's own `fn_800A7C84` (clamped to each tile's maximum, so ground under buildings stays bare),
  into both copies; saved with the town at the next save.

### Verification

Route `tools/routes/grass_walk.txt` (`gen_town.py`): new game to the town gate, A through the map
tutorial, then a square from the bus stop run (B held) and walked in turns. Runs fast-forwarded to
the town with `CF_FF=0-16600`. The new town is the same in every run (its layout and grass
values matched tile for tile).

## Skin tone

(below)

## Test aids

- `CF_GRASS_LOG=1`: every player step wear (tile, amount, speed, decision, health before ->
  after) and every regrowth (sum of all tiles and tiles under 64, before -> after).
- `CF_GRASS_WEAR_X=<n>`: player steps wear n times as much (shows a path in one run).
- `CF_GRASS_REGROW_AT=<frame>`: presses "Regrow all grass now" at that guest frame.
- `CF_FF=<from>-<to>[,...][:<speed>]` (src/game/cf_devaids.cpp): fast-forward during those guest
  frames (retraces/alarms <speed> x, default 16; one frame in 12 shown). The scripted route to the
  town takes about 70 s instead of 280.
- `CF_MEM_DUMP=<dir>,<frame>,...`: MEM1 + MEM2 written to `<dir>/mem_<frame>.bin` at those frames
  (offline diffs of game state).
- `CF_CLOCK_JUMP=<frame>:<seconds>,...`: moves the console clock forward at that guest frame (a
  day change while playing).
- `CF_POKE=<frame>:<hex addr>:<hex byte>,...`: writes a guest byte at that frame.
- `CF_WATCH_FROM=<frame>`: the `CF_WATCH` store report starts at that frame; `CF_FRAME_LOG=1`
  logs every 1000th guest frame (to place other log lines in time).
