// HLE: audio interface (AI) and the JAudio DSP driver.
//
// AI: the game hands finished 16-bit stereo blocks to AIInitDMA; a host thread
// plays them through SDL3 (audio_out.cpp) and raises the AI DMA interrupt each
// time a block starts, paced by the host device so the game's audio clock
// follows the sound card. Without a device the interrupt runs off a timer.
//
// DSP: JAudio's DSP driver (jaudio/dspboot.c, dspproc.c) talks to its microcode
// through a mailbox. Its entry points are hooked (tools/recomp/hooks.txt):
// command packets go to our microcode stand-in (audio_dsp.cpp), and every
// DSPReleaseHalt mixes one subframe and then raises the DSP interrupt that
// JAudio's audio thread waits for. See docs/AUDIO.md.
#include "port/settings.h"
#include "recomp/audio.h"
#include "recomp/cpu_qos.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
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

// Set when the AI interrupt is raised, cleared once JAudio has mixed for it (Jac_imixcopy).
std::atomic<bool> sAwaitingMix{false};

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
    const auto quiet = std::min(audio::dspFrameEnd() + std::chrono::milliseconds(2), deadline);
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
                log("[audio] AI interrupt without the game's mix (waited %.1f ms, %.1f ms queued)", maxWait * 1e3,
                    audio::outQueuedSeconds() * 1e3);
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
            sched::postIrq([cb](Ctx& ic) { guestCall(ic, cb); });
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

// DSP task interface (dsp/dsp.c, dsp_task.c), as the RVL SDK's AX uses it. Boot stand-in until
// the AX microcode stand-in exists (docs/WII.md "Audio"): a task added with DSPAddTask gets its
// init callback at once (the "DSP" is ready), and every command list the CPU hands over (the
// mail pair 0xBABExxxx + address that AX sends per audio frame) is "finished" a moment later
// through the task's resume callback, so AX's frame loop and the callbacks NW4R's sound system
// runs on keep going. No samples are mixed (silence).
// DSPTaskInfo: +0x00 state, +0x28 init_cb, +0x2C res_cb, +0x30 done_cb, +0x34 req_cb.
namespace {
std::atomic<u32> sDspTask{0};
u32 sMailHi = 0;
bool sDspInit = false;
void callTaskCb(u32 task, u32 offset) {
    if (!task) return;
    const u32 cb = rd32(task + offset);
    if (cb) sched::postIrqAfter(0.0005, [cb, task](Ctx& ic) { guestCall(ic, cb, task); });
}
}  // namespace
HLE(DSPInit) { sDspInit = true; }
HLE(DSPCheckInit) { c.r[3] = sDspInit ? 1 : 0; }
HLE(DSPAddTask) {
    const u32 task = c.r[3];
    sDspTask = task;
    wr32(task, 1);  // DSP_TASK_STATE_INIT: running
    callTaskCb(task, 0x28);
    c.r[3] = task;
}
HLE(DSPAssertTask) { c.r[3] = c.r[3]; }
HLE(DSPCheckMailToDSP) { c.r[3] = 0; }      // the "DSP" took every mail
HLE(DSPCheckMailFromDSP) { c.r[3] = 0; }
HLE(DSPReadMailFromDSP) { c.r[3] = 0; }
HLE(DSPSendMailToDSP) {
    const u32 mail = c.r[3];
    static int sLogged = 0;
    if (sLogged < 4 && std::getenv("CF_AUDIO_LOG")) {
        sLogged++;
        log("[dsp] mail %08X (lr %08X)", mail, c.lr);
    }
    if ((mail >> 16) == 0xBABE) {
        sMailHi = mail;
        return;
    }
    if (sMailHi) {  // second word of a command list: finished at once
        sMailHi = 0;
        callTaskCb(sDspTask, 0x2C);  // res_cb: the task resumes after its command list
    }
}
HLE(DSPAssertInt) {}
HLE(DSPGetDMAStatus) { c.r[3] = 0; }

// JAudio voice classification and the game's sound mode come from LM's game hooks (Luigi's
// Mansion: the second half of hle_audio.cpp there). City Folk mixes with AX (docs/WII.md
// "Audio"): no JAudio voices.
int audio::gameSoundMode() { return 1; }
int audio::voiceBank(u32) { return -1; }
u32 audio::voiceSequence(u32) { return 0; }
void audio::noteSequenceData(u32, u32) {}
audio::VoiceKind audio::voiceKind(u32) { return audio::VoiceKind::Unknown; }

}  // namespace recomp
