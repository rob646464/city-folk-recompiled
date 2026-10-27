// HLE: VI. A host timer raises the retrace interrupt at the rate of the TV mode the game configures
// (VIConfigure: 59.94 Hz NTSC / MPAL / EuRGB60, 50 Hz PAL); the game paces itself on it. VISetNextFrameBuffer marks a finished frame, which is
// handed to the host loop (see frame.h) before the game continues.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/cpu_qos.h"
#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/game.h"
#include "recomp/system.h"

#include <dolphin/gx.h>
#include <dolphin/vi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace recomp {
bool wpadRadioActive();  // hle_wpad.cpp: a Wii Remote is connected (reports arrive)
namespace {

std::atomic<u32> sRetraceCount{0};
u32 sPreCallback = 0, sPostCallback = 0;
u32 sRetraceQueue = 0;  // guest-address key for VIWaitForRetrace sleepers
bool sBlack = true;
std::thread sTimer;
std::atomic<bool> sTimerRun{false};
std::atomic<bool> sRetracePending{false};  // fast-forward: one retrace in flight at a time

void retrace(Ctx& c) {
    sRetracePending = false;
    const u32 count = sRetraceCount.fetch_add(1) + 1;
    if (sPreCallback) guestCall(c, sPreCallback, count);
    if (sPostCallback) guestCall(c, sPostCallback, count);
    sched::wakeup(sRetraceQueue);
}

bool palGame() { return port::install().running == port::GameVersion::PAL; }

// The TV format of the render mode the game last passed to VIConfigure (VI_NTSC 0, VI_PAL 1,
// VI_MPAL 2, VI_DEBUG 3, VI_DEBUG_PAL 4, VI_EURGB60 5; -1 before the first call).
std::atomic<int> sTvFormat{-1};
bool fiftyHz(int fmt) { return fmt == 1 || fmt == 4; }

// VI rate = the TV mode the game configures. The European game picks EuRGB60 (60 Hz) or PAL
// (50 Hz) from the console setting (SYSCONF IPL.E60 = F1 > Game > Console "60 Hz mode
// (EuRGB60)", sys::settings().eurgb60, default on); before its first VIConfigure that setting
// decides. City Folk paces itself on the retrace, so its frame rate and speed follow. Read
// every tick.
double retraceHz() {
    const int fmt = sTvFormat.load(std::memory_order_relaxed);
    const bool fifty = fmt >= 0 ? fiftyHz(fmt) : palGame() && !sys::settings().eurgb60;
    return fifty ? 50.0 : 60000.0 / 1001.0;
}

}  // namespace

namespace vi {
void startTimer() {
    sRetraceQueue = hleAlloc(8);
    sTimerRun = true;
    sTimer = std::thread([] {
        cpu::setCurrent(cpu::Role::Timer);
        using clock = std::chrono::steady_clock;
        auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / retraceHz()));
        auto next = clock::now() + period;
        while (sTimerRun) {
            period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / retraceHz()));
            if (const double speed = game::retraceSpeed(); speed > 1.0) {
                // Fast-forward (skip.h): retraces as fast as the game takes them, up to `speed` x.
                const auto fast = std::chrono::duration_cast<clock::duration>(period / speed);
                while (clock::now() < next && sTimerRun) std::this_thread::yield();
                next = std::max(next + fast, clock::now());
                if (!frame::gamePaused() && !sRetracePending.exchange(true)) sched::postIrq(&retrace);
                continue;
            }
            std::this_thread::sleep_until(next);
            next += period;
            if (clock::now() > next + period * 4) next = clock::now() + period;  // don't spiral after a stall
            if (frame::gamePaused()) continue;
            // An RVL game's post-retrace callback can end the frame itself (EGG::AsyncDisplay calls
            // VISetNextFrameBuffer there), so with no guest thread holding the CPU this thread may
            // run present() and wait for the host; at shutdown that wait ends in ThreadExit.
            try {
                sched::postIrq(&retrace);
            } catch (const sched::ThreadExit&) {
                break;
            }
        }
    });
}
// Shutdown: stopTimer() only after frame::stop(), which releases a present() the timer thread may
// be waiting in (it used to wait there for the host forever while the main thread joined it).
void requestStop() { sTimerRun = false; }
void stopTimer() {
    sTimerRun = false;
    if (sTimer.joinable()) sTimer.join();
}
}  // namespace vi

u32 hw::viRead(u32 addr, int size) {
    switch (addr & 0xFFF) {
    case 0x2C:  // VI_DISP_POS: vertical beam position (high half)
        return size == 4 ? (1u << 16) : 1;
    case 0x6E:  // VI_CLK_SEL / DTV status
        return 0;
    default:
        return 0;
    }
}

HLE(VIInit) {}
// The render mode sizes Aurora's frame buffer (fbWidth x efbHeight): RVL games often draw
// 456 or 528 lines instead of 480 (City Folk: 640x456), which Aurora otherwise shows as the top
// of a 640x480 EFB with the clear colour below. GXRenderModeObj (guest, big-endian): viTVmode
// u32 +0, fbWidth +4, efbHeight +6, xfbHeight +8, viXOrigin +A, viYOrigin +C, viWidth +E,
// viHeight +10 (u16), xFBmode u32 +14, field_rendering +18, aa +19, sample_pattern[12][2] +1A,
// vfilter[7] +32.
HLE(VIConfigure) {
    const u32 rm = c.r[3];
    if (!rm) return;
    GXRenderModeObj m{};
    m.viTVmode = VITVMode(rd32(rm));
    if (const int fmt = int(u32(m.viTVmode) >> 2); fmt != sTvFormat.load()) {
        log("[vi] TV format %d (%s Hz)", fmt, fiftyHz(fmt) ? "50" : "59.94");
        sTvFormat = fmt;
    }
    m.fbWidth = u16(rd16(rm + 4));
    m.efbHeight = u16(rd16(rm + 6));
    m.xfbHeight = u16(rd16(rm + 8));
    m.viXOrigin = u16(rd16(rm + 0xA));
    m.viYOrigin = u16(rd16(rm + 0xC));
    m.viWidth = u16(rd16(rm + 0xE));
    m.viHeight = u16(rd16(rm + 0x10));
    m.xFBmode = VIXFBMode(rd32(rm + 0x14));
    m.field_rendering = u8(rd8(rm + 0x18));
    m.aa = u8(rd8(rm + 0x19));
    for (int i = 0; i < 12; i++)
        for (int j = 0; j < 2; j++) m.sample_pattern[i][j] = u8(rd8(rm + 0x1A + i * 2 + j));
    for (int i = 0; i < 7; i++) m.vfilter[i] = u8(rd8(rm + 0x32 + i));
    static u32 sLast = 0;
    const u32 key = u32(m.fbWidth) << 16 | m.efbHeight;
    if (key != sLast) {
        log("[vi] VIConfigure: tv mode %u, fb %ux%u, xfb %u lines, vi %ux%u at (%u,%u), xfb mode %u, aa %u",
            u32(m.viTVmode), m.fbWidth, m.efbHeight, m.xfbHeight, m.viWidth, m.viHeight, m.viXOrigin, m.viYOrigin,
            u32(m.xFBmode), m.aa);
        sLast = key;
    }
    if (m.fbWidth < 16 || m.fbWidth > 720 || m.efbHeight < 16 || m.efbHeight > 528) return;
    ::VIConfigure(&m);
}
HLE(VIConfigurePan) {}
HLE(VIFlush) {}
HLE(VISetBlack) { sBlack = c.r[3] != 0; }
HLE(VIGetRetraceCount) { c.r[3] = sRetraceCount.load(); }
HLE(VIGetNextField) { c.r[3] = 1; }
HLE(VIGetCurrentLine) { c.r[3] = 0; }
// The configured format (VI_EURGB60 for the European game at 60 Hz); before VIConfigure the console's.
HLE(VIGetTvFormat) {
    const int fmt = sTvFormat.load();
    c.r[3] = fmt >= 0 ? u32(fmt) : palGame() ? (sys::settings().eurgb60 ? 5u : 1u) : 0u;
}
HLE(VIGetDTVStatus) { c.r[3] = 0; }
// RVL: screen burn-in dimming (the console darkens the picture after minutes without input).
// VIGetDimmingCount = retraces left before dimming. The SDK resets the count on every Wii Remote
// report (__VIResetRFIdle), so it only runs out while no Remote talks to the console: City Folk
// then says "Communications with the Wii Remote have been interrupted" (hle_wpad.cpp).
HLE(VIGetDimmingCount) { c.r[3] = wpadRadioActive() ? 18000 : 0; }
HLE(VIEnableDimming) { c.r[3] = 0; }
HLE(VIResetDimmingCount) {}
HLE(VISetPreRetraceCallback) {
    const u32 old = sPreCallback;
    sPreCallback = c.r[3];
    c.r[3] = old;
}
HLE(VISetPostRetraceCallback) {
    const u32 old = sPostCallback;
    sPostCallback = c.r[3];
    c.r[3] = old;
}
HLE(VIWaitForRetrace) {
    const u32 start = sRetraceCount.load();
    while (sRetraceCount.load() == start) sched::sleepOn(sRetraceQueue);
}
// RVL games built on EGG (City Folk: EGG::AsyncDisplay) set the next frame buffer from their
// retrace callback, at an interrupt that can arrive in the middle of a draw (the poll at a loop
// back-edge between GXBegin and the last vertex). Presenting there ended the Aurora frame inside
// the draw (malformed FIFO data: vertices cut off from their draw command). The first such call
// switches to presenting at GXCopyDisp, where the frame's drawing is finished.
std::atomic<bool> sCopyPresents{false};
HLE(VISetNextFrameBuffer) {
    if (frame::presentOnCopy()) return;
    if (sched::inIrq()) {
        sCopyPresents = true;
        log("[vi] VISetNextFrameBuffer from an interrupt (lr %08X): frames end at GXCopyDisp", c.lr);
        return;
    }
    frame::present(c, c.r[3], sBlack);
}

bool frame::presentOnCopy() { return sCopyPresents.load(std::memory_order_relaxed); }

bool frame::viBlack() { return sBlack; }

}  // namespace recomp

namespace recomp {
u32 viRetraceCount() { return sRetraceCount.load(); }

// Time stamp of the frame being presented, in VI fields, for interpolation (interp::endTape):
// City Folk paces its frames on the retrace (EGG::AsyncDisplay), one frame per field in every
// region; a field lasts vi::fieldSeconds() (1001/60000 s, or 1/50 s for a 50 Hz European game).
u32 frameStamp() { return sRetraceCount.load(); }
namespace vi {
double fieldSeconds() { return 1.0 / retraceHz(); }
}  // namespace vi
}  // namespace recomp
