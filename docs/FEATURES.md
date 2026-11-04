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

## Move in from Wild World

Rover's "Move from the DS!" with a Wild World save file instead of a DS: a virtual DS behind the
DS wireless libraries feeds the game's own receive path and import. F1 > Enhancements > Gameplay
"Move in from Wild World". Everything (protocol, data, region rules, tests): docs/MOVE_IN.md.

## Unused items at Nook's

F1 > Enhancements > Unused content > "Unused items at Nook's" (off; settings.ini
`[unused_content] nook_item=<item code>`, 0000 = none; src/game/cf_unused_items.cpp). One of the
game's unused items is sold in place of one of the day's items of the same kind; the shop, its
prices, buying and the catalog are the game's own. One item at a time (ticking one unticks the
other); unticking brings back what was for sale.

### What the game does (USA rev 1; PAL/JP/KR: the same functions, hash matches "identical apart from addresses")

- **Shop stock**: save block (fn_8010DD60, r13-0x2FE0) + 0x630C0 = S. S+0 shop level (0 Nook's
  Cranny .. 3 Nookington's), S+8: 36 slots of 4 bytes (u16 item code, 0xFFF1 = none; u8 sold flag,
  fn_80146524; u8 unused). fn_801456B4(S) fills the day's stock; called by the daily update
  fn_8014406C (when the date at save + 0x631FC is not today; from fn_800939E8 at the session start
  and fn_80155074) and by d_shop_fgNP fn_119_A8 when the shop's level changed, right before the
  shop room places the stock on its display points (fn_119_360: fn_8015F994(kind) finds a free
  point on the room's 16 x 16 tile grid, fn_119_2F0 puts the item there).
- **Slot groups**: 0-16 clothes, tools, plants and the rest; 17-24 furniture (count per level
  fn_801460E8: 2/3/5/8); 25-27 wallpaper (fn_80146118: 1/1/2/3); 28-30 flooring (fn_80146128:
  1/1/2/3); 31-34 stationery (fn_80146138: 1/2/3/4); 35 one more. The day's special furniture
  (seasonal item, event item: categories 0x29/0x2A/0x2B/0x52/0x53) is written into furniture
  positions 0 and 1 (levels 0-2) or 2 and 7 (level 3).
- **Item codes**: 0x9000 + item number x 4 (+ sheets - 1 for stationery). Table in memory: manager
  fn_800C24CC(), records at [mgr + 0x78] + number x 0x18C (fn_800C2704) = the first 0x18C bytes of
  each Item/item.arc item.bin record: +4 price, +8 number, +0x10 0x1701, +0x12 ten 0x22-byte UTF-16
  names (JP, US en/es/fr, EU en/de/it/es/fr, KR), +0x166 kind (1 wallpaper, 2 flooring, 3
  furniture, 9 stationery), +0x16A source (3/4/5 Nook's pools, 0x21 Saharah, 0x2C unused).
- **Models**: Item/Ftr/Ftr<n>.brres, n = the furniture's ordinal (numbers 0x9C4.. -> 0..717, then
  0xE07.. -> 718..): the moving boxes #E08-E0A are Ftr719-721 (`int_itd_mov_box_L/M/S`); walls and
  floors Item/Wall/Wall<n>, Item/Carpet/Carpet<n> (cranny wall Wall87 .. department floor
  Carpet88): every item has its model / texture on the disc.
- **Save writes**: the starters fn_800D256C, fn_800D25D8, fn_800D2648, fn_800D26B0, fn_800D2720,
  fn_800D2780 (HUD save / "Save and quit"), fn_800D27E0, fn_800D2830 compute what they need
  (checksums over the block: fn_8010E1CC, fn_80115D0C, fn_80115F00) and start the save worker
  (fn_800D284C, thread 0x805A3000), which writes the block from memory and leaves its result in
  r13-0x6930 (0 while it runs, then 1 done or an error).

### The items

Every code checked in item.bin of all four discs (same number, kind, price; only the Korean name
slot differs on the Korean disc). Names are the game's (US English).

| Code | Item no. | Name in the game | Category | Game's price | Price sold at | Why |
|---|---|---|---|---|---|---|
| C0AC | C2B | Nintendo bench | furniture | 120 | 2400 | the user's price (the game's is far too cheap), set over the game's |
| C178 | C5E | pikmin | furniture | 1024 | 2400 | the user's price, set over the game's |
| C17C | C5F | Blue Falcon | furniture | 1024 | 2400 | the user's price, set over the game's |
| C23C | C8F | brown pot | furniture | 800 | 800 | the game's own |
| C240 | C90 | white pot | furniture | 1200 | 1200 | the game's own |
| C244 | C91 | moth orchid | furniture | 2480 | 2480 | the game's own |
| C820 | E08 | "not used" (JP: moving cardboard L) | furniture (5 moving boxes) | 0 | 500 | 100 per box = the cardboard box's price (#C42) |
| C824 | E09 | "not used" (JP: moving cardboard M) | furniture (4 moving boxes) | 0 | 400 | same |
| C828 | E0A | "not used" (JP: moving cardboard S) | furniture (2 moving boxes) | 0 | 200 | same |
| A0FC | 43F | cranny wall | wallpaper | 0 | 1440 | median of Nook's 54 regular wallpapers (sources 3/4/5, 800-2660: 1435), rounded to 10 like every game price |
| A100 | 440 | convenient wall | wallpaper | 0 | 1440 | same |
| A104 | 441 | supermarket wall | wallpaper | 0 | 1440 | same |
| A108 | 442 | department wall | wallpaper | 0 | 1440 | same |
| A414 | 505 | cranny floor | flooring | 0 | 1540 | median of Nook's 51 regular floors (720-3200) |
| A418 | 506 | convenient floor | flooring | 0 | 1540 | same |
| A41C | 507 | market floor | flooring | 0 | 1540 | same |
| A420 | 508 | department floor | flooring | 0 | 1540 | same |
| 9670-9673 | 19C | weathered paper (1-4 sheets) | stationery | 160 | 160 | the game's own = all 53 of Nook's regular papers |
| 974C-974F | 1D3 | bulletin paper (1-4 sheets) | stationery | 160 | 160 | same |

The moving boxes are "not used" in the English table and a blank placeholder in the other
European slots; while an item is chosen the port names them "5 / 4 / 2 moving boxes" (also the
bulletin paper's blank Spanish/French/German/Italian slots), in memory only. Stationery is sold
with the sheet count of the stationery it replaces.

### What the port does

- **Where**: the chosen item goes into one slot of its category (moving boxes: furniture), the
  same one every time for a day's stock: the last position that is not a special-furniture
  position, sold or not, so reloading or saving gives the same slot and an item bought once stays
  bought (if the slot's own item was bought before ticking, the replacement shows sold out).
  Stationery keeps the slot's sheet count. The original code is kept to put back.
- **The save never holds it**: each save starter puts the original back before its checksum
  (sold flag kept); the replacement returns once the worker's result word is set. So every save
  file has the day's own stock whether the option is on or off, and turning it off (with the game
  running or not) leaves nothing behind. A reload with the option on gives the same slot again.
- **New day**: the hook on fn_801456B4 places the item in the new stock at once (also when the
  shop is rebuilt for a new level, before its shelves are filled).
- **Live changes**: ticking / unticking changes the stock at the next frame; inside the shop
  (d_shop_fgNP linked) it waits until the player leaves, so the shelves and the stock agree.
  While in an online room the stock stays retail (visitors receive the town data).
- **Prices**: walls, floors and moving boxes have price 0 in the game's table, and the user set
  the Nintendo bench, pikmin and Blue Falcon to 2400; while an item is chosen these table records
  carry the prices above (and the names); with nothing chosen the table is the disc's.
- Default (nothing ticked): no stock, table or save byte is touched.

### Test aids

- `CF_UNUSED_LOG=1`: every stock generated (all 36 slots, `*` = sold), each replacement / restore
  with its reason, the save starters and their end, and once per shop visit the shop room's item
  grid (`(x,z):code`, `<-` = the replacement).
- The setting through `CF_SETTINGS_AT="<frame>:unused_content.nook_item=C0AC"` or a settings.ini
  written before the start (`run.ps1 -Settings "[unused_content]|nook_item=C0AC"`).
