// HLE: ARAM (16 MiB auxiliary memory) and the ARQ DMA queue. Transfers
// complete immediately; their callbacks are delivered as interrupts.
#include "recomp/audio.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <cstring>
#include <vector>

namespace recomp {
namespace {

constexpr u32 kAramSize = 0x01000000;
constexpr u32 kAramBase = 0x4000;  // the first 16 KiB are reserved for the audio DSP
std::vector<u8> sAram(kAramSize);
u32 sAramTop = kAramBase;

u8* aram(u32 addr, u32 len) {
    if (addr >= kAramSize || len > kAramSize - addr) fatal("ARAM access out of range: %08X+%X", addr, len);
    return sAram.data() + addr;
}

}  // namespace

// Read access for the audio DSP stand-in (src/recomp/audio_dsp.cpp).
const u8* audio::aramRead(u32 addr, u32 len) {
    if (addr >= kAramSize || len > kAramSize - addr) return nullptr;
    return sAram.data() + addr;
}

HLE(ARInit) { c.r[3] = kAramBase; }
HLE(ARGetBaseAddress) { c.r[3] = kAramBase; }
HLE(ARGetSize) { c.r[3] = kAramSize; }
HLE(ARAlloc) {
    const u32 len = c.r[3];
    c.r[3] = sAramTop;
    sAramTop += len;
}
HLE(ARQInit) {}

// ARQPostRequest(request, owner, type, priority, source, dest, length, callback)
HLE(ARQPostRequest) {
    const u32 req = c.r[3], type = c.r[5], src = c.r[7], dst = c.r[8], len = c.r[9], cb = c.r[10];
    wr32(req + 0x00, 0);
    wr32(req + 0x04, c.r[4]);
    wr32(req + 0x08, type);
    wr32(req + 0x0C, c.r[6]);
    wr32(req + 0x10, src);
    wr32(req + 0x14, dst);
    wr32(req + 0x18, len);
    wr32(req + 0x1C, cb);
    if (type == 0) std::memcpy(aram(dst, len), hostp(src), len);  // main memory -> ARAM
    else {
        watchRange(dst, len, "ARQ");
        std::memcpy(hostp(dst), aram(src, len), len);
    }            // ARAM -> main memory
    if (cb) sched::postIrq([req, cb](Ctx& ic) { guestCall(ic, cb, req); });
}

}  // namespace recomp
