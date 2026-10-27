#pragma once

// Memory report (issue #34, docs/RENDERING.md "Memory use over time"): a `[mem]` line in the log
// (last_run.log) every 60 s with the process's private bytes, working set and peak, the GPU memory
// it uses (Windows: DXGI per-process local / non-local usage), and the sizes of the main host-side
// caches (Aurora textures, pipelines, bind groups, EFB copies to RAM, vertex snapshots, interpolation
// tapes). A plateau is a warm cache; a line that keeps climbing over many room visits is a leak.
//
// Test aid: CF_MEM_LOG=<seconds> (interval, default 60; 0 = off).

namespace recomp::mem {

// Host thread, once per frame: writes the line when the interval has passed.
void report();
// CF_MEM_TRACK=1 (Windows): the C++ heap call sites that grew most (mem_track.cpp); =2: every
// module's heap allocations (HeapAlloc imports redirected), also summed per module.
void trackInit();  // after aurora_initialize (the graphics driver and Dawn are loaded)
void trackReport();

}  // namespace recomp::mem
