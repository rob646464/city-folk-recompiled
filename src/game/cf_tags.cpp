// Frame interpolation tag sources for City Folk (docs/INTERPOLATION.md "Tagged interpolation";
// docs/WII.md "Frame interpolation"). The game draws through NW4R: g3d scene objects for the 3D,
// lyt panes for the 2D (cf_hud.cpp tags those), ef for effects. g3d writes its model matrices
// straight into the write-gather pipe; hle_gx.cpp notes those raw XF loads like the GX calls.
//
// - View: nw4r::g3d::ScnRoot::DrawOpa / DrawXlu (one structural scope per pass).
// - Object: G3dProc of ScnMdlSimple / ScnMdl / ScnProc (the scene object = r3; the task = r4, so a
//   model's opaque and translucent draws are separate scopes). Its matrices blend rigidly.
// - Camera: G3DState::SetCameraProjMtx(const Camera&, int, bool) makes that camera current; its
//   CameraData (Camera +0) starts with the camera (view) matrix, MTX34, which viewMatrixSource
//   returns for camera factoring.
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"
#include "recomp/version.h"
#include "game/cf_light.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/gx_bridge.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace recomp {
namespace world {
void onScnRoot(u32 root, bool xlu, bool begin);  // cf_world.cpp (F1 > Visuals > Enhanced world)
}
namespace {
u32 sViewSrc = 0;
}  // namespace

namespace interp {
u32 viewMatrixSource() { return sViewSrc; }
}  // namespace interp

HLE(SetCameraProjMtx__Q34nw4r3g3d8G3DStateFRCQ34nw4r3g3d6Cameraib) {
    // ScnRoot::SetGlbSettings stores all 32 cameras of the scene (r4 = index) and passes r5 = true
    // only for the current one (ScnRoot +0xF4), the camera the scene is drawn with. Taking the last
    // one stored made camera 31 (a fixed identity camera) the view of every scene: no camera motion
    // was ever factored and no cut was ever found.
    if (const u32 data = rd32(c.r[3]); data && c.r[5] != 0) sViewSrc = data;
    // Test aid CF_CAM_LOG=1: every camera made current (its CameraData, view matrix translation, caller).
    static const bool sCamLog = std::getenv("CF_CAM_LOG") != nullptr;
    if (sCamLog && c.r[5] && sViewSrc)
        log("[cam] f%u camera %08X index %u data %08X t %.2f %.2f %.2f r0 %.3f %.3f %.3f lr %08X", game::guestFrames(), c.r[3],
            c.r[4], sViewSrc, rdf32(sViewSrc + 12), rdf32(sViewSrc + 28), rdf32(sViewSrc + 44), rdf32(sViewSrc),
            rdf32(sViewSrc + 4), rdf32(sViewSrc + 8), c.lr);
    cflight::onCamera(c, rd32(c.r[3]));
    callOriginal(c, "SetCameraProjMtx__Q34nw4r3g3d8G3DStateFRCQ34nw4r3g3d6Cameraib");
}

HLE(DrawOpa__Q34nw4r3g3d7ScnRootFv) {
    if (!cflight::onRootDraw(c, false, true)) return;
    {
        interp::TagScope t(interp::TagCat::View, c.r[3], 0, 0);
        world::onScnRoot(c.r[3], false, true);
        callOriginal(c, "DrawOpa__Q34nw4r3g3d7ScnRootFv");
        world::onScnRoot(0, false, false);
    }
    cflight::onRootDraw(c, false, false);
}
HLE(DrawXlu__Q34nw4r3g3d7ScnRootFv) {
    if (!cflight::onRootDraw(c, true, true)) return;
    {
        interp::TagScope t(interp::TagCat::View, c.r[3], 0, 1);
        world::onScnRoot(c.r[3], true, true);
        callOriginal(c, "DrawXlu__Q34nw4r3g3d7ScnRootFv");
        world::onScnRoot(0, true, false);
    }
    cflight::onRootDraw(c, true, false);
}

namespace {
void objectScope(Ctx& c, const char* name, bool proc = false) {
    const u32 obj = c.r[3];
    // A culled object kept for its shadow (cf_shadow_casters.cpp): a shadow caster only, never
    // drawn in the picture; tagged like any other object, so it blends when it comes into view.
    const int cast = cfcast::g3dDrawMode(obj, c.r[4]);
    if (cast == 2) return;
    if (cast == 0 && !cflight::onModel(c, obj)) return;
    interp::TagScope t(interp::TagCat::Object, obj, rd32(obj), c.r[4]);
    interp::tagGroupIfNone(obj);
    if (cast == 1) cfcast::shadowOnlyBegin();
    // Indoors the room background outside the objects casts no shadow (cflight::roomShellMode): an
    // object does.
    // (ScnProcs without a model are the room background's own draws, sun_* models the window light's
    // shafts and floor patches: they stay non-casting.)
    (void)proc;
    const bool shell = enh::noCast() && cflight::castsIndoors(obj, c.r[4]);
    if (shell) gx::endPendingDraw(), enh::setNoCast(false);
    callOriginal(c, name);
    if (shell) gx::endPendingDraw(), enh::setNoCast(true);
    if (cast == 1) cfcast::shadowOnlyEnd();
}
}  // namespace

// nw4r::ef particles. The billboard draw strategy (0x80292388, the only caller of
// DrawQuad(const MTX34&, const VEC3*, bool) seen) loops over its ParticleManager's (r26) particles
// in r27 and draws each with DrawQuad: on entry to DrawQuad r27 still holds the caller's particle
// (callee-saved), so each quad is a Particle scope of its own instead of the n-th quad of the
// emitter (a particle born or retired shifted every later one's pairing).
// Test aid CF_EF_PEEK=<words>: each particle's first words once per game frame (birth stamp search).
HLE(DrawQuad__Q24nw4r2efFRCQ34nw4r4math5MTX34PCQ34nw4r4math5_VEC3b) {
    static const char* const kName = "DrawQuad__Q24nw4r2efFRCQ34nw4r4math5MTX34PCQ34nw4r4math5_VEC3b";
    if (c.lr < VA(0x80292388) || c.lr >= VA(0x802930BC)) return callOriginal(c, kName);
    const u32 particle = c.r[27], manager = c.r[26];
    static const int sPeek = [] { const char* v = std::getenv("CF_EF_PEEK"); return v ? std::atoi(v) : 0; }();
    if (sPeek > 0 && interp::capturing()) {
        std::string line;
        char buf[16];
        for (int w = 0; w < sPeek; w++) {
            std::snprintf(buf, sizeof(buf), " %08X", rd32(particle + 4 * w));
            line += buf;
        }
        log("[efpeek] f%u pm %08X p %08X%s", game::guestFrames(), manager, particle, line.c_str());
    }
    // Birth stamp: the particle's age (u16 at +0xDC, +1 per game frame in every particle followed
    // with CF_EF_PEEK) makes its birth frame part of the identity, so a recycled particle slot is a
    // new particle.
    const u32 birth = (game::guestFrames() - rd16(particle + 0xDC)) & 0xFFFFu;
    interp::TagScope t(interp::TagCat::Particle, particle, manager, birth);
    callOriginal(c, kName);
}

// Field objects (trees, bushes, flowers, fences, signs...): the town's field draw is ONE ScnProc
// (d_fg_drawNP) that draws every visible object through d_fg_drawNP fn_112_2184 (r3 = the object's
// record: +0x8 kind, +0x14..0x1C position; a visibility test, then the kind's draw function), each
// loading its model matrix from the same scratch MTX34 (0x80763AD0). Under the one ScnProc scope
// those loads were paired by their order: when a tree came into or left the view while walking,
// every later object's matrix was blended with ANOTHER object's (trees slid between two trees for
// an in-between, or "jumped" back at the game frame when the pair was caught as a jump; the user's
// "trees jumping around" at 120 fps, branch interp-town). Each object is now its own scope keyed by
// its place in the world and its kind, whatever the draw order. A/B: CF_FG_TAG=0.
static void fgObjDraw(Ctx& c) {
    static const char* const kName = "d_fg_drawNP::fn_112_2184";
    static const bool sOn = [] {
        const char* v = std::getenv("CF_FG_TAG");
        return !(v && *v == '0');
    }();
    const u32 rec = c.r[3];
    // (cf_shadow_casters.cpp: the object's visibility test inside may keep it as a shadow caster)
    cfcast::fgBegin();
    if (!sOn || !interp::capturing() || rec < 0x80000000u) {
        callOriginal(c, kName);
        cfcast::fgEnd();
        return;
    }
    const u32 x = rd32(rec + 0x14), y = rd32(rec + 0x18), z = rd32(rec + 0x1C), kind = rd32(rec + 0x8);
    // The place as the identity (not an address: draw order and record reuse do not matter). The
    // object word stays below guest memory, so freeing guest memory never bumps its generation.
    const u32 place = ((x * 0x9E3779B1u) ^ (z * 0x85EBCA77u) ^ (y * 0xC2B2AE35u)) & 0x00FFFFFFu;
    {
        interp::TagScope t(interp::TagCat::Object, 0x01000000u | place, 0xF6F60000u ^ kind, x ^ (z << 16 | z >> 16) ^ y);
        callOriginal(c, kName);
        cfcast::fgEnd();  // (inside the scope: the class switch back belongs to this object)
    }
}
static HleReg sFgObjReg("d_fg_drawNP::fn_112_2184", &fgObjDraw);

// nw4r::ef particles of every draw strategy (the billboard strategy is tagged per particle in DrawQuad
// above; the others, e.g. DrawQuad(VEC3, VEC3, VEC3) of the fireflies and the fireworks bursts, and
// fn_8028FF0C's of Mr. Resetti's dirt and the running dust, drew every particle of an emitter under
// one scope, keyed by draw order or matched by place). While DrawOrder::Draw(Effect*, DrawInfo&)
// draws an effect (cf_light.cpp hooks it), every particle of its managers (Effect + 0x90: list of
// ParticleManagers; + 0x38: list of 0xE4-byte Particles, age u16 at + 0xDC) is handed to the
// interpolation with its manager and birth frame; a strategy's loop keeps the particle it draws in a
// callee-saved register (interp gxCall). A/B: CF_EF_TAG_ALL=0.
namespace cftags {
void efBegin(u32 effect) {
    static const bool sOn = [] { const char* v = std::getenv("CF_EF_TAG_ALL"); return !(v && *v == '0'); }();
    if (!sOn || !interp::capturing() || effect < 0x80000000u) return;
    std::vector<std::array<u32, 3>> parts;
    const u32 list = effect + 0x90;
    const u32 off = rd16(list + 0xA);
    u32 pm = rd32(list);
    const u32 now = game::guestFrames();
    for (int g = 0; g < 256 && pm >= 0x80000000u && pm < 0x94000000u; g++, pm = rd32(pm + off + 4)) {
        static const bool sLog = std::getenv("CF_EF_TAG_LOG") != nullptr;
        static std::unordered_map<u32, u32> sNamed;
        if (sLog && sNamed.size() < 4096 && sNamed.emplace(pm, rd32(pm + 0x24)).second) {
            std::string n;
            const u32 res = rd32(pm + 0x24), str = res >= 0x80000000u ? rd32(res) : 0;
            for (u32 k = 0; str >= 0x80000000u && k < 40; k++) {
                const char ch = char(rd8(str + k));
                if (!ch) break;
                n += ch >= 32 && ch < 127 ? ch : '?';
            }
            log("[eftag] manager %08X emitter '%s'", pm, n.c_str());
        }
        const u32 plist = pm + 0x38;
        const u32 poff = rd16(plist + 0xA);
        u32 p = rd32(plist);
        for (u32 n = 0; n < 2048 && p >= 0x80000000u && p < 0x94000000u; n++, p = rd32(p + poff + 4))
            parts.push_back({p, pm, (now - rd16(p + 0xDC)) & 0xFFFFu});
    }
    interp::setEffectParticles(std::move(parts));
}
void efEnd() { interp::setEffectParticles({}); }
}  // namespace cftags

HLE(G3dProc__Q34nw4r3g3d12ScnMdlSimpleFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d12ScnMdlSimpleFUlUlPv"); }
HLE(G3dProc__Q34nw4r3g3d6ScnMdlFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d6ScnMdlFUlUlPv"); }
HLE(G3dProc__Q34nw4r3g3d7ScnProcFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d7ScnProcFUlUlPv", true); }

}  // namespace recomp
