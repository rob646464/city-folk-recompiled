// RevoEX MP / MPDL (DS wireless) on the host (docs/MOVE_IN.md "DS wireless"). See mp.h.
//
// On a Wii these libraries drive IOS's /dev/net/wd/command (the Wii's DS-compatible radio). The
// port has no such radio: every API function the game calls is answered here, and the DS side is
// a virtual child (mp::Child). Functions are hooked by their USA addresses (hooks.txt, `all`);
// their meaning comes from the libraries' code and from how City Folk calls them (USA addresses):
//
//   MPDL (DS Download Play; City Folk's wrapper fn_800E4344 polls these once a frame)
//     fn_8040141C (cfg)     startup: offer the program in cfg (+0x0C ggid); 0 = ok
//     fn_8040171C ()        cleanup; 0 = ok
//     fn_80401A2C ()        aid mask of children that hold the program (state 2..6 per child)
//     fn_804017C8 ()        boot those children; returns their aid mask
//     fn_80401B04 ()        aid mask of booted children
//     fn_8040193C (aid, o)  child info (0x28 bytes; MAC at +0x1E); 1 = present
//   MP (data; City Folk's wrapper fn_800E3EE4 / fn_800E40C4)
//     fn_803FAA64 (cfg)     startup: cfg+0x10 ggid, cfg+0x194 indication callback (type, ind*)
//     fn_803FAC4C ()        cleanup
//     fn_803FBC4C (p, n)    user game info in the beacon (n <= 0x70)
//     fn_803FE8FC (cb, x)   another callback (City Folk passes an empty function)
//     fn_803FCA0C ()        disconnect every child; 0 = done
//     fn_803FBDB8 (data, len, aids, port, result*, cb, arg)  send; cb(result, arg) when sent
//   Indications to cfg+0x194 (type, ind): 4 = child connected (ind+0x10 aid, +0x18 MAC),
//   5 = disconnected (+0x10 aid), 3 = data (+0x10 aid, +0x14 data*, +0x18 length).
//
// The libraries call back from their own threads; here the indications and send completions
// queue up and mp::deliver() runs them on the game's thread at its MP poll (outside the link's
// mutex), a few packets per frame (cf_move_in.cpp).
#include "recomp/mp.h"

#include "recomp/guest_util.h"
#include "recomp/hle.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace recomp::mp {
namespace {

using clk = std::chrono::steady_clock;

struct SendDone {
    u32 cb, arg, result;
};

struct State {
    std::mutex m;
    recomp::mp::Child* child = nullptr;
    // MPDL
    bool dl = false;
    u32 dlGgid = 0;
    bool booting = false;
    clk::time_point bootAt;
    // MP
    bool mp = false;
    u32 mpGgid = 0, indCb = 0;
    bool connected = false;
    bool connectQueued = false;
    clk::time_point connectAt;
    std::deque<SendDone> sends;
    bool disconnectPending = false;
    u32 indBuf = 0, dataBuf = 0;
};
State& st() {
    static State s;
    return s;
}

constexpr int kPacketsPerFrame = 4;
constexpr u32 kDataBufSize = 0x800;
const u8 kChildMac[6] = {0x00, 0x09, 0xBF, 0x43, 0x46, 0x01};  // Nintendo OUI, made-up device

void ensureBuffers(State& s) {
    if (!s.indBuf) s.indBuf = hleAlloc(0x40);
    if (!s.dataBuf) s.dataBuf = hleAlloc(kDataBufSize);
}

double sinceStart() {
    static const clk::time_point t0 = clk::now();
    return std::chrono::duration<double>(clk::now() - t0).count();
}

#define MPLOG(...)                                         \
    do {                                                   \
        if (::recomp::mp::logOn()) {                       \
            std::printf("[mp %8.3f] ", ::recomp::mp::sinceStart()); \
            std::printf(__VA_ARGS__);                      \
            std::printf("\n");                             \
            std::fflush(stdout);                           \
        }                                                  \
    } while (0)

}  // namespace

bool logOn() {
    // also on with CF_MOVE_IN_LOG (the move-in's own log, cf_move_in.cpp)
    static const bool on = [] {
        for (const char* n : {"CF_MP_LOG", "CF_MOVE_IN_LOG"}) {
            const char* e = std::getenv(n);
            if (e && *e && *e != '0') return true;
        }
        return false;
    }();
    return on;
}

void setChild(Child* child) {
    std::lock_guard lk(st().m);
    st().child = child;
}

void deliver(Ctx& c) {
    State& s = st();
    std::unique_lock lk(s.m);
    if (!s.mp || !s.indCb) return;
    ensureBuffers(s);
    const u32 ind = s.indBuf;
    // Connection of the booted DS program.
    if (!s.connected && s.child && !s.connectQueued && s.child->connects(s.mpGgid)) {
        s.connectQueued = true;
        s.connectAt = clk::now() + std::chrono::milliseconds(1200);  // the DS program starts its MP child
    }
    if (!s.connected && s.connectQueued && clk::now() >= s.connectAt) {
        s.connected = true;
        const u32 cb = s.indCb;
        lk.unlock();
        std::memset(hostp(ind), 0, 0x40);
        wr32(ind + 0x10, 1);
        std::memcpy(hostp(ind + 0x18), kChildMac, 6);
        MPLOG("indication 4: DS connected as aid 1");
        gu::call(c, cb, {4u, ind});
        lk.lock();
    }
    // Send completions (before the child's answers to them).
    while (!s.sends.empty()) {
        const SendDone d = s.sends.front();
        s.sends.pop_front();
        lk.unlock();
        if (d.result) wr32(d.result, 0);
        if (d.cb) gu::call(c, d.cb, {0u, d.arg});
        lk.lock();
        if (!s.mp) return;
    }
    if (s.disconnectPending) {
        s.disconnectPending = false;
        const u32 cb = s.indCb;
        lk.unlock();
        std::memset(hostp(ind), 0, 0x40);
        wr32(ind + 0x10, 1);
        MPLOG("indication 5: DS disconnected");
        gu::call(c, cb, {5u, ind});
        return;
    }
    if (!s.connected || !s.child) return;
    for (int i = 0; i < kPacketsPerFrame; i++) {
        std::vector<u8> pkt;
        if (!s.child->nextPacket(pkt)) break;
        if (pkt.size() > kDataBufSize) pkt.resize(kDataBufSize);
        const u32 cb = s.indCb;
        lk.unlock();
        std::memcpy(hostp(s.dataBuf), pkt.data(), pkt.size());
        std::memset(hostp(ind), 0, 0x40);
        wr32(ind + 0x10, 1);
        wr32(ind + 0x14, s.dataBuf);
        wr32(ind + 0x18, u32(pkt.size()));
        gu::call(c, cb, {3u, ind});
        lk.lock();
        if (!s.mp || !s.connected) return;
        if (s.sends.size()) break;  // the game answered: let its completion go first next frame
    }
}

}  // namespace recomp::mp

using namespace recomp;
using recomp::mp::st;

// ---- MPDL ----------------------------------------------------------------------------------

HLE(fn_8040141C) {
    auto& s = st();
    const u32 cfg = c.r[3];
    {
        std::lock_guard lk(s.m);
        s.dl = true;
        s.dlGgid = rd32(cfg + 0xC);
        s.booting = false;
    }
    MPLOG("MPDL startup: ggid %06X, program image %08X", s.dlGgid, rd32(cfg + 0x2C));
    if (s.child) s.child->downloadStarted(s.dlGgid);
    c.r[3] = 0;
}

HLE(fn_8040171C) {
    auto& s = st();
    MPLOG("MPDL cleanup");
    {
        std::lock_guard lk(s.m);
        s.dl = false;
        s.booting = false;
    }
    if (s.child) s.child->downloadEnded();
    c.r[3] = 0;
}

HLE(fn_80401A2C) {
    auto& s = st();
    std::lock_guard lk(s.m);
    c.r[3] = s.dl && s.child && s.child->hasProgram(s.dlGgid) ? 2u : 0u;
}

HLE(fn_804017C8) {
    auto& s = st();
    std::lock_guard lk(s.m);
    const bool have = s.dl && s.child && s.child->hasProgram(s.dlGgid);
    if (have && !s.booting) {
        s.booting = true;
        s.bootAt = recomp::mp::clk::now() + std::chrono::milliseconds(800);
        MPLOG("MPDL: booting the DS program on aid 1");
    }
    c.r[3] = have ? 2u : 0u;
}

HLE(fn_80401B04) {
    auto& s = st();
    std::lock_guard lk(s.m);
    c.r[3] = s.dl && s.booting && recomp::mp::clk::now() >= s.bootAt ? 2u : 0u;
}

HLE(fn_8040193C) {
    auto& s = st();
    const u32 aid = c.r[3], out = c.r[4];
    std::lock_guard lk(s.m);
    if (aid != 1 || !s.dl || !s.child || !s.child->hasProgram(s.dlGgid)) {
        c.r[3] = 0;
        return;
    }
    std::memset(hostp(out), 0, 0x28);
    std::memcpy(hostp(out + 0x1E), recomp::mp::kChildMac, 6);
    c.r[3] = 1;
}

// ---- MP ------------------------------------------------------------------------------------

HLE(fn_803FAA64) {
    auto& s = st();
    const u32 cfg = c.r[3];
    {
        std::lock_guard lk(s.m);
        s.mp = true;
        s.mpGgid = rd32(cfg + 0x10);
        s.indCb = rd32(cfg + 0x194);
        s.connected = false;
        s.connectQueued = false;
        s.disconnectPending = false;
        s.sends.clear();
    }
    MPLOG("MP startup: ggid %06X, indication callback %08X", s.mpGgid, s.indCb);
    c.r[3] = 0;
}

HLE(fn_803FAC4C) {
    auto& s = st();
    MPLOG("MP cleanup");
    {
        std::lock_guard lk(s.m);
        s.mp = false;
        s.connected = false;
        s.connectQueued = false;
        s.sends.clear();
    }
    if (s.child) s.child->disconnected();
    c.r[3] = 0;
}

HLE(fn_803FBC4C) { c.r[3] = c.r[4] > 0x70 ? u32(-0xFF) : 0u; }

HLE(fn_803FE8FC) { c.r[3] = 0; }

HLE(fn_803FCA0C) {
    auto& s = st();
    std::lock_guard lk(s.m);
    if (s.connected) {
        MPLOG("MP: disconnect every child");
        s.connected = false;
        s.connectQueued = false;
        if (s.child) s.child->disconnected();
    }
    c.r[3] = 0;
}

HLE(fn_803FBDB8) {
    auto& s = st();
    const u32 data = c.r[3], len = c.r[4], aids = c.r[5], port = c.r[6], result = c.r[7], cb = c.r[8], arg = c.r[9];
    std::vector<u8> copy(hostp(data), hostp(data) + len);
    std::unique_lock lk(s.m);
    if (!s.mp) {
        c.r[3] = u32(-0xEC);
        return;
    }
    if (recomp::mp::logOn()) {
        char code[5] = {};
        if (len == 4)
            for (int i = 0; i < 4; i++) code[i] = char(copy[3 - i]);  // the game's 4-char codes are little-endian words
        MPLOG("send %u bytes to aids %X port %u%s%s", len, aids, port, len == 4 ? ": " : "", code);
    }
    recomp::mp::Child* child = s.connected && (aids & 2) ? s.child : nullptr;
    s.sends.push_back({cb, arg, result});
    if (child) child->received(copy.data(), len);
    c.r[3] = 0;
}
