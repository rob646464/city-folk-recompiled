// Animal Crossing: City Folk: the game side of recomp/game.h. Nothing game-specific is hooked
// yet (boot work, docs/WII.md "Order of work"); these defaults leave the game untouched.
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/version.h"

#include <cstdio>
#include <cstdlib>

#include "game/cf_features.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace recomp::vi {
double fieldSeconds();  // hle_vi.cpp
}

namespace recomp {
namespace game {
namespace {
std::atomic<u32> sFrames{0};
}  // namespace

// Test aid CF_FORCE_SAVE=<frame>: at that guest frame the game's own save-write request
// (fn_800D2780: starts the save worker with entry fn_800D242C -> fn_800D3118 -> create /
// safe-open / write rvforest.dat and the banner) is called on the game thread, as the HUD's
// "Save" does after its checks; the worker's result word (r13-0x6930, read by fn_800D2A44:
// 1 = done, 4 = no file, 2/3/5/6+ = errors) is logged when it changes. Exercises the whole NAND
// write path without reaching a house first (docs/NAND.md "First save").
static void forceSaveOnFrame(Ctx& c, u32 frame) {
    static const u32 at = [] {
        const char* v = std::getenv("CF_FORCE_SAVE");
        return v ? u32(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    static const bool logAll = std::getenv("CF_SAVE_LOG") != nullptr;  // result word changes, any time
    if (!at && !logAll) return;
    const u32 kResultWord = VA(0x8074B010);  // r13 - 0x6930 (USA)
    static u32 last = ~0u;
    if (logAll) {
        const u32 r = rd32(kResultWord);
        if (r != last) log("[save] result word %d at frame %u", s32(r), frame);
        last = r;
        if (!at) return;
    }
    if (frame == at) {
        log("[save] CF_FORCE_SAVE: calling the save write request (fn_800D2780) at frame %u, result word %d", frame,
            s32(rd32(kResultWord)));
        guestCall(c, VA(0x800D2780));
    }
    if (frame >= at) {
        const u32 r = rd32(kResultWord);
        if (r != last) {
            log("[save] result word %d at frame %u", s32(r), frame);
            last = r;
        }
    }
}

// Test aid CF_FORCE_WC24=<frame>: the WiiConnect24 step the HUD save runs before writing a new
// save (fn_801782FC: task fn_8017B2B8 on the WiiConnect24 thread, which sets up the game's
// download task), started at that frame; its status (result word r13-0x6140 = 0x8074B800) (fn_80178498: -1 running, 1 done, 0 failed ->
// the save shows "Could not access Wii system memory") is logged when it changes.
static void forceWc24OnFrame(Ctx& c, u32 frame) {
    static const u32 at = [] {
        const char* v = std::getenv("CF_FORCE_WC24");
        return v ? u32(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (!at || frame < at) return;
    static s32 last = 0x7FFFFFFF;
    static bool done = false;
    if (frame == at) log("[save] CF_FORCE_WC24: fn_801782FC -> %d at frame %u", s32(guestCall(c, VA(0x801782FC))), frame);
    if (done) return;
    const s32 st = s32(guestCall(c, VA(0x80178498)));
    if (st != last) {
        log("[save] WiiConnect24 step status %d at frame %u (task result %d, NWC24 error %d)", st, frame,
            s32(rd32(VA(0x8074B800))), s32(rd32(VA(0x805BEBC0) + 0x418)));
        last = st;
    }
    if (st >= 0) done = true;
}

bool onGuestFrame(Ctx& c) {
    const u32 frame = sFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    forceSaveOnFrame(c, frame);
    forceWc24OnFrame(c, frame);
    cf::grassOnFrame(c);  // cf_grass.cpp, cf_skin.cpp (docs/FEATURES.md)
    cf::skinOnFrame(c);
    return cf::devOnGuestFrame(c);  // CF_FF fast-forward and other test aids (cf_devaids.cpp)
}
u32 guestFrames() { return sFrames.load(std::memory_order_relaxed); }

// Test aid CF_RNG_SEED=<hex>: deterministic random numbers for scripted routes (tools/routes/).
// The game's main random number generator (state 0x8074F290, an LCG x*0x19660D+0x3C6EF35F,
// Random::next fn_802AA1A8) is seeded at boot from the clock (fn_8016BBA0 -> Random::setSeed
// fn_802AA1A0) and then drawn every frame, also during loads whose length in frames depends on
// real time, and by a second guest thread (the new town is built on one while the main thread
// draws for the bus scene) in whatever order the threads run. Scenes seed their own generators
// from main-generator draws (fn_8014C198: five draws summed, via fn_80107E90). With the aid:
// - every setSeed gets a value from (seed, thread, caller, n-th such call), and the calling
//   thread's main-generator stream is reset the same way at each of them;
// - each guest thread other than the boot thread draws "main" numbers from its own stream.
// So the game's random choices (town layout, villagers) stop depending on load times and thread
// timing. Off (no variable): both functions do exactly what the game's do.
namespace rng {
inline u32 mainRng() { return VA(0x8074F290); }
const char* const kEnv = std::getenv("CF_RNG_SEED");
const u32 kSeed = kEnv ? u32(std::strtoul(kEnv, nullptr, 16)) : 0;
std::mutex sMutex;
std::unordered_map<u64, u32> sCalls;    // (guest thread, setSeed caller) -> calls so far
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
        const u32 thread = sched::currentGuest();
        const u32 n = ++rng::sCalls[u64(thread) << 32 | c.lr];
        if (c.lr == VA(0x8016C0A0)) {
            v = rng::kSeed;  // the boot seed of the main generator
            rng::sBootThread = thread;
        } else if (c.r[3] != rng::mainRng()) {
            v = rng::mix(rng::kSeed ^ rng::mix(c.lr ^ rng::mix(thread)) ^ (n * 0x9E3779B9u));
            const u32 reset = rng::mix(v ^ 0x5BD1E995u);
            if (u32* s = rng::stream(thread)) *s = reset;
            else wr32(rng::mainRng(), reset);
        }
        static const bool verbose = std::getenv("CF_RNG_LOG") != nullptr;
        if (verbose || c.lr == VA(0x8016C0A0)) {
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
    if (obj == rng::mainRng() && drawWindow.first <= drawWindow.second) {
        const u32 f = guestFrames();
        if (f >= drawWindow.first && f <= drawWindow.second)
            log("[rng] draw frame %u thread %08X lr %08X caller %08X", f, sched::currentGuest(), c.lr,
                c.lr == VA(0x802AA208) ? rd32(c.r[1] + 0x14) : 0);
    }
    if (rng::kEnv && obj == rng::mainRng() && rng::sBootThread) {
        std::lock_guard lk(rng::sMutex);
        if (u32* s = rng::stream(sched::currentGuest())) {
            *s = rngStep(*s);
            c.r[3] = *s;
            return;
        }
        // The boot thread's stream restarts every guest frame from (seed, frame): how many
        // frames a load took no longer shifts the numbers drawn after it (a new player's and
        // town's ids are drawn at the name entry, many frames after the last setSeed).
        static u32 lastFrame = ~0u;
        const u32 f = guestFrames();
        if (f != lastFrame) {
            lastFrame = f;
            wr32(obj, rng::mix(rng::kSeed ^ (f * 0x9E3779B9u) ^ 0xA5A5A5A5u));
        }
    }
    const u32 x = rngStep(rd32(obj));
    wr32(obj, x);
    c.r[3] = x;
}
void filterPad(PADStatus*, int) {}
bool padHeld() { return false; }
double retraceSpeed() { return cf::devRetraceSpeed(); }
float audioGain() { return cf::devRetraceSpeed() > 1.0 ? 0.0f : 1.0f; }
bool audioMirrored() { return false; }
// One GXCopyDisp per retrace in every scene measured (title, Rover's intro, bus, keyboards,
// town, town map; docs/WII.md "Frame rate"): 60 fps, 50 for a European game at 50 Hz.
int nativeFps() { return int(std::lround(1.0 / vi::fieldSeconds())); }
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
// viewMatrixSource: cf_tags.cpp (the current g3d camera).
namespace detail {
bool trackPoints(float out[9]) {
    (void)out;
    return false;
}  // interp.h: path check (CF_TRACK_LOG)
}
}  // namespace interp
}  // namespace recomp
