# Rendering notes (City Folk)

Rendering status of the accurate base: docs/WII.md "Rendering status". This file collects the
optional enhancement passes layered on top. LM's docs/RENDERING.md is the reference for the shared
Aurora machinery (post-process chain 0005/0006, enhanced lighting 0016+, depth snapshots).

## Enhancement passes

Every enhancement that changes the 3D picture runs at one point of the frame: after the game's main
g3d scene (the first `nw4r::g3d::ScnRoot` DrawOpa/DrawXlu pass and the `nw4r::ef` effects after it)
and before the HUD (the first `nw4r::lyt` root pane, `cf_hud.cpp`). Order (shared by the branches
that add passes; keep it when adding one):

1. **Enhanced lighting** (branch enh-light, `GXSceneEndAurora`, Aurora `enhanced.cpp`): lights,
   shadows, AO, bloom, sun rays. Emitted first at the layout point (`cflight::onLayoutDraw`).
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
| Water reflections | on, strength 0.8, medium (24 steps), ripple distortion 1 | rivers, ponds, the sea reflect the scene (screen-space march) and the sky + moon where the ray leaves the picture; the game's own water pattern bends it |
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
  0x10C n: world -> view MTX34 first, camera target at + 0x8C) goes into Aurora's application XF
  memory (0x0700 view, 0x070C view x translate(target)) as plain XF loads noted for interpolation
  as position matrices (tag scope 0xCF57F000), so in-between frames get the in-between camera; the
  projection is the main view's own (Hor+ included), taken by Aurora from the classified draws.
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
  keyed to the game's 60 Hz.

### Test aids

`CF_WFX=1` (on for one run), `CF_WFX_SET="key=value;..."` (`[enhanced_world]` keys, e.g.
`grade=1;tilt_shift=1`), `CF_WFX_FREEZE=1` (effect time fixed, deterministic shots), `CF_WFX_AB=1`
(effects on odd guest frames only: consecutive shots compare with / without), `CF_WFX_LOG=1`
(camera, weather and amounts every 120 frames), `CF_WFX_MAT_LOG=1`, `AURORA_WORLD_FX_LOG=1`.
Weather and season: `CF_WEATHER_FAKE=rain|heavyrain|snow|clear`, `CF_CLOCK_START` (docs/CALENDAR.md,
tools/routes/run.ps1).
