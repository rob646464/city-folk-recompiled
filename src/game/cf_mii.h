#pragma once
// Mii maker (F1 > Miis; docs/MIIS.md): edit, import and export the Miis of the emulated console's
// Mii database, with a preview drawn by the game's own RFL library from the disc's Mii parts.
#include "recomp/ctx.h"

namespace recomp::cf {

// Guest thread, once per presented frame (cf_game.cpp): hands saved changes to the game's RFL
// database in memory when it is safe, the Shampoodle hint.
void miiOnGuestFrame(Ctx& c);
// F1 > Miis (menu.cpp). True when a setting changed (the console's Mii system ID).
bool miiMenu();
// Host, every frame (cf_game.cpp drawOverlay): the Shampoodle hint, CF_MII_AT test aid.
void miiDrawOverlay();

}  // namespace recomp::cf
