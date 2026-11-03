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

#include <cstdio>
#include <cstdlib>
#include <string>

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
    callOriginal(c, "SetCameraProjMtx__Q34nw4r3g3d8G3DStateFRCQ34nw4r3g3d6Cameraib");
}

HLE(DrawOpa__Q34nw4r3g3d7ScnRootFv) {
    interp::TagScope t(interp::TagCat::View, c.r[3], 0, 0);
    world::onScnRoot(c.r[3], false, true);
    callOriginal(c, "DrawOpa__Q34nw4r3g3d7ScnRootFv");
    world::onScnRoot(0, false, false);
}
HLE(DrawXlu__Q34nw4r3g3d7ScnRootFv) {
    interp::TagScope t(interp::TagCat::View, c.r[3], 0, 1);
    world::onScnRoot(c.r[3], true, true);
    callOriginal(c, "DrawXlu__Q34nw4r3g3d7ScnRootFv");
    world::onScnRoot(0, true, false);
}

namespace {
void objectScope(Ctx& c, const char* name) {
    const u32 obj = c.r[3];
    interp::TagScope t(interp::TagCat::Object, obj, rd32(obj), c.r[4]);
    interp::tagGroupIfNone(obj);
    callOriginal(c, name);
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
    if (c.lr < 0x80292388u || c.lr >= 0x802930BCu) return callOriginal(c, kName);
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

HLE(G3dProc__Q34nw4r3g3d12ScnMdlSimpleFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d12ScnMdlSimpleFUlUlPv"); }
HLE(G3dProc__Q34nw4r3g3d6ScnMdlFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d6ScnMdlFUlUlPv"); }
HLE(G3dProc__Q34nw4r3g3d7ScnProcFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d7ScnProcFUlUlPv"); }

}  // namespace recomp
