// Entry point of the recompiled build: loads the DOL from the user's disc into
// guest memory, boots it on a guest thread, and runs the Aurora host loop.
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <aurora/present_timing.h>
#include <dolphin/dvd.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_video.h>

#include "port/crash_handler.h"
#include "port/disc_folder.h"
#include "port/dol.h"
#include "port/install.h"
#include "port/options.h"
#include "port/recovery.h"
#include "port/settings.h"
#include "port/update.h"
#include "recomp/audio.h"
#include "recomp/cpu_qos.h"
#include "recomp/mem_report.h"
#include "recomp/frame.h"
#include "recomp/game.h"
#include "recomp/gx_bridge.h"
#include "recomp/menu.h"
#include "recomp/pacing.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/module_loader.h"
#include "recomp/precache.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/texture_packs.h"
#include "recomp/widescreen.h"
#include "recomp/wiimote.h"
#ifndef CF_STATIC_MODULE
#include "setup/disc.h"
#include "setup/setup.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
#ifdef __APPLE__
void hideWindowMacos(SDL_Window* w);  // macos_window.cpp
#endif
#ifdef CF_STATIC_MODULE
extern const GameModule kGameModule;  // generated func_table.cpp, linked in (cfrecomp)
#endif
namespace vi {
void startTimer();
void requestStop();
void stopTimer();
}  // namespace vi

namespace {

void loadDol(const u8* dol) {
    auto be = [&](u32 off) { return u32(dol[off]) << 24 | u32(dol[off + 1]) << 16 | u32(dol[off + 2]) << 8 | dol[off + 3]; };
    // .bss overlaps the small-data sections, so clear it before copying them in.
    std::memset(hostp(be(0xD8)), 0, be(0xDC));
    for (int i = 0; i < 18; i++) {
        const u32 off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
        if (size) std::memcpy(hostp(addr), dol + off, size);
    }
}

// Reads the `lis rD, hi; ori rD, rD, lo` pairs of __init_registers (r1, r2, r13; the RVL SDK's
// version clears every register first).
void initRegisters(Ctx& c) {
    const u32 start = guestSymbol("__init_registers");
    u32 hi[32] = {};
    for (u32 pc = start; pc < start + 0x100; pc += 4) {
        const u32 w = rd32(pc);
        const u32 op = w >> 26, d = (w >> 21) & 31, a = (w >> 16) & 31;
        if (op == 15 && a == 0) hi[d] = w << 16;                              // lis rD, hi
        else if (op == 24 && a == d && hi[d]) c.r[d] = hi[d] | (w & 0xFFFF);  // ori rD, rD, lo
        else if (op == 19 && ((w >> 1) & 0x3FF) == 16) break;                 // blr
    }
    if (!c.r[1] || !c.r[2] || !c.r[13]) fatal("__init_registers: r1 %08X r2 %08X r13 %08X", c.r[1], c.r[2], c.r[13]);
}

void boot() {
    Ctx init;
    initRegisters(init);
    const u32 stackTop = init.r[1];
    init.r[1] = stackTop - 8;
    wr32(init.r[1], 0);

    const u32 defaultThread = hleAlloc(0x310);
    GuestThread* t = sched::adoptBootThread(defaultThread, init);
    wr32(defaultThread + 0x304, stackTop);
    wr32(defaultThread + 0x308, stackTop - 0x10000);
    const u32 irqStack = hleAlloc(0x10000);
    sched::setIrqContext(init, irqStack + 0x10000 - 8);

    Ctx& c = sched::ctxOf(t);
    log("[boot] r1 %08X r2 %08X r13 %08X", init.r[1], init.r[2], init.r[13]);
    guestCall(c, guestSymbol("OSInit"));
    vi::startTimer();
    game::onBoot(c);
    guestCall(c, guestSymbol("__init_user"));
    log("[boot] entering main");
    guestCall(c, guestSymbol("main"), 0, 0);
    log("[boot] main returned %d", s32(c.r[3]));
}

bool sNoGpuAdapter = false;  // Aurora found no usable graphics adapter (no Vulkan/D3D driver)

void logCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
    static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    std::fprintf(level >= LOG_ERROR ? stderr : stdout, "[%s: %s] %s\n", names[level], module, message);
    std::fflush(stdout);
    if (port::recovery::runLog()) {
        const std::string line = std::string("[") + names[level] + ": " + module + "] " + message;
        port::recovery::logLine(line.c_str(), line.size());
    }
    if (level == LOG_FATAL) port::recovery::noteFatal(message);
    if (level == LOG_ERROR && std::strstr(message, "Failed to create adapter")) sNoGpuAdapter = true;
    if (level == LOG_FATAL) {
        std::fflush(stderr);
        // Without any usable GPU Aurora falls back to its null device and stops on the first
        // allocation; tell the player what is missing instead of only crashing.
        std::string text = std::string("The game stopped with an error:\n\n") + message +
                           "\n\nThe log of this run (last_run.log) is in the folder of settings.ini; please "
                           "attach it to a bug report. If the game no longer starts, the next start offers "
                           "to reset the settings or rebuild the game data.";
        if (sNoGpuAdapter) {
#ifdef _WIN32
            text = "No usable graphics adapter was found (Direct3D 12, Direct3D 11 or Vulkan).\n\n"
                   "Update your graphics driver.";
#else
            text = "No usable Vulkan graphics driver was found.\n\n"
                   "Install the Vulkan driver for your GPU (AMD/Intel: Mesa's Vulkan drivers, e.g. the "
                   "mesa-vulkan-drivers package; NVIDIA: the proprietary driver) and check it with "
                   "'vulkaninfo --summary'. See README.txt.";
#endif
        }
        if (!std::getenv("CF_BACKGROUND"))  // automated runs must not block on a dialog
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Animal Crossing City Folk Recompiled", text.c_str(), nullptr);
        std::abort();
    }
}

#ifdef CF_STATIC_MODULE
std::string findDisc(const char*) {
    if (!port::options().disc.empty()) return port::options().disc;
#ifdef CF_DEV_DISC
    return CF_DEV_DISC;  // the build's CF_DISC
#else
    return {};
#endif
}
#else
bool hasArg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

std::string argValue(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return {};
}
#endif

// Hang reports (port/crash_handler.h): whether the game itself kept running and what Aurora's
// threads are doing, so a log tells a stopped game from a stopped display (issue #11: on a Steam
// Deck the game kept running at the file select while nothing was presented any more).
std::atomic<u32> sFramesAtStall{0};
std::atomic<bool> sStallOpen{false};
void hangDetail(char* out, size_t size, bool stallBegins) {
    const u32 frames = frame::guestFrames();
    if (stallBegins) {
        sFramesAtStall.store(frames, std::memory_order_relaxed);
        sStallOpen.store(true, std::memory_order_relaxed);
        return;
    }
    char act[400] = "";
    aurora_describe_activity(act, sizeof(act), 1000);
    char since[64] = "";
    if (sStallOpen.load(std::memory_order_relaxed))
        std::snprintf(since, sizeof(since), ", %u since the main thread stopped", frames - sFramesAtStall.load(std::memory_order_relaxed));
    std::snprintf(out, size, "game frames %u%s (%s)%s%s", frames, since,
                  frame::interpolating() ? "interpolated presentation" : "lockstep", act[0] ? "; aurora: " : "", act);
}

}  // namespace
}  // namespace recomp

int main(int argc, char* argv[]) {
    using namespace recomp;
    // Windows: no console window for players; output to a terminal, a redirect or last_run.log.
    port::recovery::setupConsole(argc, argv);
    port::installCrashHandler();
    port::setHangDetail(&hangDetail);
    port::startHangWatchdog();
    port::parseOptions(argc, argv);
    port::setLaunchArgs(argc, argv);
    std::printf("Animal Crossing City Folk Recompiled %s\n", port::update::buildVersion());
    std::fflush(stdout);
#ifndef CF_STATIC_MODULE
    // --check-disc <path>: identify a disc the way setup does (game ID, revision, main.dol and REL
    // hashes) and exit: 0 = a supported disc. No window, nothing written (docs/SETUP.md).
    if (const std::string path = argValue(argc, argv, "--check-disc"); !path.empty()) {
        port::GameVersion v{};
        std::string err;
        const bool ok = setup::probeDisc(path, v, err);
        if (ok) std::printf("OK: %s\n", setup::describe(v).c_str());
        else std::printf("REFUSED: %s\n", err.c_str());
        std::fflush(stdout);
        return ok ? 0 : 3;
    }
#endif
    // A game folder too deep for the graphics start-up (docs/SETUP.md "Long paths"): say so
    // instead of stopping without a word.
    if (const std::string p = port::installPathProblem(); !p.empty()) {
        std::fprintf(stderr, "%s\n", p.c_str());
        if (!std::getenv("CF_BACKGROUND"))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Animal Crossing City Folk Recompiled", p.c_str(), nullptr);
        return 1;
    }
    // Portable or system folders (docs/SETUP.md "Portable mode"), before any file is opened: a
    // data move the F1 menu asked for runs here.
    port::dataLocation();
    // Windows: last_run.log (recovery.h), in the folder dataLocation just chose (portable or system).
    port::recovery::openRunLog();

    AuroraConfig config{};
    config.appName = "Animal Crossing City Folk Recompiled";
    config.logCallback = &logCallback;
    port::loadSettings();
    // Start-up recovery (docs/SETUP.md "When the game does not start"): after a start that failed,
    // offers to reset the settings, rebuild the game data or put the saves aside.
#ifdef CF_STATIC_MODULE
    port::recovery::onStart(false);
#else
    port::recovery::onStart(true);
#endif
    // Updates (docs/SETUP.md "Updates"): remove what a finished update left, then look for a newer
    // release in the background (never blocks; the F1 menu shows the result).
    port::update::cleanupAfterUpdate();
    port::update::startupCheck();
    // A data folder too deep for the files Aurora writes with plain paths (docs/SETUP.md "Long paths").
    if (const std::string p = port::dataPathProblem(); !p.empty()) {
        std::fprintf(stderr, "%s\n", p.c_str());
        if (!std::getenv("CF_BACKGROUND"))  // automated runs must not block on a dialog
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Animal Crossing City Folk Recompiled", p.c_str(), nullptr);
    }
    config.msaa = u32(port::settings().video.msaa);
    config.vsync = port::settings().video.vsync;
    // Under Gamescope (Steam Deck game mode) the swap chain always waits for the refresh (FIFO).
    // Gamescope composites every frame, so VSync off (Mailbox there) never tore and only lowered
    // latency, and interpolated presentation with Mailbox stopped showing frames on a Steam Deck
    // while the game ran on (issue #11). CF_GAMESCOPE_VSYNC=0 keeps the setting (test aid).
    if (!config.vsync && pacing::gamescope()) {
        const char* keep = std::getenv("CF_GAMESCOPE_VSYNC");
        if (!(keep && *keep == '0')) {
            config.vsync = true;
            std::printf("[video] Gamescope: VSync on (FIFO presentation; the VSync setting is ignored under Gamescope)\n");
        }
    }
    // CF_BACKEND=d3d12|d3d11|vulkan (testing): graphics backend instead of Aurora's choice.
    if (const char* b = std::getenv("CF_BACKEND")) {
        const std::string s = b;
        config.desiredBackend = s == "d3d12"    ? BACKEND_D3D12
                                : s == "d3d11"  ? BACKEND_D3D11
                                : s == "vulkan" ? BACKEND_VULKAN
                                : s == "opengl" ? BACKEND_OPENGL
                                : s == "gles"   ? BACKEND_OPENGLES
                                                : BACKEND_AUTO;
    }
    // CF_ALLOW_CPU_GPU=1: accept a software renderer (e.g. Mesa llvmpipe for Vulkan in WSL or a
    // VM without GPU passthrough; docs/LINUX.md). Slow, for testing only.
    if (const char* c = std::getenv("CF_ALLOW_CPU_GPU"); c && *c == '1') config.allowCpuAdapter = true;
    // CF_CACHE_DIR: separate shader/pipeline cache, e.g. for test runs of different builds side by side.
    // Default: next to settings.ini, so a scratch APPDATA (test runs) also gets its own cache
    // (Aurora's default, SDL_GetPrefPath, ignores the APPDATA variable).
    // Linux: $XDG_CACHE_HOME/Animal Crossing City Folk Recompiled (port::cacheDir; docs/LINUX.md).
    // Aurora takes UTF-8 paths (path::string() is the ANSI code page on Windows: a user name such
    // as "José" made Aurora's path conversion throw at startup).
    const auto utf8 = [](const std::filesystem::path& p) {
        const std::u8string s = p.u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    };
    static const std::string sCacheDir = utf8(port::cacheDir());
    config.cachePath = sCacheDir.c_str();
    // Memory cards and imgui.ini (Aurora's userPath) in dataDir, which honours XDG_DATA_HOME /
    // APPDATA like the rest. On Windows this is the same folder SDL_GetPrefPath gives for players,
    // but SDL ignores the APPDATA variable, so test runs with a scratch APPDATA used to write
    // imgui.ini into the real install.
    // CF_CARD_DIR (test runs) is the user path too: Aurora's CARDInit builds the card paths from
    // userPath whenever one is set, so CARDSetBasePath alone no longer moved the cards.
    static const std::string sUserDir =
        std::getenv("CF_CARD_DIR") ? utf8(port::envPath("CF_CARD_DIR")) + "/" : utf8(port::dataDir()) + "/";
    config.userPath = sUserDir.c_str();
    static const std::string sCacheOverride = std::getenv("CF_CACHE_DIR") ? utf8(port::envPath("CF_CACHE_DIR")) : "";
    if (!sCacheOverride.empty()) config.cachePath = sCacheOverride.c_str();
    const port::Options& opts = port::options();
    config.startFullscreen = opts.fullscreen || port::settings().video.fullscreen;
    config.windowWidth = opts.windowWidth;
    config.windowHeight = opts.windowHeight;
    if (!config.windowWidth && opts.aspectRatio > 0.0f) {  // fixed aspect: open a window of that shape
        config.windowHeight = 960;
        config.windowWidth = u32(960.0f * opts.aspectRatio + 0.5f);
    } else if (!config.windowWidth && ws::gameWidescreen()) {  // the game's 16:9 frame
        config.windowWidth = 1280;
        config.windowHeight = 720;
    }
    precache::configure();
    port::setPhase("graphics initialization (device, render targets, anti-aliasing)");
    // CF_TOPMOST=1 (testing: screen-capture judder runs, tools/interp/screen_run.py): the window
    // stays on top and a fullscreen window is not minimized when another window takes the focus.
    const bool topmost = std::getenv("CF_TOPMOST") != nullptr;
    if (topmost) SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    // CF_OFFSCREEN test runs never take the keyboard focus from the person using the PC (their
    // typing went into the off-screen game: PC keyboard typing, key bindings).
    if (std::getenv("CF_OFFSCREEN")) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    cpu::init();  // thread policy + Aurora thread hooks (issue #18), before Aurora starts its threads
    aurora_initialize(argc, argv, &config);
    mem::trackInit();  // CF_MEM_TRACK (test aid, docs/RENDERING.md "Memory use over time")
    if (topmost)
        if (SDL_Window* w = aurora_get_window()) SDL_SetWindowAlwaysOnTop(w, true);
    // CF_OFFSCREEN=1 (testing: automated runs on a machine someone is using): the window is moved far
    // outside every display, so it renders and presents as usual but is never seen (a hidden window
    // pauses the game; Start-Process -WindowStyle Hidden does not hide SDL's window).
    if (std::getenv("CF_OFFSCREEN"))
        if (SDL_Window* w = aurora_get_window()) {
#ifdef __APPLE__
            // macOS keeps windows on a display whatever position is asked for: fully transparent
            // and click-through instead (it still renders and presents; docs/MACOS.md).
            hideWindowMacos(w);
#else
            SDL_SetWindowPosition(w, -32000, -32000);
#endif
        }
    // CF_MAXIMIZE=1 (testing): open the window maximized, as players often run it.
    if (std::getenv("CF_MAXIMIZE"))
        if (SDL_Window* w = aurora_get_window()) SDL_MaximizeWindow(w);
    precache::init();
    ws::init();
    port::setPhase("loading the game");
    gx::installFifoErrorReport();

#ifdef CF_STATIC_MODULE
    // cfrecomp (developer build): the game was recompiled at build time and linked in.
    setGameModule(&kGameModule);
    const char* wantId = kGameModule.gameId;
    std::string disc = findDisc(wantId);
    if (const std::string root = port::discfolder::resolve(disc); !root.empty()) disc = root;  // e.g. "US DATA"
    {
        auto& inst = port::install();
        inst.staticBuild = true;
        port::versionFromId(wantId, inst.running);
        inst.ready[int(inst.running)] = true;
    }
#else
    // CityFolkRecomp: set up from the user's discs (first run, or after changes).
    setup::Options so;
    so.forceUi = hasArg(argc, argv, "--setup");
    so.autoStart = hasArg(argc, argv, "--setup-auto");
    // --usa-disc / --pal-disc / --jpn-disc / --kor-disc: a region's slot; --disc: any region (docs/SETUP.md).
    so.disc[int(port::GameVersion::USA)] = argValue(argc, argv, "--usa-disc");
    if (so.disc[0].empty()) so.disc[0] = argValue(argc, argv, "--us-disc");
    so.disc[int(port::GameVersion::PAL)] = argValue(argc, argv, "--pal-disc");
    so.disc[int(port::GameVersion::JPN)] = argValue(argc, argv, "--jpn-disc");
    so.disc[int(port::GameVersion::KOR)] = argValue(argc, argv, "--kor-disc");
    so.anyDisc = argValue(argc, argv, "--disc");
    setup::LaunchInfo launch;
    if (!setup::run(so, launch)) {
        port::recovery::onExit();
        aurora_shutdown();
        std::_Exit(0);
    }
    const std::string disc = launch.disc;
    const char* wantId = port::gameId(launch.version);
#endif
    port::setPhase("loading the game");
    // The disc: an extracted folder (sys/, files/) or an image's game partition (nod), both served
    // by port/disc_folder.h (hle_dvd.cpp reads every file through it).
    std::string discError;
    if (disc.empty() || !port::discfolder::open(disc, discError)) {
        if (disc.empty())
            std::fprintf(stderr, "Could not open an Animal Crossing: City Folk disc. Pass --disc <disc image or extracted folder>.\n");
        else
            std::fprintf(stderr, "Could not open the disc %s: %s\n", disc.c_str(), discError.c_str());
        aurora_shutdown();
        return 1;
    }
    const u8* dol = port::discfolder::dol().data();
    port::dol::setDol(dol, port::discfolder::dol().size());
    const char* discId = port::dol::versionId(port::dol::version());
    if (std::strcmp(discId, wantId) != 0) {
        std::fprintf(stderr, "The game was recompiled from Animal Crossing: City Folk %s; the disc is %s.\n", wantId, discId);
        aurora_shutdown();
        return 1;
    }

    menu::init();
    g_mem = allocGuestMemory();
    loadDol(dol);
    initTimeBase();
    initTrace();
    initWatch();
#ifndef CF_STATIC_MODULE
    if (std::string err; !loadGameModule(launch.module, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        aurora_shutdown();
        return 1;
    }
#endif
    std::printf("Opened %s (%s)\n", disc.c_str(), discId);
    texpacks::init(discId);  // texture packs + dumper (docs/TEXTURE_PACKS.md), loaded in the background

    frame::start(&boot);
    if (std::getenv("CF_WATCHDOG")) {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                sched::dumpThreads();
                if (const char* peek = std::getenv("CF_PEEK")) {  // CF_PEEK=addr,addr,...: guest words
                    std::string line = "[peek]";
                    for (const char* p = peek; *p;) {
                        char* end = nullptr;
                        const u32 a = u32(std::strtoul(p, &end, 16));
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), " %08X=%08X", a, rd32(a));
                        line += buf;
                        p = (*end == ',') ? end + 1 : end;
                        if (end == p && *p) break;
                    }
                    log("%s", line.c_str());
                }
            }
        }).detach();
    }

    bool exiting = false;
    bool paused = false;
    // Test aid CF_QUIT_AFTER=<seconds>: a normal quit after that long (the shader and pipeline
    // caches are written as on a user's exit; a killed test run loses what it compiled).
    const double quitAfter = [] {
        const char* v = std::getenv("CF_QUIT_AFTER");
        return v ? std::atof(v) : 0.0;
    }();
    const auto runStart = std::chrono::steady_clock::now();
    port::setPhase("game loop");
    // Stalls the watchdog cannot see because this loop keeps running (port::reportStall): no frame
    // presented (aurora_begin_frame failing), or with interpolation the game thread no longer
    // finishing frames while the display replays its last one.
    using SteadyClock = std::chrono::steady_clock;
    auto lastPresented = SteadyClock::now();
    auto lastGuestProgress = SteadyClock::now();
    auto lastIteration = SteadyClock::now();
    u32 lastGuestFrames = frame::guestFrames();
    bool presentStallReported = false, guestStallReported = false;
    while (!exiting) {
        port::heartbeat();
        sStallOpen.store(false, std::memory_order_relaxed);
        if (!paused) port::setPhase("game loop: window events (aurora_update)");  // (paused: nullptr, a wait on purpose)
        if (quitAfter > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count() > quitAfter)
            break;
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_SDL_EVENT) menu::handleEvent(ev->sdl);
            if (ev->type == AURORA_EXIT) exiting = true;
            else if (ev->type == AURORA_PAUSED) {
                paused = true;
                port::setPhase(nullptr);  // waits for the window to come back: not a hang
                audio::outSetPaused(true);
            } else if (ev->type == AURORA_UNPAUSED) {
                paused = false;
                port::setPhase("game loop");
                audio::outSetPaused(false);
            }
        }
        // Shaders first (precache.h holdGame): at startup the game does not start, and after a
        // settings change that needs other shader variants it waits, until they are compiled.
        const bool hold = !exiting && precache::holdGame();
        static bool sHoldAudio = false;
        if (hold != sHoldAudio) {
            sHoldAudio = hold;
            if (!paused) audio::outSetPaused(hold);
        }
        // Test aid CF_FREEZE_AT=<frame>: from that game frame (copyDispCount) on, the guest is held
        // as for a focus loss while the host keeps presenting its last tape with the post-process
        // chain (A/B screenshots of one game frame, e.g. CRT signals with CF_CRT_CYCLE). Needs
        // interpolated presentation (frame rate above 30).
        static const u32 sFreezeAt = [] {
            const char* v = std::getenv("CF_FREEZE_AT");
            return v ? u32(std::strtoul(v, nullptr, 10)) : 0u;
        }();
        const bool frozen = sFreezeAt && copyDispCount() >= sFreezeAt;
        frame::setHostPaused(paused || hold || frozen);
        // Test aid CF_FREEZE_SHOTS=<dir>,<every>,<count> (with CF_FREEZE_AT; branch light-tiles): while
        // frozen, a CF_SHOT-style screenshot of the held frame every <every> loop iterations,
        // <dir>/z<k>.ppm (with AURORA_ENH_TILES_AB the same frame with and without light tiles).
        if (frozen && !paused && !hold) {
            static struct FreezeShots {
                std::string dir;
                unsigned every = 0, count = 0, n = 0, k = 0;
            } sFz = [] {
                FreezeShots f;
                if (const char* v = std::getenv("CF_FREEZE_SHOTS")) {
                    const std::string s = v;
                    const auto c = s.find(',');
                    if (c != std::string::npos && std::sscanf(s.c_str() + c + 1, "%u,%u", &f.every, &f.count) == 2) f.dir = s.substr(0, c);
                }
                return f;
            }();
            if (!sFz.dir.empty() && sFz.every && sFz.k < sFz.count && ++sFz.n % sFz.every == 0) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/z%03u.ppm", sFz.dir.c_str(), sFz.k++);
                aurora_request_screenshot(path);
            }
        }
        // Test aid CF_SHOT_RUN=<dir>,<frame>,<count>[+<frame>,<count>...] (branch face-shadow): from that
        // game frame (copyDispCount) on, a CF_SHOT-style render-resolution screenshot of every loop
        // iteration (each present, in-betweens included) for <count> iterations, <dir>/r<frame>_<k>.ppm
        // (the game frame when requested, the iteration). The writes slow the presentation down; the
        // in-betweens still come from the presentation clock (alpha logged by CF_INTERP_LOG).
        {
            struct ShotRun {
                u32 frame = 0, count = 0, k = 0;
            };
            static std::string sRunDir;
            static std::vector<ShotRun> sRuns = [] {
                std::vector<ShotRun> v;
                if (const char* e = std::getenv("CF_SHOT_RUN")) {
                    std::string s = e;
                    const auto c = s.find(',');
                    if (c != std::string::npos) {
                        sRunDir = s.substr(0, c);
                        for (std::string rest = s.substr(c + 1); !rest.empty();) {
                            const auto semi = rest.find_first_of(";+");  // ('+' in run.ps1 -Env, which splits on ';')
                            ShotRun r;
                            if (std::sscanf(rest.c_str(), "%u,%u", &r.frame, &r.count) == 2) v.push_back(r);
                            rest = semi == std::string::npos ? "" : rest.substr(semi + 1);
                        }
                    }
                }
                return v;
            }();
            for (ShotRun& r : sRuns) {
                if (paused || hold || r.k >= r.count || copyDispCount() < r.frame) continue;
                char path[512];
                std::snprintf(path, sizeof path, "%s/r%05u_%03u.ppm", sRunDir.c_str(), copyDispCount(), r.k++);
                aurora_request_screenshot(path);
            }
        }
        {
            const auto now = SteadyClock::now();
            // (A gap between iterations is a stall of this thread itself, which the watchdog reports.)
            if (paused || now - lastIteration > std::chrono::seconds(1)) {
                lastPresented = now;
                lastGuestProgress = now;
            }
            lastIteration = now;
            if (const u32 g = frame::guestFrames(); g != lastGuestFrames || paused || hold || frozen || !frame::interpolating()) {
                lastGuestFrames = g;
                lastGuestProgress = now;
                if (guestStallReported) log("[hang] the game finishes frames again");
                guestStallReported = false;
            } else if (!guestStallReported && now - lastGuestProgress > std::chrono::seconds(15)) {
                guestStallReported = true;
                port::reportStall("the game has not finished a frame for 15 s while the display loop runs (interpolated "
                                  "presentation replays its last frame)",
                                  true);
                sched::dumpThreads();
            }
            if (!presentStallReported && now - lastPresented > std::chrono::seconds(10)) {
                presentStallReported = true;
                port::reportStall("the main loop runs but no frame could be begun for 10 s (aurora_begin_frame fails)", true);
            }
        }
        if (exiting || paused) continue;
        if (hold) {
            port::setPhase("game loop: shader preparation frame");
            const auto beginAt = SteadyClock::now();
            if (!aurora_begin_frame()) continue;
            const double beginMs = std::chrono::duration<double, std::milli>(SteadyClock::now() - beginAt).count();
            lastPresented = SteadyClock::now();
            presentStallReported = false;
            menu::draw();  // applies settings changes (they may change what is being compiled)
            precache::drawHold();
            precache::frame();
            aurora_end_frame();
            // Hold frames well below the display's refresh rate (issue #33): a swap chain image is
            // then always free, so the render worker never waits for one inside Dawn, where the wait
            // holds the device lock every pipeline creation needs.
            precache::holdFramePause(beginMs);
            if (menu::quitRequested()) break;
            continue;
        }
        // (Phases name the step in a hang report: issue #11.)
        port::setPhase("game loop: frame pacing wait");
        pacing::beforeFrame();
        interp::hostMark(0);
        port::setPhase("game loop: aurora_begin_frame (frame slot, staging buffer)");
        if (!aurora_begin_frame()) continue;
        lastPresented = SteadyClock::now();
        if (presentStallReported) log("[hang] frames are presented again");
        presentStallReported = false;
        pacing::afterBegin();
        interp::hostMark(1);
        ws::beginFrame();
        port::setPhase(frame::interpolating() ? "game loop: replaying a game frame" : "game loop: running the game (lockstep)");
        const bool running = frame::hostFrame();
        pacing::afterReplay();
        interp::hostMark(2);
        port::setPhase("game loop: F1 menu / overlays");
        menu::draw();
        precache::frame();
        port::setPhase("game loop: present hold");
        pacing::beforeEnd();
        port::setPhase("game loop: aurora_end_frame (GX FIFO drain, hand-off to the render worker)");
        aurora_end_frame();
        port::setPhase("game loop");
        pacing::afterFrame();
        interp::hostMark(3);
        frame::pace();
        interp::hostMark(4);
        if (!running || menu::quitRequested()) break;
    }

    port::setPhase("shutdown (releasing graphics objects, writing caches)");
    menu::shutdown();
    wiimote::hidShutdown();  // real Wii Remotes: motor off, devices closed
    vi::requestStop();
    frame::stop();
    vi::stopTimer();
    audio::outShutdown();
    texpacks::shutdown();
    aurora_shutdown();
    port::recovery::onExit();
    std::_Exit(0);
}
