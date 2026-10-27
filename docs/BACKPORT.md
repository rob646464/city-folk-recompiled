# Generic changes for the LM / SMS ports

Fixes and features made here that the Luigi's Mansion (LM) and Super Mario Sunshine (SMS) ports
can take. Status: pending = not in LM/SMS yet.

| # | What | Files | LM/SMS use it? | Status |
|---|---|---|---|---|
| 1 | Recompiler: REL modules (relocation-aware translation, cross-module branches, run-time section bases, REL jump tables from relocations), OSLink/OSLinkFixed/OSUnlink HLE + linked-module function lookup for indirect calls (docs/RECOMPILER.md "REL modules") | src/recompiler/recompiler.cpp, rel_format.h, src/recomp/rel.cpp, ctx.h (g_relBase, RelModuleDesc, GameModule rels), module_abi.h/module_rt.cpp (relBase, callOther, relUnresolved; ABI 4) | no (no RELs), SMS has none either | pending, optional |
| 2 | Recompiler: built-in function scan (bl targets, code pointers in data and lis/addi pairs, relocation targets) filling gaps of a partial symbol map; jump-table detection without a map entry for both DOL (SMS's detector extended) and REL | src/recompiler/recompiler.cpp | could help maps with holes | pending |
| 3 | Recompiler: names in generated string literals escaped, comments without backslashes (map names like `__sinit_\ut_FileStream_cpp` broke the build) | src/recompiler/recompiler.cpp `lit`, `commentText` | latent | pending |
| 4 | Recompiler: every named DOL function is replaceable by name at run time (hook prologue + `_orig`), not only hooks.txt entries | src/recompiler/recompiler.cpp `run` | LM/SMS use hooks.txt; optional | pending, optional |
| 5 | `CF_HLE_MISSING=log`: a missing HLE logs once and returns 0 instead of stopping (boot work) | src/recomp/runtime.cpp hleMissing | useful test aid | pending |
| 6 | `CF_TRACE_FRAMES=<first>,<count>` with `CF_TRACE=all`: trace only those guest frames; trace lines name the guest thread | src/recomp/runtime.cpp trace_enter | test aid | pending |
| 7 | Scheduler: `setPriority` (OSSetThreadPriority) and `suspendCount` (OSIsThreadSuspended) | src/recomp/sched.cpp/.h, hle_os.cpp | not linked in LM | pending, optional |
| 8 | Guest memory helpers `hostOffset()` / `guestAddrOfOffset()` instead of `a & kHostMemMask` (MEM2 layout) | src/recomp/ctx.h, runtime.cpp watch, hle_gx.cpp phys | LM: drop-in with its 64 MiB window | pending, optional |
| 9 | DVD HLE: copied DVDFileInfo structs are recognised by their start address + length (NW4R's DvdFileStream reads through its own copy); folder discs get each file's FST offset as start address | src/recomp/hle_dvd.cpp | LM/SMS (JSystem keeps its pointer): latent | pending |
| 10 | GX HLE additions: GXGetYScaleFactor / GXGetNumXfbLines (not implemented in Aurora), GXSetProjectionv, GXInitTexObjWrapMode, texture object getters, GXGetTexObjLODAll (remembered LOD params), light getters + GXInitLightAttnA/K, indexed matrix loads (GXLoadNrmMtxIndx3x3 written as CP LOAD_INDX_B: Aurora lacks it) | src/recomp/hle_gx.cpp | not linked in LM | pending, optional |
| 11 | OSCalendarTimeToTicks HLE; RVL OSAlarm size 0x30 (user data) | src/recomp/hle_os.cpp | GC alarm is 0x28 (LM's memset is right there) | n/a for LM |
| 12 | `cfembed` folder mode (a directory becomes one blob of its .txt files) | tools/recomp/cfembed.cpp | — | optional |

## Aurora patch series
Same files as LM (patches/aurora, LM's 0001-0250 at Aurora 50e5be3); no City Folk patches (0300+)
yet. Note: upstream Aurora has moved on (WPAD/KPAD implementations); we stay on LM's commit.
