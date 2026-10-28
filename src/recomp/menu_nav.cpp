// "Snap pointer to menu items" (menu_nav.h, docs/CONTROLS.md "Snap pointer to menu items").
//
// Per guest frame the game side reports the hit regions the game tested the pointer against;
// apply() turns D-pad / Nunchuk stick presses into jumps between them:
//   - a press (and its auto-repeat while held) picks the nearest item in that direction from the
//     selected item (or from the pointer), favouring items in line with it;
//   - the pointer is then kept on the selected item's centre (clipped to the screen) every sample,
//     so it follows slide-in animations and scrolling lists, until a free pointer device moves it;
//   - "Recenter pointer" jumps to the item nearest the pointer instead of the screen centre;
//   - the field HUD (toolbar) alone does not take the D-pad / stick (they change tools and walk):
//     "Recenter pointer" enters it, B / Recenter / moving the pointer leaves it.
#include "recomp/menu_nav.h"

#include "recomp/game.h"
#include "recomp/wiimote.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::menunav {
namespace {

constexpr float kAspect = 16.0f / 9.0f;  // KPAD x units are this much wider than y units on screen
constexpr float kEdge = 0.96f;           // targets are kept this far inside the screen
constexpr double kRepeatDelay = 0.36, kRepeatRate = 0.11;

const bool sLog = [] {
    const char* v = std::getenv("CF_NAV_LOG");
    return v && *v && *v != '0';
}();

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
    bool toolbar = false;       // field HUD entered with Recenter
    bool haveSaved = false;     // pointer before entering the toolbar
    float savedX = 0, savedY = 0;
    float dirX = 0, dirY = 0;   // direction held (screen, isotropic), 0 none
    double nextRepeat = 0;
    u32 prevButtons = 0;
    bool stickHeld = false;
    bool wasActive = false;
};
std::array<ChanState, wiimote::kRemotes> sChan;

void rotate(u32 frame) {
    if (sCollectFrame != frame && !sCollect.empty()) {
        sPublished.swap(sCollect);
        sPublishedFrame = sCollectFrame;
        sCollect.clear();
    }
    sCollectFrame = frame;
}

struct Box {
    float x0, y0, x1, y1;
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

// The part of an item on screen; false when nothing of it is.
bool visibleBox(const Item& it, Box& b) {
    b = {std::max(it.x0, -kEdge), std::max(it.y0, -kEdge), std::min(it.x1, kEdge), std::min(it.y1, kEdge)};
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

const Item* nearest(const std::vector<Item>& items, float x, float y, bool hudOnly) {
    const Item* best = nullptr;
    float bestD = 1e9f;
    for (const auto& it : items) {
        Box b;
        if ((hudOnly && !it.hud) || !visibleBox(it, b)) continue;
        // Distance to the box (0 inside), centre distance as the tie-break.
        const float dx = std::max({b.x0 - x, 0.0f, x - b.x1}) * kAspect, dy = std::max({b.y0 - y, 0.0f, y - b.y1});
        const float cx = (b.cx() - x) * kAspect, cy = b.cy() - y;
        const float d = std::sqrt(dx * dx + dy * dy) + 0.01f * std::sqrt(cx * cx + cy * cy);
        if (d < bestD) {
            bestD = d;
            best = &it;
        }
    }
    return best;
}

// The best item in direction (dx, dy) (isotropic screen units, unit length) from box `from`
// (or the point when from is degenerate), skipping `self`.
const Item* inDirection(const std::vector<Item>& items, const Box& from, float dx, float dy, u32 self, bool hudOnly) {
    const float fx = from.cx() * kAspect, fy = from.cy();
    // Half extents of `from` along / across the direction.
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
        // Extents projected on the direction: the candidate must lie beyond the source's middle.
        const float fAlong = std::fabs(fw * dx) + std::fabs(fh * dy), bAlong = std::fabs(bw * dx) + std::fabs(bh * dy);
        if (along <= 0.02f || along + bAlong <= fAlong * 0.5f) continue;
        const float across = -vx * dy + vy * dx;
        const float fAcross = std::fabs(fw * dy) + std::fabs(fh * dx), bAcross = std::fabs(bw * dy) + std::fabs(bh * dx);
        const float gap = std::max(0.0f, std::fabs(across) - fAcross - bAcross);  // 0: in line
        const float edge = std::max(0.0f, along - fAlong - bAlong);               // gap along
        const float cost = edge + 0.25f * along + 3.0f * gap + 0.35f * std::fabs(across);
        if (cost < bestCost) {
            bestCost = cost;
            best = &it;
        }
    }
    return best;
}

void target(const Item& it, float& x, float& y) {
    Box b;
    if (!visibleBox(it, b)) b = {it.x0, it.y0, it.x1, it.y1};
    x = std::clamp(b.cx(), -kEdge, kEdge);
    y = std::clamp(b.cy(), -kEdge, kEdge);
}

}  // namespace

bool wanted() { return sWanted.load(std::memory_order_relaxed); }

void report(const Item& it) {
    std::lock_guard lk(sMutex);
    rotate(game::guestFrames());
    for (auto& e : sCollect)
        if (e.key == it.key) {
            e = it;
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
    auto reset = [&] {
        st.selected = 0;
        st.toolbar = false;
        st.haveSaved = false;
        st.dirX = st.dirY = 0;
        st.stickHeld = false;
        st.wasActive = false;
    };
    if (in.mode == Mode::Off || (in.mode == Mode::Auto && in.mouseDrives)) {
        reset();
        return out;
    }

    std::vector<Item> items;
    {
        std::lock_guard lk(sMutex);
        rotate(game::guestFrames());
        if (sPublishedFrame + 3 >= game::guestFrames()) items = sPublished;
    }
    bool menu = false, hud = false;
    for (const auto& it : items) {
        Box b;
        if (!visibleBox(it, b)) continue;
        (it.hud ? hud : menu) = true;
    }
    if (!menu && !hud) {
        reset();
        return out;
    }
    if (menu) st.toolbar = false;
    if (in.pointerMoved) {
        st.selected = 0;
        st.toolbar = false;
        st.haveSaved = false;
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

    if (st.selected && !findKey(items, st.selected)) st.selected = 0;

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
    } else if (move) {
        const Item* cur = st.selected ? findKey(items, st.selected) : itemAt(items, in.px, in.py);
        if (cur && cur->hud && !hudOnly && !st.selected) cur = itemAt(items, in.px, in.py);
        const Item* next = nullptr;
        if (cur) {
            Box from;
            if (!visibleBox(*cur, from)) from = {cur->x0, cur->y0, cur->x1, cur->y1};
            next = inDirection(items, from, dx, dy, cur->key, hudOnly);
            if (!next) next = cur;  // edge of the screen: stay
        } else {
            // Not on an item yet: the first press lands on the item nearest the pointer.
            next = nearest(items, in.px, in.py, hudOnly);
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
        }
    return out;
}

}  // namespace recomp::menunav
