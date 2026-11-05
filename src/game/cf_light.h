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
// A furniture glow material's TEV colour data with its light amount (cf_furniture_light.cpp):
// drawn with the emissive hint while lit (Aurora 0420), like the structures' lit materials.
void markLitMaterial(u32 tevColorData, float amount);
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
