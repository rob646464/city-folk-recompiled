#pragma once
// Runtime services shared by the HLE modules.
#include "recomp/ctx.h"

#include <string>

namespace recomp {

// OS timer: bus clock / 4. Wii: 243 MHz bus, 729 MHz CPU (the GameCube's 162 MHz bus gave
// LM/SMS a 40.5 MHz timer).
constexpr double kTimerClock = 60750000.0;
constexpr u32 kBusClock = 243000000;
constexpr u32 kCpuClock = 729000000;

// The recompiled game: set once at startup, before any guest code runs (module_loader.cpp).
extern const GameModule* g_module;

GuestFn lookupFunc(u32 addr);
[[noreturn]] void callMissing(Ctx& c, u32 addr);  // indirect call to an address with no function
// An indirect call to an address the DOL's table lacks: a linked REL module's function (rel.cpp)
// or callMissing.
void callOther(Ctx& c, u32 addr);
// REL modules (rel.cpp): the recompiled function at a code address of a linked module, or null.
GuestFn lookupRelFunc(u32 addr);
// "module:function+0xOFF" for an address inside a linked module, or "" (diagnostics).
std::string relSymbolize(u32 addr);void initTimeBase();
// Runtime-switchable instruction patches declared in tools/recomp/hooks.txt as
// "patch <id> <address> <instruction word> [us] [pal]" (id < kMaxPatches).
void setPatch(u32 id, bool on);
void initTrace();
void initWatch();
void watchRange(u32 addr, u32 len, const char* what);  // HLE bulk writes
void verifyLowMemory();  // hle_os.cpp: boot values in low memory intact (restored and logged if not)
u64 timeBase();
// The guest memory window (kHostMemSize bytes, zeroed). On Windows the pages are write-watched so
// guestWrittenRanges can skip pages nothing ever wrote (a scan then does not touch them).
u8* allocGuestMemory();
// Calls fn(host offset, length) for every range of the window that may hold data (written pages
// on Windows, the whole window elsewhere), in ascending order.
template <typename F>
void guestWrittenRanges(F&& fn);
void guestWrittenRangesImpl(void (*fn)(u32 offset, u32 len, void* user), void* user);
template <typename F>
void guestWrittenRanges(F&& fn) {
    guestWrittenRangesImpl([](u32 o, u32 l, void* u) { (*static_cast<F*>(u))(o, l); }, &fn);
}

namespace hw {
u32 viRead(u32 addr, int size);        // hle_vi.cpp
void fifoWrite(u32 value, int size);  // hle_gx.cpp
// Optional rewrite of values the game writes to the GX write-gather pipe (set only for the
// duration of a hook, e.g. hidden_mansion_hooks.cpp's mirrored screen-copy quad).
extern u32 (*fifoFilter)(u32 value, int size);
}  // namespace hw

}  // namespace recomp
