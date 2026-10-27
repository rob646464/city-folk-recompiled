// Stand-in for the audio DSP microcode that JAudio (jaudio/dspdriver.c,
// dspinterface.c) drives. The DSP itself is never emulated: we take the
// command packets JAudio sends through DSPSendCommands, read the per-voice
// parameter blocks the CPU writes into main memory, and mix the voices on the
// host the way the microcode does.
//
// The behaviour below follows the game's own microcode (the `jdsp` image in the
// DOL, 0x8038F340, read with tools/dsp_disasm.py): the same
// mixing buffers at the same DSP addresses, the same per-subframe order, the
// same fixed-point arithmetic. docs/AUDIO.md walks through it with the
// microcode addresses.
#include "port/settings.h"
#include "recomp/audio.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp: guest frame number (CF_AUDIO_LOG time stamps)
}

namespace recomp::audio {
bool g_log = [] {
    const char* e = std::getenv("CF_AUDIO_LOG");
    return e && *e && *e != '0';
}();

namespace {

constexpr u32 kSubframe = 80;      // samples per subframe (fixed by the microcode)
constexpr u32 kBlockSize = 0x180;  // bytes per voice parameter block
constexpr u32 kMaxVoices = 64;
constexpr int kMixers = 6;

// Voice parameter block offsets (bytes, big-endian fields).
enum : u32 {
    kEnabled = 0x000,
    kDone = 0x002,       // set by the DSP when a sample ran out / a stop finished
    kPitch = 0x004,      // 4.12: 0x1000 = one source sample per output sample
    kReset = 0x008,      // set by DSP_PlayStart; the DSP restarts the decoder and clears it
    kPause = 0x00C,      // play the constant sample (+0x66) instead of the source
    kMaxDelay = 0x00E,   // samples of history kept for the per-mixer delays
    kMixer = 0x010,      // 6 x { u16 bus, s16 target volume, s16 current volume, u16 delay }
    kPosFrac = 0x060,    // resampler fraction, 12 bits
    kConstant = 0x066,   // last resampled sample (the paused voice's output)
    kPosition = 0x068,   // u32 decode position (ring voices: high half = position in the ring)
    kRingLeft = 0x06C,   // ring voices: samples before the ring wraps (high half)
    kRemaining = 0x074,  // ARAM voices: frames left; ring voices: samples left (CPU sets)
    kSourceType = 0x100,
    kLoopFlag = 0x102,
    kLoopYn1 = 0x104,
    kLoopYn2 = 0x106,
    kFilterMode = 0x108,  // 0x20 = IIR, low 5 bits = FIR taps
    kStopReq = 0x10A,     // ForceStopDSPchannel: fade out, then flag done
    kLoopStart = 0x110,   // sample index (ring voices: ring start address)
    kEnd = 0x114,         // sample index (ring voices: ring length << 16)
    kAramAddr = 0x118,    // sample base (ring voices: current segment address)
    kSampleCount = 0x11C,
    kFirCoef = 0x120,     // s16[24]
    kIirCoef = 0x148,     // s16[4]
    kDistCoef = 0x150,    // s16: "distortion" gain (0 = off)
};

// Source types (block +0x100). The microcode's dispatch: 0x09 AFC, 0x08 PCM8 (ARAM),
// 0x20 PCM16 in main RAM at the output rate, 0x21 PCM16 ring in main RAM (resampled);
// everything else goes to its oscillator table (0x05 and 0x10 land on an empty entry).
enum : u16 { kSrcAfc5 = 0x05, kSrcPcm8 = 0x08, kSrcAfc9 = 0x09, kSrcPcm16 = 0x10, kSrcRam = 0x20, kSrcRamRing = 0x21 };

// DSP data memory used for the mixing buffers. Bus IDs are these word addresses
// (JAudio's connect table: 0x0D00 0x0D60 0x0DC0 0x0E20 0x0E80 0x0EE0 0x0CA0 0x0F40
// 0x0FA0 0x0B00 0x09A0).
enum : u16 {
    kBusL = 0x0D00,
    kBusR = 0x0D60,
    kBusFx0 = 0x0DC0,  // effect line n reads/writes 0x0DC0 + 0x60 * n (80 samples + 8 history)
    kBusOsc = 0x0CA0,  // oscillator modulation input, never output
    kBusSurround = 0x0F40,  // next subframe: +L, -R (Dolby Surround matrix)
    kBusRearR = 0x0FA0,     // subtracted from R only
    kBusChorus = 0x0B00,    // modulated delay, then half into L and R
    kBusCenter = 0x09A0,    // 0.707 into L and R
    kChorusRing = 0x0B60,   // 160-sample ring for the modulated delay
};
s16 D[0x1000];
u64 sSamplesMixed = 0;  // output samples mixed so far (CF_AUDIO_LOG time stamps)

bool validBus(u16 id) {
    switch (id) {
    case 0x0D00: case 0x0D60: case 0x0DC0: case 0x0E20: case 0x0E80: case 0x0EE0:
    case 0x0CA0: case 0x0F40: case 0x0FA0: case 0x0B00: case 0x09A0:
        return true;
    default:
        return false;
    }
}

inline s16 sat16(s64 v) { return s16(std::clamp<s64>(v, -32768, 32767)); }
// Reading an accumulator's middle word: bits 31..16. The microcode runs in 16-bit mode
// (SET16, uc 0x0015) except for the passes it brackets with SET40: the resampler, the
// IIR, the voice mixers, the output level, AFC/PCM8 decoding and the distortion. In
// 40-bit mode the read saturates; in 16-bit mode it is the raw word, so an overflow
// wraps (effect-line FIR and mixing, the voice FIR, the bus-to-bus mixes).
inline s16 accMid(s64 acc) { return sat16(acc >> 16); }
u32 sWraps = 0;  // CF_AUDIO_LOG: 16-bit-mode results that overflowed (wrapped)
inline s16 accMidWrap(s64 acc) {
    if (const s64 hi = acc >> 16; hi != s16(hi)) ++sWraps;
    return s16(u16(u64(acc) >> 16));
}
// MOVPZ / MULMVZ: product rounded into the middle word (round half to even).
inline s64 roundMid(s64 acc) { return acc + 0x7FFF + ((acc >> 16) & 1); }
inline s16 accMidRound(s64 acc) { return accMid(roundMid(acc)); }
inline s16 accMidRoundWrap(s64 acc) { return accMidWrap(roundMid(acc)); }

// AFC coefficient pairs, used until JAudio's setup command hands us its own table.
constexpr s16 kAfcCoefDefault[16][2] = {
    {0, 0}, {2048, 0}, {0, 2048}, {1024, 1024}, {4096, -2048}, {3584, -1536}, {3072, -1024}, {4608, -2560},
    {4200, -2248}, {4800, -2300}, {5120, -3072}, {2048, -2048}, {1024, -1024}, {-1024, 1024}, {-1024, 0}, {-2048, 0},
};

struct Voice {
    // Decoder (ARAM sources).
    u32 cur = 0;  // next source sample to decode
    s16 hist1 = 0, hist2 = 0;
    s32 frameIdx = -1;
    s16 frame[16]{};
    bool sourceEnded = false;
    bool warned = false;
    // Resampler history (VPB +0x78 on hardware) and filter states (+0x80, +0xA8, +0xF0).
    s16 rs[4]{};
    s16 fir[24]{};
    s16 iir[4]{};  // x[n-1], x[n-2], y[n-1], y[n-2]
    s16 distX = 0, distY = 0;
    // Per-mixer delay history (VPB +0xD0..+0xEE). DSP_PlayStart does not clear it.
    s16 delay[256]{};
    bool loggedFx = false;  // CF_AUDIO_LOG: filters/delays reported once per start
    u32 stopSubframes = 0, stopVolume = 0;  // CF_AUDIO_LOG: force-stop fade length
    VoiceKind kind = VoiceKind::Unknown;  // music or effect (volume sliders), since the voice started
    u32 kindKey = 0;  // CF_AUDIO_KIND_LOG: what was last reported for the voice
};

struct FxLine {
    u16 enabled, size;
    u32 base;
    u16 bus[2];
    s16 vol[2];
    s16 fir[8];
};

struct State {
    u32 chanBase = 0, numVoices = 0, fxBase = 0;
    s16 afcCoef[16][2]{};
    s16 interp[64][4]{};
    bool haveInterp = false;
    // Current frame.
    bool frameActive = false;
    u32 subframes = 0, sub = 0, bufL = 0, bufR = 0;
    u16 level = 0x4000;
    std::array<Voice, kMaxVoices> voices{};
    FxLine fx[4]{};      // snapshot taken at the start of each frame (the microcode DMAs it once)
    u16 fxCounter[4]{};  // ring slot of each effect line
    // Modulated delay (chorus bus) state, microcode DRAM 0x3F0..0x3F7.
    u16 chWrite = 0, chRead = 0x96, chFrac = 0, chSpeed = 0x7F00;
    s16 chDir = -1;
} S;
// Read by the AI pacing thread (hle_audio.cpp): a frame is being mixed / when the last one ended.
std::atomic<bool> sFrameBusy{false};
std::atomic<std::chrono::steady_clock::rep> sFrameEnd{0};

inline u16 bu16(u32 a) { return u16(rd16(a)); }
inline s16 bs16(u32 a) { return s16(rd16(a)); }

bool filtersOn() { return port::settings().audio.voiceFilters; }
bool fxOn() {
    static const bool enabled = [] {
        const char* e = std::getenv("CF_AUDIO_FX");
        return !(e && *e == '0');
    }();
    return enabled && port::settings().audio.reverb;
}

void setup(u32 count, u32 chanBase, u32 interpTable, u32 afcTable, u32 fxBase) {
    S.numVoices = std::min<u32>(count, kMaxVoices);
    S.chanBase = chanBase;
    S.fxBase = fxBase;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 2; j++)
            S.afcCoef[i][j] = afcTable ? bs16(afcTable + (i * 2 + j) * 2) : kAfcCoefDefault[i][j];
    if (interpTable) {
        for (int p = 0; p < 64; p++)
            for (int t = 0; t < 4; t++) S.interp[p][t] = bs16(interpTable + (p * 4 + t) * 2);
        S.haveInterp = true;
    }
    S.voices = {};
    std::fill(std::begin(D), std::end(D), s16(0));
    for (auto& c : S.fxCounter) c = 0;
    S.chWrite = 0;
    S.chRead = 0x96;
    S.chFrac = 0;
    S.chSpeed = 0x8000 - 0x100;
    S.chDir = -1;
    if (g_log) log("[audio] DSP setup: %u voices at %08X, interp %08X, afc %08X, fx %08X", count, chanBase, interpTable, afcTable, fxBase);
}

void loadFx() {
    if (!S.fxBase) return;
    for (int l = 0; l < 4; l++) {
        const u32 f = S.fxBase + l * 0x20;
        FxLine n{};
        n.enabled = bu16(f);
        n.size = bu16(f + 2);
        n.base = rd32(f + 4);
        n.bus[0] = bu16(f + 8);
        n.vol[0] = bs16(f + 10);
        n.bus[1] = bu16(f + 12);
        n.vol[1] = bs16(f + 14);
        for (int t = 0; t < 8; t++) n.fir[t] = bs16(f + 0x10 + t * 2);
        if (g_log && std::memcmp(&n, &S.fx[l], sizeof n) != 0)
            log("[audio] fx line %d: enabled %u size %u base %08X -> %04X x %04X, %04X x %04X, fir %d %d %d %d %d %d %d %d", l,
                n.enabled, n.size, n.base, n.bus[0], u16(n.vol[0]), n.bus[1], u16(n.vol[1]), n.fir[0], n.fir[1], n.fir[2],
                n.fir[3], n.fir[4], n.fir[5], n.fir[6], n.fir[7]);
        S.fx[l] = n;
    }
}

// ---- Sources ----

// Decodes AFC frame `f` of the voice's sample into v.frame, continuing from v.hist.
void decodeAfc(Voice& v, u32 aram, u16 type, u32 f) {
    const u32 frameBytes = type == kSrcAfc9 ? 9 : 5;
    const u8* p = aramRead(aram + f * frameBytes, frameBytes);
    if (!p) {
        std::fill(std::begin(v.frame), std::end(v.frame), s16(0));
        return;
    }
    const s32 scale = 1 << (p[0] >> 4);
    const s32 c1 = S.afcCoef[p[0] & 15][0], c2 = S.afcCoef[p[0] & 15][1];
    for (int i = 0; i < 16; i++) {
        s32 delta;
        if (type == kSrcAfc9) {
            const int nib = (p[1 + i / 2] >> (i & 1 ? 0 : 4)) & 15;
            delta = ((nib ^ 8) - 8) * scale << 11;
        } else {
            const int bits = (p[1 + i / 4] >> (6 - 2 * (i & 3))) & 3;
            delta = ((bits ^ 2) - 2) * scale << 13;
        }
        s32 s = (delta + c1 * v.hist1 + c2 * v.hist2) >> 11;
        s = std::clamp(s, -32768, 32767);
        v.frame[i] = s16(s);
        v.hist2 = v.hist1;
        v.hist1 = s16(s);
    }
    v.frameIdx = s32(f);
}

// One sample from an ARAM source (AFC, PCM8; PCM16 and 2-bit AFC as a fallback).
// Returns 0 once a one-shot has run out.
s16 fetch(Voice& v, u32 blk, u16 type) {
    if (v.sourceEnded) return 0;
    const u32 end = rd32(blk + kEnd);
    if (v.cur >= end) {
        if (bu16(blk + kLoopFlag)) {
            v.cur = rd32(blk + kLoopStart);
            v.hist1 = bs16(blk + kLoopYn1);
            v.hist2 = bs16(blk + kLoopYn2);
            v.frameIdx = -1;
            if (v.cur >= end) {
                v.sourceEnded = true;
                return 0;
            }
        } else {
            v.sourceEnded = true;
            return 0;
        }
    }
    const u32 aram = rd32(blk + kAramAddr);
    const u32 n = v.cur++;
    switch (type) {
    case kSrcPcm16: {
        const u8* p = aramRead(aram + n * 2, 2);
        return p ? s16(u16(p[0] << 8 | p[1])) : 0;
    }
    case kSrcPcm8: {
        const u8* p = aramRead(aram + n, 1);
        return p ? s16(s8(p[0]) * 256) : 0;
    }
    default: {  // AFC
        const u32 f = n / 16;
        if (v.frameIdx != s32(f)) decodeAfc(v, aram, type, f);  // a jump (loop) restarts the history
        return v.frame[n % 16];
    }
    }
}

// Source types 0x20/0x21: PCM16 in main RAM, refilled by the CPU (the game's AFC
// stream player, movie audio). Microcode 0x08E8: +0x74 counts the samples left
// (the voice ends when it runs out, holding its last sample); +0x68 high half is
// the position, relative to +0x118, in a ring of (+0x114 >> 16) samples that wraps
// to +0x110. The DSP re-reads the block every subframe, so the wrap target only
// lasts for the rest of the subframe; +0x6C high half reports the samples left
// before the wrap.
void readRam(u32 blk, s16* out, u32 count, s16 last, bool& done) {
    const u32 remaining = rd32(blk + kRemaining);
    u32 pos = bu16(blk + kPosition);
    auto read = [&](u32 base, u32 p, u32 n, s16* dst) {
        for (u32 i = 0; i < n; i++) dst[i] = s16(rd16(base + (p + i) * 2));
    };
    if (remaining < count) {
        const u32 n = remaining;
        read(rd32(blk + kAramAddr), pos, n, out);
        pos += n;
        const s16 hold = n ? out[n - 1] : last;
        for (u32 i = n; i < count; i++) out[i] = hold;
        wr16(blk + kRemaining + 2, 0);
        done = true;
    } else {
        wr32(blk + kRemaining, remaining - count);
        const u32 len = bu16(blk + kEnd);
        const u32 left = (len - pos) & 0xFFFF;
        wr16(blk + kRingLeft, left);
        if (s32(left) - s32(count) >= 0) {
            read(rd32(blk + kAramAddr), pos, count, out);
            pos += count;
        } else {
            read(rd32(blk + kAramAddr), pos, left, out);
            read(rd32(blk + kLoopStart), 0, count - left, out + left);
            pos = count - left;
        }
    }
    wr16(blk + kPosition, pos);
}

// ---- Voice pipeline (microcode 0x0509..0x064C) ----

// 4-tap polyphase resampler (microcode 0x0141): the input is the 4 samples of history
// followed by `count` new samples; output n uses taps x[i..i+3] with the phase from the
// top 6 bits of the 16-bit fraction. From a ratio of 4.0 up it picks the nearest sample.
void resample(Voice& v, u32 blk, const s16* in, u32 count, s16* out) {
    const u32 pitch = std::min<u32>(bu16(blk + kPitch), 0x7FFF);
    const u32 step = pitch << 4;
    u32 pos = u32(bu16(blk + kPosFrac) & 0xFFF) << 4;
    if ((pitch >> 12) >= 4) {
        for (u32 n = 0; n < kSubframe; n++, pos += step) out[n] = in[pos >> 16];
    } else {
        for (u32 n = 0; n < kSubframe; n++, pos += step) {
            const s16* c = S.interp[(pos & 0xFFFF) >> 10];
            const s16* x = in + (pos >> 16);
            const s64 acc = (s64(c[0]) * x[0] + s64(c[1]) * x[1] + s64(c[2]) * x[2] + s64(c[3]) * x[3]) * 2;
            out[n] = accMidRound(acc);
        }
    }
    const u32 i = std::min<u32>(pos >> 16, count);
    for (int k = 0; k < 4; k++) v.rs[k] = in[i + k];
    wr16(blk + kPosFrac, (pos & 0xFFFF) >> 4);
    wr16(blk + kConstant, u16(out[kSubframe - 1]));
}

// "Distortion" (JAudio DSP_SetDistFilter, microcode 0x0B90): an incremental gain of
// coef/128 on the sample-to-sample difference, saturating, so large gains clip.
void distort(Voice& v, s16 coef, s16* buf) {
    s16 xp = v.distX, yp = v.distY;
    for (u32 n = 0; n < kSubframe; n++) {
        const s64 d = sat16(s64(buf[n]) - xp);  // the difference is read back saturated
        xp = buf[n];
        const s64 acc = ((d * coef * 2) << 8) + (s64(yp) << 16);
        yp = accMid(acc);
        buf[n] = yp;
    }
    v.distX = xp;
    v.distY = yp;
}

// Variable FIR (microcode 0x01E1): y[n] = sum c[k] * x[n - taps + k], 1.15 coefficients
// (so the newest input tap is x[n-1]: the identity, c[taps-1] = 0x7FFF, delays by one).
// Runs in 16-bit mode: the rounded result wraps on overflow.
void firFilter(Voice& v, u32 blk, u32 taps, s16* buf) {
    s16 c[24];
    for (u32 k = 0; k < taps; k++) c[k] = bs16(blk + kFirCoef + k * 2);
    s16 w[24 + kSubframe];
    std::copy(v.fir, v.fir + taps, w);
    std::copy(buf, buf + kSubframe, w + taps);
    for (u32 n = 0; n < kSubframe; n++) {
        s64 acc = 0;
        for (u32 k = 0; k < taps; k++) acc += s64(c[k]) * w[n + k];
        buf[n] = accMidRoundWrap(acc * 2);
    }
    // History: the last `taps` input samples, oldest first.
    std::copy(w + kSubframe, w + kSubframe + taps, v.fir);
}

// IIR (microcode 0x01FC): y[n] = c0*x[n-1] + c1*x[n-2] + c2*y[n-1] + c3*y[n-2], 1.15,
// truncated and saturated. {0x7FFF, 0, 0, 0} is skipped as pass-through.
void iirFilter(Voice& v, u32 blk, s16* buf) {
    const s16 c0 = bs16(blk + kIirCoef), c1 = bs16(blk + kIirCoef + 2), c2 = bs16(blk + kIirCoef + 4),
              c3 = bs16(blk + kIirCoef + 6);
    if (!c1 && !c2 && !c3 && c0 == 0x7FFF) return;
    s16 x1 = v.iir[0], x2 = v.iir[1], y1 = v.iir[2], y2 = v.iir[3];
    for (u32 n = 0; n < kSubframe; n++) {
        const s64 acc = (s64(c0) * x1 + s64(c1) * x2 + s64(c2) * y1 + s64(c3) * y2) * 2;
        const s16 y = accMid(acc);
        x2 = x1;
        x1 = buf[n];
        y2 = y1;
        y1 = y;
        buf[n] = y;
    }
    v.iir[0] = x1;
    v.iir[1] = x2;
    v.iir[2] = y1;
    v.iir[3] = y2;
}

void logStart(u32 i, u32 blk) {
    log("[audio] voice %u start (frame %u, t %.3f): type %02X aram %06X loop %u [%u,%u) count %u pitch %04X maxdelay %u filter %04X dist %d "
        "iir %d %d %d %d", i, copyDispCount(), double(sSamplesMixed) / kConsoleRate32, bu16(blk + kSourceType), rd32(blk + kAramAddr), bu16(blk + kLoopFlag), rd32(blk + kLoopStart),
        rd32(blk + kEnd), rd32(blk + kSampleCount), bu16(blk + kPitch), bu16(blk + kMaxDelay), bu16(blk + kFilterMode),
        bs16(blk + kDistCoef), bs16(blk + kIirCoef), bs16(blk + kIirCoef + 2), bs16(blk + kIirCoef + 4), bs16(blk + kIirCoef + 6));
    if (bu16(blk + kFilterMode) & 0x1F) {
        char b[256];
        int o = 0;
        for (int k = 0; k < (bu16(blk + kFilterMode) & 0x1F) && k < 24; k++)
            o += std::snprintf(b + o, sizeof b - o, " %d", bs16(blk + kFirCoef + k * 2));
        log("[audio]    fir%s", b);
    }
    for (int m = 0; m < kMixers; m++) {
        const u32 mx = blk + kMixer + m * 8;
        if (bu16(mx))
            log("[audio]    mixer %d: bus %04X target %04X current %04X delay %04X", m, bu16(mx), bu16(mx + 2), bu16(mx + 4),
                bu16(mx + 6));
    }
}

// Mixes one subframe of voice `i` into the DSP buses.
void mixVoice(u32 i) {
    const u32 blk = S.chanBase + i * kBlockSize;
    Voice& v = S.voices[i];
    if (!bu16(blk + kEnabled) || bu16(blk + kDone)) return;
    const u16 type = bu16(blk + kSourceType);
    const bool reset = bu16(blk + kReset) != 0;
    const bool paused = bu16(blk + kPause) != 0;
    if (reset) {
        Voice fresh{};
        std::copy(std::begin(v.delay), std::end(v.delay), fresh.delay);  // not cleared by PlayStart
        v = fresh;
        if (g_log) logStart(i, blk);
    }

    // Work buffer: delay history (up to 255 samples) followed by this subframe's samples.
    s16 work[256 + kSubframe];
    s16* buf = work + 256;
    bool done = false;

    if (paused) {
        std::fill(buf, buf + kSubframe, bs16(blk + kConstant));
    } else if (type == kSrcRam) {
        readRam(blk, buf, kSubframe, v.rs[3], done);
    } else {
        // New input samples this subframe (microcode 0x0540): (pitch * 80 + frac) >> 12.
        const u32 pitch = std::min<u32>(bu16(blk + kPitch), 0x7FFF);
        const u32 count = (pitch * kSubframe + (bu16(blk + kPosFrac) & 0xFFF)) >> 12;
        s16 in[4 + (0x7FFF * kSubframe >> 12) + 1];
        std::copy(v.rs, v.rs + 4, in);
        s16* fresh = in + 4;
        switch (type) {
        case kSrcRamRing:
            readRam(blk, fresh, count, v.rs[3], done);
            break;
        case kSrcAfc9:
        case kSrcPcm8:
        case kSrcAfc5:   // not handled by LM's microcode; decoded anyway
        case kSrcPcm16:  // likewise
            for (u32 n = 0; n < count; n++) fresh[n] = fetch(v, blk, type);
            if (v.sourceEnded) done = true;
            break;
        default:
            if (!v.warned) {
                v.warned = true;
                if (g_log) log("[audio] voice %u uses source type %u (oscillator), not supported", i, type);
            }
            std::fill(fresh, fresh + count, s16(0));
            break;
        }
        resample(v, blk, in, count, buf);
    }

    if (g_log && !v.loggedFx) {
        const u16 fm = bu16(blk + kFilterMode);
        const bool iir = (fm & 0x20) && (bs16(blk + kIirCoef) != 0x7FFF || bu16(blk + kIirCoef + 2) ||
                                         bu16(blk + kIirCoef + 4) || bu16(blk + kIirCoef + 6));
        bool delay = false;
        for (int m = 0; m < kMixers; m++) delay |= bu16(blk + kMixer + m * 8 + 6) != 0;
        if ((fm & 0x1F) || iir || bs16(blk + kDistCoef) || delay) {
            v.loggedFx = true;
            log("[audio] voice %u now uses: filter %04X dist %d iir %d %d %d %d delays %04X %04X", i, fm, bs16(blk + kDistCoef),
                bs16(blk + kIirCoef), bs16(blk + kIirCoef + 2), bs16(blk + kIirCoef + 4), bs16(blk + kIirCoef + 6),
                bu16(blk + kMixer + 6), bu16(blk + kMixer + 14));
        }
    }

    // Per-voice filters: distortion, FIR, IIR (in the microcode's order).
    if (filtersOn()) {
        if (const s16 dc = bs16(blk + kDistCoef)) distort(v, dc, buf);
        const u16 fm = bu16(blk + kFilterMode);
        if (const u32 taps = std::min<u32>(fm & 0x1F, 24)) firFilter(v, blk, taps, buf);
        if (fm & 0x20) iirFilter(v, blk, buf);
    }

    // User volume per category (Sound tab). Streamed music (.afc streams, the movie) plays
    // through ring voices; sequenced music and sound effects are both notes from ARAM, told
    // apart by the sequence that plays them (audio::voiceKind, docs/AUDIO.md "Volume sliders").
    {
        const auto& a = port::settings().audio;
        bool music = type == kSrcRamRing || type == kSrcRam;
        if (!music) {
            // Once the note's sequence has ended its track is released (data 0) while the
            // voice may still be fading out: keep what the voice was since its start.
            VoiceKind k = voiceKind(i);
            if (k == VoiceKind::Unknown) k = v.kind;
            v.kind = k;
            music = k == VoiceKind::Music;
            static const bool sKindLog = std::getenv("CF_AUDIO_KIND_LOG") != nullptr;
            if (sKindLog) {
                const int bank = voiceBank(i);
                const u32 seq = voiceSequence(i);
                const u32 key = rd32(blk + kAramAddr) ^ u32(bank) << 8 ^ seq ^ u32(k);
                if (v.kindKey != key) {
                    v.kindKey = key;
                    log("[audio] voice %u (aram %06X, bank %d program %d, track %08X data %08X): %s", i, rd32(blk + kAramAddr),
                        bank >> 8, bank & 0xFF, seq, gu::inMem1(seq) ? rd32(seq) : 0,
                        k == VoiceKind::Music ? "music" : k == VoiceKind::Effect ? "sound effect" : "unknown (effect volume)");
                }
            }
        }
        const float g = music ? a.musicVolume : a.sfxVolume;
        if (g < 0.999f) {
            const s32 q = s32(std::clamp(g, 0.0f, 1.0f) * 32768.0f);
            for (u32 n = 0; n < kSubframe; n++) buf[n] = s16((s32(buf[n]) * q) >> 15);
        }
    }

    // Per-mixer delays read up to +0x0E samples back.
    const u32 keep = std::min<u32>(bu16(blk + kMaxDelay), 255);
    std::copy(v.delay + 256 - keep, v.delay + 256, buf - keep);

    // Stop request (microcode 0x05E0, 16-bit mode): each mixer's *target* volume becomes
    // its current volume shifted right by one (LRRD reads the current word, SN stores the
    // halved value one word lower, into the target), so the mixers below ramp every
    // subframe from the current volume to half of it: a smooth exponential fade. The
    // shift is logical (the accumulator's high word is 0). The voice is flagged done once
    // all six current volumes are 0 (the test sums the full 40-bit shifted values, so a
    // volume of 1 still counts). The halved targets are written back like the rest of
    // the first 0x100 bytes.
    if (bu16(blk + kStopReq)) {
        bool allZero = true;
        for (int m = 0; m < kMixers; m++) {
            const u32 mx = blk + kMixer + m * 8;
            const u16 cur = bu16(mx + 4);
            allZero &= cur == 0;
            wr16(mx + 2, u16(cur >> 1));
        }
        if (!v.stopSubframes++ && g_log) v.stopVolume = bu16(blk + kMixer + 4) | u32(bu16(blk + kMixer + 12)) << 16;
        if (allZero) {
            done = true;
            if (g_log)
                log("[audio] voice %u stopped: faded from %04X/%04X in %u subframes", i, v.stopVolume & 0xFFFF,
                    v.stopVolume >> 16, v.stopSubframes);
        }
    }

    for (int m = 0; m < kMixers; m++) {
        const u32 mx = blk + kMixer + m * 8;
        const u16 bus = bu16(mx);
        const s32 cur = bs16(mx + 4);
        const s32 target = bs16(mx + 2);
        // Delay: low byte current, high byte target; one sample per subframe toward it. The
        // mixer reads (max delay - current delay) into the history; when the current delay
        // exceeds the history kept (uc 0x061C JLZ), the mixer is skipped for this subframe
        // and its volume stays where it was.
        const u16 lv = bu16(mx + 6);
        const u32 d = lv & 0xFF;
        if ((lv >> 8) != (lv & 0xFF)) wr16(mx + 6, (lv >> 8) > (lv & 0xFF) ? lv + 1 : lv - 1);
        if (d > keep) continue;
        // Volume ramp (microcode 0x011E): 16.16 step (target - current) / 32, applied every
        // second sample over the first 64; the product is 1.15 x 1.15 >> 16 (no doubling),
        // added to the bus with saturation (40-bit mode). A mixer without a bus still ramps
        // (on hardware it mixes into scratch memory at DSP address 0).
        if (bus && validBus(bus) && (cur || target)) {
            const s64 delta = (s64(target - cur) << 16) >> 5;
            s16* out = D + bus;
            const s16* src = buf - d;
            for (u32 n = 0; n < kSubframe; n++) {
                const s64 k = n == 0 ? 0 : std::min<s64>((n - 1) / 2, 32);
                const s32 vol = s32(((s64(cur) << 16) + k * delta) >> 16);
                out[n] = accMid((s64(out[n]) << 16) + s64(vol) * src[n]);
            }
        }
        wr16(mx + 4, u16(target));
    }
    std::copy(v.delay + kSubframe, v.delay + 256, v.delay);  // keep the last 256 samples
    std::copy(buf, buf + kSubframe, v.delay + 256 - kSubframe);

    if (!paused) wr16(blk + kReset, 0);

    // Status the CPU reads back (ring voices wrote theirs in readRam).
    if (type != kSrcRam && type != kSrcRamRing && !paused) {
        const u32 end = rd32(blk + kEnd);
        wr32(blk + kPosition, v.cur);
        const u32 left = end > v.cur ? end - v.cur : 0;
        wr32(blk + kRemaining, (type == kSrcAfc9 || type == kSrcAfc5) ? left >> 4 : left);
    }
    if (done) wr16(blk + kDone, 1);
}

// Mixes `src` into `dst` at a 1.15 volume (microcode 0x03BD, multiplier x2): MULMVZ
// rounds the product into the accumulator, ADDR adds the destination sample, and the
// middle word is stored in 16-bit mode, so the sum wraps on overflow.
void mixBus(u16 src, u16 dst, s16 vol) {
    for (u32 n = 0; n < kSubframe; n++) {
        const s64 p = roundMid(s64(vol) * D[src + n] * 2) & ~s64(0xFFFF);
        D[dst + n] = accMidWrap(p + (s64(D[dst + n]) << 16));
    }
}

// 8-tap FIR over an effect line's 88-sample buffer, in place (microcode 0x01C2).
void fxFir(u16 bus, const s16* c) {
    for (u32 n = 0; n < kSubframe; n++) {
        s64 acc = 0;
        for (int k = 0; k < 8; k++) acc += s64(c[k]) * D[bus + n + k];
        D[bus + n] = accMidRoundWrap(acc * 2);  // 16-bit mode
    }
}

// Effect lines, before the voices (microcode 0x0446). Line n's buffer is bus
// 0x0DC0 + 0x60 * n: its last 8 samples move to the front as history, the 80 samples
// written `size` subframes ago are fetched from the ring in main RAM behind them, the
// FIR runs over the whole buffer (enabled bit 0), the result goes to the two output
// buses, and (bit 1) the FIR runs again. Voices then add their sends on top, and the
// sum goes back into the ring (fxPost): a feedback delay.
void fxPre() {
    for (int l = 0; l < 4; l++) {
        const FxLine& f = S.fx[l];
        if (!f.enabled || !f.base) continue;
        const u16 bus = u16(kBusFx0 + 0x60 * l);
        std::copy(D + bus + 80, D + bus + 88, D + bus);
        const u32 slot = f.base + u32(S.fxCounter[l]) * 0xA0;
        for (u32 n = 0; n < kSubframe; n++) D[bus + 8 + n] = s16(rd16(slot + n * 2));
        if (f.enabled & 1) fxFir(bus, f.fir);
        for (int k = 0; k < 2; k++)
            if (f.bus[k] && validBus(f.bus[k])) mixBus(bus, f.bus[k], f.vol[k]);
        if (f.enabled & 2) fxFir(bus, f.fir);
    }
}

// Effect lines, after the voices (microcode 0x047D): store the line's buffer in its ring slot.
void fxPost() {
    for (int l = 0; l < 4; l++) {
        const FxLine& f = S.fx[l];
        if (!f.enabled || !f.base) continue;
        const u16 bus = u16(kBusFx0 + 0x60 * l);
        const u32 slot = f.base + u32(S.fxCounter[l]) * 0xA0;
        for (u32 n = 0; n < kSubframe; n++) wr16(slot + n * 2, u16(D[bus + n]));
        S.fxCounter[l] = u16(S.fxCounter[l] + 1 >= f.size ? 0 : S.fxCounter[l] + 1);
    }
}

// Chorus bus (microcode 0x0B11/0x0B24/0x0B7B): written into a 160-sample ring, read back
// at 1 +- 1/128 speed (the direction flips when the read pointer comes within 16 / 48
// samples of the write side), nearest sample, then mixed at 0.5 into R and L.
void chorus() {
    const u16 w = S.chWrite;
    S.chWrite = w ? 0 : 80;
    std::copy(D + kBusChorus, D + kBusChorus + 80, D + kChorusRing + w);
    s32 dist = 0xA0 - S.chRead;
    if (dist >= 80) dist -= 80;
    if (S.chDir >= 0) {
        if (dist - 0x10 <= 0) {
            S.chSpeed = 0x8000 - 0x100;
            S.chDir = s16(-S.chDir);
        }
    } else if (0x30 - dist <= 0) {
        S.chSpeed = 0x8000 + 0x100;
        S.chDir = s16(-S.chDir);
    }
    const u32 step = u32(S.chSpeed) << 1;
    u32 frac = S.chFrac;
    u32 rd = S.chRead;
    for (u32 n = 0; n < kSubframe; n++) {
        D[kBusChorus + n] = D[kChorusRing + rd];
        frac += step;
        rd = (rd + (frac >> 16)) % 160;
        frac &= 0xFFFF;
    }
    S.chRead = u16(rd);
    S.chFrac = u16(frac);
    mixBus(kBusChorus, kBusR, 0x3FFF);
    mixBus(kBusChorus, kBusL, 0x3FFF);
}

// Output level (microcode 0x0130): 4.12 fixed point, saturated.
void applyLevel(u16 bus) {
    for (u32 n = 0; n < kSubframe; n++) D[bus + n] = accMid((s64(D[bus + n]) * s16(S.level)) << 4);
}

// Side outputs for discrete multichannel playback (audio.h, SideSample): per output
// sample, the parts of the stereo mix that came from the centre, surround and rear-right
// buses, each at the output level. One store per DSP output buffer (JAudio rotates three).
struct SideStore {
    u32 bufL = 0;
    std::vector<SideSample> v;
};
std::array<SideStore, 4> sSide;
u32 sSideNext = 0;
SideStore* sCurSide = nullptr;

// Bus energy for CF_AUDIO_LOG (sum of squares per bus since the last report).
struct BusEnergy {
    double l = 0, r = 0, s = 0, c = 0, rr = 0, ch = 0;
} sEnergy;

void beginSide(u32 bufL, u32 subframes) {
    SideStore* st = nullptr;
    for (auto& x : sSide)
        if (x.bufL == bufL) st = &x;
    if (!st) {
        st = &sSide[sSideNext];
        sSideNext = (sSideNext + 1) % sSide.size();
        st->bufL = bufL;
    }
    st->v.assign(subframes * kSubframe, SideSample{});
    sCurSide = st;
}

inline s16 level(s64 v) { return sat16((v * s16(S.level)) >> 12); }

void runSubframe() {
    // 0x03D9: the surround bus of the previous subframe starts the outputs (+L, -R), and the
    // utility buses are cleared.
    s16 sur[kSubframe];
    for (u32 n = 0; n < kSubframe; n++) {
        sur[n] = D[kBusSurround + n];
        D[kBusL + n] = sur[n];
        D[kBusR + n] = s16(~sur[n]);
    }
    for (u16 b : {kBusOsc, kBusSurround, kBusRearR, kBusChorus, kBusCenter}) std::fill(D + b, D + b + kSubframe, s16(0));
    const bool fx = fxOn();
    if (fx) fxPre();
    if (S.chanBase && S.haveInterp)
        for (u32 i = 0; i < S.numVoices; i++) mixVoice(i);
    if (fx) chorus();
    if (g_log)
        for (u32 n = 0; n < kSubframe; n++) sEnergy.ch += double(D[kBusChorus + n]) * D[kBusChorus + n];
    mixBus(kBusCenter, kBusL, 0x5A82);
    mixBus(kBusCenter, kBusR, 0x5A82);
    s16 preL[kSubframe];
    std::copy(D + kBusL, D + kBusL + kSubframe, preL);
    applyLevel(kBusL);
    mixBus(kBusRearR, kBusR, s16(0x8000));
    s16 preR[kSubframe];
    std::copy(D + kBusR, D + kBusR + kSubframe, preR);
    applyLevel(kBusR);
    const u32 off = S.sub * kSubframe * 2;
    for (u32 n = 0; n < kSubframe; n++) {
        wr16(S.bufL + off + n * 2, u16(D[kBusL + n]));
        wr16(S.bufR + off + n * 2, u16(D[kBusR + n]));
    }
    // Side outputs: what the centre (0.707 into both sides), surround (+L, NOT R) and
    // rear-right (-R) buses added, so the output can take them back out of the fronts.
    if (sCurSide && (S.sub + 1) * kSubframe <= sCurSide->v.size()) {
        SideSample* o = sCurSide->v.data() + S.sub * kSubframe;
        for (u32 n = 0; n < kSubframe; n++) {
            const s16 c = D[kBusCenter + n], rr = D[kBusRearR + n];
            const s64 cMid = roundMid(s64(0x5A82) * c * 2) >> 16;
            const s64 fl = s64(preL[n]) - sur[n] - cMid;
            const s64 fr = s64(preR[n]) - s16(~sur[n]) - cMid + rr;
            o[n].lExtra = s16(s32(D[kBusL + n]) - level(fl));
            o[n].rExtra = s16(s32(D[kBusR + n]) - level(fr));
            o[n].c = level(c);
            o[n].s = level(sur[n]);
            o[n].rr = level(rr);
        }
    }
    if (g_log)
        for (u32 n = 0; n < kSubframe; n++) {
            auto sq = [](double v) { return v * v; };
            sEnergy.l += sq(D[kBusL + n]);
            sEnergy.r += sq(D[kBusR + n]);
            sEnergy.s += sq(sur[n]);
            sEnergy.c += sq(D[kBusCenter + n]);
            sEnergy.rr += sq(D[kBusRearR + n]);
        }
    if (fx) fxPost();
    sSamplesMixed += kSubframe;
}

}  // namespace

void dspCommand(const u32* w, u32 count) {
    if (!count) return;
    switch (w[0] >> 24) {
    case 0x81:  // setup: voice count | 0x81000000, voice blocks, interpolation table, AFC table, FX lines
        if (count >= 5) setup(w[0] & 0xFFFF, w[1], w[2], w[3], w[4]);
        break;
    case 0x82:  // sync frame: subframes << 16 | mixer level, left buffer, right buffer
        if (count >= 3) {
            S.subframes = (w[0] >> 16) & 0xFF;
            S.level = u16(w[0]);
            S.bufL = w[1];
            S.bufR = w[2];
            S.sub = 0;
            S.frameActive = S.subframes != 0;
            sFrameBusy.store(S.frameActive, std::memory_order_release);
            beginSide(S.bufL, S.subframes);
            loadFx();
        }
        break;
    case 0x80:  // wait frame: nothing to wait for
        break;
    default:
        if (g_log) log("[audio] unknown DSP command %08X (%u words)", w[0], count);
        break;
    }
}

bool dspSideData(u32 bufL, SideSample* out, u32 n) {
    for (const auto& st : sSide) {
        if (st.bufL != bufL || st.v.empty()) continue;
        const u32 m = std::min<u32>(n, u32(st.v.size()));
        std::copy(st.v.begin(), st.v.begin() + m, out);
        std::fill(out + m, out + n, SideSample{});
        return true;
    }
    std::fill(out, out + n, SideSample{});
    return false;
}

bool dspFrameBusy() { return sFrameBusy.load(std::memory_order_acquire); }
std::chrono::steady_clock::time_point dspFrameEnd() {
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(sFrameEnd.load(std::memory_order_acquire)));
}

bool dspRunSubframe() {
    if (!S.frameActive) return false;
    static double busy = 0;  // CF_AUDIO_LOG: host time spent mixing
    const auto t0 = std::chrono::steady_clock::now();
    runSubframe();
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (++S.sub >= S.subframes) {
        S.frameActive = false;
        sFrameEnd.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
        sFrameBusy.store(false, std::memory_order_release);
    }
    if (g_log) {
        static u32 frames = 0;
        if (!S.frameActive && ++frames % 114 == 0) {
            log("[audio] frame %u (%.1f s), level %04X, mixing took %.2f%% of real time", frames, frames * 560 / 32000.0, S.level,
                busy / (114 * 560 / 32000.0) * 100);
            busy = 0;
            {
                const double k = 114.0 * 560;
                auto rms = [&](double e) { return std::sqrt(e / k); };
                log("[audio]   bus rms: out L %.0f R %.0f, pre-level surround %.0f center %.0f rear-R %.0f chorus %.0f",
                    rms(sEnergy.l), rms(sEnergy.r), rms(sEnergy.s), rms(sEnergy.c), rms(sEnergy.rr), rms(sEnergy.ch));
                if (sWraps) log("[audio]   %u 16-bit overflows (wrapped as on hardware)", sWraps);
                sWraps = 0;
                sEnergy = {};
            }
            for (u32 i = 0; i < S.numVoices; i++) {
                const u32 blk = S.chanBase + i * kBlockSize;
                if (!bu16(blk + kEnabled) || bu16(blk + kDone)) continue;
                log("[audio]   v%u type %02X pitch %04X L %04X R %04X aux %04X %04X stop %u", i, bu16(blk + kSourceType),
                    bu16(blk + kPitch), bu16(blk + kMixer + 4), bu16(blk + kMixer + 12), bu16(blk + kMixer + 20),
                    bu16(blk + kMixer + 28), bu16(blk + kStopReq));
            }
        }
    }
    return true;
}

}  // namespace recomp::audio
