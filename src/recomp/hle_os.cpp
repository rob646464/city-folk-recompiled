// HLE: OS (threads, messages, mutexes, interrupts, caches, time, reset) and
// the small debug/serial units.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/efb_copy.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/game.h"
#include "recomp/system.h"
#include "recomp/ios.h"
#include "recomp/menu.h"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace recomp {
namespace {

// Initial stack pointer from __init_registers: the `lis r1, hi` / `ori r1, r1, lo` pair (the RVL
// SDK's version clears every register first). The arena starts just above the stack.
u32 initialStack() {
    const u32 f = guestSymbol("__init_registers");
    u32 hi = 0;
    for (u32 pc = f; pc < f + 0x100; pc += 4) {
        const u32 w = rd32(pc);
        if ((w >> 26) == 15 && ((w >> 21) & 31) == 1 && ((w >> 16) & 31) == 0) hi = w << 16;               // lis r1, hi
        else if ((w >> 26) == 24 && ((w >> 21) & 31) == 1 && ((w >> 16) & 31) == 1) return hi | (w & 0xFFFF);  // ori r1, r1, lo
    }
    fatal("__init_registers: no stack pointer");
}

u32 sProgressiveMode = 0;
u32 sSoundMode = 1;  // stereo

void setBool(Ctx& c, bool v) { c.r[3] = v ? 1 : 0; }

// Wii memory layout as IOS and the apploader leave it (docs/WII.md "Memory"; public Wii memory
// map documentation). MEM2's top belongs to IOS; the PPC may use 0x90000800..0x933E0000, the IPC
// buffer is 0x933E0000..0x93400000.
constexpr u32 kMem2ArenaLo = 0x90000800, kMem2ArenaHi = 0x933E0000;
constexpr u32 kIpcBufferLo = 0x933E0000, kIpcBufferHi = 0x93400000;
constexpr u32 kMem2PpcEnd = 0x93400000;
constexpr u32 kIosVersion = 0x00350000 | 0x0F;  // IOS53 (City Folk's title IOS) v15 (major << 16 | revision)

struct LowWord {
    u32 addr, value;
};
// Values that stay as set for the whole run (checked once per frame: verifyLowMemory).
const LowWord kLowWords[] = {
    {0x80000028, kMem1Size},  // physical MEM1 size
    {0x800000F0, kMem1Size},  // simulated MEM1 size
    {0x800000F8, kBusClock},  // bus clock
    {0x800000FC, kCpuClock},  // CPU clock
    {0x80003100, kMem1Size},  // MEM1 physical size
    {0x80003104, kMem1Size},  // MEM1 simulated size
    {0x80003108, 0x81800000}, // MEM1 end
    {0x80003118, kMem2Size},  // MEM2 physical size
    {0x8000311C, kMem2Size},  // MEM2 simulated size
    {0x80003120, kMem2PpcEnd},   // end of MEM2 the PPC may use
    {0x80003130, kIpcBufferLo},  // IOS IPC buffer
    {0x80003134, kIpcBufferHi},
    {0x80003138, 0x00000011},    // Hollywood version
    {0x80003140, kIosVersion},   // IOS version
};

}  // namespace

// --- boot -------------------------------------------------------------------

HLE(_ExitProcess) {
    log("[os] _ExitProcess");
    std::exit(0);
}

// The OS start-up (os/OS.c OSInit): low memory as IOS and the apploader leave it, the MEM1 and
// MEM2 arenas. Everything below the OS (interrupts, IPC, alarms, EXI/SI) is HLE'd separately.
HLE(OSInit) {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    // Disc header (boot.bin) at 0x80000000: game id, maker, disc number, version.
    std::memcpy(hostp(0x80000000), port::gameId(port::install().running), 6);
    wr8(0x80000006, 0);
    wr8(0x80000007, u8(port::partitionRevision(port::install().running)));  // partition header: 1 (Korea 0)
    wr32(0x80000018, 0x5D1C9EA3);  // Wii disc magic
    wr32(0x8000001C, 0);
    wr32(0x80000020, 0x0D15EA5E);  // normal boot
    wr32(0x80000024, 1);
    wr32(0x8000002C, 0x00000023);  // retail Wii (RVL production)
    wr32(0x800000CC, port::install().running == port::GameVersion::PAL ? 1 : 0);  // VI_NTSC / VI_PAL
    for (const LowWord& w : kLowWords) wr32(w.addr, w.value);
    // MEM1 arena: from the top of the boot stack to below the FST (which the apploader puts at the
    // top of MEM1; the DVD is HLE'd, so the FST itself is not in guest memory).
    const u32 fstMax = 0x10000;
    const u32 arenaHi = (0x81800000u - fstMax) & ~31u;
    wr32(0x80000034, arenaHi);
    wr32(0x80000038, arenaHi);
    wr32(0x8000003C, fstMax);
    wr32(0x8000310C, (initialStack() + 8 + 31) & ~31u);  // MEM1 arena start
    wr32(0x80003110, arenaHi);                           // MEM1 arena end
    wr32(0x80003124, kMem2ArenaLo);                      // MEM2 usable start / end
    wr32(0x80003128, kMem2ArenaHi);
    std::memcpy(hostp(0x80003180), port::gameId(port::install().running), 4);       // the running title's game id
    wr8(0x80003184, 0x80);                               // application type: disc
    guestCall(c, guestSymbol("OSSetMEM1ArenaLo"), (initialStack() + 8 + 31) & ~31u);
    guestCall(c, guestSymbol("OSSetMEM1ArenaHi"), arenaHi);
    guestCall(c, guestSymbol("OSSetMEM2ArenaLo"), kMem2ArenaLo);
    guestCall(c, guestSymbol("OSSetMEM2ArenaHi"), kMem2ArenaHi);
    // The encrypted setting.txt the System Menu leaves for the SC library (product area, region).
    sys::writeSettingTxtToLowMemory();
    ios::startTestAids();
    log("[os] OSInit: MEM1 arena %08X-%08X, MEM2 arena %08X-%08X", (initialStack() + 8 + 31) & ~31u, arenaHi,
        kMem2ArenaLo, kMem2ArenaHi);
}

// IPC buffer (os/OSIpc.c): the guest-side buffer IOS requests are built in.
namespace {
u32 sIpcLo = kIpcBufferLo, sIpcHi = kIpcBufferHi;
}
HLE(__OSInitIPCBuffer) {
    sIpcLo = rd32(0x80003130);
    sIpcHi = rd32(0x80003134);
}
HLE(__OSGetIPCBufferHi) { c.r[3] = sIpcHi; }
HLE(__OSGetIPCBufferLo) { c.r[3] = sIpcLo; }
HLE(__OSSetIPCBufferLo) { sIpcLo = c.r[3]; }

// Boot values in low memory that game code reads at run time: checked once per frame (frame.cpp)
// and restored, with an error in the log, if anything wrote over them.
void verifyLowMemory() {
    for (const LowWord& w : kLowWords) {
        const u32 v = rd32(w.addr);
        if (v == w.value) continue;
        static int sReports = 0;
        if (sReports++ < 20) log("[os] ERROR: low memory %08X overwritten (%08X, expected %08X); restored", w.addr, v, w.value);
        wr32(w.addr, w.value);
    }
}

// --- alarms (used by the PAL release's frame pacing, LMDisplayUtil) -----------
// OSAlarm (0x28 bytes): handler at +0. A host timer raises the handler as an interrupt;
// re-arming or cancelling an alarm invalidates the pending expiry.

namespace {
std::mutex sAlarmM;
std::unordered_map<u32, u32> sAlarmGen;  // alarm -> generation of its pending expiry
using AlarmClock = std::chrono::steady_clock;
std::unordered_map<u32, AlarmClock::time_point> sAlarmFired;  // alarm -> when it last expired

void armAlarm(u32 alarm, u64 ticks, u32 handler, u64 period) {
    // Fast-forward (skip.h) speeds alarms up like it speeds up retraces.
    const double speed = std::max(1.0, game::retraceSpeed());
    const auto delay = std::chrono::duration_cast<AlarmClock::duration>(
        std::chrono::duration<double>(double(ticks) / kTimerClock / speed));
    const auto now = AlarmClock::now();
    auto when = now + delay;
    u32 gen;
    {
        std::lock_guard lk(sAlarmM);
        gen = ++sAlarmGen[alarm];
        // An alarm re-armed from its own expiry (the PAL frame pacer: wait for the alarm,
        // set it again) counts from the expiry, not from when the host got around to it:
        // on the console that gap is microseconds, here it includes host scheduling and
        // the lockstep wait for the display, which would otherwise slow the game down.
        if (auto it = sAlarmFired.find(alarm); it != sAlarmFired.end() && now - it->second < delay)
            when = it->second + delay;
    }
    wr32(alarm, handler);
    const double seconds = std::chrono::duration<double>(when - now).count();
    static const bool sAlarmLog = std::getenv("CF_ALARM_LOG") != nullptr;
    if (sAlarmLog)
        log("[alarm] %08X ticks %llu (%.2f ms) fires in %.2f ms speed %.2f", alarm, (unsigned long long)ticks,
            double(ticks) / kTimerClock * 1000.0, seconds * 1000.0, speed);
    sched::postIrqAfter(std::max(0.0, seconds), [alarm, gen, handler, period, when](Ctx& c) {
        {
            std::lock_guard lk(sAlarmM);
            if (sAlarmGen[alarm] != gen) return;  // cancelled or re-armed
            sAlarmFired[alarm] = when;
        }
        if (period) armAlarm(alarm, period, handler, period);
        guestCall(c, handler, alarm, 0);
    });
}
}  // namespace

static u64 gameClock();  // the clock the game reads (below)

// RVL OSAlarm is 0x30 bytes: the GameCube layout plus user data at +0x28.
HLE(OSSetAlarmUserData) { wr32(c.r[3] + 0x28, c.r[4]); }
HLE(OSGetAlarmUserData) { c.r[3] = rd32(c.r[3] + 0x28); }
HLE(OSCreateAlarm) {
    std::memset(hostp(c.r[3]), 0, 0x30);
    std::lock_guard lk(sAlarmM);
    ++sAlarmGen[c.r[3]];
    sAlarmFired.erase(c.r[3]);
}
HLE(OSSetAlarm) { armAlarm(c.r[3], u64(c.r[5]) << 32 | c.r[6], c.r[7], 0); }
HLE(OSSetPeriodicAlarm) {
    // (alarm, u64 start r5:r6, u64 period r7:r8, handler r9); start is absolute time.
    const u64 now = gameClock(), start = u64(c.r[5]) << 32 | c.r[6], period = u64(c.r[7]) << 32 | c.r[8];
    armAlarm(c.r[3], start > now ? start - now : 0, c.r[9], period ? period : 1);
}
HLE(OSCancelAlarm) {
    std::lock_guard lk(sAlarmM);
    ++sAlarmGen[c.r[3]];
}

// --- interrupts ---------------------------------------------------------------

HLE(OSDisableInterrupts) {
    const u32 old = (c.msr >> 15) & 1;
    c.msr &= ~0x8000u;
    c.r[3] = old;
}
HLE(OSEnableInterrupts) {
    const u32 old = (c.msr >> 15) & 1;
    c.msr |= 0x8000u;
    c.r[3] = old;
    c.poll();
}
HLE(OSRestoreInterrupts) {
    const u32 old = (c.msr >> 15) & 1;
    if (c.r[3]) c.msr |= 0x8000u;
    else c.msr &= ~0x8000u;
    c.r[3] = old;
    c.poll();
}
HLE(__OSSetInterruptHandler) { c.r[3] = 0; }
HLE(__OSUnmaskInterrupts) { c.r[3] = 0; }
HLE(__OSMaskInterrupts) { c.r[3] = 0; }

// --- threads ------------------------------------------------------------------

HLE(OSCreateThread) {
    setBool(c, sched::create(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], s32(c.r[8]), c.r[9]));
}
HLE(OSResumeThread) { c.r[3] = u32(sched::resume(c.r[3])); }
HLE(OSSuspendThread) { c.r[3] = u32(sched::suspend(c.r[3])); }
HLE(OSCancelThread) { sched::cancel(c.r[3]); }
HLE(OSExitThread) { sched::exitThread(c.r[3]); }
HLE(OSGetCurrentThread) { c.r[3] = sched::currentGuest(); }
HLE(OSGetThreadPriority) { c.r[3] = u32(sched::priority(c.r[3])); }
HLE(OSSetThreadPriority) { setBool(c, sched::setPriority(c.r[3], s32(c.r[4]))); }
HLE(OSSetSwitchThreadCallback) { c.r[3] = 0; }  // (debugging aid; no callback runs)
HLE(OSIsThreadSuspended) { setBool(c, sched::suspendCount(c.r[3]) > 0); }
// The scheduler lock (OSDisableScheduler/OSEnableScheduler): a nesting count; the host scheduler
// switches threads only at blocking calls and interrupts, which the callers avoid meanwhile.
namespace {
s32 sSchedulerLock = 0;
u32 sResetCallback = 0, sPowerCallback = 0;
}  // namespace
HLE(OSDisableScheduler) { c.r[3] = u32(sSchedulerLock++); }
HLE(OSEnableScheduler) { c.r[3] = u32(sSchedulerLock > 0 ? sSchedulerLock-- : 0); }
// Reset / Power button callbacks (os/OSStateTM.c). The OS's STM event hook is host-side: a console
// button event (ios::stmEvent: F1 / test aid) calls the game's callback as the OS's event handler
// does, with interrupts disabled. No callback registered = the OS default, which does nothing.
HLE(OSSetResetCallback) {
    const u32 old = sResetCallback;
    sResetCallback = c.r[3];
    c.r[3] = old;
}
HLE(OSSetPowerCallback) {
    const u32 old = sPowerCallback;
    sPowerCallback = c.r[3];
    c.r[3] = old;
}
HLE(__OSInitSTM) { c.r[3] = 1; }
// RVL SDK: each library registers its version string (shown by the system's debug tools).
HLE(OSRegisterVersion) {
    static const bool sLog = std::getenv("CF_OS_LOG") != nullptr;
    if (sLog) log("[os] %s", gstr(c.r[3]));
}
HLE(OSIsThreadTerminated) {
    const bool t = sched::isTerminated(c.r[3]);
    // CF_THREAD_LOG=1: a thread's terminated state each time a caller sees it change (who waits on whom).
    static const bool on = std::getenv("CF_THREAD_LOG") != nullptr;
    if (on) {
        static std::mutex m;
        static std::unordered_map<u32, int> last;
        std::lock_guard lk(m);
        auto [it, fresh] = last.try_emplace(c.r[3], -1);
        if (it->second != int(t))
            log("[thread] OSIsThreadTerminated(%08X) = %d (lr %08X, frame %u)", c.r[3], int(t), c.lr, game::guestFrames());
        it->second = int(t);
    }
    setBool(c, t);
}
HLE(OSDetachThread) { sched::detach(c.r[3]); }
HLE(OSInitThreadQueue) {
    wr32(c.r[3], 0);
    wr32(c.r[3] + 4, 0);
}
HLE(OSSleepThread) { sched::sleepOn(c.r[3]); }
HLE(OSWakeupThread) { sched::wakeup(c.r[3]); }
HLE(OSYieldThread) { c.poll(); }

// --- message queues (OSMessageQueue: send queue, receive queue, array, count, first, used)

HLE(OSInitMessageQueue) {
    const u32 mq = c.r[3];
    for (u32 i = 0; i < 0x10; i += 4) wr32(mq + i, 0);
    wr32(mq + 0x10, c.r[4]);
    wr32(mq + 0x14, c.r[5]);
    wr32(mq + 0x18, 0);
    wr32(mq + 0x1C, 0);
}

bool traceMq() {
    static const bool on = std::getenv("CF_TRACE_MQ") != nullptr;
    return on;
}

HLE(OSSendMessage) {
    const u32 mq = c.r[3], msg = c.r[4], flags = c.r[5];
    if (traceMq()) log("[mq] send %08X msg %08X flags %u (lr %08X) used %u", mq, msg, flags, c.lr, rd32(mq + 0x1C));
    for (;;) {
        const u32 count = rd32(mq + 0x14), used = rd32(mq + 0x1C);
        if (used < count) break;
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sched::sleepOn(mq);
    }
    const u32 count = rd32(mq + 0x14), first = rd32(mq + 0x18), used = rd32(mq + 0x1C);
    wr32(rd32(mq + 0x10) + ((first + used) % count) * 4, msg);
    wr32(mq + 0x1C, used + 1);
    sched::wakeup(mq + 8);
    c.r[3] = 1;
}

HLE(OSJamMessage) {
    const u32 mq = c.r[3], msg = c.r[4], flags = c.r[5];
    for (;;) {
        if (rd32(mq + 0x1C) < rd32(mq + 0x14)) break;
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sched::sleepOn(mq);
    }
    const u32 count = rd32(mq + 0x14);
    const u32 first = (rd32(mq + 0x18) + count - 1) % count;
    wr32(rd32(mq + 0x10) + first * 4, msg);
    wr32(mq + 0x18, first);
    wr32(mq + 0x1C, rd32(mq + 0x1C) + 1);
    sched::wakeup(mq + 8);
    c.r[3] = 1;
}

HLE(OSReceiveMessage) {
    const u32 mq = c.r[3], out = c.r[4], flags = c.r[5];
    if (traceMq()) log("[mq] recv %08X flags %u (lr %08X) used %u", mq, flags, c.lr, rd32(mq + 0x1C));
    while (rd32(mq + 0x1C) == 0) {
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sched::sleepOn(mq + 8);
    }
    const u32 count = rd32(mq + 0x14), first = rd32(mq + 0x18);
    if (out) wr32(out, rd32(rd32(mq + 0x10) + first * 4));
    wr32(mq + 0x18, (first + 1) % count);
    wr32(mq + 0x1C, rd32(mq + 0x1C) - 1);
    sched::wakeup(mq);
    c.r[3] = 1;
}

// --- mutexes (OSMutex: queue, owner thread, count, link) ------------------------

HLE(OSInitMutex) {
    for (u32 i = 0; i < 0x18; i += 4) wr32(c.r[3] + i, 0);
}
HLE(OSLockMutex) {
    const u32 m = c.r[3], self = sched::currentGuest();
    for (;;) {
        const u32 owner = rd32(m + 8);
        if (owner == 0) {
            wr32(m + 8, self);
            wr32(m + 0xC, 1);
            return;
        }
        if (owner == self) {
            wr32(m + 0xC, rd32(m + 0xC) + 1);
            return;
        }
        sched::sleepOn(m);
    }
}
HLE(OSTryLockMutex) {
    const u32 m = c.r[3], self = sched::currentGuest();
    const u32 owner = rd32(m + 8);
    if (owner == 0 || owner == self) {
        wr32(m + 8, self);
        wr32(m + 0xC, rd32(m + 0xC) + 1);
        c.r[3] = 1;
    } else {
        c.r[3] = 0;
    }
}
HLE(OSUnlockMutex) {
    const u32 m = c.r[3];
    if (rd32(m + 8) != sched::currentGuest()) return;
    const u32 n = rd32(m + 0xC) - 1;
    wr32(m + 0xC, n);
    if (n == 0) {
        wr32(m + 8, 0);
        sched::wakeup(m);
    }
}

// --- time -----------------------------------------------------------------------

// CF_TIME_LOG=1: the first call of OSGetTime/OSGetTick from each return address (who reads the
// clock: RNG seeds, timers), with the guest frame.
static void logTimeCaller(const char* fn, u32 lr) {
    static const bool on = std::getenv("CF_TIME_LOG") != nullptr;
    if (!on) return;
    static std::mutex m;
    static std::set<u32> seen;
    std::lock_guard lk(m);
    if (seen.insert(lr).second) log("[time] %s from lr %08X (guest frame %u)", fn, lr, game::guestFrames());
}

// Test aid CF_CLOCK_FRAMES=1 (scripted routes, with CF_CLOCK_START): the clock the game reads
// (OSGetTime/OSGetTick) follows guest frames instead of host time: frame n starts at n/59.94 s
// after the clock start, and each read inside a frame adds 1 us. Dates, times of day and values
// the game derives from the clock (City Folk's new player/town ids, its town generator's inputs)
// are then the same at the same frame in every run. A frame that reads the clock more than 8000
// times (a loop waiting for time) continues in real time from there, so waits still end. Reads
// are counted per guest thread (threads whose reads depend on host timing don't shift others).
// Host-side timing (alarms, IOS/DVD latency) stays real.
static u64 gameClock() {
    static const bool frameClock = std::getenv("CF_CLOCK_FRAMES") != nullptr;
    if (!frameClock) return timeBase();
    static std::mutex m;
    struct PerThread {
        u64 last = 0, frameStart = 0;
        u32 frame = ~0u, reads = 0;
    };
    static std::unordered_map<u32, PerThread> threads;  // guest thread -> its reads
    constexpr double kFrameTicks = kTimerClock / 59.94;
    constexpr u64 kReadTicks = u64(kTimerClock / 1e6);
    constexpr u32 kCountedReads = 8000;
    std::lock_guard lk(m);
    const u32 f = game::guestFrames();
    const u64 real = timeBase();
    PerThread& t = threads[sched::currentGuest()];  // per thread: other threads' reads don't shift it
    if (f != t.frame) {
        t.frame = f;
        t.frameStart = real;
        t.reads = 0;
    }
    u64 offset = u64(++t.reads) * kReadTicks;
    if (t.reads > kCountedReads) offset = std::max(offset, real - t.frameStart);
    const u64 v = timeBaseStart() + u64(double(f) * kFrameTicks) + offset;
    t.last = std::max(t.last + 1, v);  // never backwards
    return t.last;
}

HLE(OSGetTime) {
    logTimeCaller("OSGetTime", c.lr);
    const u64 t = gameClock();
    c.r[3] = u32(t >> 32);
    c.r[4] = u32(t);
}
HLE(OSGetTick) {
    logTimeCaller("OSGetTick", c.lr);
    c.r[3] = u32(gameClock());
}
// OSCalendarTime {sec, min, hour, mday, mon (0-11), year, wday, yday, msec, usec} -> ticks since
// 2000-01-01 (the inverse of OSTicksToCalendarTime; out-of-range fields carry over).
HLE(OSCalendarTimeToTicks) {
    const u32 td = c.r[3];
    auto f = [&](int i) { return s64(s32(rd32(td + 4 * i))); };
    s64 year = f(5), mon = f(4);
    year += mon >= 0 ? mon / 12 : -((11 - mon) / 12);
    mon = ((mon % 12) + 12) % 12;
    // Days from 2000-01-01 to the first of the month (proleptic Gregorian).
    auto daysFromCivil = [](s64 y, s64 m, s64 d) {  // m 1..12
        y -= m <= 2;
        const s64 era = (y >= 0 ? y : y - 399) / 400;
        const s64 yoe = y - era * 400;
        const s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;  // days since 1970-01-01
    };
    const s64 days = daysFromCivil(year, mon + 1, 1) - daysFromCivil(2000, 1, 1) + f(3) - 1;
    const s64 secs = days * 86400 + f(2) * 3600 + f(1) * 60 + f(0);
    const double ticks = double(secs) * kTimerClock + double(f(8)) * (kTimerClock / 1000.0) +
                         double(f(9)) * (kTimerClock / 1000000.0);
    const s64 t = s64(ticks);
    c.r[3] = u32(u64(t) >> 32);
    c.r[4] = u32(u64(t));
}
HLE(OSTicksToCalendarTime) {
    const s64 ticks = s64(u64(c.r[3]) << 32 | c.r[4]);
    const u32 td = c.r[5];
    const double secs = double(ticks) / kTimerClock;
    const std::time_t t = std::time_t(946684800 + s64(secs));
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    const double frac = secs - std::floor(secs);
    const u32 fields[10] = {u32(tm.tm_sec), u32(tm.tm_min), u32(tm.tm_hour), u32(tm.tm_mday), u32(tm.tm_mon),
                            u32(tm.tm_year + 1900), u32(tm.tm_wday), u32(tm.tm_yday), u32(frac * 1000),
                            u32(std::fmod(frac * 1e6, 1000.0))};
    for (int i = 0; i < 10; i++) wr32(td + i * 4, fields[i]);
}

// --- caches -------------------------------------------------------------------------

HLE(DCFlushRange) {}
HLE(DCFlushRangeNoSync) {}
HLE(DCStoreRange) {}
HLE(DCStoreRangeNoSync) {}
HLE(DCInvalidateRange) { efbcopy::cpuRead(c.r[3], c.r[4]); }  // EFB copies the CPU reads
HLE(ICInvalidateRange) {}
HLE(DCZeroRange) { std::memset(hostp(c.r[3] & ~31u), 0, (c.r[4] + 31) & ~31u); }
HLE(LCEnable) {}
HLE(LCDisable) {}
HLE(LCQueueWait) {}
HLE(LCQueueLength) { c.r[3] = 0; }  // locked-cache DMA completes at once
HLE(LCStoreData) {                  // (mem dest, lc src, bytes)
    std::memmove(hostp(c.r[3]), hostp(c.r[4]), (c.r[5] + 31) & ~31u);
    c.r[3] = (c.r[5] + 4095) / 4096;  // transactions queued
}
HLE(LCLoadData) {  // (lc dest, mem src, bytes)
    std::memmove(hostp(c.r[3]), hostp(c.r[4]), (c.r[5] + 31) & ~31u);
    c.r[3] = (c.r[5] + 4095) / 4096;
}
HLE(LCLoadBlocks) {  // (lc dest, mem src, blocks)
    const u32 n = c.r[5] ? c.r[5] : 128;
    std::memmove(hostp(c.r[3]), hostp(c.r[4]), n * 32);
}
HLE(LCStoreBlocks) {  // (mem dest, lc src, blocks)
    const u32 n = c.r[5] ? c.r[5] : 128;
    std::memmove(hostp(c.r[3]), hostp(c.r[4]), n * 32);
}

// --- contexts / errors --------------------------------------------------------------

HLE(OSClearContext) {}
HLE(OSSetCurrentContext) {}
HLE(OSFillFPUContext) {}
HLE(OSSetErrorHandler) { c.r[3] = 0; }
// os/OSPlayRecord-era app info (os/ TU, so HLE'd): the running title's game code and type. The
// SDK copies the 4-character code from low memory (0x80003194 when it holds a valid code, else
// 0x80003180, where OSInit puts the disc's id) into its static buffer at r13-0x2180 and returns
// it. NWC24 asks for it when City Folk's WiiConnect24 task lists its download tasks.
HLE(OSGetAppGamename) {
    const u8 c0 = rd8(0x80003194);
    const bool alnum = (c0 >= '0' && c0 <= '9') || (c0 >= 'A' && c0 <= 'Z');
    const u32 src = alnum ? 0x80003194 : 0x80003180;
    const u32 buf = c.r[13] - 0x2180;
    for (u32 i = 0; i < 4; i++) wr8(buf + i, rd8(src + i));
    wr8(buf + 4, 0);
    c.r[3] = buf;
}
HLE(OSGetAppType) { c.r[3] = rd8(0x80003184); }
HLE(OSPanic) { fatal("OSPanic %s:%u: %s", gstr(c.r[3]), c.r[4], gstr(c.r[5])); }
HLE(OSReport) { std::fputs(gstr(c.r[3]), stdout); }

// --- reset / settings ---------------------------------------------------------------

// Leaving the game (os/OSReset.c): back to the Wii Menu, a restart, power off. The program ends
// the way "Quit game" does (settings and caches written); the calling thread never returns, as
// on the console.
[[noreturn]] static void leaveGame(const char* what) {
    log("[os] %s: the game leaves; the program ends", what);
    menu::requestQuit();
    for (;;) sched::sleepOn(0xFFFFFFE0u);  // (a queue nothing wakes; shutdown ends the thread)
}
HLE(OSResetSystem) {
    char what[64];
    std::snprintf(what, sizeof(what), "OSResetSystem(%u, %08X, %u)", c.r[3], c.r[4], c.r[5]);
    leaveGame(what);
}
HLE(OSShutdownSystem) { leaveGame("OSShutdownSystem (power off)"); }
HLE(OSRestart) { leaveGame("OSRestart"); }
HLE(OSReturnToMenu) { leaveGame("OSReturnToMenu (Wii Menu)"); }
HLE(OSReturnToDataManager) { leaveGame("OSReturnToDataManager (Wii Menu data management)"); }
HLE(OSReturnToSetting) { leaveGame("OSReturnToSetting (Wii Settings)"); }
HLE(__OSReturnToMenuForError) { leaveGame("__OSReturnToMenuForError"); }
HLE(__OSHotResetForError) { leaveGame("__OSHotResetForError"); }
HLE(__OSShutdownToSBY) { leaveGame("__OSShutdownToSBY (standby)"); }
HLE(__OSHotReset) { leaveGame("__OSHotReset"); }
HLE(OSGetResetSwitchState) { c.r[3] = 0; }
HLE(OSGetResetCode) { c.r[3] = 0; }
HLE(OSGetProgressiveMode) { c.r[3] = sProgressiveMode; }
HLE(OSSetProgressiveMode) { sProgressiveMode = c.r[3]; }
HLE(OSGetSoundMode) { c.r[3] = sSoundMode; }
// Console language (SRAM: 0 English; the Wii's comes from SC).
HLE(OSGetLanguage) { c.r[3] = 0; }
HLE(OSSetSoundMode) { sSoundMode = c.r[3]; }

// --- EXI / serial (no devices attached; UART output goes to stdout) -----------------

HLE(EXIProbe) { c.r[3] = 0; }
HLE(EXIProbeEx) { c.r[3] = u32(-1); }
HLE(EXISelect) { c.r[3] = 0; }
HLE(EXIDeselect) { c.r[3] = 0; }
HLE(EXIImmEx) { c.r[3] = 0; }
HLE(EXILock) { c.r[3] = 0; }
HLE(EXIUnlock) { c.r[3] = 1; }
HLE(InitializeUART) { c.r[3] = 0; }
HLE(ReadUARTN) { c.r[3] = 0; }
HLE(WriteUARTN) {
    std::fwrite(hostp(c.r[3]), 1, c.r[4], stdout);
    std::fflush(stdout);
    c.r[3] = 0;
}

// --- debugger ---------------------------------------------------------------------------

HLE(DBInit) {}
HLE(DBPrintf) { std::fputs(gstr(c.r[3]), stdout); }

// --- PPC architecture helpers -----------------------------------------------------------

HLE(PPCMfmsr) { c.r[3] = c.msr; }
HLE(PPCMtmsr) { c.msr = c.r[3]; }
HLE(PPCMfhid2) { c.r[3] = c.mfspr(920); }
HLE(PPCMthid2) { c.mtspr(920, c.r[3]); }
HLE(PPCSync) {}
HLE(PPCHalt) { fatal("PPCHalt"); }

// A console button (ios.h stmEvent): the OS state-event handler's work. Reset calls the reset
// callback (once, like the OS, which then puts the default back); power calls the power callback.
void osStateEvent(u32 event) {
    const bool reset = event == ios::kStmEventReset;
    const u32 cb = reset ? sResetCallback : sPowerCallback;
    if (reset) sResetCallback = 0;
    else sPowerCallback = 0;
    if (!cb) {
        log("[os] %s button: no callback registered (nothing happens, as on the console)", reset ? "Reset" : "Power");
        return;
    }
    log("[os] %s button: calling the game's callback %08X", reset ? "Reset" : "Power", cb);
    sched::postIrq([cb](Ctx& ic) { guestCall(ic, cb); });
}

}  // namespace recomp
