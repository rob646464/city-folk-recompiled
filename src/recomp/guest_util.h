#pragma once
// Small helpers for host code that calls guest functions from inside a hook (guest thread).
#include "recomp/hle.h"

#include <bit>
#include <initializer_list>

namespace recomp::gu {

// Calls a guest function with integer arguments in r3.. and float arguments in f1..;
// the caller's registers (including the stack pointer) are left untouched. Returns r3.
inline u32 call(Ctx& c, u32 fn, std::initializer_list<u32> ints = {}, std::initializer_list<double> floats = {}) {
    const Ctx saved = c;
    int r = 3;
    for (u32 v : ints) c.r[r++] = v;
    int f = 1;
    for (double v : floats) {
        c.f[f] = v;
        c.ps1[f] = v;
        f++;
    }
    c.call(fn);
    const u32 result = c.r[3];
    c = saved;
    return result;
}

// Same, by symbol name (looked up once per call site through a static).
#define GU_SYM(name)                                     \
    ([]() -> ::recomp::u32 {                             \
        static const ::recomp::u32 a = ::recomp::guestSymbol(name); \
        return a;                                        \
    }())

inline float rdf(u32 a) { return std::bit_cast<float>(rd32(a)); }
inline void wrf(u32 a, float v) { wr32(a, std::bit_cast<u32>(v)); }
inline bool inMem1(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }

}  // namespace recomp::gu
