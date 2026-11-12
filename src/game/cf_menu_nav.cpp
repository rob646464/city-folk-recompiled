// Menu items for "Snap pointer to menu items" (recomp/menu_nav.h; docs/CONTROLS.md "Snap pointer
// to menu items"). City Folk's menus are pointer-only; this file finds, every guest frame, the
// regions the game itself tests the pointer against, so the D-pad / stick can jump between them.
// Nothing here changes what the game does: the hooks call the original and then read its data.
//
// How the game hit-tests the pointer (USA rev 1):
//   1. Most screens (HUD toolbar, town map, pockets, dialogue choices, shops, ...): Bounding panes
//      whose name starts with 'B' ("B_msg_00", "B_tb_03", "B_mapBtn_08"; "BC.."/"Bc.." = circles).
//      fn_802B5598(layout, test, filter, data) walks the layout's pane tree; the test fn_80007878
//      checks a pane (visible chain, name 'B', inverse global matrix, Pane rect * data.scale)
//      against data = {x, y, scale, hit pane}; the filters fn_80007A44 / fn_80007B14 pick the
//      circle / rectangle panes. fn_80008ADC (and REL menus directly) call it, the hit pane's name
//      number is the button index.
//   2. Keyboards (name / letters) use the game's copy of the HOME Menu's gui library: a
//      gui::PaneManager with one PaneComponent per pane; gui::Manager::update fn_80314AF4
//      (manager, chan, x, y, trig, hold, release, data) tests the components (contain
//      fn_803158DC: pane at +0x9C). Trigger targets (the keys) have byte +0x90 set (vtable +0x50).
//      Managers of screens that are not up get x = y = 1000.
//   3. The HOME Menu: homebutton::gui::Manager::update, components with their trigger-target byte
//      (+0x24) set; PaneComponent pane at +0x2C.
// Layout coordinates: origin at the screen centre, y up; the scale from the KPAD pointer is
// calibrated per source from the tested point (walks: x * 304, y * -228; keyboards at 16:9:
// x * 416), so it follows whatever mapping the screen uses (aspect ratio, widescreen).
//
// Test aids: CF_NAV_LOG=1 (item sets as they appear, moves; recomp/menu_nav.cpp), CF_NAV_DUMP=1
// (every hit test source and its items, every 30 frames).
#include "game/cf_features.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/version.h"
#include "recomp/menu_nav.h"
#include "recomp/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace recomp {
namespace {

// USA rev 1 addresses, mapped to the running version by VA() (src/recomp/version.h).
#define kHitTestPane VA(0x80007878)   // the 'B' pane test of the walk
#define kListGetNext VA(0x8022A514)   // nw4r::ut::List_GetNext
#define kGuiContain VA(0x803158DC)    // game gui::PaneComponent::contain
#define kHbmListGetNth VA(0x802DF948) // nw4hbm::ut::List_GetNth (HOME Menu)
#define kBoundingVtable VA(0x805231B0) // nw4r::lyt::Bounding

const bool sDump = [] {
    const char* v = std::getenv("CF_NAV_DUMP");
    return v && *v && *v != '0';
}();

// Pane fields (nw4r::lyt::Pane; nw4hbm's is the same): +0x0C parent, +0x14 children list (nodes
// at child + 4), +0x4C size, +0x84 global matrix (MTX34), +0xBA base position, +0xBB flags
// (bit 0 visible), +0xBC name.
std::string paneName(u32 pane) {
    char buf[17] = {};
    if (!pane) return "";
    for (u32 i = 0; i < 16; i++) {
        const u8 ch = u8(rd8(pane + 0xBC + i));
        if (ch == 0) break;
        buf[i] = (ch >= 0x20 && ch < 0x7F) ? char(ch) : '?';
    }
    return buf;
}

bool visibleChain(u32 pane) {
    for (int i = 0; pane && i < 64; i++) {
        if (!(rd8(pane + 0xBB) & 1)) return false;
        pane = rd32(pane + 0xC);
    }
    return true;
}

template <class F>
void walk(u32 pane, int depth, const F& f) {
    if (!pane || depth > 32) return;
    f(pane);
    const u32 head = pane + 0x14;
    u32 node = rd32(head);
    for (int i = 0; node && node != head && i < 1024; i++) {
        walk(node - 4, depth + 1, f);
        node = rd32(node);
    }
}

// The pane's rectangle (Pane::GetPaneRect, scaled by s about the pane's origin) through its
// global matrix: the bounding box in layout coordinates. False for a collapsed pane.
bool paneBox(u32 p, float s, float& x0, float& y0, float& x1, float& y1) {
    const float w = rdf32(p + 0x4C) * s, h = rdf32(p + 0x50) * s;
    const u8 base = rd8(p + 0xBA);
    const float left = (base % 3 == 0) ? 0.0f : (base % 3 == 1) ? -w * 0.5f : -w;
    const float top = (base / 3 == 0) ? 0.0f : (base / 3 == 1) ? h * 0.5f : h;
    const float m00 = rdf32(p + 0x84), m01 = rdf32(p + 0x88), m03 = rdf32(p + 0x90);
    const float m10 = rdf32(p + 0x94), m11 = rdf32(p + 0x98), m13 = rdf32(p + 0xA0);
    x0 = y0 = 1e9f;
    x1 = y1 = -1e9f;
    const float xs[2] = {left, left + w}, ys[2] = {top, top - h};
    for (float lx : xs)
        for (float ly : ys) {
            const float gx = m00 * lx + m01 * ly + m03, gy = m10 * lx + m11 * ly + m13;
            if (!std::isfinite(gx) || !std::isfinite(gy)) return false;
            x0 = std::min(x0, gx);
            x1 = std::max(x1, gx);
            y0 = std::min(y0, gy);
            y1 = std::max(y1, gy);
        }
    return x1 - x0 > 0.5f && y1 - y0 > 0.5f;
}

// Calibrated layout scales per hit-test source (layout / manager address).
std::unordered_map<u32, menunav::Scale> sScales;
menunav::Scale sLastWalkScale, sLastGuiScale{416.0f, -228.0f}, sLastHbmScale{304.0f, -228.0f};

menunav::Scale& scaleFor(u32 source, const menunav::Scale& fallback) {
    auto it = sScales.find(source);
    if (it == sScales.end()) it = sScales.emplace(source, fallback).first;
    return it->second;
}

// The field HUD's layouts (toolbar slots, memo button, action buttons): present whenever the
// player walks, so alone they must not take the D-pad / stick (menu_nav.h Item::hud).
bool hudName(const std::string& n) { return n.rfind("B_tb", 0) == 0 || n.rfind("B_actnBtn", 0) == 0; }

struct Pending {
    u32 pane;
    std::string name;
    float x0, y0, x1, y1;
    bool choice = false, cursor = false;
};

// Choice boxes (Layout/chc_win.arc, the same file on every disc): the dialogue choices and the
// pockets' "Drop / Carry / Quit" popup. Rows B_msg_00..04 (the unused ones hidden), the window
// N_chcWin_all, and csr_00 = the game's own cursor (the arrow in front of the highlighted row; it
// starts on the box's default row and follows the pointer). Noted during the pane walks.
struct ChoiceScan {
    bool choice = false;  // the layout is a choice box
    bool cursor = false;  // csr_00 visible, centre y (layout coordinates) in cy
    float cy = 0;
};

void scanChoicePane(u32 p, ChoiceScan& cs) {
    const u8 c0 = u8(rd8(p + 0xBC));
    if (c0 != 'N' && c0 != 'c') return;
    const std::string n = paneName(p);
    if (n == "N_chcWin_all") {
        cs.choice = true;
    } else if (n == "csr_00" && visibleChain(p)) {
        float x0, y0, x1, y1;
        if (paneBox(p, 1.0f, x0, y0, x1, y1)) {
            cs.cursor = true;
            cs.cy = (y0 + y1) * 0.5f;
        }
    }
}

// Marks the rows of a choice box and the row csr_00 is on. The box object (USA fn_8015D27C builds
// it: +0x930 rows, +0x934 cursor, +0x938 cancel row for B, all -1 at first) puts its cursor on the
// top row when it has opened (fn_8015C83C) and whenever the pointer leaves the screen
// (fn_8015DCC0 mode 1); while the pointer is on the screen (mode 0) the cursor is the row under
// the pointer, or none (-1, csr_00 hidden). So csr_00 shows the box's own highlight only while the
// game does not hit-test the rows (menu_nav.cpp uses it then, and the top row otherwise).
void markChoices(std::vector<Pending>& v, const ChoiceScan& cs) {
    if (!cs.choice) return;
    for (auto& p : v) {
        if (p.name.rfind("B_msg_", 0) != 0) continue;
        p.choice = true;
        if (cs.cursor && cs.cy >= std::min(p.y0, p.y1) && cs.cy <= std::max(p.y0, p.y1)) p.cursor = true;
    }
}

void publish(const std::vector<Pending>& v, const menunav::Scale& s, u32 group, bool hud, bool tested = true, u16 layer = 0) {
    for (const auto& p : v) {
        menunav::Item it;
        it.choice = p.choice;
        it.cursor = p.cursor;
        float ax, ay, bx, by;
        menunav::layoutToPointer(s, p.x0, p.y0, ax, ay);
        menunav::layoutToPointer(s, p.x1, p.y1, bx, by);
        it.x0 = std::min(ax, bx);
        it.x1 = std::max(ax, bx);
        it.y0 = std::min(ay, by);
        it.y1 = std::max(ay, by);
        it.key = p.pane;
        it.group = group;
        it.hud = hud;
        it.tested = tested;
        it.layer = layer;
        std::snprintf(it.name, sizeof it.name, "%s", p.name.c_str());
        menunav::report(it);
    }
}

bool dumpFrame() { return sDump && game::guestFrames() % 30 == 0; }

}  // namespace

// 1. The pane-tree hit test walk.
HLE(fn_802B5598) {
    const u32 layout = c.r[3], test = c.r[4], filter = c.r[5], data = c.r[6];
    const u32 lr = c.lr;
    callOriginal(c, "fn_802B5598");
    if (!menunav::wanted() || test != kHitTestPane || !layout) return;
    const u32 ret = c.r[3];
    const float qx = rdf32(data), qy = rdf32(data + 4), qs = rdf32(data + 8);
    menunav::Scale& s = scaleFor(layout, sLastWalkScale);
    menunav::calibrate(s, 0, qx, qy);
    sLastWalkScale = s;
    std::vector<Pending> found;
    bool hud = true;
    ChoiceScan cs;
    walk(rd32(layout + 0x20), 0, [&](u32 p) {
        scanChoicePane(p, cs);
        if (rd8(p + 0xBC) != 'B' || !visibleChain(p)) return;
        if (filter && !guestCall(c, filter, p)) return;
        Pending pd{p, paneName(p), 0, 0, 0, 0};
        if (!paneBox(p, std::isfinite(qs) && qs > 0 ? qs : 1.0f, pd.x0, pd.y0, pd.x1, pd.y1)) return;
        hud &= hudName(pd.name);
        found.push_back(pd);
    });
    if (dumpFrame()) {
        log("[navdump] f%u walk lr %08X layout %08X filter %08X q %.1f,%.1f,%.2f scale %.1f,%.1f: %zu items%s", game::guestFrames(),
            lr, layout, filter, qx, qy, qs, s.sx, s.sy, found.size(), hud ? " (hud)" : "");
        for (const auto& p : found) log("[navdump]   %s %.1f,%.1f .. %.1f,%.1f", p.name.c_str(), p.x0, p.y0, p.x1, p.y1);
    }
    markChoices(found, cs);
    publish(found, s, rd32(layout + 0x20), hud && !found.empty());  // group = root pane, as in 1b
    c.r[3] = ret;
}

// 1b. Layouts drawn this frame: screens that hit-test against positions they stored when they
// opened (the pockets record their slot panes' centres once, with the same pane walk) are found
// by their visible Bounding panes named 'B*' as they are drawn.
void cf::navLayoutDrawn(Ctx& c, u32 root) {
    (void)c;
    if (!menunav::wanted() || !root) return;
    std::vector<Pending> found;
    bool hud = true;
    ChoiceScan cs;
    walk(root, 0, [&](u32 p) {
        scanChoicePane(p, cs);
        if (rd8(p + 0xBC) != 'B' || rd32(p) != kBoundingVtable || !visibleChain(p)) return;
        Pending pd{p, paneName(p), 0, 0, 0, 0};
        if (pd.name == "B_msgWin") return;  // the text box: A advances it wherever the pointer is
        if (!paneBox(p, 1.0f, pd.x0, pd.y0, pd.x1, pd.y1)) return;
        hud &= hudName(pd.name);
        found.push_back(pd);
    });
    if (found.empty()) return;
    markChoices(found, cs);
    // Test aid CF_NAV_TREE=1: every pane of small menu layouts (up to 8 items) every 10 frames, to
    // find what a screen's own cursor / highlight is (csr_00 of the choice boxes was found this way).
    static const bool sTree = std::getenv("CF_NAV_TREE") != nullptr;
    if (sTree && game::guestFrames() % 10 == 0 && !hud && found.size() <= 8) {
        log("[navtree] f%u root %08X", game::guestFrames(), root);
        walk(root, 0, [&](u32 p) {
            log("[navtree]   %08X %-16s vt %08X vis %u a %u/%u t %.1f,%.1f s %.2f,%.2f", p, paneName(p).c_str(), rd32(p),
                rd8(p + 0xBB) & 1, rd8(p + 0xB8), rd8(p + 0xB9), rdf32(p + 0x90), rdf32(p + 0xA0), rdf32(p + 0x4C), rdf32(p + 0x50));
        });
    }
    // Draw order of the layouts with items this frame: a layout drawn later is in front (a submenu
    // over the design list; menu_nav.cpp leaves out the screen behind it).
    static u32 seqFrame = 0;
    static u16 seq = 0;
    if (seqFrame != game::guestFrames()) {
        seqFrame = game::guestFrames();
        seq = 0;
    }
    if (seq < 0xFFFF) seq++;
    if (dumpFrame()) {
        log("[navdump] f%u drawn root %08X: %zu items%s", game::guestFrames(), root, found.size(), hud ? " (hud)" : "");
        for (const auto& p : found)
            log("[navdump]   %s %.1f,%.1f .. %.1f,%.1f alpha %u/%u", p.name.c_str(), p.x0, p.y0, p.x1, p.y1, rd8(p.pane + 0xB8),
                rd8(p.pane + 0xB9));
    }
    publish(found, sLastWalkScale, root, hud, false, seq);
}

// 2. The game's gui::Manager::update (keyboards).
HLE(fn_80314AF4) {
    const u32 mgr = c.r[3], chan = c.r[4];
    const float x = float(c.f[1]), y = float(c.f[2]);
    callOriginal(c, "fn_80314AF4");
    if (!menunav::wanted() || (x == 1000.0f && y == 1000.0f) || chan > 3) return;
    const u32 ret = c.r[3];
    menunav::Scale& s = scaleFor(mgr, sLastGuiScale);
    menunav::calibrate(s, int(chan), x, y);
    sLastGuiScale = s;
    std::vector<Pending> found;
    const u32 list = mgr + 0xC;
    u32 node = guestCall(c, kListGetNext, list, 0);
    for (int n = 0; node && n < 1024; n++) {
        const u32 comp = rd32(node + 4);
        const u32 vt = rd32(comp);
        if (rd32(vt + 0x64) == kGuiContain && rd8(comp + 0x90)) {
            const u32 p = rd32(comp + 0x9C);
            Pending pd{p, paneName(p), 0, 0, 0, 0};
            if (p && visibleChain(p) && paneBox(p, 1.0f, pd.x0, pd.y0, pd.x1, pd.y1)) found.push_back(pd);
        }
        node = guestCall(c, kListGetNext, list, node);
    }
    if (dumpFrame()) {
        log("[navdump] f%u gui mgr %08X q %.1f,%.1f scale %.1f,%.1f: %zu items", game::guestFrames(), mgr, x, y, s.sx, s.sy,
            found.size());
        for (const auto& p : found) log("[navdump]   %s %.1f,%.1f .. %.1f,%.1f", p.name.c_str(), p.x0, p.y0, p.x1, p.y1);
    }
    publish(found, s, mgr, false);
    c.r[3] = ret;
}

// 3. The HOME Menu's gui::Manager::update.
HLE(update__Q310homebutton3gui7ManagerFiffUlUlUlPv) {
    const u32 mgr = c.r[3], chan = c.r[4];
    const float x = float(c.f[1]), y = float(c.f[2]);
    callOriginal(c, "update__Q310homebutton3gui7ManagerFiffUlUlUlPv");
    if (!menunav::wanted() || chan > 3 || std::fabs(x) >= 999.0f) return;
    const u32 ret = c.r[3];
    menunav::Scale& s = scaleFor(mgr, sLastHbmScale);
    menunav::calibrate(s, int(chan), x, y);
    sLastHbmScale = s;
    std::vector<Pending> found;
    const u32 count = rd16(mgr + 0x10);
    for (u32 i = 0; i < count && i < 1024; i++) {
        const u32 node = guestCall(c, kHbmListGetNth, mgr + 8, i);
        if (!node) continue;
        const u32 comp = rd32(node + 4);
        if (!comp || !rd8(comp + 0x24)) continue;  // trigger target
        const u32 p = rd32(comp + 0x2C);
        Pending pd{p, paneName(p), 0, 0, 0, 0};
        if (p && visibleChain(p) && paneBox(p, 1.0f, pd.x0, pd.y0, pd.x1, pd.y1)) found.push_back(pd);
    }
    if (dumpFrame()) {
        log("[navdump] f%u hbm mgr %08X chan %u q %.1f,%.1f scale %.1f,%.1f: %zu items", game::guestFrames(), mgr, chan, x, y, s.sx,
            s.sy, found.size());
        for (const auto& p : found) log("[navdump]   %s %.1f,%.1f .. %.1f,%.1f", p.name.c_str(), p.x0, p.y0, p.x1, p.y1);
    }
    publish(found, s, mgr, false);
    c.r[3] = ret;
}

}  // namespace recomp
