// The Wii Remote pointer over the whole window in ultrawide (docs/CONTROLS.md "Pointer in
// ultrawide"). With widescreen wider than the game's 16:9 and F1 > Visuals "Keep HUD at 16:9" off,
// cf_hud.cpp moves the clock and the memo / camera buttons to the window edges by their panes'
// global matrices, which the game's own hit tests read. The pointer reaches the whole window there
// (ws::pointerRangeX() = 1 / factor in KPAD x, wiimote.cpp), so the game's hit tests find those
// panes where they are drawn and its cursor is drawn wherever the mouse is. What stopped it was the
// game's "pointer on the screen" tests, which take a pointer past the 16:9 frame as pointing away
// (no toolbar, no memo button, no walking to the pointer). Those tests are widened here, in x only
// and only by the pointer range (1 everywhere else, so nothing changes at 16:9 / 4:3 or with the
// HUD kept at 16:9):
//   - fn_8017FF4C(): |x| <= 1.05 and |y| <= 1.075 of the KPAD position from fn_800FA6AC (the
//     game's copy of the Remote's pointer); the field HUD (toolbar, memo button) and the other
//     pointer users ask it. Run with the x it reads divided by the range.
//   - d_a_playerNP fn_80_2DB28(player, f1 = x limit, f2 = y limit, u8* state): the player's
//     "pointer inside these limits" test before walking to the pointer: f1 times the range.
// USA rev 1 names; "all" in tools/recomp/hooks.txt maps them to every version.
//
// Test aid CF_POINTER_LOG=1: each change of fn_8017FF4C's answer with the pointer it saw.
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/widescreen.h"

#include <cstdlib>

namespace recomp {
namespace {

int sNarrowing = 0;  // inside fn_8017FF4C with a range > 1: fn_800FA6AC reports x / range
float sRange = 1.0f;

const bool sLog = [] {
    const char* v = std::getenv("CF_POINTER_LOG");
    return v && *v && *v != '0';
}();

}  // namespace

// fn_800FA6AC(Vec2* out): the game's KPAD pointer position.
HLE(fn_800FA6AC) {
    const u32 out = c.r[3];
    callOriginal(c, "fn_800FA6AC");
    if (sNarrowing > 0 && out) wrf32(out, rdf32(out) / sRange);
}

// fn_8017FF4C() -> BOOL: the pointer is on the screen.
HLE(fn_8017FF4C) {
    const float range = ws::pointerRangeX();
    if (range > 1.0f) {
        sRange = range;
        sNarrowing++;
        callOriginal(c, "fn_8017FF4C");
        sNarrowing--;
    } else {
        callOriginal(c, "fn_8017FF4C");
    }
    if (sLog) {
        static u32 sLast = 2;
        if (c.r[3] != sLast) {
            sLast = c.r[3];
            log("[pointer] on screen %u (x range %.3f)", c.r[3], range);
        }
    }
}

// d_a_playerNP fn_80_2DB28(player, x limit, y limit, state*): pointer inside the limits.
static void playerPointerTest(Ctx& c) {
    const float range = ws::pointerRangeX();
    if (range > 1.0f) c.f[1] = double(float(c.f[1]) * range);
    callOriginal(c, "d_a_playerNP::fn_80_2DB28");
}
static HleReg sPlayerPointerReg("d_a_playerNP::fn_80_2DB28", &playerPointerTest);

}  // namespace recomp
