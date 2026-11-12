#pragma once
#include <cstdint>

namespace recomp::gx {
// Display copy settings the game made (GXSetCopyFilter, GXSetDispCopyGamma, GXSetDispCopySrc,
// GXSetCopyClamp), applied on presentation by the display copy filter pass (crt.cpp).
struct DisplayCopy {
    uint8_t vfilter[7] = {0, 0, 21, 22, 21, 0, 0};  // vertical filter taps (sum 64)
    uint8_t gamma = 0;                              // GXGamma: 0 = 1.0, 1 = 1.7, 2 = 2.2
    uint8_t clamp = 3;                              // GXFBClamp bits (top 1, bottom 2)
    uint16_t efbHeight = 480;                       // display copy source height (EFB lines)
};
const DisplayCopy& displayCopy();
// F1 > Visuals > Rendering accuracy options -> the flags to use (host thread, every frame; pushed to
// Aurora with the enhanced-lighting configuration by precache::applyGraphics, called from
// enh::applySettings, which runs right after this).
void applyAccuracySettings();
uint32_t requestedAccuracy();
// The flags the F1 options give with enhanced lighting on / off (no test A/B alternation).
uint32_t accuracyFlagsFor(bool enhancedOn);
// Enhanced lighting is on: "EFB color depth" (and "6-bit blending") are off whatever the
// setting says (their F1 toggles are greyed out; docs/RENDERING.md).
bool enhancedOverridesEfbDepth();
// Effective "Smooth color banding" (Aurora 0190): 0 off, 1 smooth + sub-step noise (the option),
// 2 smooth without the noise (A/B test only). The setting, forced on while enhanced lighting is
// on; CF_SMOOTH_BANDING=<0-2> overrides (test aid).
int smoothBandingLevel();
// Enhanced lighting is what forces smoothing on (for the F1 menu note).
bool smoothBandingForced();

// Re-evaluate the per-draw state (projection, enhanced-lighting draw class) before the next
// draw written straight into the FIFO by game code.
void recheckDrawState();
// Closes a draw opened by GXBegin whose vertices were written through the FIFO.
void endPendingDraw();
// Malformed FIFO data (Aurora 0235): the guest GX call sites before each FIFO stream position, for
// naming the caller when Aurora reports a bad command (docs/RENDERING.md "Malformed FIFO data").
void noteCallSite(uint32_t lr);
void installFifoErrorReport();
// Model viewer: draw with textures replaced by white (TEV colour inputs / modes).
void setUntextured(bool on);
// The guest finished a frame (VISetNextFrameBuffer): closes any open draw.
void endFrame();
// Fast-forward (skip.cpp): the current guest frame is not shown, so its draws and
// copies are dropped (state changes still reach Aurora; display lists are still recorded).
extern bool g_discardDraws;
// Current vertex layout (hle_gx shadows): bytes attribute `attr` takes in a vertex of
// format `fmt` (-1 unknown), and its GXAttrType.
int vtxAttrSize(int fmt, int attr);
unsigned char vtxDesc(int attr);
// The VAT entry of `attr` in format `fmt` (component count, GXCompType, fraction bits).
bool vtxAttrFormat(int fmt, int attr, unsigned char& cnt, unsigned char& type, unsigned char& frac);
// A CP register write found in a display list or raw FIFO data (LOAD_CP_REG): VCD (0x50, 0x60) and
// VAT (0x70-0x9F) update the layout above (NW4R g3d sets its shapes' layout in display lists).
void cpRegWrite(unsigned char reg, unsigned int value);
// An active TEV stage samples an EFB copy.
bool samplesCopy();
}  // namespace recomp::gx
