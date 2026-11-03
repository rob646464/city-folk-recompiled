# Rendering (City Folk)

General rendering status and the GX work are in docs/WII.md ("Rendering status"). This file holds
the enhancement passes on top of the accurate renderer.

## Enhancement passes

Order of the optional passes in a frame (agents adding passes: keep this list current; the
post-process chain of Aurora 0006 runs on the finished picture, everything else here is part of
the 3D scene, before the HUD):

1. **EFB pass prologue (enhanced lighting, Aurora 0016 / 0028 / 0224 / 0340)**: before every EFB
   pass with opaque world draws: depth (+ normal) pre-pass, shadow-map atlas (sun / moon cascades,
   night-light cube maps), shadow + AO mask, sun shafts (0342, from the same atlas). Read by the
   world shaders of that pass (forward shadows and AO).
2. The game's own 3D (stage passes, d_s_stageNP fn_125_C0), with the enhanced world shaders.
3. **Scene end (GXSceneEndAurora, enhanced lighting)**, at the first layout pane after the stage's
   first root pass: colour grade (0342, time of day), then the composite (bloom, sun shafts).
4. The 2D (lyt HUD, dialogue) — never graded or bloomed.
5. Post-process chain (Aurora 0006: CRT filter etc.) on the whole picture.

Water / weather / atmosphere effects (branch enh-world) go between 2 and 3 or into 5; coordinate
here.

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
model name (obj_myhome*, obj_house*, obj_shop*, obj_museum*, obj_police*, obj_townhall*,
obj_busstop*, obj_lamp* / obj_light*, obj_cafe*), placed in front of the model at window height
(model space offset through its world matrix), from dusk (sun below +1 degree) to dawn. Shadows
for the house / shop / lamp kinds ("Night light shadows", cube maps). Collected from frame N's
draws, submitted with frame N+1 (static sources). **Open**: whether a villager's window is really
lit (the game's own window state is not read: every house glows at night); fireflies and
fireworks are not light sources yet (their particle emitters are not identified); only names seen
in the user's town were verified (obj_myhome0, obj_house0/2, obj_busstop0).

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
