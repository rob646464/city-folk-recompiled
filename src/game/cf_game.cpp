// Animal Crossing: City Folk: the game side of recomp/game.h. Nothing game-specific is hooked
// yet (boot work, docs/WII.md "Order of work"); these defaults leave the game untouched.
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <cstdio>
#include <cstdlib>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace recomp {
namespace game {
namespace {
std::atomic<u32> sFrames{0};
}  // namespace

bool onGuestFrame(Ctx&) {
    sFrames.fetch_add(1, std::memory_order_relaxed);
    return false;
}
u32 guestFrames() { return sFrames.load(std::memory_order_relaxed); }

// Test aid CF_RNG_SEED=<hex>: deterministic random numbers for scripted routes (tools/routes/).
// The game's main random number generator (state 0x8074F290, an LCG x*0x19660D+0x3C6EF35F,
// Random::next fn_802AA1A8) is seeded at boot from the clock (fn_8016BBA0 -> Random::setSeed
// fn_802AA1A0) and then drawn every frame, also during loads whose length in frames depends on
// real time, and by a second guest thread (the new town is built on one while the main thread
// draws for the bus scene) in whatever order the threads run. Scenes seed their own generators
// from main-generator draws (fn_8014C198: five draws summed, via fn_80107E90). With the aid:
// - every setSeed gets a value from (seed, caller, n-th call from that caller), and the calling
//   thread's main-generator stream is reset the same way at each of them;
// - each guest thread other than the boot thread draws "main" numbers from its own stream.
// So the game's random choices (town layout, villagers) stop depending on load times and thread
// timing. Off (no variable): both functions do exactly what the game's do.
namespace rng {
constexpr u32 kMainRng = 0x8074F290;
const char* const kEnv = std::getenv("CF_RNG_SEED");
const u32 kSeed = kEnv ? u32(std::strtoul(kEnv, nullptr, 16)) : 0;
std::mutex sMutex;
std::unordered_map<u32, u32> sCalls;    // setSeed caller -> calls so far
std::unordered_map<u32, u32> sStreams;  // guest thread -> its main-generator state
u32 sBootThread = 0;

u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
// The main-generator state the current thread uses (guest memory for the boot thread).
u32* stream(u32 thread) {
    if (thread == sBootThread) return nullptr;
    auto [it, fresh] = sStreams.try_emplace(thread, 0u);
    if (fresh) it->second = mix(kSeed ^ mix(thread));
    return &it->second;
}
}  // namespace rng

HLE(fn_802AA1A0) {  // Random::setSeed(this, seed)
    u32 v = c.r[4];
    if (rng::kEnv) {
        std::lock_guard lk(rng::sMutex);
        const u32 n = ++rng::sCalls[c.lr];
        const u32 thread = sched::currentGuest();
        if (c.lr == 0x8016C0A0) {
            v = rng::kSeed;  // the boot seed of the main generator
            rng::sBootThread = thread;
        } else if (c.r[3] != rng::kMainRng) {
            v = rng::mix(rng::kSeed ^ rng::mix(c.lr) ^ (n * 0x9E3779B9u));
            const u32 reset = rng::mix(v ^ 0x5BD1E995u);
            if (u32* s = rng::stream(thread)) *s = reset;
            else wr32(rng::kMainRng, reset);
        }
        static const bool verbose = std::getenv("CF_RNG_LOG") != nullptr;
        if (verbose || c.lr == 0x8016C0A0) {
            const u64 secs = u64(double(timeBase()) / kTimerClock) % 86400;
            log("[rng] setSeed %08X from lr %08X (#%u): %08X -> %08X (CF_RNG_SEED) thread %08X frame %u clock %02u:%02u:%02u",
                c.r[3], c.lr, n, c.r[4], v, thread, guestFrames(), u32(secs / 3600), u32(secs / 60 % 60), u32(secs % 60));
        }
    }
    wr32(c.r[3], v);
}

// Random::next(this) -> new state. The game's step (from its code): t = 0x19660D * x + 0x3C6EF35F
// in 64 bits, new x = low word + high word of t.
static u32 rngStep(u32 x) {
    const u64 t = u64(0x19660Du) * x + 0x3C6EF35Fu;
    return u32(t) + u32(t >> 32);
}
HLE(fn_802AA1A8) {
    const u32 obj = c.r[3];
    // CF_RNG_DRAWS=<first>,<last>: log every main-generator draw in those guest frames (thread,
    // caller of the float wrapper fn_802AA1F8, which keeps its return address at sp+0x14).
    static const auto drawWindow = [] {
        std::pair<u32, u32> w{1, 0};
        if (const char* e = std::getenv("CF_RNG_DRAWS")) std::sscanf(e, "%u,%u", &w.first, &w.second);
        return w;
    }();
    if (obj == rng::kMainRng && drawWindow.first <= drawWindow.second) {
        const u32 f = guestFrames();
        if (f >= drawWindow.first && f <= drawWindow.second)
            log("[rng] draw frame %u thread %08X lr %08X caller %08X", f, sched::currentGuest(), c.lr,
                c.lr == 0x802AA208 ? rd32(c.r[1] + 0x14) : 0);
    }
    if (rng::kEnv && obj == rng::kMainRng && rng::sBootThread) {
        std::lock_guard lk(rng::sMutex);
        if (u32* s = rng::stream(sched::currentGuest())) {
            *s = rngStep(*s);
            c.r[3] = *s;
            return;
        }
    }
    const u32 x = rngStep(rd32(obj));
    wr32(obj, x);
    c.r[3] = x;
}
void filterPad(PADStatus*, int) {}
bool padHeld() { return false; }
double retraceSpeed() { return 1.0; }
float audioGain() { return 1.0f; }
bool audioMirrored() { return false; }
bool unadjustedPass() { return false; }
bool splitScreen() { return false; }
bool textBoxShown() { return false; }
int textMode() { return 0; }
bool viewerOpen() { return false; }
bool enhViewMatrix(float*) { return false; }
bool enhPlayerPos(float*) { return false; }
void enhBeginScene(Ctx&) {}
bool enhRoomBounds(const float*, float*, float*) { return false; }
void drawOverlay() {}
void debugMenu() {}
s32 overlayEntry(const char*) { return -1; }
bool isOverlayEntry(s32) { return false; }
u32 overlayLength(s32) { return 0; }
s32 overlayRead(s32, void*, s32, s32) { return -1; }
void onBoot(Ctx&) {}
void onFrame() {}

}  // namespace game
}  // namespace recomp

// Game-side hooks the generic runtime declares in its own headers (LM: skip.cpp, shadows.cpp,
// tags.cpp).
namespace recomp {
namespace gx {
bool g_discardDraws = false;  // gx_bridge.h: the frame being drawn is not shown (fast-forward)
}
namespace interp {
u32 viewMatrixSource() { return 0; }  // interp.h: no camera matrix in guest memory known yet
namespace detail {
bool trackPoints(float out[9]) {
    (void)out;
    return false;
}  // interp.h: path check (CF_TRACK_LOG)
}
}  // namespace interp
}  // namespace recomp
