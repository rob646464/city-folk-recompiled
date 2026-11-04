# Warps: straight into any place

Branch `warps` (src/game/cf_warp.cpp). For tests (agents, routes) and the user: load the town at
any building, any room, the city and its shops in seconds instead of walking or scripting long
routes. A warp is the game's own stage change, made with the arguments a door would pass, so the
game loads the place, its actors, music and save state as it always does. Nothing is built by hand.

## Interfaces

**Developer builds only.** Warps are a test tool, never shipped: everything below is compiled in
only with `CF_DEV_TOOLS` (the `cfrecomp` developer build). The shipped `CityFolkRecomp` has no
F1 > Enhancements > Debug page, ignores `CF_WARP` / `CF_WARP_SWEEP` and the route token `WARP=`
(user decision 2026-11-04). Agents test warps with `cfrecomp`.

- **F1 > Enhancements > Debug > Warp**: every destination below, grouped and searchable, a `Warp`
  button each (labels `Warp##<name>` for `CF_MENU_CLICK`), the current stage, the last result.
  "Keep the current time and weather" (on): the warp is like walking through a door. Off: the
  clock is first moved forward to the chosen hour (same mechanism as `CF_CLOCK_JUMP`); the weather
  is then the game's own for that hour (e.g. Crazy Redd's or the observatory at night).
- **`CF_WARP=<target>[,<frame>]`**: one warp per run. Without a frame it fires once the game has
  been in a playable stage for `CF_WARP_SETTLE` frames (default 1500 = past the continue route
  and Resetti's greeting in the user-town fixture). Needs a route that reaches the town
  (`--input @tools/routes/continue_any.txt`).
- **Route token `WARP=<target>`** (with `/` for `,`, e.g. `WARP=town@1880/28/2000`): fires at its
  item's time, so it can follow an `after link` anchor; it waits until the game can change stage
  (a playable stage, no change running, 90 frames after the last arrival). `tools/routes/warp.txt`
  = continue + an anchor at the arrival; the `*_warp.txt` routes include it.
- `<target>` = a name from the table, or `stage:<number | NAME>` (`stage:0x25`, `stage:RM_MM_CAFE`;
  rooms are entered as below, the town and the city need a position), either followed by
  `@x,y,z` (arrival position, world units of that stage) and optionally `#<type>` (arrival type
  in hex instead of the default, for experiments: `stage:45@752,56,640#8`).
- `CF_TELEPORT` / route `TP=` (move the player within the current stage) are unchanged.
- Test aids: `CF_WARP_LOG=1` (every stage change the game or a warp makes with its arguments,
  every arrival with the saved door data), `CF_WARP_DUMP=<frame>` (the stage table, every exit
  table, every building's door spot in this town), `CF_WARP_SWEEP=<shot dir>[,<dwell>[,<via>[,<filter>]]]`
  (every destination in turn: warp, first to `<via>` when given, wait for the arrival + `<dwell>`
  frames (400), write `<dir>/<name>.ppm`, log `[warp] sweep <name>: ok|LEFT AGAIN|FAILED|TIMEOUT`;
  `LEFT AGAIN` = the game had moved the player elsewhere by the screenshot; `<filter>` =
  substrings of names with `|` between them; demo scenes only when the filter names them). A
  sweep of all ~90 places takes about an hour on a loaded PC; `CF_FF` makes the loads finish
  after the dwell, so sweep at normal speed.

## How the game changes places (USA addresses; VA() maps them on PAL/JP/KR)

- **Stages**: one table of 68 at 0x804F0C98 (8 bytes: name, stage info). The names are the
  game's: FIELD (the town, 0), RM_HS0..3_F1/F2/BM/LOFT (player houses 1-4: main room, upstairs,
  basement, the upper floor's back room), RM_NPC0..9 (villager houses by slot), RM_OFFICE (town
  hall), RM_CHKP / RM_CHKP_NET (gatehouse, its online variant), RM_MM_ENT / FISH / INS / FOSSIL0-1 /
  PIC0-1 / CAFE / ASTRO (museum), RM_SHOP0-3_F1/F2 (Nook's four shops), RM_TAILOR (Able Sisters),
  TOWN (the city, 0x2D), RM_BROKER (Crazy Redd's), RM_BARBER (Shampoodle), RM_FORTUNE (Katrina),
  RM_GRACE (GracieGrace), RM_HAPPY_ENT / MDL (HRA office, model room), RM_THEATER_ENT / SHOW
  (Club LOL), RM_AUCTION (Lyle's auction house), RM_RESET (Resetti's surveillance center), DM_*
  demo scenes (title, save, load, player select, the buses, the gate's online connection), and two
  test stages (CHECK_FIELD, RM_SAMPLE). Per-stage tables beside it: 0x80479E60 flags (0x10 room,
  0x20 demo, 0x400 player house, 0x800 villager house, 0x2000 museum, 0x4000 bus), 0x80479F70 the
  structure id of a room's building (fn_801628F0). Current stage: byte r13-0x6384 (fn_8016266C),
  previous r13-0x6383.
- **Exit tables**: stage info +8 = {entries, count}, 0x1C bytes each: u8 stage[4] (one per
  variant: which player house, fn_801627CC), pos, s16 angle (+0x10), arrival type (+0x14).
  Codes above 0x43 lead outside: 0x44 the door of the room's own building, 0x45 the building
  entered last, 0x46-0x49 player house 1-4, 0x4A the gate, 0x4B the door data saved when the town
  was left (0x805FB248). All static data in main.dol, so every room's way in is known up front.
- **Stage manager** (static object 0x805FB218, fn_80161DE8): `fn_80163064(mgr, stage, pos*,
  type, angle, fadeOut, fadeIn)` = "go to <stage>, arrive at <pos> facing <angle>"; it requests
  the change through `fn_80162EC8` (mgr+0 next stage, fades, r13-0x6510 = 1, mgr+0x1F = 1) and
  stores the arrival (mgr+4 pos, +0x10 angle, +0x14 type). Callers: a building's door in the town
  and the city (d_a_strNP fn_99_41F4: the building's room, pos 256,0,464, type 0x4A, angle 0x8000,
  fades 2/2), a room's exits (fn_801635D0 -> fn_801634A4 -> the exit table), the continue from the
  title (FIELD at the player's house, type 0xB).
- `fn_801630FC(structure id, &stage, &pos, &type, &angle, ...)` = the spot outside a building's
  door in the town or city (what a room's "outside" exit uses; type 0xB). Structure ids: D001-D004
  player houses, D009-D012 villager houses 1-10, D013 town hall, D014 gate, D015 Nook's, D016 Able
  Sisters, D017 museum, D022 Shampoodle, D023 HRA, D024 Crazy Redd's, D025 Katrina, D026
  GracieGrace, D027 auction house, D028 Club LOL (CF_WARP_DUMP lists the town's).
- **Kapp'n's bus**: the bus stop sign starts the ride with `fn_80162C40(DM_BUS_TO_TOWN, 4, 0)`;
  at the end of the ride Kapp'n (d_a_npc_sp_kappeiNP fn_54_824) calls `fn_80169950(fn_80169AF0(),
  0 = city / 1 = town, 0, 0, 0)`, which puts the player at that end's bus stop (structure D042 /
  D041) with the bus arrival (type 0xAA in the city). Before the ride home he sets player flag 0x26
  (fn_801386E4).
- `fn_80162C40(stage, fadeOut, fadeIn)` = a stage change without an arrival spot (demo scenes).

### What a warp calls

| Kind | Call | Arguments |
| --- | --- | --- |
| Room | fn_80163064 | rooms with an exit outside (0x44-0x49): their building's front door, as d_a_strNP (256,0,464, type 0x4A, angle 0x8000); other rooms (upstairs, basement, museum wings, cafe, observatory, HRA model room, Club LOL stage, Nookington's upper floor, gatehouse online, Resetti's room): the exit entry of the room that leads into it |
| Door (town) | fn_801630FC + fn_80163064 | the town outside the building's door (type 0xB), as a room's exit |
| Door (city) | fn_80163064, then fn_801635D0 | two steps: into that building's room (fn_801629BC: structure id -> stage), then out through the room's own exit to the outside (code 0x44), as when leaving the shop |
| Position | fn_80163064 | the town at a position (type 0xB, angle 0), optionally relative to a building's door; the city at a position with type 0x8 (as Resetti's room's exit to the streets) |
| Bus | fn_80169950 | stepping off Kapp'n's bus in the city or the town (no ride) |
| Demo | fn_80162C40 | the bus rides (fades 4/0 as the bus stop sign), the title (2/2) |

Fades are the doors' 2/2. A warp waits until the current stage is playable (not a demo scene or
test stage), no change is pending (mgr+0x1F), the previous warp has arrived and 90 frames have
passed since the last arrival.

**The city and arrival type 0xB**: type 0xB means "out of a door". In the city the player then
waits, hidden, for the door of the building they were in; out of any other door, or with no
building (from the town, from the streets), nothing lets them out and after 292 frames
d_a_playerNP (state fn_80_1A5F4, counter +0x22B2, then fn_80_1B304) sends them home (FIELD at
their house, type 0x75). The town has no such wait. Hence the two-step city door warps and type
0x8 for city positions (found by the sweep's `LEFT AGAIN` check, which compares the stage after
the dwell with the one warped to).

### Preconditions

- **The city**: nothing the game checks for the stage itself. Kapp'n's first-trip talk (and
  whatever flag it sets) is skipped by `city`; a later real bus ride gives the talk again. The
  warp sets no flag (player flag 0x26, set before the real ride home, is left alone).
- **Rooms a town does not have yet** (upstairs/basement of a small house, Nook's later shops,
  more villager houses than the town has): the game loads the room anyway (its data is on the
  disc); what it shows there follows the save (see the results). Use them for rendering tests
  only; the save is not changed by being there, but saving the game in such a room is untested.
- **Crazy Redd's**: walking in needs a membership (the doorman's check at the city door); the
  warp enters the room directly, where Redd greets a non-member with his membership offer
  (3,000 Bells: answer it as in the game). `city.redd_door` goes through the room the same way.
- Visitors (Wendell, Saharah, Gracie's car, ...) are characters in the town on their days, not
  places: no warp; `TP=mod<N>` moves the player to one that is there.

## Routes

`tools/routes/warp.txt` = the continue (continue_any.txt) + an `after link d_bgo_drawNP 2` anchor
at the arrival in the town; include it and add `+N:WARP=...` items. WARP versions of the slow
routes (the walking routes stay, as gameplay tests):

| Route | Walking version | What it does |
| --- | --- | --- |
| museum_warp.txt | museum.txt | lobby, a painting room, The Roost, the observatory, each by WARP |
| town_hall_warp.txt | town_hall.txt | the town hall, around the room |
| beach_warp.txt | beach.txt | `town.beach`, down onto the sand, along the shore and back |
| river_warp.txt | river.txt | `town@1880/28/2000` below the waterfall, down the bank and back |
| city_warp.txt | city.txt (via city_bus.txt) | `city` (off the bus at the city's stop), the plaza and streets |
| city_shops_warp.txt | city_shops.txt | GracieGrace, HRA, Club LOL hall and stage, room to room |
| city_shops2_warp.txt | city_shops2.txt | Shampoodle, Katrina's, Crazy Redd's (inside), Lyle's auction house |
| warp_scenes.txt | - | the bus rides (WARP=bus.to_city / bus.to_town, A through Kapp'n's talk) and the title |

They start the place ~1600 frames after the town arrival instead of after a walk or a 3.5 min
bus ride; all eight were run through on 2026-11-03 (screenshots looked at). `run.ps1 -Env "CF_FF=1-3600"` fast-forwards the continue as well (title to town in
seconds; the arrival anchor still works). Several `CF_MENU_CLICK` actions contain `;`, which
run.ps1's `-Env` splits on: set that variable in the shell before calling run.ps1.

## Destinations and results

The table is the warp list in the order of the F1 page (names for CF_WARP / WARP=). "Stage" =
what the game loaded; "from town" / "from interior" = the sweep result starting in front of player
1's house / in the town hall (see Verification); "refused" = the warp declines with a message (the
town has no such villager), nothing is loaded. Villager houses are by slot (the order of the
town's villager structures D009-D012); the labels do not show the villager's name yet.

| Name | Place | Stage | From town | From interior | Seen in user-town |
| --- | --- | --- | --- | --- | --- |
| **Town** | | | | | |
| `town` | Your house (player 1's house door) | FIELD | ok | ok | outside player 1's house (the continue's arrival spot) |
| `town.house1` | Player house 1 door | FIELD | ok | ok | player 1 (rob) |
| `town.house2` | Player house 2 door | FIELD | ok | ok | unowned house plot 2 (house built, nobody lives there) |
| `town.house3` | Player house 3 door | FIELD | ok | ok | unowned plot 3 |
| `town.house4` | Player house 4 door | FIELD | ok | ok | unowned plot 4 |
| `town.town_hall` | Town hall door | FIELD | ok | ok |  |
| `town.plaza` | Plaza (in front of the town hall) | FIELD | ok | ok | the stone plaza in front of the town hall |
| `town.museum` | Museum door | FIELD | ok | ok |  |
| `town.nook` | Nook's shop door | FIELD | ok | ok | Nook's Cranny in this town |
| `town.able` | Able Sisters door | FIELD | ok | ok |  |
| `town.gate` | Gate (Copper's gatehouse door) | FIELD | ok | ok |  |
| `town.bus_stop` | Bus stop (off Kapp'n's bus from the city) | FIELD | ok | ok | off the bus, the bus at the stop |
| `town.beach` | Beach (south shore) | FIELD | ok | ok | lower terrace above the sand |
| `town.river_mouth` | River mouth (where the river meets the sea) | FIELD | ok | ok | acre column 3 (model 083, the bridge over the mouth), 1792,28,2800 |
| `town.villager1` | Villager house 1 door | FIELD | ok | ok |  |
| `town.villager2` | Villager house 2 door | FIELD | ok | ok |  |
| `town.villager3` | Villager house 3 door | FIELD | ok | ok |  |
| `town.villager4` | Villager house 4 door | FIELD | ok | ok |  |
| `town.villager5` | Villager house 5 door | FIELD | ok | ok |  |
| `town.villager6` | Villager house 6 door | FIELD | ok | ok |  |
| `town.villager7` | Villager house 7 door |  | refused | refused | refused: the town has 6 villagers |
| `town.villager8` | Villager house 8 door |  | refused | refused | refused |
| `town.villager9` | Villager house 9 door |  | refused | refused | refused |
| `town.villager10` | Villager house 10 door |  | refused | refused | refused |
| **Player houses** | | | | | |
| `house1` | House 1: main room | RM_HS0_F1 | ok | ok | main room |
| `house1.upstairs` | House 1: upstairs | RM_HS0_F2 | ok | ok | not built in user-town: empty room |
| `house1.basement` | House 1: basement | RM_HS0_BM | ok | ok | not built: empty room |
| `house1.loft` | House 1: back room / loft | RM_HS0_LOFT | ok | ok | the small back room with the bed |
| `house2` | House 2: main room | RM_HS1_F1 | ok | ok | unowned: an empty main room |
| `house2.upstairs` | House 2: upstairs | RM_HS1_F2 | ok | ok | empty room |
| `house2.basement` | House 2: basement | RM_HS1_BM | ok | ok | empty room |
| `house2.loft` | House 2: back room / loft | RM_HS1_LOFT | ok | ok | bed room |
| `house3` | House 3: main room | RM_HS2_F1 | ok | ok | unowned |
| `house3.upstairs` | House 3: upstairs | RM_HS2_F2 | ok | ok | empty room |
| `house3.basement` | House 3: basement | RM_HS2_BM | ok | ok | empty room |
| `house3.loft` | House 3: back room / loft | RM_HS2_LOFT | ok | ok | bed room |
| `house4` | House 4: main room | RM_HS3_F1 | ok | ok | unowned |
| `house4.upstairs` | House 4: upstairs | RM_HS3_F2 | ok | ok | empty room |
| `house4.basement` | House 4: basement | RM_HS3_BM | ok | ok | empty room |
| `house4.loft` | House 4: back room / loft | RM_HS3_LOFT | ok | ok | bed room |
| **Villager houses** | | | | | |
| `villager1` | Villager house 1 | RM_NPC0 | ok | ok | villager inside |
| `villager2` | Villager house 2 | RM_NPC1 | ok | ok |  |
| `villager3` | Villager house 3 | RM_NPC2 | ok | ok |  |
| `villager4` | Villager house 4 | RM_NPC3 | ok | ok | villager inside |
| `villager5` | Villager house 5 | RM_NPC4 | ok | ok | villager inside |
| `villager6` | Villager house 6 | RM_NPC5 | ok | ok | villager inside |
| `villager7` | Villager house 7 |  | refused | refused | refused (empty slot; before the check the game showed a placeholder room) |
| `villager8` | Villager house 8 |  | refused | refused | refused |
| `villager9` | Villager house 9 |  | refused | refused | refused |
| `villager10` | Villager house 10 |  | refused | refused | refused |
| **Town buildings** | | | | | |
| `town_hall` | Town hall (Pelly / Phyllis, Tortimer) | RM_OFFICE | ok | ok | Pelly at the counter |
| `gate` | Gatehouse (Copper, Booker) | RM_CHKP | ok | ok | Copper's gatehouse |
| `gate.online` | Gatehouse, gate open (online) | RM_CHKP | ok (gives RM_CHKP) | ok (gives RM_CHKP) | the game itself loads RM_CHKP instead while not online (fn_80162E58) |
| `nook` | Nook's Cranny | RM_SHOP0 | ok | ok | Tom Nook's greeting |
| `nook.n_go` | Nook 'n' Go | RM_SHOP1 | ok | ok | Nook 'n' Go room with Tom Nook (the town has only the Cranny) |
| `nook.nookway` | Nookway | RM_SHOP2 | ok | ok | Nookway room with Tom Nook |
| `nook.nookington` | Nookington's ground floor | RM_SHOP3_F1 | ok | ok | Nookington's ground floor with Tom Nook |
| `nook.nookington2` | Nookington's upper floor | RM_SHOP3_F2 | ok | ok | Nookington's upper floor (dark, nobody there) |
| `able` | Able Sisters | RM_TAILOR | ok | ok |  |
| **Museum** | | | | | |
| `museum` | Museum lobby (Blathers) | RM_MM_ENT | ok | ok | Blathers |
| `museum.fish` | Museum: fish (aquarium) | RM_MM_FISH | ok | ok | aquarium (loads slowly: ~1 s of loading icon) |
| `museum.insects` | Museum: insects | RM_MM_INS | ok | ok | insect room (loads slowly) |
| `museum.fossils` | Museum: fossils, first room | RM_MM_FOSSIL0 | ok | ok |  |
| `museum.fossils2` | Museum: fossils, second room | RM_MM_FOSSIL1 | ok | ok |  |
| `museum.art` | Museum: paintings, first room | RM_MM_PIC0 | ok | ok |  |
| `museum.art2` | Museum: paintings, second room | RM_MM_PIC1 | ok | ok |  |
| `museum.cafe` | Museum: The Roost (Brewster) | RM_MM_CAFE | ok | ok | Brewster |
| `museum.observatory` | Museum: observatory (Celeste) | RM_MM_ASTRO | ok | ok | Celeste |
| **City** | | | | | |
| `city` | City bus stop (off Kapp'n's bus) | TOWN | ok | ok | off the bus at the city's stop |
| `city.plaza` | City plaza (in front of the fountain) | TOWN | ok | ok | in front of the fountain, arrival type 8 |
| `city.gracie_door` | City: GracieGrace door | TOWN | ok | ok | into the building, out through its exit |
| `city.shampoodle_door` | City: Shampoodle door | TOWN | ok | ok | into the building, out through its exit |
| `city.katrina_door` | City: Katrina's door | TOWN | ok | ok | into the building, out through its exit |
| `city.redd_door` | City: Crazy Redd's door | TOWN | ok | ok | into the building, out through its exit |
| `city.hra_door` | City: Happy Room Academy door | TOWN | ok | ok | into the building, out through its exit |
| `city.auction_door` | City: Lyle's auction house door | TOWN | ok | ok | into the building, out through its exit |
| `city.theatre_door` | City: Club LOL door | TOWN | ok | ok | into the building, out through its exit |
| **City rooms** | | | | | |
| `city.gracie` | GracieGrace | RM_GRACE | ok | ok | Labelle's greeting |
| `city.shampoodle` | Shampoodle (Harriet) | RM_BARBER | ok | ok | Harriet's greeting |
| `city.katrina` | Katrina's fortune shop | RM_FORTUNE | ok | ok | Katrina at her table |
| `city.redd` | Crazy Redd's | RM_BROKER | ok | ok | inside, Redd offers the membership (the doorman at the door is skipped) |
| `city.hra` | Happy Room Academy (Lyle) | RM_HAPPY_ENT | ok | ok | Lyle's HRA office |
| `city.hra_model` | Happy Room Academy model room | RM_HAPPY_MDL | ok | ok | the model room |
| `city.auction` | Lyle's auction house | RM_AUCTION | ok | ok | Lloid's welcome |
| `city.theatre` | Club LOL (ticket hall) | RM_THEATER_ENT | ok | ok | ticket hall |
| `city.theatre_show` | Club LOL stage (Dr. Shrunk) | RM_THEATER_SHOW | ok | ok | the stage hall |
| `city.resetti` | Reset Surveillance Center | RM_RESET | ok | ok | Resetti's surveillance center |
| **Scenes** | | | | | |
| `bus.to_city` | Kapp'n's bus to the city | DM_BUS_TO_TOWN | ok (route) | - | warp_scenes.txt: the ride with Kapp'n's first-trip talk (A), off in the city |
| `bus.to_town` | Kapp'n's bus back to the town | DM_BUS_TO_LAND | - | ok (route, from the city) | warp_scenes.txt (from the city): the ride home, off at the town's bus stop |
| `title` | Title screen | DM_TITLE | ok (route) | - | warp_scenes.txt: the title, then Rover's player select |


## Verification (2026-11-03, USA, user-town fixture "Frunch", 10:00 Oct 27)

- Every destination by `CF_WARP_SWEEP` twice: **from the town** (`via` = `town`: each warp starts
  in front of player 1's house) and **from an interior** (`via` = `town_hall`), screenshot
  `<dwell>` frames after the arrival (500-600; one first pass at 8x fast-forward, whose
  screenshots of the aquarium and the insect room still showed the loading icon, redone at
  normal speed). `ok` = the stage the game was asked for (after its own mapping) loaded and the
  player was still there at the screenshot; every screenshot looked at. The log had no errors
  besides the known once-per-run Aurora BP/XF register notes.
- The city: the first passes found the player hidden and sent home (to the house, type 0x75)
  292 frames after a warp to the city streets with type 0xB, from the town, from the streets
  and out of GracieGrace to another door (`LEFT AGAIN`; see "The city and arrival type 0xB");
  city door spots now go through their own building and its exit, city positions use type 0x8,
  both checked from the town and from the town hall (the last rows' results).
- Menu: `CF_MENU_CLICK` opened F1 > Enhancements > Debug and pressed `Warp##museum.observatory`
  in the town: Celeste's observatory loaded. A second run unticked "Keep the current time and
  weather" (hour 22) and pressed `Warp##city.plaza` at 10:01: `clock moved +43108 s to 22:00`,
  the city plaza at night, the player still there 2000 frames later.
- Scenes: `warp_scenes.txt`: `bus.to_city` -> Kapp'n's ride with his first-trip talk (A) -> the
  city's bus stop; `bus.to_town` from the city -> the ride home -> the town's bus stop;
  `title` -> the title, then Rover's player select as after a reset.
- Save: a run warped town -> The Roost -> GracieGrace -> villager house 2 -> the museum's door,
  then saved with the game's own save request (`CF_FORCE_SAVE`, result 1 = written); a second run
  on that NAND (no fixture) continued normally: "Preparing saved data", Rover, the town at the
  house (save2 run).
- Routes: museum_warp.txt (lobby, painting room, cafe, observatory), warp_scenes.txt,
  city_shops_warp.txt, city_shops2_warp.txt run through (see the table above for the places).
- Not done: PAL / Japan / Korea (hooks are `all`, addresses in version_addrs.inc; not run), a
  town with more villagers or a bigger house / later Nook's (the rooms load anyway, see the table),
  saving while inside a room the town does not have.
