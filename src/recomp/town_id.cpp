// The loaded town's identity from the save (town_id.h).
#include "recomp/town_id.h"

#include "recomp/distrib_record.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

namespace recomp::town {

std::string keyInSave(std::string* readable) {
    const u32 save = rd32(VA(0x8074E960));
    if (save < 0x80000000u || save >= 0x93F00000u) return "";
    for (u32 slot = 0; slot < 4; slot++) {
        const u32 pd = save + 0x20 + slot * 0x86C0;
        const u32 player = rd16(pd + 0x7ED8);  // the resident's player id: 0 / FFFF = empty slot
        if (!player || player == 0xFFFF) continue;
        const std::string k = distrib::record::townKey(static_cast<const u8*>(hostp(pd + 0x7EC2)), readable);
        if (!k.empty()) return k;
    }
    return "";
}

}  // namespace recomp::town
