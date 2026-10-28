// USA -> other versions address table for VA(...) (version.h, docs/SETUP.md "Version-aware code").
#include "recomp/version.h"

#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <algorithm>
#include <iterator>

namespace recomp::ver {
namespace {
struct Row {
    u32 v[4];  // USA, PAL, Japan, Korea
};
constexpr Row kRows[] = {
#include "recomp/version_addrs.inc"
};
}  // namespace

Id id() {
    const char c = g_module && g_module->gameId && g_module->gameId[0] ? g_module->gameId[3] : 'E';
    return c == 'P' ? Id::PAL : c == 'J' ? Id::JPN : c == 'K' ? Id::KOR : Id::USA;
}

const char* name() {
    static const char* const kNames[] = {"USA", "PAL", "JPN", "KOR"};
    return kNames[int(id())];
}

u32 addr(u32 usAddr) {
    const Id v = id();
    if (v == Id::USA) return usAddr;
    const auto it = std::lower_bound(std::begin(kRows), std::end(kRows), usAddr,
                                     [](const Row& r, u32 a) { return r.v[0] < a; });
    if (it == std::end(kRows) || it->v[0] != usAddr) {
        log("[version] USA address %08X has no %s mapping: re-run tools/recomp/gen_version_map.py", usAddr, name());
        return 0;
    }
    if (!it->v[int(v)]) log("[version] USA address %08X does not exist in the %s game", usAddr, name());
    return it->v[int(v)];
}

}  // namespace recomp::ver
