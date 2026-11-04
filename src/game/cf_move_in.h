#pragma once
// Move in from Wild World (docs/MOVE_IN.md): Rover's "Move from the DS!" with a Wild World save
// file instead of a DS. cf_move_in.cpp.
#include "recomp/ctx.h"

namespace recomp::movein {
// The "Move in from Wild World" window (host thread, every frame; drawn when open).
void drawWindow();
// The window is up: game input waits (like the F1 menu).
bool windowOpen();
// F1 > Enhancements > Gameplay section; true when a setting changed.
bool menuSection();
// Per guest frame (cf_game.cpp): the CF_OPEN_MENU test aid.
void onGuestFrame(Ctx& c, u32 frame);
}  // namespace recomp::movein
