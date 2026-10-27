// HLE: the display settings a Wii game reads from the system configuration (SC, SYSCONF). Only
// the ones the port decides itself live here; the rest of SC (language, sound mode, sensor bar,
// EULA...) belongs to the SC/NAND HLE.
//   - SCGetAspectRatio: the TV's aspect. City Folk draws its 640x456 frame for it (anamorphic at
//     16:9). 16:9 unless F1 > Visuals > Aspect ratio is "Original 4:3" (or --aspect 4:3); read at
//     boot, so a change applies after a restart. Widescreen presentation on top: widescreen.cpp.
#include "recomp/hle.h"
#include "recomp/widescreen.h"

namespace recomp {

namespace {
constexpr u32 kScAspect4x3 = 0, kScAspect16x9 = 1;  // SC_ASPECT_RATIO_*
}

HLE(SCGetAspectRatio) {
    static bool sLogged = false;
    const bool wide = ws::gameWidescreen();
    if (!sLogged) log("[sc] SCGetAspectRatio: %s", wide ? "16:9" : "4:3");
    sLogged = true;
    c.r[3] = wide ? kScAspect16x9 : kScAspect4x3;
}

}  // namespace recomp
