// "Snap pointer to menu items" (menu_nav.h, docs/CONTROLS.md "Snap pointer to menu items").
//
// Per guest frame the game side reports the hit regions the game tested the pointer against;
// apply() turns D-pad / Nunchuk stick presses into jumps between them:
//   - a press (and its auto-repeat while held) picks the next item in that direction from the
//     selected item (or from the pointer): the nearest one in the same row / column, else one
//     close to the direction (35 degrees), and up / down in a grid with a short last row the
//     nearest item of the next row; nothing in that direction = the selection stays, never a jump
//     along the other axis (moveCardinal below); the screen in front is modal (navigable());
//   - the pointer is then kept on the selected item's centre (clipped to the screen) every sample,
//     so it follows slide-in animations and scrolling lists, until a free pointer device moves it;
//   - "Recenter pointer" jumps to the item nearest the pointer instead of the screen centre;
//   - the field HUD (toolbar) alone does not take the D-pad / stick (they change tools and walk):
//     "Recenter pointer" enters it, B / Recenter / moving the pointer leaves it.
//
// Test aids: CF_NAV_LOG=1 (item sets, moves), CF_NAV_TRACE=<file> (every distinct navigable item
// set with its geometry and the move chosen from every item in 8 directions, JSON lines; checked
// offline by tools/nav/check.py), CF_NAV_OLD=1 (the first version's cost search, A/B).
#include "recomp/menu_nav.h"

#include "recomp/game.h"
#include "recomp/widescreen.h"
#include "recomp/wiimote.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::menunav {
namespace {

constexpr float kAspect = 16.0f / 9.0f;  // KPAD x units are this much wider than y units on screen
constexpr float kEdge = 0.96f;           // targets are kept this far inside the screen
constexpr double kRepeatDelay = 0.36, kRepeatRate = 0.11;

bool envOn(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
const bool sLog = envOn("CF_NAV_LOG");
bool sOld = envOn("CF_NAV_OLD");  // not const: the offline replay switches it

std::mutex sMutex;
std::vector<Item> sCollect, sPublished;
u32 sCollectFrame = 0, sPublishedFrame = 0;
std::atomic<bool> sWanted{true};

struct PointerNote {
    bool valid = false;
    float x = 0, y = 0;
};
std::array<PointerNote, wiimote::kRemotes> sPointer;

struct ChanState {
    u32 selected = 0;           // key of the item the pointer is locked on, 0 none
    u32 selGroup = 0;           // its group
    float selX = 0, selY = 0;   // where it was last (KPAD): a vanished selection is re-found near it
    bool toolbar = false;       // field HUD entered with Recenter
    bool haveSaved = false;     // pointer before entering the toolbar
    float savedX = 0, savedY = 0;
    bool haveMem = false;       // column / row kept across moves through wide items (KPAD)
    float memX = 0, memY = 0;
    float dirX = 0, dirY = 0;   // direction held (screen, isotropic), 0 none
    double nextRepeat = 0;
    u32 prevButtons = 0;
    bool stickHeld = false;
    bool wasActive = false;
    bool swallowB = false;      // the B that left the toolbar is kept from the game until released
};
std::array<ChanState, wiimote::kRemotes> sChan;

void traceSet(const std::vector<Item>& published, u32 frame);

void rotate(u32 frame) {
    if (sCollectFrame != frame && !sCollect.empty()) {
        sPublished.swap(sCollect);
        sPublishedFrame = sCollectFrame;
        sCollect.clear();
        traceSet(sPublished, sPublishedFrame);
    }
    sCollectFrame = frame;
}

struct Box {
    float x0, y0, x1, y1;
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

// The screen's x extent in KPAD units: the window's in ultrawide with the HUD anchored to its
// edges (widescreen.h pointerRangeX; anchored HUD panes lie beyond +-1 there).
float edgeX() { return kEdge * ws::pointerRangeX(); }

// The part of an item on screen; false when nothing of it is.
bool visibleBox(const Item& it, Box& b) {
    const float ex = edgeX();
    b = {std::max(it.x0, -ex), std::max(it.y0, -kEdge), std::min(it.x1, ex), std::min(it.y1, kEdge)};
    return b.x1 - b.x0 > 0.004f && b.y1 - b.y0 > 0.004f;
}

const Item* findKey(const std::vector<Item>& items, u32 key) {
    for (const auto& it : items)
        if (it.key == key) return &it;
    return nullptr;
}

// Smallest item containing the point.
const Item* itemAt(const std::vector<Item>& items, float x, float y) {
    const Item* best = nullptr;
    float area = 1e9f;
    for (const auto& it : items) {
        Box b;
        if (!visibleBox(it, b) || x < b.x0 || x > b.x1 || y < b.y0 || y > b.y1) continue;
        const float a = (b.x1 - b.x0) * (b.y1 - b.y0);
        if (a < area) {
            area = a;
            best = &it;
        }
    }
    return best;
}

bool overlapping(const Item& a, const Item& b) {
    Box p, q;
    if (!visibleBox(a, p) || !visibleBox(b, q)) return false;
    const float w = std::min(p.x1, q.x1) - std::max(p.x0, q.x0), h = std::min(p.y1, q.y1) - std::max(p.y0, q.y0);
    if (w <= 0 || h <= 0) return false;
    const float small = std::min((p.x1 - p.x0) * (p.y1 - p.y0), (q.x1 - q.x0) * (q.y1 - q.y0));
    return w * h > 0.3f * small;
}

// The navigable set as apply() sees it. The screen in front is modal, as in the game:
//   - a screen the game hit-tests (a choice box over the pockets) is in front of the layouts
//     only seen drawn that it overlaps (the whole layout's extent): those are left out. Drawn
//     layouts beside the tested items stay: the "Donate what?" / sell / give pickers hit-test
//     their Quit / OK buttons every frame (fn_80008ADC) but their item slots only while the
//     pointer is on the grid (or from stored positions), so "tested" alone does not mean "in
//     front" (it left only the two buttons navigable there). Drawn-only HUD items are left out
//     while any tested menu item is up (the toolbar is hit-tested whenever the game uses it);
//   - of two menu layouts that overlap (a submenu over the design list, both hit-tested), the one
//     drawn later is in front: every item of the one behind is left out (only menu items: the
//     toolbar stays reachable below every screen, and layouts side by side, e.g. the map's house
//     markers next to its resident list, are not affected).
// Items with nothing on screen are dropped. `menu` / `hud`: whether non-HUD / HUD items remain.
std::vector<Item> navigable(const std::vector<Item>& in, bool& menu, bool& hud) {
    std::vector<Item> items;
    const bool front = std::any_of(in.begin(), in.end(), [](const Item& it) { return it.tested && !it.hud; });
    // Drawn-only layouts covered by a tested menu item of another layout (CF_NAV_OLD: every
    // drawn-only item while anything is tested, the first version's rule).
    std::vector<u32> covered;
    if (front && !sOld) {
        struct Extent {
            u32 group;
            Box b;
        };
        std::vector<Extent> ext;  // union of each drawn-only layout's visible items
        for (const auto& it : in) {
            Box b;
            if (it.tested || it.hud || !visibleBox(it, b)) continue;
            auto e = std::find_if(ext.begin(), ext.end(), [&](const Extent& x) { return x.group == it.group; });
            if (e == ext.end()) {
                ext.push_back({it.group, b});
            } else {
                e->b = {std::min(e->b.x0, b.x0), std::min(e->b.y0, b.y0), std::max(e->b.x1, b.x1), std::max(e->b.y1, b.y1)};
            }
        }
        for (const auto& e : ext)
            for (const auto& t : in) {
                Box b;
                if (!t.tested || t.hud || t.group == e.group || !visibleBox(t, b)) continue;
                if (std::min(b.x1, e.b.x1) > std::max(b.x0, e.b.x0) && std::min(b.y1, e.b.y1) > std::max(b.y0, e.b.y0)) {
                    covered.push_back(e.group);
                    break;
                }
            }
    }
    for (const auto& it : in) {
        Box b;
        if (!visibleBox(it, b)) continue;
        if (front && !it.tested &&
            (sOld || it.hud || std::find(covered.begin(), covered.end(), it.group) != covered.end()))
            continue;
        items.push_back(it);
    }
    if (!sOld) {
        // In front = a layout of several items (a single marker drawn over its neighbour is not a
        // screen in front of it).
        auto several = [&](u32 group) {
            int n = 0;
            for (const auto& it : items) n += it.group == group;
            return n >= 2;
        };
        std::vector<u32> behind;
        for (const auto& a : items)
            for (const auto& b : items)
                if (!a.hud && !b.hud && a.group != b.group && a.layer && b.layer && a.layer < b.layer && overlapping(a, b) &&
                    several(b.group) &&
                    std::find(behind.begin(), behind.end(), a.group) == behind.end())
                    behind.push_back(a.group);
        if (!behind.empty())
            items.erase(std::remove_if(items.begin(), items.end(),
                                       [&](const Item& it) {
                                           return !it.hud && std::find(behind.begin(), behind.end(), it.group) != behind.end();
                                       }),
                        items.end());
    }
    menu = hud = false;
    for (const auto& it : items) (it.hud ? hud : menu) = true;
    return items;
}

// --- direction search ---
// Geometry in isotropic screen units (x scaled by the aspect ratio), axis 0 = x, 1 = y.
struct Geo {
    const Item* it = nullptr;
    float lo[2] = {}, hi[2] = {};
    bool container = false;  // holds another, smaller item's centre: a panel / list background
    float c(int a) const { return (lo[a] + hi[a]) * 0.5f; }
    float size(int a) const { return hi[a] - lo[a]; }
};

Geo geoOf(const Item& it) {
    Box b;
    if (!visibleBox(it, b)) b = {it.x0, it.y0, it.x1, it.y1};
    Geo g;
    g.it = &it;
    g.lo[0] = b.x0 * kAspect;
    g.hi[0] = b.x1 * kAspect;
    g.lo[1] = b.y0;
    g.hi[1] = b.y1;
    return g;
}

std::vector<Geo> geometry(const std::vector<Item>& items, bool hudOnly) {
    std::vector<Geo> v;
    for (const auto& it : items)
        if (!hudOnly || it.hud) v.push_back(geoOf(it));
    // Containers: a big pane (list or panel background, a page behind its tabs) that holds the
    // centre of a smaller item. Moves never land on one: it would steal the move from the items
    // on it (its centre is often beyond them). It stays clickable under the pointer.
    for (auto& g : v)
        for (const auto& o : v) {
            if (&o == &g) continue;
            const float ox = o.c(0), oy = o.c(1);
            if (ox > g.lo[0] && ox < g.hi[0] && oy > g.lo[1] && oy < g.hi[1] &&
                o.size(0) * o.size(1) * 2.0f < g.size(0) * g.size(1)) {
                g.container = true;
                break;
            }
        }
    return v;
}

// The move from `from` in a cardinal direction (axis a, sign s = +1 right / down, -1 left / up).
// `ref` = the across coordinate to stay close to (the kept column / row, isotropic).
//   1. In line: items overlapping the source's band across the direction (its row for left /
//      right, its column for up / down): the nearest one (edge gap), then the one closest to ref.
//   2. Else items near the direction (gap across under the distance along, centres within 35
//      degrees): the nearest by gap along + 2 x gap across.
//   3. Up / down only, a grid with a ragged last row: the next row of the same group (the same
//      layout) right after the source, its item closest to ref. Used when 1 and 2 find nothing,
//      and instead of an item of another layout further away than that row. Never sideways:
//      left / right at the end of a short row stays.
//   Nothing: no move (no wrapping: the game's menus do not wrap by pointer either).
// Candidates must lie beyond the source (centre past its centre, far edge past its far edge)
// and must not contain the source (a panel behind it) nor be containers.
const Geo* moveCardinal(const std::vector<Geo>& v, const Geo& from, int a, float s, float ref) {
    const int b = 1 - a;
    const float fFar = s > 0 ? from.hi[a] : from.lo[a];
    const float fC = from.c(a);
    const Geo* best1 = nullptr;
    float cost1 = 1e9f, gap1 = 0;
    const Geo* best2 = nullptr;
    float cost2 = 1e9f, gap2 = 0;
    struct Row {
        const Geo* g;
        float gap;
    };
    std::vector<Row> group;
    for (const auto& g : v) {
        if (g.it == from.it || g.container) continue;
        // Contains the source's centre: behind it, not next to it.
        if (from.c(0) >= g.lo[0] && from.c(0) <= g.hi[0] && from.c(1) >= g.lo[1] && from.c(1) <= g.hi[1]) continue;
        const float along = (g.c(a) - fC) * s;
        const float gFar = s > 0 ? g.hi[a] : g.lo[a];
        if (along <= 0.01f || (gFar - fFar) * s <= 0.005f) continue;
        const float gNear = s > 0 ? g.lo[a] : g.hi[a];
        const float gap = std::max(0.0f, (gNear - fFar) * s);
        const float overlap = std::min(from.hi[b], g.hi[b]) - std::max(from.lo[b], g.lo[b]);
        const float refR = std::clamp(ref, g.lo[b], g.hi[b]);
        const float acrossRef = std::fabs(refR - ref) + 0.1f * std::fabs(g.c(b) - ref);
        // Rows of the own grid: items wholly past the source (an item beside it, overlapping its
        // height, is not the next row).
        if (a == 1 && g.it->group == from.it->group && (gNear - fFar) * s >= -0.25f * from.size(a)) group.push_back({&g, gap});
        if (overlap > 0.2f * std::min(from.size(b), g.size(b))) {
            const float cost = gap + 0.3f * acrossRef;
            if (cost < cost1) {
                cost1 = cost;
                best1 = &g;
                gap1 = gap;
            }
            continue;
        }
        // Off the line: only close to the direction, both from the source's band (gap) and by the
        // centres (35 degrees): the bag button down-left of the pockets' left column is not "left".
        const float acrossGap = std::max(0.0f, -overlap);
        if (acrossGap < along && std::fabs(g.c(b) - from.c(b)) < 0.7f * along) {
            const float cost = gap + 2.0f * acrossGap;
            if (cost < cost2) {
                cost2 = cost;
                best2 = &g;
                gap2 = gap;
            }
        }
    }
    // 3. The next row of the source's own grid: the nearest item of the group below / above sets
    //    it, if it follows right after the source (gap under one item height); its items overlap
    //    that one's band; the one closest to ref.
    const Geo* best3 = nullptr;
    float gap3 = 1e9f;
    if (!group.empty()) {
        const Row* first = &group[0];
        for (const auto& r : group)
            if (r.gap < first->gap) first = &r;
        if (first->gap <= from.size(a)) {
            float cost3 = 1e9f;
            gap3 = first->gap;
            for (const auto& r : group) {
                const float ov = std::min(first->g->hi[a], r.g->hi[a]) - std::max(first->g->lo[a], r.g->lo[a]);
                if (ov <= 0.5f * std::min(first->g->size(a), r.g->size(a))) continue;
                const float d = std::fabs(r.g->c(b) - ref);
                if (d < cost3) {
                    cost3 = d;
                    best3 = r.g;
                }
            }
        }
    }
    // An item of another layout further away (the toolbar below the pockets' short last row)
    // does not skip the grid's own next row.
    const auto grid = [&](const Geo* g, float gap) { return best3 && g->it->group != from.it->group && gap3 < gap ? best3 : g; };
    if (best1) return grid(best1, gap1);
    if (best2) return grid(best2, gap2);
    return best3;
}

// Diagonal (stick) moves: items in the 45 degree quadrant around the direction (both
// coordinates beyond the source's centre), the nearest by distance + off-direction distance.
const Geo* moveDiagonal(const std::vector<Geo>& v, const Geo& from, float dx, float dy) {
    const Geo* best = nullptr;
    float bestCost = 1e9f;
    for (const auto& g : v) {
        if (g.it == from.it || g.container) continue;
        if (from.c(0) >= g.lo[0] && from.c(0) <= g.hi[0] && from.c(1) >= g.lo[1] && from.c(1) <= g.hi[1]) continue;
        const float vx = g.c(0) - from.c(0), vy = g.c(1) - from.c(1);
        if (vx * dx <= 0.01f || vy * dy <= 0.01f) continue;
        const float along = vx * dx + vy * dy, across = std::fabs(-vx * dy + vy * dx);
        if (across > along) continue;
        const float cost = along + 1.5f * across;
        if (cost < bestCost) {
            bestCost = cost;
            best = &g;
        }
    }
    return best;
}

// The first version's search (CF_NAV_OLD=1 for A/B): any item beyond the middle, cost = gap
// along + 0.25 x centre distance along + (off-line: 1 + 4 x gap across) + 0.35 x across.
const Item* inDirectionOld(const std::vector<Item>& items, const Box& from, float dx, float dy, u32 self, bool hudOnly) {
    const float fx = from.cx() * kAspect, fy = from.cy();
    const float fw = (from.x1 - from.x0) * 0.5f * kAspect, fh = (from.y1 - from.y0) * 0.5f;
    const Item* best = nullptr;
    float bestCost = 1e9f;
    for (const auto& it : items) {
        Box b;
        if (it.key == self || (hudOnly && !it.hud) || !visibleBox(it, b)) continue;
        const float cx = b.cx() * kAspect, cy = b.cy();
        const float vx = cx - fx, vy = cy - fy;
        const float along = vx * dx + vy * dy;
        const float bw = (b.x1 - b.x0) * 0.5f * kAspect, bh = (b.y1 - b.y0) * 0.5f;
        const float fAlong = std::fabs(fw * dx) + std::fabs(fh * dy), bAlong = std::fabs(bw * dx) + std::fabs(bh * dy);
        if (along <= 0.02f || along + bAlong <= fAlong * 0.5f) continue;
        const float across = -vx * dy + vy * dx;
        const float fAcross = std::fabs(fw * dy) + std::fabs(fh * dx), bAcross = std::fabs(bw * dy) + std::fabs(bh * dx);
        const float gap = std::max(0.0f, std::fabs(across) - fAcross - bAcross);
        const float edge = std::max(0.0f, along - fAlong - bAlong);
        const float cost = edge + 0.25f * along + (gap > 0 ? 1.0f + 4.0f * gap : 0.0f) + 0.35f * std::fabs(across);
        if (cost < bestCost) {
            bestCost = cost;
            best = &it;
        }
    }
    return best;
}

// The item a move from `cur` in direction (dx, dy) (isotropic, unit length, y down) lands on;
// nullptr = no item that way. memX / memY: kept column / row (KPAD), or NaN for the source's.
const Item* moveTarget(const std::vector<Item>& items, const Item& cur, float dx, float dy, bool hudOnly, float memX = NAN,
                       float memY = NAN) {
    if (sOld) {
        Box from;
        if (!visibleBox(cur, from)) from = {cur.x0, cur.y0, cur.x1, cur.y1};
        return inDirectionOld(items, from, dx, dy, cur.key, hudOnly);
    }
    std::vector<Geo> v = geometry(items, hudOnly);
    Geo from = geoOf(cur);
    for (const auto& g : v)
        if (g.it->key == cur.key) from = g;
    // Snap to one of 8 directions (22.5 degree sectors).
    const float adx = std::fabs(dx), ady = std::fabs(dy);
    const Geo* g = nullptr;
    if (adx > 2.414f * ady) {
        const float ref = std::isnan(memY) ? from.c(1) : memY;
        g = moveCardinal(v, from, 0, dx > 0 ? 1.0f : -1.0f, ref);
    } else if (ady > 2.414f * adx) {
        const float ref = std::isnan(memX) ? from.c(0) : memX * kAspect;
        g = moveCardinal(v, from, 1, dy > 0 ? 1.0f : -1.0f, ref);
    } else {
        g = moveDiagonal(v, from, dx > 0 ? 0.7071f : -0.7071f, dy > 0 ? 0.7071f : -0.7071f);
    }
    return g ? g->it : nullptr;
}

// Nearest item to a point (the first press, Recenter): containers only when nothing else.
const Item* nearest(const std::vector<Item>& items, float x, float y, bool hudOnly) {
    std::vector<Geo> v = geometry(items, hudOnly);
    const Item* best = nullptr;
    float bestD = 1e9f;
    const float px = x * kAspect;
    for (const auto& g : v) {
        // Distance to the box (0 inside), centre distance as the tie-break.
        const float dx = std::max({g.lo[0] - px, 0.0f, px - g.hi[0]}), dy = std::max({g.lo[1] - y, 0.0f, y - g.hi[1]});
        const float cx = g.c(0) - px, cy = g.c(1) - y;
        float d = std::sqrt(dx * dx + dy * dy) + 0.01f * std::sqrt(cx * cx + cy * cy);
        if (g.container && !sOld) d += 10.0f;
        if (d < bestD) {
            bestD = d;
            best = g.it;
        }
    }
    return best;
}

void target(const Item& it, float& x, float& y) {
    Box b;
    if (!visibleBox(it, b)) b = {it.x0, it.y0, it.x1, it.y1};
    x = std::clamp(b.cx(), -edgeX(), edgeX());
    y = std::clamp(b.cy(), -kEdge, kEdge);
}

// --- CF_NAV_TRACE ---
// Each distinct navigable set (keys + geometry to 0.01, stable for 6 frames) once, with the move
// from every item in 8 directions: {"f":frame,"mode":"menu"|"toolbar","items":[...],"moves":[...]}.
struct Trace {
    FILE* f = nullptr;
    std::string pending, last;
    u32 pendingSince = 0;
    int sets = 0;
};
Trace& trace() {
    static Trace t = [] {
        Trace t;
        if (const char* p = std::getenv("CF_NAV_TRACE"); p && *p) t.f = std::fopen(p, "w");
        return t;
    }();
    return t;
}

std::string hex(u32 v) {
    char b[12];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

void traceSet(const std::vector<Item>& published, u32 frame) {
    Trace& t = trace();
    if (!t.f) return;
    bool menu, hud;
    std::vector<Item> items = navigable(published, menu, hud);
    const bool hudOnly = !menu;
    if (hudOnly) items.erase(std::remove_if(items.begin(), items.end(), [](const Item& it) { return !it.hud; }), items.end());
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
    // Every visible item of the frame, navigable or not (nav 0: behind the screen in front), so
    // the checker sees what the modal rules left out and the replay can apply them again.
    std::vector<Item> all;
    for (const auto& it : published) {
        Box b;
        if (visibleBox(it, b)) all.push_back(it);
    }
    std::sort(all.begin(), all.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
    auto isNav = [&](const Item& it) { return findKey(items, it.key) != nullptr; };
    std::string sig;
    char buf[200];
    for (const auto& it : all) {
        std::snprintf(buf, sizeof buf, "%08X %.2f %.2f %.2f %.2f %d %d|", it.key, it.x0, it.y0, it.x1, it.y1, it.tested ? 1 : 0,
                      isNav(it) ? 1 : 0);
        sig += buf;
    }
    if (sig != t.pending) {
        t.pending = sig;
        t.pendingSince = frame;
        return;
    }
    if (sig == t.last || frame < t.pendingSince + 6 || items.empty()) return;
    t.last = sig;
    std::string out = "{\"f\":" + std::to_string(frame) + ",\"mode\":\"" + (menu ? "menu" : "toolbar") + "\",\"items\":[";
    for (size_t i = 0; i < all.size(); i++) {
        const auto& it = all[i];
        std::string name;
        for (const char* p = it.name; *p; p++)
            if (*p != '"' && *p != '\\') name += *p;
        std::snprintf(buf, sizeof buf,
                      "%s{\"k\":\"%08X\",\"n\":\"%s\",\"g\":\"%08X\",\"b\":[%.4f,%.4f,%.4f,%.4f],\"hud\":%d,\"t\":%d,\"l\":%u,\"nav\":%d}",
                      i ? "," : "", it.key, name.c_str(), it.group, it.x0, it.y0, it.x1, it.y1, it.hud ? 1 : 0, it.tested ? 1 : 0,
                      unsigned(it.layer), isNav(it) ? 1 : 0);
        out += buf;
    }
    out += "],\"moves\":[";
    static const struct {
        const char* n;
        float x, y;
    } kDirs[8] = {{"L", -1, 0}, {"R", 1, 0}, {"U", 0, -1}, {"D", 0, 1},
                  {"UL", -0.7071f, -0.7071f}, {"UR", 0.7071f, -0.7071f}, {"DL", -0.7071f, 0.7071f}, {"DR", 0.7071f, 0.7071f}};
    bool firstMove = true;
    for (const auto& it : items) {
        for (const auto& d : kDirs) {
            const Item* to = moveTarget(items, it, d.x, d.y, hudOnly);
            out += std::string(firstMove ? "" : ",") + "[\"" + hex(it.key) + "\",\"" + d.n + "\",\"" + (to ? hex(to->key) : "") + "\"]";
            firstMove = false;
        }
    }
    out += "]}\n";
    std::fputs(out.c_str(), t.f);
    std::fflush(t.f);
    if (sLog) log("[nav] trace: set %d at frame %u (%zu items)", ++t.sets, frame, items.size());
}

}  // namespace

bool wanted() { return sWanted.load(std::memory_order_relaxed); }

void report(const Item& it) {
    std::lock_guard lk(sMutex);
    rotate(game::guestFrames());
    for (auto& e : sCollect)
        if (e.key == it.key) {
            const bool tested = e.tested || it.tested;
            const u16 layer = std::max(e.layer, it.layer);
            e = it;
            e.tested = tested;
            e.layer = layer;
            return;
        }
    sCollect.push_back(it);
}

void onFrame() {
    std::lock_guard lk(sMutex);
    rotate(game::guestFrames());
}

void notePointer(int chan, bool valid, float x, float y) {
    if (chan < 0 || chan >= wiimote::kRemotes) return;
    sPointer[chan] = {valid, x, y};
}

bool lastPointer(int chan, float& x, float& y) {
    if (chan < 0 || chan >= wiimote::kRemotes || !sPointer[chan].valid) return false;
    x = sPointer[chan].x;
    y = sPointer[chan].y;
    return true;
}

bool menuUp() {
    std::lock_guard lk(sMutex);
    rotate(game::guestFrames());
    if (sPublishedFrame + 3 < game::guestFrames()) return false;
    Box b;
    return std::any_of(sPublished.begin(), sPublished.end(), [&](const Item& it) { return !it.hud && visibleBox(it, b); });
}

bool calibrate(Scale& s, int chan, float qx, float qy) {
    float kx, ky;
    if (std::fabs(qx) >= 999.0f || !lastPointer(chan, kx, ky)) return false;
    bool changed = false;
    if (std::fabs(kx) > 0.08f) {
        const float sx = qx / kx;
        if (std::fabs(sx) > 100.0f && std::fabs(sx) < 1500.0f && std::fabs(sx - s.sx) > 0.01f * std::fabs(s.sx)) {
            s.sx = sx;
            changed = true;
        }
    }
    if (std::fabs(ky) > 0.08f) {
        const float sy = qy / ky;
        if (std::fabs(sy) > 100.0f && std::fabs(sy) < 1500.0f && std::fabs(sy - s.sy) > 0.01f * std::fabs(s.sy)) {
            s.sy = sy;
            changed = true;
        }
    }
    return changed;
}

void setWanted(bool on) { sWanted.store(on, std::memory_order_relaxed); }

Output apply(int chan, const Input& in) {
    Output out;
    out.buttons = in.buttons;
    out.stickX = in.stickX;
    out.stickY = in.stickY;
    if (chan < 0 || chan >= wiimote::kRemotes) return out;
    ChanState& st = sChan[chan];
    const u32 pressed = in.buttons & ~st.prevButtons;
    st.prevButtons = in.buttons;
    // The B press that left the toolbar: held over several samples, only its first one was taken,
    // so the game still saw B go down (a cancel / pick up in the field).
    if (st.swallowB) {
        if (in.buttons & wiimote::kBtnB) out.buttons &= ~wiimote::kBtnB;
        else st.swallowB = false;
    }
    auto reset = [&] {
        st.selected = 0;
        st.toolbar = false;
        st.haveSaved = false;
        st.haveMem = false;
        st.dirX = st.dirY = 0;
        st.stickHeld = false;
        st.wasActive = false;
    };
    if (in.mode == Mode::Off || (in.mode == Mode::Auto && in.mouseDrives)) {
        reset();
        return out;
    }

    std::vector<Item> published;
    {
        std::lock_guard lk(sMutex);
        rotate(game::guestFrames());
        if (sPublishedFrame + 3 >= game::guestFrames()) published = sPublished;
    }
    bool menu = false, hud = false;
    std::vector<Item> items = navigable(published, menu, hud);
    if (!menu && !hud) {
        reset();
        return out;
    }
    if (menu) st.toolbar = false;
    if (in.pointerMoved) {
        st.selected = 0;
        st.toolbar = false;
        st.haveSaved = false;
        st.haveMem = false;
    }

    const bool recenter = (pressed & wiimote::kBtnRecenter) != 0;
    if (!menu && !st.toolbar) {
        st.selected = 0;
        st.wasActive = false;
        if (!recenter) return out;
        // Field HUD: Recenter brings the pointer onto the toolbar.
        st.toolbar = true;
        st.haveSaved = true;
        st.savedX = in.px;
        st.savedY = in.py;
        if (const Item* it = nearest(items, in.px, 1.0f, true)) st.selected = it->key;
        out.buttons &= ~wiimote::kBtnRecenter;
        if (sLog) log("[nav] remote %d: toolbar entered", chan + 1);
    } else if (!menu && st.toolbar && (pressed & wiimote::kBtnB)) {
        // Leave the toolbar: the pointer goes back where it was.
        st.toolbar = false;
        st.selected = 0;
        out.buttons &= ~wiimote::kBtnB;
        st.swallowB = true;
        if (st.haveSaved) {
            out.setPointer = true;
            out.px = st.savedX;
            out.py = st.savedY;
        }
        st.haveSaved = false;
        if (sLog) log("[nav] remote %d: toolbar left", chan + 1);
        return out;
    } else if (!menu && st.toolbar && recenter) {
        st.toolbar = false;
        st.selected = 0;
        out.buttons &= ~wiimote::kBtnRecenter;
        if (st.haveSaved) {
            out.setPointer = true;
            out.px = st.savedX;
            out.py = st.savedY;
        }
        st.haveSaved = false;
        return out;
    }

    out.active = true;
    const bool hudOnly = !menu;
    if (!st.wasActive && sLog) {
        log("[nav] remote %d: %zu items (%s)", chan + 1, items.size(), menu ? "menu" : "toolbar");
        for (const auto& it : items)
            log("[nav]   %-16s %s %.3f,%.3f .. %.3f,%.3f", it.name, it.hud ? "hud " : "menu", it.x0, it.y0, it.x1, it.y1);
    }
    st.wasActive = true;
    // The D-pad and the stick belong to the navigation now.
    constexpr u32 kDpad = wiimote::kBtnUp | wiimote::kBtnDown | wiimote::kBtnLeft | wiimote::kBtnRight;
    out.buttons &= ~(kDpad | wiimote::kBtnRecenter);
    out.stickX = out.stickY = 0;

    // A selection that vanished (a list scrolled it away, the screen rebuilt its panes): the item
    // now nearest where it was, preferring its own group, keeps the place in the list.
    if (st.selected && !findKey(items, st.selected)) {
        st.selected = 0;
        if (!sOld) {
            std::vector<Item> same;
            for (const auto& it : items)
                if (it.group == st.selGroup && (!hudOnly || it.hud)) same.push_back(it);
            if (const Item* it = nearest(same.empty() ? items : same, st.selX, st.selY, hudOnly)) {
                Box b;
                if (visibleBox(*it, b) && std::fabs(b.cx() - st.selX) < 0.3f && std::fabs(b.cy() - st.selY) < 0.3f) {
                    st.selected = it->key;
                    if (sLog) log("[nav] remote %d: selection gone, now %s", chan + 1, it->name);
                }
            }
        }
    }

    // Direction from the D-pad, else the stick (hysteresis), in isotropic screen units, y down.
    float dx = 0, dy = 0;
    if (in.buttons & kDpad) {
        dx = float(((in.buttons & wiimote::kBtnRight) != 0) - ((in.buttons & wiimote::kBtnLeft) != 0));
        dy = float(((in.buttons & wiimote::kBtnDown) != 0) - ((in.buttons & wiimote::kBtnUp) != 0));
        st.stickHeld = false;
    } else {
        const float m = std::sqrt(in.stickX * in.stickX + in.stickY * in.stickY);
        if (m > (st.stickHeld ? 0.35f : 0.55f)) {
            st.stickHeld = true;
            dx = in.stickX;
            dy = -in.stickY;
        } else {
            st.stickHeld = false;
        }
    }
    bool move = false;
    if (const float m = std::sqrt(dx * dx + dy * dy); m > 0) {
        dx /= m;
        dy /= m;
        const bool fresh = st.dirX == 0 && st.dirY == 0;
        const bool turned = !fresh && dx * st.dirX + dy * st.dirY < 0.92f;
        if (fresh || turned) {
            move = true;
            st.nextRepeat = in.now + kRepeatDelay;
        } else if (in.now >= st.nextRepeat) {
            move = true;
            st.nextRepeat = in.now + kRepeatRate;
        }
        st.dirX = dx;
        st.dirY = dy;
    } else {
        st.dirX = st.dirY = 0;
    }

    if (recenter && menu) {
        if (const Item* it = nearest(items, in.px, in.py, false)) st.selected = it->key;
        st.haveMem = false;
    } else if (move) {
        const Item* cur = st.selected ? findKey(items, st.selected) : itemAt(items, in.px, in.py);
        if (cur && cur->hud && !hudOnly && !st.selected) cur = itemAt(items, in.px, in.py);
        const Item* next = nullptr;
        if (cur) {
            float cx, cy;
            target(*cur, cx, cy);
            if (!st.haveMem || cur->key != st.selected) {
                st.memX = cx;
                st.memY = cy;
                st.haveMem = true;
            }
            const bool horizontal = std::fabs(dx) > 2.414f * std::fabs(dy), vertical = std::fabs(dy) > 2.414f * std::fabs(dx);
            next = moveTarget(items, *cur, dx, dy, hudOnly, vertical ? st.memX : NAN, horizontal ? st.memY : NAN);
            if (!next) next = cur;  // nothing that way: stay
            float nx, ny;
            target(*next, nx, ny);
            // The column is kept while moving up / down (and the row left / right) through items
            // wider than one cell, so down-down-up returns to the same key; a move along the
            // other axis (or a diagonal) starts a new one.
            if (!vertical) st.memX = nx;
            if (!horizontal) st.memY = ny;
        } else {
            // Not on an item yet: the first press lands on the item nearest the pointer.
            next = nearest(items, in.px, in.py, hudOnly);
            st.haveMem = false;
        }
        if (next) {
            if (sLog && next->key != st.selected) log("[nav] remote %d: -> %s", chan + 1, next->name);
            st.selected = next->key;
        }
    }

    if (st.selected)
        if (const Item* it = findKey(items, st.selected)) {
            out.setPointer = true;
            target(*it, out.px, out.py);
            st.selGroup = it->group;
            st.selX = out.px;
            st.selY = out.py;
        }
    return out;
}

}  // namespace recomp::menunav

#ifdef MENUNAV_REPLAY
// tools/nav/replay.cpp: offline replay of traced item sets through moveTarget.
namespace recomp::menunav::replay {
const Item* move(const std::vector<Item>& items, const Item& cur, float dx, float dy, bool hudOnly) {
    return moveTarget(items, cur, dx, dy, hudOnly);
}
bool container(const std::vector<Item>& items, size_t i) { return geometry(items, false)[i].container; }
void setOld(bool on) { sOld = on; }
// The navigable items of a frame's set, as apply() takes them (toolbar mode: HUD items only).
std::vector<Item> nav(const std::vector<Item>& all, bool& hudOnly) {
    bool menu, hud;
    std::vector<Item> items = navigable(all, menu, hud);
    hudOnly = !menu;
    if (hudOnly) items.erase(std::remove_if(items.begin(), items.end(), [](const Item& it) { return !it.hud; }), items.end());
    return items;
}
}  // namespace recomp::menunav::replay
#endif
