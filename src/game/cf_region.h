#pragma once
#include "recomp/ctx.h"
// Content from other regions (F1 > Enhancements > Gameplay; docs/REGION_CONTENT.md
// "Content from other regions in the port"). cf_region.cpp.

namespace recomp::region {
// Draws the "Content from other regions" section of the Gameplay tab; returns true when a
// setting changed (the menu then saves settings.ini).
bool menuSection();
// Once per guest frame (cf_game.cpp onGuestFrame): test aids.
void onGuestFrame(u32 frame);

// Disc overlay (recomp/game.h overlay*): field structure archives with another region's
// villager houses.
s32 overlayEntry(const char* path);
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 len, s32 off);
}  // namespace recomp::region
