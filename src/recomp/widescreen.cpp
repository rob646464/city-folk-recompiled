#include "recomp/widescreen.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/game.h"
#include "recomp/guest_util.h"

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
namespace tags {
bool textBoxShown();  // tags.cpp: a message window is on screen
int textMode();       // tags.cpp: 2 text boxes blended as one layout, 1 locked to the game frame, 0 neither
}
}
#include <cmath>
#include <vector>

namespace recomp::ws {
namespace {

// The aspect the game draws its frame for (Wii: the console's TV setting, SCGetAspectRatio):
// 16:9 (anamorphic in the 640-wide EFB) unless F1 / --aspect asks for 4:3. Read by the game at
// boot, so a change applies after a restart.
float sBaseAspect = 16.0f / 9.0f;
std::atomic<float> sFactor{1.0f};
std::atomic<float> sFov{1.0f};
std::atomic<bool> sAnchorHud{false};
bool sOriginal = false;  // Aurora fits the frame (at the base aspect) into the window
bool sPolicySet = false;

float factorFor(float aspect) { return aspect > sBaseAspect ? sBaseAspect / aspect : 1.0f; }

}  // namespace

// --aspect on the command line wins; otherwise the Visuals tab's setting (live).
float configuredAspect() {
    if (port::options().aspectRatio > 0.0f) return port::options().aspectRatio;
    switch (port::settings().video.aspect) {
    case port::AspectMode::Original4x3: return 4.0f / 3.0f;
    case port::AspectMode::Wide16x9: return 16.0f / 9.0f;
    case port::AspectMode::Wide16x10: return 16.0f / 10.0f;
    case port::AspectMode::Ultra21x9: return 21.0f / 9.0f;
    case port::AspectMode::Ultra32x9: return 32.0f / 9.0f;
    default: return 0.0f;  // match the window
    }
}

bool gameWidescreen() {
    static const bool wide = [] {
        const float a = configuredAspect();
        return !(a > 0.0f && a < 1.5f);
    }();
    return wide;
}
float baseAspect() { return gameWidescreen() ? 16.0f / 9.0f : 4.0f / 3.0f; }

namespace {
float windowAspect() {
    int w = 0, h = 0;
    if (SDL_Window* win = aurora_get_window()) SDL_GetWindowSizeInPixels(win, &w, &h);
    return w > 0 && h > 0 ? float(w) / float(h) : 0.0f;
}
}  // namespace

// The target aspect (fixed, or the window's) narrower than or equal to the base aspect: Aurora fits
// the frame at the base aspect into the window (letter/pillarbox); wider: stretched + Hor+.
void applyPolicy(float aspect) {
    sBaseAspect = baseAspect();
    const float target = aspect > 0.0f ? aspect : windowAspect();
    const bool original = target > 0.0f && target <= sBaseAspect + 1e-3f;
    if (original != sOriginal || !sPolicySet) {
        sOriginal = original;
        sPolicySet = true;
        VISetDisplayAspect(sBaseAspect);  // Aurora 0301: the fit is to the display aspect, not 640x456
        AuroraSetViewportPolicy(sOriginal ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
        log("[ws] aspect %s, game frame %s", sOriginal ? "fitted (base aspect)" : aspect > 0.0f ? "fixed" : "auto (window)",
            gameWidescreen() ? "16:9" : "4:3");
    }
}

void init() { applyPolicy(configuredAspect()); }

void beginFrame() {
    const float aspect = configuredAspect();
    applyPolicy(aspect);
    float f = 1.0f;
    if (!sOriginal) f = factorFor(aspect > 0.0f ? aspect : windowAspect());
    const float old = sFactor.exchange(f);
    if (old != f) log("[ws] widescreen factor %.4f (aspect %.4f)", f, sBaseAspect / f);
    sFov = std::clamp(port::settings().video.fovScale, 0.5f, 2.0f);
    sAnchorHud = !port::settings().video.hudKeep4x3;
}

float fovRatio(float m11) {
    const float k = sFov.load(std::memory_order_relaxed);
    if (k == 1.0f || m11 <= 0.0f) return 1.0f;
    const float half = std::atan(1.0f / m11);
    const float scaled = std::clamp(half * k, 0.01f, 1.5f);
    return std::tan(half) / std::tan(scaled);
}

bool adjusting() { return active() || sFov.load(std::memory_order_relaxed) != 1.0f; }

namespace {
bool sShadowLightSetup = false;
}  // namespace
void setShadowLightSetup(bool on) { sShadowLightSetup = on; }


float factor() { return sFactor.load(std::memory_order_relaxed); }

Background backgroundMode() { return Background(port::options().wsBackgrounds); }

// HUD anchoring (widescreen.h, AnchorScope). The game's hooks (src/game) open the scopes around
// the functions that draw one HUD element each. The guest thread is the only caller.
namespace {
int sAnchorStack[16];
int sAnchorDepth = 0;
int sRttDepth = 0;  // RenderToTextureScope nesting
int sShift[8];
int sShiftDepth = 0;
}  // namespace

ShiftScope::ShiftScope(int dx) {
    if (sShiftDepth < int(std::size(sShift))) sShift[sShiftDepth] = dx;
    sShiftDepth++;
}
ShiftScope::~ShiftScope() { sShiftDepth--; }
int hudShift() { return sShiftDepth > 0 ? sShift[std::min(sShiftDepth, int(std::size(sShift))) - 1] : 0; }

bool splitHalf(float x, float w) {
    return game::splitScreen() && std::fabs(w - 320.0f) < 1.0f && (std::fabs(x) < 1.0f || std::fabs(x - 320.0f) < 1.0f);
}

AnchorScope::AnchorScope(int anchor) {
    if (sAnchorDepth < int(std::size(sAnchorStack))) sAnchorStack[sAnchorDepth] = anchor;
    sAnchorDepth++;
}
AnchorScope::~AnchorScope() { sAnchorDepth--; }

int hudAnchor() {
    if (hudShift() != 0) return 0;  // a half of the split screen: centered, then moved
    if (sAnchorDepth <= 0 || !sAnchorHud.load(std::memory_order_relaxed) || !active()) return 0;
    return sAnchorStack[std::min(sAnchorDepth, int(std::size(sAnchorStack))) - 1];
}

float pointerRangeX() {
    const float f = factor();
    return sAnchorHud.load(std::memory_order_relaxed) && f < 0.9999f && f > 0.05f ? 1.0f / f : 1.0f;
}

bool inHudScope() {
    return sRttDepth == 0 && ((sAnchorDepth > 0 && active()) || hudShift() != 0);
}

RenderToTextureScope::RenderToTextureScope() { sRttDepth++; }
RenderToTextureScope::~RenderToTextureScope() { sRttDepth--; }

// The game's frame loop setting up its main view (src/game sets this around it).
namespace {
bool sMainViewSetup = false;
int sWipeDepth = 0;
}  // namespace
bool inMainViewSetup() { return sMainViewSetup; }
void setMainViewSetup(bool on) { sMainViewSetup = on; }
WipeScope::WipeScope() { sWipeDepth++; }
WipeScope::~WipeScope() { sWipeDepth--; }
bool inWipe() { return sWipeDepth > 0 && active(); }

}  // namespace recomp::ws
