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
// frames; a CF_SHOT image s<N> shows the draws made while the copy count was N-1, so with 1 the
// EVEN images are the widened ones), CF_CASTERS_KINDS=<mask> (which kinds are widened, below;
// CF_CASTERS_NOSTR=1 = all but structures), CF_CASTERS_INSERT_LOG=1 (every model an actor draw
// inserts, the villagers' windows), CF_CASTERS_FORCE=npc|obj (every villager / fgobj object kept),
// CF_CASTERS_NPC_WINDOW=<f> (the NPC draw's window scaled: villagers in the picture culled and kept).
//
// Branch shadow-casters-2 adds the actors (villagers / NPCs, other players, fgobj objects, a main.dol
// actor with the frustum test), trees behind the camera and single fruits, blob shadows of kept
// objects, per-structure heights from the model's box (the city's buildings); see the docs.
#include "game/cf_light.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <aurora/enhanced.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
// CF_CASTERS_KINDS=<mask>: which culling decisions are widened (A/B of one kind): 1 field objects
// (trees, fruits, ...), 2 structures, 4 villagers / NPCs, 8 fgobj objects, 16 other actors (other
// players, fn_80181978), 32 g3d's own culling; default all. CF_CASTERS_NOSTR=1 = all but 2.
constexpr u32 kKindFg = 1, kKindStr = 2, kKindNpc = 4, kKindFgObj = 8, kKindActor = 16, kKindG3d = 32;
const u32 sKinds = [] {
    const char* v = std::getenv("CF_CASTERS_KINDS");
    u32 k = v ? u32(std::strtoul(v, nullptr, 0)) : 0xFFFFFFFFu;
    if (std::getenv("CF_CASTERS_NOSTR")) k &= ~kKindStr;
    return k;
}();
bool kindOn(u32 k) { return (sKinds & k) != 0; }
const float sScale = [] {
    const char* v = std::getenv("CF_CASTERS_SCALE");
    return v ? std::clamp(float(std::atof(v)), 0.0f, 10.0f) : 1.0f;
}();

// Counts for the log (game thread), per game frame.
struct Stats {
    u32 fgCulled = 0, fgKept = 0, strCulled = 0, strKept = 0, g3dCulled = 0, g3dKept = 0, drawn = 0;
    u32 npcCulled = 0, npcKept = 0, objCulled = 0, objKept = 0, lineKept = 0, fruitKept = 0, actCulled = 0, actKept = 0;
    u32 blobs = 0;
};
Stats sStats;
// CF_CASTERS_INSERT_LOG=1: every model an actor draw inserts (log frames only), with the actor kind.
const bool sInsertLog = std::getenv("CF_CASTERS_INSERT_LOG") != nullptr;
// CF_CASTERS_FORCE=npc|obj: test aid for the caster path of actors the routes never see culled:
// every shown villager / every fgobj object is treated as culled and kept for its shadow, so it
// vanishes from the picture and only its shadow is left (CF_CASTERS_SHOW=1: drawn normally again).
const int sForce = [] {
    const char* v = std::getenv("CF_CASTERS_FORCE");
    if (!v) return 0;
    return std::strcmp(v, "npc") == 0 ? 1 : std::strcmp(v, "obj") == 0 ? 2 : 0;
}();

bool logNow() { return sLog && copyDispCount() % sLog == 0; }

// Structures and g3d objects are decided while the game builds the scene (Insert / gather) and
// drawn a copy later (sLag, measured): CF_CASTERS_AB decides them for the frame they are drawn in.
u32 sLag = 1;  // (measured 1: inserted at copy count N, drawn at N + 1; set from the first kept model)
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

constexpr float kStrHeight = 160.0f;  // structures not seen yet (houses 113-216, town hall 188)
// A model's height from its MDL0 bounding box (the info block after the header: min / max at
// info + 0x28 / + 0x34, model space, y up), 0 if unknown.
float modelHeight(u32 obj) {
    for (u32 off = 0xC; off < 0x180; off += 4) {
        const u32 p = rd32(obj + off);
        if (!((p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u)) || (p & 3) || rd32(p) != 0x4D444C30u) continue;
        const u32 rev = rd32(p + 8);
        const u32 info = p + 0x10 + 4 * (rev >= 10 ? 14u : 11u) + 4;
        // (City Folk's models leave the "extents enabled" byte at + 0x22 clear but store the box.)
        if (rd32(info) < 0x40) return 0.0f;
        const float h = rdf32(info + 0x38) - rdf32(info + 0x2C);
        return std::isfinite(h) && h > 0.0f && h < 5000.0f ? h : 0.0f;
    }
    return 0.0f;
}
// Each structure's height: the tallest of the models it inserted (seen once drawn or kept), else
// the common height. Keyed by the actor and its unique id (fBase + 0: a new stage's actor at the
// same address is another key).
std::unordered_map<u64, float> sStrHeights;
u64 actorKey(u32 actor) { return u64(actor) << 32 | rd32(actor); }
float strHeight(u32 actor) {
    const auto it = sStrHeights.find(actorKey(actor));
    return it != sStrHeights.end() ? std::max(it->second, 40.0f) : kStrHeight;
}
void noteStrModel(u32 actor, u32 obj) {
    const float h = modelHeight(obj);
    if (!(h > 0.0f)) return;
    if (sStrHeights.size() > 4096) sStrHeights.clear();
    float& v = sStrHeights[actorKey(actor)];
    v = std::max(v, h);
}
// The shadow of an object on the flat map (structures, NPCs, line tests): its position moves by `o`
// (x east, z south) from the foot to the tip of the shadow of a point `height` up.
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

// Field objects: inside d_fg_drawNP fn_112_2184 (sInFg); the object kept only for its shadow
// (sFgShadow), its first frustum test done (sFgTested: later tests in the same draw are a tree's
// fruits), the fruit being drawn kept only for its shadow (sFruitShadow).
bool sInFg = false, sFgShadow = false, sFgTested = false, sFruitShadow = false;
// Structures: inside d_a_strNP fn_99_2B04 (its cull test fn_99_2548, then its draw); kept = the
// test passed only with the shadow.
bool sInStr = false, sInStrCull = false, sStrWidened = false;
// Other actors whose draw inserts their models: villagers and other NPCs (fn_8001665C), other
// players (d_a_playerNP fn_80_E20), d_fgobj_managerNP's objects (fn_111_E4C), the main.dol actor
// with a frustum test (fn_80181978). One draw at a time (the game thread).
enum class Actor { None, Npc, Player, FgObj, Sphere };
Actor sActor = Actor::None;
bool sActorTested = false, sObjShadow = false;
// The actor being drawn is kept only for its shadow: the models it inserts are shadow casters,
// its blob shadow is not drawn (structures too).
bool sActorKept = false;
u32 sKeptActor = 0;
const char* actorKind() {
    switch (sActor) {
    case Actor::Npc: return "npc";
    case Actor::Player: return "player";
    case Actor::FgObj: return "fgobj";
    case Actor::Sphere: return "actor";
    default: return sInStr ? "structure" : "-";
    }
}
// Shadow lengths of the actors (flat map, like structures): villagers / players are about 40-50
// units tall, fgobj objects (items, presents, balloons on the ground...) below 40.
constexpr float kNpcHeight = 60.0f;
constexpr float kObjHeight = 40.0f;
// An actor draw scope: kept state cleared around the game's draw.
struct ActorScope {
    Actor prev;
    explicit ActorScope(Actor a, u32 actor) : prev(sActor) {
        sActor = a, sActorTested = false, sActorKept = false, sKeptActor = actor;
    }
    ~ActorScope() {
        if (sObjShadow) {
            sObjShadow = false;
            recomp::cfcast::shadowOnlyEnd();
        }
        sActor = prev, sActorKept = false, sActorTested = false;
    }
};
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

// A reference point loaded by lis / addi at the start of a function (fn_80166CDC, fn_800284F8:
// USA lis r5, 0x8062 / addi r5, r5, 0x426C, the camera's point in the town's flat coordinates),
// decoded so every version finds its own.
u32 decodeRef(const char* name, u32 lisOff, u32 addiOff) {
    const u32 fn = guestSymbol(name);
    if (!fn) return 0u;
    const u32 i0 = rd32(fn + lisOff), i1 = rd32(fn + addiOff);
    if ((i0 >> 26) != 15 || (i1 >> 26) != 14) return 0u;
    const u32 ref = ((i0 & 0xFFFF) << 16) + u32(s32(s16(u16(i1 & 0xFFFF))));
    log("[casters] %s window reference at %08X", name, ref);
    return ref;
}
u32 windowRef() {
    static const u32 sRef = decodeRef("fn_80166CDC", 0x0, 0xC);
    return sRef;
}
u32 npcWindowRef() {
    static const u32 sRef = decodeRef("fn_800284F8", 0x0, 0x4);
    return sRef;
}

bool blobSkipped() { return sActorKept || sFgShadow || sFruitShadow; }
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
            "npcs culled %u kept %u, fgobj culled %u kept %u, actors culled %u kept %u, field objects behind kept %u, fruits kept %u, "
            "blob shadows skipped %u, models drawn as casters %u, Aurora caster-only draws %u | sun %.2f %.2f %.2f sinEl %.2f reach %.0f",
            frame, active() ? 1 : 0, sStats.fgCulled, sStats.fgKept, sStats.strCulled, sStats.strKept, sStats.g3dCulled, sStats.g3dKept,
            sStats.npcCulled, sStats.npcKept, sStats.objCulled, sStats.objKept, sStats.actCulled, sStats.actKept, sStats.lineKept,
            sStats.fruitKept, sStats.blobs, sStats.drawn, aurora_enhanced_shadow_class_draws(), sSun.l[0], sSun.l[1], sSun.l[2], sinEl,
            reach);
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
void fgBegin() { sInFg = true, sFgTested = false; }
void fgEnd() {
    sInFg = false, sFgTested = false;
    if (sFruitShadow) {
        sFruitShadow = false;
        shadowOnlyEnd();
    }
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
// The same test in an actor's draw (fn_80181978): kept = its models are casters.
// In a field object's draw the first test is the object's; d_fg_drawNP's tree draw (kind 0,
// fn_112_17F0) then tests each of its fruits the same way before drawing it (the fruit loop at the
// end of the draw, nothing else drawn after a fruit): a culled fruit of a visible tree is a caster
// on its own, until the next fruit's test or the object's end.
HLE(fn_802B777C) {
    const u32 fr = c.r[3], sp = c.r[4];
    callOriginal(c, "fn_802B777C");
    if (sActor == Actor::Sphere && !sInFg) {
        if (sActorTested) return;
        sActorTested = true;
        if (c.r[3] != 0 || !active(sLag) || !kindOn(kKindActor)) return;
        sStats.actCulled++;
        if (!keptSphere(fr, sp)) return;
        sStats.actKept++;
        c.r[3] = 1;
        sActorKept = true;
        return;
    }
    if (!sInFg) return;
    const bool fruit = sFgTested;
    sFgTested = true;
    if (sFruitShadow) {  // the previous fruit's draw is over
        sFruitShadow = false;
        cfcast::shadowOnlyEnd();
    }
    if (c.r[3] != 0) return;
    if (!active() || !enh::inScene() || enh::casterPass() || !kindOn(kKindFg)) return;
    if (fruit && sFgShadow) {  // a fruit of a tree drawn as a caster: a caster too where its shadow reaches
        if (keptSphere(fr, sp)) c.r[3] = 1;
        return;
    }
    if (!fruit) sStats.fgCulled++;
    if (!keptSphere(fr, sp)) return;
    c.r[3] = 1;
    if (fruit) {
        sStats.fruitKept++;
        sFruitShadow = true;
    } else {
        sStats.fgKept++;
        sFgShadow = true;
    }
    cfcast::shadowOnlyBegin();
}

// The line behind the camera (r3 = camera, r4 = position, f1 = size; nonzero = culled), used by the
// tree draw d_fg_drawNP fn_112_17F0 (before anything else: a tree behind the camera is culled even
// when its sphere is in the frustum) and by d_fgobj_managerNP's object draw fn_111_E4C (its only
// cull test: those objects are never culled at the sides). Tested again with the shadow's tip when
// the shadow falls north (toward the view; the culled side is +z).
HLE(fn_8018AB88) {
    static const char* const kName = "fn_8018AB88";
    const bool fg = sInFg, obj = sActor == Actor::FgObj && !sInFg;
    if (!fg && !obj) return callOriginal(c, kName);
    const u32 r3 = c.r[3], pos = c.r[4];
    const double f1 = c.f[1];
    callOriginal(c, kName);
    if (obj) {
        if (sActorTested) return;
        sActorTested = true;
    }
    if (c.r[3] == 0 && !(obj && sForce == 2)) return;
    if (fg ? (!active() || !enh::inScene() || enh::casterPass()) : !active(sLag)) return;
    if (!kindOn(fg ? kKindFg : kKindFgObj)) return;
    if (obj) sStats.objCulled++;
    // Height: the field object's bounding sphere (record + 0x34, the position is record + 0x14).
    const float height = fg ? std::clamp(2.0f * rdf32(pos + 0x20), 20.0f, 400.0f) : kObjHeight;
    float o[2];
    flatShadow(height, o);
    if (c.r[3] != 0) {
        if (!(o[1] < 0.0f)) return;  // the shadow falls south (behind): nothing reaches the view
        const float z = rdf32(pos + 8);
        wrf32(pos + 8, z + o[1]);
        c.r[3] = r3, c.r[4] = pos, c.f[1] = f1;
        callOriginal(c, kName);
        wrf32(pos + 8, z);
        if (c.r[3] != 0) return;
    }
    if (fg) {
        sStats.lineKept++;
        if (!sFgShadow) {
            sFgShadow = true;
            cfcast::shadowOnlyBegin();
        }
    } else {
        sStats.objKept++;
        sActorKept = true;
        if (enh::inScene() && !sObjShadow) {  // (drawn at once inside a scene pass: the class too)
            sObjShadow = true;
            cfcast::shadowOnlyBegin();
        }
    }
}

// ---- actors ----
// Blob shadows (the round sdw_* shadow under an actor, fn_801C863C; structures and trees
// fn_801C874C) are registered by the actor's draw after its model: not for one kept only for its
// shadow (the game culled it, blob included).
HLE(fn_801C863C) {
    if (blobSkipped() && on()) {
        sStats.blobs++;
        return;
    }
    callOriginal(c, "fn_801C863C");
}
HLE(fn_801C874C) {
    if (blobSkipped() && on()) {
        sStats.blobs++;
        return;
    }
    callOriginal(c, "fn_801C874C");
}

// Villagers and the other NPCs (the NPC base draw fn_8001665C, used by d_a_npc_outNP's villagers,
// the special NPCs d_a_npc_sp_*, the city's d_a_npc_town_*...): when the NPC is shown (+0x1C38),
// its position (+0x64) is tested against a window around the camera's point in the town's flat
// coordinates (fn_800284F8: box at NPC + 0x19E4, min x / z at +0 / +8, max at +0xC / +0x14, relative
// to the reference point; 1 = outside), then its model (vtable + 0x74) and the held / worn one
// (+ 0x7C) are drawn (inserted into the scene), then its blob shadow.
HLE(fn_8001665C) {
    static const char* const kName = "fn_8001665C";
    if (!active(sLag)) return callOriginal(c, kName);
    ActorScope s(Actor::Npc, c.r[3]);
    if (sInsertLog && logNow()) {
        const u32 npc = c.r[3], box = npc + 0x19E4, ref = npcWindowRef(), vt = rd32(npc + 0x60);
        u32 mod = 0, best = 0;  // the REL whose .data (section 5) holds the vtable: nearest base below
        for (u32 m = 1; m < 200; m++)
            if (const u32 b = relBase(m, 5); b && b <= vt && b > best) best = b, mod = m;
        log("[casters] f%u npc %08X shown %u pos %.0f %.0f %.0f window x %.0f..%.0f z %.0f..%.0f around %.0f %.0f vtable %08X (module %u)",
            copyDispCount(), npc, rd8(npc + 0x1C38), rdf32(npc + 0x64), rdf32(npc + 0x68), rdf32(npc + 0x6C), rdf32(box), rdf32(box + 0xC),
            rdf32(box + 8), rdf32(box + 0x14), ref ? rdf32(ref) : 0.0f, ref ? rdf32(ref + 8) : 0.0f, vt, mod);
    }
    callOriginal(c, kName);
}

// Test aid CF_CASTERS_NPC_WINDOW=<f>: the game's own test in the draw sees its window scaled by f
// (the AI's tests and the widened test keep the real one): with f < 1 the game culls villagers that
// are in the picture and the widened test keeps them as casters, so their shadows (animated,
// without the villager) can be checked on screen against the normal draw (CF_CASTERS_AB).
const float sNpcWindow = [] {
    const char* v = std::getenv("CF_CASTERS_NPC_WINDOW");
    return v ? std::clamp(float(std::atof(v)), 0.01f, 10.0f) : 1.0f;
}();
struct ScaledBox {
    u32 box = 0;
    float v[6] = {};
    ScaledBox(u32 b, bool on) {
        if (!on || sNpcWindow == 1.0f || !b) return;
        box = b;
        for (int i = 0; i < 6; i++) v[i] = rdf32(b + 4 * i), wrf32(b + 4 * i, v[i] * sNpcWindow);
    }
    ~ScaledBox() {
        if (box)
            for (int i = 0; i < 6; i++) wrf32(box + 4 * i, v[i]);
    }
};
HLE(fn_800284F8) {
    const u32 box = c.r[3], pos = c.r[4];
    const bool own = sActor == Actor::Npc && !sActorTested;
    {
        ScaledBox scaled(box, own);  // (the game's own test only; the widened one below uses the real box)
        callOriginal(c, "fn_800284F8");
    }
    if (!own) return;
    sActorTested = true;  // the draw's own test (its first)
    if ((c.r[3] == 0 && sForce != 1) || !kindOn(kKindNpc)) return;
    const u32 ref = npcWindowRef();
    if (!ref || !box || !pos) return;
    sStats.npcCulled++;
    float o[2];
    flatShadow(kNpcHeight, o);
    const float dx = rdf32(pos) - rdf32(ref), dz = rdf32(pos + 8) - rdf32(ref + 8);
    float t0 = 0.0f, t1 = 1.0f;
    if (sForce == 1 || (clipSlab(dx, o[0], rdf32(box), rdf32(box + 0xC), t0, t1) &&
                        clipSlab(dz, o[1], rdf32(box + 8), rdf32(box + 0x14), t0, t1))) {
        c.r[3] = 0;
        sActorKept = true;
        sStats.npcKept++;
    }
}

// Other players (visitors online; d_a_playerNP fn_80_E20): the player being played is always
// drawn, another one only when fn_80_248C (r3 = player, 1 = culled) finds its position (+0x64 /
// +0x6C) inside the flat window around the camera's point. The window's bounds are the module's
// constants, so the test is repeated with the position moved along the shadow (quarter steps).
static void playerDraw(Ctx& c) {
    static const char* const kName = "d_a_playerNP::fn_80_E20";
    if (!active(sLag)) return callOriginal(c, kName);
    ActorScope s(Actor::Player, c.r[3]);
    callOriginal(c, kName);
}
static HleReg sPlayerDrawReg("d_a_playerNP::fn_80_E20", &playerDraw);

static void playerCull(Ctx& c) {
    static const char* const kName = "d_a_playerNP::fn_80_248C";
    const u32 pl = c.r[3];
    callOriginal(c, kName);
    if (sActor != Actor::Player || sActorTested || c.r[3] == 0 || !kindOn(kKindActor)) return;
    sActorTested = true;
    sStats.actCulled++;
    float o[2];
    flatShadow(kNpcHeight, o);
    const float x = rdf32(pl + 0x64), z = rdf32(pl + 0x6C);
    bool kept = false;
    for (int k = 1; k <= 4 && !kept; k++) {
        wrf32(pl + 0x64, x + o[0] * 0.25f * float(k));
        wrf32(pl + 0x6C, z + o[1] * 0.25f * float(k));
        c.r[3] = pl;
        callOriginal(c, kName);
        kept = c.r[3] == 0;
    }
    wrf32(pl + 0x64, x), wrf32(pl + 0x6C, z);
    c.r[3] = kept ? 0u : 1u;
    if (kept) {
        sActorKept = true;
        sStats.actKept++;
    }
}
static HleReg sPlayerCullReg("d_a_playerNP::fn_80_248C", &playerCull);

// d_fgobj_managerNP's objects (fn_111_E4C, r3 = object: +0xC position, model at +0x24): only the
// line behind the camera (fn_8018AB88 above).
static void fgObjDraw(Ctx& c) {
    static const char* const kName = "d_fgobj_managerNP::fn_111_E4C";
    if (!active(sLag)) return callOriginal(c, kName);
    ActorScope s(Actor::FgObj, c.r[3]);
    callOriginal(c, kName);
}
static HleReg sFgObjDrawReg("d_fgobj_managerNP::fn_111_E4C", &fgObjDraw);

// A main.dol actor's draw with the camera frustum test (fn_80181978: bounding sphere of its model
// +0x138, then the model and its blob shadow).
HLE(fn_80181978) {
    static const char* const kName = "fn_80181978";
    if (!active(sLag)) return callOriginal(c, kName);
    ActorScope s(Actor::Sphere, c.r[3]);
    callOriginal(c, kName);
}

// ---- structures ----
// daSTR_c's draw: cull test (fn_99_2548), then the model draw that inserts its models.
static void strDraw(Ctx& c) {
    static const char* const kName = "d_a_strNP::fn_99_2B04";
    if (!active(sLag)) return callOriginal(c, kName);
    sInStr = true, sActorKept = false, sKeptActor = c.r[3];
    callOriginal(c, kName);
    sInStr = false, sActorKept = false;
}
static HleReg sStrDrawReg("d_a_strNP::fn_99_2B04", &strDraw);

// Its cull test: nonzero = not drawn. The window / line tests inside are widened while it runs.
static void strCull(Ctx& c) {
    static const char* const kName = "d_a_strNP::fn_99_2548";
    if (!sInStr) return callOriginal(c, kName);
    if (!kindOn(kKindStr)) return callOriginal(c, kName);
    sInStrCull = true, sStrWidened = false;
    callOriginal(c, kName);
    sInStrCull = false;
    if (sStrWidened) {
        sStats.strCulled++;
        if (c.r[3] == 0) {
            sActorKept = true;  // drawn only because of its shadow: its models are casters
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
    flatShadow(strHeight(sKeptActor), o);
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
    flatShadow(strHeight(sKeptActor), o);
    if (!(o[1] < 0.0f)) return;  // the shadow falls south (behind): nothing reaches the view
    // Tested again with the shadow's tip (the culled side is +z; the tip is further north).
    const float z = rdf32(pos + 8);
    wrf32(pos + 8, z + o[1]);
    c.r[3] = r3, c.r[4] = pos, c.f[1] = f1;
    callOriginal(c, "fn_8018ABC8");
    wrf32(pos + 8, z);
}

// A structure drawn for its shadow inserts its models (every frame): they are casters this frame.
// The same for the other actors' models (villagers, their tools / umbrellas, fgobj objects...).
HLE(Insert__Q34nw4r3g3d8ScnGroupFUlPQ34nw4r3g3d6ScnObj) {
    const u32 obj = c.r[5];
    markObj(sMarksStr, obj, sActorKept);
    if (sInStr && sKeptActor) noteStrModel(sKeptActor, obj);
    if (logNow() && (sActorKept || (sInsertLog && (sActor != Actor::None || sInStr))))
        log("[casters] f%u %s %08X model %08X %s%s (lr %08X, height %.0f%s)", copyDispCount(), actorKind(), sKeptActor, obj,
            cflight::modelName(obj).c_str(), sActorKept ? " drawn as a shadow caster" : "", c.lr, modelHeight(obj),
            sInStr ? (" of " + std::to_string(int(strHeight(sKeptActor)))).c_str() : "");
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
    if (!active(sLag) || !kindOn(kKindG3d)) return;
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
