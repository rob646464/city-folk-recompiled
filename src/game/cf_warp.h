#pragma once
// Warps to any stage (town, rooms, city) through the game's own stage change (cf_warp.cpp,
// docs/WARPS.md).
#include "recomp/ctx.h"

#include <string>

namespace recomp::warp {

// Game thread, once per guest frame: CF_WARP / route WARP= / menu requests, CF_WARP_DUMP.
void onGuestFrame(Ctx& c, u32 frame);
// Route token WARP=<target> ('/' for ','), from the script action queue (cf_region.cpp).
void requestFromScript(const std::string& target);
// F1 > Enhancements > Debug.
void debugMenu();

}  // namespace recomp::warp
