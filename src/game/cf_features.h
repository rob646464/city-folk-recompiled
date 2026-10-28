#pragma once
// City Folk gameplay features (F1 > Enhancements > Gameplay; docs/FEATURES.md) and the test aids
// used to build them. Called from cf_game.cpp (frame loop) and menu.cpp (the Gameplay tab).
#include "recomp/ctx.h"

namespace recomp::cf {

// cf_devaids.cpp: CF_FF fast-forward windows, CF_MEM_DUMP, CF_CLOCK_JUMP (docs/FEATURES.md
// "Test aids"). True: the frame is not shown (fast-forward).
bool devOnGuestFrame(Ctx& c);
double devRetraceSpeed();

// cf_grass.cpp: grass wear (Original / New Leaf style / Off).
void grassOnFrame(Ctx& c);
void grassMenu(bool& dirty);

// cf_skin.cpp: skin tone of the player being played.
void skinOnFrame(Ctx& c);
void skinMenu(bool& dirty);

}  // namespace recomp::cf
