// Animal Crossing: City Folk: the game side of recomp/game.h. Nothing game-specific is hooked
// yet (boot work, docs/WII.md "Order of work"); these defaults leave the game untouched.
#include "recomp/game.h"
#include "recomp/hle.h"

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
// fn_802AA1A8) is seeded at boot from the clock (fn_8016BBA0 -> Random::setSeed fn_802AA1A0) and
// then drawn every frame, also during loads whose length in frames depends on real time. Scenes
// seed their own generators from main-generator draws (fn_8014C198: five draws summed, via
// fn_80107E90), e.g. the new town's layout and villagers. With the aid every setSeed gets a value
// from (seed, caller, n-th call from that caller), and the main generator is reset the same way
// at each of them, so the game's random choices stop depending on load times.
static u32 mixSeed(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

HLE(fn_802AA1A0) {
    constexpr u32 kMainRng = 0x8074F290;
    static const char* env = std::getenv("CF_RNG_SEED");
    static const u32 seed = env ? u32(std::strtoul(env, nullptr, 16)) : 0;
    u32 v = c.r[4];
    if (env) {
        static std::mutex m;
        static std::unordered_map<u32, u32> calls;
        std::lock_guard lk(m);
        const u32 n = ++calls[c.lr];
        if (c.lr == 0x8016C0A0) {
            v = seed;  // the boot seed of the main generator
        } else if (c.r[3] != kMainRng) {
            v = mixSeed(seed ^ mixSeed(c.lr) ^ (n * 0x9E3779B9u));
            wr32(kMainRng, mixSeed(v ^ 0x5BD1E995u));
        }
        static const bool verbose = std::getenv("CF_RNG_LOG") != nullptr;
        if (verbose || c.lr == 0x8016C0A0)
            log("[rng] setSeed %08X from lr %08X (#%u): %08X -> %08X (CF_RNG_SEED)", c.r[3], c.lr, n, c.r[4], v);
    }
    wr32(c.r[3], v);
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
