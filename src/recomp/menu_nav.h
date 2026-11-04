#pragma once
// Menu navigation without aiming the pointer: "Snap pointer to menu items" (F1 > Controls >
// Pointer & motion; docs/CONTROLS.md "Snap pointer to menu items").
//
// The game side (src/game/cf_menu_nav.cpp) reports the hit regions the game tests the pointer
// against while a pointer-driven screen is up, every guest frame, in KPAD pointer units (x -1..1
// left to right, y -1..1 top to bottom of the game image). The Wii Remote side (wiimote.cpp) asks
// apply() each time it builds a port's state: while items are up, the D-pad and the Nunchuk stick
// move the pointer from item to item (the nearest one in that direction) instead of reaching the
// game, and the pointer stays locked on the chosen item (it follows the item when it moves) until
// the pointer is moved freely again (right stick, mouse, gyro).
//
// Shared helpers for code that maps between the game's layout coordinates and the KPAD pointer
// (pointerToLayout / layoutToPointer) live here so other pointer work (ultrawide click positions)
// uses the same mapping.
#include <dolphin/types.h>

namespace recomp::menunav {

// One hit region of the screen that is up, in KPAD units (x0 < x1, y0 < y1).
struct Item {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    u32 key = 0;      // stable identity while the screen is up (the pane's guest address)
    u32 group = 0;    // the layout / gui manager it belongs to (rows of a grid are one group)
    u16 layer = 0;    // draw order of its layout this frame (higher = drawn later = in front), 0 unknown
    bool hud = false; // part of the always-present field HUD (toolbar): navigable, but alone it
                      // does not take the D-pad / stick from the game
    bool tested = true; // the game hit-tested it this frame (false: only seen drawn). While tested
                        // non-HUD items are up (a choice box over the pockets), only tested items
                        // are navigable: the screen in front is modal.
    char name[17] = {};
};

// --- game side (guest thread) ---
// True while any port may snap (so the game side can skip collecting items otherwise).
bool wanted();
// Adds a hit region for the current guest frame. Items of a frame become the navigable set when
// the next frame starts reporting (or onFrame() runs); a screen that stops reporting has no items.
void report(const Item& it);
// Once per guest frame (before the game's update): closes the previous frame's item set.
void onFrame();
// The pointer the game was given for a Remote most recently (KPAD units). False: pointing off
// the screen / never sampled.
bool lastPointer(int chan, float& x, float& y);
// True while the game shows a pointer screen (pockets, map, shops, dialogue choices, ...): items
// other than the field toolbar were reported in the last few frames.
bool menuUp();

// A layout-coordinate scale pair as the game converts the pointer for one hit test:
// layout = (x * sx, y * sy). Calibrated from the point the game tested and the pointer it had.
struct Scale {
    float sx = 304.0f, sy = -228.0f;
};
// Updates s from a tested layout point (qx, qy) and the Remote's pointer; true when it changed.
bool calibrate(Scale& s, int chan, float qx, float qy);
inline void layoutToPointer(const Scale& s, float lx, float ly, float& px, float& py) {
    px = lx / s.sx;
    py = ly / s.sy;
}
inline void pointerToLayout(const Scale& s, float px, float py, float& lx, float& ly) {
    lx = px * s.sx;
    ly = py * s.sy;
}

// --- Wii Remote side (guest thread, wiimote.cpp) ---
enum class Mode : u8 { Off, Auto, On };
struct Input {
    Mode mode = Mode::Auto;
    bool mouseDrives = false;   // the mouse placed the pointer last (Auto: no snapping)
    bool pointerMoved = false;  // a free pointer device moved the pointer this sample
    u32 buttons = 0;            // wiimote.h kBtn* incl. port-only bits (kBtnRecenter)
    float stickX = 0, stickY = 0;
    float px = 0, py = 0;       // the pointer before snapping
    double now = 0;             // seconds
};
struct Output {
    bool active = false;        // a navigable screen is up and snapping applies: D-pad / stick were taken
    bool setPointer = false;    // move the pointer to (px, py)
    float px = 0, py = 0;
    u32 buttons = 0;            // buttons left for the game (and port actions still to apply)
    float stickX = 0, stickY = 0;
};
Output apply(int chan, const Input& in);
// The pointer handed to the game for a Remote (every sample), for lastPointer / calibrate.
void notePointer(int chan, bool valid, float x, float y);
// Whether any port may snap (the settings); the game side skips collecting while false.
void setWanted(bool on);

}  // namespace recomp::menunav
