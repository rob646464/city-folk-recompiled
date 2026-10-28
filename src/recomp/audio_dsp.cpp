// Stand-in for the AX audio DSP microcode (RVL SDK AX, the image the game uploads from
// 0x80542900: 0x2000 bytes of IRAM, init vector 0x0010, resume vector 0x0037). The DSP is
// never emulated: we take the command list AX hands over each 3 ms audio frame (mail pair
// 0xBABE<len> + address), read the voice parameter blocks (PBs) the CPU wrote into main
// memory, mix the voices on the host and write back exactly what the microcode writes: the
// PBs, the aux bus uploads AXFX processes, the delayed surround, the AI output buffer and the
// four Wii Remote speaker buffers. The game's own AX / AXFX / NintendoWare snd code runs
// unchanged on top.
//
// The behaviour follows the game's own microcode, read with LM's tools/dsp_disasm.py
// (docs/AUDIO.md has the walk-through with microcode addresses): the same command set and
// parameter order, the same bus layout (32-bit samples in DSP DRAM), the same per-voice order
// (accelerator + resampler, volume envelope, low-pass, biquad, mixer, remote), the same
// fixed-point rounding and saturation, the same end-of-sample / loop handling (its
// accelerator overflow exception handler). The one thing the image does not contain is the
// DSP's coefficient ROM (the polyphase resampler's three 128-phase 4-tap tables at DSP
// 0x1000/0x1200/0x1400): ours are computed here (windowed sinc, see makeCoefs).
// Nothing here comes from Dolphin.
#include "port/settings.h"
#include "recomp/audio.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp: guest frame number (CF_AUDIO_LOG time stamps)
}

namespace recomp::audio {
bool g_log = [] {
    const char* e = std::getenv("CF_AUDIO_LOG");
    return e && *e && *e != '0';
}();

namespace {

constexpr int kFrame = 96;     // samples per 3 ms frame at 32 kHz (fixed by the microcode)
constexpr int kRmtFrame = 18;  // Wii Remote speaker samples per frame (6 kHz)
constexpr u32 kPbSize = 0x140;

// ---- DSP data memory ----
// The mixing buses are 32-bit samples (high word, low word) in DSP DRAM; bus ids below are
// their DSP word addresses. Main L/R/S, the aux A/B/C buses (each L/R/S, contiguous: the aux
// upload/download is one 0x480-byte DMA), the remote speaker buses (18 samples).
// In Dolby Pro Logic II mode S is the left surround and the aux C buses carry the right
// surround of main / aux A / aux B (the DPL2 commands read them that way).
enum : u16 {
    kMainL = 0x0000, kMainR = 0x00C0, kMainS = 0x0180,
    kAuxAL = 0x0400, kAuxAR = 0x04C0, kAuxAS = 0x0580,
    kAuxBL = 0x0640, kAuxBR = 0x0700, kAuxBS = 0x07C0,
    kAuxCL = 0x0880, kAuxCR = 0x0940, kAuxCS = 0x0A00,
};
constexpr u16 kRmtMain[4] = {0x0240, 0x0264, 0x0288, 0x02AC};
constexpr u16 kRmtAux[4] = {0x0AC0, 0x0AE4, 0x0B08, 0x0B2C};
s32 sBus[0x0B50 / 2];  // index = DSP word address / 2
inline s32* bus(u16 dspAddr) { return &sBus[dspAddr / 2]; }

// Microcode state that lives in DSP DRAM across frames (set by its init entry, 0x0010).
u16 sPrevMaster = 0x8000;                       // 0x0CE5: master volume of the last frame
u16 sPrevAuxVol[3] = {0x8000, 0x8000, 0x8000};  // 0x0CE6..8: aux A/B/C return volumes
u16 sCompCount = 0;                             // 0x0CE4: compressor release counter
// The voice buffer (0x0C40..0x0CBF: 32 samples of ITD history, then the 96 voice samples):
// a stopped voice with ITD on writes back whatever the previous voice left there, like the
// microcode does.
s16 sVoice[32 + kFrame + 64];

std::atomic<bool> sFrameBusy{false};
std::atomic<long long> sFrameEnd{0};
u64 sFrames = 0;  // command lists processed

inline s16 sat16(s64 v) { return s16(std::clamp<s64>(v, -32768, 32767)); }
inline s32 floorDiv32768(s64 v) { return s32(v >> 15); }  // arithmetic shift = floor

// Guest memory, big-endian; the DSP addresses main memory physically (MEM1 0x0..., MEM2
// 0x1...), which ctx.h maps like the cached mirrors.
inline u16 g16(u32 a) { return u16(rd16(a)); }
inline s32 g32(u32 a) { return s32(rd32(a)); }

// ---- Coefficient ROM stand-in ----
// The polyphase resampler (srcSelect 0) sums four input samples x0 (oldest) .. x3 (newest)
// with the taps of phase p = top 7 bits of the 16-bit position fraction: out = sum x_k c_k
// >> 15. coefSelect picks one of three tables (the SDK's 4TAP_8K / 12K / 16K: band limits
// for a 32 kHz output). The tables are in the DSP's coefficient ROM, not in the game; ours
// are 4-tap windowed sincs (Hann window over the 4 taps) with the cutoff at 0.5 / 0.75 / 1.0
// of the input Nyquist frequency, centred between x1 and x2 (position 1 + phase/128), each
// phase normalised to unity gain. Table 2 at phase 0 is the identity on x1, so a voice at
// its native rate (ratio 1.0) passes through unchanged.
struct CoefTables {
    s32 c[3][128][4];
};
const CoefTables& coefs() {
    static const CoefTables t = [] {
        CoefTables r{};
        const double cut[3] = {0.5, 0.75, 1.0};
        for (int s = 0; s < 3; s++)
            for (int p = 0; p < 128; p++) {
                double h[4], sum = 0;
                for (int k = 0; k < 4; k++) {
                    const double d = k - (1.0 + p / 128.0);
                    const double x = 3.14159265358979323846 * cut[s] * d;
                    const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
                    const double w = std::fabs(d) < 2.0 ? 0.5 * (1.0 + std::cos(3.14159265358979323846 * d / 2.0)) : 0.0;
                    h[k] = sinc * w;
                    sum += h[k];
                }
                int isum = 0;
                for (int k = 0; k < 4; k++) {
                    r.c[s][p][k] = int(std::lround(h[k] / sum * 32768.0));
                    isum += r.c[s][p][k];
                }
                r.c[s][p][1 + (p >= 64)] += 32768 - isum;  // exact unity gain per phase
            }
        return r;
    }();
    return t;
}

// ---- Parameter block (AXPB of this SDK; byte offsets, big-endian u16 fields) ----
// The microcode DMAs the 0x140-byte PB to DSP 0x02D0, works on it there and DMAs the whole
// block back to its own address (+0x04) when the voice is done.
namespace pb {
enum : u32 {
    kNext = 0x00, kCurr = 0x04, kSrcSelect = 0x08, kCoefSelect = 0x0A, kMixerCtrl = 0x0C,
    kState = 0x10, kType = 0x12,
    kMix = 0x14,  // 24 u16: vL dL vR dR, AuxA L/R, AuxB L/R, AuxC L/R, then vS dS, AuxA/B/C S
    kItdOn = 0x44, kItdBuf = 0x46, kItdShiftL = 0x4A, kItdShiftR = 0x4C, kItdTargetL = 0x4E, kItdTargetR = 0x50,
    kDpop = 0x52,  // 12 s16: aL aAuxAL aAuxBL aAuxCL aR ... aS ... (written by the DSP)
    kVeVol = 0x6A, kVeDelta = 0x6C,
    kLoopFlag = 0x6E, kFormat = 0x70, kLoopAddr = 0x72, kEndAddr = 0x76, kCurAddr = 0x7A,
    kAdpcmCoef = 0x7E, kGain = 0x9E, kPredScale = 0xA0, kYn1 = 0xA2, kYn2 = 0xA4,
    kRatio = 0xA6, kFrac = 0xAA, kLast = 0xAC,  // 4 samples of resampler history, oldest first
    kLoopPredScale = 0xB4, kLoopYn1 = 0xB6, kLoopYn2 = 0xB8,
    kLpfOn = 0xBA, kLpfYn1 = 0xBC, kLpfA0 = 0xBE, kLpfB0 = 0xC0,
    kBiqOn = 0xC2, kBiqXn1 = 0xC4,  // xn1 xn2 yn1 yn2 b0 b1 b2 a1 a2
    kRmtOn = 0xD6, kRmtMixerCtrl = 0xD8, kRmtMix = 0xDA,  // 16 u16: vMain0 dMain0 vAux0 dAux0, x4
    kRmtDpop = 0xFA,                                       // aMain0..3, aAux0..3
    kRmtFrac = 0x10A, kRmtLast = 0x10C,                    // remote resampler: fraction, 4 samples
    kRmtIir = 0x114,  // 1: low-pass (yn1 a0 b0 at 0x116), 2: biquad (xn1 xn2 yn1 yn2 b0 b1 b2 a1 a2 at 0x116)
};
}  // namespace pb

struct Pb {
    u8 b[kPbSize];
    u16 u(u32 o) const { return u16(b[o] << 8 | b[o + 1]); }
    s16 s(u32 o) const { return s16(u(o)); }
    u32 u32at(u32 o) const { return u32(u(o)) << 16 | u(o + 2); }
    void set(u32 o, u16 v) {
        b[o] = u8(v >> 8);
        b[o + 1] = u8(v);
    }
    void set32(u32 o, u32 v) {
        set(o, u16(v >> 16));
        set(o + 2, u16(v));
    }
};

// ---- Sample accelerator ----
// What the microcode reads through 0xFFDD: the ARAM/main-memory accelerator decoding ADPCM
// (format 0x00, nibble addresses, 8-byte frames: predictor/scale header + 14 nibbles),
// PCM16 (0x0A, 16-bit word addresses) or PCM8 (0x19, byte addresses). When the address passes
// the end address the hardware jumps to the loop address and raises the accelerator
// exception; the microcode's handler (0x0F61) stops a voice without the loop flag (and the
// rest of its frame reads a zero word), or reloads the predictor/scale (and, for a voice that
// is not a stream, the history) from the PB's loop context.
struct Accel {
    Pb& p;
    u16 format;
    u32 loop, end, cur;
    s16 coef[16];
    u16 gain, ps;
    s16 yn1, yn2;
    bool zero = false;  // stopped this frame: reads return 0

    explicit Accel(Pb& pb) : p(pb) {
        format = p.u(pb::kFormat);
        loop = p.u32at(pb::kLoopAddr);
        end = p.u32at(pb::kEndAddr);
        cur = p.u32at(pb::kCurAddr);
        for (int i = 0; i < 16; i++) coef[i] = p.s(pb::kAdpcmCoef + 2 * i);
        gain = p.u(pb::kGain);
        ps = p.u(pb::kPredScale);
        yn1 = p.s(pb::kYn1);
        yn2 = p.s(pb::kYn2);
    }
    void store() {
        p.set32(pb::kCurAddr, cur);
        p.set(pb::kPredScale, ps);
        p.set(pb::kYn1, u16(yn1));
        p.set(pb::kYn2, u16(yn2));
    }
    void overflow() {  // microcode 0x0F61
        if (p.u(pb::kLoopFlag) != 1) {
            p.set(pb::kState, 0);
            zero = true;
            return;
        }
        ps = p.u(pb::kLoopPredScale);
        if (p.u(pb::kType) != 1) {  // AX_PB_TYPE_STREAM keeps its history across the loop
            yn1 = p.s(pb::kLoopYn1);
            yn2 = p.s(pb::kLoopYn2);
        }
    }
    s16 read() {
        if (zero) return 0;
        s16 v;
        switch (format) {
        case 0x00: {  // DSP-ADPCM
            if ((cur & 15) == 0) {
                ps = u16(rd8(cur >> 1));
                cur += 2;
            } else if ((cur & 15) == 1) {
                cur += 1;
            }
            const u8 byte = u8(rd8(cur >> 1));
            int nib = (cur & 1) ? (byte & 15) : (byte >> 4);
            if (nib >= 8) nib -= 16;
            const int scale = 1 << (ps & 15);
            const int ci = (ps >> 4) & 7;
            const s64 acc = (s64(nib * scale) << 11) + 1024 + s64(coef[ci * 2]) * yn1 + s64(coef[ci * 2 + 1]) * yn2;
            v = sat16(acc >> 11);
            break;
        }
        case 0x0A:  // PCM16
            v = sat16((s64(s16(rd16(cur * 2))) * gain) >> 11);
            break;
        case 0x19:  // PCM8
            v = sat16(s64(s8(rd8(cur))) * gain);
            break;
        default:
            v = 0;
            break;
        }
        yn2 = yn1;
        yn1 = v;
        if (cur == end) {
            cur = loop;
            overflow();
        } else {
            cur++;
        }
        return v;
    }
};

// ---- Voice pipeline (microcode 0x027E PROCESS) ----

// Resamplers, into out[96] (0x0C60). History: 4 samples, oldest first, at PB +0xAC.
void srcPolyphase(Pb& p, Accel& a, s16* out) {  // srcSelect 0, microcode 0x071A
    const auto& tab = coefs().c[std::min<u16>(p.u(pb::kCoefSelect), 2)];
    const u32 ratio = p.u32at(pb::kRatio);
    s16 ring[4];
    for (int k = 0; k < 4; k++) ring[k] = p.s(pb::kLast + 2 * k);
    int w = 0;  // next write = oldest
    u64 pos = u64(p.u(pb::kFrac)) + ratio;
    for (int n = 0; n < kFrame; n++) {
        const u32 cnt = u32(pos >> 16) & 0xFFFF;
        const int phase = int((pos & 0xFFFF) >> 9);
        for (u32 i = 0; i < cnt; i++) {
            ring[w] = a.read();
            w = (w + 1) & 3;
        }
        pos = (pos & 0xFFFF) + ratio;
        s64 acc = 0;
        for (int k = 0; k < 4; k++) acc += s64(ring[(w + k) & 3]) * tab[phase][k];
        out[n] = sat16(acc >> 15);
    }
    pos -= ratio;
    p.set(pb::kFrac, u16(pos));
    for (int k = 0; k < 4; k++) p.set(pb::kLast + 2 * k, u16(ring[(w + k) & 3]));
}

void srcLinear(Pb& p, Accel& a, s16* out) {  // srcSelect 1, microcode 0x076B
    const u32 ratio = p.u32at(pb::kRatio);
    s16 ring[4];
    for (int k = 0; k < 4; k++) ring[k] = p.s(pb::kLast + 2 * k);
    int w = 0;
    u64 pos = u64(p.u(pb::kFrac)) + ratio;
    for (int n = 0; n < kFrame; n++) {
        const u32 cnt = u32(pos >> 16) & 0xFFFF;
        for (u32 i = 0; i < cnt; i++) {
            ring[w] = a.read();
            w = (w + 1) & 3;
        }
        const u32 f = u32(pos & 0xFFFF);
        // Between the two oldest samples of the history, unsigned 16-bit weights, truncated.
        if (f == 0) out[n] = ring[w];
        else out[n] = sat16((s64(ring[w]) * (0x10000 - f) + s64(ring[(w + 1) & 3]) * f) >> 16);
        pos = (pos & 0xFFFF) + ratio;
    }
    pos -= ratio;
    p.set(pb::kFrac, u16(pos));
    for (int k = 0; k < 4; k++) p.set(pb::kLast + 2 * k, u16(ring[(w + k) & 3]));
}

void srcNone(Pb& p, Accel& a, s16* out) {  // srcSelect 2, microcode 0x07C0: one input sample per output
    for (int n = 0; n < kFrame; n++) out[n] = a.read();
    for (int k = 0; k < 4; k++) p.set(pb::kLast + 2 * k, u16(out[kFrame - 4 + k]));
}

// One-pole low-pass (microcode 0x06AB): y = (a0 x + b0 y') >> 15, unsigned 16-bit a0/b0,
// truncated and saturated. Params: yn1, a0, b0. In place from `in` to `out` (may alias).
void lowPass(Pb& p, u32 at, const s16* in, s16* out, int n) {
    s16 y = p.s(at);
    const u32 a0 = p.u(at + 2), b0 = p.u(at + 4);
    for (int i = 0; i < n; i++) {
        y = sat16((s64(in[i]) * a0 + s64(y) * b0) >> 15);
        out[i] = y;
    }
    p.set(at, u16(y));
}

// Biquad (microcode 0x06C3): y = (b0 x + b1 x' + b2 x'' + a1 y' + a2 y'') >> 14, signed 2.14
// coefficients (the SDK stores a1/a2 negated), truncated and saturated.
// Params: xn1 xn2 yn1 yn2 b0 b1 b2 a1 a2.
void biquad(Pb& p, u32 at, const s16* in, s16* out, int n) {
    s16 x1 = p.s(at), x2 = p.s(at + 2), y1 = p.s(at + 4), y2 = p.s(at + 6);
    const s64 b0 = p.s(at + 8), b1 = p.s(at + 10), b2 = p.s(at + 12), a1 = p.s(at + 14), a2 = p.s(at + 16);
    for (int i = 0; i < n; i++) {
        const s16 x = in[i];
        const s16 y = sat16((b0 * x + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2) >> 14);
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        out[i] = y;
    }
    p.set(at, u16(x1));
    p.set(at + 2, u16(x2));
    p.set(at + 4, u16(y1));
    p.set(at + 6, u16(y2));
}

// Mixes n samples into a bus at a constant or ramped volume (microcode 0x0BD1 / 0x0C21 and the
// remote ones 0x0CB3 / 0x0CD0): bus += (s * v) >> 15 with v unsigned (0x8000 = 1.0). A ramp
// uses v + i*delta for sample i (16-bit wrap) and leaves v + n*delta in the PB. Returns the
// last sample's contribution, saturated: the voice's depop value for that bus.
s16 mixInto(Pb& p, u32 volAt, bool ramp, const s16* src, u16 busAddr, int n) {
    s32* b = bus(busAddr);
    const u16 v = p.u(volAt);
    s64 last = 0;
    if (!ramp) {
        for (int i = 0; i < n; i++) {
            last = s64(src[i]) * v;
            b[i] += floorDiv32768(last);
        }
    } else {
        const u16 d = p.u(volAt + 2);
        for (int i = 0; i < n; i++) {
            const u16 vi = u16(v + i * d);
            last = s64(src[i]) * vi;
            b[i] += floorDiv32768(last);
        }
        p.set(volAt, u16(v + n * d));
    }
    return sat16(last >> 15);
}

// Mixer control groups (PB +0x0C): bits 0-4 main, 16-20 aux A, 21-25 aux B, 26-31 aux C.
// In each 5-bit group: 1 L, 2 R, 4 L+R ramped (implies both), 8 S, 16 S ramped (implies S).
// Aux C's sixth bit (31) feeds its L/R from the voice before the ITD delays.
struct Group {
    u16 busL, busR, busS;
    u32 volL, volR, volS;  // PB offsets of the volume (delta follows)
    u32 dpL, dpR, dpS;     // PB offsets of the depop values
};
constexpr Group kGroups[4] = {
    {kMainL, kMainR, kMainS, 0x14, 0x18, 0x34, 0x52, 0x5A, 0x62},
    {kAuxAL, kAuxAR, kAuxAS, 0x1C, 0x20, 0x38, 0x54, 0x5C, 0x64},
    {kAuxBL, kAuxBR, kAuxBS, 0x24, 0x28, 0x3C, 0x56, 0x5E, 0x66},
    {kAuxCL, kAuxCR, kAuxCS, 0x2C, 0x30, 0x40, 0x58, 0x60, 0x68},
};

void mixGroup(Pb& p, int g, u32 bits, const s16* srcL, const s16* srcR, const s16* srcS) {
    const Group& G = kGroups[g];
    const bool lr = bits & 4;
    if ((bits & 1) || lr) p.set(G.dpL, u16(mixInto(p, G.volL, lr, srcL, G.busL, kFrame)));
    if ((bits & 2) || lr) p.set(G.dpR, u16(mixInto(p, G.volR, lr, srcR, G.busR, kFrame)));
    if (bits & 0x18) p.set(G.dpS, u16(mixInto(p, G.volS, bits & 0x10, srcS, G.busS, kFrame)));
}

struct VoiceStats {
    u32 voices = 0, active = 0, starts = 0, lpf = 0, biquad = 0, remote = 0, itd = 0, aux = 0;
    u32 kind[4] = {};  // active voice-frames by VoiceKind
} sVoiceStats;

// ---- Volume sliders: the NW4R sound that owns an AX voice (docs/AUDIO.md "Volume sliders") ----
// AX keeps its voice tables in SDA globals: the DSP PBs (__AXGetPBs: `lwz r3, d(r13)`), then
// the ITD buffers and the CPU-side AXVPBs (0x168 bytes each) 4 and 8 bytes after it (the order
// __AXVPBInitCommon clears them in). An AXVPB's user context (+0x14, AXAcquireVoice's third
// argument) is NW4R's AxVoice when its callback (AxVoice +0x38) is Voice::AxVoiceCallbackFunc;
// AxVoice +0x3C is the Voice, whose callback / data (+0x94 / +0x98, Voice::InitParam) name the
// owner: a Channel (Channel::VoiceCallbackFunc) or a StrmPlayer. A Channel's callback / data
// (+0xE0 / +0xE4, Channel::InitParam) are a SeqTrack (its SeqPlayer at +0xC0) or a WsdPlayer.
// Seq/Strm/Wave sounds hold their player at +0x10C (GetBasicPlayer) and their SoundPlayer at
// +0x10 (BasicSound::AttachSoundPlayer), whose index in the SoundArchivePlayer's array is the
// player id of rv_forest.brsar (same 29 players in the USA, PAL and Japanese archives).
u32 sSda = 0;
struct SoundPlayers {
    u32 base = 0, count = 0, stride = 0;
} sPlayers;

struct Nw4rAddrs {
    u32 axVoiceCb = 0, chanVoiceCb = 0, strmVoiceCb = 0, seqTrackCb = 0, wsdCb = 0;
    s32 pbOff = 0;
    bool ok = false;
};
const Nw4rAddrs& nw4r() {
    static const Nw4rAddrs a = [] {
        Nw4rAddrs r;
        r.axVoiceCb = guestSymbol("AxVoiceCallbackFunc__Q44nw4r3snd6detail5VoiceFPQ44nw4r3snd6detail7AxVoiceQ54nw4r3snd6detail7AxVoice21AxVoiceCallbackStatusPv");
        r.chanVoiceCb = guestSymbol("VoiceCallbackFunc__Q44nw4r3snd6detail7ChannelFPQ44nw4r3snd6detail5VoiceQ54nw4r3snd6detail5Voice19VoiceCallbackStatusPv");
        r.strmVoiceCb = guestSymbol("VoiceCallbackFunc__Q44nw4r3snd6detail10StrmPlayerFPQ44nw4r3snd6detail5VoiceQ54nw4r3snd6detail5Voice19VoiceCallbackStatusPv");
        r.seqTrackCb = guestSymbol("ChannelCallbackFunc__Q44nw4r3snd6detail8SeqTrackFPQ44nw4r3snd6detail7ChannelQ54nw4r3snd6detail7Channel21ChannelCallbackStatusUl");
        r.wsdCb = guestSymbol("ChannelCallbackFunc__Q44nw4r3snd6detail9WsdPlayerFPQ44nw4r3snd6detail7ChannelQ54nw4r3snd6detail7Channel21ChannelCallbackStatusUl");
        const u32 insn = rd32(guestSymbol("__AXGetPBs"));
        if ((insn & 0xFFFF0000u) == 0x806D0000u) {  // lwz r3, d(r13)
            r.pbOff = s16(insn & 0xFFFF);
            r.ok = true;
        } else {
            log("[ax] __AXGetPBs is not lwz r3,d(r13) (%08X): volume sliders apply to every voice", insn);
        }
        return r;
    }();
    return a;
}

inline bool guestPtr(u32 a) {
    return ((a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x94000000u)) && (a & 3) == 0;
}

// Sound archive player ids (rv_forest.brsar) by kind. Music: PLAYER_STRM (21, the title
// stream), PLAYER_MUSIC (22, K.K. songs / records), PLAYER_CONDUCTOR (23), PLAYER_FANFARE (24,
// jingles), PLAYER_BGM (25, the background music). Voices: PLAYER_DOUBUTSUGO (15, animal
// speech) and PLAYER_KEYBOARD (16, the per-letter speech sounds of every language). Everything
// else (system, walk, furniture, action, environment, town tune melodies, gyroids, TV) is a
// sound effect.
VoiceKind kindOfPlayer(u32 id) {
    switch (id) {
    case 21: case 22: case 23: case 24: case 25: return VoiceKind::Music;
    case 15: case 16: return VoiceKind::Voice;
    default: return VoiceKind::Effect;
    }
}

// The kind of the voice whose DSP PB is at (physical) pbAddr, and its player id (-1 unknown).
// CF_AUDIO_KIND_LOG=1: why a voice could not be classified (the chain step that failed).
const bool sKindLog = [] {
    const char* e = std::getenv("CF_AUDIO_KIND_LOG");
    return e && *e && *e != '0';
}();

VoiceKind classify(u32 pbAddr, int& player, bool logFail) {
    player = -1;
    const auto& a = nw4r();
    auto fail = [&](const char* why, u32 v0, u32 v1) {
        if (logFail && sKindLog) log("[ax] voice %08X: kind unknown, %s (%08X %08X)", pbAddr, why, v0, v1);
        return VoiceKind::Unknown;
    };
    if (!a.ok || !sSda) return fail("no AX tables", sSda, u32(a.pbOff));
    const u32 pbBase = rd32(sSda + a.pbOff), vpbBase = rd32(sSda + a.pbOff + 8);
    const u32 off = (pbAddr & 0x1FFFFFFFu) - (pbBase & 0x1FFFFFFFu);
    if (!guestPtr(vpbBase) || off % kPbSize || off / kPbSize >= 256) return fail("PB not in AX's table", pbBase, vpbBase);
    const u32 vpb = vpbBase + off / kPbSize * 0x168;
    const u32 axv = rd32(vpb + 0x14);
    if (!guestPtr(axv) || rd32(axv + 0x38) != a.axVoiceCb)  // not an NW4R voice (HOME Menu, ...)
        return fail("AXVPB context is no NW4R AxVoice", axv, guestPtr(axv) ? rd32(axv + 0x38) : 0);
    const u32 voice = rd32(axv + 0x3C);
    if (!guestPtr(voice)) return fail("AxVoice has no Voice", axv, voice);
    const u32 vcb = rd32(voice + 0x94), vdata = rd32(voice + 0x98);
    if (!guestPtr(vdata)) return fail("Voice has no owner", vcb, vdata);
    u32 bp = 0;  // the sound's BasicPlayer
    auto inPlayers = [](u32 sp) {
        return sp >= sPlayers.base && (sp - sPlayers.base) % sPlayers.stride == 0 &&
               (sp - sPlayers.base) / sPlayers.stride < sPlayers.count;
    };
    if (vcb == a.strmVoiceCb) {
        // A stream voice's data is its track (StrmPlayer::AllocVoices: 0x38-byte tracks from
        // StrmPlayer +0xB78, the track count at +0x82C): the track whose player belongs to a
        // sound of the archive.
        for (u32 k = 0; k < 8 && !bp && sPlayers.stride; k++) {
            const u32 cand = vdata - 0xB78 - k * 0x38;
            if (guestPtr(cand) && rd32(cand + 0x82C) > k && inPlayers(rd32(cand - 0x10C + 0x10))) bp = cand;
        }
    } else if (vcb == a.chanVoiceCb) {
        const u32 ccb = rd32(vdata + 0xE0), cdata = rd32(vdata + 0xE4);
        if (!guestPtr(cdata)) return fail("Channel has no owner", ccb, cdata);
        if (ccb == a.seqTrackCb) bp = rd32(cdata + 0xC0);
        else if (ccb == a.wsdCb) bp = cdata;
        else return fail("Channel owner unknown", ccb, cdata);
    } else {
        return fail("Voice owner unknown", vcb, vdata);
    }
    if (!guestPtr(bp) || !sPlayers.stride) return fail("no player", bp, sPlayers.base);
    const u32 sp = rd32(bp - 0x10C + 0x10);
    if (!inPlayers(sp))
        return fail("sound's SoundPlayer not in the archive player's array", bp, sp);
    player = int((sp - sPlayers.base) / sPlayers.stride);
    return kindOfPlayer(u32(player));
}

// Slider gain of a kind (unknown voices count as sound effects).
float kindGain(VoiceKind k) {
    const auto& as = port::settings().audio;
    const float g = k == VoiceKind::Music ? as.musicVolume : k == VoiceKind::Voice ? as.voiceVolume : as.sfxVolume;
    return std::clamp(g, 0.0f, 1.0f);
}
const char* kindName(VoiceKind k) {
    return k == VoiceKind::Music ? "music" : k == VoiceKind::Voice ? "voice" : k == VoiceKind::Effect ? "effect" : "unknown";
}

// Per voice slot: the slider gain applied last frame (Q15), ramped to the new one over a frame
// so a slider move does not click; a voice that starts takes its gain at once.
u16 sVoiceGain[256];
bool sVoiceOn[256];
VoiceKind sVoiceKind[256];
int sVoicePlayer[256];

// ---- Extras per AI block (surround side data, Remote speakers) ----
FrameExtras sPending;  // built during the list, filed under the OUTPUT command's AI buffer
s32 sFoldA[kFrame], sFoldB[kFrame];
int sFoldCmd = 0;
std::atomic<int> sAxMode{0};
struct ExtrasSlot {
    u32 ai = 0;
    FrameExtras x;
};
std::mutex sExtrasM;
ExtrasSlot sExtras[4];
u32 sExtrasNext = 0;


// CF_AUDIO_LOG: one line per voice start (the first 400), so the source types, rates and
// mixer settings the game uses can be checked against the PB layout.
void logVoiceStart(u32 addr, const Pb& p) {
    static int sCount = 0;
    static const int sLimit = [] {  // CF_AUDIO_LOG=2: every voice start
        const char* e = std::getenv("CF_AUDIO_LOG");
        return e && *e == '2' ? 1 << 30 : 400;
    }();
    if (++sCount > sLimit) return;
    const u32 ratio = p.u32at(pb::kRatio);
    log("[ax] voice %08X start: fmt %02X src %u/%u ratio %.4f loop %u type %u addr %08X..%08X loop %08X "
        "mix %08X L %04X R %04X S %04X A %04X/%04X B %04X/%04X ve %04X%+d lpf %u biq %u itd %u rmt %u/%04X; %s, player %d",
        addr, p.u(pb::kFormat), p.u(pb::kSrcSelect), p.u(pb::kCoefSelect), ratio / 65536.0, p.u(pb::kLoopFlag),
        p.u(pb::kType), p.u32at(pb::kCurAddr), p.u32at(pb::kEndAddr), p.u32at(pb::kLoopAddr), p.u32at(pb::kMixerCtrl),
        p.u(0x14), p.u(0x18), p.u(0x34), p.u(0x1C), p.u(0x20), p.u(0x24), p.u(0x28), p.u(pb::kVeVol),
        int(p.s(pb::kVeDelta)), p.u(pb::kLpfOn), p.u(pb::kBiqOn), p.u(pb::kItdOn), p.u(pb::kRmtOn),
        p.u(pb::kRmtMixerCtrl), kindName(sVoiceKind[(addr / kPbSize) & 255]), sVoicePlayer[(addr / kPbSize) & 255]);
}

// CF_AX_TRACE=<PB address>: that voice's state at the start of each frame and its first and
// last output samples (frame-to-frame continuity checks).
u32 traceAddr() {
    static const u32 v = [] {
        const char* e = std::getenv("CF_AX_TRACE");
        return e && *e ? u32(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    return v;
}

void processVoice(Pb& p, u32 addr) {
    const auto& as = port::settings().audio;
    sVoiceStats.voices++;
    const bool trace = addr == traceAddr() && p.u(pb::kState) == 1;
    if (trace)
        log("[ax] trace %08X: cur %08X ps %02X yn %d %d frac %04X last %d %d %d %d ve %04X%+d vL %04X%+d vR %04X%+d",
            addr, p.u32at(pb::kCurAddr), p.u(pb::kPredScale), p.s(pb::kYn1), p.s(pb::kYn2), p.u(pb::kFrac),
            p.s(pb::kLast), p.s(pb::kLast + 2), p.s(pb::kLast + 4), p.s(pb::kLast + 6), p.u(pb::kVeVol),
            int(p.s(pb::kVeDelta)), p.u(0x14), int(p.s(0x16)), p.u(0x18), int(p.s(0x1A)));
    // Volume sliders: the voice's kind, looked up each frame (a slot changes owner when NW4R
    // reuses it), and its gain, ramped from last frame's.
    const u32 slot = (addr / kPbSize) & 255;
    const bool on = p.u(pb::kState) == 1;
    const bool started = on && !sVoiceOn[slot];
    sVoiceOn[slot] = on;
    u16 gainFrom = 0x8000, gainTo = 0x8000;
    if (on) {
        int player;
        VoiceKind kind = classify(addr, player, started);
        // NW4R lets go of a voice before its release has died away (the Voice is freed, the AX
        // voice still plays out): it keeps the kind it had.
        if (kind == VoiceKind::Unknown && !started) {
            kind = sVoiceKind[slot];
            player = sVoicePlayer[slot];
        }
        sVoiceKind[slot] = kind;
        sVoicePlayer[slot] = player;
        gainTo = u16(std::lround(kindGain(kind) * 32768.0f));
        gainFrom = started ? gainTo : sVoiceGain[slot];
        sVoiceGain[slot] = gainTo;
        sVoiceStats.kind[int(kind)]++;
    }
    if (g_log) {
        if (started) {
            sVoiceStats.starts++;
            logVoiceStart(addr, p);
        }
        if (on) {
            sVoiceStats.lpf += p.u(pb::kLpfOn) != 0;
            sVoiceStats.biquad += p.u(pb::kBiqOn) != 0;
            sVoiceStats.remote += p.u(pb::kRmtOn) != 0;
            sVoiceStats.itd += p.u(pb::kItdOn) != 0;
            sVoiceStats.aux += (p.u32at(pb::kMixerCtrl) & 0xFFFF0000u) != 0;
        }
    }
    s16* hist = sVoice;      // 0x0C40: ITD history
    s16* v = sVoice + 32;    // 0x0C60: the voice's 96 samples
    const bool itd = p.u(pb::kItdOn) != 0;
    const u32 itdBuf = p.u32at(pb::kItdBuf);
    if (p.u(pb::kState) == 1) {
        sVoiceStats.active++;
        const s16* srcL = v;
        const s16* srcR = v;
        if (itd) {
            for (int i = 0; i < 32; i++) hist[i] = s16(g16(itdBuf + 2 * i));
            srcL = sVoice + std::min<u16>(p.u(pb::kItdShiftL), 64);
            srcR = sVoice + std::min<u16>(p.u(pb::kItdShiftR), 64);
        }
        // Accelerator + resampler.
        Accel a(p);
        switch (p.u(pb::kSrcSelect)) {
        case 0: srcPolyphase(p, a, v); break;
        case 1: srcLinear(p, a, v); break;
        default: srcNone(p, a, v); break;
        }
        a.store();
        // Volume envelope (0x034C): v + i*delta per sample, 16-bit wrap; the PB keeps v + 96*delta.
        {
            const u16 ve = p.u(pb::kVeVol), d = p.u(pb::kVeDelta);
            for (int i = 0; i < kFrame; i++) v[i] = sat16((s64(v[i]) * u16(ve + i * d)) >> 15);
            p.set(pb::kVeVol, u16(ve + kFrame * d));
        }
        if (trace)
            log("[ax] trace %08X: out %d %d %d .. %d %d %d", addr, v[0], v[1], v[2], v[93], v[94], v[95]);
        if (as.voiceFilters) {
            if (p.u(pb::kLpfOn)) lowPass(p, pb::kLpfYn1, v, v, kFrame);
            if (p.u(pb::kBiqOn)) biquad(p, pb::kBiqXn1, v, v, kFrame);
        }
        // F1 > Sound music / effects / voices volume (not the console's: default 1.0 leaves the
        // mix bit-exact). Before the mixer, so the aux (reverb), remote and depop values follow.
        if (gainFrom != 0x8000 || gainTo != 0x8000) {
            const s32 d = s32(gainTo) - s32(gainFrom);
            for (int i = 0; i < kFrame; i++) v[i] = s16((s32(v[i]) * (s32(gainFrom) + d * (i + 1) / kFrame)) >> 15);
        }
        // Mixer.
        for (u32 o = pb::kDpop; o < pb::kDpop + 24; o += 2) p.set(o, 0);
        for (u32 o = pb::kRmtDpop; o < pb::kRmtDpop + 16; o += 2) p.set(o, 0);
        const u32 ctrl = p.u32at(pb::kMixerCtrl);
        mixGroup(p, 0, ctrl & 0x1F, srcL, srcR, v);
        mixGroup(p, 1, (ctrl >> 16) & 0x1F, srcL, srcR, v);
        mixGroup(p, 2, (ctrl >> 21) & 0x1F, srcL, srcR, v);
        const bool cPre = ctrl & 0x80000000u;
        mixGroup(p, 3, (ctrl >> 26) & 0x1F, cPre ? v : srcL, cPre ? v : srcR, v);
        // Wii Remote speaker (0x03A2): optional low-pass / biquad, then 32 kHz -> 6 kHz with
        // coefficient table 0 (a fixed step of 0x55555), then up to 8 sends.
        if (p.u(pb::kRmtOn)) {
            s16 r[4 + kFrame];
            for (int k = 0; k < 4; k++) r[k] = p.s(pb::kRmtLast + 2 * k);
            const u16 iir = p.u(pb::kRmtIir);
            if (as.voiceFilters && iir == 2) biquad(p, pb::kRmtIir + 2, v, r + 4, kFrame);
            else if (as.voiceFilters && iir) lowPass(p, pb::kRmtIir + 2, v, r + 4, kFrame);
            else std::copy(v, v + kFrame, r + 4);
            const auto& tab = coefs().c[0];
            u32 pos = p.u(pb::kRmtFrac);
            s16 ro[kRmtFrame];
            u32 i = 0;
            for (int n = 0; n < kRmtFrame; n++) {
                pos += 0x00055555;
                i = std::min<u32>(pos >> 16, kFrame);
                const int phase = int((pos & 0xFFFF) >> 9);
                s64 acc = 0;
                for (int k = 0; k < 4; k++) acc += s64(r[i + k]) * tab[phase][k];
                ro[n] = sat16(acc >> 15);
            }
            for (int k = 0; k < 4; k++) p.set(pb::kRmtLast + 2 * k, u16(r[i + k]));
            p.set(pb::kRmtFrac, u16(pos));
            const u16 rc = p.u(pb::kRmtMixerCtrl);
            for (int s = 0; s < 8; s++) {
                const u32 mode = (rc >> (2 * s)) & 3;
                if (!mode) continue;
                const int remote = s >> 1;
                const bool aux = s & 1;
                const u16 dst = aux ? kRmtAux[remote] : kRmtMain[remote];
                const s16 dp = mixInto(p, pb::kRmtMix + 4 * s, mode >= 2, ro, dst, kRmtFrame);
                p.set(pb::kRmtDpop + 2 * (aux ? 4 + remote : remote), u16(dp));
            }
        }
        // ITD delays move one sample per frame towards their targets.
        if (itd) {
            for (u32 o : {u32(pb::kItdShiftL), u32(pb::kItdShiftR)}) {
                const s16 cur = p.s(o), tgt = p.s(o + 4);
                if (cur > tgt) p.set(o, u16(cur - 1));
                else if (cur < tgt) p.set(o, u16(cur + 1));
            }
        }
    }
    if (itd)  // the newest 32 samples become the next frame's history
        for (int i = 0; i < 32; i++) wr16(itdBuf + 2 * i, u16(v[kFrame - 32 + i]));
}

// ---- Commands ----

// Volume ramp of the aux returns and the master volume (microcode 0x04E6 / 0x061B): 96 steps
// from the last frame's volume to the new one in 1/96 increments (multipliers 0x2AB / 0x2AA
// alternating, so the 96th step lands exactly on the new value).
void volRamp(u16 from, u16 to, u16* out) {
    // The microcode's pipelined multiply-accumulate, step by step: each MULCAC stores the
    // accumulator, adds the previous product and forms the next one (0x2AB or 0x2AA * diff).
    const s64 d = s16(u16(to - from));
    s64 acc = s64(s16(from)) * 65536;
    s64 prod = d * 0x2AB;  // 0x04F1 MULC
    acc += prod;           // 0x04F2 MULCAC
    prod = d * 0x2AB;
    int n = 0;
    auto step = [&](s64 next) {
        out[n++] = u16(u64(acc) >> 16);
        acc += prod;
        prod = d * next;
    };
    for (int it = 0; it < 31; it++) {  // 0x04F5..0x04F7
        step(0x2AA);
        step(0x2AB);
        step(0x2AB);
    }
    step(0x2AA);                        // 0x04F8
    out[n++] = u16(u64(acc) >> 16);     // 0x04F9 ADDP (its add is not stored)
    out[n] = to;                        // 96th entry: the new volume itself
}

// Adds `n` 32-bit samples from guest memory (big-endian) times a per-sample volume to a bus.
void mixReturn(u16 dstBus, const s32* src, const u16* vol, int n) {
    s32* b = bus(dstBus);
    for (int i = 0; i < n; i++) b[i] += floorDiv32768(s64(src[i]) * vol[i]);
}

void uploadBus(u16 from, u32 guest, int samples) {
    const s32* b = bus(from);
    for (int i = 0; i < samples; i++) wr32(guest + 4 * i, u32(b[i]));
}
void downloadBus(u16 to, u32 guest, int samples) {
    s32* b = bus(to);
    for (int i = 0; i < samples; i++) b[i] = g32(guest + 4 * i);
}

// Aux bus round trip (commands 5/6/7, microcode 0x049B): the aux bus goes to `in` for the
// CPU's aux callback (AXFX reverb, chorus, delay run there as game code), and what the CPU
// left in `out` (a frame processed earlier: AX rotates three buffers) comes back and is mixed
// into the main buses at the ramped return volume.
void auxMix(int aux, u16 vol, u32 in, u32 out) {
    static const u16 kBase[3] = {kAuxAL, kAuxBL, kAuxCL};
    const u16 base = kBase[aux];
    u16 ramp[kFrame];
    volRamp(sPrevAuxVol[aux], vol, ramp);
    sPrevAuxVol[aux] = vol;
    uploadBus(base, in, 3 * kFrame);
    downloadBus(base, out, 3 * kFrame);
    if (!port::settings().audio.reverb) return;
    mixReturn(kMainL, bus(base), ramp, kFrame);
    mixReturn(kMainR, bus(base + 0xC0), ramp, kFrame);
    mixReturn(kMainS, bus(base + 0x180), ramp, kFrame);
}

// Dolby Pro Logic II aux round trip (commands 8/9, microcode 0x0DBD): L/R/Ls up together, Rs
// (the aux C bus slot) separately; four returns into main L, R, Ls and Rs.
void auxMixDpl2(int aux, u16 vol, u32 in, u32 inRs, const u32* out) {
    const u16 base = aux ? kAuxBL : kAuxAL;
    const u16 rs = aux ? kAuxCS : kAuxCR;
    u16 ramp[kFrame];
    volRamp(sPrevAuxVol[aux], vol, ramp);
    sPrevAuxVol[aux] = vol;
    uploadBus(base, in, 3 * kFrame);
    uploadBus(rs, inRs, kFrame);
    static const u16 kDst[4] = {kMainL, kMainR, kMainS, kAuxCL};
    for (int c = 0; c < 4; c++) {
        downloadBus(base, out[c], kFrame);
        if (port::settings().audio.reverb) mixReturn(kDst[c], bus(base), ramp, kFrame);
    }
}

// Compressor (command 10, microcode 0x057B): when main L or R reaches `threshold` this frame
// the release counter restarts at `frames` and the frame is scaled by gain ramp
// table[old counter]; otherwise each frame steps the counter down with release ramp
// table[0x840 + counter * 0xC0]. Each ramp is 96 unsigned 1.15 gains in guest memory.
void compressor(u16 threshold, u16 frames, u32 table) {
    bool clip = false;
    for (u16 b : {u16(kMainL), u16(kMainR)})
        for (int i = 0; i < kFrame; i++)
            if (std::llabs(s64(bus(b)[i])) >= threshold) clip = true;
    u32 gains;
    if (clip) {
        gains = table + sCompCount * 0xC0;
        sCompCount = frames;
    } else {
        if (sCompCount == 0) return;
        sCompCount--;
        gains = table + 0x840 + sCompCount * 0xC0;
    }
    for (u16 b : {u16(kMainL), u16(kMainR)})
        for (int i = 0; i < kFrame; i++) bus(b)[i] = floorDiv32768(s64(bus(b)[i]) * g16(gains + 2 * i));
}

struct Levels {  // CF_AUDIO_LOG: output peaks / clipping per ~1 s
    u32 frames = 0, clipped = 0;
    int peakL = 0, peakR = 0;
    double sumSq = 0;
} sLevels;

// Final output (commands 11/12, microcode 0x060B / 0x0EC6): the surround bus (DPL2: Ls and
// Rs) goes to `sAddr` for the next frame's ADD_TO_LR (a one-frame surround delay), then main
// L/R at the ramped master volume, saturated to 16 bits, into the AI buffer: right first
// (the console's DAC plays the first sample of each stereo frame on the right).
void output(u16 vol, u32 sAddr, u32 aiAddr, bool dpl2) {
    uploadBus(kMainS, sAddr, kFrame);
    if (dpl2) uploadBus(kAuxCL, sAddr + 0x180, kFrame);
    u16 ramp[kFrame];
    volRamp(sPrevMaster, vol, ramp);
    sPrevMaster = vol;
    const s32* L = bus(kMainL);
    const s32* R = bus(kMainR);
    // Discrete surround side data (audio_out.cpp): the surround this frame's fold added, at the
    // output level.
    sPending.hasSide = sFoldCmd == 2 || sFoldCmd == 3;
    if (sPending.hasSide) {
        for (int i = 0; i < kFrame; i++) {
            const float k = ramp[i] / 32768.0f;
            SideSample& sd = sPending.side[i];
            sd.sl = sFoldA[i] * k;
            sd.sr = (sFoldCmd == 2 ? sFoldA[i] : sFoldB[i]) * k;
            sd.lExtra = sFoldCmd == 2 ? -sd.sl : sd.sl;
            sd.rExtra = sFoldCmd == 2 ? sd.sr : -sd.sr;
        }
    }
    {
        std::lock_guard lk(sExtrasM);
        ExtrasSlot* slotp = nullptr;
        for (auto& x : sExtras)
            if (x.ai == aiAddr) slotp = &x;
        if (!slotp) {
            slotp = &sExtras[sExtrasNext];
            sExtrasNext = (sExtrasNext + 1) % 4;
        }
        slotp->ai = aiAddr;
        slotp->x = sPending;
    }
    sFoldCmd = 0;
    for (int i = 0; i < kFrame; i++) {
        const s16 r = sat16(floorDiv32768(s64(R[i]) * ramp[i]));
        const s16 l = sat16(floorDiv32768(s64(L[i]) * ramp[i]));
        wr16(aiAddr + 4 * i, u16(r));
        wr16(aiAddr + 4 * i + 2, u16(l));
        if (g_log) {
            sLevels.peakL = std::max(sLevels.peakL, std::abs(int(l)));
            sLevels.peakR = std::max(sLevels.peakR, std::abs(int(r)));
            if (l == 32767 || l == -32768 || r == 32767 || r == -32768) sLevels.clipped++;
            sLevels.sumSq += double(l) * l + double(r) * r;
        }
    }
}

// Wii Remote speakers (command 13, microcode 0x067C): each remote's main bus, saturated to
// 16 bits, 18 samples into its ring at the address AX passes (AXRmtGetSamples reads them).
// The remote aux buses are not output by this microcode.
s16 sRmtOut[4][kRmtFrame];
void wmOutput(const u32* addr) {
    for (int r = 0; r < 4; r++) {
        const s32* b = bus(kRmtMain[r]);
        for (int i = 0; i < kRmtFrame; i++) {
            sRmtOut[r][i] = sat16(b[i]);
            wr16(addr[r] + 2 * i, u16(sRmtOut[r][i]));
        }
    }
}

// SETUP (command 0, microcode 0x008A): every bus starts the frame with the depop ramp the CPU
// put in the studio block (s32 start + s16 step per sample, from the voices that stopped
// last frame) or silence.
void setup(u32 studio) {
    static const u16 kMain[12] = {kMainL, kMainR, kMainS, kAuxAL, kAuxAR, kAuxAS,
                                  kAuxBL, kAuxBR, kAuxBS, kAuxCL, kAuxCR, kAuxCS};
    auto ramp = [](u32 at, u16 dst, int n) {
        s32 v = g32(at);
        const s32 d = s16(g16(at + 4));
        s32* b = bus(dst);
        if (v == 0) {
            std::fill(b, b + n, 0);
            return;
        }
        for (int i = 0; i < n; i++, v += d) b[i] = v;
    };
    for (int i = 0; i < 12; i++) ramp(studio + 6 * i, kMain[i], kFrame);
    for (int r = 0; r < 4; r++) {
        ramp(studio + 0x48 + 0xC * r, kRmtMain[r], kRmtFrame);
        ramp(studio + 0x4E + 0xC * r, kRmtAux[r], kRmtFrame);
    }
}

// ADD_TO_LR / SUB_TO_LR / ADD_SUB_TO_LR (commands 1-3): last frame's surround (from OUTPUT)
// folded into L/R: stereo L+S R+S, surround L-S R+S, DPL2 L+Ls R-Rs.
void foldSurround(int cmd, u32 addr) {
    s32* L = bus(kMainL);
    s32* R = bus(kMainR);
    sFoldCmd = cmd;
    sAxMode.store(cmd - 1, std::memory_order_relaxed);
    for (int i = 0; i < kFrame; i++) {
        const s32 s = g32(addr + 4 * i);
        sFoldA[i] = s;
        if (cmd == 1) L[i] += s, R[i] += s;
        else if (cmd == 2) L[i] -= s, R[i] += s;
        else R[i] -= (sFoldB[i] = g32(addr + 0x180 + 4 * i)), L[i] += s;
    }
}

void processVoices(u32 pbAddr) {
    int guard = 0;
    while (pbAddr && guard++ < 256) {
        Pb p;
        for (u32 i = 0; i < kPbSize; i += 4) {
            const u32 w = rd32(pbAddr + i);
            p.b[i] = u8(w >> 24), p.b[i + 1] = u8(w >> 16), p.b[i + 2] = u8(w >> 8), p.b[i + 3] = u8(w);
        }
        processVoice(p, pbAddr);
        const u32 back = p.u32at(pb::kCurr);
        for (u32 i = 0; i < kPbSize; i += 4)
            wr32(back + i, u32(p.b[i]) << 24 | u32(p.b[i + 1]) << 16 | u32(p.b[i + 2]) << 8 | p.b[i + 3]);
        pbAddr = p.u32at(pb::kNext);
    }
}

void logFrame() {
    if (!g_log) return;
    sLevels.frames++;
    if (sLevels.frames < 333) return;  // ~1 s
    const double rms = std::sqrt(sLevels.sumSq / (2.0 * kFrame * sLevels.frames));
    const double f = sLevels.frames;
    log("[ax] frame %llu (guest frame %u): %.1f playing (%u starts; per frame lpf %.1f biquad %.1f remote %.1f "
        "itd %.1f aux %.1f; music %.1f effect %.1f voice %.1f unknown %.1f), peak L %d R %d, rms %.0f, %u clipped "
        "samples, compressor %u, mode %d",
        (unsigned long long)sFrames, copyDispCount(), sVoiceStats.active / f, sVoiceStats.starts, sVoiceStats.lpf / f,
        sVoiceStats.biquad / f, sVoiceStats.remote / f, sVoiceStats.itd / f, sVoiceStats.aux / f,
        sVoiceStats.kind[int(VoiceKind::Music)] / f, sVoiceStats.kind[int(VoiceKind::Effect)] / f,
        sVoiceStats.kind[int(VoiceKind::Voice)] / f, sVoiceStats.kind[int(VoiceKind::Unknown)] / f, sLevels.peakL,
        sLevels.peakR, rms, sLevels.clipped, sCompCount, axMode());
    sLevels = {};
    sVoiceStats = {};
}

}  // namespace

// One AX command list (__AXNextFrame builds them; __AXOutNewFrame mails 0xBABE<len> + address).
void axCommandList(u32 addr, u32 bytes) {
    sFrameBusy.store(true, std::memory_order_release);
    static const bool sOff = [] {  // CF_AX=0: lists "finish" without mixing (the boot stand-in), A/B aid
        const char* e = std::getenv("CF_AX");
        return e && *e == '0';
    }();
    if (sOff) return;
    const u32 words = std::min<u32>(bytes / 2, 0x40);
    u32 i = 0;
    auto w = [&]() -> u16 { return i < words ? g16(addr + 2 * i++) : (i++, u16(14)); };
    auto a32 = [&]() -> u32 {
        const u32 hi = w();
        return hi << 16 | w();
    };
    sFoldCmd = 0;
    static int sListsLogged = 0;
    const bool logList = g_log && sListsLogged < 2;
    u32 cmdMask = 0;  // CF_AUDIO_LOG: the command set (aux effects registered, mode) when it changes
    for (;;) {
        const u16 cmd = w();
        if (logList) log("[ax] list %08X: command %u", addr, cmd);
        cmdMask |= 1u << (cmd & 31);
        switch (cmd) {
        case 0: setup(a32()); break;
        case 1: case 2: case 3: foldSurround(cmd, a32()); break;
        case 4: processVoices(a32()); break;
        case 5: case 6: case 7: {
            const u16 vol = w();
            const u32 in = a32(), out = a32();
            auxMix(cmd - 5, vol, in, out);
            break;
        }
        case 8: case 9: {
            const u16 vol = w();
            const u32 in = a32(), inRs = a32();
            u32 out[4];
            for (auto& o : out) o = a32();
            auxMixDpl2(cmd - 8, vol, in, inRs, out);
            break;
        }
        case 10: {
            const u16 thr = w(), frames = w();
            compressor(thr, frames, a32());
            break;
        }
        case 11: case 12: {
            const u16 vol = w();
            const u32 s = a32(), ai = a32();
            output(vol, s, ai, cmd == 12);
            break;
        }
        case 13: {
            u32 r[4];
            for (auto& x : r) x = a32();
            wmOutput(r);
            break;
        }
        case 14:
            goto done;
        default:  // the microcode mails 0xBAAD<cmd> and halts
            log("[ax] unknown command %u at %08X+%u: list abandoned", cmd, addr, (i - 1) * 2);
            goto done;
        }
    }
done:
    if (logList) sListsLogged++;
    static u32 sLastMask = 0;
    if (g_log && cmdMask != sLastMask) {
        log("[ax] command set now %08X (guest frame %u): aux A %s, B %s, C %s, mode %d", cmdMask, copyDispCount(),
            cmdMask & (1u << 5 | 1u << 8) ? "on" : "off", cmdMask & (1u << 6 | 1u << 9) ? "on" : "off",
            cmdMask & (1u << 7) ? "on" : "off", axMode());
        sLastMask = cmdMask;
    }
    sFrames++;
    logFrame();
}

// The resume callback (the DSP yielded after the list) has been delivered to the CPU.
void axFrameDone() {
    sFrameEnd.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
    sFrameBusy.store(false, std::memory_order_release);
}

bool dspFrameBusy() { return sFrameBusy.load(std::memory_order_acquire); }
std::chrono::steady_clock::time_point dspFrameEnd() {
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(sFrameEnd.load(std::memory_order_acquire)));
}

bool dspFrameExtras(u32 aiAddr, FrameExtras& out) {
    std::lock_guard lk(sExtrasM);
    for (const auto& x : sExtras)
        if (x.ai == aiAddr && aiAddr) {
            out = x.x;
            return true;
        }
    return false;
}

int axMode() { return sAxMode.load(std::memory_order_relaxed); }

void axSetSdaBase(u32 r13) { sSda = r13; }

void noteSoundPlayers(u32 base, u32 count, u32 stride) {
    sPlayers = {base, count, stride};
    log("[ax] NW4R sound players: %u at %08X (volume sliders by player id)", count, base);
}

const s16* axRemoteFrame(int remote) { return sRmtOut[remote & 3]; }

}  // namespace recomp::audio
