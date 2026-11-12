#pragma once
// City Folk enhanced lighting provider (src/game/cf_light.cpp; docs/RENDERING.md "Enhanced
// lighting (City Folk)"): the scene bracket, the main camera, the sun and moon, time-of-day
// grading, night lights.
#include "recomp/ctx.h"

#include <string>

namespace recomp::cflight {
// nw4r::lyt::Pane::Draw of a root pane (cf_hud.cpp): the 2D starts.
void onLayoutDraw(Ctx& c);
// nw4r::g3d::ScnRoot::DrawOpa / DrawXlu (cf_tags.cpp), before (begin) and after the original.
// Returns false when the pass is to be skipped (test aid CF_SCENE_SKIP).
bool onRootDraw(Ctx& c, bool xlu, bool begin);
// G3DState::SetCameraProjMtx (cf_tags.cpp): camera data (MTX34 view, then the projection).
void onCamera(Ctx& c, u32 cameraData);
// G3dProc of a g3d scene model (cf_tags.cpp): night-light sources by model name.
bool onModel(Ctx& c, u32 scnObj);  // false: do not draw (blob shadows hidden)
// Indoors: world draws outside g3d objects (the room background: floor, walls) cast no shadow
// (branch indoor-light); false outdoors or with enhanced lighting off.
bool roomShellMode();
bool hasModel(u32 scnObj);      // a g3d object with a ResMdl (MDL0)
bool isLightModel(u32 scnObj);  // a model of light itself (sun_*: a window's light shafts / floor patch)
// Indoors: a g3d object that casts shadows (a model that is not light itself; ScnProcs without a
// model are the room background).
bool castsIndoors(u32 scnObj, u32 task);
// Rim-light kind of a g3d object (branch indoor-light): 1 a character (a model name without '_', up
// to 8 characters: the player 'boy' / 'girl', villagers' species codes), 2 furniture ('int_*'),
// 0 anything else or with enhanced lighting / the rim light off.
int rimKind(u32 scnObj);  // task: the G3dProc task (6 opa, 7 xlu: translucent parts never cast)
// Test aids CF_INDOOR_LEGACY=1 / CF_INDOOR_AB=<n>: indoor lighting as before branch indoor-light
// (always / in every other block of n frames, for same-frame A/B shots).
bool indoorLegacy();
// A furniture glow material's TEV colour data with its light amount (cf_furniture_light.cpp):
// drawn with the emissive hint while lit (Aurora 0420), like the structures' lit materials.
// glow: a flame (branch indoor-light): also a bloom source of its own (Aurora 0480 EMISSIVE_GLOW).
void markLitMaterial(u32 tevColorData, float amount, bool glow = false);
// The town's "rolling log" (branch interp-town): the point where the view's forward ray meets the
// log's ground, from a cylinder fitted to the placed objects (g3d world; false until fitted, or
// indoors). wantGroundFit(): keep fitting this frame even with enhanced lighting off.
bool groundAhead(const float view[12], float out[3]);
void wantGroundFit();
// The fitted log: its axis (parallel to world X through y = cy, z = cz) and the ground radius.
bool logAxis(float& cy, float& cz, float& radius);
// The main camera of the last scene (world -> view, MTX34); false before the first one.
bool mainView(float view[12]);
// A g3d scene object's model name ("" if none / not a model).
std::string modelName(u32 scnObj);
// The same, checked against the object first (a stale cache entry is refreshed).
std::string modelNameChecked(u32 scnObj);
// Test aid CF_MODEL_TRACK=<prefix> (route action TP=mdl<prefix>, cf_region.cpp): the drawn scene
// object whose model name starts with `prefix` nearest the player's model ('boy' / 'girl'): its
// world x, its angle about the log's axis from the player (south positive) and the player's radius
// there. The town's g3d world is a fixed cylinder about X that the player walks around: the field's
// z is the angle times (the player's radius - the player's field height). False: none drawn in the
// last frames, no player model, or no log fit yet.
bool trackedModel(const char* prefix, float& x, float& angle, float& playerRadius, std::string& name, bool logCandidates = false);
// Shadows of objects outside the view (src/game/cf_shadow_casters.cpp, docs/RENDERING.md "Shadows
// of objects outside the view"): the sun / moon shadow light of this scene in the g3d world
// (direction toward the light, unit), the same in the ground's frame (east, up, south), the sine
// of its elevation over the ground in view, and how far its shadows are computed (view depth).
// Called by submitSky while sun shadows are cast.
void castersSetSun(const float dirWorld[3], const float dirLocal[3], float sinEl, float reach, u32 frame);
}  // namespace recomp::cflight

namespace recomp::cfcast {
// d_fg_drawNP fn_112_2184 (cf_tags.cpp): a field object's visibility test and draw.
void fgBegin();
void fgEnd();
// G3dProc of a g3d scene object (cf_tags.cpp): 0 as the game does, 1 a shadow caster only (the
// object was culled, but its shadow reaches into the view), 2 not drawn (culled as the game does).
int g3dDrawMode(u32 scnObj, u32 task);
void shadowOnlyBegin();
void shadowOnlyEnd();
}  // namespace recomp::cfcast
