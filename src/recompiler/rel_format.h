#pragma once
// The Dolphin/Revolution SDK's relocatable module format (.rel, written by makerel), as far as
// the recompiler (src/recompiler) and the OSLink HLE (src/recomp/rel.cpp) need it. Header only,
// no dependencies, shared by both sides so they agree on the module checksum.
//
// Header (big-endian; OSModuleHeader):
//   0x00 id            0x04 next          0x08 prev          0x0C numSections
//   0x10 sectionInfoOffset                0x14 nameOffset    0x18 nameSize
//   0x1C version (1-3) 0x20 bssSize       0x24 relOffset     0x28 impOffset     0x2C impSize
//   0x30 prologSection, epilogSection, unresolvedSection, bssSection (u8 each)
//   0x34 prolog        0x38 epilog        0x3C unresolved
//   v2+: 0x40 align    0x44 bssAlign      v3+: 0x48 fixSize
// Section table: numSections x {u32 offset (bit 0: executable), u32 size}; offset 0 with a
// size is the bss section (placed by OSLink).
// Import table: impSize/8 x {u32 module id (0 = the DOL), u32 offset of its relocation list}.
// Relocation list: {u16 offset (from the previous entry), u8 type, u8 section, u32 addend};
// R_DOLPHIN_SECTION switches the patched section (offset restarts at 0), R_DOLPHIN_END ends.
#include <cstddef>
#include <cstdint>

namespace relfmt {

enum : std::uint8_t {
    R_PPC_NONE = 0,
    R_PPC_ADDR32 = 1,
    R_PPC_ADDR24 = 2,
    R_PPC_ADDR16 = 3,
    R_PPC_ADDR16_LO = 4,
    R_PPC_ADDR16_HI = 5,
    R_PPC_ADDR16_HA = 6,
    R_PPC_ADDR14 = 7,
    R_PPC_ADDR14_BRTAKEN = 8,
    R_PPC_ADDR14_BRNTAKEN = 9,
    R_PPC_REL24 = 10,
    R_PPC_REL14 = 11,
    R_PPC_REL14_BRTAKEN = 12,
    R_PPC_REL14_BRNTAKEN = 13,
    R_DOLPHIN_NOP = 201,
    R_DOLPHIN_SECTION = 202,
    R_DOLPHIN_END = 203,
};

// Key of a hooked REL function's original body in GameModule::orig (func_table.cpp kOrigTable),
// next to the DOL originals: 0x40000000 | module id << 19 | section << 16 | offset / 4. No DOL
// address (0x80000000+) can equal it. callOriginal(c, "<module>::<name>") looks it up (runtime.cpp).
constexpr std::uint32_t relOrigKey(std::uint32_t module, std::uint32_t section, std::uint32_t offset) {
    return 0x40000000u | (module & 0xFF) << 19 | (section & 7) << 16 | (offset >> 2 & 0xFFFF);
}

inline std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

// FNV-1a over the bytes of every executable section, as stored in the file: the code OSLink
// has not patched yet. The runtime compares it with the module the game links (rel.cpp), so a
// game module built from other REL files is noticed instead of running the wrong code.
// `read(offset, out, n)` fetches file bytes (the runtime reads them from guest memory).
template <typename Read>
std::uint32_t codeChecksum(Read&& read, std::uint32_t numSections, std::uint32_t sectionTable) {
    std::uint32_t h = 2166136261u;
    for (std::uint32_t i = 0; i < numSections; i++) {
        std::uint8_t e[8];
        read(sectionTable + 8 * i, e, 8);
        const std::uint32_t off = be32(e), size = be32(e + 4);
        if (!(off & 1) || !size) continue;
        std::uint8_t buf[256];
        for (std::uint32_t p = 0; p < size; p += sizeof(buf)) {
            const std::uint32_t n = size - p < sizeof(buf) ? size - p : std::uint32_t(sizeof(buf));
            read((off & ~3u) + p, buf, n);
            for (std::uint32_t k = 0; k < n; k++) h = (h ^ buf[k]) * 16777619u;
        }
    }
    return h;
}

inline std::uint32_t fileCodeChecksum(const std::uint8_t* rel, std::size_t size) {
    if (size < 0x40) return 0;
    const std::uint32_t n = be32(rel + 0x0C), table = be32(rel + 0x10);
    return codeChecksum(
        [&](std::uint32_t off, std::uint8_t* out, std::uint32_t len) {
            for (std::uint32_t k = 0; k < len; k++) out[k] = off + k < size ? rel[off + k] : 0;
        },
        n, table);
}

}  // namespace relfmt
