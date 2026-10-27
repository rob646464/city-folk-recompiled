#pragma once
// CRT post-process filter: an original WGSL shader chain (scanline beams, phosphor
// mask, halation/diffusion/bloom, convergence, geometry) run by Aurora's postfx hook
// on the game image only, so the ImGui overlay stays sharp. Settings: port::CrtSettings.
namespace port {
struct CrtSettings;
enum class CrtPreset;
enum class CrtSignal;
}

namespace recomp::crt {
void update();                                         // host thread, once per frame: pushes settings to Aurora
bool drawMenu();                                       // Visuals tab section; returns true when a setting changed
void applyPreset(port::CrtSettings& s, port::CrtPreset preset);  // tube preset (keeps the signal)
void applySignal(port::CrtSettings& s, port::CrtSignal signal);  // signal + its default parameters
}  // namespace recomp::crt
