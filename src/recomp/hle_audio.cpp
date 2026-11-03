// HLE: audio interface (AI) and the DSP task interface AX runs on.
//
// AI: the game hands finished 16-bit stereo blocks to AIInitDMA; a host thread
// plays them through SDL3 (audio_out.cpp) and raises the AI DMA interrupt each
// time a block starts, paced by the host device so the game's audio clock
// follows the sound card. Without a device the interrupt runs off a timer.
//
// DSP: AX (revolution/ax) runs its microcode as a DSP task and mails it one command
// list per 3 ms frame from the AI interrupt; the DSP task SDK is HLE'd below and the
// lists go to our microcode stand-in (audio_dsp.cpp). See docs/AUDIO.md.
#include "port/settings.h"
#include "recomp/audio.h"
#include "recomp/cpu_qos.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace recomp {
namespace {
std::atomic<u32> sDmaCallback{0};
u32 sStreamCallback = 0;
std::atomic<u32> sDmaAddr{0};
std::atomic<u32> sDmaLength{0};
std::atomic<bool> sDmaRunning{false};
std::atomic<u32> sDspRate{0};  // 0 = 32 kHz, 1 = 48 kHz
std::thread sDmaThread;

// The console's real rates (audio.h): the game's audio clock runs at these, so music
// tempo and pitch match the console (0.09% above the nominal 32000 Hz).
double rateHz() { return sDspRate.load() ? audio::kConsoleRate48 : audio::kConsoleRate32; }

// Set when the AI interrupt is raised, cleared once the DSP has mixed for it (the AX command
// list sent from the interrupt).
std::atomic<bool> sAwaitingMix{false};
std::atomic<bool> sCallbackRan{false};  // CF_AUDIO_LOG: the last AI interrupt's callback has returned

// JAudio's DSP overload check (UpdateDSPchannelAll) times its DSP updates: when a subframe
// takes nearly as long as the gap before the frame's first one, it decides the DSP cannot
// keep up and force-stops a voice, whatever its priority. On the console a frame starts at
// an AI interrupt, ~17 ms after the previous one, and is mixed long before the next. Here
// two interrupts could reach the game together (a host stall, or the device draining its
// queue in big chunks) and the second frame then started right after the first: every
// subframe looked "slow" and a voice was killed (sound effects cut short; the movie
// player's audio stream stopped, which ended the movie). So the next interrupt waits until
// the game has mixed for the previous one and the DSP frame it started is done, for at most
// `maxSeconds` (100 ms: the game may be stalled). Returns false when it gave up.
//
// The SMS port also bounds the wait by the audio still queued on the device (keeping 4 ms),
// so a slow host replays the AI block instead of running the device dry. Measured in LM it
// does more harm than good (docs/AUDIO.md, "AI output and pacing"): by the time the bound
// expires the queue is empty anyway, and the early interrupt starts a DSP frame on top of
// the unfinished one, so LM's overload check (which stops voices of any priority) cut 2-6
// voices each time. Kept as a test aid: CF_AI_QUEUE_BOUND=1.
bool waitForGuestMix(double maxSeconds) {
    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(maxSeconds));
    while ((sAwaitingMix.load() || audio::dspFrameBusy()) && clock::now() < deadline && !audio::outPaused())
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    const bool mixed = !sAwaitingMix.load();
    const auto quiet = std::min(audio::dspFrameEnd(), deadline);
    if (clock::now() < quiet) std::this_thread::sleep_until(quiet);
    return mixed;
}

// CF_AUDIO_LOG=1: AI pacing statistics every ~10 s (cumulative): interrupt spacing, how long
// the interrupt waited for the game's mix (and how often the wait gave up), and the device
// queue when each block was pushed ("dry" = the SDL stream had run empty: an audible gap).
struct PacingStats {
    using clock = std::chrono::steady_clock;
    u64 blocks = 0, dry = 0, low = 0, gaveUp = 0, close = 0;
    double minQueued = 1e9, sumQueued = 0, maxWait = 0, sumWait = 0, minGap = 1e9, maxGap = 0;
    clock::time_point last{}, report = clock::now();

    void block(double secs, double queued, double waited, bool timedOut) {
        if (!audio::g_log) return;
        const auto now = clock::now();
        if (blocks) {
            const double gap = std::chrono::duration<double>(now - last).count();
            minGap = std::min(minGap, gap);
            maxGap = std::max(maxGap, gap);
            if (gap < secs * 0.5) close++;  // two interrupts nearly back to back
        }
        last = now;
        blocks++;
        if (queued <= 0) dry++;
        if (queued < 0.004) low++;
        minQueued = std::min(minQueued, queued);
        sumQueued += queued;
        maxWait = std::max(maxWait, waited);
        sumWait += waited;
        if (timedOut) gaveUp++;
        if (now - report < std::chrono::seconds(10)) return;
        report = now;
        log("[audio] pacing: %llu blocks, gap %.1f-%.1f ms (%llu < half a block), mix wait avg %.2f max %.1f ms "
            "(%llu gave up), queue avg %.1f min %.1f ms, %llu below 4 ms, %llu dry",
            (unsigned long long)blocks, minGap * 1e3, maxGap * 1e3, (unsigned long long)close, sumWait / blocks * 1e3,
            maxWait * 1e3, (unsigned long long)gaveUp, sumQueued / blocks * 1e3, minQueued * 1e3,
            (unsigned long long)low, (unsigned long long)dry);
    }
} sPacing;

// Device underruns (issue #25, docs/AUDIO.md "Underruns"). When the game's mix comes late (the
// guest CPU held by another guest thread, a host stall) the device queue runs dry and the
// device plays silence: a click, or a crackle when it repeats. Each underrun raises the queue
// target by one block, up to kMaxExtraBlocks (about 100 ms more latency), so a scene that
// stalls now and then stops crackling after the first click; after kRelaxSeconds without one the
// target steps back down. Always on; every minute with an underrun gives one line in the log
// (last_run.log), so user reports show it.
struct Underruns {
    using clock = std::chrono::steady_clock;
    static constexpr int kMaxExtraBlocks = 6;
    static constexpr double kRelaxSeconds = 30;
    int extraBlocks = 0;
    u64 pushes = 0;
    u32 count = 0, total = 0;  // since the last log line, since start
    double maxWait = 0, maxGap = 0;
    clock::time_point lastPush{}, lastDry = clock::now(), lastLine = clock::now();

    static int forcedExtra() {
        static const int v = [] {
            const char* e = std::getenv("CF_AUDIO_EXTRA_BLOCKS");  // test aid: fixed extra blocks
            return e && *e ? std::atoi(e) : -1;
        }();
        return v;
    }
    double extraSeconds(double block) const {
        const int f = forcedExtra();
        return (f >= 0 ? f : extraBlocks) * block;
    }
    void reset() {  // a new start (host pause, no DMA): the empty queue is not an underrun
        pushes = 0;
        lastPush = {};
    }
    // Called just before a block is pushed, with the device queue at that moment.
    void push(double queued, double block, double waited) {
        const auto now = clock::now();
        const double gap = pushes ? std::chrono::duration<double>(now - lastPush).count() : 0;
        lastPush = now;
        if (++pushes > 20 && queued <= 0.0005) {  // the first blocks fill an empty queue
            count++;
            total++;
            maxWait = std::max(maxWait, waited);
            maxGap = std::max(maxGap, gap);
            lastDry = now;
            if (extraBlocks < kMaxExtraBlocks) extraBlocks++;
            if (total == 1)
                log("[audio] device queue ran dry (waited %.1f ms for the game's mix, %.1f ms since the last block); "
                    "queue target now %.0f ms", waited * 1e3, gap * 1e3, (std::max(3 * block, 0.040) + extraSeconds(block)) * 1e3);
        } else if (extraBlocks > 0 && now - lastDry > std::chrono::duration<double>(kRelaxSeconds)) {
            extraBlocks--;
            lastDry = now;
        }
        if (now - lastLine >= std::chrono::seconds(60)) {
            if (count)
                log("[audio] last 60 s: device queue ran dry %u times (%u since start; longest wait for the game's mix "
                    "%.1f ms, longest gap between blocks %.1f ms); queue target %.0f ms",
                    count, total, maxWait * 1e3, maxGap * 1e3, (std::max(3 * block, 0.040) + extraSeconds(block)) * 1e3);
            count = 0;
            maxWait = maxGap = 0;
            lastLine = now;
        }
    }
} sUnderruns;

void dmaThread() {
    cpu::setCurrent(cpu::Role::Audio);
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    double smoothed = -1;
    for (;;) {
        if (audio::outPaused()) {  // game paused by the host: no audio interrupts
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            next = clock::now();
            sUnderruns.reset();
            continue;
        }
        const u32 len = sDmaLength.load();
        const double secs = len ? (len / 4.0) / rateHz() : 0.005;  // 16-bit stereo frames
        if (audio::outHasDevice()) {
            // Keep about three blocks queued on the device so its clock paces the game, with
            // the interrupts evenly spaced like the console's AI (see waitForGuestMix): one
            // block length apart, stretched or shortened by at most 5% from the smoothed queue
            // level to follow the device clock. (Pushing whenever the queue dropped below the
            // target raised two interrupts back to back now and then: the device drains its
            // queue in whole periods of ~10 ms.)
            const double target = std::max(3 * secs, 0.040) + sUnderruns.extraSeconds(secs);  // + after underruns
            const double queued = audio::outQueuedSeconds();
            smoothed = smoothed < 0 ? queued : smoothed + (queued - smoothed) * 0.05;
            double scale = std::clamp(1.0 + (smoothed - target) * 2.0, 0.95, 1.05);
            if (queued < secs) scale = 0.5;  // about to run dry (start, or after a stall): refill quickly
            const auto now = clock::now();
            next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(secs * scale));
            if (next < now) next = now;  // late (host stall): never catch up with interrupts back to back
            std::this_thread::sleep_until(next);
        } else {
            next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(secs));
            if (clock::now() > next + std::chrono::milliseconds(100)) next = clock::now();
            std::this_thread::sleep_until(next);
        }
        const u32 cb = sDmaCallback.load();
        if (!sDmaRunning.load()) {
            if (audio::outHasDevice()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            sUnderruns.reset();
            continue;
        }
        const auto waitStart = clock::now();
        bool mixed = true;
        if (cb) {
            static const bool sQueueBound = [] {
                const char* e = std::getenv("CF_AI_QUEUE_BOUND");
                return e && *e == '1';
            }();
            const double maxWait = sQueueBound && audio::outHasDevice()
                                       ? std::clamp(audio::outQueuedSeconds() - 0.004, 0.0, 0.1)
                                       : 0.1;
            mixed = waitForGuestMix(maxWait);
            if (!mixed && audio::g_log)
                log("[audio] AI interrupt without the game's mix (waited %.1f ms, %.1f ms queued; callback %s, DSP "
                    "frame %s)",
                    maxWait * 1e3, audio::outQueuedSeconds() * 1e3, sCallbackRan.load() ? "ran" : "not run yet",
                    audio::dspFrameBusy() ? "in flight" : "done");
        }
        if (audio::outHasDevice()) {
            const double queued = audio::outQueuedSeconds();
            const double waited = std::chrono::duration<double>(clock::now() - waitStart).count();
            sUnderruns.push(queued, secs, waited);
            if (audio::g_log) sPacing.block(secs, queued, waited, !mixed);
        }
        // The block programmed by the last AIInitDMA starts playing now (the AI repeats
        // the previous block if the game did not supply a new one in time).
        if (const u32 addr = sDmaAddr.load(); addr && len) audio::outPushGuestBlock(addr, len);
        if (cb) {
            sAwaitingMix = true;
            sCallbackRan = false;
            sched::postIrq([cb](Ctx& ic) {
                const auto t0 = clock::now();
                guestCall(ic, cb);
                sCallbackRan = true;
                if (audio::g_log) {  // a slow AI callback holds every interrupt (with CF_TRACE=ring: its recent functions)
                    const double ms = std::chrono::duration<double>(clock::now() - t0).count() * 1e3;
                    if (ms > 20) {
                        std::string s;
                        for (u32 i = ic.ringPos > 24 ? ic.ringPos - 24 : 0; i < ic.ringPos; i++) {
                            char b[12];
                            std::snprintf(b, sizeof b, " %08X", ic.ring[i & 31]);
                            s += b;
                        }
                        log("[audio] AI callback took %.1f ms; recent%s", ms, s.c_str());
                    }
                }
            });
        }
    }
}
}  // namespace

// The game has mixed the DSP output into the next AI block for the last AI interrupt
// (Jac_imixcopy; shared with the SMS port, whose mix hook is game code).
void audio::noteGuestMixed() { sAwaitingMix = false; }

HLE(AIInit) {}
HLE(AICheckInit) { c.r[3] = 1; }
HLE(AIGetDMABytesLeft) { c.r[3] = 0; }  // the block in flight counts as played
HLE(AIInitDMA) {
    sDmaAddr = c.r[3];
    sDmaLength = c.r[4];
}
HLE(AIStartDMA) {
    sDmaRunning = true;
    audio::outStart(rateHz());
    static bool sStarted = false;  // (a detached thread is never joinable: test a flag)
    if (!sStarted) {
        sStarted = true;
        sDmaThread = std::thread(dmaThread);
        sDmaThread.detach();
    }
}
HLE(AIStopDMA) { sDmaRunning = false; }
HLE(AIGetDMAStartAddr) { c.r[3] = sDmaAddr; }
HLE(AISetDSPSampleRate) {
    sDspRate = c.r[3];
    audio::outSetRate(rateHz());
}
HLE(AIGetDSPSampleRate) { c.r[3] = sDspRate; }
HLE(AIRegisterDMACallback) {
    const u32 old = sDmaCallback;
    sDmaCallback = c.r[3];
    c.r[3] = old;
}
HLE(AIRegisterStreamCallback) {
    const u32 old = sStreamCallback;
    sStreamCallback = c.r[3];
    c.r[3] = old;
}
// DVD audio streaming (DTK). JAudio's StreamMain (jaudio/streamctrl.c) supports it,
// but the disc has no DTK tracks: LM's streamed music is AFC played through the DSP.
HLE(AISetStreamSampleRate) {}
HLE(AISetStreamVolLeft) {}
HLE(AISetStreamVolRight) {}
HLE(AIResetStreamSampleCount) {}
HLE(AISetStreamPlayState) {}
HLE(AISetStreamTrigger) {}
HLE(AIGetStreamSampleCount) { c.r[3] = 0; }
HLE(AIGetStreamTrigger) { c.r[3] = 0; }

// DSP task interface (dsp/dsp.c, dsp_task.c), as the RVL SDK's AX uses it. The DSP itself is
// our AX microcode stand-in (audio_dsp.cpp): a task added with DSPAddTask gets its init
// callback at once (the microcode booted and mailed 0xDCD10000), and each command list the CPU
// hands over (__AXOutNewFrame: mail 0xBABE<length>, then the list's address) is mixed right
// there, writing the AI buffer, the aux uploads and the PBs back as the microcode does. The
// list's END command yields the task (0xDCD10002); the CPU's resume callback
// (__AXDSPResumeCallback) follows a little later, as on the console, where the DSP needs part
// of the 3 ms frame. (Mixing at the mail is safe: AX synced the PBs before sending, and the
// buffers the CPU touches until the next frame are not the ones this list writes.)
// DSPTaskInfo: +0x00 state, +0x28 init_cb, +0x2C res_cb, +0x30 done_cb, +0x34 req_cb.
namespace {
std::atomic<u32> sDspTask{0};
u32 sMailHi = 0;
bool sDspInit = false;
// How long the DSP "takes" for a command list before it yields (CF_AX_DSP_US, microseconds).
double dspListSeconds() {
    static const double v = [] {
        const char* e = std::getenv("CF_AX_DSP_US");
        return (e && *e ? std::atof(e) : 400.0) * 1e-6;
    }();
    return v;
}
void callTaskCb(u32 task, u32 offset, double delay, bool frameDone = false) {
    if (!task) return;
    const u32 cb = rd32(task + offset);
    if (!cb) {
        if (frameDone) audio::axFrameDone();
        return;
    }
    sched::postIrqAfter(delay, [cb, task, frameDone](Ctx& ic) {
        if (frameDone) audio::axFrameDone();
        guestCall(ic, cb, task);
    });
}
}  // namespace
HLE(DSPInit) { sDspInit = true; }
HLE(DSPCheckInit) { c.r[3] = sDspInit ? 1 : 0; }
HLE(DSPAddTask) {
    const u32 task = c.r[3];
    sDspTask = task;
    wr32(task, 1);  // DSP_TASK_STATE_INIT: running
    callTaskCb(task, 0x28, 0.0005);
    c.r[3] = task;
}
HLE(DSPAssertTask) { c.r[3] = c.r[3]; }
HLE(DSPCheckMailToDSP) { c.r[3] = 0; }      // the "DSP" took every mail
HLE(DSPCheckMailFromDSP) { c.r[3] = 0; }
HLE(DSPReadMailFromDSP) { c.r[3] = 0; }
HLE(DSPSendMailToDSP) {
    const u32 mail = c.r[3];
    static int sLogged = 0;
    if (sLogged < 4 && audio::g_log) {
        sLogged++;
        log("[dsp] mail %08X (lr %08X)", mail, c.lr);
    }
    if ((mail >> 16) == 0xBABE) {
        sMailHi = mail;
        return;
    }
    if (sMailHi) {  // second word of a command list
        const u32 len = sMailHi & 0xFFFF;
        sMailHi = 0;
        audio::axSetSdaBase(c.r[13]);
        audio::axCommandList(mail, len);
        audio::noteGuestMixed();  // the list's OUTPUT command filled the next AI block
        callTaskCb(sDspTask, 0x2C, dspListSeconds(), true);  // res_cb: the task yielded after its list
    }
}
HLE(DSPAssertInt) {}
HLE(DSPGetDMAStatus) { c.r[3] = 0; }

// ---- NW4R snd / AX hooks (docs/AUDIO.md) ----

// nw4r::snd::SoundArchivePlayer::SetupSoundPlayer(archive, &buf, end): places one SoundPlayer
// per player of the sound archive at SoundArchivePlayer +0x3C (count at +0x38), 0x64 bytes
// each (the stride its loop uses). The volume sliders map a sound to its player by this array.
HLE(SetupSoundPlayer__Q34nw4r3snd18SoundArchivePlayerFPCQ34nw4r3snd12SoundArchivePPvPv) {
    const u32 self = c.r[3];
    callOriginal(c, "SetupSoundPlayer__Q34nw4r3snd18SoundArchivePlayerFPCQ34nw4r3snd12SoundArchivePPvPv");
    if (c.r[3]) audio::noteSoundPlayers(rd32(self + 0x3C), rd32(self + 0x38), 0x64);
}

// The console's sound setting (SCGetSoundMode: 0 mono, 1 stereo, 2 surround), read by NW4R's
// InitSoundSystem, which picks the NW4R output mode mono / stereo / DPL2 from it. City Folk
// (fn_801CE6B0, right after InitSoundSystem) then sets DPL2 back to stereo: on a console the
// game always mixes in stereo. With F1 > Sound > Output 5.1 / 7.1 (discrete) hooks.txt patch 30
// skips that call, so the game keeps mixing in DPL2 and the surround buses exist for the extra
// speakers (docs/AUDIO.md "Surround output"); every other choice leaves the game's behaviour.
// The switch is set here, before the game's call (patch 30 is tagged "all": every version).
HLE(SCGetSoundMode) {
    callOriginal(c, "SCGetSoundMode");
    const auto out = port::settings().audio.output;
    const bool discrete = out == port::AudioOutput::Surround51 || out == port::AudioOutput::Surround71;
    setPatch(30, discrete && c.r[3] == 2);
    if (audio::g_log || discrete)
        log("[ax] SCGetSoundMode -> %u%s", c.r[3],
            discrete && c.r[3] == 2 ? " (discrete surround: the game keeps Dolby Pro Logic II mixing)" : "");
}

// CF_AUDIO_LOG: the AX output mode and the aux effects (AXFX reverb / chorus / delay callbacks).
HLE(AXSetMode) {
    if (audio::g_log) log("[ax] AXSetMode(%u) (0 stereo, 1 surround, 2 DPL2), lr %08X", c.r[3], c.lr);
    callOriginal(c, "AXSetMode");
}
std::string guestSymbolize(u32 addr);  // runtime.cpp
namespace {
void logAux(char bus, const Ctx& c) {
    if (audio::g_log) log("[ax] aux %c callback %s (context %08X)", bus, c.r[3] ? guestSymbolize(c.r[3]).c_str() : "none", c.r[4]);
}
}  // namespace
HLE(AXRegisterAuxACallback) {
    logAux('A', c);
    callOriginal(c, "AXRegisterAuxACallback");
}
HLE(AXRegisterAuxBCallback) {
    logAux('B', c);
    callOriginal(c, "AXRegisterAuxBCallback");
}
HLE(AXRegisterAuxCCallback) {
    logAux('C', c);
    callOriginal(c, "AXRegisterAuxCCallback");
}

}  // namespace recomp
