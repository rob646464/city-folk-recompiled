// The Wii Remote speakers, played on the PC (docs/AUDIO.md "Wii Remote speaker on the PC").
//
// What a Remote's speaker plays is what the game streams to it: WPADSendStreamData packets (up to
// 20 bytes, one output report 0x18 each) of 4-bit Yamaha ADPCM, after WPADControlSpeaker has
// turned the speaker on and started playback. Two writers use it in City Folk: NW4R's
// RemoteSpeaker (it encodes the DSP's Remote output, AXRmtGetSamples, with WENC), and the HOME
// Menu, which streams its own pre-encoded sounds. Decoding the stream covers both, and plays what
// the real speaker would get (its ADPCM included).
//
// The decoder follows the public description of Yamaha 4-bit ADPCM (the Wii Remote speaker's
// format 0x00): per nibble a difference (2|n|+1) * step / 8 is added or subtracted (sign bit 3),
// the step scales by 230/256 (|n| < 4) .. 614/256 (|n| = 7) and stays in 127..24576. Each packet
// is decoded into a per-Remote queue at the speaker's 6 kHz (the rate the SDK sets up; the sending
// rate measured in the game matches it, CF_AUDIO_LOG), and audio_out.cpp pulls the summed queues
// resampled to the AI rate once per AI block (8-tap Kaiser-windowed sinc, cutoff 0.9 of the
// 3 kHz Nyquist frequency) with a 250 Hz one-pole high-pass for the small speaker. The queue keeps
// 40-120 ms: the game sends from an alarm every ~6.7 ms, the AI pulls every 3 ms.
//
// CF_RMT_DUMP=<file>: every packet appended to <file>.<remote> (raw bytes, for offline checks).
#include "port/settings.h"
#include "recomp/audio.h"
#include "recomp/hle.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>

namespace recomp::audio {
namespace {

constexpr double kSpeakerRate = 6000.0;  // 4-bit ADPCM at 6 kHz (WPAD's speaker configuration)
constexpr size_t kStart = 240;           // 40 ms queued before playback starts
constexpr size_t kMax = 720;             // 120 ms: older samples are dropped beyond this

struct Remote {
    // Decoder
    int pred = 0, step = 127;
    // Queue (6 kHz samples) and playback state
    std::deque<float> q;
    bool playing = false;
    u64 bytes = 0, packets = 0, dropped = 0, underruns = 0;
    u64 burstPackets = 0, burstBytes = 0;  // CF_AUDIO_LOG: the current burst (one sound)
    double burstStart = 0, lastPacket = 0;
    std::FILE* dump = nullptr;
};
Remote sRm[4];
std::mutex sM;

// Resampler from the summed 6 kHz queues to the AI rate.
struct Resampler {
    static constexpr int kTaps = 8, kPhases = 64;
    float h[kPhases + 1][kTaps];
    float hist[kTaps] = {};  // newest last
    double frac = 0;         // position between hist[kTaps/2-1] and hist[kTaps/2], in input samples
    float hpX = 0, hpY = 0;
    Resampler() {
        constexpr double kPi = 3.14159265358979323846;
        auto bessel0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 40; k++) {
                term *= (x / (2 * k)) * (x / (2 * k));
                sum += term;
            }
            return sum;
        };
        const double fc = 0.9, beta = 5.0;
        for (int p = 0; p <= kPhases; p++) {
            double sum = 0;
            for (int k = 0; k < kTaps; k++) {
                const double t = (k - (kTaps / 2 - 1)) - double(p) / kPhases;
                const double x = fc * t;
                const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double r = t / (kTaps / 2);
                const double w = std::fabs(r) >= 1 ? 0.0 : bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta);
                h[p][k] = float(sinc * w);
                sum += h[p][k];
            }
            for (int k = 0; k < kTaps; k++) h[p][k] = float(h[p][k] / sum);
        }
    }
    void reset() {
        std::fill(std::begin(hist), std::end(hist), 0.0f);
        frac = 0;
        hpX = hpY = 0;
    }
} sRs;
bool sAnyPlaying = false;

const char* dumpBase() {
    static const char* v = std::getenv("CF_RMT_DUMP");
    return v && *v ? v : nullptr;
}

s16 decodeNibble(Remote& r, int n) {
    static const int kScale[8] = {230, 230, 230, 230, 307, 409, 512, 614};
    const int mag = n & 7;
    const int diff = ((2 * mag + 1) * r.step) >> 3;
    r.pred = std::clamp(n & 8 ? r.pred - diff : r.pred + diff, -32768, 32767);
    r.step = std::clamp((r.step * kScale[mag]) >> 8, 127, 24576);
    return s16(r.pred);
}

}  // namespace

void remoteSpeakerCommand(int chan, int cmd) {
    if (chan < 0 || chan >= 4) return;
    std::lock_guard lk(sM);
    Remote& r = sRm[chan];
    if (cmd == 0 || cmd == 1 || cmd == 4) {  // off / on (initialised) / play: the decoder starts over
        r.pred = 0;
        r.step = 127;
        if (cmd == 0) {
            r.q.clear();
            r.playing = false;
        }
    }
    if (g_log) log("[rmt] remote %d speaker command %d", chan + 1, cmd);
}

void remoteStreamData(int chan, u32 data, u32 len, bool audible) {
    if (chan < 0 || chan >= 4 || len > 64) return;
    std::lock_guard lk(sM);
    Remote& r = sRm[chan];
    r.packets++;
    r.bytes += len;
    if (const char* base = dumpBase()) {
        if (!r.dump) r.dump = std::fopen((std::string(base) + "." + char('1' + chan)).c_str(), "wb");
        if (r.dump) {
            for (u32 i = 0; i < len; i++) std::fputc(int(rd8(data + i)), r.dump);
            std::fflush(r.dump);
        }
    }
    for (u32 i = 0; i < len; i++) {
        const int b = int(rd8(data + i));
        const s16 a = decodeNibble(r, b >> 4);  // high nibble first (docs/AUDIO.md)
        const s16 c = decodeNibble(r, b & 15);
        if (!audible) continue;
        r.q.push_back(a);
        r.q.push_back(c);
    }
    while (r.q.size() > kMax) {
        r.q.pop_front();
        r.dropped++;
    }
    // CF_AUDIO_LOG: one line per burst of packets (a sound): its length and packet rate (the
    // speaker plays 6000 samples/s = 150 packets of 20 bytes per second).
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (r.burstPackets && now - r.lastPacket > 0.1) {
        if (g_log && r.burstPackets > 1)
            log("[rmt] remote %d: %llu packets (%llu bytes) in %.0f ms = %.1f packets/s (%.0f samples/s); queue %zu, "
                "dropped %llu, underruns %llu", chan + 1, (unsigned long long)r.burstPackets,
                (unsigned long long)r.burstBytes, (r.lastPacket - r.burstStart) * 1e3,
                (r.burstPackets - 1) / (r.lastPacket - r.burstStart), (r.burstPackets - 1) / (r.lastPacket - r.burstStart) * 2.0 * double(r.burstBytes) / double(r.burstPackets),
                r.q.size(), (unsigned long long)r.dropped, (unsigned long long)r.underruns);
        r.burstPackets = r.burstBytes = 0;
    }
    if (!r.burstPackets) r.burstStart = now;
    r.burstPackets++;
    r.burstBytes += len;
    r.lastPacket = now;
}

bool remoteFetch(float* out, int n, double outRate) {
    std::lock_guard lk(sM);
    bool any = false;
    for (auto& r : sRm) {
        if (!r.playing && r.q.size() >= kStart) r.playing = true;
        any |= r.playing;
    }
    if (!any) {
        if (sAnyPlaying) sRs.reset();
        sAnyPlaying = false;
        return false;
    }
    sAnyPlaying = true;
    const double step = kSpeakerRate / outRate;
    constexpr float kHp = 0.952f;  // exp(-2 pi 250 / 32028)
    for (int i = 0; i < n; i++) {
        sRs.frac += step;
        while (sRs.frac >= 1.0) {  // next input sample: the sum of the playing Remotes
            sRs.frac -= 1.0;
            float s = 0;
            for (auto& r : sRm) {
                if (!r.playing) continue;
                if (r.q.empty()) {
                    r.playing = false;  // ran dry: wait for kStart again
                    r.underruns++;
                    continue;
                }
                s += r.q.front();
                r.q.pop_front();
            }
            std::copy(sRs.hist + 1, sRs.hist + Resampler::kTaps, sRs.hist);
            sRs.hist[Resampler::kTaps - 1] = s;
        }
        const double fp = sRs.frac * Resampler::kPhases;
        const int ph = std::min(int(fp), Resampler::kPhases - 1);
        const float a = float(fp - ph);
        float acc = 0;
        for (int k = 0; k < Resampler::kTaps; k++) acc += (sRs.h[ph][k] + (sRs.h[ph + 1][k] - sRs.h[ph][k]) * a) * sRs.hist[k];
        sRs.hpY = kHp * (sRs.hpY + acc - sRs.hpX);
        sRs.hpX = acc;
        out[i] = sRs.hpY;
    }
    return true;
}

}  // namespace recomp::audio
