#pragma once
// Which City Folk release is running, and USA -> PAL / Japan / Korea addresses for host code
// (docs/SETUP.md "Version-aware code"; LM's src/recomp/version.h with four versions).
//
// Host code is written against the USA game (RUUE01 rev 1). Wrap every USA guest address it
// uses (a game function to call, a return address to compare, a static object) in VA(0x...):
// on the USA game the address itself, on the others the matching address from
// src/recomp/version_addrs.inc, which tools/recomp/gen_version_map.py generates by scanning the
// sources for VA(...) (code: the matched function + offset; data: decoded from the matched
// code that builds the address). Re-run the script after adding a VA(...); it fails on
// addresses it cannot map.
//
// Game functions are hooked by their USA names on every version: hooks.txt entries tagged
// `all` (the recompiler renames each version's matched function to the USA name), so one
// HLE(fn_<USA address>) serves all four. REL functions: `<module>::fn_<id>_<USA offset> all`.
//
// Struct field offsets are not mapped (function matching ignores load/store displacements):
// check each with `gen_version_map.py field <USA function> <offset>` and write
// VOFF(us, pal, jp, kr) where a version differs.
#include "recomp/ctx.h"

namespace recomp::ver {

enum class Id { USA, PAL, JPN, KOR };
Id id();  // from the running module's game id
inline bool usa() { return id() == Id::USA; }
const char* name();  // "USA" / "PAL" / "JPN" / "KOR"

// USA address -> running version's address. 0 (and a log line) when version_addrs.inc has no
// mapping for it on this version (stale table, or a function that version lacks).
u32 addr(u32 usAddr);

inline u32 off(u32 us, u32 pal, u32 jp, u32 kr) {
    switch (id()) {
    case Id::PAL: return pal;
    case Id::JPN: return jp;
    case Id::KOR: return kr;
    default: return us;
    }
}

}  // namespace recomp::ver

// Looked up once per use site (the version never changes while the game runs).
#define VA(usAddr)                                                  \
    ([]() -> ::recomp::u32 {                                        \
        static const ::recomp::u32 a = ::recomp::ver::addr(usAddr); \
        return a;                                                   \
    }())
#define VOFF(us, pal, jp, kr) (::recomp::ver::off((us), (pal), (jp), (kr)))
