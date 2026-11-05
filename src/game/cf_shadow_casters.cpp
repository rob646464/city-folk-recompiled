// Shadows of objects outside the view (branch shadow-casters; docs/RENDERING.md "Shadows of objects
// outside the view"). With Enhanced lighting's sun / moon shadows, a tree, house or rock that left
// the picture was culled by the game, so its shadow vanished with it (very visible with the long
// morning / evening shadows). The game's own culling decisions are widened toward the light by
// about how far the shadows reach, and an object kept only by the widened test is drawn as a shadow
// caster only (Aurora 0470 AURORA_DRAW_CLASS_SHADOW: into the shadow maps, never into the picture,
// the pre-pass, AO, glow, world effects or EFB copies).
//
// The culling decisions (USA rev 1; every version by name / hash):
// - Field objects (trees, bushes, rocks, fences, signs...): d_fg_drawNP fn_112_2184 tests each
//   object's bounding sphere (record + 0x14 position through fn_800829D4 onto the rolling log,
//   radius record + 0x34) with fn_802B777C, a sphere-in-frustum test against the camera object's
//   frustum (camera + 0x254: view matrix rows at +0x00..0x2C, near / far view z at +0x70 / +0x74,
//   side planes (x, z) at +0x30/0x38 and +0x40/0x48, (y, z) at +0x54/0x58 and +0x64/0x68, a sphere
//   is out when a plane distance exceeds its radius), then calls the kind's draw.
// - Structures (houses, shops, town hall, museum, bulletin board, bus stop...: daSTR_c actors):
//   their draw d_a_strNP fn_99_2B04 skips the model when fn_99_2548 says so: either a window in the
//   town's flat coordinates (fn_80166CDC: |x - ref.x| <= w, b <= z - ref.z <= a around a reference
//   point of the camera, ref = the global the function loads, +x east, +z south) or a line behind
//   the camera (fn_8018ABC8: z - k r beyond camera + 0x368). A drawn structure inserts its models
//   into the scene (ScnGroup::Insert, every frame).
// - g3d's own culling: nw4r::g3d::ScnObjGather::Add culls objects with culling enabled (ScnObj
//   option 9) by their world AABB (ScnObj + 0xB4) against the gather frustum (math::FRUSTUM: six
//   world planes (n, d) at +0x90, out when n.p + d > 0 at the box's innermost corner); Add returns 2
//   for a culled object. No town object uses it (all 0 in the town); kept for other places.
// Each is re-tested with the object swept along the light's travel direction by the length of its
// shadow (height / sin(elevation) in 3D, height / tan(elevation) on the flat map; at most the shadow
// distance) and limited to the shadow distance: what passes only that way is a shadow caster for
// this frame (field objects: the class around their draw; structures and g3d objects: their models
// are marked, cf_tags.cpp draws them as casters). Only while sun / moon shadows are cast outdoors
// (cf_light submitSky); otherwise nothing here changes what the game does.
// Test aids: CF_CASTERS=0 (off, A/B), CF_CASTERS_LOG=<n> (counts every n frames, names of the
// structures / g3d objects kept), CF_CASTERS_SHOW=1 (kept objects drawn normally too: they appear in
// the picture), CF_CASTERS_SCALE=<f> (sweep length factor, default 1), CF_CASTERS_AB=<n> (on in every
// other block of n frames: CF_SHOT frames n apart show about the same moment without / with; 1 = odd
// frames), CF_CASTERS_NOSTR=1 (field objects only).
#include "game/cf_light.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <aurora/enhanced.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp

namespace {
struct Sun {
    float w[3] = {0.0f, 1.0f, 0.0f};  // toward the light, g3d world
    float l[3] = {0.0f, 1.0f, 0.0f};  // the same in the ground's frame (east, up, south)
    float sinEl = 1.0f;
    float reach = 0.0f;  // shadow distance (view depth)
    u32 frame = 0;
    bool set = false;
};
Sun sSun;

const bool sOff = [] {
    const char* v = std::getenv("CF_CASTERS");
    return v && *v == '0';
}();
const u32 sLog = [] {
    const char* v = std::getenv("CF_CASTERS_LOG");
    return v ? u32(std::max(1, std::atoi(v))) : 0u;
}();
const bool sShow = std::getenv("CF_CASTERS_SHOW") != nullptr;
const u32 sAb = [] {  // CF_CASTERS_AB=<n>: on and off in blocks of n frames (1 = odd frames on)
    const char* v = std::getenv("CF_CASTERS_AB");
    return v ? u32(std::max(1, std::atoi(v))) : 0u;
}();
const float sScale = [] {
    const char* v = std::getenv("CF_CASTERS_SCALE");
    return v ? std::clamp(float(std::atof(v)), 0.0f, 10.0f) : 1.0f;
}();

// Counts for the log (game thread), per game frame.
struct Stats {
    u32 fgCulled = 0, fgKept = 0, strCulled = 0, strKept = 0, g3dCulled = 0, g3dKept = 0, drawn = 0;
};
Stats sStats;

bool logNow() { return sLog && copyDispCount() % sLog == 0; }

// Structures and g3d objects are decided while the game builds the scene (Insert / gather) and
// drawn a copy later (sLag, measured): CF_CASTERS_AB decides them for the frame they are drawn in.
u32 sLag = 0;
bool on() { return !sOff && sSun.set && enh::masterOn() && copyDispCount() - sSun.frame <= 2u && sSun.reach > 0.0f; }
bool active(u32 lag = 0) {
    if (sAb && (((copyDispCount() + lag) / sAb) & 1) == 0) return false;
    return on();
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// 3D sweep for an object `height` tall: where the light's travel from its top meets the ground.
float sweepLength(float height) { return std::min(sScale * height / sSun.sinEl, sSun.reach); }

// The game's sphere-in-frustum test (fn_802B777C) with the sphere swept by the shadow length.
bool keptSphere(u32 fr, u32 sp) {
    const float c[3] = {rdf32(sp), rdf32(sp + 4), rdf32(sp + 8)}, r = rdf32(sp + 12);
    if (!(r > 0.0f) || !std::isfinite(c[0] + c[1] + c[2])) return false;
    float m[12];
    for (int k = 0; k < 12; k++) m[k] = rdf32(fr + 4 * k);
    const float s = sweepLength(2.0f * r);
    const float t[3] = {-sSun.w[0] * s, -sSun.w[1] * s, -sSun.w[2] * s};
    float v0[3], v1[3];
    for (int i = 0; i < 3; i++) {
        v0[i] = m[i * 4] * c[0] + m[i * 4 + 1] * c[1] + m[i * 4 + 2] * c[2] + m[i * 4 + 3];
        v1[i] = v0[i] + m[i * 4] * t[0] + m[i * 4 + 1] * t[1] + m[i * 4 + 2] * t[2];
    }
    const float zNear = rdf32(fr + 0x70), zFar = rdf32(fr + 0x74);
    if (std::min(v0[2], v1[2]) - r > zNear) return false;       // behind the near plane (view z < 0 ahead)
    if (std::max(v0[2], v1[2]) + r < zFar) return false;        // beyond the far plane
    if (-std::max(v0[2], v1[2]) - r > sSun.reach) return false;  // beyond the shadow distance
    const auto out = [&](u32 a, u32 b, int axis) {
        const float pa = rdf32(fr + a), pb = rdf32(fr + b);
        return std::min(v0[axis] * pa + v0[2] * pb, v1[axis] * pa + v1[2] * pb) > r;
    };
    return !(out(0x30, 0x38, 0) || out(0x40, 0x48, 0) || out(0x54, 0x58, 1) || out(0x64, 0x68, 1));
}

// The g3d gather frustum's AABB test (IntersectAABB_Ex) with the box swept by the shadow length.
bool keptAabb(u32 fr, u32 obj) {
    float mn[3], mx[3];
    for (int i = 0; i < 3; i++) mn[i] = rdf32(obj + 0xB4 + 4 * i), mx[i] = rdf32(obj + 0xC0 + 4 * i);
    const float e[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
    if (!(e[0] >= 0.0f && e[1] >= 0.0f && e[2] >= 0.0f) || !std::isfinite(e[0] + e[1] + e[2])) return false;
    const float diag = std::sqrt(dot3(e, e));
    if (!(diag > 0.0f) || diag > 20000.0f) return false;
    const float s = sweepLength(diag);
    const float t[3] = {-sSun.w[0] * s, -sSun.w[1] * s, -sSun.w[2] * s};
    for (int p = 0; p < 6; p++) {
        const u32 a = fr + 0x90 + 16 * u32(p);
        const float n[3] = {rdf32(a), rdf32(a + 4), rdf32(a + 8)}, d = rdf32(a + 12);
        float f = d + std::min(0.0f, dot3(n, t));
        for (int i = 0; i < 3; i++) f += n[i] * (n[i] >= 0.0f ? mn[i] : mx[i]);
        if (f > 0.0f) return false;
    }
    float view[12];
    if (cflight::mainView(view)) {
        const float ctr[3] = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
        const float d0 = -(view[8] * ctr[0] + view[9] * ctr[1] + view[10] * ctr[2] + view[11]);
        const float d1 = d0 - (view[8] * t[0] + view[9] * t[1] + view[10] * t[2]);
        if (std::min(d0, d1) - diag * 0.5f > sSun.reach) return false;
    }
    return true;
}

// The shadow of a structure on the flat map: its position moves by `o` (x east, z south) from the
// foot to the tip of the shadow of a point `height` up.
constexpr float kStrHeight = 160.0f;  // tallest common structures (houses ~110-150, shops, town hall)
void flatShadow(float height, float o[2]) {
    const float up = std::max(sSun.l[1], 0.2079f);
    float ox = -sSun.l[0] / up * height * sScale, oz = -sSun.l[2] / up * height * sScale;
    const float len = std::sqrt(ox * ox + oz * oz);
    if (len > sSun.reach && len > 0.0f) ox *= sSun.reach / len, oz *= sSun.reach / len;
    o[0] = ox, o[1] = oz;
}

// Parameter interval [t0, t1] of v0 + t * dv inside [lo, hi].
bool clipSlab(float v0, float dv, float lo, float hi, float& t0, float& t1) {
    if (std::fabs(dv) < 1e-6f) return v0 >= lo && v0 <= hi;
    float a = (lo - v0) / dv, b = (hi - v0) / dv;
    if (a > b) std::swap(a, b);
    t0 = std::max(t0, a), t1 = std::min(t1, b);
    return t0 <= t1;
}

bool sInFg = false, sFgShadow = false;
// Structures: inside d_a_strNP fn_99_2B04 (its cull test fn_99_2548, then its draw); kept = the
// test passed only with the shadow.
bool sInStr = false, sInStrCull = false, sStrWidened = false, sStrKept = false;
u32 sStrActor = 0;
// g3d scene objects drawn as shadow casters: each object's latest decision with the copy count it
// was made at, one map per decision (structures: their Insert; g3d culling: the gather).
std::unordered_map<u32, u32> sMarksStr, sMarksG3d;
void markObj(std::unordered_map<u32, u32>& m, u32 obj, bool kept) {
    if (kept) m[obj] = copyDispCount();
    else if (!m.empty()) m.erase(obj);
    if (m.size() > 4096) m.clear();
}
// The copy count of a kept object's latest decision, or ~0u.
u32 markAge(u32 obj) {
    u32 best = ~0u;
    for (const auto* m : {&sMarksStr, &sMarksG3d}) {
        if (m->empty()) continue;
        if (const auto it = m->find(obj); it != m->end()) best = std::min(best, copyDispCount() - it->second);
    }
    return best;
}

u32 sFrustumOff = 0;  // r13 offset of g3d's gather frustum pointer (decoded from Add)
bool sFrustumOffRead = false;
u32 gatherFrustum(Ctx& c) {
    if (!sFrustumOffRead) {
        sFrustumOffRead = true;
        // ScnObjGather::Add + 0x14: lwz r0, <frustum>(r13) (USA -0x27F0); decoded so every version
        // finds its own small-data offset.
        const u32 fn = guestSymbol("Add__Q34nw4r3g3d12ScnObjGatherFPQ34nw4r3g3d6ScnObjbb");
        const u32 ins = fn ? rd32(fn + 0x14) : 0;
        if ((ins >> 26) == 32 && ((ins >> 16) & 31) == 13) sFrustumOff = ins & 0xFFFF;
        log("[casters] g3d gather frustum at r13%+d (instruction %08X)", s32(s16(u16(sFrustumOff))), ins);
    }
    if (!sFrustumOff) return 0;
    return rd32(c.r[13] + u32(s32(s16(u16(sFrustumOff)))));
}

// fn_80166CDC's reference point: decoded from its first instructions (USA lis r5, 0x8062 / addi
// r5, r5, 0x426C), so every version finds its own.
u32 windowRef() {
    static const u32 sRef = [] {
        const u32 fn = guestSymbol("fn_80166CDC");
        if (!fn) return 0u;
        const u32 i0 = rd32(fn), i3 = rd32(fn + 0xC);
        if ((i0 >> 26) != 15 || (i3 >> 26) != 14) return 0u;
        const u32 ref = ((i0 & 0xFFFF) << 16) + u32(s32(s16(u16(i3 & 0xFFFF))));
        log("[casters] structure window reference at %08X", ref);
        return ref;
    }();
    return sRef;
}
}  // namespace

namespace cflight {
void castersSetSun(const float dirWorld[3], const float dirLocal[3], float sinEl, float reach, u32 frame) {
    for (int k = 0; k < 3; k++) sSun.w[k] = dirWorld[k], sSun.l[k] = dirLocal[k];
    sSun.sinEl = sinEl;
    sSun.reach = reach;
    sSun.frame = frame;
    sSun.set = true;
    if (logNow())
        log("[casters] f%u active %d | last frame: field objects culled %u kept %u, structures culled %u kept %u, g3d culled %u kept %u, "
            "models drawn as casters %u, Aurora caster-only draws %u | sun %.2f %.2f %.2f sinEl %.2f reach %.0f",
            frame, active() ? 1 : 0, sStats.fgCulled, sStats.fgKept, sStats.strCulled, sStats.strKept, sStats.g3dCulled, sStats.g3dKept,
            sStats.drawn, aurora_enhanced_shadow_class_draws(), sSun.l[0], sSun.l[1], sSun.l[2], sinEl, reach);
    sStats = {};
}
}  // namespace cflight

namespace cfcast {
void shadowOnlyBegin() {
    if (sShow) return;
    gx::endPendingDraw();  // (the game's last immediate-mode draw is closed lazily: not inside it)
    enh::setShadowOnly(true);
}
void shadowOnlyEnd() {
    if (sShow) return;
    gx::endPendingDraw();
    enh::setShadowOnly(false);
}
void fgBegin() { sInFg = true; }
void fgEnd() {
    sInFg = false;
    if (sFgShadow) {
        sFgShadow = false;
        shadowOnlyEnd();
    }
}
int g3dDrawMode(u32 scnObj, u32 task) {
    if (sMarksStr.empty() && sMarksG3d.empty()) return 0;
    const u32 t = task & 0xFFFFu;
    if (t != 6 && t != 7) return 0;
    const u32 age = markAge(scnObj);
    if (age > 2u) return 0;
    // Opaque draws in the main view only; the translucent ones (blob shadows, fades) and the second
    // root pass (another camera) are not drawn, as the game culled them.
    if (logNow())
        log("[casters] f%u model %08X %s task %X age %u scene %d pass2 %d -> %s", copyDispCount(), scnObj, cflight::modelName(scnObj).c_str(),
            task, age, enh::inScene() ? 1 : 0, enh::casterPass() ? 1 : 0,
            t == 6 && enh::inScene() && !enh::casterPass() && on() ? "caster" : "skipped");
    if (t == 6 && enh::inScene() && !enh::casterPass() && on()) {
        sLag = age;
        sStats.drawn++;
        return 1;
    }
    return 2;
}
}  // namespace cfcast

// nw4r::math::FRUSTUM-like sphere test of the game's camera (field objects; see the top). Only the
// calls from d_fg_drawNP fn_112_2184 in the main view are widened.
HLE(fn_802B777C) {
    const u32 fr = c.r[3], sp = c.r[4];
    callOriginal(c, "fn_802B777C");
    if (!sInFg || c.r[3] != 0 || sFgShadow) return;
    if (!active() || !enh::inScene() || enh::casterPass()) return;
    sStats.fgCulled++;
    if (!keptSphere(fr, sp)) return;
    sStats.fgKept++;
    c.r[3] = 1;
    sFgShadow = true;
    cfcast::shadowOnlyBegin();
}

// ---- structures ----
// daSTR_c's draw: cull test (fn_99_2548), then the model draw that inserts its models.
static void strDraw(Ctx& c) {
    static const char* const kName = "d_a_strNP::fn_99_2B04";
    if (!active(sLag)) return callOriginal(c, kName);
    sInStr = true, sStrKept = false, sStrActor = c.r[3];
    callOriginal(c, kName);
    sInStr = false, sStrKept = false;
}
static HleReg sStrDrawReg("d_a_strNP::fn_99_2B04", &strDraw);

// Its cull test: nonzero = not drawn. The window / line tests inside are widened while it runs.
static void strCull(Ctx& c) {
    static const char* const kName = "d_a_strNP::fn_99_2548";
    if (!sInStr) return callOriginal(c, kName);
    static const bool sNoStr = std::getenv("CF_CASTERS_NOSTR") != nullptr;  // debug
    if (sNoStr) return callOriginal(c, kName);
    sInStrCull = true, sStrWidened = false;
    callOriginal(c, kName);
    sInStrCull = false;
    if (sStrWidened) {
        sStats.strCulled++;
        if (c.r[3] == 0) {
            sStrKept = true;  // drawn only because of its shadow: its models are casters
            sStats.strKept++;
        }
    }
}
static HleReg sStrCullReg("d_a_strNP::fn_99_2548", &strCull);

// The flat window (r3 = {a, b, w}, r4 = position; 1 = inside).
HLE(fn_80166CDC) {
    const u32 box = c.r[3], pos = c.r[4];
    callOriginal(c, "fn_80166CDC");
    if (!sInStrCull || c.r[3] != 0) return;
    const u32 ref = windowRef();
    if (!ref) return;
    sStrWidened = true;
    float o[2];
    flatShadow(kStrHeight, o);
    const float dx = rdf32(pos) - rdf32(ref), dz = rdf32(pos + 8) - rdf32(ref + 8);
    const float a = rdf32(box), b = rdf32(box + 4), w = rdf32(box + 8);
    float t0 = 0.0f, t1 = 1.0f;
    if (clipSlab(dx, o[0], -w, w, t0, t1) && clipSlab(dz, o[1], b, a, t0, t1)) c.r[3] = 1;
}

// The line behind the camera (r3 = camera, r4 = position, f1 = size; nonzero = culled).
HLE(fn_8018ABC8) {
    const u32 pos = c.r[4];
    if (!sInStrCull) return callOriginal(c, "fn_8018ABC8");
    const double f1 = c.f[1];
    const u32 r3 = c.r[3];
    callOriginal(c, "fn_8018ABC8");
    if (c.r[3] == 0) return;
    sStrWidened = true;
    float o[2];
    flatShadow(kStrHeight, o);
    if (!(o[1] < 0.0f)) return;  // the shadow falls south (behind): nothing reaches the view
    // Tested again with the shadow's tip (the culled side is +z; the tip is further north).
    const float z = rdf32(pos + 8);
    wrf32(pos + 8, z + o[1]);
    c.r[3] = r3, c.r[4] = pos, c.f[1] = f1;
    callOriginal(c, "fn_8018ABC8");
    wrf32(pos + 8, z);
}

// A structure drawn for its shadow inserts its models (every frame): they are casters this frame.
HLE(Insert__Q34nw4r3g3d8ScnGroupFUlPQ34nw4r3g3d6ScnObj) {
    markObj(sMarksStr, c.r[5], sStrKept);
    if (sStrKept) {
        if (logNow()) log("[casters] f%u structure %08X model %08X %s drawn as a shadow caster", copyDispCount(), sStrActor, c.r[5],
                          cflight::modelName(c.r[5]).c_str());
    }
    callOriginal(c, "Insert__Q34nw4r3g3d8ScnGroupFUlPQ34nw4r3g3d6ScnObj");
}

// ---- g3d's own culling ----
// nw4r::g3d::ScnObjGather::Add(ScnObj*, bool opa, bool xlu) with the gather frustum set (see the top).
HLE(Add__Q34nw4r3g3d12ScnObjGatherFPQ34nw4r3g3d6ScnObjbb) {
    static const char* const kName = "Add__Q34nw4r3g3d12ScnObjGatherFPQ34nw4r3g3d6ScnObjbb";
    const u32 gather = c.r[3], obj = c.r[4], opa = c.r[5];
    callOriginal(c, kName);
    if (c.r[3] != 2) {
        markObj(sMarksG3d, obj, false);
        return;
    }
    if (!active(sLag)) return;
    const u32 fr = gatherFrustum(c);
    if (!fr) return;
    sStats.g3dCulled++;
    if (!keptAabb(fr, obj)) return;
    // Kept: a culled group's children are visited (0 = intersecting), a culled model goes into the
    // opaque list only (no translucent draw is a caster) and is marked for its draw.
    c.r[3] = 0;
    if (!opa) return;
    const u32 n = rd32(gather + 0x10), cap = rd32(gather + 0xC), list = rd32(gather + 4);
    if (n >= cap || !list) return;
    wr32(list + 4 * n, obj);
    wr32(gather + 0x10, n + 1);
    markObj(sMarksG3d, obj, true);
    sStats.g3dKept++;
    if (logNow()) log("[casters] f%u g3d object %08X %s drawn as a shadow caster", copyDispCount(), obj, cflight::modelName(obj).c_str());
}

}  // namespace recomp
