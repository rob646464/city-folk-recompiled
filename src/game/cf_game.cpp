// Animal Crossing: City Folk: the game side of recomp/game.h. Nothing game-specific is hooked
// yet (boot work, docs/WII.md "Order of work"); these defaults leave the game untouched.
#include "recomp/game.h"
#include "game/cf_region.h"

#include <atomic>
#include <cmath>

namespace recomp::vi {
double fieldSeconds();  // hle_vi.cpp
}

namespace recomp::game {
namespace {
std::atomic<u32> sFrames{0};
}  // namespace

bool onGuestFrame(Ctx&) {
    region::onGuestFrame(sFrames.fetch_add(1, std::memory_order_relaxed) + 1);
    return false;
}
u32 guestFrames() { return sFrames.load(std::memory_order_relaxed); }
void filterPad(PADStatus*, int) {}
bool padHeld() { return false; }
double retraceSpeed() { return 1.0; }
float audioGain() { return 1.0f; }
bool audioMirrored() { return false; }
// One GXCopyDisp per retrace in every scene measured (title, Rover's intro, bus, keyboards,
// town, town map; docs/WII.md "Frame rate"): 60 fps, 50 for a European game at 50 Hz.
int nativeFps() { return int(std::lround(1.0 / vi::fieldSeconds())); }
bool unadjustedPass() { return false; }
bool splitScreen() { return false; }
bool textBoxShown() { return false; }
int textMode() { return 0; }
bool viewerOpen() { return false; }
bool enhViewMatrix(float*) { return false; }
bool enhPlayerPos(float*) { return false; }
void enhBeginScene(Ctx&) {}
bool enhRoomBounds(const float*, float*, float*) { return false; }
void drawOverlay() {}
void debugMenu() {}
s32 overlayEntry(const char*) { return -1; }
bool isOverlayEntry(s32) { return false; }
u32 overlayLength(s32) { return 0; }
s32 overlayRead(s32, void*, s32, s32) { return -1; }
void onBoot(Ctx&) {}
void onFrame() {}

}  // namespace recomp::game

// Game-side hooks the generic runtime declares in its own headers (LM: skip.cpp, shadows.cpp,
// tags.cpp).
namespace recomp {
namespace gx {
bool g_discardDraws = false;  // gx_bridge.h: the frame being drawn is not shown (fast-forward)
}
namespace interp {
// viewMatrixSource: cf_tags.cpp (the current g3d camera).
namespace detail {
bool trackPoints(float out[9]) {
    (void)out;
    return false;
}  // interp.h: path check (CF_TRACK_LOG)
}
}  // namespace interp
}  // namespace recomp
