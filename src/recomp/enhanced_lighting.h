#pragma once
// Enhanced lighting, port side (docs/RENDERING.md "Enhanced lighting"; Aurora patch 0016,
// include/aurora/enhanced.h and include/aurora/lights.h).
//
// - Settings (F1 > Visuals > Enhanced lighting) are pushed to Aurora every host frame.
// - Draw classes: hle_gx.cpp marks world-geometry draws (perspective, full viewport, outside the
//   shadow-map pass), ghosts (the actor ghost pass) and everything else, only while enabled.
// - The main 3D view (fn_8000BB98) is bracketed: extra lights are sent before it, the
//   scene-end marker (bloom, volumetric beam, ambient occlusion) after it, before the HUD.
// - Extra lights: a provider (e.g. ghosts) calls submitExtraLights once per game frame.
#include <aurora/lights.h>

#include <cstdint>

namespace recomp {

// Extra dynamic lights for the current game frame (guest thread). Positions are in VIEW space
// (GX eye space: the space of GXLoadLightObjImm positions; see enh::worldToView). The set is
// sent with the next main-view draw and replaces the previous one; a frame in which nobody
// submits has no extra lights. Up to aurora::gfx::MaxExtraLights (strongest first). Lights
// are blended between game frames by `id` when frame interpolation is on.
void submitExtraLights(const aurora::gfx::ExtraLight* lights, uint32_t count);

namespace enh {
// Master switch as last applied (host and guest thread; cheap).
bool active();
// Master switch only (active() is also false on the plain frames of CF_ENHANCED_AB).
bool masterOn();
// Master switch as configured (settings or CF_ENHANCED), before applySettings runs: the
// accuracy options that enhanced lighting turns off (hle_gx.cpp) are decided from it.
bool configuredOn();
// CF_ENH_PERF_AB phase: -1 off, 0 part on, 1 part off.
int perfPhase();
// F1 settings -> Aurora (host thread, every frame).
void applySettings();
// Ghost actor pass (fn_80060E70 pass 0x30): draws in it are class GHOST.
void setGhostPass(bool on);
bool ghostPass();
// Actor draw pass (fn_80060E70 other than the ghost layer): Luigi, NPCs and other characters get
// the matte character material (GX_AURORA_SET_MATERIAL_HINT): the game's own vertex lighting, no
// glossy highlights, soft wrap-around response to the colored lights.
void setCharacterPass(bool on);
bool characterPass();
// A light-emitting material is being drawn (City Folk: a structure material the game's light
// controller has lit, cf_light.cpp): Aurora's emissive hint (0420), no sun / moon shadow and no
// AO on it. Reset by the provider before other draws.
void setEmissive(bool on, bool glow = false);  // glow: also a bloom source (a flame; Aurora 0480)
// Shadow-map caster pass (shadows.cpp: fn_8005D984 .. fn_8005D8CC): draws in it are class OTHER.
void setCasterPass(bool on);
// Branch shadow-casters (docs/RENDERING.md "Shadows of objects outside the view"): the following
// draws are shadow casters only (Aurora 0470 AURORA_DRAW_CLASS_SHADOW: shadow maps, nothing else).
// Only inside the scene bracket, outside the caster pass; reset by the caller after the object.
void setShadowOnly(bool on);
// Branch indoor-light: the following draws are a room's shell (floor, walls): world draws that
// receive light and shadows but cast none (Aurora 0480 AURORA_DRAW_CLASS_WORLD_NOCAST), so a window
// light over a wall does not put the room in that wall's shadow. Reset by the caller after the model.
void setNoCast(bool on);
bool noCast();
// Branch indoor-light: the following draws are a character (1) or furniture (2): Aurora 0520a's
// rim-light material hints (shaded as world draws otherwise). 0 = none. Reset by the caller.
void setRimMaterial(int kind);
int rimMaterial();
bool casterPass();  // inside setCasterPass(true) (City Folk: the stage's second root pass)
bool inScene();     // inside beginScene .. endScene
// Main view bracket (fn_8000BB98 hook): sends the frame's extra lights / the scene-end marker.
void beginScene();
void endScene();
// World (game) space -> view space with the current camera (lbl_803A3820 view eye/target).
// False if there is no camera yet.
bool worldToView(const float world[3], float view[3]);
// Room (index into the room table lbl_804D8170: the index the game's room visibility, room
// objects and actor room bytes use) whose bounds contain a world position, -1 if none.
int roomAt(const float world[3]);
// Limits a light (view-space position already set) to a room's bounds (ExtraLightRoomBox,
// Aurora 0187): no light through walls into the next room. `world` = the light's world position.
// No-op for room < 0 or a room without sane bounds.
void limitToRoom(aurora::gfx::ExtraLight& l, int room, const float world[3]);
// Enhanced "Shadow map size": width of the offscreen shadow map (height 3/4 of it), 0 = the
// EFB (render resolution).
uint32_t shadowMapWidth();
// Colored-light shadows: how many lights may cast them this frame (settings; screen-space at
// most 4; 0 when off). Providers flag that many of their strongest lights.
uint32_t shadowLightCap();

// Realistic flashlight (Aurora 0196): a ghost's AI got the "in the flashlight beam" message
// (EnThought message 2). The first one after a pause makes the beam flare, as the light stuns it.
void onGhostInBeam(uint32_t actor);

// Time-of-day look of this game frame (cf_light.cpp; Aurora 0342 grade, 0343 shafts): applied with
// the next applySettings. gain/lift per channel, saturation/contrast 1 = unchanged; shafts =
// sun-shaft brightness (0 off).
void setGrade(const float gain[3], const float lift[3], float saturation, float contrast);
void setShafts(float intensity);
// 0..1: bright coloured pixels (light sources) keep their colour through the grade (Aurora 0421).
void setGradeKeepLights(float amount);
// A GX light object was loaded (GXLoadLightObjImm HLE, guest thread): Aurora's GXLightObj layout
// (color u32, a0 a1 a2 k0 k1 k2, pos xyz, dir xyz; view space as loaded), light id mask.
void noteGameLight(const float* obj, uint32_t idMask);
// Branch indoor-light: the game's own lighting (GX ambient colours and GX light colours) is scaled
// by this inside the enhanced scene (1 = unchanged; cf_light.cpp sets it indoors at night while our
// lamps light the room, so their pools stand out of a darker room). gameLightScale() is 1 outside
// the scene, in the caster pass and with enhanced lighting off.
void setGameLightScale(float s);
float gameLightScale();
struct GameLight {
    float pos[3], dir[3];
    float a[3], k[3];
    uint8_t rgba[4];
    uint32_t idMask;
    uint32_t frame;  // copyDispCount() at the load
};
// The lights loaded in the current game frame so far (guest thread).
const GameLight* gameLights(uint32_t& count);
}  // namespace enh

}  // namespace recomp
