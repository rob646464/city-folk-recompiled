#pragma once
// Widescreen presentation for the recompiled game (docs/ENHANCEMENTS.md).
//
// The game renders a 640x480 logical frame. Aurora's STRETCH viewport policy maps
// that frame onto the whole window, so everything is drawn stretched; the GX HLE
// then undoes the stretch per draw:
//   - 3D (perspective projection, full-screen viewport): Hor+. The projection's
//     x row is scaled by factor() so the vertical FOV is kept and the horizontal
//     FOV grows. The game's frustum culling is widened to match (hook on
//     fn_80008D58).
//   - 2D (orthographic projection, full-screen viewport): kept at 4:3 and centered
//     ("pillarboxed" inside the wide frame), except draws that cover the whole
//     logical screen width: fills (fades) and blits of EFB copies stay
//     stretched to the window so they line up with the stretched frame; 4:3
//     background pictures follow backgroundMode().
//   - Draws into a partial viewport (render-to-texture) are left alone, so EFB
//     copies of those regions keep matching what was drawn. Partial viewports set
//     by a HUD element (picture-in-picture, see AnchorScope) follow its 2D instead.
#include "recomp/ctx.h"

namespace recomp::ws {

void init();          // host thread, before the first frame: viewport policy from --aspect
void beginFrame();    // host thread, before the guest runs a frame: picks up window resizes

// (4:3) / display aspect: 1 when the display is 4:3 or narrower or the mode is
// off, < 1 when wider. Constant while the guest runs a frame.
float factor();
// The aspect the game draws its frame for: 16:9 (Wii widescreen, the default) or 4:3 (F1 Original
// 4:3 / --aspect 4:3), fixed at start-up; SCGetAspectRatio reports it (hle_display.cpp).
bool gameWidescreen();
float baseAspect();
inline bool active() { return factor() < 0.9999f; }

// Field of view (settings().video.fovScale): the factor by which a perspective
// projection with this m[1][1] (= cot(fovY/2)) is scaled in x and y so its
// vertical FOV becomes fovY * fovScale. 1 when the scale is 1.
float fovRatio(float m11);
// True if full-screen 3D projections need adjusting (widescreen or FOV).
bool adjusting();

// Guest thread: the game's shadow light camera is being set up (fn_8005D984, shadows.cpp).
// Its culling planes are the light's frustum, not the view's, so fn_80008D58 leaves them alone.
void setShadowLightSetup(bool on);

// HUD elements (guest thread). The game functions that draw one in-game HUD element
// (the Game Boy Horror with everything on its screen, the health meter, ...) are hooked
// and run inside an AnchorScope, so the element moves as one unit:
//   - every 2D draw in the scope gets the same anchor: -1/0/+1 = moved to the left edge,
//     kept centered, moved to the right edge (the centered 4:3 projection shifted by
//     (1 - factor()) of the half-width). Only with settings().video.hudKeep4x3 off;
//   - a viewport smaller than the screen set in a scope (the GBH's live 3D view) gets the
//     same transform as the 2D around it, and so do the scissors set while that viewport
//     is current (the room portals drawn into it). This also applies with the HUD kept at
//     4:3 (anchor 0), where it keeps the 3D view inside the centered GBH frame.
// Everything else (menus, the GBH map screen, dialogue, markers placed at projected 3D
// positions) stays centered.
struct AnchorScope {
    explicit AnchorScope(int anchor);
    ~AnchorScope();
    AnchorScope(const AnchorScope&) = delete;
    AnchorScope& operator=(const AnchorScope&) = delete;
};
// A scope whose element is made of separate pieces on both sides (the room name: floor at
// the top right, name at the top left): each 2D draw goes to the edge on its own side.
constexpr int kAnchorBySide = 2;
// Anchor of the innermost scope (-1/0/+1 or kAnchorBySide); 0 outside scopes or with
// anchoring off.
int hudAnchor();
bool inHudScope();   // inside any scope (widescreen active), and not in a RenderToTextureScope
// The Wii Remote pointer's x range in the game's KPAD units (docs/CONTROLS.md "Pointer in
// ultrawide"): 1 normally; 1 / factor() while the HUD is anchored to the window edges
// (widescreen active and "Keep HUD at 16:9" off), so the pointer reaches the whole window and the
// HUD elements drawn at its edges. Live (follows the window and the setting).
float pointerRangeX();
// True while the frame loop sets up view 0, the main view (fn_800115F8(0)). A main view smaller
// than the screen (the game's unused bordered view, patch 50) follows the centered frame there;
// the same rectangle set again later for that view's passes (ghost layer, effects) follows too
// (hle_gx.cpp), so the whole view stays in one place.
bool inMainViewSetup();
void setMainViewSetup(bool on);  // the game's hooks (src/game) around its main view setup

// A render-to-texture pass run from inside a HUD element's scope (the event-scene renderer
// fn_80114E18 draws the door knob's reflection into a 128x128 viewport at the EFB origin and
// copies that rectangle with GXCopyTex). Its partial viewport must stay where the game put it,
// like every render-to-texture viewport, or the copy reads a different rectangle and the pass
// is left on screen: inside this scope viewports do not follow the HUD scope around it.
struct RenderToTextureScope {
    RenderToTextureScope();
    ~RenderToTextureScope();
    RenderToTextureScope(const RenderToTextureScope&) = delete;
    RenderToTextureScope& operator=(const RenderToTextureScope&) = delete;
};

// The circle wipe (City Folk: mWipeFader_c::draw, one quad with a clamped circle texture over the
// game's 16:9 2D frame; cf_hud.cpp). Inside a WipeScope (widescreen active) every 2D draw is
// centered in the game's frame, and its full-frame quad is widened to the display's edges with
// its texture coordinates extrapolated (hle_gx.cpp widenWipeHeld), so the mask covers the whole
// width and the circle stays round. (LM: the iris wipes' J2DFillBox boxes.)
struct WipeScope {
    WipeScope();
    ~WipeScope();
    WipeScope(const WipeScope&) = delete;
    WipeScope& operator=(const WipeScope&) = delete;
};
bool inWipe();

// A backdrop the game places in front of its camera, sized to cover the game's own frame and no
// more (City Folk: the sky and cloud planes of dSky_c, 1300 units ahead in view space; cf_tags.cpp).
// Hor+ / a wider FOV see past its edges (black corners above the horizon). Inside a BackdropScope
// (widescreen active, or a field of view wider than the game's) every 3D draw keeps the game's own projection, i.e. the game's
// frame stretched across the window, which the backdrop always covers; and every texture matrix
// loaded (GXLoadTexMtxImm) is widened about the texture coordinate `uCentre` (the s the backdrop
// has at the centre of the picture) by the same 1 / factor(), so its texture continues sideways at
// the game's density instead of being stretched. Valid for backdrops whose s is proportional to x
// about the centre on every row (the sky gradient, the cloud layer's perspective mapping); t is
// left alone (the game's framing vertically). Nothing changes at the base aspect without FOV.
struct BackdropScope {
    explicit BackdropScope(float uCentre);
    ~BackdropScope();
    BackdropScope(const BackdropScope&) = delete;
    BackdropScope& operator=(const BackdropScope&) = delete;
};
bool inBackdrop();
float backdropUCentre();

// 2-player split screen (coop_fixes.cpp: two side-by-side halves). A HUD element drawn for
// one half runs inside a ShiftScope: every 2D draw (and a partial viewport/scissor set in it)
// is moved right by dx logical pixels and kept centered (anchor 0), at any aspect: with
// dx = +320 an element of the left 4:3 half lands in the right half of the window, with
// dx = -320 an element of the right half lands in the left half. Works with widescreen off.
struct ShiftScope {
    explicit ShiftScope(int dx);
    ~ShiftScope();
    ShiftScope(const ShiftScope&) = delete;
    ShiftScope& operator=(const ShiftScope&) = delete;
};
int hudShift();  // dx of the innermost ShiftScope, 0 outside
// A viewport (logical x, width) that is one half of the 2-player split screen: its 3D gets
// the same Hor+ / FOV adjustments as a full-screen view.
bool splitHalf(float x, float w);

// Full-screen 2D pictures (title backdrops, menu backgrounds), --ws-backgrounds.
enum class Background { Pillarbox, Stretch, Zoom };
Background backgroundMode();

}  // namespace recomp::ws
