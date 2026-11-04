#pragma once
// Furniture lights for Enhanced lighting (src/game/cf_furniture_light.cpp; docs/RENDERING.md
// "Furniture lights"). Called from the enhanced-lighting provider (cf_light.cpp).
#include "recomp/ctx.h"

#include <string>

namespace recomp::cffurn {
// Enhanced lighting on and F1 "Furniture lights" on.
bool enabled();
// A g3d scene model drawn in the stage's main pass (cf_light.cpp onModel): furniture models
// (int_*) are noted with their ResMdl (MDL0) for this frame.
void noteModel(u32 scnObj, u32 mdl0, const std::string& name);
// Scene begin (main camera known): the lights of the furniture drawn last frame, at their current
// world matrices. `view` = world -> view (MTX34).
void submit(const float* view, bool viewValid);
}  // namespace recomp::cffurn
