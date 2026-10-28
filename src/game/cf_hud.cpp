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
#include "port/settings.h"
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
bool hudElement(int depth, u32 pane) {
    if (depth < 1 || depth > 2) return false;
    const std::string name = paneName(pane);
    if (name == "N_all") {
        const std::string child = firstChildName(pane);
        return child == "timeFrm_00" || child == "N_tbBtn_00";
    }
    return name == "N_ichichousei";
}
int anchorFor(int depth, u32 pane, float tx) {
    if (sAnchoredAt >= 0 || !hudElement(depth, pane)) return 0;
    return tx < 0.0f ? -1 : 1;
}

// Anchoring moves the element itself (default): its panes' global matrices are shifted right after
// the layout computes them, so the game's pointer hit tests (which read the panes' global
// matrices) find the buttons where they are drawn. CF_HUD_ANCHOR=picture: the old way (only the
// drawing moves, through a ws::AnchorScope; clicks stay at the 16:9 places).
const bool sMoveMatrices = [] {
    const char* v = std::getenv("CF_HUD_ANCHOR");
    return !(v && std::strcmp(v, "picture") == 0);
}();
int sCalcDepth = 0;
int sShiftedAt = -1;  // depth of the pane whose subtree is being shifted (CalculateMtx), -1 none

bool anchoringOn() { return ws::active() && !port::settings().video.hudKeep4x3; }

// The layout's width in its own units, from the DrawInfo's view rectangle (+0x34 left, +0x3C right).
float layoutWidth(u32 drawInfo) {
    const float w = rdf32(drawInfo + 0x3C) - rdf32(drawInfo + 0x34);
    return w > 16.0f && w < 4096.0f ? w : 608.0f;
}

void shiftSubtree(u32 pane, float dx, int guard = 0) {
    if (!pane || guard > 32) return;
    wrf32(pane + kGlbTx, rdf32(pane + kGlbTx) + dx);
    const u32 head = pane + kChildren;
    for (u32 node = rd32(head), n = 0; node && node != head && n < 512; node = rd32(node), n++)
        shiftSubtree(node - 4, dx, guard + 1);
}

}  // namespace

// Pane::CalculateMtx(const DrawInfo&) computes the pane's global matrix and then its children's.
// For an anchored HUD element, once the whole subtree is computed it is moved to its screen edge:
// dx = (1/f - 1) x half the layout width (f = ws::factor(): the 16:9 frame's share of the window).
HLE(CalculateMtx__Q34nw4r3lyt4PaneFRCQ34nw4r3lyt8DrawInfo) {
    const u32 pane = c.r[3], info = c.r[4];
    const int depth = sCalcDepth;
    // (Descendants of an element being moved are not elements of their own: moved with it.)
    const bool a = sMoveMatrices && sShiftedAt < 0 && anchoringOn() && hudElement(depth, pane);
    sCalcDepth = depth + 1;
    if (a) sShiftedAt = depth;
    callOriginal(c, "CalculateMtx__Q34nw4r3lyt4PaneFRCQ34nw4r3lyt8DrawInfo");
    sCalcDepth = depth;
    if (a) {
        sShiftedAt = -1;
        // The side from the pane's own place (computed just now), so an element sliding in from
        // off screen goes to its edge.
        const int side = rdf32(pane + kGlbTx) < 0.0f ? -1 : 1;
        if (side != 0) {
            const float f = ws::factor();
            const float dx = float(side) * (1.0f / f - 1.0f) * 0.5f * layoutWidth(info);
            shiftSubtree(pane, dx);
            static bool sLogged = false;
            if (!sLogged) {
                sLogged = true;
                log("[hud] anchoring by pane matrices: %s moved %.1f layout units (layout width %.0f, factor %.3f)",
                    paneName(pane).c_str(), dx, layoutWidth(info), f);
            }
        }
    }
}

HLE(Draw__Q34nw4r3lyt4PaneFRCQ34nw4r3lyt8DrawInfo) {
    const u32 pane = c.r[3];
    const int depth = sDepth;
    if (depth == 0) {
        sRootLr = c.lr;
        sRootPane = pane;
    }
    const float tx = rdf32(pane + kGlbTx);
    std::optional<ws::AnchorScope> scope;
    if (ws::active() && !sMoveMatrices)
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
