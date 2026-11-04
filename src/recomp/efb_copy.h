#pragma once
// EFB copies to RAM, runtime side (efb_copy.cpp, docs/RENDERING.md).
#include "recomp/ctx.h"

#include <functional>
#include <string>

namespace recomp::efbcopy {
void onFrame(u32 frameNo);         // guest frame boundary: settings, writebacks, stats
void cpuRead(u32 addr, u32 size);  // DCInvalidateRange: the CPU will read guest memory
// GXCopyTex issued by the game (its thread, before the copy goes to Aurora): what the memory held
// then, so a late readback never lands on something the game wrote there afterwards.
void guestCopy(u32 addr, u32 width, u32 height, u32 fmt);
void* dumpTarget(u32 frameNo);     // CF_FRAME_DUMP: host buffer to copy this frame's EFB into, or null
// One-off dump of the next finished guest frame (model viewer sweep): `done` gets the 640x480
// RGB pixels; `path` (optional) receives a .ppm downscaled by `shrink`.
void requestNamedDump(const std::string& path, int shrink, std::function<void(const u8* rgb, u32 w, u32 h)> done);
// Interpolation tests (CF_INTERP_TEST with CF_FRAME_DUMP): a buffer whose copy is written to
// <dump dir>/<name>.ppm when its readback lands; namedDumpWritten tells when it did.
void* namedDumpTarget(const char* name);
bool namedDumpWritten(const void* target);
void applyWritebacks();  // host thread, while the guest is held
// Port features that read their own EFB copies back (e.g. the Mii maker preview): `fn` is called
// after every writeback into guest memory (the thread applying writebacks; guest held), with the
// host pointer and size of the range written. Register once.
void addWritebackListener(void (*fn)(const void* dest, u32 size));
}  // namespace recomp::efbcopy
