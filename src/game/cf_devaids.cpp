// Test aids for scripted runs (docs/FEATURES.md "Test aids"). None of them is active unless its
// environment variable is set.
//
//   CF_FF=<from>-<to>[,<from>-<to>...][:<speed>]   fast-forward during those guest frames: retraces
//       and alarms run <speed> x (default 16) and only one frame in 12 is shown. Input scripts
//       timed with CF_INPUT_CLOCK=frames keep their meaning (guest frames).
//   CF_MEM_DUMP=<dir>,<frame>[,<frame>...]   writes MEM1 (24 MiB) then MEM2 (64 MiB) to
//       <dir>/mem_<frame>.bin at the end of those guest frames (offline diffs of game state).
//   CF_CLOCK_JUMP=<frame>:<seconds>[,<frame>:<seconds>...]   moves the console clock (time base)
//       forward by <seconds> at that guest frame, as if that much time had passed (day changes).
#include "game/cf_features.h"

#include "recomp/game.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace recomp {
void setTimeBaseBias(u64 bias);  // runtime.cpp
}

namespace recomp::cf {
namespace {

struct FfConfig {
    std::vector<std::pair<u32, u32>> windows;
    double speed = 16.0;
};
const FfConfig sFf = [] {
    FfConfig f;
    const char* v = std::getenv("CF_FF");
    if (!v) return f;
    std::string s = v;
    if (const size_t colon = s.find(':'); colon != std::string::npos) {
        f.speed = std::atof(s.c_str() + colon + 1);
        if (f.speed < 1.0) f.speed = 1.0;
        s.resize(colon);
    }
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        unsigned a = 0, b = 0;
        if (std::sscanf(s.substr(p, q - p).c_str(), "%u-%u", &a, &b) == 2) f.windows.push_back({a, b});
        p = q + 1;
    }
    return f;
}();

bool inFf(u32 frame) {
    for (auto& [a, b] : sFf.windows)
        if (frame >= a && frame < b) return true;
    return false;
}

struct DumpConfig {
    std::string dir;
    std::vector<u32> frames;
};
const DumpConfig sDump = [] {
    DumpConfig d;
    const char* v = std::getenv("CF_MEM_DUMP");
    if (!v) return d;
    std::string s = v;
    size_t p = s.find(',');
    d.dir = s.substr(0, p);
    while (p != std::string::npos) {
        const size_t q = s.find(',', p + 1);
        d.frames.push_back(u32(std::strtoul(s.substr(p + 1, q == std::string::npos ? q : q - p - 1).c_str(), nullptr, 10)));
        p = q;
    }
    return d;
}();

std::vector<std::pair<u32, long long>> parseJumps() {
    std::vector<std::pair<u32, long long>> out;
    const char* v = std::getenv("CF_CLOCK_JUMP");
    if (!v) return out;
    std::string s = v;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        unsigned f = 0;
        long long secs = 0;
        if (std::sscanf(s.substr(p, q - p).c_str(), "%u:%lld", &f, &secs) == 2) out.push_back({f, secs});
        p = q + 1;
    }
    return out;
}
const std::vector<std::pair<u32, long long>> sJumps = parseJumps();

void dumpMemory(u32 frame) {
    const std::string path = sDump.dir + "/mem_" + std::to_string(frame) + ".bin";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        log("[devaids] cannot write %s", path.c_str());
        return;
    }
    std::fwrite(hostp(0x80000000u), 1, 0x01800000u, f);
    std::fwrite(hostp(0x90000000u), 1, kMem2Size, f);
    std::fclose(f);
    log("[devaids] guest frame %u: memory written to %s", frame, path.c_str());
}

void jumpClock(long long secs) {
    // timeBase() = bias + time since boot: find the time since boot, then move the bias.
    const u64 before = timeBase();
    setTimeBaseBias(0);
    const u64 elapsed = timeBase();
    setTimeBaseBias(before - elapsed + u64(s64(double(secs) * kTimerClock)));
    log("[devaids] console clock moved %+lld s at guest frame %u", secs, game::guestFrames());
}

std::atomic<bool> sFast{false};

}  // namespace

bool devOnGuestFrame(Ctx&) {
    const u32 frame = game::guestFrames();
    // CF_WATCH_FROM=<frame>: the CF_WATCH store report starts at that guest frame (boot and the
    // fast-forwarded route stay quick).
    static const u32 sWatchFrom = std::getenv("CF_WATCH_FROM") ? u32(std::atoi(std::getenv("CF_WATCH_FROM"))) : 0;
    static bool sWatchWanted = g_watchOn;
    if (sWatchFrom && sWatchWanted) g_watchOn = frame >= sWatchFrom;
    if (frame % 1000 == 0 && std::getenv("CF_FRAME_LOG")) log("[devaids] guest frame %u", frame);
    for (u32 d : sDump.frames)
        if (d == frame) dumpMemory(frame);
    for (auto& [f, secs] : sJumps)
        if (f == frame) jumpClock(secs);
    // CF_POKE=<frame>:<hex addr>:<hex byte>[,...]: writes one guest byte at that guest frame.
    static const std::vector<std::tuple<u32, u32, u32>> sPokes = [] {
        std::vector<std::tuple<u32, u32, u32>> out;
        if (const char* v = std::getenv("CF_POKE")) {
            std::string s = v;
            size_t p = 0;
            while (p < s.size()) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                unsigned f = 0, a = 0, b = 0;
                if (std::sscanf(s.substr(p, q - p).c_str(), "%u:%x:%x", &f, &a, &b) == 3) out.push_back({f, a, b});
                p = q + 1;
            }
        }
        return out;
    }();
    for (auto& [f, a, b] : sPokes)
        if (f == frame) {
            log("[devaids] guest frame %u: %08X = %02X (was %02X)", frame, a, b, rd8(a));
            wr8(a, b);
        }
    const bool fast = !sFf.windows.empty() && inFf(frame);
    sFast.store(fast, std::memory_order_relaxed);
    if (fast && frame % 12 != 0) {
        gx::g_discardDraws = true;
        return true;
    }
    gx::g_discardDraws = false;
    return false;
}

double devRetraceSpeed() { return sFast.load(std::memory_order_relaxed) ? sFf.speed : 1.0; }

}  // namespace recomp::cf
