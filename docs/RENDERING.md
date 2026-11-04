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

All saved-town routes start with `continue_any.txt` (A with the pointer away, so every Rover
question takes its first answer, whatever the load timing) and time their walk from the arrival
(`after link d_bgo_drawNP 2`); continue.txt's pointer clicks land in another dialogue branch when
"Preparing saved data" takes longer than usual. Not covered yet: town hall / post office, Able
Sisters, the city (bus at the gate), villager houses, festivals.

### Numbers (2026-11-02)

- Recorded so far: town day/night/rain/snow, house, Nook's, new_game: **300 configs** (6.7 KB
  zstd). Per route a fresh cache met 135-186 configs.
- Warm start, 2400p/120 fps settings, 238 pipelines (list + derived variants): **hold 0.17 s**
  (1.5 s after startup). Cold hold of the same set on the loaded PC: ~26 s (476 shaders compiled
  on 8 threads); a quiet-PC cold measurement is pending (cold runs held, see above).
- Coverage: a fully warm cache of one nook run still missed 9-12 configs in the next nook run
  (Rover's dialogue, the shop): see "Vertex-format variants". The fresh-cache verification pass
  (every route with the list, target ~0 misses) is pending a quiet window.

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
| Seasonal particles | on, density 1 | petals Apr 1-8, leaves Oct 17 - Nov 25, snowflakes Nov 26 - Feb 24 (hemisphere option: +6 months); outdoors only |
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
  flower / house materials). The city's plaza materials are not classified yet (not reached by a
  route).
- **Camera**: the main ScnRoot's current camera (u8 [root + 0xF4], CameraData at root + 0xF8 +
  0x10C n; its world -> view MTX34 comes first; the camera position / target fields there are 0 in
  City Folk) goes into Aurora's application XF memory (0x0700) as a plain XF load noted for
  interpolation as a position matrix (tag scope 0xCF57F000), so in-between frames get the
  in-between camera. **The town is z-up** in g3d's world: the camera's up row leans to +z and its
  forward to -z (e.g. rows (1,0,0) (0,0.73,0.68) (0,-0.68,0.73)); the acres' model matrices are the
  view x a translation, the "rolling log" curve is a small rotation about x per acre (seen 7 deg).
  The port picks the world's up axis per frame among the six axes (the one the camera's up points
  along and its forward away from, with hysteresis) and sends view x B, B rotating that axis to +y,
  so the effects' "world" is always y-up. The projection is the main view's own (Hor+ included),
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

- The sea and rivers were not reached by a route (only the pond by the house); the city's plaza
  materials are not classified (no route to the city yet).
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
  (0, 0.53, 0.85)). So "up" for the sun is the local ground normal (`sLocalUp`: mean Y axis of the
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
- Budget: at most 12 firefly and 8 firework lights (strongest first, sources of one kind closer
  than 25 / 200 units merged), then the shared ranking in enhanced_lighting.cpp into Aurora's 64
  extra lights; no shadows. Outdoors scaled by darkness (0.25 + 0.75 x night), indoors full.
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

### Indoor places (branch enh-light-2)

Checked with A/B shots (user-town fixture, `CF_TELEPORT` in front of the door, then walking in):
town hall (Pelly), museum (Blathers' hall), Able Sisters (Mabel) besides the house, Nook's and
Rover's bus from enh-light: `[sky] indoors` in each (d_bgi_drawNP linked, d_bgo_drawNP not), the
room key light's soft shadows and AO, no sun / grade / night lights, nothing broken. Not reached:
the city (bus), the museum's other rooms, the café.

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
