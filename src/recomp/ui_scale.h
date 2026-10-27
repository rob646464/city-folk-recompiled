#pragma once
// Size of every Dear ImGui window the port draws (F1 menu, overlays, setup screen): the "Menu size"
// option (settings interface.menu_scale, F1 > Visuals > General; issue #20). See ui_scale.cpp.

#include <imgui.h>

namespace recomp::ui {

// Menu size steps in percent (index 0 of the menu's slider is Auto). Each one rebuilds the font at
// round(13 x step) pixels, so text stays as sharp as the original 13 px font.
inline constexpr int kSteps[] = {75, 100, 125, 150, 175, 200, 250, 300};
inline constexpr int kStepCount = int(sizeof(kSteps) / sizeof(kSteps[0]));

// Which automatic size: the game (menu over the game) or the setup screen (a full-window page,
// larger by default).
enum class Context { Game, Setup };

// Registers the frame callback with Aurora (idempotent). The size is applied before each ImGui
// frame, so a change shows from the next frame on.
void install(Context context);

// Scale in effect for this frame (1 = the original 13 px font and ImGui's default style), in
// ImGui units (logical pixels).
float scale();
// The default font 1.5 times larger (rasterized at that size), for full-screen notices such as
// "Preparing shaders": ImGui::PushFont(largeFont()). Null before the first frame.
ImFont* largeFont();
// What Auto picks now (window height, display scaling, Steam Deck), and the largest size that
// still fits the window (a larger setting is limited to it).
int autoPercent();
int fitPercent();
// Percent in effect (the setting, or Auto's choice, limited to fitPercent).
int percent();

// Before ImGui::Begin of a window the player can resize (imgui.ini keeps its size): default and
// minimum size (at 100%) scale with the menu size, a saved size is rescaled when the menu size
// changes, and the window is kept on the screen.
void beforeWindow(const char* name, ImVec2 defaultSize, ImVec2 minSize);
// The next size change rescales that window about this point (e.g. the mouse on the menu-size
// control), so what is under it stays there; otherwise about the window's centre.
void setAnchor(const char* window, ImVec2 point);

// Font size in logical pixels for text drawn directly with ImDrawList (overlays): px x scale().
inline float px(float basePx) { return basePx * scale(); }

}  // namespace recomp::ui
