#pragma once
// City Folk enhanced lighting provider (src/game/cf_light.cpp; docs/RENDERING.md "Enhanced
// lighting (City Folk)"): the scene bracket, the main camera, the sun and moon, time-of-day
// grading, night lights.
#include "recomp/ctx.h"

namespace recomp::cflight {
// nw4r::lyt::Pane::Draw of a root pane (cf_hud.cpp): the 2D starts.
void onLayoutDraw(Ctx& c);
// nw4r::g3d::ScnRoot::DrawOpa / DrawXlu (cf_tags.cpp), before (begin) and after the original.
// Returns false when the pass is to be skipped (test aid CF_SCENE_SKIP).
bool onRootDraw(Ctx& c, bool xlu, bool begin);
// G3DState::SetCameraProjMtx (cf_tags.cpp): camera data (MTX34 view, then the projection).
void onCamera(Ctx& c, u32 cameraData);
}  // namespace recomp::cflight
