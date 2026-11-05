// HLE: GX, forwarded to Aurora. Guest arguments are converted to host form
// (big-endian matrices and structs read out of guest memory; guest pointers to
// textures, arrays and display lists translated to host pointers into MEM1).
//
// Raw FIFO writes from game code (vertex data after GXBegin, inlined register
// writes) arrive through the write-gather pipe and are appended to Aurora's
// FIFO unchanged. The console needs no GXEnd; Aurora does, so an open draw is
// closed before the next GX call.
#include "recomp/efb_copy.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/frame.h"
#include "recomp/game.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/widescreen.h"
#include "port/settings.h"

#include <cstring>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <aurora/enhanced.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace recomp {
namespace {

bool sInBegin = false;
u32 sBeginGuest = 0;  // the guest thread whose GXBegin opened the draw
bool sInDisplayList = false;  // GXBeginDisplayList..GXEndDisplayList: everything is recorded

// Fast-forward (skip.cpp): a frame that is not shown sends no draws or copies.
bool dropDraw() { return gx::g_discardDraws && !sInDisplayList; }

GXColor color(u32 p) { return GXColor{u8(rd8(p)), u8(rd8(p + 1)), u8(rd8(p + 2)), u8(rd8(p + 3))}; }
f32 farg(const Ctx& c, int n) { return f32(c.f[n]); }

template <int R, int C>
struct Mtx {
    f32 m[R][C];
    explicit Mtx(u32 p) {
        for (int i = 0; i < R; i++)
            for (int j = 0; j < C; j++) m[i][j] = rdf32(p + 4 * (i * C + j));
    }
};

// Guest GXTexObj/GXTlutObj are smaller than Aurora's, so the host objects live
// in tables and the guest object holds a tag + index (copies of it still work).
constexpr u32 kTexTag = 0x41555458;   // 'AUTX'
constexpr u32 kTlutTag = 0x4155544C;  // 'AUTL'

//
// Objects nothing refers to any more are reused (issue #34): J3D's loadTexNo initializes a
// GXTexObj on the stack for every material texture it loads, the stack is overwritten before the
// next call, and every call took a new entry (~30 per game frame, 64 + 4 bytes each: ~12 MB a
// minute, 1 GB over a long session). When the free list is empty and kCollectEvery objects have
// been initialized since the last collection, guest memory (the pages ever written) is scanned for
// the tag: an entry is live while any guest word pair (tag, index) names it (the object itself or
// a copy anywhere), every other one goes on the free list. The table so stays near
// kCollectEvery + the live objects (a scan of ~25 MB, 2-7 ms, every ~18 s of play). Host code
// never keeps a pointer to an entry past the GX call that looked it up. Test aid
// CF_OBJ_TABLE_LOG=1 logs each collection.
template <typename T>
struct ObjTable {
    static constexpr u32 kCollectEvery = 16384;
    std::deque<T> objs;
    std::deque<u32> owners;
    std::vector<u32> freeList;
    u32 initsSinceCollect = 0, liveAfterCollect = 0, collections = 0;

    T* forInit(u32 guest, u32 tag) {
        const u32 idx = rd32(guest + 4);
        if (rd32(guest) == tag && idx < objs.size() && owners[idx] == guest) return &objs[idx];
        if (freeList.empty() && initsSinceCollect >= kCollectEvery) collect(tag);
        initsSinceCollect++;
        u32 slot;
        if (!freeList.empty()) {
            slot = freeList.back();
            freeList.pop_back();
            objs[slot] = T{};
            owners[slot] = guest;
        } else {
            objs.emplace_back();
            owners.push_back(guest);
            slot = u32(objs.size() - 1);
        }
        wr32(guest, tag);
        wr32(guest + 4, slot);
        statSize.store(u32(objs.size()), std::memory_order_relaxed);
        statFree.store(u32(freeList.size()), std::memory_order_relaxed);
        return &objs[slot];
    }
    std::atomic<u32> statSize{0}, statFree{0};  // for the [mem] report (host thread)
    void collect(u32 tag) {
        const auto t0 = std::chrono::steady_clock::now();
        const u32 n = u32(objs.size());
        std::vector<u8> live(n, 0);
        const u32 want = bswap32(tag);  // as the host reads the big-endian word
        guestWrittenRanges([&](u32 off, u32 len) {
            const u32* w = reinterpret_cast<const u32*>(g_mem + off);
            const u32 words = len / 4;
            const auto check = [&](u32 i) {
                if (w[i] == want) {
                    const u32 idx = bswap32(w[i + 1]);
                    if (idx < n) live[idx] = 1;
                }
            };
            u32 i = 0;
            // Blocks of 64 words without a match are skipped (a vectorizable test).
            for (; i + 64 < words; i += 64) {
                bool any = false;
                for (u32 k = 0; k < 64; k++) any |= w[i + k] == want;
                if (any)
                    for (u32 k = 0; k < 64; k++) check(i + k);
            }
            for (; i + 1 < words; i++) check(i);
        });
        freeList.clear();
        for (u32 i = n; i-- > 0;)
            if (!live[i]) freeList.push_back(i);  // lowest index last: reused first
        liveAfterCollect = n - u32(freeList.size());
        initsSinceCollect = 0;
        collections++;
        static const bool sLog = std::getenv("CF_OBJ_TABLE_LOG") != nullptr;
        if (sLog)
            log("[gx] object table %08X collection %u: %u entries, %u live, %u free (%.2f ms)", tag, collections, n,
                liveAfterCollect, u32(freeList.size()),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    T* get(u32 guest, u32 tag) {
        const u32 idx = rd32(guest + 4);
        if (rd32(guest) != tag || idx >= objs.size()) return nullptr;
        return &objs[idx];
    }
};
ObjTable<GXTexObj> sTex;
ObjTable<GXTlutObj> sTlut;

const void* ptr(u32 a) { return a ? hostp(a) : nullptr; }

// --- widescreen (see widescreen.h) -------------------------------------------------
// The guest's projection, viewport and scissor are tracked here, and the host
// projection/scissor are chosen per draw: unchanged ("plain", i.e. stretched to
// the window), with the projection's x row scaled by ws::factor() ("scaled":
// Hor+ for perspective, centered 4:3 for orthographic), or with the y row scaled
// by 1/factor ("zoom": a 4:3 picture enlarged to fill the width, top and bottom
// cropped).

constexpr f32 kLogicalWidth = 640.0f;

enum class ProjMode : u8 { None, Plain, Scaled, Zoom };

struct {
    f32 m[4][4] = {};
    GXProjectionType type = GX_PERSPECTIVE;
} sProj;
bool sProjSet = false;              // the guest has set a projection
bool sProjDirty = false;            // guest projection not yet sent to Aurora
ProjMode sProjEmitted = ProjMode::None;
int sProjEmittedAnchor = 0;         // HUD anchor the emitted projection was built for
int sProjEmittedShift = 0;          // 2D shift (ws::ShiftScope) the emitted projection was built for

struct {
    f32 left = 0, wd = kLogicalWidth;
} sVp;
// A partial viewport set by a HUD element (ws::AnchorScope: the GBH's live 3D view) is sent
// to Aurora moved and narrowed like the 2D around it, with this anchor; scissors set while
// it is current follow it too.
bool sVpFollow = false;
int sVpAnchor = 0;
int sVpShift = 0;
struct {
    u32 x = 0, y = 0, w = 640, h = 480;
} sScissor;
bool sScissorPillar = false;  // Aurora currently has the centered-4:3 version of the guest scissor
int sScissorAnchor = 0;
int sScissorShift = 0;
// Draws written straight into the FIFO by game code (J3D inlines its display-list
// calls and matrix loads) bypass the HLE draw entry points, so the projection for
// them is brought up to date before the first raw FIFO write after any change.
bool sRawCheck = true;

// HUD anchor for the draw being prepared: a held draw keeps the one from its GXBegin (it
// is only sent at the next GX call, which may come after the HUD element's scope ended).
int sHeldAnchor = 0;
int sHeldShift = 0;
bool sHeldWipe = false;  // GXBegin came inside a ws::WipeScope (the draw ends after the scope)
bool sInHeldPrepare = false;
// A by-side scope is resolved per held draw (classifyHeld); anything else in it stays centered.
int scopeAnchor() {
    const int a = ws::hudAnchor();
    return a == ws::kAnchorBySide ? 0 : a;
}
int hudAnchor() { return sInHeldPrepare ? sHeldAnchor : scopeAnchor(); }
// 2D shift of the draw being prepared (ws::ShiftScope: the 2-player split screen's HUD halves).
int hudShift() { return sInHeldPrepare ? sHeldShift : ws::hudShift(); }

// x of the centered (and anchored) 4:3 frame for logical x: what the scaled orthographic
// projection does to 2D, in pixels. `shift` moves the logical x first (ws::ShiftScope).
f32 pillarX(f32 x, int anchor, int shift = 0) {
    const f32 s = ws::factor(), mid = kLogicalWidth * 0.5f;
    return mid + (x + f32(shift) - mid) * s + f32(anchor) * (1.0f - s) * mid;
}

bool viewportFull() { return sVp.left <= 1.0f && sVp.left + sVp.wd >= kLogicalWidth - 1.0f; }

// Frame interpolation: 2D (under an orthographic projection) is blended too (HUD panes,
// dialogue boxes, screen transitions), paired by draw like 3D but not camera-factored.
u32 mark3D() {
    interp::setOrthographic(sProj.type != GX_PERSPECTIVE);
    return interp::mark();
}

void emitProjection(ProjMode mode) {
    f32 m[4][4];
    std::memcpy(m, sProj.m, sizeof(m));
    int anchor = 0;
    // 2D moved by whole logical pixels (2-player split screen HUD halves): x' = x + shift.
    const int shift = sProj.type == GX_ORTHOGRAPHIC ? hudShift() : 0;
    if (shift) m[0][3] += m[0][0] * f32(shift);
    if (mode == ProjMode::Scaled && sProj.type == GX_PERSPECTIVE) {
        // Field of view: the vertical FOV times fovScale, then Hor+ for the width.
        const f32 r = ws::fovRatio(m[1][1]);
        const f32 s = ws::factor();
        for (int j = 0; j < 4; j++) {
            m[0][j] *= s * r;
            m[1][j] *= r;
        }
    } else if (mode == ProjMode::Scaled) {
        // 2D at 4:3, centered; HUD elements anchored to a side move to that edge.
        const f32 s = ws::factor();
        anchor = hudAnchor();
        for (int j = 0; j < 4; j++) m[0][j] *= s;
        m[0][3] += f32(anchor) * (1.0f - s);
    } else if (mode == ProjMode::Zoom) {
        const f32 s = ws::factor();
        for (int j = 0; j < 4; j++) m[1][j] /= s;
    }
    const u32 at = mark3D();
    GXSetProjection(m, sProj.type);
    interp::note(at, interp::Kind::Projection, 0, 0, 6);
    sProjEmitted = mode;
    sProjEmittedAnchor = anchor;
    sProjEmittedShift = shift;
    sProjDirty = false;
}

void emitScissor(bool pillar) {
    sScissorAnchor = 0;
    const u32 at = interp::mark();
    if (pillar) {
        sScissorAnchor = sVpFollow ? sVpAnchor : hudAnchor();
        const int shift = sVpFollow ? sVpShift : hudShift();
        sScissorShift = shift;
        const f32 l = std::max(0.0f, pillarX(f32(sScissor.x), sScissorAnchor, shift));
        const f32 r = std::min(kLogicalWidth, pillarX(f32(sScissor.x + sScissor.w), sScissorAnchor, shift));
        const u32 li = u32(std::max(0.0f, std::floor(l)));
        GXSetScissor(li, sScissor.y, u32(std::ceil(r)) - li, sScissor.h);
        interp::noteTextScissor(at, ws::factor(), pillarX(0.0f, sScissorAnchor));
    } else {
        GXSetScissor(sScissor.x, sScissor.y, sScissor.w, sScissor.h);
        interp::noteTextScissor(at, 1.0f, 0.0f);
    }
    sScissorPillar = pillar;
}

// Projection mode for a draw. `ortho2D` is what an orthographic draw into the
// full-screen viewport gets: Scaled (centered 4:3) or Plain (stretched).
ProjMode drawMode(ProjMode ortho2D) {
    // A half of the 2-player split screen (coop_fixes.cpp, side by side) is drawn Hor+ like a
    // full-screen view: it is stretched across its half of the window by the same factor.
    if (!viewportFull() && !(sProj.type == GX_PERSPECTIVE && ws::splitHalf(sVp.left, sVp.wd))) return ProjMode::Plain;
    // A pass the game renders for its own projection (LM: the light's-view shadow map, copied as a
    // texture the receivers project with the unmodified light matrices) must not get the Hor+/FOV
    // or centered-4:3 adjustments (game.h unadjustedPass).
    if (game::unadjustedPass()) return ProjMode::Plain;
    if (sProj.type == GX_PERSPECTIVE) return ws::adjusting() ? ProjMode::Scaled : ProjMode::Plain;
    return ws::active() ? ortho2D : ProjMode::Plain;
}

// CF_WS_TRACE=1: per frame, a summary of the widescreen decisions (draw path,
// projection type, viewport, result; consecutive repeats counted), logged
// whenever it differs from the previous frame's.
const bool sTrace = std::getenv("CF_WS_TRACE") != nullptr;
using TraceFrame = std::vector<std::pair<std::string, u32>>;
TraceFrame sTraceFrame, sTraceLast;
void traceNote(const char* fmt, ...) {
    if (!sTrace) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!sTraceFrame.empty() && sTraceFrame.back().first == buf) sTraceFrame.back().second++;
    else sTraceFrame.emplace_back(buf, 1);
}

void prepareDraw(ProjMode mode, const char* what = "draw") {
    traceNote("%s %s vp(%.0f,+%.0f) -> %s", what, sProj.type == GX_PERSPECTIVE ? "persp" : "ortho", sVp.left,
              sVp.wd, mode == ProjMode::Scaled ? "scaled" : mode == ProjMode::Zoom ? "zoom" : "plain");
    const bool hud = mode == ProjMode::Scaled && sProj.type == GX_ORTHOGRAPHIC;
    if (sProjSet && (sProjDirty || mode != sProjEmitted || (hud && hudAnchor() != sProjEmittedAnchor) ||
                     (sProj.type == GX_ORTHOGRAPHIC && hudShift() != sProjEmittedShift)))
        emitProjection(mode);
    const bool shifted = sProj.type == GX_ORTHOGRAPHIC && hudShift() != 0;
    const bool pillar = sVpFollow || ((hud || shifted) && (sScissor.x > 0 || sScissor.x + sScissor.w < 640));
    const int anchor = sVpFollow ? sVpAnchor : hudAnchor();
    const int shift = sVpFollow ? sVpShift : hudShift();
    if (pillar != sScissorPillar || (pillar && (anchor != sScissorAnchor || shift != sScissorShift))) emitScissor(pillar);
}

// Mirror of the vertex layout state that immediate-mode 2D draws use, so a
// buffered draw's positions can be decoded (see coversScreen).
struct VatEntry {
    u8 cnt = 0, type = 0, frac = 0;
};
u8 sDesc[GX_VA_MAX_ATTR] = {};           // GXAttrType per attribute
VatEntry sVat[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
u32 sPosArray = 0, sPosStride = 0;
f32 sPosMtx[10][3][4];
bool sPosMtxValid[10] = {};
u32 sCurPosMtx = 0;

int attrSlot(u32 attr) { return attr == GX_VA_NBT ? GX_VA_NRM : attr < GX_VA_MAX_ATTR ? int(attr) : -1; }

int compSize(u8 type) { return type <= GX_S8 ? 1 : type <= GX_S16 ? 2 : 4; }

// Bytes one vertex of attribute `a` takes in the stream, or -1 if unknown.
int attrSize(int fmt, int a) {
    const u8 d = sDesc[a];
    if (d == GX_NONE) return 0;
    if (d == GX_INDEX8 || d == GX_INDEX16) {
        const int idx = d == GX_INDEX8 ? 1 : 2;
        return a == GX_VA_NRM && sVat[fmt][a].cnt == GX_NRM_NBT3 ? idx * 3 : idx;
    }
    if (a <= GX_VA_TEX7MTXIDX) return 1;
    const VatEntry& v = sVat[fmt][a];
    switch (a) {
    case GX_VA_POS: return (v.cnt == GX_POS_XYZ ? 3 : 2) * compSize(v.type);
    case GX_VA_NRM: return (v.cnt == GX_NRM_XYZ ? 3 : 9) * compSize(v.type);
    case GX_VA_CLR0:
    case GX_VA_CLR1: {
        static constexpr int kClr[] = {2, 3, 4, 2, 3, 4};
        return v.type < 6 ? kClr[v.type] : -1;
    }
    default:
        if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7) return (v.cnt == GX_TEX_ST ? 2 : 1) * compSize(v.type);
        return -1;
    }
}

f32 readComp(const u8* p, u8 type, u8 frac) {
    const f32 scale = 1.0f / f32(1u << frac);
    switch (type) {
    case GX_U8: return f32(p[0]) * scale;
    case GX_S8: return f32(s8(p[0])) * scale;
    case GX_U16: return f32(u16(p[0] << 8 | p[1])) * scale;
    case GX_S16: return f32(s16(p[0] << 8 | p[1])) * scale;
    default: return std::bit_cast<f32>(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]);
    }
}

// Immediate-mode draws under an orthographic projection are held back until the
// draw ends, so the widescreen mode can look at where the vertices land before
// choosing the projection (see classifyHeld).
struct {
    bool active = false;
    GXPrimitive prim;
    GXVtxFmt fmt;
    u16 verts;
    std::vector<u8> bytes;    // the stream as the console's FIFO would see it (big-endian)
    std::vector<u32> writes;  // per FIFO write: byte offset | (size - 1) << 30, for exact replay
} sHeld;

// What the TEV samples, to tell a full-screen picture (a 4:3 background image)
// from a full-screen fill or a blit of an EFB copy.
std::vector<u32> sTexImage;      // guest image address (physical) per host texture object
u32 sMapImage[8] = {};           // image bound to each GXTexMapID
u8 sTevMap[16] = {};             // GXSetTevOrder map per stage (0xFF: none)
u32 sNumTevStages = 1;
std::vector<u32> sCopyDests;     // EFB copy destinations (physical), most recent last

u32 phys(u32 a) { return hostOffset(a); }
bool isCopyDest(u32 image) { return std::find(sCopyDests.begin(), sCopyDests.end(), image) != sCopyDests.end(); }

// True if any active TEV stage samples a texture that is not an EFB copy.
bool samplesPicture() {
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) {
        const u32 map = sTevMap[s];
        if (map >= 8) continue;  // GX_TEXMAP_NULL, or GX_TEX_DISABLE set
        if (sMapImage[map] && !isCopyDest(sMapImage[map])) return true;
    }
    return false;
}

f32 sHeldLo = 0, sHeldHi = 0;  // NDC x extent of the last decoded held draw
bool sHeldDecoded = false;
std::string sHeldChain;  // CF_HUD_TRACE
bool coversScreen() {
    const int fmt = sHeld.fmt;
    const size_t n = sHeld.verts;
    if (n == 0 || fmt >= GX_MAX_VTXFMT || sHeld.bytes.size() % n) return false;
    const size_t stride = sHeld.bytes.size() / n;
    int expected = 0, posOff = -1;
    for (int a = 0; a <= GX_VA_TEX7; a++) {
        const int sz = attrSize(fmt, a);
        if (sz < 0) return false;
        if (a == GX_VA_POS) posOff = expected;
        expected += sz;
    }
    if (size_t(expected) != stride || sDesc[GX_VA_POS] == GX_NONE) return false;
    const VatEntry pv = sVat[fmt][GX_VA_POS];
    const int comps = pv.cnt == GX_POS_XYZ ? 3 : 2;
    const int cs = compSize(pv.type);
    f32 lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < n; i++) {
        const u8* v = sHeld.bytes.data() + i * stride;
        const u32 mtxId = sDesc[GX_VA_PNMTXIDX] == GX_DIRECT ? v[0] : sCurPosMtx;
        if (mtxId % 3 || mtxId / 3 >= 10 || !sPosMtxValid[mtxId / 3]) return false;
        u8 tmp[12];
        const u8* p = v + posOff;
        if (sDesc[GX_VA_POS] != GX_DIRECT) {
            const u32 idx = sDesc[GX_VA_POS] == GX_INDEX8 ? p[0] : u32(p[0] << 8 | p[1]);
            if (!sPosArray) return false;
            for (int b = 0; b < comps * cs; b++) tmp[b] = u8(rd8(sPosArray + idx * sPosStride + b));
            p = tmp;
        }
        const f32 pos[4] = {readComp(p, pv.type, pv.frac), readComp(p + cs, pv.type, pv.frac),
                            comps == 3 ? readComp(p + 2 * cs, pv.type, pv.frac) : 0.0f, 1.0f};
        const auto& m = sPosMtx[mtxId / 3];
        f32 eye[4] = {0, 0, 0, 1};
        for (int r = 0; r < 3; r++) eye[r] = m[r][0] * pos[0] + m[r][1] * pos[1] + m[r][2] * pos[2] + m[r][3];
        const f32 ndc = sProj.m[0][0] * eye[0] + sProj.m[0][1] * eye[1] + sProj.m[0][2] * eye[2] + sProj.m[0][3];
        lo = std::min(lo, ndc);
        hi = std::max(hi, ndc);
    }
    sHeldLo = lo;
    sHeldHi = hi;
    sHeldDecoded = true;
    return lo <= -0.98f && hi >= 0.98f;
}

// Inside a ws::WipeScope (the circle wipe, mWipeFader_c::draw fn_802B6B48, hooked in cf_hud.cpp):
// the wipe is one quad over the game's whole 2D frame whose texture (the circle hole, clamped, so
// everything outside the texture is its opaque black edge) is placed by a texture matrix. The quad
// is drawn centered in the game's frame like other 2D (classifyHeld), which keeps the circle round,
// and is widened here about the frame's centre by 1 / factor() so it reaches the display's left
// and right edges; every other direct F32 attribute (the texture coordinates) is extrapolated
// along the quad's x in the same way, so the circle keeps its place and size and the added strips
// show what the texture shows beyond its edge (black). Direct F32 positions with no matrix
// rotation only; anything else is left as it is.
void widenWipeHeld() {
    if (!sHeldWipe || !ws::active()) return;
    const int fmt = sHeld.fmt;
    const size_t n = sHeld.verts;
    if (n < 3 || n > 64 || fmt >= GX_MAX_VTXFMT || sHeld.bytes.size() % n) return;
    const size_t stride = sHeld.bytes.size() / n;
    if (sDesc[GX_VA_POS] != GX_DIRECT || sVat[fmt][GX_VA_POS].type != GX_F32) return;
    // Offsets of the direct F32 components (position x first) and of the matrix index.
    int off = 0, posOff = -1, mtxOff = -1;
    std::vector<int> comps;  // byte offsets of the F32 components to extrapolate (not position)
    for (int a = 0; a <= GX_VA_TEX7; a++) {
        const int sz = attrSize(fmt, a);
        if (sz < 0) return;
        if (a == GX_VA_PNMTXIDX && sz) mtxOff = off;
        if (a == GX_VA_POS) posOff = off;
        else if (sz && a >= GX_VA_TEX0 && sDesc[a] == GX_DIRECT && sVat[fmt][a].type == GX_F32)
            for (int k = 0; k < sz / 4; k++) comps.push_back(off + k * 4);
        off += sz;
    }
    if (size_t(off) != stride || posOff < 0) return;
    auto rf = [&](size_t v, int o) { return readComp(sHeld.bytes.data() + v * stride + o, GX_F32, 0); };
    auto wf = [&](size_t v, int o, f32 x) {
        const u32 b = std::bit_cast<u32>(x);
        u8* p = sHeld.bytes.data() + v * stride + o;
        p[0] = u8(b >> 24), p[1] = u8(b >> 16), p[2] = u8(b >> 8), p[3] = u8(b);
    };
    const u32 mtxId = mtxOff >= 0 ? sHeld.bytes[mtxOff] : sCurPosMtx;
    if (mtxId % 3 || mtxId / 3 >= 10 || !sPosMtxValid[mtxId / 3]) return;
    const auto& m = sPosMtx[mtxId / 3];
    if (std::fabs(m[0][1]) > 1e-4f || std::fabs(m[0][2]) > 1e-4f || std::fabs(m[0][0]) < 1e-6f) return;
    if (std::fabs(sProj.m[0][0]) < 1e-9f) return;
    // Object-space x at the frame's centre (NDC x = 0).
    const f32 xc = ((-sProj.m[0][3] / sProj.m[0][0]) - m[0][3]) / m[0][0];
    // Each extrapolated component as an affine function of (x, y): from three vertices that span
    // the quad (the first one and the two farthest from it).
    const size_t i0 = 0;
    size_t i1 = 0, i2 = 0;
    f32 best = 0;
    for (size_t i = 1; i < n; i++) {
        const f32 d = std::fabs(rf(i, posOff) - rf(0, posOff)) + std::fabs(rf(i, posOff + 4) - rf(0, posOff + 4));
        if (d > best) best = d, i1 = i;
    }
    best = 0;
    const f32 ax = rf(i1, posOff) - rf(i0, posOff), ay = rf(i1, posOff + 4) - rf(i0, posOff + 4);
    for (size_t i = 1; i < n; i++) {
        const f32 bx = rf(i, posOff) - rf(i0, posOff), by = rf(i, posOff + 4) - rf(i0, posOff + 4);
        const f32 cr = std::fabs(ax * by - ay * bx);
        if (cr > best) best = cr, i2 = i;
    }
    const f32 bx = rf(i2, posOff) - rf(i0, posOff), by = rf(i2, posOff + 4) - rf(i0, posOff + 4);
    const f32 det = ax * by - ay * bx;
    if (std::fabs(det) < 1e-6f) return;
    std::vector<f32> dfdx(comps.size());
    for (size_t k = 0; k < comps.size(); k++) {
        const f32 fa = rf(i1, comps[k]) - rf(i0, comps[k]), fb = rf(i2, comps[k]) - rf(i0, comps[k]);
        dfdx[k] = (fa * by - fb * ay) / det;  // solve fa = gx*ax + gy*ay, fb = gx*bx + gy*by for gx
    }
    const f32 k = 1.0f / ws::factor() - 1.0f;  // extra share of the distance from the centre
    for (size_t v = 0; v < n; v++) {
        const f32 x = rf(v, posOff), dx = (x - xc) * k;
        wf(v, posOff, x + dx);
        for (size_t c = 0; c < comps.size(); c++) wf(v, comps[c], rf(v, comps[c]) + dfdx[c] * dx);
    }
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        log("[ws] circle wipe widened to the display (factor %.3f, %zu vertices, %zu coordinates)", ws::factor(), n,
            comps.size());
    }
}

// A held 2D draw into the full-screen viewport:
//  - covers the logical width and is a fill or samples an EFB copy (fades,
//    screen-space effects that copy the frame and draw it back): stretched,
//    so it lines up with the stretched EFB;
//  - covers the width and samples an ordinary texture (a 4:3 background
//    picture): per --ws-backgrounds, centered by default;
//  - anything else is HUD: centered 4:3 (moved to an edge with its HUD element's anchor).
ProjMode classifyHeld() {
    sHeldDecoded = false;
    // The circle wipe stays centered in the game's frame (round circle); widenWipeHeld then
    // stretches its quad to the display's edges.
    if (sHeldWipe) return ProjMode::Scaled;
    if (!coversScreen()) {
        if (sHeldAnchor == ws::kAnchorBySide) sHeldAnchor = !sHeldDecoded ? 0 : sHeldLo + sHeldHi < 0.0f ? -1 : 1;
        return ProjMode::Scaled;
    }
    if (sHeldAnchor == ws::kAnchorBySide) sHeldAnchor = 0;
    traceNote("full-screen 2D, %s", samplesPicture() ? "picture" : "fill/copy");
    if (!samplesPicture()) return ProjMode::Plain;
    switch (ws::backgroundMode()) {
    case ws::Background::Stretch: return ProjMode::Plain;
    case ws::Background::Zoom: return ProjMode::Zoom;
    default: return ProjMode::Scaled;
    }
}

}  // namespace

// Vertex layout for interp's vertex-array tracking (interp_vtx.cpp).
int gx::vtxAttrSize(int fmt, int attr) { return fmt < GX_MAX_VTXFMT ? attrSize(fmt, attr) : -1; }
// Whether an active TEV stage samples an EFB copy (frame interpolation: such draws don't fade).
bool gx::samplesCopy() {
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) {
        const u32 map = sTevMap[s];
        if (map < 8 && sMapImage[map] && isCopyDest(sMapImage[map])) return true;
    }
    return false;
}
u8 gx::vtxDesc(int attr) { return attr < GX_VA_MAX_ATTR ? sDesc[attr] : 0; }
void gx::cpRegWrite(u8 reg, u32 v) {
    const auto bits = [&](int lo, int n) { return u8((v >> lo) & ((1u << n) - 1)); };
    const u32 fmt = reg & 7;
    switch (reg & 0xF0) {
    case 0x50:  // VCD low: matrix indices (1 bit: direct or none), position, normal, colours
        sDesc[GX_VA_PNMTXIDX] = bits(0, 1);
        for (int t = 0; t < 8; t++) sDesc[GX_VA_TEX0MTXIDX + t] = bits(1 + t, 1);
        sDesc[GX_VA_POS] = bits(9, 2);
        sDesc[GX_VA_NRM] = bits(11, 2);
        sDesc[GX_VA_CLR0] = bits(13, 2);
        sDesc[GX_VA_CLR1] = bits(15, 2);
        break;
    case 0x60:  // VCD high: texture coordinates
        for (int t = 0; t < 8; t++) sDesc[GX_VA_TEX0 + t] = bits(2 * t, 2);
        break;
    case 0x70:  // VAT group 0
        if (reg & 8) break;
        sVat[fmt][GX_VA_POS] = {bits(0, 1), bits(1, 3), bits(4, 5)};
        sVat[fmt][GX_VA_NRM] = {u8(bits(9, 1) ? (bits(31, 1) ? GX_NRM_NBT3 : GX_NRM_NBT) : GX_NRM_XYZ), bits(10, 3), 0};
        sVat[fmt][GX_VA_CLR0] = {bits(13, 1), bits(14, 3), 0};
        sVat[fmt][GX_VA_CLR1] = {bits(17, 1), bits(18, 3), 0};
        sVat[fmt][GX_VA_TEX0] = {bits(21, 1), bits(22, 3), bits(25, 5)};
        break;
    case 0x80:  // VAT group 1
        if (reg & 8) break;
        sVat[fmt][GX_VA_TEX1] = {bits(0, 1), bits(1, 3), bits(4, 5)};
        sVat[fmt][GX_VA_TEX2] = {bits(9, 1), bits(10, 3), bits(13, 5)};
        sVat[fmt][GX_VA_TEX3] = {bits(18, 1), bits(19, 3), bits(22, 5)};
        sVat[fmt][GX_VA_TEX4].cnt = bits(27, 1), sVat[fmt][GX_VA_TEX4].type = bits(28, 3);
        break;
    case 0x90:  // VAT group 2
        if (reg & 8) break;
        sVat[fmt][GX_VA_TEX4].frac = bits(0, 5);
        sVat[fmt][GX_VA_TEX5] = {bits(5, 1), bits(6, 3), bits(9, 5)};
        sVat[fmt][GX_VA_TEX6] = {bits(14, 1), bits(15, 3), bits(18, 5)};
        sVat[fmt][GX_VA_TEX7] = {bits(23, 1), bits(24, 3), bits(27, 5)};
        break;
    default: break;
    }
}
bool gx::vtxAttrFormat(int fmt, int attr, u8& cnt, u8& type, u8& frac) {
    if (fmt >= GX_MAX_VTXFMT || attr >= GX_VA_MAX_ATTR) return false;
    cnt = sVat[fmt][attr].cnt, type = sVat[fmt][attr].type, frac = sVat[fmt][attr].frac;
    return true;
}

namespace {

void rawFifoWrite(u32 v, int size) {
    interp::snapBytes(v, size);
    switch (size) {
    case 1: GXCmd1u8(u8(v)); break;
    case 2: GXCmd1u16(u16(v)); break;
    default: GXCmd1u32(v); break;
    }
}

gx::DisplayCopy sDisplayCopy;

}  // namespace

const gx::DisplayCopy& gx::displayCopy() { return sDisplayCopy; }

void gx::recheckDrawState() { sRawCheck = true; }

// Enhanced lighting used to force the smoothing on; it now turns the 6-bit framebuffer off
// altogether (enhancedOverridesEfbDepth), so nothing forces the smoothing any more.
bool gx::smoothBandingForced() { return false; }

int gx::smoothBandingLevel() {
    static const int sForce = [] {
        const char* e = std::getenv("CF_SMOOTH_BANDING");
        return e ? std::clamp(std::atoi(e), 0, 2) : -1;
    }();
    if (sForce >= 0) return sForce;
    return port::settings().rendering.smoothBanding || smoothBandingForced() ? 1 : 0;
}

// F1 > Visuals > Rendering accuracy options -> Aurora. Test overrides: CF_ACCURACY=<mask>
// (bit 0 integer TEV, bit 1 EFB color depth, bit 2 texture copy filter, bit 3 6-bit blending, bit 4 early-Z alpha
// depth) and CF_ACCURACY_AB=<seconds>
// (alternate between the configured options and all off every <seconds>, for A/B screenshots of one run).
bool gx::enhancedOverridesEfbDepth() { return enh::configuredOn(); }

void gx::applyAccuracySettings() {
    const auto& r = port::settings().rendering;
    u32 flags = (r.integerTev ? GX_AURORA_ACCURACY_INT_TEV : 0) | (r.efbColorDepth ? GX_AURORA_ACCURACY_EFB_DEPTH : 0) |
                (r.efbColorDepth && r.efbBlend6 ? GX_AURORA_ACCURACY_EFB_BLEND : 0) |
                (r.earlyZAlphaDepth ? GX_AURORA_ACCURACY_EARLY_Z : 0) |
                // Interpolated presentation replays every frame on the host after the fact: the
                // per-draw early-Z split cost ~20 ms per replay in the foyer (presentation fell to
                // ~35 fps); merged split pairs keep it at ~4 ms. Original 30 keeps the exact split.
                (r.earlyZAlphaDepth && (port::settings().video.matchDisplay || port::settings().video.unlimitedFps ||
                                        frame::targetFps() > std::max(port::kMinFps, game::nativeFps()))
                     ? GX_AURORA_ACCURACY_EARLY_Z_MERGE
                     : 0) |
                (r.textureCopyFilter ? GX_AURORA_ACCURACY_COPY_FILTER : 0);
    // Enhanced lighting turns the 6-bit EFB (and with it 6-bit blending, the dither and the
    // smoothing of 0190) off: its smooth per-pixel and colored-light gradients went through the
    // 6-bit framebuffer and the game's RGB565 scene copy, which banded them into visible steps
    // (docs/RENDERING.md "Enhanced lighting fixes"; the F1 toggles are greyed out while it is on).
    const bool efbDepth = r.efbColorDepth && !enhancedOverridesEfbDepth();
    if (!efbDepth) flags &= ~(GX_AURORA_ACCURACY_EFB_DEPTH | GX_AURORA_ACCURACY_EFB_BLEND);
    // Smooth color banding (Aurora 0190): an enhancement on top of the EFB color depth
    // (docs/RENDERING.md "Smooth color banding").
    const int smooth = gx::smoothBandingLevel();
    if (efbDepth && smooth > 0)
        flags |= GX_AURORA_ACCURACY_EFB_SMOOTH | (smooth == 1 ? GX_AURORA_ACCURACY_EFB_SMOOTH_NOISE : 0);
    static const char* sMask = std::getenv("CF_ACCURACY");
    if (sMask) flags = u32(std::strtoul(sMask, nullptr, 0));
    static const double sAb = [] {
        const char* e = std::getenv("CF_ACCURACY_AB");
        return e ? std::atof(e) : 0.0;
    }();
    if (sAb > 0.0) {
        static const auto sStart = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - sStart).count();
        const long long phase = (long long)(t / sAb);
        const bool off = phase % 2 == 1;
        // Frame rate per phase (presented frames; use an uncapped frame rate to measure cost).
        static long long sPhase = 0;
        static int sFrames = 0;
        if (phase != sPhase) {
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            log("[accuracy] phase %lld (%s): %.1f fps, ended at %.1f", sPhase, sPhase % 2 ? "off" : "on", sFrames / sAb, now);
            sPhase = phase;
            sFrames = 0;
        }
        sFrames++;
        static const bool sFilter = port::settings().rendering.displayCopyFilter;
        if (off) flags = 0;
        port::settings().rendering.displayCopyFilter = off ? false : sFilter;
    }
    if (flags != GXGetAuroraAccuracy()) GXSetAuroraAccuracy(flags);
    // CF_TEXGEN_BEYOND=0|1|2 (test aid): what a stage reads from a texcoord beyond GXSetNumTexGens
    // ((0,0) / generated from its texgen (default, the console's behaviour) / texcoord 0).
    static const int sBeyond = [] {
        const char* e = std::getenv("CF_TEXGEN_BEYOND");
        if (e) GXSetAuroraTexGenBeyondCount(u32(std::atoi(e)));
        return 0;
    }();
    (void)sBeyond;
}

// CF_HUD_TRACE=<first>,<last>[,<step>]: for those frames (GXCopyDisp count), every 2D draw, viewport,
// scissor and texture copy with its guest call chain, to map HUD pieces to game functions.
static u32 sDiagFrame = 0;
static bool hudTraceOn() {
    static const auto range = [] {
        u32 a = ~0u, b = 0, step = 1;
        if (const char* v = std::getenv("CF_HUD_TRACE")) std::sscanf(v, "%u,%u,%u", &a, &b, &step);
        return std::tuple<u32, u32, u32>{a, b, step ? step : 1};
    }();
    const auto [first, last, step] = range;
    return sDiagFrame >= first && sDiagFrame <= last && (sDiagFrame - first) % step == 0;
}
static std::string guestChain(const Ctx& c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", c.lr);
    std::string s = buf;
    u32 sp = c.r[1];
    for (int i = 0; i < 12; i++) {
        sp = rd32(sp);
        if (sp < 0x80000000u || sp >= 0x81800000u) break;
        std::snprintf(buf, sizeof(buf), "<%08X", rd32(sp + 4));
        s += buf;
    }
    return s;
}
#define HUD_TRACE(fmt, ...) \
    do { if (hudTraceOn()) log("[hud] f%u " fmt, sDiagFrame, __VA_ARGS__); } while (0)

// CF_IND_LOG=1: every draw made while indirect texturing is active (GXSetNumIndStages > 0 or an
// active TEV stage with an indirect setup), grouped per frame by guest call chain + indirect
// state, printed whenever a frame's set differs from the previous frame's. Mirrors the state
// from the HLE'd GX calls (all of LM's indirect setup goes through them: fn_8005FDD4 and J3D);
// AURORA_GX_AUDIT=1 cross-checks state that would arrive as raw BP writes in display lists.
namespace {
struct IndShadow {
    u32 numInd = 0;
    u32 tev[16] = {};        // GXSetTevIndirect word: st | fmt<<2 | bias<<4 | alpha<<7 | mtx<<9 | ws<<13 | wt<<16 | lod<<19 | prev<<20
    u8 order[4][2] = {};     // texcoord, texmap
    u8 scale[4][2] = {};
    f32 mtx[3][6] = {};
    s8 exp[3] = {};
    u8 texGenMtx[8] = {};
    u8 numTexGens = 0;      // GXSetTexCoordGen2 matrix per texcoord
    f32 texMtx[10][8] = {};    // GX_TEXMTX0..9 (2x4 rows), as loaded
} sInd;
std::unordered_map<u32, u32> sImageDesc;  // image -> w<<20 | h<<8 | fmt (CF_IND_LOG only)
std::map<std::string, u32> sIndFrame, sIndLast;
u32 sIndLastFrame = 0;
// CF_IND_OFF=1 (A/B test aid): indirect stages are never enabled, i.e. the warp is drawn unwarped.
bool indOff() {
    static const bool on = [] { const char* e = std::getenv("CF_IND_OFF"); return e && e[0] == '1'; }();
    return on;
}
bool indLogOn() {
    static const bool on = [] { const char* e = std::getenv("CF_IND_LOG"); return e && e[0] == '1'; }();
    return on;
}
u32 indWord(u32 st, u32 fmt, u32 bias, u32 mtx, u32 ws, u32 wt, u32 prev, u32 lod, u32 alpha) {
    return st | fmt << 2 | bias << 4 | alpha << 7 | mtx << 9 | ws << 13 | wt << 16 | lod << 19 | prev << 20;
}
std::string imageDesc(u32 image) {
    char b[64];
    const auto it = sImageDesc.find(image);
    if (it == sImageDesc.end()) std::snprintf(b, sizeof(b), "%08X", image);
    else std::snprintf(b, sizeof(b), "%08X(%ux%u f%u)", image, it->second >> 20, (it->second >> 8) & 0xFFF, it->second & 0xFF);
    return b;
}
}  // namespace

// CF_TEXGEN_LOG=1: every distinct draw (guest call chain) whose TEV or indirect stages read a
// texture coordinate at or beyond GXSetNumTexGens, logged once with the stage setup.
u8 sTevCoord[16] = {};
static void texgenNoteDraw(const Ctx& c, const char* kind) {
    static const bool on = [] { const char* e = std::getenv("CF_TEXGEN_LOG"); return e && e[0] == '1'; }();
    if (!on) return;
    std::string what;
    char b[96];
    for (u32 s = 0; s < sNumTevStages && s < 16; s++)
        if (sTevMap[s] < 8 && sTevCoord[s] < 8 && sTevCoord[s] >= sInd.numTexGens)
            std::snprintf(b, sizeof(b), " tev%u{tc%u map%u}", s, sTevCoord[s], sTevMap[s]), what += b;
    for (u32 i = 0; i < sInd.numInd && i < 4; i++)
        if (sInd.order[i][0] >= sInd.numTexGens)
            std::snprintf(b, sizeof(b), " ind%u{tc%u map%u}", i, sInd.order[i][0], sInd.order[i][1]), what += b;
    if (what.empty()) return;
    static std::map<std::string, u32> sSeen;
    std::snprintf(b, sizeof(b), " | nTexGen=%u nTev=%u nInd=%u", sInd.numTexGens, sNumTevStages, sInd.numInd);
    const std::string k = std::string(kind) + " " + guestChain(c) + b + what;
    if (sSeen[k]++ == 0) log("[texgen] f%u %s", sDiagFrame, k.c_str());
}
static void indNoteDraw(const Ctx& c, const char* kind) {
    texgenNoteDraw(c, kind);
    if (!indLogOn()) return;
    bool any = sInd.numInd > 0;
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) any |= sInd.tev[s] != 0;
    if (!any) return;
    std::string k = std::string(kind) + " " + guestChain(c);
    char b[160];
    std::snprintf(b, sizeof(b), " | nInd=%u nTev=%u nTexGen=%u", sInd.numInd, sNumTevStages, sInd.numTexGens);
    k += b;
    for (u32 i = 0; i < sInd.numInd && i < 4; i++) {
        std::snprintf(b, sizeof(b), " ind%u{tc%u map%u %s scale%u,%u}", i, sInd.order[i][0], sInd.order[i][1],
                      sInd.order[i][1] < 8 ? imageDesc(sMapImage[sInd.order[i][1]]).c_str() : "-", sInd.scale[i][0],
                      sInd.scale[i][1]);
        k += b;
        if (const u32 img = sInd.order[i][1] < 8 ? sMapImage[sInd.order[i][1]] : 0) {
            const u8* p = hostp(0x80000000u | img);
            std::snprintf(b, sizeof(b), " bytes[%02X %02X %02X %02X %02X %02X %02X %02X]", p[0], p[1], p[2], p[3], p[4],
                          p[5], p[6], p[7]);
            k += b;
        }
        const u32 tm = sInd.texGenMtx[sInd.order[i][0] & 7];
        if (tm >= GX_TEXMTX0 && tm <= GX_TEXMTX9) {
            const f32* x = sInd.texMtx[(tm - GX_TEXMTX0) / 3];
            std::snprintf(b, sizeof(b), " texmtx%u[%.4f %.4f %.4f %.4f; %.4f %.4f %.4f %.4f]", (tm - GX_TEXMTX0) / 3,
                          x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
            k += b;
        }
    }
    u32 mtxUsed = 0;
    for (u32 s = 0; s < sNumTevStages && s < 16; s++) {
        const u32 w = sInd.tev[s];
        if (!w) continue;
        std::snprintf(b, sizeof(b), " tev%u{ind%u fmt%u bias%u alpha%u mtx%u wrap%u,%u lod%u prev%u map%u %s}", s,
                      w & 3, (w >> 2) & 3, (w >> 4) & 7, (w >> 7) & 3, (w >> 9) & 15, (w >> 13) & 7, (w >> 16) & 7,
                      (w >> 19) & 1, (w >> 20) & 1, sTevMap[s],
                      sTevMap[s] < 8 ? imageDesc(sMapImage[sTevMap[s]]).c_str() : "-");
        k += b;
        // RGB5A3 image (e.g. an EFB copy, read back with CF_EFB_READBACK_ALL=1): opaque/translucent texels
        if (const u32 img = sTevMap[s] < 8 ? sMapImage[sTevMap[s]] : 0; img) {
            const auto it = sImageDesc.find(img);
            if (it != sImageDesc.end() && (it->second & 0xFF) == GX_TF_RGB5A3) {
                const u32 n = (it->second >> 20) * ((it->second >> 8) & 0xFFF);
                const u8* p = hostp(0x80000000u | img);
                u32 opaque = 0, clear = 0;
                for (u32 t = 0; t < n; t++) {
                    const u16 v = u16(p[2 * t] << 8 | p[2 * t + 1]);
                    if (v & 0x8000) opaque++;
                    else if ((v >> 12) == 0) clear++;
                }
                std::snprintf(b, sizeof(b), " [5A3 opaque %u, alpha0 %u, translucent %u]", opaque, clear,
                              n - opaque - clear);
                k += b;
            }
        }
        const u32 m = (w >> 9) & 15;
        if (m) mtxUsed |= 1u << ((m - 1) & 3);
    }
    for (u32 m = 0; m < 3; m++) {
        if (!(mtxUsed & 1u << m)) continue;
        const f32* x = sInd.mtx[m];
        std::snprintf(b, sizeof(b), " mtx%u[%.4f %.4f %.4f; %.4f %.4f %.4f]e%d", m, x[0], x[1], x[2], x[3], x[4], x[5],
                      sInd.exp[m]);
        k += b;
    }
    sIndFrame[k]++;
}
static void indEndFrame() {
    if (!indLogOn()) return;
    if (sIndFrame != sIndLast) {
        std::string text;
        for (const auto& [k, n] : sIndFrame) text += "\n  " + k + (n > 1 ? " x" + std::to_string(n) : "");
        log("[ind] frame %u (same as previous for %u frames):%s", sDiagFrame, sIndLastFrame,
            sIndFrame.empty() ? " none" : text.c_str());
        std::fflush(stdout);
        sIndLastFrame = 0;
    }
    sIndLastFrame++;
    sIndLast = std::move(sIndFrame);
    sIndFrame.clear();
}

static u32 sDlFrame = 0;
// CF_DL_TRACE_FRAMES=<first>,<last>: only those frames (by the trace's own frame count).
static bool dlTraceFrameOn() {
    static const std::pair<u32, u32> range = [] {
        u32 a = 0, b = ~0u;
        if (const char* e = std::getenv("CF_DL_TRACE_FRAMES")) std::sscanf(e, "%u,%u", &a, &b);
        return std::pair<u32, u32>{a, b};
    }();
    return sDlFrame >= range.first && sDlFrame <= range.second;
}
static FILE* dlTraceFile() {
    static FILE* f = [] { const char* p = std::getenv("CF_DL_TRACE"); return p ? std::fopen(p, "w") : nullptr; }();
    return f && dlTraceFrameOn() ? f : nullptr;
}

u32 copyDispCount();

// Test aid CF_FIFO_CORRUPT=<frame>[:<kind>][,<frame>[:<kind>]...]: at the end of that game frame
// (GXCopyDisp count) malformed data goes into the FIFO (or the frame's tape), to exercise the
// command processor's recovery (Aurora 0235). Kinds: byte (one 0x1D, the issue #17 opcode; default),
// draw (a draw of 32767 vertices with no data), junk (96 pseudo-random bytes), xf (an XF load of
// 4096 words with none following).
static void injectFifoCorruption() {
    static const std::vector<std::pair<u32, std::string>> sPlan = [] {
        std::vector<std::pair<u32, std::string>> v;
        const char* e = std::getenv("CF_FIFO_CORRUPT");
        for (std::string s = e ? e : ""; !s.empty();) {
            const size_t comma = s.find(',');
            const std::string item = s.substr(0, comma);
            const size_t colon = item.find(':');
            v.emplace_back(u32(std::strtoul(item.c_str(), nullptr, 10)), colon == std::string::npos ? "byte" : item.substr(colon + 1));
            s = comma == std::string::npos ? "" : s.substr(comma + 1);
        }
        return v;
    }();
    for (const auto& [frame, kind] : sPlan) {
        if (frame != sDiagFrame) continue;
        log("[fifo-test] frame %u: injecting malformed FIFO data (%s)%s", frame, kind.c_str(),
            interp::capturing() ? " into the frame's tape" : "");
        if (kind == "draw") {
            GXCmd1u8(0x90);
            GXCmd1u16(0x7FFF);
        } else if (kind == "junk") {
            u32 x = 0x12345678u ^ frame;
            for (int i = 0; i < 96; i++) {
                x = x * 1664525u + 1013904223u;
                GXCmd1u8(u8(x >> 24));
            }
        } else if (kind == "xf") {
            GXCmd1u8(0x10);
            GXCmd1u32(0x0FFF0000u);
        } else {
            GXCmd1u8(0x1D);
        }
    }
}

void gx::endFrame() {
    endPendingDraw();
    injectFifoCorruption();
    interp::snapEndFrame();
    efbcopy::onFrame(copyDispCount());
    indEndFrame();
    if (FILE* f = dlTraceFile()) std::fprintf(f, "\nF%u", sDlFrame), std::fflush(f);
    sDlFrame++;
    if (!sTrace) return;
    static u32 frameNo = 0;
    if (sTraceFrame != sTraceLast) {
        std::string text;
        for (const auto& [note, n] : sTraceFrame) text += "\n  " + note + (n > 1 ? " x" + std::to_string(n) : "");
        log("[ws] frame %u (factor %.3f):%s", frameNo, ws::factor(), text.c_str());
        std::fflush(stdout);
    }
    frameNo++;
    sTraceLast = std::move(sTraceFrame);
    sTraceFrame.clear();
}

void gx::endPendingDraw() {
    if (!sInBegin) return;
    sInBegin = false;
    if (sHeld.active) {
        sHeld.active = false;
        sInHeldPrepare = true;
        const ProjMode mode = classifyHeld();
        if (mode == ProjMode::Scaled) widenWipeHeld();
        prepareDraw(mode, "held");
        sInHeldPrepare = false;
        HUD_TRACE("held %s x[%.3f,%.3f] mode=%d anchor=%d tex %s %s", sHeldDecoded ? "dec" : "nodec", sHeldLo, sHeldHi,
                  int(mode), sHeldAnchor,
                  sTevMap[0] < 8 && sMapImage[sTevMap[0]] ? imageDesc(sMapImage[sTevMap[0]]).c_str() : "-",
                  sHeldChain.c_str());
        sRawCheck = true;
        interp::snapBegin(sHeld.fmt, sHeld.verts);
        GXBegin(sHeld.prim, sHeld.fmt, sHeld.verts);
        if (interp::capturing()) interp::vtxDataAt(interp::mark());
        for (u32 w : sHeld.writes) {
            const int size = int(w >> 30) + 1;  // 1, 2 or 4 (encoded 0, 1, 3)
            const u32 off = w & 0x3FFFFFFF;
            u32 v = 0;
            for (int b = 0; b < size; b++) v = v << 8 | sHeld.bytes[off + b];
            rawFifoWrite(v, size);
        }
    }
    interp::snapEnd();
    GXEnd();
    interp::snapAfterDraw();
    interp::vtxEnd();
}

// Raw XF matrix loads from game code (NW4R g3d writes its model matrices straight into the
// write-gather pipe: 0x10, (count - 1) << 16 | address, then the floats). While a tape is being
// captured, the raw command stream outside immediate-mode draws is parsed so these loads are
// noted like GXLoadPosMtxImm & co. (interpolation pairs and blends them). The parser follows
// whole commands from the opcode on; it gives up on anything it cannot size (a raw primitive)
// until the next GX HLE call, which always starts at a command boundary.
namespace {
struct RawXf {
    enum State : u8 { Opcode, Skip, XfHeader, XfData, Lost } state = Opcode;
    u32 need = 0;        // bytes still to come in this state
    u32 header = 0;      // XF header bytes so far
    u32 addr = 0, count = 0;
    u32 at = 0;          // tape offset before the opcode
    bool atValid = false;
} sRawXf;
u32 sRawXfNoted = 0, sRawXfLost = 0;

// Matrix arrays (GXSetArray GX_POS_MTX_ARRAY..GX_LIGHT_ARRAY: CP arrays 12-15) for indexed loads.
u32 sMtxArray[4] = {};
u8 sMtxStride[4] = {};
u32 sIndexedConverted = 0;

// While a tape is captured, an indexed matrix load (XF reads the matrix from the array in memory
// when the GPU processes the command) would read the array when the tape is replayed, a game
// frame or more later, after the game has rewritten it: NW4R g3d computes its view-space matrices
// into the same buffers every frame, so every model was drawn with the newest frame's matrices
// and nothing could be blended. Such loads are sent as immediate loads of what is in memory now
// (what the console's GPU reads) and noted for blending. Not inside display lists being recorded.
bool indexedAsImmediate(int array, u32 index, u32 xfAddr, u32 n) {
    if (!interp::capturing() || sInDisplayList || array < 0 || array > 2) return false;
    const u32 base = sMtxArray[array];
    if (!base || n == 0 || n > 16) return false;
    const u32 src = base + index * sMtxStride[array];
    const u32 at = mark3D();
    rawFifoWrite(GX_LOAD_XF_REG, 1);
    rawFifoWrite(((n - 1) << 16) | xfAddr, 4);
    for (u32 i = 0; i < n; i++) rawFifoWrite(rd32(src + 4 * i), 4);
    if (array == 1) interp::noteBySource(at, interp::Kind::NrmMtx, src, (xfAddr - 0x400) / 3, int(n));
    else if (array == 0 && xfAddr < 0x78 && n == 12) interp::noteBySource(at, interp::Kind::PosMtx, src, xfAddr / 4, 12);
    else interp::noteBySource(at, interp::Kind::TexMtx, src, xfAddr / 4, int(n));
    sIndexedConverted++;
    return true;
}

struct RawIndx;
void rawIndxFlush();
void rawCallFlush();
void rawXfReset() {
    rawIndxFlush();
    rawCallFlush();
    sRawXf.state = RawXf::Opcode;
}
u32 sRawCallsInlined = 0, sRawCallsLeft = 0;
u32 sListLoadsConverted = 0;  // indexed loads in display lists made immediate (inlineDisplayList)
// Once a minute of interpolated play: how the raw matrix loads were handled.
void rawXfReport() {
    static u32 sFrames = 0;
    if (!interp::capturing() || ++sFrames % 3600 != 0) return;
    log("[interp] raw XF matrix loads noted %u, indexed loads sent as immediate %u, raw commands not followed %u, "
        "raw display list calls inlined %u (left as calls %u), indexed loads in display lists made immediate %u "
        "(last 3600 game frames)", sRawXfNoted, sIndexedConverted, sRawXfLost, sRawCallsInlined, sRawCallsLeft,
        sListLoadsConverted);
    sRawXfNoted = sIndexedConverted = sRawXfLost = sRawCallsInlined = sRawCallsLeft = sListLoadsConverted = 0;
}

// Raw display list calls (GX_CMD_CALL_DL 0x40, physical address, size) written by game code
// (NW4R g3d's material/shape lists, EGG): Aurora 0300 processes the called list in place when its
// FIFO thread reaches it, i.e. when a tape is replayed, a game frame or more later, by which time
// the game may have rewritten the list (g3d keeps material lists it patches every frame: colour
// and texture animation results). While a tape is captured such a call is held back and made as
// a GXCallDisplayList, so the list is inlined into the tape as it is now (what the console's GPU
// reads) and parsed like any display list (its colour writes noted for blending, its draw
// identified). Not while recording a display list (capture suspended there).
struct RawCall {
    bool active = false;
    u32 need = 0;
    u64 word = 0;
} sRawCall;
void rawCallFlush() {
    auto& h = sRawCall;
    if (!h.active) return;
    h.active = false;
    rawFifoWrite(GX_CMD_CALL_DL, 1);
    const u32 got = 8 - h.need;  // bytes held after the opcode
    for (u32 b = got; b-- > 0;) rawFifoWrite(u8(h.word >> (8 * b)), 1);
}
// Physical (or cached/uncached virtual) display list [a, a + n) -> guest virtual address of a, 0 if
// it is not inside MEM1 or MEM2.
u32 dlVirtual(u32 a, u32 n) {
    const u64 p = a & 0x3FFFFFFFu;
    if (p + n <= kMem1Size) return 0x80000000u | u32(p);
    if (p >= 0x10000000u && p + n <= 0x10000000u + u64(kMem2Size)) return 0x90000000u | u32(p - 0x10000000u);
    return 0;
}
void callDisplayListInline(u32 addr, u32 size);  // below (GXCallDisplayList's path)
void inlineDisplayList(u32 addr, u32 size);
bool rawCallWrite(u32 v, int size) {
    auto& h = sRawCall;
    if (!h.active) {
        // CF_RAW_DL_INLINE=0 (A/B): raw calls left to Aurora (the list read when the tape is replayed).
        static const bool sOff = [] { const char* e = std::getenv("CF_RAW_DL_INLINE"); return e && *e == '0'; }();
        if (sOff) return false;
        if (sRawXf.state != RawXf::Opcode || size != 1 || u8(v) != GX_CMD_CALL_DL) return false;
        h = {true, 8, 0};
        return true;
    }
    if (u32(size) > h.need) {  // misaligned: forward what was held, stop following this command
        rawCallFlush();
        sRawXf.state = RawXf::Lost;
        return false;
    }
    h.word = h.word << (8 * size) | (size == 4 ? v : (v & ((1u << (8 * size)) - 1)));
    h.need -= u32(size);
    if (h.need) return true;
    h.active = false;
    const u32 addr = u32(h.word >> 32), n = u32(h.word);
    const u32 virt = n && n <= (16u << 20) ? dlVirtual(addr, n) : 0;
    if (!virt) {
        rawFifoWrite(GX_CMD_CALL_DL, 1);
        rawFifoWrite(addr, 4);
        rawFifoWrite(n, 4);
        sRawCallsLeft++;
        return true;
    }
    callDisplayListInline(virt, n);
    sRawCallsInlined++;
    return true;
}

// Raw indexed loads (0x20 pos, 0x28 normal, 0x30 texture matrix; then index << 16 |
// (count - 1) << 12 | address) are held back and sent as immediate loads (indexedAsImmediate).
struct RawIndx {
    bool active = false;
    u8 opcode = 0;
    u32 need = 0, word = 0;
} sRawIndx;
void rawIndxFlush() {
    auto& h = sRawIndx;
    if (!h.active) return;
    h.active = false;
    rawFifoWrite(h.opcode, 1);
    for (u32 b = 4 - h.need; b-- > 0;) rawFifoWrite(u8(h.word >> (8 * b)), 1);
}
// True when the write was consumed (held or converted).
bool rawIndxWrite(u32 v, int size) {
    auto& h = sRawIndx;
    if (!h.active) {
        if (sRawXf.state != RawXf::Opcode || size != 1) return false;
        const u8 op = u8(v);
        if (op != GX_LOAD_INDX_A && op != GX_LOAD_INDX_B && op != GX_LOAD_INDX_C) return false;
        h = {true, op, 4, 0};
        return true;
    }
    if (u32(size) > h.need) {  // misaligned: give up on it, forward what was held
        h.active = false;
        rawFifoWrite(h.opcode, 1);
        for (u32 b = 4 - h.need; b-- > 0;) rawFifoWrite(u8(h.word >> (8 * b)), 1);
        sRawXf.state = RawXf::Lost;  // the rest of this command is not followed
        return false;
    }
    h.word = (size == 4 ? 0 : h.word << (8 * size)) | (size == 4 ? v : (v & ((1u << (8 * size)) - 1)));
    h.need -= u32(size);
    if (h.need) return true;
    h.active = false;
    const u32 index = h.word >> 16, n = ((h.word >> 12) & 0xF) + 1, addr = h.word & 0xFFF;
    const int array = h.opcode == GX_LOAD_INDX_A ? 0 : h.opcode == GX_LOAD_INDX_B ? 1 : 2;
    if (!indexedAsImmediate(array, index, addr, n)) {
        rawFifoWrite(h.opcode, 1);
        rawFifoWrite(h.word, 4);
    }
    return true;
}

void rawXfFinish() {
    auto& p = sRawXf;
    if (!p.atValid) return;
    // XF memory: 0x000-0x077 position matrices (12 floats each), 0x078-0x0FF texture matrices,
    // 0x400-0x45F normal matrices (9 floats), 0x500-0x5FF post-transform (dual texture) matrices.
    if (p.addr + p.count <= 0x78 && p.count == 12 && p.addr % 4 == 0) {
        interp::note(p.at, interp::Kind::PosMtx, 0, p.addr / 4, 12);
        sRawXfNoted++;
    } else if (p.addr >= 0x78 && p.addr + p.count <= 0x100 && (p.count == 8 || p.count == 12)) {
        interp::note(p.at, interp::Kind::TexMtx, 0, p.addr / 4, int(p.count));
        sRawXfNoted++;
    } else if (p.addr >= 0x400 && p.addr + p.count <= 0x460 && p.count == 9) {
        interp::note(p.at, interp::Kind::NrmMtx, 0, (p.addr - 0x400) / 3, 9);
        sRawXfNoted++;
    } else if (p.addr >= 0x500 && p.addr + p.count <= 0x600 && p.count == 12) {
        interp::note(p.at, interp::Kind::TexMtx, 0, 64 + (p.addr - 0x500) / 3, 12);
        sRawXfNoted++;
    }
}

// Before the write (v, size) goes to the FIFO: a new XF command's opcode marks its tape offset.
void rawXfBefore(u32 v, int size) {
    if (sRawXf.state == RawXf::Opcode) {
        sRawXf.atValid = u8(v >> (8 * (size - 1))) == GX_LOAD_XF_REG;
        if (sRawXf.atValid) sRawXf.at = interp::mark();
    }
}
// After it: advance the parser over its bytes; true when an XF load just completed.
bool rawXfAfter(u32 v, int size) {
    auto& p = sRawXf;
    bool done = false;
    for (int b = size - 1; b >= 0; b--) {
        const u8 byte = u8(v >> (8 * b));
        switch (p.state) {
        case RawXf::Opcode:
            if (b != size - 1) p.atValid = false;  // an opcode inside a wider write: not marked
            switch (byte) {
            case 0x00: case 0x48: break;                                   // NOP, invalidate vertex cache
            case GX_LOAD_BP_REG: p.state = RawXf::Skip; p.need = 4; break;
            case GX_LOAD_CP_REG: p.state = RawXf::Skip; p.need = 5; break;
            case GX_LOAD_INDX_A: case GX_LOAD_INDX_B: case GX_LOAD_INDX_C: case GX_LOAD_INDX_D:
                p.state = RawXf::Skip; p.need = 4; break;
            case GX_CMD_CALL_DL: p.state = RawXf::Skip; p.need = 8; break;
            case GX_LOAD_XF_REG: p.state = RawXf::XfHeader; p.need = 4; p.header = 0; break;
            default: p.state = RawXf::Lost; sRawXfLost++; break;              // a raw primitive or unknown
            }
            break;
        case RawXf::Skip:
            if (--p.need == 0) p.state = RawXf::Opcode;
            break;
        case RawXf::XfHeader:
            p.header = p.header << 8 | byte;
            if (--p.need == 0) {
                p.count = ((p.header >> 16) & 0xFFFF) + 1;
                p.addr = p.header & 0xFFFF;
                p.need = p.count * 4;
                p.state = RawXf::XfData;
            }
            break;
        case RawXf::XfData:
            if (--p.need == 0) {
                p.state = RawXf::Opcode;
                done = true;
            }
            break;
        case RawXf::Lost: break;
        }
    }
    return done;
}
}  // namespace

namespace {
std::vector<u32> sLineWords;  // CF_LINE_LOG (GXBegin): words of the draw being logged
u32 sLineWant = 0;
std::string sLineHead;
void lineWord(u32 v, int size) {
    if (!sLineWant || size != 4) return;
    sLineWords.push_back(v);
    if (--sLineWant) return;
    std::string s = sLineHead;
    char b[24];
    for (u32 w : sLineWords) {
        std::snprintf(b, sizeof b, " %.3f", std::bit_cast<f32>(w));
        s += b;
    }
    log("[line] %s", s.c_str());
}
}  // namespace

void hw::fifoWrite(u32 v, int size) {
    if (sLineWant) [[unlikely]] lineWord(v, size);
    frame::gxWaitForGuestTurn();
    interp::joinCapture();
    if (dropDraw()) [[unlikely]] return;
    if (!sHeld.active && !sInBegin && interp::capturing()) {
        if (sRawCheck) {
            sRawCheck = false;
            prepareDraw(drawMode(ProjMode::Scaled), "fifo");
        }
        if (rawIndxWrite(v, size)) return;
        if (rawCallWrite(v, size)) return;
        rawXfBefore(v, size);
        rawFifoWrite(v, size);
        interp::vtxBytes(v, size);
        if (rawXfAfter(v, size)) rawXfFinish();
        return;
    }
    if (sHeld.active) [[unlikely]] {
        if (interp::capturing()) interp::vtxBytes(v, size);  // (its tape offset: vtxDataAt when replayed)
        const u32 off = u32(sHeld.bytes.size());
        for (int b = size - 1; b >= 0; b--) sHeld.bytes.push_back(u8(v >> (8 * b)));
        sHeld.writes.push_back(off | u32(size - 1) << 30);
        return;
    }
    if (sRawCheck && !sInBegin) [[unlikely]] {
        sRawCheck = false;
        prepareDraw(drawMode(ProjMode::Scaled), "fifo");
    }
    rawFifoWrite(v, size);
    if (interp::capturing()) interp::vtxBytes(v, size);
}

// --- Malformed FIFO data (Aurora 0235) ---------------------------------------------------------
// Aurora reports a bad command with its absolute stream position, on its FIFO thread. Commands the
// guest sends directly (lockstep frames) are found in a ring of recent GX call sites with the
// stream position each started at; captured frames (interpolation) are found through the replay
// that put them in the stream (interp::describeStreamPos).
std::string guestSymbolize(u32 addr);  // runtime.cpp

namespace {
struct CallSite {
    std::atomic<u64> pos{0};
    std::atomic<u32> lr{0};
    std::atomic<u32> frame{0};
};
constexpr u32 kCallSites = 1024;
CallSite sCallSites[kCallSites];
std::atomic<u32> sCallSiteNext{0};

void fifoErrorReport(const AuroraFifoError* e) {
    std::string where = interp::describeStreamPos(e->streamPos);
    if (where.empty()) {
        // The last call sites that started at or before the bad byte (newest first).
        struct Hit { u64 pos; u32 lr, frame; };
        std::vector<Hit> hits;
        for (const CallSite& s : sCallSites) {
            const u64 pos = s.pos.load(std::memory_order_relaxed);
            if (pos != 0 && pos <= e->streamPos) hits.push_back({pos, s.lr.load(std::memory_order_relaxed), s.frame.load(std::memory_order_relaxed)});
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.pos > b.pos; });
        char buf[64];
        for (size_t i = 0; i < hits.size() && i < 4; i++) {
            std::snprintf(buf, sizeof(buf), "%s-%llu B: ", i ? "; " : "", static_cast<unsigned long long>(e->streamPos - hits[i].pos));
            where += buf + guestSymbolize(hits[i].lr);
            if (i == 0) where += " (game frame " + std::to_string(hits[i].frame) + ")";
        }
        where = where.empty() ? "no GX call recorded before it" : "GX calls before it (bytes before the bad one): " + where;
    }
    log("[fifo] malformed GX command (%s), error %u, %s; %s", e->what, e->count,
        e->resynced ? "resynchronized" : "rest of the block dropped", where.c_str());
}
}  // namespace

void gx::noteCallSite(u32 lr) {
    if (interp::capturing()) {
        interp::noteCallSite(lr);
        return;
    }
    CallSite& s = sCallSites[sCallSiteNext.fetch_add(1, std::memory_order_relaxed) % kCallSites];
    s.pos.store(AuroraFifoStreamPos(), std::memory_order_relaxed);
    s.lr.store(lr, std::memory_order_relaxed);
    s.frame.store(sDiagFrame, std::memory_order_relaxed);
}

void gx::installFifoErrorReport() { AuroraSetFifoErrorCallback(&fifoErrorReport); }

namespace {
// Before every GX call that can reach the FIFO (GXHLE). Guest threads are emulated on host threads
// and share one CPU FIFO, as on the console: a guest thread other than the one capturing the frame
// for interpolation joins its capture (interp::joinCapture; it used to write to Aurora's FIFO
// directly, at the same time as the host's replays). The open draw is closed first (Aurora needs
// GXEnd), but only by the thread that opened it: a loader thread that ran while the drawing thread
// was preempted between GXBegin and its vertices used to close that draw early, and the rest of
// its vertices then arrived as stray FIFO bytes (docs/RENDERING.md "Malformed FIFO data").
void beforeGxCall(const Ctx& c) {
    interp::joinCapture();
    if (sInBegin && sched::currentGuest() != sBeginGuest) [[unlikely]] {
        static int sLogged = 0;
        if (sLogged++ < 8)
            log("[gx] GX call (lr %08X) from guest thread %08X while thread %08X has a draw open; left open", c.lr,
                sched::currentGuest(), sBeginGuest);
    } else {
        gx::endPendingDraw();
    }
    gx::noteCallSite(c.lr);
    interp::gxCall(c);
    rawXfReset();
}
}  // namespace

#define GXHLE(name)                                       \
    static void gxImpl_##name(::recomp::Ctx& c);          \
    HLE(name) {                                           \
        frame::gxWaitForGuestTurn();                      \
        beforeGxCall(c);                                  \
        gxImpl_##name(c);                                 \
    }                                                     \
    static void gxImpl_##name([[maybe_unused]] ::recomp::Ctx& c)

// GX functions that only fill in a guest-memory object or compute a value (GXInitTexObj,
// GXInitLight*, GXGetTexBufferSize...): no FIFO traffic on the console, so they neither close the
// open draw nor count as a GX call site; any guest thread may call them at any time (LM's loader
// thread does, for every map load, while the game thread draws).
#define GXHLE_CPU(name)                                   \
    static void gxImpl_##name(::recomp::Ctx& c);          \
    HLE(name) {                                           \
        frame::gxWaitForGuestTurn();                      \
        gxImpl_##name(c);                                 \
    }                                                     \
    static void gxImpl_##name([[maybe_unused]] ::recomp::Ctx& c)

// GX state audit (CF_GX_AUDIT=1): logs each distinct argument combination of the state calls
// that matter for hardware accuracy (copy filter, gamma, fog, lights, LOD...), once.
static void gxAudit(const char* fmt, ...) {
    static const bool on = [] {
        const char* e = std::getenv("CF_GX_AUDIT");
        return e && e[0] == '1';
    }();
    if (!on) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static std::vector<std::string> seen;
    if (std::find(seen.begin(), seen.end(), buf) != seen.end()) return;
    seen.emplace_back(buf);
    log("[gx-audit] %s", buf);
}

// --- setup / sync ---------------------------------------------------------------

GXHLE(GXInit) {
    static u32 fifoObj = 0;
    if (!fifoObj) fifoObj = hleAlloc(128);
    GXInit(hostp(c.r[3]), c.r[4]);
    // Game-built display lists and raw FIFO writes (NW4R g3d's shape lists set vertex array bases,
    // EGG calls display lists) carry physical addresses: MEM1 at 0, MEM2 at 0x10000000 (Aurora 0300).
    GXSetAuroraPhysicalMemoryRange(0, kMem1Size, g_mem);
    GXSetAuroraPhysicalMemoryRange(0x10000000, kMem2Size, g_mem + kMem2Offset);
    sInd = {};
    c.r[3] = fifoObj;
}
GXHLE(GXFlush) { GXFlush(); }
// The console's GXDrawDone sleeps until the draw-done interrupt, so interrupts and other
// threads run meanwhile; draining Aurora's FIFO here can take a good part of a frame.
GXHLE(GXDrawDone) { frame::gxBlockingWait([] { GXDrawDone(); }); }
// Draw-done callback (GXSetDrawDoneCallback): the console raises it when the GPU reaches the
// GXSetDrawDone token; here the GPU is done as far as the game can tell, so it runs as an
// interrupt right away.
static u32 sDrawDoneCb = 0;
static void drawDoneIrq(Ctx& c) {
    if (sDrawDoneCb) guestCall(c, sDrawDoneCb);
}
GXHLE(GXSetDrawDone) {
    if (sDrawDoneCb) sched::postIrq(&drawDoneIrq);
}
GXHLE(GXWaitDrawDone) {}
GXHLE(GXPixModeSync) { GXPixModeSync(); }
GXHLE(GXSetMisc) {}
GXHLE(GXInvalidateVtxCache) {
    GXInvalidateVtxCache();
    interp::invalidateVtxCache();
}
GXHLE(GXInvalidateTexAll) { GXInvalidateTexAll(); }

// --- vertex format ----------------------------------------------------------------

GXHLE(GXClearVtxDesc) {
    std::memset(sDesc, 0, sizeof(sDesc));
    GXClearVtxDesc();
}
GXHLE(GXSetVtxDesc) {
    if (const int a = attrSlot(c.r[3]); a >= 0) sDesc[a] = u8(c.r[4]);
    GXSetVtxDesc(GXAttr(c.r[3]), GXAttrType(c.r[4]));
}
GXHLE(GXSetVtxDescv) {
    GXVtxDescList list[32];
    int n = 0;
    for (u32 p = c.r[3]; n < 31; p += 8) {
        list[n].attr = GXAttr(rd32(p));
        list[n].type = GXAttrType(rd32(p + 4));
        if (const int a = attrSlot(list[n].attr); a >= 0) sDesc[a] = u8(list[n].type);
        if (list[n++].attr == GX_VA_NULL) break;
    }
    list[n - 1].attr = GX_VA_NULL;
    GXSetVtxDescv(list);
}
GXHLE(GXGetVtxDesc) {
    GXAttrType t;
    GXGetVtxDesc(GXAttr(c.r[3]), &t);
    wr32(c.r[4], u32(t));
}
GXHLE(GXSetVtxAttrFmt) {
    if (const int a = attrSlot(c.r[4]); a >= 0 && c.r[3] < GX_MAX_VTXFMT)
        sVat[c.r[3]][a] = {u8(c.r[5]), u8(c.r[6]), u8(c.r[7])};
    GXSetVtxAttrFmt(GXVtxFmt(c.r[3]), GXAttr(c.r[4]), GXCompCnt(c.r[5]), GXCompType(c.r[6]), u8(c.r[7]));
}
GXHLE(GXSetVtxAttrFmtv) {
    GXVtxAttrFmtList list[32];
    int n = 0;
    for (u32 p = c.r[4]; n < 31; p += 16) {
        list[n] = {GXAttr(rd32(p)), GXCompCnt(rd32(p + 4)), GXCompType(rd32(p + 8)), u8(rd8(p + 12))};
        if (const int a = attrSlot(list[n].attr); a >= 0 && c.r[3] < GX_MAX_VTXFMT)
            sVat[c.r[3]][a] = {u8(list[n].cnt), u8(list[n].type), list[n].frac};
        if (list[n++].attr == GX_VA_NULL) break;
    }
    list[n - 1].attr = GX_VA_NULL;
    GXSetVtxAttrFmtv(GXVtxFmt(c.r[3]), list);
}
// Size 0: Aurora sizes the array from the indices each draw uses (patch 0002).
GXHLE(GXSetArray) {
    if (c.r[3] == GX_VA_POS) {
        sPosArray = c.r[4];
        sPosStride = u8(c.r[5]);
    }
    if (c.r[3] >= GX_POS_MTX_ARRAY && c.r[3] <= GX_LIGHT_ARRAY) {
        sMtxArray[c.r[3] - GX_POS_MTX_ARRAY] = c.r[4];
        sMtxStride[c.r[3] - GX_POS_MTX_ARRAY] = u8(c.r[5]);
    }
    const u32 at = interp::mark();
    GXSetArray(GXAttr(c.r[3]), ptr(c.r[4]), 0, u8(c.r[5]), false);
    interp::arrayBase(at, c.r[3], c.r[4], u8(c.r[5]));
}

GXHLE(GXBegin) {
    if (dropDraw()) return;
    indNoteDraw(c, "begin");
    sInBegin = true;
    sBeginGuest = sched::currentGuest();
    if (interp::drawLogActive()) interp::drawLogCaller(guestChain(c));
    if (interp::capturing()) interp::vtxBegin(c.r[3], c.r[4], c.r[5], sProj.type == GX_PERSPECTIVE);
    if (ws::active() && sProj.type == GX_ORTHOGRAPHIC && viewportFull()) {
        sHeld.active = true;
        sHeldAnchor = ws::hudAnchor();
        sHeldShift = ws::hudShift();
        sHeldWipe = ws::inWipe();
        sHeld.prim = GXPrimitive(c.r[3]);
        sHeld.fmt = GXVtxFmt(c.r[4]);
        sHeld.verts = u16(c.r[5]);
        if (hudTraceOn()) sHeldChain = guestChain(c);
        sHeld.bytes.clear();
        sHeld.writes.clear();
        return;
    }
    prepareDraw(drawMode(ProjMode::Scaled), "begin");
    interp::snapBegin(c.r[4], c.r[5]);
    // Test aid CF_LINE_LOG=1: the first words of the first few line/point draws per 60 frames.
    static const bool sLineLog = std::getenv("CF_LINE_LOG") != nullptr;
    if (sLineLog && (c.r[3] == GX_LINES || c.r[3] == GX_LINESTRIP || c.r[3] == GX_POINTS)) {
        static u32 sFrame = ~0u, sCount = 0;
        if (sDiagFrame / 60 != sFrame) sFrame = sDiagFrame / 60, sCount = 0;
        if (sCount++ < 3) {
            sLineWords.clear();
            sLineWant = 12;
            char b[128];
            std::snprintf(b, sizeof b, "f%u prim %02X fmt %u n %u lr %08X proj %s", sDiagFrame, c.r[3], c.r[4], c.r[5], c.lr,
                          sProj.type == GX_PERSPECTIVE ? "persp" : "ortho");
            sLineHead = b;
        }
    }
    GXBegin(GXPrimitive(c.r[3]), GXVtxFmt(c.r[4]), u16(c.r[5]));
}

GXHLE(GXCallDisplayList) {
    // A list outside guest memory (a corrupt pointer: the console's GPU would read garbage) would
    // crash the host reading it (Aurora, the vertex snapshots, interpolation's parse): skipped.
    if ((c.r[3] >> 28) != 0xE && ((c.r[3] & kLowWindowMask) + u64(c.r[4]) > kLowWindowMask + 1ull || c.r[4] > (16u << 20))) {
        static int sLogged = 0;
        if (sLogged++ < 8) log("[gx] GXCallDisplayList(%08X, %X) outside guest memory, skipped (lr %08X)", c.r[3], c.r[4], c.lr);
        return;
    }
    if (FILE* f = dlTraceFile()) {
        std::fprintf(f, " %08X:%X@%08X", c.r[3], c.r[4], c.lr);
        u32 sp = c.r[1];
        for (int i = 0; i < 10 && sp >= 0x80000000u && sp < 0x81800000u; i++) {
            sp = rd32(sp);
            if (sp < 0x80000000u || sp >= 0x81800000u) break;
            std::fprintf(f, "<%08X", rd32(sp + 4));
        }
    }
    if (dropDraw()) return;
    if (sProj.type == GX_ORTHOGRAPHIC || !viewportFull())
        HUD_TRACE("dlist %s vp(%.0f,+%.0f) %s", sProj.type == GX_ORTHOGRAPHIC ? "ortho" : "persp", sVp.left, sVp.wd,
                  guestChain(c).c_str());
    prepareDraw(drawMode(ProjMode::Scaled), "dlist");
    indNoteDraw(c, "dlist");
    if (interp::capturing() && !sInDisplayList) {
        if (interp::drawLogActive()) interp::drawLogCaller(guestChain(c));
        inlineDisplayList(c.r[3], c.r[4]);
        return;
    }
    interp::snapDisplayList(hostp(c.r[3]), c.r[4]);
    GXCallDisplayList(hostp(c.r[3]), c.r[4]);
    interp::snapAfterDraw();
    if (interp::drawLogActive()) interp::drawLogCaller(guestChain(c));
    if (interp::capturing()) interp::vtxDisplayList(c.r[3], hostp(c.r[3]), c.r[4]);
}
namespace {
// Indexed matrix loads inside a display list called while a tape is captured (NW4R g3d's shape
// pre-primitive lists load their position/normal/texture matrices with LOAD_INDX_A/B/C from the
// matrix arrays it rewrites every frame): replayed as they are, they read the arrays when the tape
// is replayed (the tape's array copy: frame b's matrices, never blended, so a walking character's
// parts were drawn a frame apart from the blended rest). The list is inlined as a copy in which
// those loads are immediate loads of the values in memory now, noted for blending like the raw
// stream's (indexedAsImmediate). The copy follows whole commands up to the first draw (or anything
// it cannot size); the rest is copied unchanged. CF_DL_INDEXED=0: lists inlined as they are (A/B).
struct ListLoad {
    u32 off, src, xf, n;
    int array;
};
std::vector<u8> sListCopy;
std::vector<ListLoad> sListLoads;
bool convertListIndexed(const u8* p, u32 size) {
    static const bool sOff = [] { const char* v = std::getenv("CF_DL_INDEXED"); return v && *v == '0'; }();
    if (sOff) return false;
    auto be16 = [&](u32 o) { return u32(p[o] << 8 | p[o + 1]); };
    auto be32 = [&](u32 o) { return u32(p[o]) << 24 | u32(p[o + 1]) << 16 | u32(p[o + 2]) << 8 | p[o + 3]; };
    u32 base[3] = {sMtxArray[0], sMtxArray[1], sMtxArray[2]};
    u32 stride[3] = {sMtxStride[0], sMtxStride[1], sMtxStride[2]};
    sListCopy.clear();
    sListLoads.clear();
    u32 i = 0, copied = 0;
    auto put32 = [](u32 v) {
        for (int b = 3; b >= 0; b--) sListCopy.push_back(u8(v >> (8 * b)));
    };
    while (i < size) {
        const u8 op = p[i];
        u32 len;
        if (op == 0x00 || op == 0x48) len = 1;
        else if (op == 0x08) len = 6;
        else if (op == 0x10) len = i + 5 <= size ? 5 + 4 * (((be32(i + 1) >> 16) & 0xFFFF) + 1) : 0;
        else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) len = 5;
        else if (op == 0x40) len = 9;
        else if (op == 0x61) len = 5;
        else break;  // a draw or an unknown command: the rest as it is
        if (len == 0 || i + len > size) break;
        if (op == 0x08) {  // CP: matrix array base (0xAC-0xAE) / stride (0xBC-0xBE), physical addresses
            const u8 reg = p[i + 1];
            const u32 v = be32(i + 2);
            if (reg >= 0xAC && reg <= 0xAE) base[reg - 0xAC] = dlVirtual(v, 4) ? dlVirtual(v, 4) : 0;
            else if (reg >= 0xBC && reg <= 0xBE) stride[reg - 0xBC] = v & 0xFF;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30) {
            const int array = (op - 0x20) / 8;
            const u32 idx = be16(i + 1), n = (be16(i + 3) >> 12) + 1, xf = be16(i + 3) & 0xFFF;
            const u32 src = base[array] ? base[array] + idx * stride[array] : 0;
            if (src && n <= 16 && dlVirtual(src, 4 * n)) {
                sListCopy.insert(sListCopy.end(), p + copied, p + i);
                sListLoads.push_back({u32(sListCopy.size()), src, xf, n, array});
                sListCopy.push_back(GX_LOAD_XF_REG);
                put32((n - 1) << 16 | xf);
                for (u32 k = 0; k < n; k++) put32(rd32(src + 4 * k));
                copied = i + len;
            }
        }
        i += len;
    }
    if (sListLoads.empty()) return false;
    sListCopy.insert(sListCopy.end(), p + copied, p + size);
    return true;
}

// GXCallDisplayList's path while a tape is captured (the HLE and raw calls from the write-gather pipe).
void inlineDisplayList(u32 addr, u32 size) {
    if (!convertListIndexed(hostp(addr), size)) {
        GXCallDisplayList(hostp(addr), size);
        interp::vtxDisplayList(addr, hostp(addr), size);
        return;
    }
    const u32 n = u32(sListCopy.size());
    GXCallDisplayList(sListCopy.data(), n);
    const u32 end = interp::mark();
    if (end != ~0u && end >= n) {
        const u32 at0 = end - n;
        for (const ListLoad& l : sListLoads) {
            const u32 at = at0 + l.off;
            if (l.array == 1) interp::noteBySource(at, interp::Kind::NrmMtx, l.src, (l.xf - 0x400) / 3, int(l.n));
            else if (l.array == 0 && l.xf < 0x78 && l.n == 12) interp::noteBySource(at, interp::Kind::PosMtx, l.src, l.xf / 4, 12);
            else interp::noteBySource(at, interp::Kind::TexMtx, l.src, l.xf / 4, int(l.n));
        }
        sListLoadsConverted += u32(sListLoads.size());
    }
    interp::vtxDisplayList(addr, sListCopy.data(), n);
}

// A raw GX_CMD_CALL_DL from the write-gather pipe while a tape is captured (rawCallWrite).
void callDisplayListInline(u32 addr, u32 size) {
    prepareDraw(drawMode(ProjMode::Scaled), "dlist");
    inlineDisplayList(addr, size);
}
}  // namespace
// Display lists the game records (GXBeginDisplayList/GXEndDisplayList). Aurora's commands are not
// all the console's size (texture and array loads are longer, draws carry a size), so a list sized
// for the console can need more room than the game's buffer. Lists are recorded into a host buffer;
// one that fits is copied into the game's buffer as before. One that does not stays in host memory
// and the game's buffer gets a 32-byte call to it (Aurora 0302 GX_AURORA_CALL_HOST_DL), so the
// game calls it through its own buffer as usual. A host list is kept while its game buffer holds
// the call to it, and for 16 frames after it is replaced (tapes replay recorded frames later).
namespace {
u32 sDlBufSize = 0, sDlBeginLr = 0, sDlGuest = 0;
std::vector<u8> sDlScratch;
struct HostDl {
    std::unique_ptr<u8[]> data;
    u32 size = 0;
};
std::unordered_map<u32, HostDl> sHostDls;  // by game buffer address
std::deque<std::pair<u32, std::unique_ptr<u8[]>>> sRetiredDls;  // (frame retired, data)
constexpr u32 kHostDlMarker = 32;
}  // namespace
GXHLE(GXBeginDisplayList) {
    sInDisplayList = true;
    sDlGuest = c.r[3];
    sDlBufSize = c.r[4];
    sDlBeginLr = c.lr;
    interp::suspendForDisplayList();
    sDlScratch.resize(std::max<size_t>(size_t(sDlBufSize) * 4, 256 * 1024));
    GXBeginDisplayList(sDlScratch.data(), u32(sDlScratch.size()));
}
GXHLE(GXEndDisplayList) {
    sInDisplayList = false;
    const u32 n = GXEndDisplayList();
    interp::resumeAfterDisplayList();
    while (!sRetiredDls.empty() && sDiagFrame - sRetiredDls.front().first > 16) sRetiredDls.pop_front();
    if (auto it = sHostDls.find(sDlGuest); it != sHostDls.end()) {
        sRetiredDls.emplace_back(sDiagFrame, std::move(it->second.data));
        sHostDls.erase(it);
    }
    static const bool sDlLog = std::getenv("CF_DL_LOG") != nullptr;
    if (sDlLog)
        log("[gx] display list recorded: %u bytes, game buffer %u bytes at %08X (begun at %s)", n, sDlBufSize,
            sDlGuest, guestSymbolize(sDlBeginLr).c_str());
    // Test aid CF_DL_FORCE_HOST=1: every list over 32 bytes goes the host-memory way.
    static const bool sForceHost = std::getenv("CF_DL_FORCE_HOST") != nullptr;
    if (n <= sDlBufSize && !(sForceHost && n > kHostDlMarker)) {
        std::memcpy(hostp(sDlGuest), sDlScratch.data(), n);
        c.r[3] = n;
        return;
    }
    if (sDlBufSize < kHostDlMarker || n == 0) {  // no room even for the call, or the scratch overflowed
        static int sLogged = 0;
        if (sLogged++ < 16)
            log("[gx] display list does not fit its %u-byte buffer (begun at %s); size 0", sDlBufSize,
                guestSymbolize(sDlBeginLr).c_str());
        c.r[3] = 0;
        return;
    }
    HostDl dl{std::make_unique<u8[]>(n), n};
    std::memcpy(dl.data.get(), sDlScratch.data(), n);
    u8* m = hostp(sDlGuest);
    std::memset(m, 0, kHostDlMarker);  // NOPs after the call
    const u64 p = reinterpret_cast<u64>(dl.data.get());
    m[0] = GX_AURORA;
    m[1] = u8(GX_AURORA_CALL_HOST_DL >> 8);
    m[2] = u8(GX_AURORA_CALL_HOST_DL);
    for (int i = 0; i < 8; i++) m[3 + i] = u8(p >> (56 - 8 * i));
    for (int i = 0; i < 4; i++) m[11 + i] = u8(n >> (24 - 8 * i));
    static int sLogged = 0;
    if (sLogged++ < 16)
        log("[gx] display list of %u bytes kept in host memory (game buffer %u bytes at %08X, begun at %s)", n,
            sDlBufSize, sDlGuest, guestSymbolize(sDlBeginLr).c_str());
    sHostDls[sDlGuest] = std::move(dl);
    c.r[3] = kHostDlMarker;
}

GXHLE(GXDrawSphere1) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawSphere1(u8(c.r[3]));
}
GXHLE(GXDrawCube) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawCube();
}
GXHLE(GXDrawCylinder) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawCylinder(u8(c.r[3]));
}

// --- texgen / transform ---------------------------------------------------------------

GXHLE(GXSetNumTexGens) {
    sInd.numTexGens = u8(c.r[3]);
    GXSetNumTexGens(u8(c.r[3]));
}
GXHLE(GXSetTexCoordGen2) {
    if (c.r[3] < 8) sInd.texGenMtx[c.r[3]] = u8(c.r[6]);
    GXSetTexCoordGen2(GXTexCoordID(c.r[3]), GXTexGenType(c.r[4]), GXTexGenSrc(c.r[5]), c.r[6], GXBool(c.r[7]),
                      c.r[8]);
}
// Sent to Aurora lazily, at the next draw, once the viewport and (for 2D) the
// vertices tell which widescreen treatment it gets.
GXHLE(GXSetProjection) {
    const Mtx<4, 4> m(c.r[3]);
    std::memcpy(sProj.m, m.m, sizeof(sProj.m));
    sProj.type = GXProjectionType(c.r[4]);
    sProjDirty = sProjSet = sRawCheck = true;
    traceNote("projection %s from %08X", c.r[4] ? "ortho" : "persp", c.lr);
    if (!ws::adjusting()) emitProjection(ProjMode::Plain);
}
// GXSetProjectionv(f32 v[7]): type, then the six significant matrix terms (GXGetProjectionv's
// form), rebuilt into the matrix GXSetProjection would take.
GXHLE(GXSetProjectionv) {
    const u32 v = c.r[3];
    const bool ortho = rdf32(v) != 0.0f;
    float m[4][4] = {};
    m[0][0] = rdf32(v + 4);
    m[1][1] = rdf32(v + 12);
    m[2][2] = rdf32(v + 20);
    m[2][3] = rdf32(v + 24);
    if (ortho) {
        m[0][3] = rdf32(v + 8);
        m[1][3] = rdf32(v + 16);
        m[3][3] = 1.0f;
    } else {
        m[0][2] = rdf32(v + 8);
        m[1][2] = rdf32(v + 16);
        m[3][2] = -1.0f;
    }
    std::memcpy(sProj.m, m, sizeof(sProj.m));
    sProj.type = ortho ? GX_ORTHOGRAPHIC : GX_PERSPECTIVE;
    sProjDirty = sProjSet = sRawCheck = true;
    if (!ws::adjusting()) emitProjection(ProjMode::Plain);
}
// GXGetProjectionv(f32 v[7]): the guest's own projection (not the widescreen-adjusted one).
GXHLE_CPU(GXGetProjectionv) {
    const u32 v = c.r[3];
    const bool ortho = sProj.type == GX_ORTHOGRAPHIC;
    wr32(v, bits32(ortho ? 1.0f : 0.0f));
    wr32(v + 4, bits32(sProj.m[0][0]));
    wr32(v + 8, bits32(ortho ? sProj.m[0][3] : sProj.m[0][2]));
    wr32(v + 12, bits32(sProj.m[1][1]));
    wr32(v + 16, bits32(ortho ? sProj.m[1][3] : sProj.m[1][2]));
    wr32(v + 20, bits32(sProj.m[2][2]));
    wr32(v + 24, bits32(sProj.m[2][3]));
}
// The scissor offset (RVL games set 0,0 around their passes): not modelled by Aurora yet.
GXHLE(GXSetScissorBoxOffset) {
    if (c.r[3] || c.r[4]) {
        static bool sLogged = false;
        if (!sLogged) log("[gx] GXSetScissorBoxOffset(%d, %d) ignored", s32(c.r[3]), s32(c.r[4]));
        sLogged = true;
    }
}
// Indexed matrix loads (NW4R g3d): the CP reads the matrix from the GX_POS_MTX_ARRAY /
// GX_NRM_MTX_ARRAY arrays (GXSetArray). Not blended by frame interpolation yet.
GXHLE(GXLoadPosMtxIndx) {
    if (indexedAsImmediate(0, u16(c.r[3]), c.r[4] * 4, 12)) return;
    GXLoadPosMtxIndx(u16(c.r[3]), c.r[4]);
}
GXHLE(GXLoadNrmMtxIndx3x3) {
    if (indexedAsImmediate(1, u16(c.r[3]), 0x400 + c.r[4] * 3, 9)) return;
    rawFifoWrite(GX_LOAD_INDX_B, 1);
    rawFifoWrite(u16(c.r[3]), 2);
    rawFifoWrite(((9u - 1) << 12) | (0x400u + c.r[4] * 3), 2);
}
GXHLE(GXLoadPosMtxImm) {
    const Mtx<3, 4> m(c.r[3]);
    if (const u32 id = c.r[4]; id % 3 == 0 && id / 3 < 10) {
        std::memcpy(sPosMtx[id / 3], m.m, sizeof(m.m));
        sPosMtxValid[id / 3] = true;
    }
    const u32 at = mark3D();
    GXLoadPosMtxImm(m.m, c.r[4]);
    interp::note(at, interp::Kind::PosMtx, c.r[3], c.r[4], 12);
}
GXHLE(GXLoadNrmMtxImm) {
    const Mtx<3, 4> m(c.r[3]);
    const u32 at = mark3D();
    GXLoadNrmMtxImm(m.m, c.r[4]);
    interp::note(at, interp::Kind::NrmMtx, c.r[3], c.r[4], 9);
}
GXHLE(GXLoadNrmMtxImm3x3) {
    const Mtx<3, 3> m3(c.r[3]);
    f32 m[3][4] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = m3.m[i][j];
    const u32 at = mark3D();
    GXLoadNrmMtxImm(m, c.r[4]);
    interp::note(at, interp::Kind::NrmMtx, c.r[3], c.r[4], 9);
}
GXHLE(GXLoadTexMtxImm) {
    const GXTexMtxType type = GXTexMtxType(c.r[5]);
    if (type == GX_MTX2x4) {
        const Mtx<2, 4> m(c.r[3]);
        if (const u32 id = c.r[4]; id >= GX_TEXMTX0 && id <= GX_TEXMTX9 && (id - GX_TEXMTX0) % 3 == 0)
            std::memcpy(sInd.texMtx[(id - GX_TEXMTX0) / 3], m.m, sizeof(sInd.texMtx[0]));
        const u32 at = mark3D();
        GXLoadTexMtxImm(m.m, c.r[4], type);
        interp::note(at, interp::Kind::TexMtx, c.r[3], c.r[4], sizeof(m.m) / 4);
    } else {
        const Mtx<3, 4> m(c.r[3]);
        const u32 at = mark3D();
        GXLoadTexMtxImm(m.m, c.r[4], type);
        interp::note(at, interp::Kind::TexMtx, c.r[3], c.r[4], sizeof(m.m) / 4);
    }
}
GXHLE(GXSetCurrentMtx) {
    sCurPosMtx = c.r[3];
    interp::setCurrentMtx(c.r[3]);
    GXSetCurrentMtx(c.r[3]);
}
// Records the guest viewport; returns the x/width to send (moved and narrowed like the 2D
// around it for a partial viewport set by a HUD element, see sVpFollow).
static f32 sGuestVp[6] = {0, 0, 640, 456, 0, 1};  // the game's own viewport (GXGetViewportv)
static std::pair<f32, f32> setViewport(const Ctx& c) {
    for (int i = 0; i < 6; i++) sGuestVp[i] = farg(c, 1 + i);
    sRawCheck = true;
    sVp.left = farg(c, 1);
    sVp.wd = farg(c, 3);
    sVpFollow = !viewportFull() && ws::inHudScope();
    sVpAnchor = sVpFollow ? scopeAnchor() : 0;
    sVpShift = sVpFollow ? ws::hudShift() : 0;
    // A partial main view (bordered view) follows the centered frame where the frame loop sets it
    // up (fn_800115F8(0), in a scope); the view's later passes set the same rectangle again outside
    // any scope (ghost layer, effects, portraits): those follow it too, or they were drawn into the
    // raw rectangle, stretched and spilling past the border in widescreen.
    static struct {
        bool valid = false;
        f32 x = 0, y = 0, w = 0, h = 0;
        int anchor = 0, shift = 0;
    } sMainVp;
    if (ws::inMainViewSetup()) {
        sMainVp = {sVpFollow, sVp.left, farg(c, 2), sVp.wd, farg(c, 4), sVpAnchor, sVpShift};
    } else if (!sVpFollow && sMainVp.valid && !viewportFull() && ws::active() && sVp.left == sMainVp.x &&
               farg(c, 2) == sMainVp.y && sVp.wd == sMainVp.w && farg(c, 4) == sMainVp.h) {
        sVpFollow = true;
        sVpAnchor = sMainVp.anchor;
        sVpShift = sMainVp.shift;
    }
    HUD_TRACE("viewport %.1f %.1f %.1f %.1f%s %s", farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4),
              sVpFollow ? " follow" : "", guestChain(c).c_str());
    if (!sVpFollow) return {sVp.left, sVp.wd};
    return {pillarX(sVp.left, sVpAnchor, sVpShift), sVp.wd * ws::factor()};
}
// GXSetZScaleOffset (RVL): __GXSetViewport writes the XF viewport Z as sz = (far - near) * S,
// oz = far * S + O (S = 1 + 16777215 * scale in its units, O = 16777215 * offset), i.e. the depth
// written is depth * scale + offset in normalized units. Sent as the viewport's depth range
// (clamped to 0..1, which WebGPU requires; the hardware clamps per pixel to 24 bits instead).
static f32 sZScale = 1.0f, sZOffset = 0.0f;
static int sLastVpJitter = -1;  // -1: the last viewport was GXSetViewport, else the jitter field
static f32 sLastVpX = 0, sLastVpW = 640;  // x/width as last sent
static std::pair<f32, f32> zRange(f32 n, f32 f) {
    if (sZScale == 1.0f && sZOffset == 0.0f) return {n, f};
    return {std::clamp(n * sZScale + sZOffset, 0.0f, 1.0f), std::clamp(f * sZScale + sZOffset, 0.0f, 1.0f)};
}
GXHLE(GXSetViewport) {
    sLastVpJitter = -1;
    const auto [x, w] = setViewport(c);
    sLastVpX = x;
    sLastVpW = w;
    const u32 vpAt = interp::mark();
    if (interp::tagLogOn()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "viewport %.1f %.1f %.1f %.1f (sent x %.1f w %.1f) lr %08X", farg(c, 1), farg(c, 2), farg(c, 3),
                      farg(c, 4), x, w, c.lr);
        interp::tagLogMark(buf);
    }
    const auto [zn, zf] = zRange(farg(c, 5), farg(c, 6));
    GXSetViewport(x, farg(c, 2), w, farg(c, 4), zn, zf);
    if (sVpFollow) interp::noteTextViewport(vpAt, ws::factor(), pillarX(0.0f, sVpAnchor));
    else interp::noteTextViewport(vpAt, 1.0f, 0.0f);
}
GXHLE(GXSetViewportJitter) {
    gxAudit("GXSetViewportJitter field=%u", c.r[3]);
    sLastVpJitter = int(c.r[3] & 1);
    const auto [x, w] = setViewport(c);
    sLastVpX = x;
    sLastVpW = w;
    const u32 vpAt = interp::mark();
    const auto [zn, zf] = zRange(farg(c, 5), farg(c, 6));
    GXSetViewportJitter(x, farg(c, 2), w, farg(c, 4), zn, zf, c.r[3]);
    if (sVpFollow) interp::noteTextViewport(vpAt, ws::factor(), pillarX(0.0f, sVpAnchor));
    else interp::noteTextViewport(vpAt, 1.0f, 0.0f);
}
GXHLE(GXSetScissor) {
    HUD_TRACE("scissor %u %u %u %u %s", c.r[3], c.r[4], c.r[5], c.r[6], guestChain(c).c_str());
    sScissor = {c.r[3], c.r[4], c.r[5], c.r[6]};
    if (interp::tagLogOn()) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "scissor %u %u %u %u lr %08X", c.r[3], c.r[4], c.r[5], c.r[6], c.lr);
        interp::tagLogMark(buf);
    }
    emitScissor(sVpFollow);
}
GXHLE(GXSetClipMode) { GXSetClipMode(GXClipMode(c.r[3])); }

// --- geometry state ------------------------------------------------------------------

GXHLE(GXSetCullMode) { GXSetCullMode(GXCullMode(c.r[3])); }
GXHLE(GXSetCoPlanar) {
    gxAudit("GXSetCoPlanar %u", c.r[3]); GXSetCoPlanar(GXBool(c.r[3])); }
GXHLE(GXSetLineWidth) {
    gxAudit("GXSetLineWidth %u off=%u", c.r[3], c.r[4]); GXSetLineWidth(u8(c.r[3]), GXTexOffset(c.r[4])); }
GXHLE(GXSetPointSize) {
    gxAudit("GXSetPointSize %u off=%u", c.r[3], c.r[4]); GXSetPointSize(u8(c.r[3]), GXTexOffset(c.r[4])); }
GXHLE(GXEnableTexOffsets) { GXEnableTexOffsets(GXTexCoordID(c.r[3]), GXBool(c.r[4]), GXBool(c.r[5])); }

// --- lighting ----------------------------------------------------------------------------
// Guest GXLightObj (64 bytes) is the same size as Aurora's, so Aurora fills it in place.

static GXLightObj* light(u32 p) { return reinterpret_cast<GXLightObj*>(hostp(p)); }

GXHLE(GXSetNumChans) { GXSetNumChans(u8(c.r[3])); }
GXHLE(GXSetChanCtrl) {
    GXSetChanCtrl(GXChannelID(c.r[3]), GXBool(c.r[4]), GXColorSrc(c.r[5]), GXColorSrc(c.r[6]), c.r[7],
                  GXDiffuseFn(c.r[8]), GXAttnFn(c.r[9]));
}
GXHLE(GXSetChanMatColor) {
    const u32 at = interp::mark();
    GXColor col = color(c.r[4]);
    GXSetChanMatColor(GXChannelID(c.r[3]), col);
    interp::noteColors(at);
}
GXHLE(GXSetChanAmbColor) {
    const u32 at = interp::mark();
    GXSetChanAmbColor(GXChannelID(c.r[3]), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE_CPU(GXInitLightPos) { GXInitLightPos(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE_CPU(GXInitLightDir) { GXInitLightDir(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE_CPU(GXInitSpecularDir) {
    gxAudit("GXInitSpecularDir"); GXInitSpecularDir(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE_CPU(GXInitLightColor) { GXInitLightColor(light(c.r[3]), color(c.r[4])); }
GXHLE_CPU(GXInitLightSpot) {
    gxAudit("GXInitLightSpot cutoff=%.1f fn=%u", farg(c, 1), c.r[4]); GXInitLightSpot(light(c.r[3]), farg(c, 1), GXSpotFn(c.r[4])); }
GXHLE_CPU(GXInitLightDistAttn) {
    gxAudit("GXInitLightDistAttn ref=%.1f bright=%.2f fn=%u", farg(c, 1), farg(c, 2), c.r[4]);
    GXInitLightDistAttn(light(c.r[3]), farg(c, 1), farg(c, 2), GXDistAttnFn(c.r[4]));
}
GXHLE_CPU(GXInitLightAttn) {
    gxAudit("GXInitLightAttn a=%.3f %.3f %.3f k=%.4f %.4f %.6f", farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), farg(c, 6));
    GXInitLightAttn(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), farg(c, 6));
}
}  // namespace recomp
extern "C" void GXGetLightDir(const GXLightObj* light, float* nx, float* ny, float* nz);  // Aurora (GXGet.cpp; not in its header)
namespace recomp {
GXHLE_CPU(GXInitLightAttnA) { GXInitLightAttnA(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE_CPU(GXInitLightAttnK) { GXInitLightAttnK(light(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3)); }
GXHLE_CPU(GXGetLightPos) {
    f32 x = 0, y = 0, z = 0;
    GXGetLightPos(light(c.r[3]), &x, &y, &z);
    wrf32(c.r[4], x);
    wrf32(c.r[5], y);
    wrf32(c.r[6], z);
}
GXHLE_CPU(GXGetLightDir) {
    f32 x = 0, y = 0, z = 0;
    GXGetLightDir(light(c.r[3]), &x, &y, &z);
    wrf32(c.r[4], x);
    wrf32(c.r[5], y);
    wrf32(c.r[6], z);
}
GXHLE(GXLoadLightObjImm) {
    // Position and direction (view space) are blended by frame interpolation.
    const u32 at = interp::mark();
    GXLoadLightObjImm(light(c.r[3]), GXLightID(c.r[4]));
    enh::noteGameLight(reinterpret_cast<const float*>(hostp(c.r[3])), c.r[4]);
    static const int sTrace = [] {
        const char* v = std::getenv("CF_LIGHT_TRACE");
        return v ? std::max(std::atoi(v), 1) : 0;
    }();
    if (sTrace >= 2 && copyDispCount() % 600 == 0) {  // =2: every load with position / direction every 600 frames
        const auto* lo = reinterpret_cast<const float*>(hostp(c.r[3]) + 4);
        log("[light] f%u obj %08X id %X col %08X a %.3f %.3f %.3f k %.4f %.4f %.4f pos %.1f %.1f %.1f dir %.3f %.3f %.3f lr %08X",
            copyDispCount(), c.r[3], c.r[4], rd32(c.r[3]), lo[0], lo[1], lo[2], lo[3], lo[4], lo[5], lo[6], lo[7], lo[8], lo[9],
            lo[10], lo[11], c.lr);
    }
    if (sTrace == 1) {
        const auto* lo = reinterpret_cast<const float*>(hostp(c.r[3]) + 4);  // a0 a1 a2 k0 k1 k2 px..nz
        static std::vector<u64> seen;
        const u64 key = u64(c.lr) << 32 | c.r[4] | u64(lo[1] != 0.0f) << 31;
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            log("[light] obj %08X id %X a %.3f %.3f %.3f k %.6f %.6f %.6f col %08X chain %s", c.r[3], c.r[4], lo[0], lo[1],
                lo[2], lo[3], lo[4], lo[5], rd32(c.r[3]), guestChain(c).c_str());
        }
    }
    interp::note(at, interp::Kind::Light, c.r[3], std::countr_zero(c.r[4] | 0x100u), 6, 10);
    interp::noteColors(at, c.r[3]);
}

// --- textures ------------------------------------------------------------------------------

void diagTexInit(const char* kind, u32 obj, u32 image, u32 w, u32 h, u32 fmt, u32 lr);
static void noteTexImage(u32 obj, u32 image) {
    const u32 idx = rd32(obj + 4);
    if (idx >= sTexImage.size()) sTexImage.resize(idx + 1);
    sTexImage[idx] = phys(image);
}
GXHLE_CPU(GXInitTexObj) {
    GXInitTexObj(sTex.forInit(c.r[3], kTexTag), ptr(c.r[4]), u16(c.r[5]), u16(c.r[6]), GXTexFmt(c.r[7]),
                 GXTexWrapMode(c.r[8]), GXTexWrapMode(c.r[9]), GXBool(c.r[10]));
    noteTexImage(c.r[3], c.r[4]);
    static const bool sHudTraceSet = std::getenv("CF_HUD_TRACE") != nullptr;  // texture sizes in its held lines
    if (indLogOn() || sHudTraceSet) sImageDesc[phys(c.r[4])] = (c.r[5] & 0xFFF) << 20 | (c.r[6] & 0xFFF) << 8 | (c.r[7] & 0xFF);
    diagTexInit("INIT", c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.lr);
}
GXHLE_CPU(GXInitTexObjCI) {
    GXInitTexObjCI(sTex.forInit(c.r[3], kTexTag), ptr(c.r[4]), u16(c.r[5]), u16(c.r[6]), GXCITexFmt(c.r[7]),
                   GXTexWrapMode(c.r[8]), GXTexWrapMode(c.r[9]), GXBool(c.r[10]), stackArg(c, 0));
    noteTexImage(c.r[3], c.r[4]);
    diagTexInit("INITCI", c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.lr);
}
GXHLE_CPU(GXInitTexObjWrapMode) {
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag)) GXInitTexObjWrapMode(o, GXTexWrapMode(c.r[4]), GXTexWrapMode(c.r[5]));
}
// Texture object getters (RVL games, NW4R g3d) and the TLUT binding.
GXHLE_CPU(GXGetTexObjWidth) {
    const GXTexObj* o = sTex.get(c.r[3], kTexTag);
    c.r[3] = o ? GXGetTexObjWidth(o) : 0;
}
GXHLE_CPU(GXGetTexObjHeight) {
    const GXTexObj* o = sTex.get(c.r[3], kTexTag);
    c.r[3] = o ? GXGetTexObjHeight(o) : 0;
}
GXHLE_CPU(GXGetTexObjWrapS) {
    const GXTexObj* o = sTex.get(c.r[3], kTexTag);
    c.r[3] = o ? u32(GXGetTexObjWrapS(o)) : 0;
}
GXHLE_CPU(GXGetTexObjWrapT) {
    const GXTexObj* o = sTex.get(c.r[3], kTexTag);
    c.r[3] = o ? u32(GXGetTexObjWrapT(o)) : 0;
}
GXHLE_CPU(GXGetTexObjTlut) {
    const GXTexObj* o = sTex.get(c.r[3], kTexTag);
    c.r[3] = o ? GXGetTexObjTlut(o) : 0;
}
GXHLE_CPU(GXInitTexObjTlut) {
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag)) GXInitTexObjTlut(o, c.r[4]);
}
// GXGetTexObjLODAll(obj, &minFilt, &magFilt, &minLod, &maxLod, &lodBias, &biasClamp, &edgeLod,
// &maxAniso): the values of the last GXInitTexObjLOD on that object (GXInitTexObj's defaults
// otherwise). Aurora keeps them in its own encoding, so they are remembered here.
namespace {
struct TexLod {
    u32 minF = 1, magF = 1;
    f32 minLod = 0, maxLod = 0, bias = 0;
    u32 clamp = 0, edge = 0, aniso = 0;
};
std::unordered_map<u32, TexLod> sTexLod;
}  // namespace
GXHLE_CPU(GXGetTexObjLODAll) {
    TexLod l;
    if (auto it = sTexLod.find(c.r[3]); it != sTexLod.end()) l = it->second;
    wr32(c.r[4], l.minF);  // enums are words, GXBool a byte
    wr32(c.r[5], l.magF);
    wrf32(c.r[6], l.minLod);
    wrf32(c.r[7], l.maxLod);
    wrf32(c.r[8], l.bias);
    wr8(c.r[9], l.clamp);
    wr8(c.r[10], l.edge);
    wr32(stackArg(c, 0), l.aniso);
}
GXHLE_CPU(GXInitTexObjLOD) {
    sTexLod[c.r[3]] = {c.r[4], c.r[5], farg(c, 1), farg(c, 2), farg(c, 3), c.r[6], c.r[7], c.r[8]};
    gxAudit("GXInitTexObjLOD min=%u mag=%u lod=%.2f..%.2f bias=%.2f clamp=%u edge=%u aniso=%u", c.r[4], c.r[5], farg(c, 1), farg(c, 2), farg(c, 3), c.r[6], c.r[7], c.r[8]);
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag))
        GXInitTexObjLOD(o, GXTexFilter(c.r[4]), GXTexFilter(c.r[5]), farg(c, 1), farg(c, 2), farg(c, 3),
                        GXBool(c.r[6]), GXBool(c.r[7]), GXAnisotropy(c.r[8]));
}
static bool zeroTexObj(u32 obj) {
    for (u32 i = 0; i < 0x20; i += 4)
        if (rd32(obj + i)) return false;
    return true;
}
GXHLE(GXLoadTexObj) {
    if (GXTexObj* o = sTex.get(c.r[3], kTexTag)) {
        const u32 at = interp::mark();
        GXLoadTexObj(o, GXTexMapID(c.r[4]));
        interp::texLoaded(at);
        const u32 idx = rd32(c.r[3] + 4);
        if (c.r[4] < 8) sMapImage[c.r[4]] = idx < sTexImage.size() ? sTexImage[idx] : 0;
    } else if (zeroTexObj(c.r[3])) {
        // Never initialised (zeroed .bss): the European game draws the file-select Luigi through
        // the shadow receivers' setup (fn_8005DD50) before the shadow object's constructor
        // (fn_8005D41C) has run, which the USA game calls earlier. The hardware loads the zero
        // registers: a 1x1 I4 texture at physical address 0, clamped, nearest filtering.
        static GXTexObj sZero = [] {
            GXTexObj o{};
            GXInitTexObj(&o, hostp(0x80000000), 1, 1, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
            GXInitTexObjLOD(&o, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
            return o;
        }();
        const u32 at = interp::mark();
        GXLoadTexObj(&sZero, GXTexMapID(c.r[4]));
        interp::texLoaded(at);
        if (c.r[4] < 8) sMapImage[c.r[4]] = 0;
    } else {
        static u32 sUnknown = 0;
        if (sUnknown++ < 16) log("[gx] GXLoadTexObj: unknown texture object %08X", c.r[3]);
    }
}
GXHLE_CPU(GXGetTexBufferSize) {
    c.r[3] = GXGetTexBufferSize(u16(c.r[3]), u16(c.r[4]), c.r[5], GXBool(c.r[6]), u8(c.r[7]));
}
GXHLE_CPU(GXInitTexCacheRegion) {}
GXHLE_CPU(GXInitTlutObj) {
    GXInitTlutObj(sTlut.forInit(c.r[3], kTlutTag), ptr(c.r[4]), GXTlutFmt(c.r[5]), u16(c.r[6]));
}
GXHLE(GXLoadTlut) {
    if (GXTlutObj* o = sTlut.get(c.r[3], kTlutTag)) {
        const u32 at = interp::mark();
        GXLoadTlut(o, c.r[4]);
        interp::tlutLoaded(at);
    }
}

// --- TEV -------------------------------------------------------------------------------------

GXHLE(GXSetNumTevStages) {
    sNumTevStages = u8(c.r[3]);
    GXSetNumTevStages(u8(c.r[3]));
}
// Model viewer "Textures off": texture colour reads as white and texture blend modes pass
// the lit vertex colour through, so models show lighting/vertex colours only. Alpha stays
// textured so cut-out edges survive.
static bool sUntextured = false;
void gx::setUntextured(bool on) { sUntextured = on; }
static GXTevColorArg untex(u32 a) {
    return GXTevColorArg(sUntextured && (a == GX_CC_TEXC || a == GX_CC_TEXA) ? GX_CC_ONE : a);
}

GXHLE(GXSetTevOp) {
    u32 mode = c.r[4];
    if (sUntextured && (mode == GX_MODULATE || mode == GX_DECAL || mode == GX_REPLACE)) mode = GX_PASSCLR;
    GXSetTevOp(GXTevStageID(c.r[3]), GXTevMode(mode));
}
GXHLE(GXSetTevOrder) {
    if (c.r[3] < 16) sTevMap[c.r[3]] = c.r[5] & 0x100 ? 0xFF : u8(c.r[5]), sTevCoord[c.r[3]] = u8(c.r[4]);
    GXSetTevOrder(GXTevStageID(c.r[3]), GXTexCoordID(c.r[4]), GXTexMapID(c.r[5]), GXChannelID(c.r[6]));
}
GXHLE(GXSetTevColorIn) {
    GXSetTevColorIn(GXTevStageID(c.r[3]), untex(c.r[4]), untex(c.r[5]), untex(c.r[6]), untex(c.r[7]));
}
GXHLE(GXSetTevAlphaIn) {
    GXSetTevAlphaIn(GXTevStageID(c.r[3]), GXTevAlphaArg(c.r[4]), GXTevAlphaArg(c.r[5]), GXTevAlphaArg(c.r[6]),
                    GXTevAlphaArg(c.r[7]));
}
GXHLE(GXSetTevColorOp) {
    GXSetTevColorOp(GXTevStageID(c.r[3]), GXTevOp(c.r[4]), GXTevBias(c.r[5]), GXTevScale(c.r[6]), GXBool(c.r[7]),
                    GXTevRegID(c.r[8]));
}
GXHLE(GXSetTevAlphaOp) {
    GXSetTevAlphaOp(GXTevStageID(c.r[3]), GXTevOp(c.r[4]), GXTevBias(c.r[5]), GXTevScale(c.r[6]), GXBool(c.r[7]),
                    GXTevRegID(c.r[8]));
}
GXHLE(GXSetTevColor) {
    const u32 at = interp::mark();
    GXSetTevColor(GXTevRegID(c.r[3]), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE(GXSetTevColorS10) {
    gxAudit("GXSetTevColorS10 reg=%u", c.r[3]);
    const u32 p = c.r[4];
    const u32 at = interp::mark();
    GXSetTevColorS10(GXTevRegID(c.r[3]), GXColorS10{s16(rd16(p)), s16(rd16(p + 2)), s16(rd16(p + 4)), s16(rd16(p + 6))});
    interp::noteColors(at);
}
GXHLE(GXSetTevKColor) {
    const u32 at = interp::mark();
    GXSetTevKColor(GXTevKColorID(c.r[3]), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE(GXSetTevKColorSel) { GXSetTevKColorSel(GXTevStageID(c.r[3]), GXTevKColorSel(c.r[4])); }
GXHLE(GXSetTevKAlphaSel) { GXSetTevKAlphaSel(GXTevStageID(c.r[3]), GXTevKAlphaSel(c.r[4])); }
GXHLE(GXSetTevSwapMode) { GXSetTevSwapMode(GXTevStageID(c.r[3]), GXTevSwapSel(c.r[4]), GXTevSwapSel(c.r[5])); }
GXHLE(GXSetTevSwapModeTable) {
    gxAudit("GXSetTevSwapModeTable %u %u%u%u%u", c.r[3], c.r[4], c.r[5], c.r[6], c.r[7]);
    GXSetTevSwapModeTable(GXTevSwapSel(c.r[3]), GXTevColorChan(c.r[4]), GXTevColorChan(c.r[5]),
                          GXTevColorChan(c.r[6]), GXTevColorChan(c.r[7]));
}
GXHLE(GXSetAlphaCompare) {
    GXSetAlphaCompare(GXCompare(c.r[3]), u8(c.r[4]), GXAlphaOp(c.r[5]), GXCompare(c.r[6]), u8(c.r[7]));
}
GXHLE(GXSetZTexture) { GXSetZTexture(GXZTexOp(c.r[3]), GXTexFmt(c.r[4]), c.r[5]); }

// --- indirect -----------------------------------------------------------------------------------

GXHLE(GXSetNumIndStages) {
    sInd.numInd = c.r[3] & 7;
    GXSetNumIndStages(indOff() ? 0 : u8(c.r[3]));
}
GXHLE(GXSetIndTexOrder) {
    if (c.r[3] < 4) sInd.order[c.r[3]][0] = u8(c.r[4]), sInd.order[c.r[3]][1] = u8(c.r[5]);
    GXSetIndTexOrder(GXIndTexStageID(c.r[3]), GXTexCoordID(c.r[4]), GXTexMapID(c.r[5]));
}
GXHLE(GXSetIndTexCoordScale) {
    if (c.r[3] < 4) sInd.scale[c.r[3]][0] = u8(c.r[4]), sInd.scale[c.r[3]][1] = u8(c.r[5]);
    GXSetIndTexCoordScale(GXIndTexStageID(c.r[3]), GXIndTexScale(c.r[4]), GXIndTexScale(c.r[5]));
}
GXHLE(GXSetIndTexMtx) {
    const Mtx<2, 3> m(c.r[4]);
    if (const u32 id = c.r[3] - GX_ITM_0; id < 3) {
        std::memcpy(sInd.mtx[id], m.m, sizeof(sInd.mtx[id]));
        sInd.exp[id] = s8(c.r[5]);
    }
    // CF_IND_EXP=<n> (test aid): adds n to the matrix scale exponent (2^n times the offsets).
    static const int sExpAdd = [] { const char* e = std::getenv("CF_IND_EXP"); return e ? std::atoi(e) : 0; }();
    GXSetIndTexMtx(GXIndTexMtxID(c.r[3]), m.m, s8(s8(c.r[5]) + sExpAdd));
}
GXHLE(GXSetTevDirect) {
    if (c.r[3] < 16) sInd.tev[c.r[3]] = 0;
    GXSetTevDirect(GXTevStageID(c.r[3]));
}
GXHLE(GXSetTevIndWarp) {
    // SDK: GXSetTevIndirect(stage, ind, ITF_8, signed ? ITB_STU : NONE, mtx, replace ? ITW_0 : OFF (s,t), 0, 0, 0)
    if (c.r[3] < 16)
        sInd.tev[c.r[3]] = indWord(c.r[4] & 3, 0, (c.r[5] & 0xFF) ? GX_ITB_STU : 0, c.r[7], (c.r[6] & 0xFF) ? GX_ITW_0 : 0,
                                   (c.r[6] & 0xFF) ? GX_ITW_0 : 0, 0, 0, 0);
    if (indOff()) return GXSetTevDirect(GXTevStageID(c.r[3]));
    GXSetTevIndWarp(GXTevStageID(c.r[3]), GXIndTexStageID(c.r[4]), GXBool(c.r[5]), GXBool(c.r[6]),
                    GXIndTexMtxID(c.r[7]));
}
GXHLE(GXSetTevIndirect) {
    if (c.r[3] < 16)
        sInd.tev[c.r[3]] = indWord(c.r[4] & 3, c.r[5] & 3, c.r[6] & 7, c.r[7] & 15, c.r[8] & 7, c.r[9] & 7,
                                   c.r[10] & 1, stackArg(c, 0) & 1, stackArg(c, 1) & 3);
    if (indOff()) return GXSetTevDirect(GXTevStageID(c.r[3]));
    GXSetTevIndirect(GXTevStageID(c.r[3]), GXIndTexStageID(c.r[4]), GXIndTexFormat(c.r[5]),
                     GXIndTexBiasSel(c.r[6]), GXIndTexMtxID(c.r[7]), GXIndTexWrap(c.r[8]), GXIndTexWrap(c.r[9]),
                     GXBool(c.r[10]), GXBool(stackArg(c, 0)), GXIndTexAlphaSel(stackArg(c, 1)));
}

// --- pixel engine ---------------------------------------------------------------------------------

GXHLE(GXSetColorUpdate) { GXSetColorUpdate(GXBool(c.r[3])); }
GXHLE(GXSetAlphaUpdate) { GXSetAlphaUpdate(GXBool(c.r[3])); }
GXHLE(GXSetZMode) { GXSetZMode(GXBool(c.r[3]), GXCompare(c.r[4]), GXBool(c.r[5])); }
GXHLE(GXSetPixelFmt) {
    gxAudit("GXSetPixelFmt %u z=%u", c.r[3], c.r[4]); GXSetPixelFmt(GXPixelFmt(c.r[3]), GXZFmt16(c.r[4])); }
GXHLE(GXSetDither) {
    gxAudit("GXSetDither %u", c.r[3]); GXSetDither(GXBool(c.r[3])); }
GXHLE(GXSetZCompLoc) {
    gxAudit("GXSetZCompLoc %u", c.r[3]); GXSetZCompLoc(GXBool(c.r[3])); }
GXHLE(GXSetBlendMode) {
    if (c.r[3] == GX_BM_LOGIC)  // logic ops: where the game uses them (Aurora 0026)
        gxAudit("GXSetBlendMode %u %u %u %u from %s", c.r[3], c.r[4], c.r[5], c.r[6], guestChain(c).c_str());
    else
        gxAudit("GXSetBlendMode %u %u %u %u", c.r[3], c.r[4], c.r[5], c.r[6]);
    GXSetBlendMode(GXBlendMode(c.r[3]), GXBlendFactor(c.r[4]), GXBlendFactor(c.r[5]), GXLogicOp(c.r[6]));
}
GXHLE(GXSetDstAlpha) {
    GXSetDstAlpha(GXBool(c.r[3]), u8(c.r[4]));
}
GXHLE(GXSetFog) {
    gxAudit("GXSetFog type=%u start=%.1f end=%.1f near=%.1f far=%.1f", c.r[3], farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4));
    const u32 at = interp::mark();
    GXSetFog(GXFogType(c.r[3]), farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), color(c.r[4]));
    interp::noteColors(at);
}
GXHLE(GXSetFogRangeAdj) {
    gxAudit("GXSetFogRangeAdj en=%u center=%u", c.r[3], c.r[4]);
    GXFogAdjTable t{};
    if (c.r[5])
        for (int i = 0; i < 10; i++) t.r[i] = u16(rd16(c.r[5] + 2 * i));
    GXSetFogRangeAdj(GXBool(c.r[3]), u16(c.r[4]), &t);
}
GXHLE_CPU(GXInitFogAdjTable) {
    const u32 table = c.r[3];
    const u16 width = u16(c.r[4]);
    const Mtx<4, 4> p(c.r[5]);
    f32 nearZ, sideX;
    if (p.m[3][3] == 0.0f) {
        nearZ = p.m[2][3] / (p.m[2][2] - 1.0f);
        sideX = nearZ / p.m[0][0];
    } else {
        sideX = 1.0f / p.m[0][0];
        nearZ = 1.73205f * sideX;
    }
    const f32 iw = 2.0f / width;
    for (u32 i = 0; i < 10; i++) {
        const f32 xi = f32((i + 1) << 5) * iw * sideX;
        const f32 range = std::sqrt(1.0f + (xi * xi) / (nearZ * nearZ));
        wr16(table + 2 * i, u32(range * 256.0f) & 0xFFF);
    }
}

// --- copies ---------------------------------------------------------------------------------------

// Current texture-copy setup (CF_COPY_LOG and the CF_FRAME_DUMP restore), copies by frame for the log.
static struct { u32 x, y, w, h, dw, dh, fmt, mip; } sCopyDiag;
static const bool sCopyLog = std::getenv("CF_COPY_LOG") != nullptr;
u32 copyDispCount() { return sDiagFrame; }
struct DiagCopy { u32 dest, size, fmt, w, h, frame; };
static std::vector<DiagCopy> sDiagCopies;
void diagTexInit(const char* kind, u32 obj, u32 image, u32 w, u32 h, u32 fmt, u32 lr) {
    if (!sCopyLog) return;
    const u32 im = phys(image);
    const u32 sz = GXGetTexBufferSize(u16(w), u16(h), fmt, GX_FALSE, 0);
    for (const auto& d : sDiagCopies) {
        if (im < d.dest + d.size && d.dest < im + sz) {
            log("[copydiag] f%u %s obj=%08X img=%08X %ux%u fmt=%u size=%X lr=%08X  overlaps copy %08X %ux%u fmt=%u (f%u)%s",
                sDiagFrame, kind, obj, image, w, h, fmt, sz, lr, d.dest, d.w, d.h, d.fmt, d.frame,
                (im == d.dest && w == d.w && h == d.h && fmt == d.fmt) ? " [same]" : " [DIFFERENT]");
        }
    }
}
GXHLE(GXSetCopyClear) { GXSetCopyClear(color(c.r[3]), c.r[4]); }
GXHLE(GXSetDispCopySrc) {
    gxAudit("GXSetDispCopySrc %u %u %u %u", c.r[3], c.r[4], c.r[5], c.r[6]);
    if (c.r[6]) sDisplayCopy.efbHeight = u16(c.r[6]);
    GXSetDispCopySrc(u16(c.r[3]), u16(c.r[4]), u16(c.r[5]), u16(c.r[6]));
}
GXHLE(GXSetDispCopyDst) {
    gxAudit("GXSetDispCopyDst %u %u", c.r[3], c.r[4]);
    GXSetDispCopyDst(u16(c.r[3]), u16(c.r[4]));
}
GXHLE(GXSetDispCopyYScale) {
    // Returns the number of XFB lines the display copy writes for the copy source height
    // (the SDK's formula for the 9-bit reciprocal scale register).
    gxAudit("GXSetDispCopyYScale %.4f", farg(c, 1));
    GXSetDispCopyYScale(farg(c, 1));
    const u32 iScale = u32(256.0f / farg(c, 1)) & 0x1FF;
    const u32 efbHeight = sDisplayCopy.efbHeight;
    u32 lines = iScale ? (efbHeight - 1) * 256 / iScale + 1 : efbHeight;
    if (iScale > 128 && iScale < 256) {
        u32 s = iScale;
        while ((s & 1) == 0) s >>= 1;
        if (efbHeight % s == 0) lines++;
    }
    c.r[3] = std::min<u32>(lines, 1024);
}
GXHLE(GXSetDispCopyGamma) {
    gxAudit("GXSetDispCopyGamma %u", c.r[3]);
    sDisplayCopy.gamma = u8(std::min<u32>(c.r[3], 2));
    GXSetDispCopyGamma(GXGamma(c.r[3]));
}
GXHLE(GXSetCopyClamp) {
    gxAudit("GXSetCopyClamp %u", c.r[3]);
    sDisplayCopy.clamp = u8(c.r[3] & 3);
    GXSetCopyClamp(GXFBClamp(c.r[3] & 3));  // texture copies: copy filter edge lines (patch 0018)
}
GXHLE(GXSetCopyFilter) {
    if (c.r[6]) gxAudit("GXSetCopyFilter aa=%u vf=%u vfilter=%u,%u,%u,%u,%u,%u,%u", c.r[3], c.r[5], rd8(c.r[6]), rd8(c.r[6] + 1), rd8(c.r[6] + 2), rd8(c.r[6] + 3), rd8(c.r[6] + 4), rd8(c.r[6] + 5), rd8(c.r[6] + 6));
    u8 pattern[12][2] = {};
    u8 vfilter[7] = {};
    if (c.r[4]) std::memcpy(pattern, hostp(c.r[4]), sizeof(pattern));
    if (c.r[6]) std::memcpy(vfilter, hostp(c.r[6]), sizeof(vfilter));
    // With vf off the SDK loads the pass-through taps {0, 0, 21, 22, 21, 0, 0}.
    static constexpr u8 kNoFilter[7] = {0, 0, 21, 22, 21, 0, 0};
    for (int i = 0; i < 7; i++) sDisplayCopy.vfilter[i] = c.r[5] && c.r[6] ? u8(vfilter[i] & 0x3F) : kNoFilter[i];
    GXSetCopyFilter(GXBool(c.r[3]), pattern, GXBool(c.r[5]), vfilter);
}
GXHLE(GXCopyDisp) {
    sDiagFrame++;
    rawXfReport();
    if (void* dump = dropDraw() ? nullptr : efbcopy::dumpTarget(sDiagFrame)) {
        // CF_FRAME_DUMP: copy the finished EFB through the copy-to-RAM path, then restore
        // the game's texture-copy setup. The dump is the raw EFB: no copy filter (texture
        // copies apply it, GX_AURORA_ACCURACY_COPY_FILTER), then the game's taps again.
        u8 pattern[12][2] = {};
        u8 taps[7];
        std::memcpy(taps, sDisplayCopy.vfilter, sizeof(taps));
        GXSetCopyFilter(GX_FALSE, pattern, GX_FALSE, nullptr);
        GXSetTexCopySrc(0, 0, 640, 480);
        GXSetTexCopyDst(640, 480, GX_TF_RGBA8, GX_FALSE);
        const u32 at = interp::mark();
        GXCopyTex(dump, GX_FALSE);
        interp::noteDump(at, dump, sDiagFrame);
        GXSetCopyFilter(GX_FALSE, pattern, GX_TRUE, taps);
        GXSetTexCopySrc(u16(sCopyDiag.x), u16(sCopyDiag.y), u16(sCopyDiag.w), u16(sCopyDiag.h));
        GXSetTexCopyDst(u16(sCopyDiag.dw), u16(sCopyDiag.dh), GXTexFmt(sCopyDiag.fmt), GXBool(sCopyDiag.mip));
    }
    if (!dropDraw()) GXCopyDisp(hostp(c.r[3]), GXBool(c.r[4]));
    if (frame::presentOnCopy()) frame::present(c, c.r[3], frame::viBlack());  // PAL release (frame.h)
}
GXHLE(GXSetTexCopySrc) {
    sCopyDiag.x = c.r[3]; sCopyDiag.y = c.r[4]; sCopyDiag.w = c.r[5]; sCopyDiag.h = c.r[6];
    traceNote("copy src (%u,%u %ux%u)", c.r[3], c.r[4], c.r[5], c.r[6]);
    GXSetTexCopySrc(u16(c.r[3]), u16(c.r[4]), u16(c.r[5]), u16(c.r[6]));
}
GXHLE(GXSetTexCopyDst) {
    sCopyDiag.dw = c.r[3]; sCopyDiag.dh = c.r[4]; sCopyDiag.fmt = c.r[5]; sCopyDiag.mip = c.r[6];
    GXSetTexCopyDst(u16(c.r[3]), u16(c.r[4]), GXTexFmt(c.r[5]), GXBool(c.r[6]));
}
GXHLE(GXCopyTex) {
    HUD_TRACE("copytex src(%u,%u %ux%u) dst %ux%u fmt %u -> %08X %s", sCopyDiag.x, sCopyDiag.y, sCopyDiag.w, sCopyDiag.h,
              sCopyDiag.dw, sCopyDiag.dh, sCopyDiag.fmt, c.r[3], guestChain(c).c_str());
    if (sCopyLog) {
        const u32 sz = GXGetTexBufferSize(u16(sCopyDiag.dw), u16(sCopyDiag.dh), sCopyDiag.fmt, GX_FALSE, 0);
        log("[copydiag] f%u COPY dest=%08X src=(%u,%u %ux%u) dst=%ux%u fmt=%u mip=%u clear=%u size=%X lr=%08X%s",
            sDiagFrame, c.r[3], sCopyDiag.x, sCopyDiag.y, sCopyDiag.w, sCopyDiag.h, sCopyDiag.dw, sCopyDiag.dh,
            sCopyDiag.fmt, sCopyDiag.mip, c.r[4], sz, c.lr, dropDraw() ? " DROPPED" : "");
        bool found = false;
        for (auto& d : sDiagCopies)
            if (d.dest == phys(c.r[3])) { d = {phys(c.r[3]), sz, sCopyDiag.fmt, sCopyDiag.dw, sCopyDiag.dh, sDiagFrame}; found = true; }
        if (!found) sDiagCopies.push_back({phys(c.r[3]), sz, sCopyDiag.fmt, sCopyDiag.dw, sCopyDiag.dh, sDiagFrame});
    }
    if (dropDraw()) return;
    if (!isCopyDest(phys(c.r[3]))) {
        if (sCopyDests.size() >= 64) sCopyDests.erase(sCopyDests.begin());
        sCopyDests.push_back(phys(c.r[3]));
    }
    interp::efbCopied(c.r[3], GXGetTexBufferSize(u16(sCopyDiag.dw), u16(sCopyDiag.dh), sCopyDiag.fmt, GX_FALSE, 0));
    efbcopy::guestCopy(c.r[3], sCopyDiag.dw, sCopyDiag.dh, sCopyDiag.fmt);
    GXCopyTex(hostp(c.r[3]), GXBool(c.r[4]));
}

// --- RVL SDK additions and helpers the GameCube games did not use ------------------
// The display copy's vertical scale (GXFrameBuf.c semantics: the scale register holds
// 256 / yScale as a 9-bit integer; the XFB gets (efbHeight - 1) * 256 / reg + 1 lines).
namespace {
u32 xfbLines(u32 efbHeight, f32 yScale) {
    const u32 reg = u32(256.0f / yScale) & 0x1FF;
    if (!reg) return efbHeight;
    return (efbHeight - 1) * 256 / reg + 1;
}
}  // namespace
HLE(GXGetNumXfbLines) { c.r[3] = xfbLines(u16(c.r[3]), farg(c, 1)); }
HLE(GXGetYScaleFactor) {
    const u32 efb = u16(c.r[3]), xfb = u16(c.r[4]);
    f32 s = efb ? f32(xfb) / f32(efb) : 1.0f;
    // The largest scale (smallest register) whose line count reaches xfb lines, as the SDK finds.
    for (u32 reg = u32(256.0f / s) & 0x1FF; reg > 1; reg--) {
        const f32 t = 256.0f / f32(reg);
        if (xfbLines(efb, t) >= xfb) {
            s = t;
            break;
        }
    }
    c.setSingle(1, s);
}
GXHLE(GXSetTexCoordScaleManually) {
    GXSetTexCoordScaleManually(GXTexCoordID(c.r[3]), GXBool(c.r[4]), u16(c.r[5]), u16(c.r[6]));
}
GXHLE(GXSetTexCoordCylWrap) { GXSetTexCoordCylWrap(GXTexCoordID(c.r[3]), GXBool(c.r[4]), GXBool(c.r[5])); }
// Texture/TLUT memory region callbacks: Aurora has no TMEM regions to allocate; the previous
// callback (none) is returned.
GXHLE(GXSetTexRegionCallback) { c.r[3] = 0; }
GXHLE(GXSetTlutRegionCallback) { c.r[3] = 0; }
// Texture object getters (RVL SDK additions; GXGetTexObjAll: NW4R lyt / the game's own code).
GXHLE_CPU(GXGetTexObjAll) {
    // (obj, void** image, u16* width, u16* height, GXTexFmt* fmt, wrapS*, wrapT*, GXBool* mipmap)
    const GXTexObj* o = sTex.get(c.r[3], kTexTag);
    const u32 idx = rd32(c.r[3] + 4);
    const u32 img = o && idx < sTexImage.size() ? sTexImage[idx] : 0;
    if (c.r[4]) wr32(c.r[4], img ? guestAddrOfOffset(img) : 0);
    if (c.r[5]) wr16(c.r[5], o ? GXGetTexObjWidth(o) : 0);
    if (c.r[6]) wr16(c.r[6], o ? GXGetTexObjHeight(o) : 0);
    if (c.r[7]) wr32(c.r[7], o ? u32(GXGetTexObjFmt(o)) : 0);
    if (c.r[8]) wr32(c.r[8], o ? u32(GXGetTexObjWrapS(o)) : 0);
    if (c.r[9]) wr32(c.r[9], o ? u32(GXGetTexObjWrapT(o)) : 0);
    if (c.r[10]) wr8(c.r[10], o ? u32(GXGetTexObjMipMap(o)) : 0);
}
GXHLE_CPU(GXGetTexObjFmt) {
    const GXTexObj* o = sTex.get(c.r[3], kTexTag);
    c.r[3] = o ? u32(GXGetTexObjFmt(o)) : 0;
}
GXHLE_CPU(GXGetTexObjMipMap) {
    const GXTexObj* o = sTex.get(c.r[3], kTexTag);
    c.r[3] = o ? u32(GXGetTexObjMipMap(o)) : 0;
}
// User data: kept per object index (the guest object holds only Aurora's tag and index).
static std::unordered_map<u32, u32> sTexUserData;
GXHLE_CPU(GXInitTexObjUserData) {
    if (sTex.get(c.r[3], kTexTag)) sTexUserData[rd32(c.r[3] + 4)] = c.r[4];
}
GXHLE_CPU(GXGetTexObjUserData) {
    const auto it = sTex.get(c.r[3], kTexTag) ? sTexUserData.find(rd32(c.r[3] + 4)) : sTexUserData.end();
    c.r[3] = it != sTexUserData.end() ? it->second : 0;
}
// TMEM is Aurora's business: a preloaded texture loads like any other.
GXHLE(GXLoadTexObjPreLoaded) {
    c.r[4] = c.r[5];  // (obj, region, id)
    hle_GXLoadTexObj(c);
}
GXHLE_CPU(GXInitTlutRegion) {}
// Viewport / scissor / vertex format getters: the game's own values (not the widescreen-adjusted ones).
GXHLE_CPU(GXGetViewportv) {
    for (int i = 0; i < 6; i++) wrf32(c.r[3] + 4 * i, sGuestVp[i]);
}
GXHLE_CPU(GXGetScissor) {
    wr32(c.r[3], sScissor.x);
    wr32(c.r[4], sScissor.y);
    wr32(c.r[5], sScissor.w);
    wr32(c.r[6], sScissor.h);
}
GXHLE_CPU(GXGetVtxDescv) {
    // GXVtxDescList[]: {attr, type} words, GX_VA_NULL-terminated; the SDK lists POS_MTX_IDX..TEX7.
    u32 p = c.r[3];
    for (u32 a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
        const int slot = attrSlot(a);
        wr32(p, a);
        wr32(p + 4, slot >= 0 ? sDesc[slot] : 0);
        p += 8;
    }
    wr32(p, GX_VA_NULL);
    wr32(p + 4, 0);
}
GXHLE_CPU(GXGetVtxAttrFmt) {
    // (fmt, attr, GXCompCnt*, GXCompType*, u8* frac)
    const int slot = attrSlot(c.r[4]);
    const VatEntry v = slot >= 0 && c.r[3] < GX_MAX_VTXFMT ? sVat[c.r[3]][slot] : VatEntry{};
    wr32(c.r[5], v.cnt);
    wr32(c.r[6], v.type);
    wr8(c.r[7], v.frac);
}
GXHLE_CPU(GXGetVtxAttrFmtv) {
    // GXVtxAttrFmtList[]: {attr, cnt, type, frac (u8 + pad)}, POS..TEX7, then GX_VA_NULL.
    u32 p = c.r[4];
    for (u32 a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        const int slot = attrSlot(a);
        const VatEntry v = slot >= 0 && c.r[3] < GX_MAX_VTXFMT ? sVat[c.r[3]][slot] : VatEntry{};
        wr32(p, a);
        wr32(p + 4, v.cnt);
        wr32(p + 8, v.type);
        wr32(p + 12, u32(v.frac) << 24);
        p += 16;
    }
    wr32(p, GX_VA_NULL);
}
// Callbacks and GPU/FIFO management the HLE'd GX has no use for: the previous callback (none) is
// returned, the FIFO is Aurora's, the GPU always idle.
GXHLE_CPU(GXSetDrawDoneCallback) {
    c.r[3] = std::exchange(sDrawDoneCb, c.r[3]);
}
GXHLE_CPU(GXSetDrawSyncCallback) { c.r[3] = 0; }
GXHLE_CPU(GXSetBreakPtCallback) { c.r[3] = 0; }
static u32 sGxThread = 0;
GXHLE_CPU(GXSetCurrentGXThread) { c.r[3] = std::exchange(sGxThread, sched::currentGuest()); }
GXHLE_CPU(GXGetCurrentGXThread) { c.r[3] = sGxThread; }
GXHLE_CPU(GXGetGPStatus) {
    // (GXBool* overhi, underlow, readIdle, cmdIdle, brkpt)
    wr8(c.r[3], 0);
    wr8(c.r[4], 1);
    wr8(c.r[5], 1);
    wr8(c.r[6], 1);
    wr8(c.r[7], 0);
}
GXHLE_CPU(GXGetFifoCount) { c.r[3] = 0; }
GXHLE_CPU(GXInitFifoBase) {}
GXHLE_CPU(GXGetCPUFifo) {}
GXHLE(GXSetCPUFifo) {}
GXHLE(GXSetGPFifo) {}
GXHLE_CPU(GXGetFifoWrap) { c.r[3] = 0; }
GXHLE(GXAbortFrame) {}
GXHLE(GXClearBoundingBox) {}
GXHLE(GXSetGPMetric) {}
GXHLE(GXClearGPMetric) {}
GXHLE(GXSetFieldMask) {}
GXHLE(GXSetFieldMode) {}
GXHLE(GXSetDispCopyFrame2Field) {}
GXHLE(GXResetWriteGatherPipe) {}
// GXSetZScaleOffset (RVL): see zRange (with the viewport HLE).
GXHLE(GXSetZScaleOffset) {
    if (farg(c, 1) == sZScale && farg(c, 2) == sZOffset) return;
    if (farg(c, 1) != 1.0f || farg(c, 2) != 0.0f) {
        static int sLogged = 0;
        if (sLogged++ < 4) log("[gx] GXSetZScaleOffset(%f, %f) (lr %08X)", farg(c, 1), farg(c, 2), c.lr);
    }
    sZScale = farg(c, 1);
    sZOffset = farg(c, 2);
    // The SDK rewrites the viewport with the new Z terms at once.
    const auto [zn, zf] = zRange(sGuestVp[4], sGuestVp[5]);
    if (sLastVpJitter < 0) GXSetViewport(sLastVpX, sGuestVp[1], sLastVpW, sGuestVp[3], zn, zf);
    else GXSetViewportJitter(sLastVpX, sGuestVp[1], sLastVpW, sGuestVp[3], zn, zf, u32(sLastVpJitter));
}
// EFB pokes (CPU writes to the EFB): their pixel-engine state; nothing pokes through it here.
GXHLE_CPU(GXPokeAlphaMode) {}
GXHLE_CPU(GXPokeAlphaRead) {}
GXHLE_CPU(GXPokeAlphaUpdate) {}
GXHLE_CPU(GXPokeBlendMode) {}
GXHLE_CPU(GXPokeColorUpdate) {}
GXHLE_CPU(GXPokeDither) {}
GXHLE_CPU(GXPokeDstAlpha) {}
GXHLE_CPU(GXPokeZMode) {}

// GXDraw.c sphere (EGG::DrawGX records its sphere display lists with it at start-up).
GXHLE(GXDrawSphere) {
    if (dropDraw()) return;
    prepareDraw(drawMode(ProjMode::Scaled));
    GXDrawSphere(u8(c.r[3]), u8(c.r[4]));
}

// For the [mem] report (mem_report.cpp): host texture / TLUT object table sizes.
void gxObjectTables(u32& tex, u32& texFree, u32& tlut, u32& tlutFree) {
    tex = sTex.statSize.load(std::memory_order_relaxed);
    texFree = sTex.statFree.load(std::memory_order_relaxed);
    tlut = sTlut.statSize.load(std::memory_order_relaxed);
    tlutFree = sTlut.statFree.load(std::memory_order_relaxed);
}

}  // namespace recomp
