// Furniture lights for Enhanced lighting (docs/RENDERING.md "Furniture lights"): lamps, TVs,
// stereos, computers, fireplaces, candles, lava lamps, festive trees... light the room around them.
//
// Everything is derived at run time from the furniture's own loaded model (MDL0 in guest memory)
// and the game's own on/off state; the only data of ours is the small exceptions table below
// (item numbers + our numbers). Off (master or "Furniture lights") nothing is submitted and the
// hooks only read guest memory and call the original.
//
// - Which models light: the same rules as the review scanner tools/research/furniture_lights.py.
//   (a) the glow materials d_a_ftrNP's helpers drive by name: lp_m (lamps), mn_m / tv_m (screens),
//   fr_m (fire). (b) other light-like materials: a lamp-style konst bypass of the lighting, additive
//   blending, unlit special materials (not df_* / ev_*), unlit or special materials named after fire
//   / lava / neon, and whole models named after fire. Water (wa_m / wb_m, pools, baths, toilets,
//   sinks, fountains) never lights.
// - Light: at the area-weighted centre of the glowing parts (model space, through the drawn
//   ScnObj's world matrix: placement, rotation, moving furniture), colour = the parts' mean texture
//   colour (kept saturated; screens cooled, fire warmed), radius from the model's size, flicker for
//   fire.
// - On / off: d_a_ftrNP fn_15_767C updates a glow helper (+0 amount 0..1, +0x14 the material:
//   ResMatData; its TEV konst colour 3 alpha = the amount); hooked, recorded per (MDL0, material).
//   d_a_ftrNP fn_15_3100 (daFTR_c's per-frame update, hooked) maps each furniture actor's model
//   (virtual +0x210: its MDL0) to its item record (+0x3A0) for the exceptions and item names.
#include "game/cf_furniture_light.h"
#include "game/cf_light.h"

#include "port/settings.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <aurora/lights.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp

namespace cffurn {
namespace {

// Test aid CF_FURN_LOG=1: every model analysed (kind, light, why), lights every 120 frames;
// =2 also every glow-helper amount change.
const int sLog = [] {
    const char* v = std::getenv("CF_FURN_LOG");
    return v ? std::max(std::atoi(v), 1) : 0;
}();

enum Kind : u8 { KNone, KLamp, KScreen, KFire, KWater, KNeon, KLava };
const char* kindName(u8 k) {
    static const char* n[] = {"none", "lamp", "screen", "fire", "water", "neon", "lava"};
    return k < 7 ? n[k] : "?";
}

// ---- exceptions (our numbers; item numbers as the game's item table numbers them) ----
struct Exception {
    u16 item;
    bool drop;            // never lights
    const char* mat;      // added light: the glowing material (nullptr = the whole model)
    float heightShare;    // added light: the light at this share of the model's height (< 0: the part's centre)
    float color[3];       // added light: colour (r < 0: from the part's texture)
    float radius, intensity;
    u8 kind;
    bool flicker;
};
constexpr Exception kExceptions[] = {
    // name matches only, no glowing part (user review 2026-11-04)
    {0xBE3, true, nullptr, -1, {}, 0, 0, KNone, false},  // stove (kitchen range)
    {0xBD5, true, nullptr, -1, {}, 0, 0, KNone, false},  // propane stove
    // additive parts that are only a shine
    {0xBED, true, nullptr, -1, {}, 0, 0, KNone, false},  // doghouse
    {0xBF2, true, nullptr, -1, {}, 0, 0, KNone, false},  // beach table
    // additions without a light-like material
    {0xB5D, false, nullptr, 0.62f, {1.0f, 0.739f, 0.486f}, 246, 1.6f, KFire, true},  // tall lantern: the fire box
    {0xC1A, false, "ev_m", -1, {0.6f, 1.0f, 0.42f}, 205, 0.7f, KNeon, false},        // glow clock: behind its glass
    {0xC16, false, "df_1_m", -1, {-1, 0, 0}, 205, 0.7f, KNeon, false},               // kitschy clock: the unlit face
};
const Exception* exceptionFor(u16 item) {
    for (const auto& e : kExceptions)
        if (e.item == item) return &e;
    return nullptr;
}

// ---- guest memory helpers ----
bool gptr(u32 p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }
s32 s32at(u32 a) { return s32(rd32(a)); }
std::string gstr(u32 a, u32 max = 64) {
    std::string s;
    if (!gptr(a)) return s;
    for (u32 k = 0; k < max; k++) {
        const char ch = char(rd8(a + k));
        if (!ch) break;
        s += (ch >= 32 && ch < 127) ? ch : '?';
    }
    return s;
}
std::string lower(std::string s) {
    for (auto& ch : s) ch = char(std::tolower(u8(ch)));
    return s;
}
// NW4R index group: (name, data address) of each entry.
std::vector<std::pair<std::string, u32>> group(u32 g) {
    std::vector<std::pair<std::string, u32>> out;
    if (!gptr(g)) return out;
    const u32 n = std::min(rd32(g + 4), 512u);
    for (u32 k = 1; k <= n; k++) {
        const u32 e = g + 8 + 16 * k;
        const u32 no = rd32(e + 8), d = rd32(e + 12);
        out.emplace_back(no ? gstr(g + no) : std::string(), d ? g + d : 0u);
    }
    return out;
}
// BP writes (0x61 rr vvvvvv) of a display list.
void bpWrites(u32 a, u32 end, std::unordered_map<u8, u32>& out) {
    for (u32 o = a; o + 5 <= end;) {
        const u8 op = rd8(o);
        if (op == 0x61) {
            out[rd8(o + 1)] = (u32(rd8(o + 2)) << 16) | (u32(rd8(o + 3)) << 8) | rd8(o + 4);
            o += 5;
        } else if (op == 0x10) {
            o += 5 + 4 * ((rd32(o + 1) >> 16) + 1);
        } else if (op == 0x08) {
            o += 6;
        } else {
            o += 1;
        }
    }
}

// ---- name hints (same lists as the scanner) ----
u8 nameHint(const std::string& text) {
    static const std::pair<const char*, u8> kHints[] = {
        {"fireplug", KNone}, {"candle", KFire}, {"fire", KFire}, {"flame", KFire}, {"torch", KFire}, {"brazier", KFire},
        {"hibachi", KFire}, {"stove", KFire}, {"lavalamp", KLava}, {"lava lamp", KLava}, {"lamp", KLamp}, {"light", KLamp},
        {"lantern", KLamp}, {"neon", KNeon}, {"glow", KLamp}, {"aquarium", KWater}};
    const std::string t = lower(text);
    for (const auto& [k, kind] : kHints)
        if (t.find(k) != std::string::npos) return kind;
    return KNone;
}
bool isWater(const std::string& text) {
    static const char* kWords[] = {"water", "pool", "bath", "rotenburo", "trough", "sink", "toilet", "urinal", "wc0", "wc1",
                                   "sprinkler", "merlion", "syon", "fountain", "sys_k"};
    const std::string t = lower(text);
    for (const char* w : kWords)
        if (t.find(w) != std::string::npos) return true;
    return false;
}

// ---- textures (one texel at a time) ----
struct TexRef {
    u32 data = 0, w = 0, h = 0, fmt = 0;
    u32 pal = 0, palFmt = 0, palCount = 0;
};
void rgb565(u32 v, float* o) {
    o[0] = float((v >> 11) & 31) / 31.0f, o[1] = float((v >> 5) & 63) / 63.0f, o[2] = float(v & 31) / 31.0f, o[3] = 1.0f;
}
void rgb5a3(u32 v, float* o) {
    if (v & 0x8000) {
        o[0] = float((v >> 10) & 31) / 31.0f, o[1] = float((v >> 5) & 31) / 31.0f, o[2] = float(v & 31) / 31.0f, o[3] = 1.0f;
    } else {
        o[0] = float((v >> 8) & 15) / 15.0f, o[1] = float((v >> 4) & 15) / 15.0f, o[2] = float(v & 15) / 15.0f;
        o[3] = float((v >> 12) & 7) / 7.0f;
    }
}
void palEntry(const TexRef& t, u32 i, float* o) {
    if (!t.pal || i >= t.palCount) {
        o[0] = o[1] = o[2] = o[3] = 1.0f;
        return;
    }
    const u32 v = rd16(t.pal + 2 * i);
    if (t.palFmt == 0) o[0] = o[1] = o[2] = float(v & 0xFF) / 255.0f, o[3] = float(v >> 8) / 255.0f;
    else if (t.palFmt == 1) rgb565(v, o);
    else rgb5a3(v, o);
}
void texel(const TexRef& t, int x, int y, float* o) {
    static const u8 kB[15][3] = {{8, 8, 4}, {8, 4, 8}, {8, 4, 8}, {4, 4, 16}, {4, 4, 16}, {4, 4, 16}, {4, 4, 32}, {0, 0, 0},
                                 {8, 8, 4}, {8, 4, 8}, {4, 4, 16}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {8, 8, 4}};
    o[0] = o[1] = o[2] = o[3] = 1.0f;
    if (t.fmt > 14 || !kB[t.fmt][0] || !t.w || !t.h) return;
    x = ((x % int(t.w)) + int(t.w)) % int(t.w);
    y = ((y % int(t.h)) + int(t.h)) % int(t.h);
    const u32 bw = kB[t.fmt][0], bh = kB[t.fmt][1], bpp = kB[t.fmt][2];
    const u32 bx = (t.w + bw - 1) / bw;
    const u32 block = (u32(y) / bh) * bx + u32(x) / bw;
    const u32 base = t.data + block * (bw * bh * bpp / 8);
    const u32 px = (u32(y) % bh) * bw + u32(x) % bw;
    switch (t.fmt) {
    case 0: {  // I4
        const u8 b = rd8(base + px / 2);
        o[0] = o[1] = o[2] = o[3] = float((px & 1) ? (b & 15) : (b >> 4)) / 15.0f;
        break;
    }
    case 1: o[0] = o[1] = o[2] = o[3] = float(rd8(base + px)) / 255.0f; break;
    case 2: {
        const u8 b = rd8(base + px);
        o[0] = o[1] = o[2] = float(b & 15) / 15.0f, o[3] = float(b >> 4) / 15.0f;
        break;
    }
    case 3: {
        const u32 v = rd16(base + 2 * px);
        o[0] = o[1] = o[2] = float(v & 0xFF) / 255.0f, o[3] = float(v >> 8) / 255.0f;
        break;
    }
    case 4: rgb565(rd16(base + 2 * px), o); break;
    case 5: rgb5a3(rd16(base + 2 * px), o); break;
    case 6:
        o[3] = float(rd8(base + 2 * px)) / 255.0f, o[0] = float(rd8(base + 2 * px + 1)) / 255.0f;
        o[1] = float(rd8(base + 32 + 2 * px)) / 255.0f, o[2] = float(rd8(base + 32 + 2 * px + 1)) / 255.0f;
        break;
    case 8: {
        const u8 b = rd8(base + px / 2);
        palEntry(t, (px & 1) ? (b & 15) : (b >> 4), o);
        break;
    }
    case 9: palEntry(t, rd8(base + px), o); break;
    case 10: palEntry(t, rd16(base + 2 * px) & 0x3FFF, o); break;
    case 14: {  // CMPR: 8x8 blocks of four 4x4 DXT1 sub-blocks
        const u32 sub = ((u32(y) % 8) / 4) * 2 + (u32(x) % 8) / 4;
        const u32 s = base + sub * 8;
        const u32 c0 = rd16(s), c1 = rd16(s + 2), bits = rd32(s + 4);
        const u32 i = (bits >> (30 - 2 * ((u32(y) % 4) * 4 + u32(x) % 4))) & 3;
        float p0[4], p1[4];
        rgb565(c0, p0), rgb565(c1, p1);
        if (i == 0) std::memcpy(o, p0, sizeof p0);
        else if (i == 1) std::memcpy(o, p1, sizeof p1);
        else if (c0 > c1) {
            for (int k = 0; k < 3; k++) o[k] = i == 2 ? (2 * p0[k] + p1[k]) / 3 : (p0[k] + 2 * p1[k]) / 3;
            o[3] = 1.0f;
        } else if (i == 2) {
            for (int k = 0; k < 3; k++) o[k] = (p0[k] + p1[k]) / 2;
            o[3] = 1.0f;
        } else {
            o[0] = o[1] = o[2] = o[3] = 0.0f;
        }
        break;
    }
    default: break;
    }
}

// ---- model analysis ----
struct Light {
    bool lit = false;
    u8 kind = KNone;
    bool flicker = false;
    float centre[3] = {};  // model space
    float color[3] = {1, 1, 1};
    float radius = 0, intensity = 0;
    float core = 0.1f;  // source core radius as a share of the reach (Aurora 0480)
    // before branch indoor-light (test aids CF_INDOOR_LEGACY / CF_INDOOR_AB)
    float oldCentre[3] = {}, oldColor[3] = {1, 1, 1}, oldRadius = 0, oldIntensity = 0;
    std::vector<std::string> gameMats;  // glow-helper materials: their amount switches the light
    bool shadow = false;
    bool roomLamp = false;  // item light kind 2: the game's room brightening follows it
    std::string model;  // the model it was made for (addresses are reused)
};
struct ItemInfo {
    u16 number = 0;
    u8 lightKind = 0;
    std::string name;
};
std::unordered_map<u32, ItemInfo> sItemOf;         // MDL0 -> item (from the furniture actors)
std::unordered_map<u64, Light> sLights;            // (MDL0, item number) -> analysis
std::unordered_map<u64, std::pair<float, u32>> sAmount;  // hash(MDL0, material) -> amount, frame
std::unordered_map<u32, u32> sDone;                     // actor -> record already mapped
u32 sR13 = 0;                                           // the game's r13 (current stage byte at r13 - 0x6384)
int sStage = -1;
u32 sLitFrame = 0;  // last frame a lit light-kind 2 lamp / candle was submitted (the room-light damping needs one)

u64 key2(u32 a, u32 b) { return (u64(a) << 32) | b; }
u64 matKey(u32 mdl, const std::string& mat) { return key2(mdl, u32(std::hash<std::string>{}(mat))); }

struct Mat {
    std::string name;
    u32 addr = 0;
    bool game = false, water = false, unlit = false, bypass = false, additive = false;
    u8 hint = KNone, gameKind = KNone;
    std::string tex, pal;
    u32 boundTex = 0, boundPal = 0;
};

struct Acc {
    double cen[3] = {}, area = 0;
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    double col[3] = {}, wsum = 0;
    double ccol[3] = {}, cwsum = 0;  // weighted by chroma too: the part's dominant hue (branch indoor-light)
};

// Accumulates one object's triangles (model space) into `acc`: area-weighted centre, bounds, mean
// texture colour sampled at each triangle's corners and centre.
void accumulateObject(u32 mdl, u32 obj, const std::vector<u32>& verts, const std::vector<u32>& uvs, const std::vector<u32>& colors,
                      const std::vector<u32>& bones, const std::vector<s32>& mtxNode, const TexRef* tex, bool useVtxColor, Acc& acc) {
    (void)mdl;
    const u32 lo = rd32(obj + 0x0C), hi = rd32(obj + 0x10);
    const s32 single = s32at(obj + 0x08);
    const u32 prim = obj + 0x24 + u32(s32at(obj + 0x2C));
    const u32 primSize = u32(std::max(s32at(obj + 0x28), 0));
    s16 ids[12];
    for (int k = 0; k < 12; k++) ids[k] = s16(rd16(obj + 0x48 + 2 * k));
    std::vector<s16> mlist;
    if (const s32 mo = s32at(obj + 0x64)) {
        const u32 ml = obj + u32(mo);
        for (u32 k = 0, n = std::min(rd32(ml), 64u); k < n; k++) mlist.push_back(s16(rd16(ml + 4 + 2 * k)));
    }
    struct Attr {
        u8 kind, type;  // kind 0 pnmtx, 1 tmtx, 2 pos, 3 nrm, 4 clr0, 5 clr1, 6+ uv
    };
    std::vector<Attr> attrs;
    if (lo & 1) attrs.push_back({0, 1});
    for (int k = 0; k < 8; k++)
        if ((lo >> (1 + k)) & 1) attrs.push_back({1, 1});
    for (int k = 0; k < 4; k++)
        if (const u8 t = u8((lo >> (9 + 2 * k)) & 3)) attrs.push_back({u8(2 + k), t});
    for (int k = 0; k < 8; k++)
        if (const u8 t = u8((hi >> (2 * k)) & 3)) attrs.push_back({u8(6 + k), t});
    // vertex arrays
    auto arrayInfo = [](u32 ao, int ncompIfSt, bool uv, u32& data, u32& fmt, u32& frac, u32& stride, u32& count, int& ncomp) {
        data = ao + u32(s32at(ao + 8));
        const u32 comp = rd32(ao + 0x14);
        fmt = rd32(ao + 0x18), frac = rd8(ao + 0x1C), stride = rd8(ao + 0x1D), count = rd16(ao + 0x1E);
        ncomp = uv ? (comp == 1 ? 2 : 1) : (comp == 1 ? 3 : 2);
        (void)ncompIfSt;
        static const u32 kSize[5] = {1, 1, 2, 2, 4};
        if (!stride) stride = kSize[std::min(fmt, 4u)] * u32(ncomp);
    };
    auto readComp = [](u32 a, u32 fmt, u32 frac) -> float {
        float v;
        switch (fmt) {
        case 0: v = float(rd8(a)); break;
        case 1: v = float(s8(rd8(a))); break;
        case 2: v = float(rd16(a)); break;
        case 3: v = float(s16(rd16(a))); break;
        default: return rdf32(a);
        }
        return v / float(1u << frac);
    };
    const u32 posArr = ids[0] >= 0 && size_t(ids[0]) < verts.size() ? verts[size_t(ids[0])] : 0;
    const u32 uvArr = ids[4] >= 0 && size_t(ids[4]) < uvs.size() ? uvs[size_t(ids[4])] : 0;
    const u32 clrArr = ids[2] >= 0 && size_t(ids[2]) < colors.size() ? colors[size_t(ids[2])] : 0;
    if (!posArr) return;
    u32 pData, pFmt, pFrac, pStride, pCount, tData = 0, tFmt = 0, tFrac = 0, tStride = 0, tCount = 0;
    int pN, tN = 0;
    arrayInfo(posArr, 3, false, pData, pFmt, pFrac, pStride, pCount, pN);
    if (uvArr) arrayInfo(uvArr, 2, true, tData, tFmt, tFrac, tStride, tCount, tN);
    auto nodeMtx = [&](s32 node, float* m) {
        if (node >= 0 && size_t(node) < bones.size()) {
            for (int k = 0; k < 12; k++) m[k] = rdf32(bones[size_t(node)] + 0x70 + 4 * k);
        } else {
            static const float kId[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            std::memcpy(m, kId, sizeof kId);
        }
    };
    std::unordered_map<u32, u32> slotMtx;
    struct V {
        float p[3], uv[2], c[4];
    };
    auto vertex = [&](u32 pnmtx, u32 pi, s32 ti, s32 ci) {
        V v{};
        float loc[3] = {};
        if (pi < pCount)
            for (int k = 0; k < pN; k++) loc[k] = readComp(pData + pi * pStride + u32(k) * (pFmt >= 4 ? 4u : pFmt >= 2 ? 2u : 1u), pFmt, pFrac);
        s32 node;
        if (single >= 0) {
            node = size_t(single) < mtxNode.size() ? mtxNode[size_t(single)] : single;
        } else {
            const u32 slot = pnmtx / 3;
            const auto it = slotMtx.find(slot);
            const s32 mid = it != slotMtx.end() ? s32(it->second) : (slot < mlist.size() ? mlist[slot] : 0);
            node = mid >= 0 && size_t(mid) < mtxNode.size() ? mtxNode[size_t(mid)] : -1;
        }
        float m[12];
        nodeMtx(node, m);
        for (int r = 0; r < 3; r++) v.p[r] = m[r * 4] * loc[0] + m[r * 4 + 1] * loc[1] + m[r * 4 + 2] * loc[2] + m[r * 4 + 3];
        if (uvArr && ti >= 0 && u32(ti) < tCount)
            for (int k = 0; k < tN; k++)
                v.uv[k] = readComp(tData + u32(ti) * tStride + u32(k) * (tFmt >= 4 ? 4u : tFmt >= 2 ? 2u : 1u), tFmt, tFrac);
        v.c[0] = v.c[1] = v.c[2] = v.c[3] = 1.0f;
        if (clrArr && ci >= 0 && useVtxColor) {
            const u32 cdata = clrArr + u32(s32at(clrArr + 8)), cfmt = rd32(clrArr + 0x18), cstride = rd8(clrArr + 0x1C);
            if (u32(ci) < rd16(clrArr + 0x1E)) {
                const u32 a = cdata + u32(ci) * cstride;
                if (cfmt == 0) rgb565(rd16(a), v.c);
                else if (cfmt == 1 || cfmt == 2) v.c[0] = rd8(a) / 255.0f, v.c[1] = rd8(a + 1) / 255.0f, v.c[2] = rd8(a + 2) / 255.0f;
                else if (cfmt == 3) {
                    const u32 x = rd16(a);
                    v.c[0] = float(x >> 12) / 15.0f, v.c[1] = float((x >> 8) & 15) / 15.0f, v.c[2] = float((x >> 4) & 15) / 15.0f;
                    v.c[3] = float(x & 15) / 15.0f;
                } else if (cfmt == 4) {
                    const u32 x = (u32(rd8(a)) << 16) | (u32(rd8(a + 1)) << 8) | rd8(a + 2);
                    v.c[0] = float((x >> 18) & 63) / 63.0f, v.c[1] = float((x >> 12) & 63) / 63.0f, v.c[2] = float((x >> 6) & 63) / 63.0f;
                    v.c[3] = float(x & 63) / 63.0f;
                } else {
                    for (int k = 0; k < 4; k++) v.c[k] = rd8(a + u32(k)) / 255.0f;
                }
            }
        }
        return v;
    };
    auto addTri = [&](const V& a, const V& b, const V& c) {
        const float e1[3] = {b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2]}, e2[3] = {c.p[0] - a.p[0], c.p[1] - a.p[1], c.p[2] - a.p[2]};
        const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const double ar = 0.5 * std::sqrt(double(cr[0]) * cr[0] + double(cr[1]) * cr[1] + double(cr[2]) * cr[2]) + 1e-6;
        for (int k = 0; k < 3; k++) {
            acc.cen[k] += (a.p[k] + b.p[k] + c.p[k]) / 3.0 * ar;
            acc.lo[k] = std::min({acc.lo[k], a.p[k], b.p[k], c.p[k]});
            acc.hi[k] = std::max({acc.hi[k], a.p[k], b.p[k], c.p[k]});
        }
        acc.area += ar;
        if (!tex) return;
        const V* vs[3] = {&a, &b, &c};
        for (int s = 0; s < 4; s++) {
            float uv[2], vc[3];
            if (s < 3) {
                uv[0] = vs[s]->uv[0], uv[1] = vs[s]->uv[1];
                for (int k = 0; k < 3; k++) vc[k] = vs[s]->c[k];
            } else {
                uv[0] = (a.uv[0] + b.uv[0] + c.uv[0]) / 3, uv[1] = (a.uv[1] + b.uv[1] + c.uv[1]) / 3;
                for (int k = 0; k < 3; k++) vc[k] = (a.c[k] + b.c[k] + c.c[k]) / 3;
            }
            float tx[4];
            texel(*tex, int(std::floor(uv[0] * float(tex->w))), int(std::floor(uv[1] * float(tex->h))), tx);
            const double w = ar * std::max(tx[3], 0.05f);
            for (int k = 0; k < 3; k++) acc.col[k] += double(tx[k] * vc[k]) * w;
            acc.wsum += w;
            {
                const float c0 = tx[0] * vc[0], c1 = tx[1] * vc[1], c2 = tx[2] * vc[2];
                const double chroma = double(std::max({c0, c1, c2}) - std::min({c0, c1, c2}));
                const double cw = w * (0.02 + chroma * chroma * 8.0);
                acc.ccol[0] += c0 * cw, acc.ccol[1] += c1 * cw, acc.ccol[2] += c2 * cw;
                acc.cwsum += cw;
            }
        }
    };
    std::vector<V> vs;
    for (u32 o = prim, end = prim + primSize; o < end;) {
        const u8 op = rd8(o);
        if (op == 0) {
            o++;
            continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            if (op == 0x20) slotMtx[(rd16(o + 3) & 0xFFF) / 12] = rd16(o + 1);
            o += 5;
            continue;
        }
        const u8 p = op & 0xF8;
        if (p == 0x80 || p == 0x90 || p == 0x98 || p == 0xA0) {
            const u32 cnt = rd16(o + 1);
            o += 3;
            vs.clear();
            for (u32 i = 0; i < cnt; i++) {
                u32 pn = 0, pi = 0;
                s32 ti = -1, ci = -1;
                for (const auto& at : attrs) {
                    if (at.kind <= 1) {
                        if (at.kind == 0) pn = rd8(o);
                        o += 1;
                        continue;
                    }
                    u32 idx = 0;
                    if (at.type == 2) idx = rd8(o), o += 1;
                    else if (at.type == 3) idx = rd16(o), o += 2;
                    if (at.kind == 2) pi = idx;
                    else if (at.kind == 4) ci = s32(idx);
                    else if (at.kind == 6) ti = s32(idx);
                }
                vs.push_back(vertex(pn, pi, ti, ci));
            }
            if (p == 0x90) {
                for (u32 i = 0; i + 2 < cnt; i += 3) addTri(vs[i], vs[i + 1], vs[i + 2]);
            } else if (p == 0x98) {
                for (u32 i = 0; i + 2 < cnt; i++)
                    if (i % 2 == 0) addTri(vs[i], vs[i + 1], vs[i + 2]);
                    else addTri(vs[i + 1], vs[i], vs[i + 2]);
            } else if (p == 0xA0) {
                for (u32 i = 1; i + 1 < cnt; i++) addTri(vs[0], vs[i], vs[i + 1]);
            } else {
                for (u32 i = 0; i + 3 < cnt; i += 4) addTri(vs[i], vs[i + 1], vs[i + 2]), addTri(vs[i], vs[i + 2], vs[i + 3]);
            }
            continue;
        }
        if (op == 0x08) {
            o += 6;
            continue;
        }
        if (op == 0x10) {
            o += 5 + 4 * ((rd32(o + 1) >> 16) + 1);
            continue;
        }
        if (op == 0x61) {
            o += 5;
            continue;
        }
        break;
    }
}

Light analyse(u32 mdl, const std::string& modelName, const ItemInfo* item) {
    Light L;
    L.model = modelName;
    if (!gptr(mdl) || rd32(mdl) != 0x4D444C30u) return L;
    const u32 ver = rd32(mdl + 8);
    const u32 ns = ver >= 10 ? 14 : 11;
    auto sec = [&](u32 k) -> u32 {
        const s32 o = s32at(mdl + 0x10 + 4 * k);
        return o ? mdl + u32(o) : 0u;
    };
    const u32 kMats = ns == 14 ? 8 : 6, kShaders = ns == 14 ? 9 : 7, kObjs = ns == 14 ? 10 : 8;
    const u32 info = mdl + 0x10 + 4 * ns + 4;
    float bbLo[3], bbHi[3];
    for (int k = 0; k < 3; k++) bbLo[k] = rdf32(info + 0x28 + 4 * k), bbHi[k] = rdf32(info + 0x34 + 4 * k);
    std::vector<s32> mtxNode;
    {
        const u32 t = info + u32(s32at(info + 0x24));
        for (u32 k = 0, n = std::min(rd32(t), 256u); k < n; k++) mtxNode.push_back(s32at(t + 4 + 4 * k));
    }
    std::vector<u32> bones, verts, colors, uvs;
    for (auto& [n, a] : group(sec(1))) bones.push_back(a);
    for (auto& [n, a] : group(sec(2))) verts.push_back(a);
    for (auto& [n, a] : group(sec(4))) colors.push_back(a);
    for (auto& [n, a] : group(sec(5))) uvs.push_back(a);
    const auto objs = group(sec(kObjs));
    // brres folders: animation names, textures, palettes
    const u32 brres = mdl + u32(s32at(mdl + 0xC));
    std::unordered_map<std::string, u32> texs, plts;
    std::string animNames;
    if (gptr(brres) && rd32(brres) == 0x62726573u) {  // 'bres'
        const u32 root = brres + rd16(brres + 0xC);
        for (auto& [folder, fa] : group(root + 8)) {
            const auto items = group(fa);
            if (folder.rfind("Textures", 0) == 0)
                for (auto& [n, a] : items) texs[n] = a;
            else if (folder.rfind("Palettes", 0) == 0)
                for (auto& [n, a] : items) plts[n] = a;
            else if (folder.rfind("Anm", 0) == 0)
                for (auto& [n, a] : items) animNames += " " + n;
        }
    }
    const std::string modelText = modelName + " " + (item ? item->name : std::string()) + animNames;
    const u8 modelHint = nameHint(modelText);
    const bool water = isWater(modelText);
    const Exception* ex = item ? exceptionFor(item->number) : nullptr;
    if (ex && ex->drop) return L;
    // materials
    std::vector<Mat> mats;
    for (auto& [name, ma] : group(sec(kMats))) {
        Mat m;
        m.name = name, m.addr = ma;
        const u32 nChan = rd8(ma + 0x15), nStage = rd8(ma + 0x16);
        const u32 chan = ma + (ver >= 10 ? 0x3F0u : 0x3E8u);
        const bool lit = nChan && (rd32(chan + 12) & 2);
        const u32 sh = ma + u32(s32at(ma + 0x28));
        std::unordered_map<u8, u32> sbp;
        bpWrites(sh + 0x20, sh + std::min(rd32(sh), 0x800u), sbp);
        bool readsRas = false;
        for (u32 s = 0; s < nStage && s < 16; s++) {
            const u32 cv = sbp.count(u8(0xC0 + 2 * s)) ? sbp[u8(0xC0 + 2 * s)] : 0u;
            const u32 ia = (cv >> 12) & 15, ib = (cv >> 8) & 15, ic = (cv >> 4) & 15, id = cv & 15;
            for (u32 x : {ia, ib, ic, id}) readsRas |= x == 10 || x == 11;
            if (ia == 10 && (ib == 11 || ib == 12 || ib == 2 || ib == 4 || ib == 6 || ib == 14) &&
                (ic == 14 || ic == 3 || ic == 5 || ic == 7 || ic == 1))
                m.bypass = true;
        }
        m.unlit = nStage && (!lit || !readsRas);
        const u32 dl = ma + u32(s32at(ma + 0x3C));
        std::unordered_map<u8, u32> mbp;
        bpWrites(dl, dl + 0x20, mbp);
        const u32 blend = mbp.count(0x41) ? mbp[0x41] : 0u;
        m.additive = (blend & 1) && ((blend >> 5) & 7) == 1;
        std::string texNames;
        const u32 ntex = std::min(rd32(ma + 0x2C), 8u), ref = ma + u32(s32at(ma + 0x30));
        for (u32 k = 0; k < ntex; k++) {
            const u32 r = ref + k * 0x34;
            const std::string tn = s32at(r) ? gstr(r + u32(s32at(r))) : std::string();
            const std::string pn = s32at(r + 4) ? gstr(r + u32(s32at(r + 4))) : std::string();
            if (k == 0) {
                m.tex = tn, m.pal = pn;
                // bound texture (g3d writes the TEX0 / PLT0 it found, e.g. in the shared
                // int_common.brres for fire / screen textures, into the reference)
                for (u32 w = 8; w < 0x34; w += 4) {
                    const u32 p = rd32(r + w);
                    if (gptr(p) && !(p & 3) && rd32(p) == 0x54455830u) m.boundTex = p;      // 'TEX0'
                    else if (gptr(p) && !(p & 3) && rd32(p) == 0x504C5430u) m.boundPal = p;  // 'PLT0'
                }
            }
            texNames += " " + tn;
        }
        m.hint = nameHint(m.name + texNames);
        if (m.name == "lp_m") m.game = true, m.gameKind = KLamp;
        else if (m.name == "mn_m" || m.name == "tv_m") m.game = true, m.gameKind = KScreen;
        else if (m.name == "fr_m") m.game = true, m.gameKind = KFire;
        else if (m.name == "wa_m" || m.name == "wb_m") m.game = true, m.gameKind = KWater;
        mats.push_back(m);
    }
    // which materials glow (the scanner's rules)
    std::unordered_set<std::string> glow;
    std::vector<u8> kinds;
    bool anyGame = false;
    for (const auto& m : mats) {
        const bool generic = m.name.rfind("df_", 0) == 0 || m.name.rfind("ev", 0) == 0;
        const u8 guess = water ? u8(KWater) : (m.hint ? m.hint : modelHint ? modelHint : (m.name.rfind("tp_", 0) == 0 ? u8(KScreen) : u8(KLamp)));
        u8 k = KNone;
        if (m.game) k = m.gameKind, anyGame = true;
        else if (m.bypass || m.additive || (m.unlit && !generic)) k = guess;
        else if (m.unlit && m.hint) k = m.hint;
        else if ((m.hint == KFire || m.hint == KNeon || m.hint == KLava) && !generic) k = m.hint;
        if (k == KNone) continue;
        glow.insert(m.name);
        kinds.push_back(k);
        if (m.game && k != KWater) L.gameMats.push_back(m.name);
    }
    bool whole = false;
    if (glow.empty() && (modelHint == KFire || modelHint == KLava || modelHint == KNeon)) whole = true, kinds.push_back(modelHint);
    if (glow.empty() && !whole && item && item->lightKind) whole = true, kinds.push_back(KLamp);
    if (ex) {  // an addition: its own part and numbers
        glow.clear(), kinds.assign(1, ex->kind), L.gameMats.clear();
        whole = !ex->mat;
        if (ex->mat) glow.insert(ex->mat);
    }
    if (glow.empty() && !whole) return L;
    // kind
    auto has = [&](u8 k) { return std::find(kinds.begin(), kinds.end(), k) != kinds.end(); };
    u8 kind;
    if (ex) kind = ex->kind;
    else if (modelHint == KFire || has(KFire)) kind = KFire;
    else if (has(KScreen)) kind = KScreen;
    else if (water || std::all_of(kinds.begin(), kinds.end(), [](u8 k) { return k == KWater; })) kind = KWater;
    else if (anyGame) kind = KLamp;  // the (a) material's kind (lamps; fire / screens handled above)
    else kind = kinds.front();
    if (kind == KWater) return L;
    // geometry of the glowing parts (all draws when none / whole model)
    Acc part, all;
    for (auto& [dn, da] : group(sec(0))) {
        if (dn.rfind("Draw", 0) != 0) continue;
        for (u32 o = da; rd8(o) == 4; o += 8) {
            const u32 mi = rd16(o + 1), oi = rd16(o + 3);
            if (mi >= mats.size() || oi >= objs.size()) continue;
            const Mat& m = mats[mi];
            TexRef t;
            const TexRef* tp = nullptr;
            u32 ta = 0;
            if (auto it = texs.find(m.tex); it != texs.end()) ta = it->second;
            else if (m.boundTex) ta = m.boundTex;  // a shared texture (int_common.brres)
            if (ta) {
                t.data = ta + u32(s32at(ta + 0x10)), t.w = rd16(ta + 0x1C), t.h = rd16(ta + 0x1E), t.fmt = rd32(ta + 0x20);
                if (t.fmt >= 8 && t.fmt <= 10) {
                    auto pit = plts.find(m.pal);
                    if (pit == plts.end()) pit = plts.find(m.tex);
                    u32 pa = 0;
                    if (pit != plts.end()) pa = pit->second;
                    else if (m.boundPal) pa = m.boundPal;
                    else if (!plts.empty()) pa = plts.begin()->second;
                    if (pa) {
                        t.pal = pa + u32(s32at(pa + 0x10)), t.palFmt = rd32(pa + 0x18), t.palCount = rd16(pa + 0x1C);
                    }
                }
                tp = &t;
            }
            const bool isGlow = whole || glow.count(m.name);
            accumulateObject(mdl, objs[oi].second, verts, uvs, colors, bones, mtxNode, tp, m.name != "lp_m", isGlow ? part : all);
            if (isGlow) accumulateObject(mdl, objs[oi].second, verts, uvs, colors, bones, mtxNode, nullptr, false, all);
        }
    }
    if (part.area <= 0) return L;
    for (int k = 0; k < 3; k++) L.centre[k] = float(part.cen[k] / part.area);
    float lo[3], hi[3];
    for (int k = 0; k < 3; k++) lo[k] = std::min(all.lo[k], bbLo[k]), hi[k] = std::max(all.hi[k], bbHi[k]);
    // A small flame (candle: the wax and its flame are one part) casts shadows from its tip: from
    // the part's centre the wax around the light would hide it from its own shadow map.
    const bool smallFlame = kind == KFire && std::max(part.hi[0] - part.lo[0], part.hi[2] - part.lo[2]) < 20.0f;
    if (smallFlame) L.centre[1] = part.hi[1] + 2.0f;
    std::memcpy(L.oldCentre, L.centre, sizeof L.centre);
    // Lamps light from their glowing part's centre and cast no shadow (second pass): the first pass
    // put the light 2 units below the shade's lower rim for a shadow origin, but a lamp's glowing
    // material often spans the base too (the lovely lamp's light went to the floor) or the rim sits
    // in the lamp's own body (the regal lamp's table top hid most of its light).
    if (ex && ex->heightShare >= 0.0f) {
        L.centre[0] = (lo[0] + hi[0]) / 2, L.centre[2] = (lo[2] + hi[2]) / 2;
        L.centre[1] = lo[1] + ex->heightShare * (hi[1] - lo[1]);
        std::memcpy(L.oldCentre, L.centre, sizeof L.centre);
    }
    // colour / radius / intensity (the scanner's propose())
    float c[3] = {1.0f, 0.85f, 0.6f};
    if (part.wsum > 0)
        for (int k = 0; k < 3; k++) c[k] = std::max(float(part.col[k] / part.wsum), 0.0f);
    float mx = std::max({c[0], c[1], c[2], 1e-3f});
    for (float& v : c) v = 0.35f + 0.65f * v / mx;
    {
        // the numbers before branch indoor-light (test aids CF_INDOOR_LEGACY / CF_INDOOR_AB)
        float o[3] = {c[0], c[1], c[2]};
        static const float kCool0[3] = {0.62f, 0.80f, 1.0f}, kWarm0[3] = {1.0f, 0.62f, 0.3f};
        if (kind == KScreen)
            for (int k = 0; k < 3; k++) o[k] = 0.45f * o[k] + 0.55f * kCool0[k];
        if (kind == KFire)
            for (int k = 0; k < 3; k++) o[k] = 0.6f * o[k] + 0.4f * kWarm0[k];
        const float om = std::max({o[0], o[1], o[2], 1e-3f});
        for (int k = 0; k < 3; k++) L.oldColor[k] = o[k] / om;
        const float size0 = std::max({bbHi[0] - bbLo[0], bbHi[1] - bbLo[1], bbHi[2] - bbLo[2]});
        L.oldRadius = std::clamp(110.0f + 2.2f * size0, 120.0f, 320.0f);
        static const float kInt0[7] = {1.0f, 1.4f, 0.8f, 1.6f, 0.0f, 1.0f, 0.9f};
        L.oldIntensity = kInt0[kind] * (item && item->lightKind == 2 ? 1.15f : 1.0f);
        if (ex) {
            if (ex->color[0] >= 0) std::memcpy(L.oldColor, ex->color, sizeof L.oldColor);
            L.oldRadius = ex->radius, L.oldIntensity = ex->intensity;
        }
    }
    // Colours and reach (branch indoor-light, second pass after the user's review: "the candle should
    // only light its corner, in flickering red / orange"). Flames are deep orange-red (~1700-1900 K;
    // the texture only tints them), lamps warm but softer, screens their picture's colour with a
    // touch of cool. The colour is made vivid afterwards (F1 "Light vibrance", vividFurn).
    // Third pass: the light's hue from the glowing part's texels weighted by their chroma (the plain
    // mean let white and grey parts wash a pink shade out to salmon, a blue lava lamp to near white).
    if (part.cwsum > 0) {
        for (int k = 0; k < 3; k++) c[k] = std::max(float(part.ccol[k] / part.cwsum), 0.0f);
        const float cm = std::max({c[0], c[1], c[2], 1e-3f});
        for (float& v : c) v /= cm;
    }
    const bool fire = kind == KFire;
    static const float kCool[3] = {0.62f, 0.80f, 1.0f}, kWarm[3] = {1.0f, 0.86f, 0.66f};
    static const float kCandle[3] = {1.0f, 0.36f, 0.08f}, kHearth[3] = {1.0f, 0.45f, 0.12f};
    if (kind == KScreen)
        for (int k = 0; k < 3; k++) c[k] = 0.85f * c[k] + 0.15f * kCool[k];
    if (kind == KLamp)
        {  // a coloured shade keeps its colour, a plain one turns warm white
            const float sat = 1.0f - std::min({c[0], c[1], c[2]}) / std::max({c[0], c[1], c[2], 1e-3f});
            const float keep = sat > 0.25f ? 0.85f : 0.4f;
            for (int k = 0; k < 3; k++) c[k] = keep * c[k] + (1.0f - keep) * kWarm[k];
        }
    if (fire)
        for (int k = 0; k < 3; k++) c[k] = 0.1f * c[k] + 0.9f * (smallFlame ? kCandle[k] : kHearth[k]);
    mx = std::max({c[0], c[1], c[2], 1e-3f});
    for (int k = 0; k < 3; k++) L.color[k] = c[k] / mx;
    // Physical falloff (Aurora 0480 ExtraLightPhysical: inverse square from a small core, windowed
    // to 0 at the reach): a strong pool around the source with a short cut-off.
    const float size = std::max({bbHi[0] - bbLo[0], bbHi[1] - bbLo[1], bbHi[2] - bbLo[2]});
    //                             none  lamp  screen fire  water neon  lava
    static const float kInt[7] = {3.4f, 6.5f, 3.4f, 6.0f, 0.0f, 5.0f, 5.0f};
    static const float kCore[7] = {0.09f, 0.12f, 0.14f, 0.09f, 0.1f, 0.10f, 0.10f};
    static const float kReach[7] = {220.0f, 360.0f, 230.0f, 380.0f, 0.0f, 260.0f, 260.0f};
    L.radius = kReach[kind] + std::min(size, 80.0f);
    L.intensity = kInt[kind];
    L.core = kCore[kind];
    if (smallFlame) L.radius = 180.0f, L.core = 0.08f, L.intensity = 7.0f;  // a candle: its table / corner
    if (item && item->lightKind == 2 && !fire) L.intensity *= 1.1f;
    if (ex) {
        if (ex->color[0] >= 0 && !fire) std::memcpy(L.color, ex->color, sizeof L.color);
        if (!fire) L.radius = ex->radius * 1.1f, L.intensity = ex->intensity * 3.0f;  // (numbers made for the old falloff)
        else L.radius = 260.0f;                                                       // the tall lantern: a lantern's reach
    }
    L.kind = kind;
    L.flicker = ex ? ex->flicker : kind == KFire;
    L.shadow = kind == KFire || kind == KScreen;  // (lamps: the light inside the shade, no shadow)
    // the game brightens the whole room for its lamps (light kind 1 and 2: the lovely lamp, kind 1,
    // brightened the room too) and candles
    L.roomLamp = (item && item->lightKind != 0) || kind == KFire;
    L.lit = true;
    if (sLog)
        log("[furn] model %08X '%s' item %04X '%s': %s at %.1f %.1f %.1f colour %.2f %.2f %.2f r %.0f I %.2f%s mats %zu glow%s%s",
            mdl, modelName.c_str(), item ? item->number : 0, item ? item->name.c_str() : "", kindName(kind), L.centre[0], L.centre[1],
            L.centre[2], L.color[0], L.color[1], L.color[2], L.radius, L.intensity, L.flicker ? " flicker" : "", mats.size(),
            whole ? " (whole model)" : "", ex ? " (exception)" : "");
    return L;
}

// ---- per frame ----
struct Drawn {
    u32 obj, mdl;
    std::string name;
};
std::vector<Drawn> sCur, sPrev;
std::unordered_set<u32> sCurSet;
u32 sCurFrame = ~0u;

// Smooth 1D value noise in 0..1 (hashed lattice values, smoothstep between them).
float flameNoise(float x, u32 seed) {
    const float fl = std::floor(x);
    const s32 i = s32(fl);
    auto h = [&](s32 k) {
        u32 v = u32(k) * 0x27D4EB2Du ^ seed;
        v ^= v >> 15, v *= 0x85EBCA6Bu, v ^= v >> 13, v *= 0xC2B2AE35u, v ^= v >> 16;
        return float(v & 0xFFFFFFu) / float(0xFFFFFF);
    };
    const float f = x - fl, u = f * f * (3.0f - 2.0f * f);
    return h(i) + (h(i + 1) - h(i)) * u;
}

// F1 "Light vibrance" for furniture lights (LM's "Light color vibrance" idea, branch indoor-light):
// the colour's HSV saturation x v (up to fully saturated), hue and brightest channel kept, so a
// flame stays deep orange instead of drifting toward white.
void vividFurn(const float in[3], float v, float out[3]) {
    const float mx = std::max({in[0], in[1], in[2]}), mn = std::min({in[0], in[1], in[2]});
    if (!(mx > 1e-5f)) {
        std::memcpy(out, in, 3 * sizeof(float));
        return;
    }
    const float sat = (mx - mn) / mx, ns = std::clamp(sat * v, 0.0f, 1.0f);
    // each channel's distance below the max scales with the saturation
    for (int k = 0; k < 3; k++) out[k] = mx - (mx - in[k]) * (sat > 1e-5f ? ns / sat : 0.0f);
}

float hostSeconds() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<float>(steady_clock::now() - t0).count();
}

}  // namespace

bool enabled() {
    return enh::masterOn() && port::settings().enhancedLighting.furnitureLights;
}

bool litRecently() { return copyDispCount() - sLitFrame < 30; }

void noteModel(u32 obj, u32 mdl, const std::string& name) {
    if (!mdl || name.rfind("int_", 0) != 0 || !enabled()) return;
    if (copyDispCount() != sCurFrame) {
        sCurFrame = copyDispCount();
        sCurSet.clear();
    }
    if (!sCurSet.insert(obj).second || sCur.size() >= 256) return;
    sCur.push_back({obj, mdl, name});
}

void submit(const float* view, bool viewValid) {
    sPrev.swap(sCur);
    sCur.clear();
    // A new place reuses the heap: other models (and actors) at the same addresses. Every cache
    // keyed by guest addresses starts over at a stage change.
    if (sR13) {
        const int stage = rd8(sR13 - 0x6384);
        if (stage != sStage) {
            if (sLog && sStage >= 0) log("[furn] f%u stage %d -> %d: caches cleared", copyDispCount(), sStage, stage);
            sStage = stage;
            sLights.clear(), sItemOf.clear(), sAmount.clear(), sDone.clear();
        }
    }
    if (!enabled() || !viewValid || sPrev.empty()) return;
    // Test aid CF_FURN_AB=1: furniture lights only in every other frame pair (dump numbers 4k+1
    // with, 4k+3 without), so neighbouring shots compare them.
    static const bool sAb = std::getenv("CF_FURN_AB") != nullptr;
    if (sAb && ((copyDispCount() + 1) / 2) % 2 == 1) return;
    const auto& s = port::settings().enhancedLighting;
    const float t = hostSeconds();
    // Test aid CF_FURN_FLICKER=0: steady flames (frame compares).
    static const bool sFlicker = [] {
        const char* v = std::getenv("CF_FURN_FLICKER");
        return !v || std::atoi(v) != 0;
    }();
    std::array<aurora::gfx::ExtraLight, 48> out{};
    const bool legacy = cflight::indoorLegacy();
    bool roomLamp = false;  // a lit lamp / candle the game brightens the whole room for
    u32 n = 0;
    for (const auto& d : sPrev) {
        // still the same model there (the furniture may have been put away since)
        bool same = false;
        for (u32 off = 0xC; off < 0x180 && !same; off += 4) same = rd32(d.obj + off) == d.mdl;
        if (!same) continue;
        const ItemInfo* item = nullptr;
        if (auto it = sItemOf.find(d.mdl); it != sItemOf.end()) item = &it->second;
        const u64 k = key2(d.mdl, item ? item->number : 0u);
        auto it = sLights.find(k);
        if (it != sLights.end() && it->second.model != d.name) sLights.erase(it), it = sLights.end();
        if (it == sLights.end()) {
            if (sLights.size() > 2048) sLights.clear();
            it = sLights.emplace(k, analyse(d.mdl, d.name, item)).first;
        }
        const Light& L = it->second;
        if (!L.lit) continue;
        // the game's own on / off (glow helpers of the model's switchable materials)
        float amount = 1.0f;
        if (!L.gameMats.empty()) {
            bool seen = false;
            float a = 0.0f;
            for (const auto& m : L.gameMats)
                if (auto at = sAmount.find(matKey(d.mdl, m)); at != sAmount.end() && copyDispCount() - at->second.second < 120)
                    seen = true, a = std::max(a, at->second.first);
            if (seen) amount = a;
        }
        if (amount <= 0.01f) continue;
        float W[12];
        for (int r = 0; r < 12; r++) W[r] = rdf32(d.obj + 0x3C + 4 * r);
        float w[3], v[3];
        const float* C = legacy ? L.oldCentre : L.centre;
        for (int r = 0; r < 3; r++) w[r] = W[r * 4] * C[0] + W[r * 4 + 1] * C[1] + W[r * 4 + 2] * C[2] + W[r * 4 + 3];
        for (int r = 0; r < 3; r++) v[r] = view[r * 4] * w[0] + view[r * 4 + 1] * w[1] + view[r * 4 + 2] * w[2] + view[r * 4 + 3];
        if (!std::isfinite(v[0] + v[1] + v[2])) continue;
        const float scale = std::sqrt(W[0] * W[0] + W[4] * W[4] + W[8] * W[8]);
        float flick = 1.0f;
        float jit[3] = {0.0f, 0.0f, 0.0f};
        if (L.flicker && legacy) {
            const float ph = float(d.obj & 0xFFF) * 0.0137f;
            flick = 1.0f + 0.10f * std::sin(t * 9.1f + ph) + 0.06f * std::sin(t * 15.3f + ph * 1.7f) + 0.04f * std::sin(t * 23.7f + ph * 2.3f);
        } else if (L.flicker && sFlicker) {
            // A flame: irregular, slow-ish flicker of +-20 % (smoothed value noise at ~1.3, ~4 and
            // ~9 Hz, not a sine: a sine reads as a pulse), occasional deeper gutters, and the light
            // moves a little with the flame (sideways more than up), so its shadows waver too. Host
            // time, per-object seed; lights are interpolated by id, so 120 fps is smooth.
            const u32 seed = d.obj * 2654435761u;
            const float slow = flameNoise(t * 1.3f, seed), mid = flameNoise(t * 4.1f, seed ^ 0x9E37u), fast = flameNoise(t * 9.3f, seed ^ 0x51EDu);
            const float gutter = std::max(0.0f, flameNoise(t * 0.7f, seed ^ 0x3C6Fu) - 0.78f) * 2.5f;  // 0..0.55, now and then
            flick = 1.0f + 0.38f * (0.45f * slow + 0.35f * mid + 0.20f * fast - 0.5f) - 0.35f * gutter;
            const float amp = 1.4f * (std::isfinite(scale) && scale > 0.1f && scale < 10.0f ? scale : 1.0f);
            const float wx = amp * 2.0f * (flameNoise(t * 2.2f, seed ^ 0x1111u) - 0.5f);
            const float wz = amp * 2.0f * (flameNoise(t * 2.6f, seed ^ 0x2222u) - 0.5f);
            const float wy = amp * 1.2f * (fast - 0.5f);
            // world offset -> view (rotation part of the view matrix)
            for (int r = 0; r < 3; r++) jit[r] = view[r * 4] * wx + view[r * 4 + 1] * wy + view[r * 4 + 2] * wz;
        }
        aurora::gfx::ExtraLight l{};
        for (int r = 0; r < 3; r++) l.pos[r] = v[r] + jit[r];
        {
            // Plain warm lamps get only part of the vibrance (warm but softer); a coloured shade
            // (saturation over 0.2: the lovely lamp's pink-red) and lava / neon lamps the full one,
            // the latter at least 60 % saturated (their texture mean is pale: the blue lava lamp
            // gave (0.89, 0.93, 1.0)), as their light colours the room.
            const float vib = std::clamp(s.furnitureVibrance, 0.0f, 2.5f);
            const float mx = std::max({L.color[0], L.color[1], L.color[2]}), mn = std::min({L.color[0], L.color[1], L.color[2]});
            const float sat = mx > 1e-5f ? (mx - mn) / mx : 0.0f;
            float v = vib;
            if (L.kind == KLamp && sat <= 0.2f) v = 1.0f + (vib - 1.0f) * 0.35f;
            if ((L.kind == KLava || L.kind == KNeon) && sat > 0.02f && vib > 1.0f) v = std::max(v, 0.6f / sat);
            vividFurn(L.color, v, l.color);
        }
        l.radius = L.radius * (std::isfinite(scale) && scale > 0.1f && scale < 10.0f ? scale : 1.0f);
        l.intensity = L.intensity * amount * flick * std::clamp(s.furnitureLightIntensity, 0.0f, 3.0f);
        l.flags = aurora::gfx::ExtraLightTranslucent | (L.kind == KFire ? aurora::gfx::ExtraLightFire : 0u) |
                  (L.shadow && s.furnitureLightShadows ? aurora::gfx::ExtraLightCastShadows : 0u) | aurora::gfx::ExtraLightPhysical;
        l.box[0] = L.core;
        if (legacy) {
            std::memcpy(l.color, L.oldColor, sizeof l.color);
            l.radius = L.oldRadius * (std::isfinite(scale) && scale > 0.1f && scale < 10.0f ? scale : 1.0f);
            l.intensity = L.oldIntensity * amount * flick * std::clamp(s.furnitureLightIntensity, 0.0f, 3.0f);
            l.flags = aurora::gfx::ExtraLightTranslucent | (L.kind == KFire ? aurora::gfx::ExtraLightFire : 0u) |
                      ((L.kind == KFire || L.kind == KScreen) && s.furnitureLightShadows ? aurora::gfx::ExtraLightCastShadows : 0u);
            l.box[0] = 0.0f;
        }
        l.id = 0x46000000u ^ (d.obj & 0x00FFFFFFu);
        out[n++] = l;
        if (L.roomLamp && !legacy) roomLamp = true;
        if (n == out.size()) break;
    }
    submitExtraLights(out.data(), n);
    if (roomLamp) sLitFrame = copyDispCount();
    if (sLog && copyDispCount() % 120 == 0)
        for (u32 i = 0; i < n; i++)
            log("[furn] f%u light %u id %08X view %.1f %.1f %.1f colour %.2f %.2f %.2f r %.0f I %.2f flags %X", copyDispCount(), i, out[i].id,
                out[i].pos[0], out[i].pos[1], out[i].pos[2], out[i].color[0], out[i].color[1], out[i].color[2], out[i].radius,
                out[i].intensity, out[i].flags);
}

// d_a_ftrNP fn_15_767C: a glow helper's per-frame update (r3 = helper: +0 amount, +0x14 ResMatData).
void noteHelper(u32 h) {
    const u32 mat = rd32(h + 0x14);
    if (!gptr(mat) || (mat & 3)) return;
    const float a = rdf32(h);
    if (!std::isfinite(a)) return;
    const u32 mdl = mat + u32(s32at(mat + 4));
    const std::string name = gstr(mat + u32(s32at(mat + 8)), 32);
    auto [it, fresh] = sAmount.try_emplace(matKey(mdl, name), -1.0f, 0u);
    auto& e = it->second;
    if (sLog && fresh) log("[furn] f%u helper %08X model %08X '%s' amount %.2f", copyDispCount(), h, mdl, name.c_str(), a);
    else if (sLog >= 2 && std::fabs(e.first - a) > 0.05f)
        log("[furn] f%u helper %08X model %08X '%s' amount %.2f -> %.2f", copyDispCount(), h, mdl, name.c_str(), e.first, a);
    e = {std::clamp(a, 0.0f, 1.0f), copyDispCount()};
    if (sAmount.size() > 4096) sAmount.clear();
    // The lit part glows by itself (no moon shadow / AO on it): Aurora 0420's emissive hint, as
    // cf_light.cpp gives the structures' lit materials. Not for water (wa_m / wb_m).
    if (name != "wa_m" && name != "wb_m") {
        const s32 dl = s32at(mat + 0x3C);
        bool fire = false;  // a flame's material: also a bloom source (its own glow)
        for (const auto& [key, L] : sLights)
            if (u32(key >> 32) == mdl && L.lit && L.kind == KFire) fire = true;
        if (dl) cflight::markLitMaterial(mat + u32(dl) + 0x20, e.first, fire);
    }
}

// d_a_ftrNP fn_15_3100 (daFTR_c per-frame update, r3 = actor): its model -> its item record.
void noteActor(Ctx& c, u32 actor) {
    sR13 = c.r[13];
    const u32 rec = rd32(actor + 0x3A0);
    if (!gptr(rec)) return;
    if (auto it = sDone.find(actor); it != sDone.end() && it->second == rec) return;
    const u32 vt = rd32(actor + 0x60);
    if (!gptr(vt)) return;
    const u32 fn = rd32(vt + 0x210);
    if (!gptr(fn)) return;
    u32 mdl = guestCall(c, fn, actor);
    if (gptr(mdl) && rd32(mdl) != 0x4D444C30u && gptr(rd32(mdl)) && rd32(rd32(mdl)) == 0x4D444C30u) mdl = rd32(mdl);
    if (!gptr(mdl) || rd32(mdl) != 0x4D444C30u) return;
    if (sDone.size() > 2048) sDone.clear();
    sDone[actor] = rec;
    ItemInfo info;
    info.number = rd16(rec + 8);
    info.lightKind = u8((rd8(rec + 0x187) >> 6) & 3);
    for (u32 k = 0; k < 16; k++) {
        const u32 ch = rd16(rec + 0x12 + 0x22 + 2 * k);
        if (!ch) break;
        info.name += ch < 128 ? char(ch) : '?';
    }
    auto [it, fresh] = sItemOf.try_emplace(mdl, info);
    if (!fresh && it->second.number != info.number) {
        it->second = info;
        for (auto li = sLights.begin(); li != sLights.end();)  // re-analyse with the new item
            li = (li->first >> 32) == mdl ? sLights.erase(li) : std::next(li);
    }
    if (sLog)
        log("[furn] actor %08X item %04X '%s' model %08X at %.1f %.1f %.1f (vtable %08X, is-on %08X, on bytes +408 %02X %02X %02X)", actor,
            info.number, info.name.c_str(), mdl, rdf32(actor + 0x64), rdf32(actor + 0x68), rdf32(actor + 0x6C), vt, rd32(vt + 0x1B0),
            rd8(actor + 0x408), rd8(actor + 0x409), rd8(actor + 0x40A));
}

}  // namespace cffurn

static void ftrGlowHelper(Ctx& c) {
    const u32 h = c.r[3];
    callOriginal(c, "d_a_ftrNP::fn_15_767C");
    if (cffurn::enabled()) cffurn::noteHelper(h);
}
static HleReg sFtrGlowReg("d_a_ftrNP::fn_15_767C", &ftrGlowHelper);

namespace cffurn {
// FTR= route action: every furniture of that item switched during the next 30 frames (each once).
u16 sSwitchItem = 0;
int sSwitchOn = 0;
u32 sSwitchUntil = 0;
std::unordered_set<u32> sSwitched;
void requestSwitch(u16 item, int on) {
    sSwitchItem = item, sSwitchOn = on, sSwitchUntil = copyDispCount() + 30;
    sSwitched.clear();
}
}  // namespace cffurn
static bool gptrFtr(u32 p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }

static void ftrExecute(Ctx& c) {
    const u32 actor = c.r[3];
    callOriginal(c, "d_a_ftrNP::fn_15_3100");
    // Route action FTR=<item hex>/<0|1> (test aid): switch that furniture through its own switch
    // (daFTR_c virtual +0x1C4 (this, on, immediate = 0), what touching it with A calls via fn_15_A88).
    if (cffurn::sSwitchItem && copyDispCount() > cffurn::sSwitchUntil) cffurn::sSwitchItem = 0;
    if (cffurn::sSwitchItem && gptrFtr(rd32(actor + 0x3A0)) && rd16(rd32(actor + 0x3A0) + 8) == cffurn::sSwitchItem &&
        cffurn::sSwitched.insert(actor).second) {
        const u32 vt = rd32(actor + 0x60);
        const u32 r3 = c.r[3], r4 = c.r[4];
        if (gptrFtr(vt) && gptrFtr(rd32(vt + 0x1C4))) {
            log("[furn] f%u switch item %04X actor %08X -> %d", copyDispCount(), cffurn::sSwitchItem, actor, cffurn::sSwitchOn);
            guestCall(c, rd32(vt + 0x1C4), actor, u32(cffurn::sSwitchOn), 0);
        }
        c.r[3] = r3, c.r[4] = r4;
    }
    if (cffurn::enabled()) {
        // keep the return registers the caller may look at
        const u32 r3 = c.r[3], r4 = c.r[4];
        cffurn::noteActor(c, actor);
        c.r[3] = r3, c.r[4] = r4;
    }
}
static HleReg sFtrExecReg("d_a_ftrNP::fn_15_3100", &ftrExecute);

// The game's own room light (branch indoor-light; docs/RENDERING.md "Indoor lighting"): fn_80197E1C
// is dEnvAmbLight_c's per-frame update (vtable 0x804FAA68, after its type name). It moves the
// room-light helper at 0x806244A0 (+0 amount 0..1) toward on / off (the target from the
// "a light-kind 2 lamp is on" byte at r13 - 0x2C90, which d_a_ftrNP sets) and writes
// this + 4 = lerp((0, 0, 0, 255), colour at r13 - 0x56AC, amount): a colour added to the room's
// ambient light, which brightens the whole room evenly when a candle or lamp is switched on. With
// furniture lights the light comes from the lamp itself, so only "Room fill from lamps" of it is
// kept (the RGB scaled; the lerp starts at black, so this is the same as a smaller amount).
// Indoors only; master or furniture lights off: untouched.
HLE(fn_80197E1C) {
    const u32 self = c.r[3];
    callOriginal(c, "fn_80197E1C");
    static const int sLog = [] {
        const char* v = std::getenv("CF_ROOMLIGHT_LOG");
        return v ? std::max(std::atoi(v), 1) : 0;
    }();
    const bool indoor = relBase(107, 1) != 0 && relBase(108, 1) == 0;
    const u32 before = rd32(self + 4);
    // Only while a furniture light of ours lights the room (a lamp the analysis does not light keeps
    // the game's brightening); eased, so the room does not pop when the last light fades out.
    float target = 1.0f;
    if (cffurn::enabled() && indoor && cffurn::litRecently()) {
        target = std::clamp(port::settings().enhancedLighting.furnitureRoomFill, 0.0f, 1.0f);
        // Test aid CF_ROOMLIGHT_FILL=<f>: overrides the setting.
        static const float sFill = std::getenv("CF_ROOMLIGHT_FILL") ? float(std::atof(std::getenv("CF_ROOMLIGHT_FILL"))) : -1.0f;
        if (sFill >= 0.0f) target = std::clamp(sFill, 0.0f, 1.0f);
    }
    static float sK = 1.0f;
    static u32 sKFrame = 0;
    if (copyDispCount() != sKFrame) {
        sK = copyDispCount() - sKFrame > 4 ? target : sK + (target - sK) * 0.1f;
        sKFrame = copyDispCount();
    }
    const float k = cflight::indoorLegacy() ? 1.0f : sK;
    if (k < 0.999f) {
        for (u32 i = 0; i < 3; i++) wr8(self + 4 + i, u8(std::lround(float(rd8(self + 4 + i)) * k)));
    }
    static u32 sLast = ~0u;
    if (sLog && (before != sLast || copyDispCount() % (600 * sLog) == 0)) {
        sLast = before;
        log("[roomlight] f%u obj %08X colour %08X -> %08X (fill %.2f, %s)", copyDispCount(), self,
            before, rd32(self + 4), k, indoor ? "indoors" : "outdoors");
    }
}

}  // namespace recomp
