// City Folk HUD anchoring in windows wider than the game's 16:9 (F1 > Visuals "Keep HUD at
// 16:9" off; docs/WII.md "Widescreen"; LM's per-element anchoring, ws::AnchorScope). The game's
// 2D is NW4R lyt layouts; every pane is drawn by nw4r::lyt::Pane::Draw (0x8029E168), which
// draws the pane and then its children (virtual calls, which come back here). The hook tracks
// the pane tree depth and runs the HUD elements' panes inside a ws::AnchorScope.
//
// Pane fields (USA rev 1, from Pane::Draw / Pane::LoadMtx): +0x00 vtable, +0x14 children list
// (nodes at child + 4), +0x84 global matrix (MTX34; x translation +0x90, y +0xA0, layout units:
// origin at the screen centre, y up), +0xBA alpha, +0xBB flags (bit 0 visible), +0xBC name.
//
// Limitation: only the picture moves. The game hit-tests the pointer against the panes'
// own positions, so an anchored button (the memo button) is clicked where it would be at 16:9.
//
// Test aid CF_LYT_LOG=<first>,<last>: every visible pane drawn in those guest frames (depth,
// name, position, the caller of the layout's root draw).
#include "game/cf_features.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/widescreen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace recomp {
namespace {

constexpr u32 kName = 0xBC, kGlbTx = 0x90, kGlbTy = 0xA0, kChildren = 0x14;

struct LogRange {
    u32 first = 1, last = 0;
};
const LogRange sLog = [] {
    LogRange r;
    if (const char* v = std::getenv("CF_LYT_LOG")) std::sscanf(v, "%u,%u", &r.first, &r.last);
    return r;
}();
bool logging() {
    const u32 f = game::guestFrames();
    return f >= sLog.first && f <= sLog.last;
}

std::string paneName(u32 pane) {
    char buf[17] = {};
    for (u32 i = 0; i < 16; i++) {
        const u8 ch = u8(rd8(pane + kName + i));
        if (ch == 0) break;
        buf[i] = (ch >= 0x20 && ch < 0x7F) ? char(ch) : '?';
    }
    return buf;
}
std::string firstChildName(u32 pane) {
    const u32 head = pane + kChildren, node = rd32(head);
    return node && node != head ? paneName(node - 4) : std::string();
}

int sDepth = 0;        // pane tree depth in the current layout draw (root = 0)
u32 sRootLr = 0;       // who drew the root pane
u32 sRootPane = 0;
int sAnchoredAt = -1;  // depth of the pane whose scope is open, -1 none

// HUD elements placed near a screen edge, recognised by their panes' names (a position rule
// also caught the keyboards, the town map and the toolbar's menu variant, which have off-centre
// groups of their own): the clock layout's N_all (first child timeFrm_00, at -217,116), the
// memo button layout's N_all (first child N_tbBtn_00, at 274,0) and the camera adjust icon
// N_ichichousei (274,-171, under a container at the origin). The side is taken from the pane's
// x, so an element sliding in from off screen moves with its edge.
int anchorFor(int depth, u32 pane, float tx) {
    if (sAnchoredAt >= 0 || depth < 1 || depth > 2) return 0;
    const std::string name = paneName(pane);
    bool hud = false;
    if (name == "N_all") {
        const std::string child = firstChildName(pane);
        hud = child == "timeFrm_00" || child == "N_tbBtn_00";
    } else {
        hud = name == "N_ichichousei";
    }
    if (!hud) return 0;
    return tx < 0.0f ? -1 : 1;
}

}  // namespace

HLE(Draw__Q34nw4r3lyt4PaneFRCQ34nw4r3lyt8DrawInfo) {
    const u32 pane = c.r[3];
    const int depth = sDepth;
    if (depth == 0) {
        sRootLr = c.lr;
        sRootPane = pane;
        cf::navLayoutDrawn(c, pane);  // menu items for the pointer snap (cf_menu_nav.cpp)
    }
    const float tx = rdf32(pane + kGlbTx);
    std::optional<ws::AnchorScope> scope;
    if (ws::active())
        if (const int a = anchorFor(depth, pane, tx); a != 0) {
            scope.emplace(a);
            sAnchoredAt = depth;
        }
    if (sLog.last && logging() && (rd8(pane + 0xBB) & 1))
        log("[lyt] f%u %*s%s (%08X) at %.1f,%.1f alpha %u root %s lr %08X%s", game::guestFrames(), depth * 2, "",
            paneName(pane).c_str(), pane, tx, rdf32(pane + kGlbTy), rd8(pane + 0xBA), paneName(sRootPane).c_str(),
            sRootLr, sAnchoredAt == depth ? " anchored" : "");
    sDepth = depth + 1;
    {
        // Frame interpolation: each pane is a 2D element of its own (cf_tags.cpp).
        interp::TagScope tag(interp::TagCat::Hud, pane);
        callOriginal(c, "Draw__Q34nw4r3lyt4PaneFRCQ34nw4r3lyt8DrawInfo");
    }
    sDepth = depth;
    if (sAnchoredAt == depth) sAnchoredAt = -1;
}

}  // namespace recomp
