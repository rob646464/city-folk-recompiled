# Rendering notes

City Folk's renderer is LM's (Aurora + the port's GX HLE); LM's docs/RENDERING.md describes the
shared parts. This file holds what is specific to City Folk. (docs/WII.md has the Wii-side
rendering status: physical addresses, VI, display lists, interpolation tags.)

## Pipeline precache

Aurora builds one render pipeline per distinct GX state (TEV stages, vertex format, blend/depth
state, render target layout). Building one on D3D12 takes 1 ms warm (from the Dawn cache) but
0.2-3.6 s cold on this PC under load: a draw waits for its pipeline (Aurora 0013, nothing
flickers), so every config first met in play is a visible hitch. As in LM:

- `data/pipelines.bin` (shipped next to the exe, `cmake --install`) lists the configs the game
  uses. At the first start the "Preparing shaders" hold (src/recomp/precache.cpp `holdGame`)
  compiles them, and every variant the current F1 settings derive from them (Aurora 0217:
  accuracy flags, enhanced lighting, early-Z split, MSAA layout), before the game's first frame;
  later starts load them from the cache (`pipeline_cache.db` + `dawn_cache.db` in the cache dir).
  Format and merge tool: `tools/pipelines/pipelines.py` (LM's, unchanged; `build -o <out> <inputs>`
  merges `pipeline_cache.db` files and older lists, `info` prints them; needs Python 3.14 for zstd:
  `py -3.14`).
- `tools/pipelines/verify.py` plays routes with the dev build (`cfrecomp`), each run with a fresh
  cache dir (`CF_CACHE_DIR`), scratch APPDATA/NAND/card, the fixture save, muted, off screen,
  controllers hidden, `CF_PIPELINE_LOG=1`, and reports per run the `MISS (not precached)` count and
  the startup hold (`game starts after <s> s of shader preparation (...; <n> pipelines`).
  `--no-list` (`CF_PRECACHE=0`) records from nothing; `--list <bin>` checks a list;
  `--cache-from <dir>` starts from a copy of a warm cache (performance runs); `--presets`
  (default, enh, fps120, msaa4, perf4k120, perf4k, perf120: settings.ini sections);
  `--file <route> --frames <n>` an ad-hoc route. Recording = run every route with `--no-list`, then
  `py -3.14 tools/pipelines/pipelines.py build -o data/pipelines.bin data/pipelines.bin <work>/*/*/cache/pipeline_cache.db`.
- **Cold runs need an idle PC** (CLAUDE.md "Load limits"): every recording or fresh-cache
  verification run compiles shaders from nothing; with other games running, single pipelines took
  up to 3.6 s and holds stalled. Schedule them when nothing else runs.

### Routes (tools/routes/, `CF_INPUT_CLOCK=frames`)

| run | route | what it shows |
|---|---|---|
| new_game | new_game.txt | boot, strap screen, title (town flyover), Rover's bus, name/town keyboards, town gate (its own deterministic town) |
| town_day | precache_town.txt | saved town (fixture user-town "Frunch") 10:00, walk/run round the house, HUD map |
| town_night | precache_town.txt | the same at 21:30 |
| town_rain | precache_town.txt | the same with `CF_WEATHER_FAKE=rain` |
| town_snow | precache_town.txt | 15 January 11:00, `CF_WEATHER_FAKE=heavysnow` |
| house | precache_house.txt | the player's house: in, round the room, out, the town east |
| nook | precache_nook.txt | west to Nook's Cranny, in (A at the door), Tom Nook, the shop, out |
| museum | to_museum.txt | new_game's town, walk to the museum (Blathers' hall) |
| beach | beach.txt | saved town: the beach and the sea (shore walk east to the river mouth and back) |
| river | river.txt | saved town: the main river below the waterfall, down the bank |
| museum_rooms | museum.txt | saved town: museum lobby (Blathers), right wing (empty exhibit room), Brewster's cafe The Roost, the observatory (Celeste) |
| town_hall | town_hall.txt | saved town: the town hall (Pelly at the counter) |
| city | city.txt | saved town: Kapp'n's bus (stop, bus, ride, first-trip talk), the city's bus stop, fountain plaza, streets |
| city_shops | city_shops.txt | the bus, then GracieGrace (Labelle's ground floor), Lyle's HRA office, Club LOL (ticket counter hall) |
| city_shops2 | city_shops2.txt | the bus, then Shampoodle (Harriet), Katrina's fortune shop, Crazy Redd's door (members only) |

All saved-town routes start with `continue_any.txt` (A with the pointer away, so every Rover
question takes its first answer, whatever the load timing) and time their walk from the arrival
(`after link d_bgo_drawNP 2`); continue.txt's pointer clicks land in another dialogue branch when
"Preparing saved data" takes longer than usual. The routes of branch routes-2 (beach .. city_shops2,
also in tools/pipelines/verify.py, not recorded into pipelines.bin yet) put the player in front of
a place with the `TP=` route action (docs/CONTROLS.md "Test aids"), so they do not depend on a walk
across the town. Not covered yet: Able Sisters, Nook's later shops (the fixture has the first
one), villager houses, festivals, the auction house / theatre shows / HRA upstairs (barricaded in
this town), Crazy Redd's shop (membership), Phineas (not in the city on the route's day).

### Places reached (branch routes-2, 2026-11-03, user-town fixture, Oct 27 10:00, USA)

Every place below rendered with nothing visibly wrong at default settings (contact sheets of
every 60th frame). The beach and the city were compared with Dolphin (routes-3, "Dolphin
reference runs" below): the beach by the river mouth (sea colour and pattern, the light
shallow-water band over the sand, surf, the sand steps, the terrace's striped side, the bridge)
and the city plaza / west street (fountain, plaza stones, GracieGrace, lamps, Kicks at the
barricaded door, Shampoodle) look the same in both; only the camera position differs a little
(both runs walk by timed stick input). Since routes-4 the river, the museum rooms, the town hall
and every shop interior (Nook's, Able Sisters, the city shops) are compared too: all the same. Positions are the game's
own (actor +0x64), the town is about x 600-3000, z 500-3000 (south = +z), upper terrace y 84,
lower 28.

| Place | Reached by | Notes |
|---|---|---|
| Beach / sea | `TP=1300/28/2860`, walk | sea (m_ocean), surf (m_wave_white), shells; the shallow water over the sand and the stepped sand terraces |
| River | `TP=1880/28/2000` | river below the waterfall (m_fall, m_hamon spray), banks; bridges at the river mouth |
| Museum | `TP=mod102t0100:0/60`, walk in | lobby 260,56,484 (Blathers), right door = an exhibit wing (empty: nothing donated), left door = The Roost (Brewster; K.K.'s module is linked there too), stairs at the back left = observatory (Celeste) |
| Town hall | `TP=mod102t00FE:0/60`, walk in | Pelly at the counter, the room, the ATM-like machine left of the door |
| City | bus stop `TP=1808/84/700`, A at the sign, "To the city!" | Kapp'n's bus, his first-trip talk (~4000 frames of A), the city (`d_bgt_drawNP`): bus stop, steps, fountain plaza, streets; visitors (town_out NPCs) talk to the player when A is pressed near them |
| GracieGrace | `TP=mod103t0114:0/60`, walk in | ground floor with Labelle (upstairs needs progress) |
| Lyle's | `TP=mod103t0111:0/60`, walk in | HRA office (Lyle; d_a_rmobj_barricadeNP linked: another floor closed) |
| Club LOL | `TP=mod103t0118:0/60`, walk in | ticket counter hall (barricade linked: the show room closed) |
| Shampoodle | `TP=mod103t0110:0/60`, A at the door | Harriet's first-visit talk; her goodbye at the door needs A |
| Katrina's | `TP=mod103t0113:0/60`, A at the door | dark fortune room lit by two candles (Katrina at her table) |
| Crazy Redd's | `TP=mod103t0112:0/60`, A at the door | the doorman turns non-members away (members need an invitation: someone has to refer you) |
| Phineas, Kicks | - | Phineas was not in the city; Kicks stands by a closed door at 1085,68,386 |

With Enhanced lighting + Enhanced world on (`CF_ENHANCED=1 CF_WFX=1`): the sea and the river now
reflect (sky, clouds, the banks, the waterfall). Interiors: AO and the room light, nothing off.
The city: sun shadows on the streets; its pavements and plaza are ground for wet ground and
puddles (`m_hodo`, `m_center`, `m_around_ft`; `m_road` already matched). Fixed on routes-3:

- **Vertical bands near the shore** (Aurora 0391): not the surface mask (it held only m_ocean,
  checked with `AURORA_WORLD_FX_SHOW=1`) and not the sky fallback (`=8`: the band pixels were
  scene hits). Under translucent water the scene depth is the bed: the sea floor and the sand
  terraces' side faces below the water line. Rays reflected up from the surface "hit" those
  underwater faces in screen space and returned their stripes. A reflection can only show what is
  above the reflecting plane, so hits below the ray's origin plane now let the march go on.
- **Milky shallow water** (Aurora 0391): the game draws the sea over the sand with a low alpha
  near the shore, but the reflection weight ignored it (35-85 % of sky over a pixel that is mostly
  sand). The weight now follows how far the bed lies behind the surface (mask depth vs scene
  depth along the view axis): full from 1.5 "metres", 15 % at the waterline. Shallow water is the
  game's own plus a faint sheen; deep sea, river and ponds unchanged.
- **Leaves in the city**: the seasonal particles followed "the frame drew ground or water", and
  the city's pavements are ground since routes-2. They now also need the town's outdoor module
  (`d_bgo_drawNP` linked, the city's `d_bgt_drawNP` not; indoors `d_bgi_drawNP`): none in the
  city or indoors, unchanged in the town (`CF_WFX_LOG` "particles").

### Dolphin reference runs (routes-3)

`tools/dolphin/dolphin_run.ps1 -User <scratch> -Script <dsu script> -Seconds <n> -Out <dir>
[-Gecko "Beach teleport"|"Bus stop teleport"]` (scripts from `tools/dolphin/make_scripts.py`):
a scratch Dolphin user folder from the templates in tools/dolphin, a Wii Remote + Nunchuk driven
by `dsu_server.py` over DSU, window captures. What it took to make Dolphin's input scriptable:
`[Input] BackgroundInput = True` (Dolphin ignores pads while its window is not focused, and ours
sits off screen), `[Controls] WiimoteSource0 = 1` (the emulated Remote; without it no Remote
connected and nothing reacted), the DSU analog face buttons in protocol order Square, Cross,
Circle, Triangle (Dolphin reads the button pressure bytes: with them reversed Cross = A never
registered while Circle = B did), and presses of 0.35 s (shorter ones were missed with Dolphin
below full speed). There is no warp in Dolphin, so a Gecko code does the `TP=`: the player actor
pointer at 0x805D26E4 (USA rev 1, found in a `CF_MEM_DUMP`; heap and REL addresses match ours),
written once while the player still stands at the house door (1200, 84, 640). The town is the
clean post-save fixture `user-town-clean` (tools/routes/save_quit.txt, make_fixtures.ps1),
copied into Dolphin's NAND before every run, so a killed run never leaves Mr. Resetti waiting at
the next start. The in-game save before quitting stays out (routes-4): the HUD Save button needs
the Wii pointer on it, and Dolphin's IR position for a given DSU stick value is not the KPAD
position (calibrating it means screenshots of the cursor per value); the NAND restore already
gives every run the clean town, so it was not worth it.

**Every place (routes-4)**: `-Gecko "Places warps"` (tools/dolphin/make_warps.py; `--nocall`
builds the same code without the call, for testing the code alone) does in Dolphin what
`tools/routes/dolphin_places_warp.txt` does in the port: from the house-door arrival, every 900
frames the game's own stage change fn_80163064(stage manager 0x805FB218, stage, &pos, type,
angle, 2, 2) with the arguments the port's warps pass (copied from its `CF_WARP_LOG=1` "go to
stage" lines): river below the waterfall, museum lobby, paintings room, aquarium, The Roost,
observatory, town hall, Nook's Cranny, Able Sisters, GracieGrace, Shampoodle, Katrina's, Crazy
Redd's (inside), the HRA office, Lyle's auction house, Club LOL's ticket hall. A C0 (execute)
code: it saves r3-r31, CR and LR in its own stack frame (the first version saved too little and
wrote above its frame; Dolphin's code handler then read through a broken pointer every frame and
no frame was drawn), keeps its counter and table inside the code block (bl / mflr), and waits for
the player at the door before counting. All 16 places side by side with the port
(screenshots in the agent scratch area, 2026-11-03): **the same in every room**: geometry,
materials, lighting and the dark room edges, characters (Blathers, Brewster, Celeste, Pelly,
Tom Nook, Mabel, Labelle, Harriet, Katrina, Redd, Lyle, Lloid), dialogue boxes and text. Only
timing differs: the wallet (Bells) HUD fades in a moment after the arrival and Dolphin, below
full speed, was captured earlier; one Dolphin frame shows the camera-adjust icon (pointer
position). Nothing to fix.

**The first city trip: Rover instead of Kapp'n (routes-4).** Not a rendering or save difference
but what the console reports about WiiConnect24. On the first ride Rover sits on the bus and
explains WiiConnect24 (letters to friends, Message Board, villagers moving between towns) when the
game's boot check (`fn_80178640`, docs/DISTRIBUTIONS.md) found WiiConnect24 available (state 1 at
r13-0x2D74); otherwise Kapp'n gives his first-trip talk. The town's own WiiConnect24 setting
(save +0x735E4) does not decide it: it is 0 in the fixture and Dolphin still showed Rover.
Dolphin's generated SYSCONF has `NET.WCFG` = 1 (WiiConnect24 on, as a Wii set up for it); main's
port writes `NET.WCFG` = 0 unless F1 > Online > Nintendo events is on. Verified both ways in the
port: with `CF_DISTRIB=manual` (WCFG 1, the check's state 1) the same fixture's first ride gives
Rover's WiiConnect24 talk, as in Dolphin; with the default, Kapp'n. A real Wii with WiiConnect24
on behaves like Dolphin; one with it off (or never set up) like main's port. The online-1 branch
(not on main yet) reports WiiConnect24 on by default (`nwc24_dl.cpp consoleWc24()`, opt-out
`CF_WC24_CONSOLE=0`; letters to other towns need it), so after that merge the port's first ride
will be Rover's, like Dolphin's; routes that time Kapp'n's talk (city_bus.txt: A until the city)
work either way.

### Numbers (2026-11-04, quiet PC, RTX 3090 Ti, branch perf-2)

- **Recorded** (fresh cache, no list, one game at a time on an otherwise idle PC): the warp sweep
  (`sweep`: all 88 destinations of docs/WARPS.md, 400 frames each, from the town) by day and at
  night, the sweep again with every enhancement on (`enh_all`: Enhanced lighting Ultra + world
  lights + Enhanced world with reflections, wet ground, ripples, sparkle, haze, particles,
  tilt-shift), the warp routes (beach, river, town hall, museum rooms through the sweep, city
  streets, city shops in two parts, beach and city at night), town rain/snow, new_game; enh_all
  also for beach, city at night, town rain and snow. Union exported in canonical form
  (`CF_PIPELINE_EXPORT`): **2517 configs** (39 KB zstd). Per run a fresh cache met 138-209
  configs (sweeps 726-779).
- **Aurora 0392** (generic, LM/SMS too): list rows whose texgen reads a position matrix row
  (`GX_PNMTX0-9`, mtx 0-27: NW4R's projected textures in Rover's dialogue and the HUD) were rejected
  at load ("row N rejected: texgen", 12 rows), so every run missed the same 7 configs at the first
  dialogue (frame ~1694). The validator now accepts every XF matrix row (the shader reads
  `postex_mtx[mtx / 3]` either way).
- **Verified, default settings** (fresh cache with the list): beach_warp 0, river_warp 0,
  city_warp_night 0, town_snow 0, new_game 2, the 88-place sweep 13 misses (before 0392 with a
  default-only list: 8 / 3 / 7 / 9 / 18). Cold hold **12.7-13.5 s** (default: 1184 pipelines on 8
  threads), warm hold **0.17-0.22 s**.
- **Enhanced lighting/world (enh_all)**: cold hold 70-81 s (4934 pipelines). Measured only with a
  default-only list (29-98 misses per route: enh class 1 draws the canonical config does not show
  as lit, built at first draw as documented in Aurora's precache_variants); the shipped list now
  holds the enh_all recordings too (a recorded class-1 config builds class 1 when the enhancement
  is on and plain class 0 when it is off, deduplicated), but a fresh-cache enh_all pass with it was
  not run (user decision: step closed). Not recorded: MSAA layouts (rebuilt from the same list),
  festivals/holidays, other regions' discs, the shipped exe (warps are dev-only).

### Vertex-format variants (open, generic)

Aurora bakes each vertex attribute's GX format into the shader: component type, array stride and
the fixed-point **frac** (`gx/shader.cpp`, the fetch calls take `mapping.frac` as a literal;
`ShaderConfig::attrs`). NW4R (lyt text and panes, g3d models) quantises positions and texture
coordinates per resource, so the same material shows up with S16 frac 10/11/12, F32, strides 6/12
...: of 299 recorded configs, 235 remain when frac is ignored and **157** when frac, stride and
component type are ignored. Each extra combination is another pipeline and, when first met in play,
another compile hitch; this is why runs of the same route keep finding a few new configs (Rover's
text boxes at 2665, the shop at 6900-8600). Fix (Aurora, would help LM/SMS too where J3D uses
several fracs): pass frac (and stride) through the vertex uniform block and keep them out of the
pipeline key; component type could follow with a switch in the fetch helper. It changes the config
layout (bump `GXPipelineConfigVersion`, re-record the list) and touches shared shader generation,
so it is left for a coordinated change (docs/BACKPORT.md).

## Performance on a quiet PC (2026-11-04, branch perf-2)

RTX 3090 Ti, nothing else running, 1280x720 window off screen on a 60 Hz display, VSync off,
warm shader cache (`verify.py --cache-from`), `[perf]` every 10 s (`CF_PERF_LOG=10`), the windows
after the arrival; tools in the agent scratch (perfsum.py / juddersum.py over the run logs).

**Game logic holds its native 60 at 120 presented fps everywhere**: the old "42-60 game fps"
(render-3) was CPU contention from ~15 other game instances, not capture cost.

| place (route) | settings | presented fps median | game fps median | host ms med / p95 | worker ms med / p95 | over 1.5x / 2 min |
|---|---|---|---|---|---|---|
| town (town_day) | 120 | 120.0 | 59.9 | 1.6 / 2.9 | 0.7 / 1.3 | 1 |
| city streets (city_warp) | 120 | 120.0 | 59.9 | 1.5-1.6 / 3.1-4.5 | 0.5-0.6 / 1.0-1.2 | 3-4 |
| city shops (city_shops_warp) | 120 | 120.0 | 59.9 | 1.2 / 2.3 | 0.4 / 0.8 | 3 |
| museum rooms (museum_warp) | 120 | 120.0 | 59.9 | 0.9 / 2.2-2.3 | 0.4 / 0.8-1.0 | 1-2 |
| town hall | 120 | 120.0 | 59.9 | 1.1 / 2.4 | 0.5 / 1.2 | 2 |
| beach + sea | 120 | 120.0 | 59.9 | 1.6 / 2.6 | 0.5 / 1.0 | 2 |
| town, city, museum | Enhanced lighting Ultra + world lights + Enhanced world, 120 | 120.0 (min 115-116) | 59.9 | 1.1-1.7 / 2.3-3.5 | 0.7-0.9 / 1.2-2.0 | 2-7 |
| town at night, city at night, beach, rain | the same | 120.0 | 59.9 | 1.7-2.1 / 3.4-5.9 | 0.9-1.2 / 1.5-2.0 | 2-6 |
| town, city, museum, shops, town hall | 144 | 119.7-120.0 | 59.9 | 0.6-1.5 / 1.5-4.6 | 0.3-0.5 / 0.6-1.3 | 1-6 (before the pacing fix 14-20) |

(The only game-fps dip, 53-54 in one window of every run, is the continue's "Preparing saved data"
load before the town, the same in every setting.) **144 cannot be measured here**: the off-screen
window on a 60 Hz display delivers at most ~120 presents/s (Mailbox), while a frame costs 1-3 ms;
measure 144+ on a high-refresh display. That cap made the pacing governor cycle 124 -> 116 -> 124
fps every ~10 s (its floor was released because the frames were cheap); now a slowdown right after
such a release is taken as a display-chain limit and held for 60 s (BACKPORT 30): slowdowns per
2-minute run 13-17 -> 7, present intervals over 1.5x 14-20 -> 2-6.

**Judder detector**: in the town it reported ~16 "blend defects" per in-between frame (100-500 px,
identical at every alpha, g3d draws whose position slot the slot table had wrong: texture
matrices that material / texture-SRT display lists load into PNMTX rows, envelope loads the table
does not see). A draw is now judged only when its slot holds exactly what the replay wrote for the
noted patch (BACKPORT 31; the summary line counts the skipped draws; A/B `CF_JUDDER_SLOTCHECK=0`);
positive control `CF_INTERP_NOBLEND=1` (no matrix blending) still flags ~3100 per 5 s. With it,
**0 defects** (blend / camera / other, visible or not) at 120 and 144 in the town, city streets,
city shops, museum rooms, town hall, beach/sea, and with every enhancement on (night town with
fireflies, night city with window lights, beach, rain). Policy switches (shown at the game frame,
not defects): ef particles re-born elsewhere in their slot (the sea's spray at the beach: ~0.7 per
in-between frame, 3000-4000 px2 each), a few dozen turns in the museum.

Original 60 and default settings: the changes only act while interpolating (pacing governor after a
slowdown, the detector only with `CF_JUDDER_LOG`), and 0392 only changes which pipelines are built
ahead; no frame dump comparison was made.

## Stutters at 4K with Enhanced lighting (2026-11-04, branch stutter-ao)

User report (test build, USA, own town, 3840x2131 window on a 120 Hz VRR display, VSync, match
refresh, Enhanced lighting Ultra + Enhanced world): "harsh stutters"; last_run.log per 60 s: 40
present intervals over 1.5x, GPU 9.1 / 10.8 / 47.9 ms median / p95 / max, indoors 88.6 fps
presented / 48.6 game, 5854 pipelines in use vs 5107 configs the hold prepared. Reproduced with
`tools/routes/perf_user_warp.txt` (town walk, the house, the plaza) and the user's settings.ini at
3840x2160 (`--window 3840x2160`, VSync off, 120 fps target, `CF_PERF_LOG=10 AURORA_GPU_PROFILE=10
CF_PIPELINE_LOG=1 AURORA_CREATE_LOG=1`; agent scratch perfrun.py / perfsum.py). Causes found:

1. **World FX surface-mask pipelines missed at every launch** (Aurora 0451). Enhanced world
   re-draws each water / ground draw into its RGBA8 mask with a derived config (no blending,
   destination alpha 255 / 128). Those pipelines were built on first use only, never by the hold,
   so each launch missed them: 15 at once in Rover's bus, 7 at the first town view, more per place
   (`MISS ... layout 8463b4af895dcd36 (1 color 22 depth 48 x1) ... dA 255`), 1-3.5 ms each from the
   shader cache (a 20-50 ms frame), 0.1-0.5 s each cold. The rebuild now builds every remembered
   mask-shaped config for the mask target (`PrecacheWorldMask` in the enhanced bits, `0xef` in the
   `Pipeline precache:` line), and precache::init sends Enhanced world's configuration before the
   first rebuild (cf_world.cpp `applyStartupConfig`; it was first sent with the first scene, after
   the hold). Base vs new, same warm cache and route: 30 MISS -> 2 (the 2 are new scene configs).
2. **Configs not in the list for these settings**: recorded from these runs into data/pipelines.bin
   (scene configs of the house / plaza met the first time: 0.3-0.5 s cold compiles each).
3. **GPU at the 120 fps budget**: town at 4K Ultra busy 7.0-7.8 ms of 8.33 (EFB pass 2.8-3.2,
   sun shafts 1.06-1.16, World FX surface mask 0.64-1.4, shadow mask 0.5, AO 0.54 + its taps in the
   EFB shaders), so every heavier view is a late frame and the governor cycles 120 -> 104 fps.
   Cuts: sun shafts at half resolution (Aurora 0452, ~0.8 ms), AO off by default (~0.6-0.8 ms;
   "Enhanced lighting (City Folk)" below).
4. **GPU keep-warm filler in play** (BACKPORT 32): it started whenever a frame took under 75% of
   the recent heavy level, 30-125 times a minute in play here (user: 92 in the first minute); each
   time the view got heavier the frame carried scene + filler. Now only after 12 menu-light frames
   in a row (under 40%); A/B `CF_KEEP_WARM_RULE=old`.
   Since then the keep-warm is **off** in City Folk and its F1 option is gone (user decision
   2026-11-04): the game keeps drawing its world under every menu, so the GPU never gets the light
   menu frames it was made for (LM's pause screen). `CF_KEEP_WARM=1` turns it on for tests.

5. **Ambient occlusion's cost at 4K**: with AO on, per frame +0.75 ms in the EFB pass (the AO mask
   taps), 0.53 ms of AO passes, +0.5 ms mask copies (its plane), more staging: busy 8.2 vs 5.3 ms
   with it off (same build, back to back). Off by default now (section "Enhanced lighting (City
   Folk)", AO item); this is most of the gain below.

Results (perf_user_warp, 220 s of play per run from the town arrival, 4K, 120 fps target, VSync off,
warm caches, runs back to back in the order shown; the PC was shared, other agents' instances ran
during t1/t3/t4, so absolute interval counts are far above the user's 40/min; `perfsum.py`,
`gpuprof.py` in the agent scratch):

| run | exe / settings | presented fps (median) | intervals over 1.5x per min | GPU ms median / p95 | GPU busy ms (profile) | MISS | keep-warm starts |
|---|---|---|---|---|---|---|---|
| t1 | main, user's settings.ini (AO on 0.4) | 61.6 | 2734 | 15.1 / 19.9 | - | 28 | 360 |
| t2 | branch, same file (migrated: AO off) | 115.7 | 19 | 8.9 / 12.4 | 5.25 | 0 | 0 |
| t3 | branch, AO kept on (ao_version=2) | 74.9 | 3016 | 13.3 / 18.0 | 8.16 | 0 | 0 |
| t4 | main again | 66.5 | 2627 | 14.3 / 20.2 | 8.83 | 28 | 516 |
| t5 | branch, migrated again | 104.2 | 278 | 9.6 / 12.4 | - | 0 | 0 |

Sun shafts 0.92 -> 0.23 ms per frame (t4 vs t3). With AO on the GPU stays at the budget at 4K /
120 (t3): the user's PC (same GPU, no other load) should hold ~120 with AO off; with AO on, 60-100
fps (or a lower render scale) is what fits. Not measured: an idle-PC pass of the same table.

Not seen as causes: render-thread creations in play (`AURORA_CREATE_LOG`: one 6 ms shadow-map
atlas creation at the first shadowed frame, everything else under 1 ms; the enhanced passes are
built in the hold, 0223), CPU (host / render worker 1-3 ms median). Furniture / night-light shadow
slots were not singled out (the house showed no hitches beyond the room load). Shared-PC caveat: other
agents' game instances ran during most runs (GPU at 100 %), which inflates "Staging copies" and
"Mask copy to buffer" to 1-3 ms and the intervals into the hundreds; compare A/B pairs only.

## The once-a-minute freeze (2026-11-04, branch mem-hitch)

User report (4K/120, Enhanced lighting Ultra, own town): `hitches 1` in every `[perf]` line,
render worker / GPU max 683-852 ms once a minute, each `[mem]` line with "heaps 887 MB". Same cause
as LM issue #43 (LM docs/RENDERING.md "The once-a-minute freeze"; BACKPORT "Taken from LM / SMS"):

- The 60 s `[mem]` report (src/recomp/mem_report.cpp) ran on the host thread, and its C/C++ heap
  total called `HeapSummary` on every process heap. `HeapSummary` walks every block of a heap while
  holding that heap's lock: ~0.4 s for the ~650-760 MB of heaps here, 0.7-0.9 s for the user's
  ~890 MB. Every thread that allocates meanwhile (render worker, Dawn, the driver, the guest's HLE
  calls, audio) waits on the lock, so no frame is presented for that long.
- Fix (same code as LM): the report runs on its own low-priority thread (the host thread only wakes
  it); the heap total only with `CF_MEM_HEAPS=1` or `CF_MEM_TRACK` (knowing it stalls the game);
  DXGI adapters enumerated once instead of every report; `[mem] report took N ms (sampler thread)`
  when a report takes over 20 ms. The `[mem]` lines are otherwise unchanged ("heaps N MB" is gone
  from the first one unless asked for).
- **Rule: no heap walks or other whole-process queries on a frame or game thread** (host, render
  worker, FIFO, guest, audio). Periodic work that needs one gets its own thread, and must not take
  locks those threads need for long.

Measured (the user's settings.ini at 3840x2160, 120 fps target, VSync off, `user-town-clean` +
`tools/routes/perf_user_warp.txt`, warm caches, runs back to back; agent scratch
`scratchpad/mem-hitch/run.ps1`, `gaps.py`):

| run | `[mem]` interval | `[perf]` windows with a hitch after a `[mem]` report | stall per report | guest frames at 240 s |
|---|---|---|---|---|
| main (`CF_MEM_LOG=30 CF_PERF_LOG=10`, 250 s) | 30 s | 8 of 8 (render worker / GPU max 400-550 ms) | 400-580 ms | 14059 |
| branch, same | 30 s | 0 of 8 | none (report < 20 ms on its thread) | 14211 |
| branch, defaults (60 s), 330 s, `CF_PACING_LOG` | 60 s | 0 of 5 (`hitches 0` in every line; no start-to-start gap over 58 ms after the start-up) | none | - |
| branch, `CF_MEM_HEAPS=1 CF_MEM_LOG=30` | 30 s | the stall is back, as designed | "report took 375-397 ms" | - |

Other periodic work on frame threads checked: `[cpu]` sampling (per-frame
GetCurrentProcessorNumberEx counters, one log line a minute), `[perf]` (sorts the minute's frame
times), the display refresh / vblank monitor checks (cached SDL display mode, MonitorFromWindow every
500 ms), the GPU memory query (now on the report thread), texture-pack stats (counters; on the host
thread only with the `CF_TEXPACK_LOG` test aid), the GX object table collection (a scan of guest
memory, 2-7 ms every ~18 s of play on the guest thread, not process-wide). The update check, the
Wii Remote HID enumeration (every 2 s) and online/distribution work already run on their own
threads. None is a whole-process query.

## Performance (2026-11-02)

Measured with `verify.py --presets perf4k120 --routes nook --cache-from <warm cache>`
(`CF_PERF_LOG=20 AURORA_GPU_PROFILE=20`): saved town + Nook's Cranny, `resolution_scale` 5
(2400p, 10.2 MP, more than 4K's 8.3 MP), 120 fps target, VSync off, window off screen, RTX 3090 Ti. **The PC was heavily loaded** (11 other agents, ~14-28 other game/compiler
processes), so these are upper bounds:

| 20 s window | presented fps | game fps | host ms med/p95 | render worker ms | GPU ms med/p95 |
|---|---|---|---|---|---|
| town | 110.7 | 59.2 | 1.0 / 1.8 | 1.4 / 2.4 | 4.7 / 11.6 |
| town | 96.5-106 | 55.7-58.1 | 1.9-2.0 / 2.8-4.1 | 1.4-1.6 / 2.9-6.8 | 8.1-9.7 / 12.8-14.2 |
| Nook's | 57-88 | 57.7-59.6 | 2.2-3.3 / 3.2-4.7 | 1.2-1.6 / 2.2-2.9 | 10.7-17.8 / 19.4-25.0 |

The game's own work (host thread, render worker) stays at 1-3 ms per frame; what limits 120 fps at
2400p here is GPU time. Aurora's profiler (busy time of our own passes) gave the main EFB pass
0.4-0.5 ms per frame in town but **4.2-8.1 ms in Nook's Cranny** (48% of wall time, 57 fps
presented); the rest of the "GPU ms" above is queueing behind the other processes' games. The shop
cost needs an idle-PC re-measurement at 1x and 5x (does it scale with pixels: overdraw / fill, or
is it contention inflating the timestamps?) before anything is changed. In the first,
cold-cache run the big hitches (0.5-3.7 s) were all on-demand pipeline compiles; with a warm cache
only the vertex-format misses remained (0.2-0.4 s each). Nothing generic and cheap to fix showed up
beyond the vertex-format variants; re-measure on an idle PC.
# Rendering notes (City Folk)

Rendering status of the accurate base: docs/WII.md "Rendering status". This file collects the
optional enhancement passes layered on top. LM's docs/RENDERING.md is the reference for the shared
Aurora machinery (post-process chain 0005/0006, enhanced lighting 0016+, depth snapshots).

## Enhancement passes

Every enhancement that changes the 3D picture runs at one point of the frame: after the game's main
g3d scene (the first `nw4r::g3d::ScnRoot` DrawOpa/DrawXlu pass and the `nw4r::ef` effects after it)
and before the HUD (the first `nw4r::lyt` root pane, `cf_hud.cpp`). Order (shared by the branches
that add passes; keep it when adding one):

1. **Enhanced lighting** (branch enh-light, `GXSceneEndAurora`, Aurora `enhanced.cpp`; section
   "Enhanced lighting (City Folk)" below). Before every EFB pass with opaque world draws: depth
   (+ normal) pre-pass, shadow-map atlas (sun / moon cascades, night-light cube maps), shadow + AO
   mask, sun shafts (0342), read by the world shaders of that pass (forward shadows, AO). At the
   layout point (`cflight::onLayoutDraw`): the time-of-day colour grade (0342), then the composite
   (bloom, sun shafts). Note for (2): its colour grade runs after this one; the two should not
   both push a time-of-day look (enh-light's is keyed by the sun's elevation, see below).
2. **Enhanced world** (this file, `GXWorldFxEndAurora`, Aurora 0350 `world_fx.cpp`), in this order
   inside its one scene op: surfaces (water reflections, rain ripples, wet ground, snow sparkle) ->
   haze -> seasonal particles -> colour grade -> tilt-shift (last before the post chain).
   Emitted right after (1) at the same point (`world::onLayoutDraw`, called after `cflight`'s).
3. **Post-process chain** (`aurora_postfx_set_chain`, Aurora 0006): CRT filter (src/recomp/crt.cpp),
   applied to the presented image including the HUD.

Both (1) and (2) are scene ops (`push_scene_op`): the EFB pass is sealed, the op runs on the
frame encoder with the EFB color/depth, and drawing continues on a pass that loads the result, so
the HUD is drawn on top unchanged.

## Enhanced world

F1 > Visuals > Enhanced world (off by default; settings section `[enhanced_world]`, a settings
extension in `src/game/cf_world.cpp`). With the master switch off nothing is written to the GX
stream: frames are byte-identical to the game's.

| Option | Default (when on) | What it does |
|---|---|---|
| Water reflections | on, strength 0.6, medium (24 steps), ripple distortion 1 | rivers, ponds, the sea reflect the scene (screen-space march) and the sky + moon where the ray leaves the picture; the game's own water pattern bends it |
| Wet ground in rain | on | while it rains (weather state 3/4): bare ground and paths darken, puddles (world-anchored noise, mostly on soil, not on grass) reflect lamps / houses / sky; dries over ~4 minutes after |
| Rain ripples | on | expanding rings on water and puddles while it rains |
| Snow sparkle | on | glints on bright snow-covered ground, snow cover dates (Dec 11 - Feb 24) in daylight with the sun out |
| Haze on grey days | on, strength 1 | depth-based haze beyond the player's distance on cloudy / rainy / snowy days, a trace at dawn and dusk |
| Seasonal particles | on, density 1 | petals Apr 1-8, leaves Oct 17 - Nov 25, snowflakes Nov 26 - Feb 24 (hemisphere option: +6 months); in the town only (not indoors, not in the city) |
| Colour preset | Original | New Leaf warmth / Soft film / Vivid (+ strength); grades the 3D, not the HUD |
| Tilt-shift (diorama) | off | blur above / below the player's row (strength, sharp band) |

### How it works

- **Surface classification** (`cf_world.cpp`): `nw4r::g3d::Draw1Mat1ShpDirectly` (every material
  draw; r3 = ResMat -> ResMatData, name at data + s32 [data + 8]) inside the frame's first ScnRoot
  pass sets `GXSetSurfaceAurora`: water = `m_ocean`, `m_river`, `m_river2_pattern` (ponds use the
  river pattern); ground = `m_grass`, `m_soil`, `m_soil_sand`, `m_soil_stone`, `m_stone`,
  `m_Es_G_soil`, `m_outgrass`, `m_buildsite`; everything else = world (camera only). Excluded on
  purpose: river foam edge `m_river1_edge`, surf `m_wave_white`, `m_wave_shadow`, the additive
  `m_z_highLight`, waterfalls `m_fall*`, `m_hamon`, `m_kemuri`, the sea floor `m_sand_sea`, indoor
  `m_floor`. `CF_WFX_MAT_LOG=1` lists every material once with its kind (town materials seen:
  m_cliff, m_grass, m_soil, m_verge, m_river_cliff, m_under_grass, m_cliff_slope, m_bridgeA,
  m_sand_sea, m_soil_sand(_side), m_sky, m_moon, m_cloud, m_ocean, m_river2_pattern, m_cross,
  m_river1_edge, m_wave_white, m_z_highLight, m_fall, m_hamon, m_fall_light, m_kemuri, tree /
  flower / house materials). City (routes-2, city.txt): `m_hodo` (pavements), `m_center`,
  `m_around_ft` (plaza) are ground with `m_road`; world: `m_kadan` (flower beds), `m_busstop1`,
  `m_fountain`, its water `m_water0-2`, the buildings (`m_grace`, `m_HRA`, `m_auction`,
  `m_theater`, `m_fortune`, `m_tsunekichi*`, `m_tw_*`), `m_tunnel*`, `m_clock`, `m_lamp`, the
  `m_shadow*` decals. `CF_WFX_KINDS="<material>=water|ground|world;..."` tries a kind for one run.
- **Camera**: the main ScnRoot's current camera (u8 [root + 0xF4], CameraData at root + 0xF8 +
  0x10C n; its world -> view MTX34 comes first; the camera position / target fields there are 0 in
  City Folk) goes into Aurora's application XF memory (0x0700) as a plain XF load noted for
  interpolation as a position matrix (tag scope 0xCF57F000), so in-between frames get the
  in-between camera. **The town is z-up** in g3d's world: the camera's up row leans to +z and its
  forward to -z (e.g. rows (1,0,0) (0,0.73,0.68) (0,-0.68,0.73)); the acres' model matrices are the
  view x a translation, the "rolling log" curve is a small rotation about x per acre (seen 7 deg).
  The port picks the world's up axis per frame among the six axes (the one the camera's up points
  along and its forward away from, with hysteresis) and sends view x B, B rotating that axis to +y,
  so the effects' "world" is always y-up. Since branch interp-town (docs/WII.md "Town interpolation"), with a fitted log the frame is the log's own flat frame at the ground ahead (the snapped axis flipped by 90 degrees as the camera turned about the log) and the centre is that ground point (Aurora 0440). The projection is the main view's own (Hor+ included),
  taken by Aurora from the surface draws (the sky dome has its own depth range). The focus point
  (particle volume, haze start, effect scale: "one metre" = 1/8 of the view height there, about 20
  units in the town at the default zoom, focus ~390 units away) is the scene depth under the view's
  centre column at 56 % of its height, read on the GPU, so no player position is needed.
- **Aurora 0350** (`lib/gfx/world_fx.cpp`, `include/aurora/world_fx.h`): at the scene end the
  classified water/ground draws are re-rendered into a surface mask (their own GX shader, no
  blending, destination alpha = kind; water writes its depth, since the scene depth under
  translucent water is the pond bed), then one fullscreen "surfaces" pass (scene color + depth +
  mask -> RGBA16F), the particles (instanced quads, procedural world-space positions in a box
  around the focus point, soft depth test against the scene depth), an optional horizontal
  tilt-shift pass, and the final pass (grade, vertical tilt-shift) back into the EFB inside the
  main view's rectangle.
- **Amounts** (`updateAmounts`): weather state from `dWeather_c` (its per-frame update
  fn_801C92F0 / PAL 801C8FE4 / JP 801C9090 / KR 801C90C8, state at +0x5884; works with
  CF_WEATHER_FAKE and the real-time weather option), the game clock (hour, date through the
  hemisphere option), host time for the wetness ramp. Sky colours for the reflection fallback by
  hour (night / dawn / day / dusk) greyed by the weather; the moon glints only at night under a
  clear sky. Outdoors = the frame drew water or ground.
- **Interpolation**: effect time is host time at each replayed scene end and the camera is blended,
  so ripples, sparkle and particles move smoothly at any presented rate; nothing in the effects is
  keyed to the game's 60 Hz. Checked at 120 fps with `CF_INTERP_TEST=2` + `CF_FRAME_DUMP`: the
  in-between frames carry the effects (leaves at their in-between positions), no interpolation
  warnings. Frame dumps / EFB copies after the scene end include the effects.
- **FIFO order**: the game writes immediate-mode vertices through the write-gather pipe and the HLE
  closes such a draw lazily at the next GX call; host code that writes Aurora commands from a game
  hook must close it first (`gx::endPendingDraw()`), or its command lands inside the open draw
  ("GXEnd: vertex data not evenly divisible" thousands of times, a corrupted draw). cf_world.cpp does;
  any other hook that emits GX*Aurora calls (enhanced lighting's draw class) should too.
- **Screen-space limits**: the town camera looks down at ~43 deg, so a reflected ray from a pond
  rises off the top of the picture within a few steps; most of what a pond reflects is the sky
  (fallback: zenith / horizon by hour and weather, soft clouds, the moon at night). Scene hits are
  the near banks and whatever stands on the far side of the water (`AURORA_WORLD_FX_SHOW=8` shows
  why each ray ends). A planar reflection (re-rendering the world mirrored) would add off-screen
  trees and houses; not done.

### Cost (RTX 3090 Ti, rain in the town with every option on incl. tilt-shift; other agents' game
instances were running on the same GPU, so these are upper bounds)

| Pass | 2x (1621x912, ~1080p) | 4x (3243x1824, ~4K) |
|---|---|---|
| surface mask (re-render of 24 water/ground draws) + mask depth | 0.20 ms | 0.43-0.67 ms |
| surfaces (reflections, wet ground, ripples, sparkle, haze) | 0.28 ms | 2.6-2.9 ms (4.4 before trimming the puddle march and the sheen's clouds) |
| particles (350 leaves) | 0.06 ms | 0.05 ms |
| tilt-shift (horizontal pass) | 0.07 ms | 0.3-1.0 ms |
| final (grade + vertical tilt-shift) | 0.05 ms | 0.7-1.0 ms |

Water reflections alone are the mask + the water pixels' march; grade-only is the final pass.
`AURORA_GPU_PROFILE=<s>` lists every "World FX ..." pass.

### Screenshots (agent scratch, not in the repo)

A/B pairs of one frozen moment (`CF_WFX_AB=1 CF_WFX_FREEZE=1`, left off / right on), from the
user-town fixture at the pond by the house: day (Oct 27 10:00), rain (`CF_WEATHER_FAKE=rain`:
wet ground, puddles, ripples, haze, leaves), snow (Jan 15 11:00: sparkle, snowflakes), night
(22:00), cherry blossom petals (Apr 3), the three grades, tilt-shift.

### Open

- Sea and river reached (routes-2: beach.txt, river.txt): they reflect; the shore's vertical
  bands and milky shallow water are fixed (Aurora 0391, "Places reached" above). A faint
  reflection of the terrace side above the water line remains (a real reflection).
- Rain streaks are drawn since render-3's Aurora 0304 (not on main yet); the rain ripples /
  puddles here do not depend on it.
- The surface-mask pipeline variants are compiled on first use (a few per scene), not precached.
- PAL / Japan / Korea: the hooks are by name (g3d, lyt) or mapped (dWeather_c), not run.
- Planar reflections for off-screen objects; moon phases.

### Test aids

`CF_WFX=1` (on for one run), `CF_WFX_SET="key=value;..."` (`[enhanced_world]` keys, e.g.
`grade=1;tilt_shift=1`), `CF_WFX_FREEZE=1` (effect time fixed, deterministic shots), `CF_WFX_AB=1`
(effects on odd guest frames only: consecutive shots compare with / without), `CF_WFX_LOG=1`
(camera, weather and amounts every 120 frames), `CF_WFX_MAT_LOG=1`, `AURORA_WORLD_FX_LOG=1`
(camera / projection / draws at the scene end), `AURORA_WORLD_FX_MTX_LOG=1` (a surface draw's
matrix), `AURORA_WORLD_FX_SHOW=<n>` debug views: 1 surface mask, 2 depth / focus row, 3 mask alpha +
wetness, 4 ground branch, 5 reflection hit share / weight, 6 focus distance / scale, 7 raw depth,
8 why each water ray's march ends. `CF_WFX_SET` takes `;` or `,` between keys (`,` inside
run.ps1's -Env).
Weather and season: `CF_WEATHER_FAKE=rain|heavyrain|snow|clear`, `CF_CLOCK_START` (docs/CALENDAR.md,
tools/routes/run.ps1).

## Enhanced lighting (City Folk)

F1 > Visuals > Enhanced lighting (src/game/cf_light.cpp provider, src/recomp/enhanced_lighting.cpp
settings -> Aurora, LM's Aurora enhanced-lighting series + 0340-0342). Master switch **off by
default**; off, nothing is classed, submitted or hooked differently: the original rendering is
unchanged (the hooks call the original and return). Presets Low / Medium / High / Ultra / Custom.

### Scene structure (USA rev 1; PAL / JP / KR hooks by hash)

- The field / room scene is drawn by **d_s_stageNP fn_125_C0** (hooked): `ScnRoot::DrawOpa` +
  `DrawXlu` + effects twice (pass 1 camera 0, pass 2 after `fn_802AEAAC(1)`), with a layout pane
  drawn between them. The enhanced scene begins at the first `G3DState::SetCameraProjMtx` of pass 1
  (camera 0; the root then loads all 32 cameras, so the last one set is not the current one) and
  ends at the first root lyt pane (`Pane::Draw` depth 0, cf_hud.cpp) or at the end of fn_125_C0.
  Draws in between are class WORLD.
- **The camera data is the true world -> view** (checked: ScnObj + 0x6C view matrix = camera x
  ScnObj + 0x3C world matrix). The town is a **"rolling log"**: a cylinder about world X; every
  placed object's world matrix is turned with the surface (the player's house: Y axis
  (0, 0.53, 0.85)). So "up" for the sun is the local ground normal (since interp-town: the fitted log's normal at the ground ahead, docs/WII.md "Town interpolation"; before, `sLocalUp`: mean Y axis of the
  obj_* / sdw_* models drawn the previous frame), east = world X, south = X x up. (A first version
  used world Y: the ground's pre-pass normals came out mirrored against the light and the house
  front lay in its own shadow.)
- g3d model names: the first word of a ScnObj pointing at an `MDL0` block is its ResMdl; the name
  offset follows the section offsets (rev 10/11: 14 sections). `G3dProc` tasks: 1 calc world,
  4 calc view, 6 draw opa, 7 draw xlu (bit 16 a flag). Test aid `CF_MODEL_LOG=1` (every model
  name twice, its matrices; obj_myhome0's full matrices every 600 frames).
- **Indoors** = d_bgi_drawNP (module 107) linked and d_bgo_drawNP (108) not (Rover's bus counts).
- The game's GX lights (CF_LIGHT_TRACE=2): directional lights ~8e6 units out, a warm key light
  (128,128,103) from behind the camera (view dir (0.24, 0.33, 0.91)), a near-black second light,
  a white specular one. They do not follow the time of day (the game tints its colours instead).

### Sun and moon shadows (Aurora 0340)

- A **directional extra light** (`ExtraLightDirectional`, include/aurora/lights.h): `pos` = view
  direction toward the light, `radius` = shadow distance, `box[]` = shadow tint (the colour a pixel
  is multiplied by in full shadow), terminator strength / width, angular size (PCSS), caster
  distance toward the light. With `ExtraLightCastShadows` it takes shadow slot 0 (always first,
  shadow maps only) and renders **four orthographic cascades** into its atlas cell (the cube
  layout's faces 0-3; faces 4/5 clip everything and carry the cascade data): practical split
  (log 0.7 / uniform 0.3), bounding spheres (size steady under camera rotation), centres snapped to
  texels (no crawling edges). The mask pass picks the cascade by view depth, blends into the next
  over the last 12 %, receiver-plane PCF (8 / 12 / 20 taps by preset) with PCSS from the light's
  angular size, fades out over the last 15 % of the distance.
- Receivers: every enhanced world draw multiplies its colour by `mix(1, tint, shadow)`, where
  shadow = max(shadow map, terminator of surfaces turned away from the light within the shadow
  distance). Unlit opaque world draws get the enhanced variant too (`lightReceivers`, like AO's
  receivers; precache bit `PrecacheRecv`). The added light (colour x intensity x N.L, shadowed)
  joins the lit channels like other extra lights.
- Interpolation: a directional light's `pos` is turned with the camera only (src/recomp/interp.cpp,
  rotation, never a "jump"), colour / intensity / reach blended by id (0x5E000001).
- Port: the sun path from the game clock (latitude 38 N, mirrored with the southern-hemisphere
  option; day of year -> declination), the **moon** opposite it at night (a full-moon path), the
  shadow light held at least 12 degrees up. Strength x weather (cf_weather's hourly state via
  fn_801C9E48: clear 1, some clouds 0.85, cloudy 0.3, rain 0.12, heavy 0.06, snow 0.15, heavy snow
  0.08), softer under clouds. Tint: cool blue by day, deeper at the golden hour, grey-blue under
  the moon. Options: direction from the time of day or from the game's own key light (shadows
  then fall away from the camera); indoors the room's key light casts softer shadows
  ("Indoor shadows"). **Round blob shadows** (the game's `sdw_*` models): kept / hidden while real
  shadows are strong ("fade", default) / hidden. Test aids `CF_SUN_HOUR=<h>`, `CF_SUN_AWAY=<f>`,
  `CF_SCENE_LOG=<n>` (`[scene]`, `[sky]` lines), `AURORA_ENH_GLOWDBG=8` (shadow mask),
  `=9` (cascade / shadow / N.L), `=10` (light direction | receiver normal).
- **AO and art normals** (Aurora 0341): City Folk lights its backdrops and foliage with normals far
  from the geometry (the tree wall behind the town has upward normals): the horizon AO took them
  and greyed the whole wall out. Where a vertex normal is more than ~60 degrees off a reliable
  depth-derived normal, the depth one is used.
- **AO only on the ambient light, off by default** (branch stutter-ao, Aurora 0450; user report
  2026-11-04: "AO overpowers the real-time shadows"). AO multiplied the pixel's whole colour after
  the sun / moon tint, so a contact shadow in full sun was as dark as a sun shadow (the two read
  alike), and its intensity curve was visibility^(1 + 2 i): even the slider's lowest value gave the
  full occlusion. Now (gx/shader.cpp) AO only darkens the share of the light a full shadow of the
  directional lights would leave (`enh_dir_full`, the product of their shadow tints, against the
  tint actually there, `enh_dir_tint`), minus the extra lights' share of colour channel 0
  (`enh_s_xl`), and never light-emitting surfaces: in full sun only the ambient part (~40% at the
  default sun strength) is occluded, inside a sun shadow all of what is left (that is ambient
  light: nothing is darkened twice). Intensity i is visibility^(2 i) (0 none, 0.5 the plain
  horizon-based occlusion). Defaults: off in every preset, intensity 0.30, radius 40 (was on,
  0.40 = ^1.8, 60). `AURORA_ENH_AO_SHOW=1` shows the occlusion actually applied (share included).
  **Settings migration** (src/port/settings.cpp `migrateAo`, logged as `[settings] ambient
  occlusion settings from an older version: ...`): a settings.ini without
  `[enhanced_lighting] ao_version` (written before this branch) with the intensity at the old
  default 0.40 (never tuned) gets the new defaults (AO off, 0.30; the user's file: on, 0.40, radius
  32 -> off, 0.30, 32); a tuned intensity is kept with AO on/off as it was, converted to the same strength on the new curve (i + 0.5); radius 60 (old default)
  becomes 40, any other radius is kept. Saved with `ao_version=2` afterwards.

### Shadows of objects outside the view (branch shadow-casters, Aurora 0470)

User request: with sun / moon shadows, trees, houses and rocks that left the picture took their
shadows with them (the game culls them), very visible with the long morning / evening shadows.
src/game/cf_shadow_casters.cpp widens the game's own culling decisions toward the light and draws
what only the widened test keeps as a **shadow caster only**.

- **The game's culling** (USA rev 1; hooks `all`):
  - Field objects (trees, bushes, rocks, fences, signs; d_fg_drawNP fn_112_2184): a bounding
    sphere (record + 0x14 through fn_800829D4 onto the log, radius + 0x34) against the game
    camera's frustum (camera object + 0x254, from fn_80162D1C) with fn_802B777C: view matrix at
    +0x00, near / far view z at +0x70 / +0x74, side planes (x, z) at +0x30/+0x38 and +0x40/+0x48,
    (y, z) at +0x54/+0x58 and +0x64/+0x68. The fg manager only hands the objects of the area
    around the view to it (5-25 culled per frame).
  - Structures (daSTR_c: houses, shops, town hall, museum, bulletin board, bus stop, build
    sites...): the draw d_a_strNP fn_99_2B04 skips the model when fn_99_2548 says so: a window
    in the town's flat coordinates around a camera reference point (fn_80166CDC: |x - ref.x| <= w,
    b <= z - ref.z <= a; ref = USA 0x8062426C, decoded from the function's own code) or a line
    behind the camera (fn_8018ABC8: z - k r beyond camera + 0x368). A drawn structure inserts its
    models into the scene group every frame (ScnGroup::Insert); a culled one does not. 28-34 of
    them are culled per frame on the plaza.
  - g3d's own culling (ScnObjGather::Add with the gather frustum, ScnObj option 9): no town or
    city object uses it (0 culled); hooked anyway (by name) for other places.
- **Widened toward the light**: each test again with the object swept along the light's travel
  direction by the length of its shadow: 3D (field objects, g3d boxes) `height / sin(elevation)`
  with height = the sphere's diameter / the box's diagonal; flat map (structures) the shadow's
  footprint `height / tan(elevation)` along the light's horizontal direction, height 160 (the
  tallest common structures); at most the sun shadow distance (and the cascades' caster reach,
  4000), and only what lies within the shadow distance in view depth. Elevation over the
  ground in view (the light is held >= 12 degrees up, like the shadows themselves): at 07:30 /
  16:45 in October about 12 degrees (shadow ~4.7 heights long), at noon ~38 degrees (~1.3 heights:
  still a few objects behind the camera, south, whose shadows fall north into the view).
- **Shadow casters only**: field objects get the draw class `AURORA_DRAW_CLASS_SHADOW` around their
  draw (enh::setShadowOnly); structures kept by the widened test mark the models they insert, and
  cf_tags.cpp's G3dProc hook draws those as class SHADOW (opaque pass of the main view only; their
  translucent parts (blob shadows, fade models) and the stage's second root pass are skipped, as
  the game culled them). Aurora 0470 records an opaque depth-writing draw of that class into the
  shadow-map atlas with the world draws' caster pipeline and drops it everywhere else (EFB pass,
  depth / normal pre-pass, AO, glow, world effects, EFB copies; the frame-interpolation tape
  carries the class, so in-betweens drop it too). The draw's main pipeline is the world class's
  (no new variants to precache).
- **Interpolation**: casters keep the objects' tags (field objects by place + kind, g3d objects by
  ScnObj), so a caster that comes into view (or leaves it) blends from its previous matrix like
  any other object: no pop at the switch. 120 fps at 4K Ultra: judder detector 0 defects (727k
  tagged draws judged, identity check 0 jumps); the walk route shows no stray geometry.
- **Off**: everything is decided only while sun / moon shadows are cast outdoors (cf_light
  submitSky calls `castersSetSun`); with sun shadows or Enhanced lighting off nothing is widened,
  no class is emitted (0 `[casters]` lines, 0 caster-only draws in a run with the user's settings
  and sun shadows off). The new hooks call the game's functions unchanged then. (Shots of two runs
  are not comparable frame by frame here: the continue's loading timing moves the arrival by up
  to 80 frames between runs of the same build.) Indoors: not needed (rooms are in view whole).
- **The merge bug found on the way** (generic, BACKPORT 0470): a draw that `note_draw` dropped
  left the draw cache as if recorded, so the next draw with unchanged GX state was merged into the
  last recorded command, indexed over the skipped vertices: kept structures showed up as huge
  stretched walls in front of the camera. The merge is cut after a dropped draw now; consecutive
  caster-only draws merge into each other instead (535 -> 16-40 caster draws per frame).
- **Cost** (RTX 3090 Ti, quiet PC, user's settings at 4K (scale 3) / 120 fps, morning walk route
  tools/routes/shadow_casters.txt, back to back with `CF_CASTERS=0`): GPU busy 2.49 vs 2.48 ms per
  frame (shadow-map atlas pass 0.077 -> 0.084 ms), host thread median 1.9 -> 2.0-2.1 ms, render
  worker 0.9 -> 0.9-1.0 ms; 120.0 fps presented in both.
- Results: A/B pairs (left the game's culling, right with off-screen casters, below the difference
  x8) in the agent scratch: morning shadow bands across the path and bridge from trees off the
  right (east) edge and from structures behind the camera, evening shadows on the plaza and the
  town hall's wall from objects off the left (west) edge; noon: no visible change in the pairs.
- Test aids: `CF_CASTERS=0` (off), `CF_CASTERS_AB=<n>` (on in every other block of n frames: shots
  n apart compare; structures are decided a frame ahead, measured), `CF_CASTERS_LOG=<n>` (counts:
  culled / kept per kind, models drawn as casters, Aurora caster-only draws; the kept models),
  `CF_CASTERS_SHOW=1` (kept objects drawn in the picture too), `CF_CASTERS_SCALE=<f>` (sweep
  length), `CF_CASTERS_NOSTR=1` (all but structures), `AURORA_ENH_SHADOW_CLASS_SHOW=1`. Route
  `tools/routes/shadow_casters.txt` (plaza, slow walks W/E/N/S/diagonal with pauses).
  shadow-casters-2: `CF_CASTERS_KINDS=<mask>` (1 field objects, 2 structures, 4 NPCs, 8 fgobj, 16
  other actors, 32 g3d culling; A/B of one kind), `CF_CASTERS_INSERT_LOG=1` (every model an actor
  draw inserts, with its height; each NPC's shown flag, position, window, vtable and module),
  `CF_CASTERS_NPC_WINDOW=<f>` (the NPC draw's own test sees its window scaled: villagers in the
  picture culled and kept, their shadows checked on screen), `CF_CASTERS_FORCE=npc|obj` (every
  shown villager / fgobj object kept: vanishes, shadow left). Routes `shadow_casters_npc.txt`
  (+ `_pm`: plaza, CF_TELEPORT next to a villager), `shadow_casters_visitor.txt` (+ `_pm`: the
  visitor of module 57).
- Open after the first pass: villagers, d_fgobj_managerNP, fn_112_17F0, the city (done on
  shadow-casters-2, below); PAL/JP/KR not run.

#### Actors, trees behind the camera, the city (branch shadow-casters-2)

- **How actors draw** (USA rev 1): actors are fBase-style objects (vtable at actor + 0x60; draw at
  vtable + 0x2C, preDraw fn_8000BDF8 at + 0x30). A model's "draw" (game model class, vtable
  0x8049E190, + 0x14 = fn_802AEDF8 -> fn_802AEBC4) only **inserts it into the scene group for this
  frame** (scene root vtable + 0x34 = ScnGroup::Insert; the group is emptied after the frame). The
  stage's scene calc (fn_802AEACC: ScnRoot CalcWorld / CalcView / Gather / ZSort; no g3d
  UpdateFrame) then poses only what was inserted. Animation frames are set by the actors' own
  execute, so an actor kept for its shadow is inserted, posed by CalcWorld with this frame's
  animation and drawn as a caster: its shadow is animated correctly (the kept models go through
  the same Insert marking as structures, cf_tags.cpp draws them as class SHADOW). Nothing the
  game's logic reads changes (only the scene insertion; execute is untouched).
- **Villagers and other NPCs**: the NPC base draw fn_8001665C (d_a_npc_outNP's villagers, the
  special NPCs d_a_npc_sp_* (their own draws call it), the city's d_a_npc_town_*): if the NPC is
  shown (+ 0x1C38), its position (+ 0x64) is tested against a window around the camera's point in
  the town's flat coordinates (fn_800284F8 -> fn_800283B4: box at NPC + 0x19E4, x -416..416,
  z -576..224 around the same point 0x8062426C as the structures' window; 1 = outside), then its
  model (vtable + 0x74), the held / worn one (+ 0x7C, fn_8001DEB0) and the blob shadow. Widened like
  the structures' window (the position swept along the shadow's footprint, height 60, exact slab
  test against the box); kept = shadow caster.
- **The villagers' own off-screen mode** (the game skips updating them): d_a_npc_outNP's AI tests
  the same window for every player (fn_8002850C with 4, e.g. fn_22_8268) and a villager outside
  it goes into a reduced mode: not shown (+ 0x1C38 = 0, the draw returns at once), no animation,
  its position stepping in 16 / 32-unit grid hops every ~2 s (logged: 1424 -> 1392 -> 1360 ...);
  it is shown again when it walks back into the window. **Not drawn for shadows** (decision):
  showing it would mean changing the AI's state (game logic: where villagers go, when they enter
  houses, meet the player), and the reduced mode has no pose or smooth position, so its shadow
  would hop and freeze. Missing are the shadows of hidden villagers up to one shadow length
  (60 / tan(elevation), ~280 at the 12-degree floor) past the window's edge, where the window is
  narrower than that plus the picture (not measured how much of the window the picture covers).
  So the NPC widening matters for NPCs the AI keeps shown outside the window: visitors (in the
  user-town-clean fixture d_a_npc_sp_roranNP = module 57, model `cml`, wandering 1500-1800 units
  east of the plaza: culled by the draw every frame, kept when within a shadow length of the
  window), event NPCs, gatekeepers.
- **The player**: the player being played is never culled (d_a_playerNP fn_80_E20 draws it
  unless hidden, + 0x2270 bit 6). Another player (online visit) is culled by fn_80_248C, a flat
  window with the module's constants: widened by testing the position moved along the shadow in
  quarter steps; untested (needs a two-instance visit).
- **Kapp'n's bus**: drawn by Kapp'n's draw (d_a_npc_sp_kappeiNP fn_54_638 -> fn_800638A8, the
  extra model, no cull test) whenever Kapp'n is drawn; Kapp'n himself goes through the NPC draw.
- **Trees behind the camera**: the field-object kind table (d_fg_drawNP rodata + 0x48: kind 0 =
  fn_112_17F0 = trees, kind 1 = fn_112_1DA4) is called by fn_112_2184 after its frustum test, and
  the tree draw first tests the line behind the camera (fn_8018AB88: z - k r beyond camera +
  0x364): a tree south of the camera was culled again even when its sphere was kept for its
  shadow. fn_8018AB88 is widened inside a field object's draw like the structures' line test (the
  shadow's tip with the object's sphere diameter as height, when the shadow falls north), the
  object then a caster. **"fn_112_17F0" is not a separate path**: it is this tree draw.
- **Fruits** (the tree draw's loop: each fruit's sphere through fn_802B777C, then its draw,
  nothing else after it): the first pass treated every frustum test inside a field object's draw
  as the object's own, so a culled fruit of a visible tree switched the rest of the tree's draw to
  shadow-only: **a visible fruit after an off-screen one vanished** (bug of the first pass). Now
  the first test is the object's, later ones are fruits: a culled fruit of a visible tree is a
  caster on its own until the next fruit's test; fruits of a tree drawn as a caster are casters
  where their shadow reaches.
- **d_fgobj_managerNP's objects** (fn_111_E4C, object + 0xC position, model at + 0x24): only the
  line behind the camera (fn_8018AB88), never culled at the sides; widened the same way (height
  40), the model inserted as a caster. Untested: the manager drew no object in any route run (no
  such objects on the map those days; `CF_CASTERS_FORCE=obj` exercises the path when there are).
- **A main.dol actor with the frustum test** (fn_80181978, data vtable 0x804F9200, near the
  snowball effect names): bounding sphere of its model (+ 0x138) through fn_802B777C, then the
  model and its blob shadow; widened like field objects, the model a caster. Not met in the routes.
- **Blob shadows of kept objects**: an actor's / structure's / tree's draw registers its round
  blob shadow after its model (fn_801C863C; structures and trees fn_801C874C). For an object kept
  only for its shadow these are skipped (the game culled the blob with it; the blob would show in
  the picture at the screen edge).
- **Structure heights**: the structures' footprint sweep used one height (160) for every
  structure; now each structure's height is the tallest box of the models it inserted (MDL0 info
  block + 0x28 / + 0x34 min / max, model space y; City Folk leaves the "extents" byte clear but
  fills the box), cached per actor (+ its unique id), 160 until its models were seen once. Town:
  obj_house2 216, obj_check1 252, obj_office 188, obj_myhome0 113, obj_nc 91, obj_bbs 81; city:
  obj_tw_grace 293, obj_tw_etc02 217, obj_tw_fountain 63, most other obj_tw_* (shops, backdrop
  buildings, even obj_tw_lamp) exactly 500 (a box the converter seems to have given them; only a
  longer sweep, i.e. a kept caster whose shadow may not reach the view: cheap).
- **The city** (TOWN stage): the city's structures (d_a_str_townNP: shops, buildings, the 12 street
  lamps) use daSTR_c's draw fn_99_2B04 / cull test fn_99_2548 (their vtables point into
  d_a_strNP), so they were already widened by the first pass, and the city's NPCs
  (d_a_npc_town_outNP / _graceNP / _theaterNP; module 75's `elp` on the plaza) use the NPC base
  draw. The city has no field objects. In the plaza route (08:00 and 16:45) 0-1 structures per
  frame were culled at all (the plaza's buildings are all inside the window), the only one kept
  was obj_busstop1, and the A/B pairs differ only by the fountain water and idle animation.
- **Verification** (user-town-clean, the user's settings copy (Enhanced lighting Ultra + world,
  sun shadows), `CF_CASTERS_AB=1`, 60 fps for exact game frames; pairs in the agent scratch
  `shadow-casters-2/`): town morning (07:30) and evening (16:45) walks: shadow bands from trees off
  the right edge in the morning (casters off: none, same frame of two runs), from trees off the
  left edge in the evening. Villager: `CF_CASTERS_NPC_WINDOW=0.25` makes the game cull a villager
  in the picture; without keeping it (first build) its shadow on the bulletin board is gone (5500
  px differ in that region per pair), kept as a caster the villager vanishes and its shadow is
  the normal one, frame after frame while it turns (0-114 px differ). Visitor (Saharah, module
  57): kept every frame it is within a shadow length of the window (`npcs culled 1 kept 1`).
  120 fps (VSync off, the NPC-window aid making the villager switch between caster and visible on
  screen): judder detector 8.94 M tagged draws judged, 0 defects, 0 jumps / switches. Sun shadows
  off (user's settings, `sun_shadows=false`): no `[casters]` line, no actor scope opened, no
  caster-only draw (the hooks call the game's functions unchanged).
- **Cost** (RTX 3090 Ti shared with 3-4 other agents' game instances, the user's settings at 4K
  (scale 3) / 120 fps / VSync off, morning walk route; `CF_CASTERS_AB=1200` in one run so on and off
  alternate every 20 s under the same load, `AURORA_GPU_PROFILE=10 CF_PERF_LOG=10`): Aurora
  caster-only draws 7-23 per frame. In the quiet windows (GPU busy < 5 ms) adjacent off / on:
  GPU busy 2.86 / 2.81 ms, shadow-map atlas pass 0.076 / 0.079 ms, host thread median 1.8 / 1.9
  ms, render worker 1.2 / 1.2 ms, 120.0 fps both; off 3.89 / on 4.37 busy (atlas 0.085 / 0.083)
  as the load returned. The loaded windows (busy 7-12 ms, 57-110 fps) swing with the other
  instances in both states; mean of the windows' host medians off 2.0 / on 2.0 ms.
- **CF_SHOT and CF_CASTERS_AB**: image s<N> shows the scene drawn while the copy count was N-1
  (models inserted at N-2): with `CF_CASTERS_AB=1` the widened images are the EVEN ones for every
  kind (the lag of inserted models is 1 from the start now; it was 0 until the first kept model,
  so runs with only villagers kept had the odd ones).

### Time of day (Aurora 0342 grade)

A colour grade of the 3D scene at scene end (gain, lift, saturation, contrast; the HUD is never
touched; a copy of the resolved scene + one full-screen pass), keyed by the sun's elevation:
noon (neutral, +5 % saturation), golden hour (warm gain, slight lift, +14 % saturation), blue dusk
(cool gain, blue lift), moonlit night (cool, desaturated, more contrast); clouds pull the day toward
a flat grey look. On top of the game's own hourly colours. Strength slider; off indoors.

### Night lights

Lit windows and lamps as warm extra lights (LM's ExtraLight path, interpolated by id): sources by
model name, placed in front of the model at window height (model space offset through its world
matrix), from dusk (sun below +1 degree) to dawn. The structure models of a town (disc archive
`Str/Brres/field<Rgn><Lng>StrBrres<S/W>.arc`): players' houses `obj_myhome0-4`, villagers' houses
`obj_house0-4` (`TN_obj_house*.brres`), Nook's stores `obj_nc` / `obj_nk` / `obj_nw` / `obj_ng`,
Able Sisters `obj_tailor`, town hall `obj_office`, `obj_museum`, `obj_lighthouse`, `obj_windmill`,
`obj_busstop0` (the earlier names obj_shop / obj_townhall / obj_police / obj_cafe do not exist).
Window offsets of obj_nk / obj_nw / obj_ng / obj_lighthouse / obj_windmill are estimates (none
of them is in the user's town).
Shadows for the house / shop / lamp kinds ("Night light shadows", cube maps). Collected from frame
N's draws, submitted with frame N+1 (static sources).

**The game's own lights-on state** (branch enh-light-2; F1 "Follow the game's lights", on): every
structure with lit materials has a `daSTR_c` light controller (d_a_strNP). Its material table
(d_a_strNP .data + 0x140, 17 entries: `m_letter`, `m_light`, `m_board`, `m_special`, `m_bus_ind`,
`m_museum`, `m_spotlight`, `m_window`, `m_tw_light`, `m_tw_light_00..07`) names the materials of a
model that glow; **d_a_strNP fn_99_A40** (hooked, `all`) sets TEV colour 1 of each of them to a
lerp of two colours by the controller's **light amount** (controller + 0, float 0..1; the ScnMdl at
controller + 0x2C, through `ScnMdl::CopiedMatAccess` / `ResMatTevColor::GXSetTevColor`). The
actors decide the amount and call fn_99_A40 only when it changes (and at creation), so the hook
records it per ScnMdl and the night light's intensity is multiplied by it. What the game decides
(USA user-town fixture): players' houses 0 while nobody is inside, villagers' houses 1 or 0 (at
21:30 three of six were dark; that is what the game draws), Nook's Cranny fades 1 -> 0 over ten
frames at 22:00 (closing time; `[window]` log), town hall / Able Sisters / museum / gate / bus 1.
Kinds without a controller (bus stop, lamps) glow from dusk to dawn. Off = the old behaviour
(every structure glows). Screenshots (build/runs/win4 of the branch's worktree): a dark villager
house without / with the old light, lit houses with their light.

**The city** (branch enh-light-3; `cf_light.cpp` "city night lights"). The city's structures
(d_a_str_townNP; models `obj_tw_*` from `Str/Brres/town<Rgn><Lng>StrBrres<S/W>.arc`: the shops,
the street lamps `obj_tw_lamp`, the plaza's buildings, the bus stop `obj_busstop1`) have the same
daSTR_c light controller, so their lights follow the game's own state too (21:30 in the user-town
fixture: GracieGrace, HRA, Shampoodle and Resetti's center 0 = closed and dark, Redd's, Katrina's,
the theatre, the auction house, the ATM, the other buildings and all 12 street lamps 1). The
light positions come from the model's own data instead of a table: the controller names its lit
materials (**controller + 0x14 + i**, s8 material index per entry of the material table, -1 =
none), the ResMdl's DrawOpa / DrawXlu byte code (opcode 4: material, shape, node) gives the
shapes drawn with each, and the shapes' position-array bounding boxes (VtxPos + 0x20 min / + 0x2C
max, turned into model space by the node's bind matrix at ResNode + 0x70; the lamp's glow sphere
is a unit sphere under a node scaled 25 at the lamp head) give the glowing part. A flat part
(a window pane, a sign: thin in x or z) gets a light 14 units in front of it, on the side away
from the model's centre; a compact one (< 40 units: a lamp head) just below it; a larger volume
in front of its +z face; a part spanning most of the building in both horizontal directions
(a facade the game also tints) none. Parts within 40 units merge, at most 3 per model; warm
windows (radius 210, x1.2), lamps (230, x1.3, shadows). The city had more sources than Aurora's
64 lights (the nearest 64 to the camera were submitted); since light-tiles all of them (up to 256) are. Test aid `CF_CITY_LIGHT_LOG=1` (every lit
part with its material entry, size, centre and light position; per model every 600 frames;
lit materials drawn per frame); route `tools/routes/city_plaza_warp.txt` (the plaza and the
shop fronts) with `CF_CLOCK_START="2026-10-27 21:30"`.

**Lit materials are light sources, not receivers** (branch enh-light-3, Aurora 0420): a lit
material at night is the game's glow (TEV colour 1 lerped to the light colour), but the enhanced
world shaders darkened it like any surface: the moon's shadow and terminator and AO took a third
of the glow off the theatre's lit doorway ((185,184,114) plain -> (124,135,113)). The
ResMatTevColor that fn_99_A40 writes (hook on `ResMatTevColor::GXSetTevColor` while it runs) is
recorded with its light amount; when g3d loads that material to draw it
(`G3DState::LoadResMatTevColor`, hooked) the draw gets the material hint
`AURORA_MATERIAL_EMISSIVE` (no directional shadow / terminator, no AO; extra lights still add;
uniform only, no pipeline variants). Applies in the town and the city, by day too where the game
lights a material by day. **The night grade keeps the light sources' colour** (Aurora 0421): the
moonlit grade (cool gain, less saturation) turned warm windows grey-green; bright, saturated pixels
(max channel 0.45 -> 0.8, chroma 0.1 -> 0.3) keep their colour by `0.85 x night`. Doorway after
both: (181,183,121), the plain game's look. Off (master off) nothing changes: the hooks only read
guest memory and call the original.

### Fireflies and fireworks (effect lights)

F1 "Fireflies and fireworks" (under Night lights, on). Both are `nw4r::ef` effects, so the lights
come from the game's own particles (cf_light.cpp `noteEffectDraw`, hook on
`nw4r::ef::DrawOrder::Draw(Effect*, const DrawInfo&)`, which draws an effect's particle managers):

- Layout (USA rev 1; NW4R, so the same on every disc): Effect + 0x90 its ParticleManagers
  (`ut::List`, link offset at list + 0xA); ParticleManager + 0x24 its EmitterResource, whose first
  word points at the emitter's name; + 0x38 its particles (`ut::List` of 0xE4-byte Particles, link
  at + 0x14); + 0x8C the effect's root matrix (`Effect::SetRootMtx` copies it to Effect + 0x54; the
  emitters' world matrix). Particle + 0x20 primary colour (RGBA; the colour animation and the
  alpha fade / blink are applied there every frame), + 0x24 secondary colour, + 0x28 a modulation
  colour (white), + 0xA0 velocity, + 0xAC position (emitter space), + 0xB8 previous position,
  + 0xDC age. Positions were checked against the matrix the billboard strategy loads with
  `GXLoadPosMtxImm` for the quad (identical to 0.1 unit) and a visible firefly glow on screen sat
  exactly where its light's projection put it.
- **Fireflies**: emitters `af_ins_hotaru_glow` + `af_ins_hotaru_glow_child` (one particle each, two
  managers per firefly at one place: merged), `af_hotaru_glow`. One green light per firefly
  (colour from the particle, ~(0.6, 1, 0.5)), radius 110, intensity 1.6 x the glow's alpha (it
  blinks with it). Seen in June at 21:30 in the user-town fixture: 5-10 fireflies around the
  player, along the river.
- **Fireworks** (the August Sunday show, also `af_itm_firework*` items): `af_fld_firework`,
  `_top`, `00`-`02`, `_child`, `_st_child`. The show is a **sky backdrop**: its effects are drawn
  with their own camera (DrawInfo + 0 view matrix: identity rotation, translation (0, 110, -1300)),
  not the town's, so a burst has no place in the town. Its light is placed where the burst appears:
  the backdrop's view-space position taken as the main camera's view space (same projection), i.e.
  up and ahead of the scene; radius 1100-1800 (with the burst's spread), intensity 2.5 x the
  burst's brightness (alpha-weighted particle count / 8), colour = alpha-weighted mean of the
  particles' primary and secondary colours with the hue pushed (bursts are green, orange, teal,
  pale yellow). The game flashes the scene's ambient colour itself during the show; this adds the
  burst's coloured light from above. Effects drawn with the main camera use world positions
  (submitted with the next frame through the main camera, like night lights).
- Budget: at most 32 firefly and 12 firework lights (light-tiles; were 12 and 8; strongest first, sources of one kind closer
  than 25 / 200 units merged), then the shared ranking in enhanced_lighting.cpp into Aurora's 256
  extra lights (64 before light-tiles); no shadows. Outdoors scaled by darkness (0.25 + 0.75 x night), indoors full.
- Ids `0x5A000000 ^ manager address` (stable while the effect lives) for interpolation; backdrop
  lights are view-space fixed, so a camera move between game frames shifts them slightly (not
  checked at 120 fps).
- Test aids: `CF_EFL_LOG=1` (every emitter name with its kind once, per manager every 30 frames:
  particles, brightness, world / backdrop, centre; the lights every 60 frames), `CF_EFL_AS=<emitter
  name prefix>` (that emitter lights like a test firefly: white, radius 400, to check placement on
  an easy effect such as `af_fsh_shadow`), `CF_EFL_AB=1` with `CF_ENHANCED_AB=1` (effect lights
  only in dump frames 4k+1, not 4k+3: neighbouring shots compare them), `CF_WINDOW_LOG=1` (light
  amount changes per structure model, controller call counts, every 1200 frames the list with
  world positions). Routes: `CF_CLOCK_START="2026-06-20 21:30"` (fireflies), `"2026-08-02 20:00"`
  (Sunday: fireworks), `CF_TELEPORT=6600:tortimer` (the plaza during the show),
  `"2026-08-03 21:57"` (Nook's closes at 22:00).
- World vs actor coordinates (for teleports): world x = actor x; the town is a cylinder about X, a
  ground point's world (y, z) = r (sin p, cos p) with p = 0.3526 - (actor z - 697) / 573 (radians,
  fitted in the user-town fixture; r ~ 700). A structure in front of its door: actor z of its centre
  + ~130.

### Furniture lights (branch furniture-lights)

F1 > Visuals > Enhanced lighting > "Furniture lights" (on with the master switch; intensity slider,
"Furniture light shadows"; `[enhanced_lighting] furniture_lights / furniture_light_intensity /
furniture_light_shadows`). Lamps, TVs, computers, stereos, fireplaces, candles, lava lamps, festive
trees, the Nintendo items with glowing parts and so on light the room around them, in the
player's house, villagers' houses and shops (and on visits: the same actors). Code:
`src/game/cf_furniture_light.cpp`, two calls from cf_light.cpp (`onModel` notes furniture models,
the scene begin submits). Off (master or the option): nothing submitted, the two hooks below only
read guest memory after calling the original.

**What the game has** (USA rev 1; hooks `all`):
- Item table (Item/item.arc item.bin, 0x18C-byte records; docs/FEATURES.md "Unused items at
  Nook's"): kind 3 = furniture, +0x171 its action type (01 plain, 02 always animated, 0A candles /
  sprinklers, 0C computer, 1F/20 stereos, 21/22 TVs, ...), **+0x187 >> 6 & 3 the light kind**
  (1: modern lamp, lovely lamp, paper lantern, desk light; 2: the other 24 lamps and candles), read
  by daFTR_c (d_a_ftrNP fn_15_6B8; kind 2 lamps also drive the room-light helper at 0x806244A0).
  Furniture model n = Item/Ftr/Ftr<n>.brres, n = the furniture's ordinal (item number - 0x9C4,
  from 0xE07: - 0xE07 + 718).
- **daFTR_c** (d_a_ftrNP): +0x3A0 the item record, vtable at +0x60 (+0x1B0 is-on, +0x210 the
  model's MDL0), +0x380 the lamp's glow helper. **Glow helpers** (0x18 bytes: +0 amount 0..1,
  +0x14 the ResMatData) are made for materials found by exact name: `lp_m` (fn_15_7BB0; lamps and
  every small light, e.g. the robo series), and an array of `mn_m`, `tv_m`, `wa_m`, `wb_m`, `fr_m`
  (fn_15_774C: displays, screens, water, fire). Their per-frame update **fn_15_767C** fades the
  amount toward on / off and writes it as TEV konst colour 3 alpha into the material, whose first
  stage is `lerp(RASC, RASA, K3.a)`: off = lit by the room, on = full brightness regardless of
  lighting. Hooked: amount per (MDL0, material). **fn_15_3100** (daFTR_c per-frame update) is
  hooked to map each actor's MDL0 (virtual +0x210) to its item record (number, name, light kind).
- Shared furniture textures (fire f1/f2, screen tv.0, glass, water) are in Ftr/roomAlways.arc
  int_common.brres, not in the model's brres.

**Which furniture lights**: the rules of the review scanner (`py tools/research/furniture_lights.py`,
contact sheet with on/off renders into out/furniture_lights, reviewed by the user 2026-11-04):
(a) the helper materials lp_m / mn_m / tv_m / fr_m, (b) a lamp-style konst bypass of the lighting,
additive blending, unlit special materials (not df_* / ev_*: tp_m screens, tk_m, electric_m),
unlit or special materials named after fire / lava / neon, whole models named after fire (hibachi,
fire flower, fire bar, potbelly stove). Water never lights (wa_m / wb_m, and models named pool,
bath, trough, sink, toilet, urinal, sprinkler, Merlion, Manneken Pis). The exceptions table
(item numbers + our numbers only) drops the stove (kitchen range) BE3, propane stove BD5, doghouse
BED and beach table BF2 (name matches / a shine), and adds the tall lantern B5D (a flickering
light in its fire box at 62 % of its height), the glow clock C1A (green, behind its glass) and the
kitschy clock C16 (its unlit face). 86 lights out of 729 furniture models (the sheet's 99 minus
12 water items, 4 dropped, plus 3 added).

**The light** (computed from the loaded model when it is first drawn, cached per MDL0 + item):
the area-weighted centre of the glowing parts (model space, through bones and their bind
matrices), turned with the drawn ScnObj's world matrix every frame (placement, rotation, moving
furniture, room changes); colour = the parts' mean texture colour (texels decoded from the model's
own TEX0 / PLT0, vertex colours), kept saturated (0.35 + 0.65 x normalised), screens 55 % toward
cool blue (0.62, 0.80, 1.0), fire 40 % toward (1.0, 0.62, 0.3); radius 110 + 2.2 x the model's
largest extent (120-320); intensity lamp 1.4 (x 1.15 for light kind 2), fire 1.6, screen 0.8,
lava lamp 0.9, other 1.0; x the game's amount for helper materials (a lamp switched off stops
lighting, fading as the game fades its shade); fire flickers (three host-time sines, per-object
phase) and gets `ExtraLightFire`. Ids `0x46000000 ^ ScnObj` (interpolated by id at 120+).
Budget: up to 48 per frame into enhanced_lighting.cpp's shared ranking (256 lights since light-tiles, 64 before); fires and
screens may cast shadows (they share the night lights' shadow slots, `shadowLightCap`), lamps
never: the light sits inside the shade, which would hide it from the cube map.
(**Superseded by branch indoor-light**, "Indoor lighting" below: physical falloff and new numbers,
~2000 K fire, lamps' shadows from below their shade, flicker in position too. The numbers in this
section, the scanner's `propose()` and `furniture_lights_check.py` are the earlier ones.)

Details: a small flame (fire whose glowing part is under 20 units across: the candle) lights from
the part's top + 2 units, not its centre (the wax around the light hid it from its own cube map);
the glowing material gets Aurora 0420's emissive hint while lit (cf_light.cpp `markLitMaterial`,
as the structures' lit materials), so the moon / AO do not darken a lit lamp shade; models with a
helper material that no helper drives (e.g. the robo series' small lights, never switched) count
as on, as the game draws them (konst alpha 255 from the file); every cache keyed by guest
addresses (analyses, item map, amounts) is cleared at a stage change (the next place reuses the
heap: a villager's blue clock and the next house's brown drum had the same MDL0 address).
The lights are submitted at the scene begin from the furniture drawn the frame before, at the
ScnObjs' current world matrices (computed before the stage draw), so moving furniture has no lag.

**Coverage**: the scanner writes `out/furniture_lights/final_lights.txt`: one line per sheet
candidate, `LIGHT` (kind, model-space position, colour, radius, intensity, flicker, shadows) or
`NO LIGHT` (water / dropped), with the runtime's rules and exceptions (86 lights).
`py tools/research/furniture_lights_check.py <run log>` compares a run's `[furn] model` lines with
the scanner's numbers per item: every item met in the game runs matched (position within 0.05,
colour within 0.006, same radius / intensity); the candle's light is 10 units higher on purpose
(flame tip).

**Checked in the game** (2026-11-04, USA, user-town-clean, 21:00, Enhanced lighting on; sample by
the user's request, the rest through the table above). Shots in out/furniture_lights/game/:
- Player's house: the candle off at arrival (helper amount 0), switched on and off through its
  on byte (actor + 0x409, `CF_POKE`): the game fades its helper 0 -> 1 in ten frames and back, the
  light follows (`[furn] ... amount`), and the game's own room light comes up with it (a light-kind
  2 lamp); with | without (`CF_FURN_AB=1`): warm light on the walls, the box, the player.
- A retro TV, a blue lava lamp and the tall lantern (addition) placed by poking the house's item
  grid before the warp (u16 item codes, rows of 16, 0xFFF1 empty, the layer on top 0x200 after;
  with this fixture the main room's floor row of the box is at 0x90E94B80 (box 0x90E94B98, tape deck
  0x90E94B9E, the row in front + 0x20); codes 0x9000 + item number x 4, + rotation 0-3): lava lamp light on the floor and walls, the TV
  switched on through its glow helper's target (+4 = 1.0) fades in and lights the room cool blue,
  the tall lantern's warm flickering light with its shadows.
- Moving: the candle and its box moved 32 units in x (actor + 0x64 poked): the light moved with
  them (view x -48 -> -16). Rotating: the TV placed with rotation 1: its light (10.7 units in
  front of the screen) turned with it.
- Villager houses 1-4: green lamp (off at 21:00, no light), robo-dresser (always lit), jukebox and
  hi-fi stereo (screens on), lights placed; Nook's Cranny and the museum lobby: no furniture lights
  there that day, nothing broken.
- 120 fps (`target_fps=120`, `CF_INTERP_TEST=2` + `CF_FRAME_DUMP`): the in-between frames carry
  the lights at their place (in-between vs game frame: only the player differs).
- Enhanced lighting off: frame dumps of this branch and main in the house differ only in the
  player (two runs of main differ the same way: the player's idle animation).
- Not done: switching furniture with the real A button by script (the pointer on the TV + A did
  not switch it; the switch paths were driven through the game's own state instead), PAL / JP /
  KR runs (hooks are `all`, d_a_ftrNP by hash), visits online.

Test aids `CF_FURN_LOG=1`: every model analysed (`[furn] model ...` kind, centre, colour, radius,
intensity), every furniture actor with its position and vtable (`[furn] actor ... item ...`), the
lights every 120 frames; `=2` also every helper amount change. `CF_FURN_AB=1`: furniture lights
only in dump frames 4k+1, not 4k+3. Routes: `tools/routes/house_furniture_warp.txt` (house),
`house_tv_warp.txt`, `furniture_places_warp.txt` (villager houses, Nook's, museum); the on byte /
helper target / item grid pokes are in the route comments and above.

### Up to 256 lights: light tiles (branch light-tiles, Aurora 0520)

Aurora's extra lights went from 64 to **256** (`MaxExtraLights`), so the city's night lights, the
furniture lights, fireflies, fireworks and the sun / moon all fit in the shared ranking at once.
Every pixel of an enhanced world draw used to loop over all extra lights (64 x the reach test per
pixel, whether a light could reach it or not); 256 that way would have cost four times as much.

**How it works** (Aurora `enhanced.cpp` "Light tiles", `gx/shader.cpp` `enh_extra_lights`):
- **A compute pass per EFB pass** ("Enhanced light tiles", queued as a pre-pass op right after
  the pass's mask job, so it runs before the pass's draws on the GPU): one 8 x 8 workgroup per
  32 x 32 px screen tile. It takes the tile's depth range from the **depth pre-pass** (the mask's
  `maskDepthR`, the frame's opaque world draws; the tile's texels plus one around it, since the
  pre-pass is at mask resolution), then its 64 threads test the lights' spheres (in turns of 64) against
  the tile's four frustum side planes (from the draw camera's projection and viewport) and the
  depth range, and set their bits in three 256-bit masks per tile:
  - list 0: within the tile's pre-pass z range (+ 4 units + 2 %): draws that are in the pre-pass
    (opaque, depth-writing world draws: the same test as the pre-pass's own),
  - list 1: in front of the tile's farthest pre-pass surface: other depth-tested draws (water,
    particles, glass),
  - list 2: the side planes only: draws without a depth test; all three lists are plain 2D when the
    frame has no pre-pass (neither light shadows nor AO).
  Directional lights (sun, moon) are in every list.
- **The shader** visits its list's set bits in index order (`firstTrailingBit`) and runs the old
  loop body on each (`enh_xl_one`, same additions in the same order). A light the test drops cannot
  reach any pixel of the tile (the reach test would have skipped it), so the sum is unchanged.
- **Where it lives**: in the mask buffer after the mask planes (the fragment stage already uses
  both of its storage buffers, and the mask buffer is at WebGPU's 128 MiB binding limit): a header
  word with the job's stamp, then 24 words per tile (0.8 MB at 4K). The draw's uniform carries the
  stamp; a draw whose pass never got its job (a pass that ended without a flush), whose camera
  differs from the job's (the first tiled draw of a pass sets it) or that is drawn after the light
  set changed within the pass sees no match and runs the full loop, as before. Tiles from 4 lights
  on; offscreen passes and orthographic draws never use them.
- **FIFO**: the light count is a u16 now (it was a u8; src/recomp/interp.cpp parses it).
- **Port side**: enhanced_lighting.cpp's ranking keeps the 256 strongest (it kept 64); shadow slots
  are unchanged (the sun / moon's slot plus the night lights' cap by preset, `shadowLightCap`).
  cf_light.cpp: night-light sources collected 128 -> 256, all of them submitted (was the nearest
  64), fireflies 12 -> 32, fireworks 8 -> 12. Furniture lights keep their 48.
- **Interpolation**: lights are still paired by id per replay (any count), and every replay
  (in-between frame) runs its own tile job with the blended lights.

**Results** (2026-11-09, USA, user-town-clean, 21:30 in October, Enhanced lighting Ultra + world
lights, RTX 3090 Ti; the PC was shared with 2-7 other agents' game instances and Dolphin, which
moved the GPU numbers of whole runs by up to 2x, so the reliable comparison is **within one run**:
`AURORA_ENH_TILES_AB=900` switches tiles on / off every 15 s, 5 s `AURORA_GPU_PROFILE` windows
without a switch are taken per state; agent scratch `light-tiles/gpuab.py`):

| 4K (3840x2160 window, 60 fps), EFB pass ms per frame (GPU busy) | tiles | full loop |
|---|---|---|
| City plaza at night, 25 lights (street lamps, shop windows, moon) | 3.98 (8.33) | 4.83 (9.00) |
| Player's house + 64 test lights, radius 60 | 1.81 (3.27) | 2.93 (4.28) |
| Player's house + 200 test lights, radius 60 | 3.55 (5.26) | 8.42 (9.98) |
| Player's house + 200 test lights, radius 150 (each reaches most of the room) | 15.1 (17.3) | 14.7 (17.1) |

The tile pass itself: 0.04-0.08 ms at 4K (8160 tiles; 0.34 ms with 16 px tiles and 16 x 16
workgroups, hence 32 px). The full loop of this branch is the old loop for <= 64 lights. Back-to-back
runs of main vs the branch agree in the direction but not reliably in size (house + 60 test lights:
EFB 4.35 main vs 3.24 branch; another pair under heavier load had them the other way round). A
light that really reaches a pixel still costs what it did: 200 big overlapping lights are as
expensive as before; what the tiles remove is the cost of the lights that cannot reach.
- **Same picture**: a frozen city frame (24 lights, `CF_FREEZE_AT=6500`, 120 fps presentation,
  `AURORA_ENH_TILES_AB=40`, 40 `CF_FREEZE_SHOTS` across the on / off turns, with the pre-pass depth
  lists in use): all 40 shots byte-identical.
- **Enhanced lighting off**: main vs branch, 16 `CF_SHOT` frames from the title through Rover to the
  town: identical except the two frames that also differ between two runs of main (title leaves,
  Rover's animation timing).
- **The city at night**: the plaza and streets (`city_warp.txt`, `city_plaza_warp.txt`) never had
  more than 24 night-light sources at once (the game draws only the structures in view; `[night]`
  lines), so main's nearest-64 cut never applied there and both builds light it alike; fireflies
  (June 20 21:30 at the house): 5 firefly lights, the same in both. With more than 64 lights the
  difference shows with `CF_TEST_LIGHTS=200,60`: main lights the room with 64 of them, the branch
  with all 200 (agent scratch `light-tiles/out/house_200_*.png`, also the heat map).
- 0 lost tile jobs in every run (`AURORA_ENH_TILE_LOG`): every pass that drew with tiles got its job,
  always with the pre-pass depth outdoors and indoors (sun / moon / room key light shadows).

Test aids: `AURORA_ENH_TILES=0` (always the full loop, A/B), `AURORA_ENH_TILE_MIN=<n>` (tiles from
n lights, default 4), `AURORA_ENH_TILE_SHOW=1` (lights visited per pixel as a heat tint: blue few,
green ~16, red 32+), `AURORA_ENH_TILE_LOG=<n>` (`light tiles: N jobs (M with pre-pass depth), L
lost`, light count and grid, every n frames), `AURORA_ENH_TILES_AB=<n>` (tiles on / off in turns of
n recorded frames, logged), `CF_FREEZE_SHOTS=<dir>,<every>,<count>` (with `CF_FREEZE_AT`: shots of
the held frame while the host keeps presenting it; with `AURORA_ENH_TILES_AB` the same frame both
ways), `CF_TEST_LIGHTS=<count>[,<radius>[,<intensity>]]` (now up to 256; City Folk has no player
position for them, so they are scattered through the view at fixed pseudo-random places 250-550
units in front of the camera (the depth of the player's house room), drifting slowly, without
shadows: a many-lights stress test).

### Indoor places (branch enh-light-2)

Checked with A/B shots (user-town fixture, `CF_TELEPORT` in front of the door, then walking in):
town hall (Pelly), museum (Blathers' hall), Able Sisters (Mabel) besides the house, Nook's and
Rover's bus from enh-light: `[sky] indoors` in each (d_bgi_drawNP linked, d_bgo_drawNP not), the
room key light's soft shadows and AO, no sun / grade / night lights, nothing broken. Not reached:
the city (bus), the museum's other rooms, the café.

### Indoor lighting (branch indoor-light, Aurora 0480)

User report 2026-11-04 (Ultra, indoor / furniture / extra-light shadows on): "indoors I'm not
wowed", "no shadows indoors at all", "lights too strong, not realistic", "the candle simply lights
the whole room, no colour or fire". Outdoors is unchanged by everything below (no flag, class or
hook acts outdoors; the shader paths are exact identities without the new flags).

**What was wrong** (user-town-clean house, `GLOWDBG=8/9` shadow-mask views, `CF_ROOMLIGHT_LOG`):
- *The room key light was a shadow caster, but its shadows could not be seen.* It took the game's
  GX key light's direction, which in the house is nearly horizontal from the left and toward the
  camera (view (-0.85, 0.11, 0.51)): the left wall shadowed the whole room evenly and the furniture's
  shadows fell behind it, and it darkened by a tenth (tint 0.90 = strength 0.6 x 0.6 x 0.28).
- *Every caster cast, the room shell included*: floor, walls and the front wall the camera looks
  through (culled in the picture, not in a shadow map: the shadow pass draws with culling off), so
  any light from the side or the front left the room in a wall's shadow.
- *Furniture point lights* had shadow slots (fires, screens; lamps never), and the mask applies to
  indoor materials, but a point light's shadow only removes that light's own contribution, which
  was small (wide smooth falloff `(1-x^2)^2 / (1 + 8x^2)` over 120-320 units, intensity ~1.6): the
  shadows had nothing to remove. Nothing else was disabled indoors (sun, grade, shafts and night
  lights are outdoor-only by design).
- *The candle lights the whole room*: that is the game itself. **dEnvAmbLight_c** (fn_80197E1C, its
  per-frame update; vtable 0x804FAA68 after its type name; next to dFixAmbLight_c,
  dFixAmbSwitchLight_c, dSubLightRoom_c) moves the room-light helper at 0x806244A0 (+0 amount)
  toward the "a light-kind 2 lamp is on" byte at r13 - 0x2C90 (set by d_a_ftrNP) and writes
  `this + 4 = lerp((0,0,0,255), colour at r13 - 0x56AC, amount)`, a colour added to the room's
  ambient: at night it goes 000000 -> EFEFBD over ten frames when the candle is lit (log), i.e. the
  whole room at almost full brightness.
- *No colour*: the candle's light colour was (1.00, 0.85, 0.72) (60 % of the wax+flame texture mean,
  40 % toward warm), and the game's white whole-room brightening swamped it; the flicker (±20 %
  intensity of a small contribution) could not show.

**What it does now**
- **Room key light** (cf_light.cpp submitSky, indoors): our own direction in the room's frame
  (camera right / forward snapped to the world axes, world Y up: rooms are level and axis-aligned,
  so the light does not swing as the camera follows the player). By day a window light from the
  left and a little from the front, ~53 degrees up (the room camera looks down at ~35 degrees:
  shadows falling back would hide behind their casters, these fall to the right, in sight); at night
  the room's ceiling light, ~75 degrees up. Neutral soft tint 1 - k (1 - (0.58, 0.58, 0.64)),
  k = strength x (0.95 night .. 1.2 day) (0.70-0.76 at the user's 0.6), no terminator darkening
  (`away` 0.1), softer at night. Aurora 0480 **ExtraLightSpareExtra**: its tint only darkens the
  share of the pixel's light that is not the furniture lights' (a lit candle is not dimmed by the
  window light's shadow).
- **The room shell casts no shadow** (Aurora 0480 **AURORA_DRAW_CLASS_WORLD_NOCAST**: same
  pipelines as WORLD, in the depth pre-pass, not a caster). Indoors every world draw of the scene is
  NOCAST by default (cflight::roomShellMode at the scene begin); a g3d object turns it off around its
  draw (cf_tags.cpp objectScope) when `castsIndoors`: a model (not a ScnProc without one), not the
  room itself (`idr_*`, e.g. `idr_myroom4` = the house's main room: floor, walls, windows; also any
  model over 240 units across), not `sun_*` (the game's window light shafts and floor patches), not
  a translucent pass (G3dProc task 7). Found on the way:
  - cf_light.cpp's model-name cache (per ScnObj address) was never invalidated: indoors the room
    model reused the title screen's object address and was still called `idr_player_select`, so the
    room cast a shadow over half of itself. Indoors an entry is now checked against the object (the
    MDL0 pointer still at its offset); outdoors unchanged (open: the same staleness may exist there).
  - Aurora (generic): a class change that does not change the pipeline let the next draw be merged
    into the last recorded caster command, so NOCAST geometry was drawn into the shadow maps with
    it; the NOCAST switch now dirties the pipeline.
- **Furniture lights** (cf_furniture_light.cpp): Aurora 0480 **ExtraLightPhysical** falloff
  `win^2 / (1 + x^2 / c^2)`, x = d / reach, c = core share in `box[0]`, win = 1 - x^4: inverse square
  from a small source, a bright pool and a faint tail to the reach. Numbers: lamp 3.2 (core 0.10),
  screen 1.8 (0.14), fire 3.4 (0.09), candle (a small flame) 4.4 (0.07, reach 340), neon / lava 2.0,
  reach 260 + 2 x model size (280-460); exceptions' old numbers x 1.5 reach, x 2 intensity.
  Colours: fire ~2000 K, 75 % toward (1.0, 0.55, 0.20) (the candle is (1.00, 0.66, 0.40) before
  Aurora's saturation), screens keep 70 % of their picture's colour. Flames flicker in intensity
  (slow sway + flutter + rare dips) and move ±1.6 units with the flame, so their shadows waver too
  (host time, interpolated by id; `CF_FURN_FLICKER=0` steady). Shadows for fires, screens and real
  lamps (item light kind 1 / 2): a lamp's light (and shadow origin) goes 2 units below its shade's
  lower rim; small lit parts on furniture bodies (robo series) neither move nor cast. A flame's lit
  material gets Aurora 0480 **AURORA_MATERIAL_EMISSIVE_GLOW** (emissive + a bloom source of its
  own: re-drawn into the bloom's glow buffer like additive draws).
- **The game's whole-room brightening** (hook `fn_80197E1C all`): with furniture lights on, indoors,
  while a lit light-kind 2 lamp / candle of ours is in view, `this + 4` is scaled to F1 "Room fill
  from lamps" (`[enhanced_lighting] furniture_room_fill`, default 0.25; eased over ~10 frames):
  the rest of the light comes from the lamp. Rooms with no lamp lit keep the game's ambient
  exactly; a room brightened by a lamp we do not light (out of view, not analysed) keeps it too.
- AO stays off by default (ambient-only option).

**Second pass** (user review of the first: "better, take it further": the candle should light only
its corner in flickering red / orange, the room still mostly lit; LM's punchy, contrasty look; lights
must add up, colours blend):
- "Room fill from lamps" default **0**: while one of our light-kind 2 lamps / candles is lit, the
  game's whole-room brightening is gone and our lights light the room; rooms with no lamp on keep
  the game's look exactly (the night room stays at the game's dark night ambient).
- **Lights add up**: every light's contribution is summed (Aurora's light loop, no per-light
  normalisation, no cap besides the 64-light set: furniture submits up to 48; branch light-tiles
  raises the set). One candle lights its corner; more lamps progressively light the room.
  Aurora **0481**: the physical lights' sum is added to the lit channel **hue-preserving** (scaled
  so that no channel passes 1, the GX channel's range, instead of each channel clipping on its own):
  an orange pool stays orange where red saturates (it went yellow-white), and a red and a blue pool
  overlapping add to purple (the sum is taken first, so colours blend; nothing picks the strongest
  light's colour). This happens in the game's own colour space (GX lighting has no linear stage);
  the other extra lights (night lights, outdoors) are unchanged (`enh_xl_phys` 0 there = identity).
- Numbers: candle reach 170 (core 0.09, intensity 5.0, colour (1.0, 0.36, 0.08) ~1700-1900 K),
  other fires 380 + size (1.0, 0.45, 0.12), the tall lantern 260; lamps warm but softer
  (40 % texture + 60 % (1.0, 0.86, 0.66), a third of the vibrance), reach 340 + size, 3.4; screens 85 % picture colour,
  reach 230 + size, core 0.16; neon / lava 220 + size. F1 **"Light vibrance"** (`furniture_vibrance`,
  1.6): HSV saturation x vibrance, up to full, hue and brightest channel kept (LM's "Light color
  vibrance" idea; Aurora's global `extra_light_saturation` still applies on top).
- **Lamps cast no shadow again and light from their glowing part's centre**: the first pass's
  origin 2 units below the shade's lower rim sat inside the lamp (the regal lamp's table top hid
  most of its light; the lovely lamp's glowing material spans its base, so its light went to the
  floor). Lamps 5.0 (core 0.14, reach 360 + size); a coloured shade (saturation > 0.2) gets the full
  vibrance, plain warm lamps a third of it; lava / neon lamps at least 60 % saturated (3.6, reach
  260 + size). Lamp shadows: open (needs a shadow origin outside the lamp's own geometry, or the
  lamp excluded from its own shadow map).
- **Flicker**: smoothed value noise at ~1.3 / 4 / 9 Hz, +-19 % with occasional gutters to -35 %
  (not sines: those read as a pulse), position jitter +-1.4 units from noise too.
- Test scenes (routes): `indoor_light_lamps1.txt` / `indoor_light_lamps3.txt` (the candle plus one
  / three regal lamps placed in the back row of the house's item grid before the warp, switched with `FTR`, which
  now switches every furniture of that item within 30 frames), `indoor_light_redblue.txt` (the
  lovely lamp and the blue lava lamp side by side).

**Third pass** (user: "on the right track, but much bolder colours, a lot more colour and contrast"):
- **The light's hue** comes from the glowing part's texels weighted by their chroma (chroma^2): the
  plain mean let white / grey parts wash the lovely lamp's pink shade out to salmon (1.0, 0.60,
  0.52 -> now 1.0, 0.37, 0.57) and the blue lava lamp to near white (0.89, 0.93, 1.0 -> 0.24, 0.50,
  1.0). A lamp with a coloured shade (saturation > 0.25) keeps 85 % of its colour (plain lamps 40 %,
  the rest warm white). "Light vibrance" default **2.4** (was 1.6).
- **Brighter cores, steeper falloff**: lamps 6.5 (core 0.12), fires 6.0 (0.09), screens 3.4 (0.14),
  lava / neon 5.0 (0.10), the candle 7.0 (core 0.08, reach 180).
- **A darker room around the lamps**: indoors at night while one of our lamps / candles is lit,
  the game's own GX ambient colours and GX light colours are scaled by F1 **"Room ambient under
  lamps"** (`furniture_night_ambient`, 0.4) inside the enhanced scene (hle_gx.cpp GXSetChanAmbColor /
  GXLoadLightObjImm, `enh::gameLightScale`), eased over ~15 frames; with it the indoor grade gets
  saturation x 1.3 and contrast x 1.18 (Aurora 0342's grade, off indoors otherwise). By day and in
  rooms with no lamp lit nothing changes. (No Aurora change: 0480 / 0481 / 0520 as before.)
- Measured (4K, user's settings, 21:00, the lovely lamp + blue lava lamp scene, mean of a floor
  patch): pink pool hue 312 sat 1.00, blue corner hue 253 sat 0.97, the blend between them hue 284
  (purple) sat 0.98; pass 2 had the room near the game's beige. Open: characters between the lights
  stay dark (the matte character response x 0.85 of the light; a coloured rim light for them would
  need a shader change in the 0520 light loop).
- **True 4K shots**: `CF_RENDER_SCALE=4.7369` (docs/CONTROLS.md "Test aids"), or `--window 3840x2160`
  with `resolution_scale=0` (run.ps1 `-ExtraArgs`).

**Checked (first pass, before the user's review; 2026-11-09 after merging main d8f7225)**: A/B
runs with the user's settings, new vs `CF_INDOOR_LEGACY=1`, same route and frames: the house by
day (the player's and the box's key-light shadows fall to the right), at night with the candle off
/ on (a warm pool on the box and the back wall, the rest of the room at the game's night look plus
a quarter of its brightening), a retro TV on (cool pool in front of it) and TV + candle; villager
houses by night (a robo-dresser's green light now local; a room the game brightens for a lamp we do
not light keeps the game's look) and by day, Nook's and the museum lobby (no change). Shadow masks
(`AURORA_ENH_GLOWDBG=8`) before / after. Enhanced lighting off: the house frame is byte-identical to
main c789775. Outdoors: the town at 16:50 (Enhanced world off), new vs legacy in one build: only the
player's idle animation and the water differ; before the merge the branch matched main c789775 too
(falling leaves only). Cost (quiet PC, 2026-11-05, house at night, 1280x720 offscreen): GPU busy
3.3-4.3 ms per frame vs 3.8-3.9 legacy; 120 fps target: 120.0 presented. (Runs on 2026-11-09 shared
the PC with other agents' instances: their perf numbers are not usable.) Flicker at 120 fps: the
flame lights are computed per game frame from host time and blended by id in between (no step at
in-betweens); not measured with a judder detector.

**Test aids**: `CF_INDOOR_LEGACY=1` (everything above as before, for A/B runs: the old key light,
furniture numbers and falloff, the full room brightening, the shell casting), `CF_INDOOR_AB=<n>`
(legacy in alternate blocks of n frames: fine for point lights, but the directional cascades do
not follow a per-frame alternation: compare separate runs), `CF_INDOOR_DIR=<right>,<up>,<toward
camera>` (key light direction), `CF_INDOOR_SHADOW=<distance>,<caster reach>`, `CF_ROOM_SHELL=0`
(the shell casts), `CF_ROOM_SHELL_LOG=1` (each object: casts / room background), `CF_CAST_ONLY=<name
prefix>` (only those objects cast indoors), `CF_NOCAST_ALL=1` (no world draw casts),
`CF_ROOMLIGHT_LOG=1` (`[roomlight]` the game's room-light colour before -> after), `CF_ROOMLIGHT_FILL=<f>`.
Route actions (any route): `FTR=<item hex>/<0|1>` switches that furniture through its own switch
(daFTR_c virtual +0x1C4 (this, on, 0), what touching it calls; the on byte +0x409 is not reliable
for the candle: it read 0x32 later in the visit), `POKE=<hex addr>/<hex byte>`. Routes
`tools/routes/indoor_light_house.txt` (candle off / on / off / on), `indoor_light_tv.txt` (the
tape deck turned into a retro TV, switched through its glow helper's target, then the candle).

### Other versions (branch enh-light-2)

Dev builds per version (`tools\build.ps1 -Game <id>`), the user-town save with its title folder
renamed (docs/SETUP.md), continue route:

| Check (2026-11-03) | USA | PAL | Japan | Korea |
|---|---|---|---|---|
| Light-controller hook (`[window]`: 31 structure models with their light amounts) | yes | yes | yes | yes |
| Fireflies (June 21:30: `af_ins_hotaru_glow*` found, 5-10 firefly lights) | yes (shots) | yes (shot) | yes | yes |
| Fireworks show (Aug 2 20:00 at the plaza: backdrop bursts, firework lights) | yes (shots) | yes | yes | yes |
| Nook's Cranny closing at 22:00 (light amount 1 -> 0) | yes | - | - | - |
| Indoors (town hall, museum, Able Sisters) | yes (shots) | - | - | - |

Korea's continue route needs its own choice-box positions (`@0.44/0.14`, `@0.56/0.33`). The town in
the save is a USA town, so its events (the August fireworks) follow the US calendar on every disc.

### Bloom and sun shafts (Aurora 0342 shafts)

Bloom is LM's (bright scene pixels + emissive draws). **Sun shafts**: single scattering of the
directional light along each view ray at mask resolution (12 / 20 / 32 steps by preset), every
step tested against its cascades, so the shadows of trees and houses cut darker shafts into lit
haze; Henyey-Greenstein forward scattering; strongest at the golden hour, faint at noon, a little
under the moon, none in rain. Added by the scene-end composite.

### GPU cost (2026-11-02)

`AURORA_GPU_PROFILE=20`, RTX 3090 Ti, town at 16:50 (golden hour: shadows, shafts, grade all
active), High preset, F1 render scale 2 (~1707x960, "1080p class") and 4 (~3413x1920, "4K
class"). **Load**: three other agents' game instances ran at the same time (game ~50 fps instead
of 60; GPU numbers per frame are still per-pass timestamps, but take them as approximate).

| Pass (ms per frame) | scale 2 | scale 4 |
|---|---|---|
| Plain renderer, whole frame (master off) | 0.38-0.46 | 0.42-0.52 |
| Whole frame, enhanced (High) | 4.0-5.7 | 9.6-12.2 |
| EFB pass (enhanced world shaders: sun/AO mask taps, lights) | 2.31 (plain 0.17) | 8.28 (plain 0.42) |
| Mask copies to the storage buffer (3 planes) | 1.17 | 1.63 |
| Sun cascades (shadow-map atlas, 4 x 1024^2) | 0.23 | 0.24 |
| Depth pre-pass | 0.09 | 0.13 |
| Shadow mask (cascade lookup, PCSS) | 0.06 | 0.12 |
| Sun shafts (20 steps, mask resolution) | 0.08 | 0.18 |
| Ambient occlusion (mask + 2 denoise + blur) | 0.18 | 0.43 |
| Bloom (bright + 5 down + 5 up) | 0.07 | 0.12 |
| Colour grade (copy + pass) | 0.01 | 0.04 |

The main cost is the enhanced variant of the world shaders in the EFB pass (two 16-tap mask
reads per pixel: sun shadow and AO, plus the light loop) and the mask copies; the new passes
themselves (cascades, shafts, grade) are well under 0.5 ms together. Open: a cheaper mask read
(the 4x4 depth-matched tent per pixel), fewer mask copies when only the sun casts.

### Screenshots

Test runs from the user-town fixture (continue.txt, frames 4800/4801 with `CF_ENHANCED_AB=1`:
even = plain, odd = enhanced): day 10:00 (shadows of the player, fence and mailbox, AO), golden
hour 16:50 (long warm shadows, golden grade, shafts), dusk 17:45 (moon rising, blue grade), night
21:00 (moonlight shadows, cool grade, lit house front), rain (state 3: shadows almost gone,
flatter), snow (Jan 15, state 5), indoors (Rover's bus stage: the room key light, no sun/grade).
Not compared with Dolphin (these are enhancements: the plain frames are the reference and stay
byte-identical with the master off).

Branch enh-light-2 (agent scratch, not in the repo; plain | enhanced pairs unless noted): a dark
villager house at 21:30 with "Follow the game's lights" on (no light) and off (the old warm light
on a house the game shows dark), lit villager houses with their light; fireworks at the plaza
(Aug 2 20:00, `CF_EFL_AB`: with | without the burst's light, orange flash on the plaza); a firefly
by a tree (with | without: the leaves beside it turn green); a firefly glow on screen exactly at its
light's projection; town hall / museum / Able Sisters interiors; a PAL firefly scene (date shown
20/6). An enhanced-off frame was not pixel-compared with main: two runs are not frame-identical
(the HUD appears at different frames), so "off = original" rests on the hooks only reading guest
memory and calling the original.
