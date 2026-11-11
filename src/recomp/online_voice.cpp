// Wii Speak voice chat from the PC microphone (docs/ONLINE.md "Wii Speak voice chat").
//
// City Folk drives the Wii Speak through Nintendo's PMIC library (USB device 057E:0308 on
// /dev/usb/oh0, isochronous 16-bit samples into a ring buffer). The game side (USA fn_8017169C,
// a periodic alarm, and the state machine fn_80172E20 run by the network thread) does, per
// voice frame: VCT's mixed incoming voices -> soft limiter -> an AX voice (heard by everyone,
// with or without a Wii Speak); PMICRead(frame) -> the game's echo suppressor (fn_80229D08:
// mic frame vs the voices being played) -> VCT_SendAudio (voice detection fn_8036CCF4, encoded,
// sent with DWC_SendUnreliable, which our DWC HLE carries over the room's sealed connection).
// An AI DMA callback (fn_801723BC) feeds the game's own output back to PMIC as an echo reference.
// The echo suppressor makes the chat half duplex (a speaker's voice playing mutes the
// listener's microphone, measured: no frame got through VCT's voice detection while the other
// player talked); F1 "Echo suppression" off (headphones) skips it.
//
// Here the PMIC API is answered by the host (hooks.txt "Wii Speak", USA names, all versions):
//   fn_80225CE4 PMICInit(work)          -> 0 (library ready; as a Wii with no Wii Speak yet)
//   fn_80225FE0 PMICQuit()              -> 0
//   fn_802260CC PMICProbe()             -> 0 device present (F1 microphone on), -2 none
//   fn_802261A0 PMICOpenAsync(cb, arg)  \
//   fn_802262C8 PMICCloseAsync(cb, arg)  | 0, callback(0, arg) at once (the game sets its
//   fn_80226358 PMICStartAsync(cb, arg)  | "waiting" word before the call and polls it
//   fn_8022646C PMICStopAsync(cb, arg)  /  every frame)
//   fn_80229B50 / fn_80229C28 / fn_8022A15C (value, cb, arg): device control requests -> same
//   fn_8022A238(value)                  -> 0 (reset pulse after the filter's alert)
//   fn_80228FD4 PMICRead(buf, n)        -> n samples from the microphone (or 0)
//   fn_802291F8 / fn_802290BC           echo reference resample/write: not needed on a PC
//                                        (no speaker next to the microphone): 0 / n
// Rate: 8 kHz mono s16, measured in a visit: the game reads 1152 samples per voice frame, 7.0
// frames per second (VCT's 144 ms frame).
//
// Privacy: no recording device is opened and nothing is read unless F1 > Online "Microphone"
// is on; with push-to-talk nothing reaches the game unless the key is held (samples read
// while it is up are dropped, so no frame is sent).
#include "recomp/online_voice.h"

#include "port/settings.h"
#include "recomp/controls.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/online.h"
#include "recomp/pc_keyboard.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace recomp {
namespace {

constexpr int kRate = 8000;  // samples per second the game reads (mono s16)

double secs() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

bool logOn() {
    static const bool on = [] {
        const char* e = std::getenv("CF_MIC_LOG");
        return e && *e && *e != '0';
    }();
    return on || online::logOn();
}

// ---- test input (CF_MIC_WAV=<file>): a WAV file instead of a microphone, looped ----
struct Wav {
    std::vector<s16> pcm;  // mono at kRate
    size_t pos = 0;
};

bool loadWav(const char* path, std::vector<s16>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<u8> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto le16 = [&](size_t o) { return u32(b[o] | b[o + 1] << 8); };
    auto le32 = [&](size_t o) { return u32(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | u32(b[o + 3]) << 24); };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    u32 fmt = 0, ch = 0, rate = 0, bits = 0;
    size_t data = 0, dataLen = 0;
    for (size_t o = 12; o + 8 <= b.size();) {
        const u32 len = le32(o + 4);
        if (!std::memcmp(b.data() + o, "fmt ", 4) && o + 24 <= b.size()) {
            fmt = le16(o + 8);
            ch = le16(o + 10);
            rate = le32(o + 12);
            bits = le16(o + 22);
        } else if (!std::memcmp(b.data() + o, "data", 4)) {
            data = o + 8;
            dataLen = std::min<size_t>(len, b.size() - data);
        }
        o += 8 + len + (len & 1);
    }
    if (!data || !ch || !rate || !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) return false;
    const size_t frameBytes = ch * bits / 8, frames = dataLen / frameBytes;
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; i++) {
        float s = 0;
        for (u32 k = 0; k < ch; k++) {
            const size_t o = data + i * frameBytes + k * bits / 8;
            if (fmt == 1) s += float(s16(le16(o))) / 32768.0f;
            else {
                const u32 w = le32(o);
                float v;
                std::memcpy(&v, &w, 4);
                s += v;
            }
        }
        mono[i] = s / float(ch);
    }
    // linear resample to kRate
    const double step = double(rate) / kRate;
    out.clear();
    for (double t = 0; t + 1 < double(frames); t += step) {
        const size_t i = size_t(t);
        const float fr = float(t - double(i));
        const float v = mono[i] * (1 - fr) + mono[i + 1] * fr;
        out.push_back(s16(std::clamp(v * 32767.0f, -32768.0f, 32767.0f)));
    }
    return !out.empty();
}

// ---- state ----
struct State {
    std::mutex m;
    // capture (host thread opens/closes; the game's read takes samples under the lock)
    SDL_AudioStream* stream = nullptr;
    std::string streamDevice;  // name it was opened with ("" = default)
    std::string status = "Off";
    bool wavMode = false;
    Wav wav;
    // PMIC as the game sees it
    bool inited = false, open = false, started = false;
    // statistics (log / F1)
    u64 reads = 0, framesSent = 0, shortReads = 0, held = 0;
    double lastRead = -1, firstRead = -1;
    float peak = 0;          // level meter (linear peak, decays)
    double sentAt = -100;    // last frame given to the game while talking
} s;

std::atomic<bool> sTalking{false};  // push-to-talk held or open mic (host thread writes)
std::atomic<double> sMeterSeen{-100.0};

// Test aids: CF_MIC_WAV=<file> (input from a WAV, microphone forced on, open mic unless
// CF_MIC_PTT_CYCLE=<on s>,<off s> toggles "push-to-talk held" in that rhythm).
const char* wavPath() {
    static const char* p = [] {
        const char* e = std::getenv("CF_MIC_WAV");
        return e && *e ? e : nullptr;
    }();
    return p;
}

bool micOn() { return port::settings().online.micOn || wavPath(); }

float gain() { return std::clamp(port::settings().online.micGain, 0.0f, 4.0f); }

void closeStreamLocked() {
    if (s.stream) {
        SDL_DestroyAudioStream(s.stream);
        s.stream = nullptr;
        if (logOn()) std::printf("[voice] microphone closed\n");
    }
}

// Host thread: open/close the recording device as needed.
void manageCapture() {
    const auto& o = port::settings().online;
    const bool want = micOn() && !wavPath() && (s.open || secs() - sMeterSeen.load() < 1.0);
    std::lock_guard lk(s.m);
    if (wavPath()) {
        if (s.wav.pcm.empty()) {
            if (loadWav(wavPath(), s.wav.pcm)) {
                s.status = "Test input: " + std::string(wavPath());
                std::printf("[voice] test input %s: %zu samples at %d Hz\n", wavPath(), s.wav.pcm.size(), kRate);
            } else {
                s.status = "Test input could not be read";
                std::printf("[voice] CF_MIC_WAV %s: not a 16-bit / float WAV\n", wavPath());
                s.wav.pcm.assign(1, 0);
            }
        }
        s.wavMode = true;
        return;
    }
    if (!want) {
        closeStreamLocked();
        s.status = micOn() ? "On (opens when the game starts voice chat)" : "Off";
        return;
    }
    if (s.stream && s.streamDevice == o.micDevice) return;
    closeStreamLocked();
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        s.status = std::string("No audio: ") + SDL_GetError();
        return;
    }
    SDL_AudioDeviceID dev = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!o.micDevice.empty()) {
        int n = 0;
        if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&n)) {
            for (int i = 0; i < n; i++)
                if (const char* nm = SDL_GetAudioDeviceName(ids[i]); nm && o.micDevice == nm) dev = ids[i];
            SDL_free(ids);
        }
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kRate};
    s.stream = SDL_OpenAudioDeviceStream(dev, &spec, nullptr, nullptr);
    s.streamDevice = o.micDevice;
    if (!s.stream) {
        s.status = std::string("Microphone not available: ") + SDL_GetError();
        std::printf("[voice] %s\n", s.status.c_str());
        return;
    }
    SDL_ResumeAudioStreamDevice(s.stream);
    s.status = "Microphone open";
    std::printf("[voice] microphone open (%s)\n", o.micDevice.empty() ? "default device" : o.micDevice.c_str());
}

// Samples for one read: n mono s16 (host order) into out. False when there are not enough yet.
bool takeLocked(s16* out, int n) {
    if (s.wavMode) {
        for (int i = 0; i < n; i++) {
            out[i] = s.wav.pcm[s.wav.pos++];
            if (s.wav.pos >= s.wav.pcm.size()) s.wav.pos = 0;
        }
        return true;
    }
    if (!s.stream) return false;
    int avail = SDL_GetAudioStreamAvailable(s.stream) / 2;
    // Keep the latency at most ~2 voice frames (~290 ms): drop the oldest samples beyond that.
    while (avail > n * 2) {
        s16 junk[512];
        const int k = std::min<int>(512, avail - n);
        SDL_GetAudioStreamData(s.stream, junk, k * 2);
        avail -= k;
    }
    if (avail < n) return false;
    return SDL_GetAudioStreamData(s.stream, out, n * 2) == n * 2;
}

void meterLocked(const s16* p, int n) {
    float pk = 0;
    for (int i = 0; i < n; i++) pk = std::max(pk, std::fabs(float(p[i]) / 32768.0f));
    s.peak = std::max(pk, s.peak * 0.85f);
}

// Push-to-talk binding capture (F1 "Change...").
bool sCapturing = false;
std::vector<u8> sKeysAtCapture;

bool pttHeld() {
    const auto& o = port::settings().online;
    if (const char* c = std::getenv("CF_MIC_PTT_CYCLE"); c && *c) {
        double on = 2, off = 2;
        std::sscanf(c, "%lf,%lf", &on, &off);
        return std::fmod(secs(), on + off) < on;
    }
    int n = 0;
    const bool* keys = SDL_GetKeyboardState(&n);
    // (a key that types text is the Wii keyboard's while typing with the PC keyboard: pc_keyboard.cpp)
    if (keys && o.micPttKey > 0 && o.micPttKey < n && keys[o.micPttKey] &&
        !(pckbd::typing() && pckbd::typesText(o.micPttKey)))
        return true;
    if (o.micPttPadButton >= 0)
        if (SDL_Gamepad* g = controls::portGamepad(0))
            if (SDL_GetGamepadButton(g, SDL_GamepadButton(o.micPttPadButton))) return true;
    return false;
}

// ---- guest side ----
void answerCb(Ctx& c, u32 cb, u32 arg) {
    if (cb) gu::call(c, cb, {0u, arg});
}

}  // namespace

// fn_80225CE4 PMICInit(void* work): library initialised, no device yet.
HLE(fn_80225CE4) {
    {
        std::lock_guard lk(s.m);
        s.inited = true;
        s.open = s.started = false;
    }
    if (logOn()) std::printf("[voice] PMICInit (work %08X): microphone %s\n", c.r[3], micOn() ? "on" : "off");
    c.r[3] = 0;
}

// fn_80225FE0 PMICQuit()
HLE(fn_80225FE0) {
    std::lock_guard lk(s.m);
    s.inited = s.open = s.started = false;
    if (logOn()) std::printf("[voice] PMICQuit\n");
    c.r[3] = 0;
}

// fn_802260CC PMICProbe(): 0 = a Wii Speak is attached, -2 = none, -4 = not initialised.
HLE(fn_802260CC) {
    std::lock_guard lk(s.m);
    static int last = 1;
    const int r = !s.inited ? -4 : micOn() ? 0 : -2;
    if (r != 0) s.open = s.started = false;  // unplugged: the library forgets the device
    if (r != last && logOn()) std::printf("[voice] PMICProbe -> %d (%s)\n", r, r == 0 ? "Wii Speak attached" : "no Wii Speak");
    last = r;
    c.r[3] = u32(r);
}

// fn_802261A0 PMICOpenAsync(cb, arg)
HLE(fn_802261A0) {
    const u32 cb = c.r[3], arg = c.r[4];
    {
        std::lock_guard lk(s.m);
        if (!s.inited || !micOn()) {
            c.r[3] = u32(-4);
            return;
        }
        s.open = true;
    }
    if (logOn()) std::printf("[voice] PMICOpen\n");
    answerCb(c, cb, arg);
    c.r[3] = 0;
}

// fn_802262C8 PMICCloseAsync(cb, arg)
HLE(fn_802262C8) {
    const u32 cb = c.r[3], arg = c.r[4];
    {
        std::lock_guard lk(s.m);
        s.open = s.started = false;
    }
    if (logOn()) std::printf("[voice] PMICClose\n");
    answerCb(c, cb, arg);
    c.r[3] = 0;
}

// fn_80226358 PMICStartAsync(cb, arg)
HLE(fn_80226358) {
    const u32 cb = c.r[3], arg = c.r[4];
    {
        std::lock_guard lk(s.m);
        if (!s.open) {
            c.r[3] = u32(-4);
            return;
        }
        s.started = true;
    }
    if (logOn()) std::printf("[voice] PMICStart\n");
    answerCb(c, cb, arg);
    c.r[3] = 0;
}

// fn_8022646C PMICStopAsync(cb, arg)
HLE(fn_8022646C) {
    const u32 cb = c.r[3], arg = c.r[4];
    {
        std::lock_guard lk(s.m);
        s.started = false;
    }
    if (logOn()) std::printf("[voice] PMICStop\n");
    answerCb(c, cb, arg);
    c.r[3] = 0;
}

// The device control requests (value, cb, arg): accepted.
static void controlRequest(Ctx& c, const char* what) {
    const u32 v = c.r[3], cb = c.r[4], arg = c.r[5];
    if (logOn()) std::printf("[voice] PMIC %s %u\n", what, v);
    answerCb(c, cb, arg);
    c.r[3] = 0;
}
HLE(fn_80229B50) { controlRequest(c, "control A"); }
HLE(fn_80229C28) { controlRequest(c, "control gain"); }
HLE(fn_8022A15C) { controlRequest(c, "control B"); }
HLE(fn_8022A238) { c.r[3] = 0; }

// fn_80229D08(s16* mic, s16* playing, int n, int first): the game's echo suppressor, in place on
// the mic frame; returns its alert flag. Off: the frame passes unchanged.
HLE(fn_80229D08) {
    static const char* env = std::getenv("CF_MIC_ECHO");  // test aid: 0 / 1 overrides the setting
    if (env && *env ? *env != '0' : port::settings().online.micEchoSuppression) {
        callOriginal(c, "fn_80229D08");
        return;
    }
    c.r[3] = 0;
}

// Echo reference (the game's own output): not used on a PC.
HLE(fn_802291F8) { c.r[3] = 0; }
HLE(fn_802290BC) { c.r[3] = c.r[4]; }

// fn_80228FD4 PMICRead(s16* buf, int n) -> samples copied (0 when none).
HLE(fn_80228FD4) {
    const u32 buf = c.r[3];
    const int n = int(c.r[4]);
    int got = 0;
    if (buf && n > 0 && n <= 4096 && micOn()) {
        static std::vector<s16> tmp;
        tmp.resize(size_t(n));
        std::lock_guard lk(s.m);
        const double t = secs();
        if (s.firstRead < 0) {
            s.firstRead = t;
            if (logOn()) std::printf("[voice] first read: %d samples per frame\n", n);
        }
        s.reads++;
        s.lastRead = t;
        if (s.started && takeLocked(tmp.data(), n)) {
            const float g = gain();
            // Plus a noise floor of about -68 dBFS (white, +-20 LSB), as the Wii Speak's analog
            // input always has: VCT's voice detection (fn_8036CCF4) ignores frames below energy
            // 49 and starts sending only when a frame is ~5x louder (rms) than the last frames
            // above that, so digital silence between words (a noise-suppressed PC microphone, a
            // test file) never lets it start.
            static u32 seed = 0x2545F491u;
            for (auto& v : tmp) {
                seed = seed * 1664525u + 1013904223u;
                const float noise = float(int(seed >> 24) % 41 - 20);
                v = s16(std::clamp(float(v) * g + noise, -32768.0f, 32767.0f));
            }
            meterLocked(tmp.data(), n);
            if (sTalking.load()) {
                for (int i = 0; i < n; i++) wr16(buf + 2 * u32(i), u16(tmp[size_t(i)]));
                got = n;
                s.framesSent++;
                s.sentAt = t;
            } else {
                s.held++;  // read and dropped: push-to-talk not held
            }
        } else {
            s.shortReads++;
        }
        static double lastLog = 0;
        if (logOn() && t - lastLog >= 5) {
            lastLog = t;
            std::printf("[voice] %.0f s: reads %llu (%.1f/s), frames given to the game %llu, held back (push-to-talk up) %llu, "
                        "short %llu, peak %.1f dBFS\n",
                        t - s.firstRead, (unsigned long long)s.reads, s.reads / std::max(0.001, t - s.firstRead),
                        (unsigned long long)s.framesSent, (unsigned long long)s.held, (unsigned long long)s.shortReads,
                        s.peak > 0 ? 20 * std::log10(s.peak) : -99.0);
        }
    }
    c.r[3] = u32(got);
}

namespace voice {

void tick() {
    const auto& o = port::settings().online;
    const bool talk = micOn() && (wavPath() ? (std::getenv("CF_MIC_PTT_CYCLE") ? pttHeld() : true) : (o.micPushToTalk ? pttHeld() : true));
    if (talk != sTalking.load()) {
        sTalking = talk;
        if (logOn()) std::printf("[voice] %s\n", talk ? "talking (frames go to the game)" : "not talking");
    }
    manageCapture();
    // Level meter while the game does not read (F1 open before voice chat started).
    std::lock_guard lk(s.m);
    if (s.stream && !s.started) {
        s16 tmp[1024];
        int got;
        while ((got = SDL_GetAudioStreamData(s.stream, tmp, sizeof tmp)) > 0) {
            const int k = got / 2;
            for (int i = 0; i < k; i++) tmp[i] = s16(std::clamp(float(tmp[i]) * gain(), -32768.0f, 32767.0f));
            meterLocked(tmp, k);
        }
    }
    if (!s.stream && !s.wavMode) s.peak *= 0.85f;
}

bool drawSection() {
    auto& o = port::settings().online;
    bool dirty = false;
    menu::openTreesForCheck();
    if (!ImGui::CollapsingHeader("Voice chat (Wii Speak)")) return false;
    sMeterSeen = secs();
    ImGui::TextWrapped("Your PC microphone as the game's Wii Speak. Everyone in a visit hears the others' voices, "
                       "with or without a microphone of their own. Nothing is recorded or sent while the microphone is off.");
    dirty |= menu::Checkbox("Microphone", &o.micOn);
    ImGui::SetItemTooltip("Off: the game sees no Wii Speak and no recording device is opened. On: the game sees a Wii Speak; "
                          "voice is sent only while you talk (push-to-talk) or always (open mic).");
    ImGui::BeginDisabled(!o.micOn);
    // device picker
    {
        const char* cur = o.micDevice.empty() ? "System default" : o.micDevice.c_str();
        if (ImGui::BeginCombo("Device##mic", cur)) {
            if (ImGui::Selectable("System default", o.micDevice.empty())) {
                o.micDevice.clear();
                dirty = true;
            }
            if (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                int n = 0;
                if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&n)) {
                    for (int i = 0; i < n; i++) {
                        const char* nm = SDL_GetAudioDeviceName(ids[i]);
                        if (!nm) continue;
                        ImGui::PushID(i);
                        if (ImGui::Selectable(nm, o.micDevice == nm)) {
                            o.micDevice = nm;
                            dirty = true;
                        }
                        ImGui::PopID();
                    }
                    SDL_free(ids);
                }
            }
            ImGui::EndCombo();
        }
    }
    int mode = o.micPushToTalk ? 0 : 1;
    if (ImGui::RadioButton("Push-to-talk", mode == 0)) mode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Open mic", mode == 1)) mode = 1;
    if ((mode == 0) != o.micPushToTalk) {
        o.micPushToTalk = mode == 0;
        dirty = true;
    }
    if (o.micPushToTalk) {
        std::string label = "Push-to-talk: ";
        label += o.micPttKey > 0 ? SDL_GetScancodeName(SDL_Scancode(o.micPttKey)) : "(no key)";
        if (o.micPttPadButton >= 0) {
            label += " / controller ";
            const char* bn = SDL_GetGamepadStringForButton(SDL_GamepadButton(o.micPttPadButton));
            label += bn ? bn : "?";
        }
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        if (sCapturing) {
            ImGui::TextDisabled("press a key or a controller button (Esc cancels)");
            int n = 0;
            const bool* keys = SDL_GetKeyboardState(&n);
            if (sKeysAtCapture.size() != size_t(n)) sKeysAtCapture.assign(size_t(n), 1);
            for (int k = 4; k < n && keys; k++) {
                if (keys[k] && !sKeysAtCapture[size_t(k)]) {
                    if (k != SDL_SCANCODE_ESCAPE && k != SDL_SCANCODE_F1) {
                        o.micPttKey = k;
                        dirty = true;
                    }
                    sCapturing = false;
                    break;
                }
                sKeysAtCapture[size_t(k)] = keys[k] ? 1 : 0;
            }
            if (SDL_Gamepad* g = controls::portGamepad(0))
                for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && sCapturing; b++)
                    if (SDL_GetGamepadButton(g, SDL_GamepadButton(b))) {
                        o.micPttPadButton = b;
                        dirty = true;
                        sCapturing = false;
                    }
        } else {
            if (menu::SmallButton("Change...##ptt")) {
                sCapturing = true;
                sKeysAtCapture.clear();
            }
            if (o.micPttPadButton >= 0) {
                ImGui::SameLine();
                if (menu::SmallButton("No controller button##ptt")) {
                    o.micPttPadButton = -1;
                    dirty = true;
                }
            }
        }
        ImGui::TextDisabled("Keyboard keys work while the game window has focus; the controller button is port 1's.");
        if (o.micPttKey > 0 && pckbd::typing() && pckbd::typesText(o.micPttKey))
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1),
                               "This key types text while \"Type with the PC keyboard\" is on (F1 > Controls): push-to-talk "
                               "uses the controller button only. Pick a key such as Right Ctrl or F5.");
    }
    dirty |= menu::SliderFloat("Input gain##mic", &o.micGain, 0.0f, 4.0f, "%.2f");
    dirty |= menu::Checkbox("Echo suppression", &o.micEchoSuppression);
    ImGui::SetItemTooltip("The Wii Speak's own echo suppression (on = as on the Wii): while someone else's voice "
                          "plays, your microphone is held back, so only one person talks at a time. Turn it off "
                          "when you use headphones, so both of you can talk at once.");
    float peak, sentAgo;
    std::string status;
    {
        std::lock_guard lk(s.m);
        peak = s.peak;
        sentAgo = float(secs() - s.sentAt);
        status = s.status;
    }
    const float db = peak > 0 ? 20 * std::log10(peak) : -60.0f;
    const float frac = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    char txt[64];
    std::snprintf(txt, sizeof txt, "%.0f dBFS%s", db, peak >= 0.99f ? " (clipping)" : "");
    ImGui::ProgressBar(frac, ImVec2(-1, 0), txt);  // level meter
    ImGui::TextDisabled("%s%s", status.c_str(), sentAgo < 0.3f ? " - sending" : sTalking.load() ? " - talking" : "");
    ImGui::EndDisabled();
    return dirty;
}

}  // namespace voice
}  // namespace recomp
