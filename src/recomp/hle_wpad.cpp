// HLE: Wii Remote at the WPAD/KPAD level (docs/WII.md "Input: Wii Remote", docs/CONTROLS.md).
//
// The real stack is KPAD (pointer/acc processing) on WPAD (Remote protocol) on WUD/BTE
// (Bluetooth, through IOS to the USB Bluetooth module). None of the lower layers runs: the
// public WPAD/KPAD calls the game makes are answered here from each port's Remote state
// (wiimote.cpp: buttons, pointer, motion, Nunchuk). Written from the RVL SDK's public
// interface (structure layouts as the game's own code reads them) and the game's use:
//   - EGG::CoreControllerMgr reads every Remote once per frame: KPADRead(chan, buf, 16) then
//     WPADProbe; it keeps buf[0] (the newest sample), derives its own trigger/release bits and
//     reads pointer pos / horizon / dpd_valid_fg / acc / Nunchuk stick through its getters;
//   - the game checks WPADProbe's device type (no controller / Nunchuk / unsupported
//     extension), battery through WPADGetInfoAsync (warns at level <= 1), rumble through
//     WPADControlMotor, and disconnects Remotes of other players (WPADDisconnect);
//   - the HOME Menu (HBM) adds speaker, volume, motor-enable and simple-sync calls.
// Timing as on hardware: the Remote reports at 200 Hz, so KPADRead returns the samples since
// the previous read (newest first; 3-4 per 60 Hz frame); callbacks (connection, extension,
// info, speaker/DPD/LED control) arrive later from the interrupt side, never inside the call.
// Remotes connect about a second after WPADInit, as paired Remotes do after the system starts.
// CF_WPAD_LOG=1 logs state changes and every callback.
#include "recomp/audio.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/wiimote.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace recomp {
namespace {

using wiimote::State;
using wiimote::kRemotes;

constexpr s32 kErrNone = 0, kErrNoController = -1, kErrBusy = -2;
constexpr u32 kStateSetup = 3;          // WPAD_STATE_SETUP: the library is up
constexpr double kReportHz = 200.0;     // Remote report rate
constexpr double kConnectDelay = 1.0;   // WPADInit -> paired Remotes connected (s)
constexpr double kExtDelay = 0.15;      // connection -> extension identified (s)
constexpr double kReplyDelay = 0.012;   // a Bluetooth round trip (info, speaker, DPD, LED)

// WPADDataFormat
constexpr u32 kFmtCoreAccDpd = 2, kFmtNunchukAccDpd = 5;

const bool sLog = [] {
    const char* v = std::getenv("CF_WPAD_LOG");
    return v && *v && *v != '0';
}();

struct Chan {
    // What the game has been told (connection and extension callbacks, WPADProbe).
    bool connected = false;
    u8 extReported = wiimote::kDevCore;
    bool extPending = false;
    bool disconnected = false;   // WPADDisconnect: off until one of its buttons is pressed
    u32 connectCb = 0, extensionCb = 0, samplingCb = 0;
    u32 dataFormat = kFmtCoreAccDpd;
    bool dpd = true;             // KPAD turns the IR camera on at connection
    bool speaker = false, speakerMuted = false;
    u32 led = 0;
    // KPAD
    u64 lastRead = 0;            // timeBase of the last sample handed out
    bool haveRead = false;
    double sampleDebt = 0;       // fractional samples carried to the next read
    State prev;                  // state at the previous read
    u32 prevHold = 0;
    float prevSpeedAcc = 0;
    bool motor = false;
    u64 streamBytes = 0;
};
Chan sChan[kRemotes];
bool sInit = false, sMotorEnabled = true, sAcceptConnection = true;
u32 sSimpleSyncCb = 0;
u8 sSpeakerVolume = 0x40;
std::recursive_mutex sMutex;

void callLater(double seconds, u32 cb, u32 a0, u32 a1, const char* what) {
    if (!cb) return;
    if (sLog) log("[wpad] %s callback %08X(%d, %d) in %.0f ms", what, cb, s32(a0), s32(a1), seconds * 1000);
    sched::postIrqAfter(seconds, [cb, a0, a1](Ctx& c) { guestCall(c, cb, a0, a1); });
}

// The state of a Remote as the game sees it now, with connection / extension changes turned
// into the game's callbacks.
State current(u32 chan) {
    State s;
    wiimote::sample(int(chan), s);
    std::lock_guard lk(sMutex);
    Chan& ch = sChan[chan];
    if (!sInit) s.connected = false;
    if (ch.disconnected) {
        // A Remote the game disconnected comes back when a button on it is pressed (accepting
        // connections, as the console does).
        if (s.connected && (s.buttons & wiimote::kBtnGameMask) && sAcceptConnection) {
            ch.disconnected = false;
            if (sLog) log("[wpad] remote %u reconnects (button pressed)", chan + 1);
        } else {
            s.connected = false;
        }
    }
    if (s.connected != ch.connected) {
        ch.connected = s.connected;
        log("[wpad] remote %u %s", chan + 1, s.connected ? "connected" : "disconnected");
        callLater(0.0, ch.connectCb, chan, u32(s.connected ? kErrNone : kErrNoController), "connect");
        ch.extReported = wiimote::kDevCore;
        ch.extPending = s.connected && s.ext != wiimote::kDevCore;
        ch.haveRead = false;
        if (s.connected) ch.dpd = true;
        if (ch.extPending) {
            const u8 ext = s.ext;
            sched::postIrqAfter(kExtDelay, [chan, ext](Ctx& c) {
                u32 cb = 0;
                {
                    std::lock_guard lk2(sMutex);
                    Chan& ch2 = sChan[chan];
                    if (!ch2.connected || !ch2.extPending) return;
                    ch2.extPending = false;
                    ch2.extReported = ext;
                    cb = ch2.extensionCb;
                }
                if (sLog) log("[wpad] remote %u extension %u identified", chan + 1, ext);
                if (cb) guestCall(c, cb, chan, ext);
            });
        }
    } else if (s.connected && !ch.extPending && s.ext != ch.extReported) {  // plugged in / pulled out
        ch.extReported = s.ext;
        log("[wpad] remote %u extension now %u", chan + 1, s.ext);
        callLater(kExtDelay, ch.extensionCb, chan, s.ext, "extension");
    }
    if (s.connected && ch.extPending) {  // the Remote reports without its extension until it is identified
        s.ext = wiimote::kDevCore;
        s.buttons &= ~(wiimote::kBtnC | wiimote::kBtnZ);
    }
    if (s.connected && !ch.dpd) s.pointerValid = false;
    return s;
}

// ---- KPADStatus (0x84 bytes, RVL SDK kpad.h) --------------------------------------------
//  +00 hold  +04 trig  +08 release
//  +0C acc (Vec)  +18 acc_value  +1C acc_speed
//  +20 pos (Vec2)  +28 vec  +30 speed
//  +34 horizon (Vec2: cos, sin of the roll)  +3C hori_vec  +44 hori_speed
//  +48 dist  +4C dist_vec  +50 dist_speed
//  +54 acc_vertical (Vec2)
//  +5C dev_type u8  +5D wpad_err s8  +5E dpd_valid_fg s8  +5F data_format u8
//  +60 extension: Nunchuk = stick (Vec2), acc (Vec) +68, acc_value +74, acc_speed +78
void wrVec2(u32 a, float x, float y) {
    wrf32(a, x);
    wrf32(a + 4, y);
}
void wrVec3(u32 a, const wiimote::Vec3& v) {
    wrf32(a, v.x);
    wrf32(a + 4, v.y);
    wrf32(a + 8, v.z);
}
float len3(const wiimote::Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
wiimote::Vec3 lerp3(const wiimote::Vec3& a, const wiimote::Vec3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

void writeStatus(u32 buf, const State& s, const State& prevSample, u32 hold, u32 trig, u32 release, u32 fmt) {
    for (u32 i = 0; i < 0x84; i += 4) wr32(buf + i, 0);
    wr32(buf + 0x00, hold);
    wr32(buf + 0x04, trig);
    wr32(buf + 0x08, release);
    wrVec3(buf + 0x0C, s.acc);
    wrf32(buf + 0x18, len3(s.acc));
    wrf32(buf + 0x1C, len3({s.acc.x - prevSample.acc.x, s.acc.y - prevSample.acc.y, s.acc.z - prevSample.acc.z}));
    wrVec2(buf + 0x20, s.px, s.py);
    const float vx = s.px - prevSample.px, vy = s.py - prevSample.py;
    wrVec2(buf + 0x28, vx, vy);
    wrf32(buf + 0x30, std::sqrt(vx * vx + vy * vy));
    const float hx = std::cos(s.roll), hy = std::sin(s.roll);
    const float phx = std::cos(prevSample.roll), phy = std::sin(prevSample.roll);
    wrVec2(buf + 0x34, hx, hy);
    wrVec2(buf + 0x3C, hx - phx, hy - phy);
    wrf32(buf + 0x44, std::sqrt((hx - phx) * (hx - phx) + (hy - phy) * (hy - phy)));
    wrf32(buf + 0x48, s.dist);
    wrf32(buf + 0x4C, s.dist - prevSample.dist);
    wrf32(buf + 0x50, std::fabs(s.dist - prevSample.dist));
    // Vertical direction from gravity in the y/z plane: (1, 0) with the Remote level.
    const float vl = std::sqrt(s.acc.y * s.acc.y + s.acc.z * s.acc.z);
    wrVec2(buf + 0x54, vl > 0.01f ? -s.acc.y / vl : 1.0f, vl > 0.01f ? s.acc.z / vl : 0.0f);
    wr8(buf + 0x5C, s.ext);
    wr8(buf + 0x5D, u8(kErrNone));
    wr8(buf + 0x5E, u8(s.pointerValid ? 2 : 0));  // both sensor bar dots in view
    wr8(buf + 0x5F, u8(fmt));
    if (s.ext == wiimote::kDevNunchuk) {
        wrVec2(buf + 0x60, s.stickX, s.stickY);
        wrVec3(buf + 0x68, s.nunAcc);
        wrf32(buf + 0x74, len3(s.nunAcc));
        wrf32(buf + 0x78, len3({s.nunAcc.x - prevSample.nunAcc.x, s.nunAcc.y - prevSample.nunAcc.y,
                                s.nunAcc.z - prevSample.nunAcc.z}));
    }
}

}  // namespace

bool wpadRadioActive() {
    std::lock_guard lk(sMutex);
    for (const auto& ch : sChan)
        if (ch.connected) return true;
    return false;
}

// ---- WPAD: library, connection --------------------------------------------------------

// The Bluetooth stack (WUD/BTE) never starts (it would wait for the USB Bluetooth module):
// WPADInit just brings the library up; the paired Remotes "connect" a moment later.
HLE(WPADInit) {
    std::lock_guard lk(sMutex);
    if (sInit) return;
    log("[wpad] WPADInit: Wii Remotes connect in %.1f s", kConnectDelay);
    // The connections themselves (callbacks) are reported at the game's next WPADProbe/KPADRead.
    sched::postIrqAfter(kConnectDelay, [](Ctx&) {
        std::lock_guard lk2(sMutex);
        sInit = true;
    });
}
HLE(KPADInit) {
    // KPAD's own setup (sampling callbacks, DPD/motor defaults) is folded into this HLE.
    for (auto& ch : sChan) ch.haveRead = false;
    const HleFn init = hleLookup("WPADInit");
    if (init) init(c);
}
HLE(WPADShutdown) {}
HLE(WPADGetStatus) {
    std::lock_guard lk(sMutex);
    c.r[3] = sInit ? kStateSetup : 1;  // WPAD_STATE_ENABLING until the Remotes are up
}
HLE(WPADRegisterAllocator) {}
HLE(WPADSetAcceptConnection) {
    std::lock_guard lk(sMutex);
    sAcceptConnection = c.r[3] != 0;
}
HLE(WPADGetAcceptConnection) {
    std::lock_guard lk(sMutex);
    c.r[3] = sAcceptConnection ? 1 : 0;
}
HLE(WPADStartFastSimpleSync) { c.r[3] = 1; }
HLE(WPADStopSimpleSync) { c.r[3] = 1; }
HLE(WPADSetSimpleSyncCallback) {
    std::lock_guard lk(sMutex);
    const u32 old = sSimpleSyncCb;
    sSimpleSyncCb = c.r[3];
    c.r[3] = old;
}

// WPADProbe(chan, u32* devType) -> WPAD_ERR_*
HLE(WPADProbe) {
    const u32 chan = c.r[3], out = c.r[4];
    if (chan >= kRemotes) {
        if (out) wr32(out, wiimote::kDevNotFound);
        c.r[3] = u32(kErrNoController);
        return;
    }
    const State s = current(chan);
    if (out) wr32(out, s.connected ? s.ext : wiimote::kDevNotFound);
    c.r[3] = u32(s.connected ? kErrNone : kErrNoController);
}

// WPADSetConnectCallback(chan, cb(chan, result)) -> previous callback
HLE(WPADSetConnectCallback) {
    const u32 chan = c.r[3];
    if (chan >= kRemotes) {
        c.r[3] = 0;
        return;
    }
    std::lock_guard lk(sMutex);
    const u32 old = sChan[chan].connectCb;
    sChan[chan].connectCb = c.r[4];
    c.r[3] = old;
}
// WPADSetExtensionCallback(chan, cb(chan, devType)) -> previous callback
HLE(WPADSetExtensionCallback) {
    const u32 chan = c.r[3];
    if (chan >= kRemotes) {
        c.r[3] = 0;
        return;
    }
    std::lock_guard lk(sMutex);
    const u32 old = sChan[chan].extensionCb;
    sChan[chan].extensionCb = c.r[4];
    c.r[3] = old;
}
HLE(WPADSetSamplingCallback) {
    const u32 chan = c.r[3];
    if (chan >= kRemotes) {
        c.r[3] = 0;
        return;
    }
    std::lock_guard lk(sMutex);
    const u32 old = sChan[chan].samplingCb;
    sChan[chan].samplingCb = c.r[4];
    c.r[3] = old;
}
HLE(WPADSetAutoSamplingBuf) {}

HLE(WPADDisconnect) {
    const u32 chan = c.r[3];
    if (chan >= kRemotes) return;
    std::lock_guard lk(sMutex);
    Chan& ch = sChan[chan];
    if (ch.disconnected || !ch.connected) return;
    ch.disconnected = true;
    if (sLog) log("[wpad] WPADDisconnect(%u)", chan);
    current(chan);  // reports the disconnection (callback)
}

HLE(WPADGetDataFormat) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    c.r[3] = chan < kRemotes ? sChan[chan].dataFormat : 0;
}
HLE(WPADSetDataFormat) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    if (chan < kRemotes) sChan[chan].dataFormat = c.r[4];
    c.r[3] = u32(chan < kRemotes && sChan[chan].connected ? kErrNone : kErrNoController);
}

// ---- WPAD: status, motor, LED, DPD --------------------------------------------------------

// WPADInfo: dpd, speaker, attach, lowBat, nearempty (BOOLs), battery u8 (0-4), led, protocol, firmware.
// WPADGetInfoAsync(chan, WPADInfo*, cb(chan, result)) -> WPAD_ERR_*
HLE(WPADGetInfoAsync) {
    const u32 chan = c.r[3], info = c.r[4], cb = c.r[5];
    if (chan >= kRemotes) {
        c.r[3] = u32(kErrNoController);
        return;
    }
    const State s = current(chan);
    if (!s.connected) {
        c.r[3] = u32(kErrNoController);
        return;
    }
    std::lock_guard lk(sMutex);
    const Chan& ch = sChan[chan];
    if (info) {
        wr32(info + 0x00, ch.dpd ? 1 : 0);
        wr32(info + 0x04, ch.speaker ? 1 : 0);
        wr32(info + 0x08, s.ext != wiimote::kDevCore ? 1 : 0);
        wr32(info + 0x0C, s.battery <= 1 ? 1 : 0);
        wr32(info + 0x10, s.battery == 0 ? 1 : 0);
        wr8(info + 0x14, s.battery);
        wr8(info + 0x15, u8(ch.led));
        wr8(info + 0x16, 0);
        wr8(info + 0x17, 0);
    }
    callLater(kReplyDelay, cb, chan, u32(kErrNone), "info");
    c.r[3] = u32(kErrNone);
}

// WPADControlMotor(chan, cmd): 0 stop, 1 rumble.
HLE(WPADControlMotor) {
    const u32 chan = c.r[3], cmd = c.r[4];
    if (chan >= kRemotes) return;
    bool on;
    {
        std::lock_guard lk(sMutex);
        on = cmd == 1 && sMotorEnabled && sChan[chan].connected;
        if (on == sChan[chan].motor) return;
        sChan[chan].motor = on;
    }
    if (sLog) log("[wpad] remote %u motor %s", chan + 1, on ? "on" : "off");
    wiimote::rumble(int(chan), on);
}
HLE(WPADEnableMotor) {
    std::lock_guard lk(sMutex);
    sMotorEnabled = c.r[3] != 0;
}
HLE(WPADIsMotorEnabled) {
    std::lock_guard lk(sMutex);
    c.r[3] = sMotorEnabled ? 1 : 0;
}
// WPADControlLed(chan, flags, cb) -> WPAD_ERR_*
HLE(WPADControlLed) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    if (chan >= kRemotes || !sChan[chan].connected) {
        c.r[3] = u32(kErrNoController);
        return;
    }
    sChan[chan].led = c.r[4];
    callLater(kReplyDelay, c.r[5], chan, u32(kErrNone), "LED");
    c.r[3] = u32(kErrNone);
}
// WPADControlDpd(chan, command, cb): command 0 = camera off, else on (basic/extended/full).
HLE(WPADControlDpd) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    if (chan >= kRemotes || !sChan[chan].connected) {
        c.r[3] = u32(kErrNoController);
        return;
    }
    sChan[chan].dpd = c.r[4] != 0;
    if (sLog) log("[wpad] remote %u DPD %s", chan + 1, sChan[chan].dpd ? "on" : "off");
    callLater(kReplyDelay, c.r[5], chan, u32(kErrNone), "DPD");
    c.r[3] = u32(kErrNone);
}
HLE(WPADIsDpdEnabled) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    c.r[3] = chan < kRemotes && sChan[chan].connected && sChan[chan].dpd ? 1 : 0;
}
HLE(WPADGetDpdSensitivity) { c.r[3] = 3; }
HLE(WPADGetSensorBarPosition) { c.r[3] = 1; }  // WPAD_SENSOR_BAR_POS_TOP (the SYSCONF default)
HLE(WPADGetRadioSensitivity) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    c.r[3] = chan < kRemotes && sChan[chan].connected ? 95 : 0;
}
// WPADSaveConfig(cb(result)) -> TRUE: the SYSCONF settings HBM changed (volume, motor).
HLE(WPADSaveConfig) {
    callLater(kReplyDelay, c.r[3], 0, 0, "save config");
    c.r[3] = 1;
}

// ---- WPAD: speaker (its stream is decoded and played on the PC: audio_remote.cpp) ----------

// WPADControlSpeaker(chan, command, cb): 0 off, 1 on, 2 mute, 3 unmute, 4 play, 5 (stream setup).
HLE(WPADControlSpeaker) {
    const u32 chan = c.r[3], cmd = c.r[4];
    std::lock_guard lk(sMutex);
    if (chan >= kRemotes || !sChan[chan].connected) {
        c.r[3] = u32(kErrNoController);
        return;
    }
    Chan& ch = sChan[chan];
    if (cmd == 0) ch.speaker = false;
    else if (cmd == 1) ch.speaker = true, ch.speakerMuted = false;
    else if (cmd == 2) ch.speakerMuted = true;
    else if (cmd == 3) ch.speakerMuted = false;
    if (sLog) log("[wpad] remote %u speaker command %u", chan + 1, cmd);
    audio::remoteSpeakerCommand(int(chan), int(cmd));  // the Remote speaker on the PC
    wiimote::speakerControl(int(chan), int(cmd));  // a real Remote's speaker (wiimote_hid.cpp)
    callLater(kReplyDelay, c.r[5], chan, u32(kErrNone), "speaker");
    c.r[3] = u32(kErrNone);
}
HLE(WPADIsSpeakerEnabled) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    c.r[3] = chan < kRemotes && sChan[chan].connected && sChan[chan].speaker && !sChan[chan].speakerMuted ? 1 : 0;
}
HLE(WPADCanSendStreamData) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    c.r[3] = chan < kRemotes && sChan[chan].connected && sChan[chan].speaker ? 1 : 0;
}
// WPADSendStreamData(chan, data, len): 4-bit ADPCM for the Remote speaker (20 bytes per report).
HLE(WPADSendStreamData) {
    const u32 chan = c.r[3];
    std::lock_guard lk(sMutex);
    if (chan >= kRemotes || !sChan[chan].connected) {
        c.r[3] = u32(kErrNoController);
        return;
    }
    sChan[chan].streamBytes += c.r[5];
    audio::remoteStreamData(int(chan), c.r[4], c.r[5], sChan[chan].speaker && !sChan[chan].speakerMuted);
    if (c.r[4] && c.r[5]) wiimote::speakerData(int(chan), hostp(c.r[4]), std::min<u32>(c.r[5], 0x400));
    c.r[3] = u32(kErrNone);
}
HLE(WPADGetSpeakerVolume) {
    std::lock_guard lk(sMutex);
    c.r[3] = sSpeakerVolume;
}
HLE(WPADSetSpeakerVolume) {
    std::lock_guard lk(sMutex);
    sSpeakerVolume = u8(c.r[3]);
}

// ---- KPAD ----------------------------------------------------------------------------------

// KPADRead(chan, KPADStatus* buf, u32 count) -> number of samples written (newest at buf[0]).
// One sample per 5 ms report since the previous read, at most count; older samples blend from
// the previous read's state to the current one (pointer, stick, acceleration).
HLE(KPADRead) {
    const u32 chan = c.r[3], buf = c.r[4], count = c.r[5];
    if (chan >= kRemotes || !buf || !count) {
        c.r[3] = 0;
        return;
    }
    const State s = current(chan);
    std::lock_guard lk(sMutex);
    Chan& ch = sChan[chan];
    if (!s.connected) {
        ch.haveRead = false;
        c.r[3] = 0;
        return;
    }
    // CF_CLOCK_FRAMES=1 (deterministic scripted runs, tools/routes/run.ps1): reports are counted on
    // the guest-frame clock (59.94 frames/s), so the number of samples per read and their blend no
    // longer depend on host timing; with real time a scripted walk drifted by fractions of a step
    // per stick change and routes diverged.
    static const bool frameClock = std::getenv("CF_CLOCK_FRAMES") != nullptr;
    const u64 now = frameClock ? u64(double(game::guestFrames()) * (kTimerClock / 59.94)) : timeBase();
    const double perSample = kTimerClock / kReportHz;
    u32 n = 1;
    if (ch.haveRead) {
        const double due = double(now - ch.lastRead) / perSample + ch.sampleDebt;
        n = u32(std::min<double>(std::floor(due), count));
        ch.sampleDebt = std::min(due - std::floor(due), 1.0);
        if (n == 0) {
            c.r[3] = 0;  // read again within one report period: nothing new
            return;
        }
    } else {
        ch.prev = s;
        ch.prevHold = 0;
        ch.sampleDebt = 0;
    }
    ch.lastRead = now;
    ch.haveRead = true;
    const u32 hold = s.buttons & wiimote::kBtnGameMask;
    const u32 fmt = ch.dataFormat ? ch.dataFormat : (s.ext == wiimote::kDevNunchuk ? kFmtNunchukAccDpd : kFmtCoreAccDpd);
    // Oldest first, so each sample's motion deltas are against the one before it.
    State older = ch.prev;
    for (u32 i = n; i-- > 0;) {
        const float t = float(n - i) / float(n);  // 1 for the newest
        State x = s;
        if (i > 0) {
            x.px = ch.prev.px + (s.px - ch.prev.px) * t;
            x.py = ch.prev.py + (s.py - ch.prev.py) * t;
            x.stickX = ch.prev.stickX + (s.stickX - ch.prev.stickX) * t;
            x.stickY = ch.prev.stickY + (s.stickY - ch.prev.stickY) * t;
            x.acc = lerp3(ch.prev.acc, s.acc, t);
            x.nunAcc = lerp3(ch.prev.nunAcc, s.nunAcc, t);
            if (!ch.prev.pointerValid) x.pointerValid = s.pointerValid;
        }
        if (!x.pointerValid) {  // KPAD keeps the last position while the dots are lost
            x.px = older.px;
            x.py = older.py;
        }
        const u32 trig = i == 0 ? hold & ~ch.prevHold : 0, release = i == 0 ? ch.prevHold & ~hold : 0;
        writeStatus(buf + i * 0x84, x, older, hold, trig, release, fmt);
        older = x;
    }
    ch.prev = older;
    ch.prevHold = hold;
    c.r[3] = n;
}

}  // namespace recomp
