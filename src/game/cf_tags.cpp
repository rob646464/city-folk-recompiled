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
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/runtime.h"

namespace recomp {
namespace {
u32 sViewSrc = 0;
}  // namespace

namespace interp {
u32 viewMatrixSource() { return sViewSrc; }
}  // namespace interp

HLE(SetCameraProjMtx__Q34nw4r3g3d8G3DStateFRCQ34nw4r3g3d6Cameraib) {
    if (const u32 data = rd32(c.r[3])) sViewSrc = data;
    callOriginal(c, "SetCameraProjMtx__Q34nw4r3g3d8G3DStateFRCQ34nw4r3g3d6Cameraib");
}

HLE(DrawOpa__Q34nw4r3g3d7ScnRootFv) {
    interp::TagScope t(interp::TagCat::View, c.r[3], 0, 0);
    callOriginal(c, "DrawOpa__Q34nw4r3g3d7ScnRootFv");
}
HLE(DrawXlu__Q34nw4r3g3d7ScnRootFv) {
    interp::TagScope t(interp::TagCat::View, c.r[3], 0, 1);
    callOriginal(c, "DrawXlu__Q34nw4r3g3d7ScnRootFv");
}

namespace {
void objectScope(Ctx& c, const char* name) {
    const u32 obj = c.r[3];
    interp::TagScope t(interp::TagCat::Object, obj, rd32(obj), c.r[4]);
    interp::tagGroupIfNone(obj);
    callOriginal(c, name);
}
}  // namespace

HLE(G3dProc__Q34nw4r3g3d12ScnMdlSimpleFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d12ScnMdlSimpleFUlUlPv"); }
HLE(G3dProc__Q34nw4r3g3d6ScnMdlFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d6ScnMdlFUlUlPv"); }
HLE(G3dProc__Q34nw4r3g3d7ScnProcFUlUlPv) { objectScope(c, "G3dProc__Q34nw4r3g3d7ScnProcFUlUlPv"); }

}  // namespace recomp
