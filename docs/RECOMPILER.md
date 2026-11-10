# Build-time recompiler

Our own tool translates the user's own `main.dol` **and its REL modules** into C++ at build time
(dev build `cfrecomp`) or at first-run setup (`CityFolkRecomp`, docs/WII.md). Generated code lives
in `build/**/recomp_gen` (or the setup's work folder) and is never committed. Base: the Luigi's
Mansion port's recompiler (LM `src/recompiler`, docs/RECOMPILER.md there); everything added here
is generic and can go back to LM/SMS (docs/BACKPORT.md).

## Pieces
1. **Recompiler** (`src/recompiler/recompiler.cpp`, `tools/recomp/cfrecompile.cpp`): one C++
   function per guest function, `void f_XXXXXXXX(Ctx&)` for the DOL, `void rMMM_S_OOOOOO(Ctx&)`
   for REL module MMM, section S, offset OOOOOO. In-function branches are labels/goto, `bl` is a
   direct call, `bctrl`/`blrl` go through the run-time lookup (`Ctx::call`), `bctr` with a known
   jump table is a `switch`. Output: `funcs.h`, `funcs_000.cpp` .. `funcs_063.cpp` (balanced by
   instruction count; `kChunks`), `func_table.cpp` (DOL function table, names, hooked originals,
   REL module tables, `kGameModule`).
2. **Runtime** (`src/recomp/`): guest memory, `Ctx`, scheduler, HLE SDK, frame driver; REL linking
   in `rel.cpp` (below).

## Inputs
- `config/<id>/symbols.txt` (DOL) and `config/<id>/rels/<module>.txt` (one per REL, offsets
  relative to the named section; `.text` = the module's first executable section, `.secN` =
  section N), in LM's symbols.txt format (written by tools/discovery, docs/DISCOVERY.md).
- `config/<id>/splits.txt` (optional): library translation units. Functions in SDK units
  (`os/`, `gx/`, `vi/`, `pad/`, `dvd/`, `ai/`, `dsp/`, `exi/`, `si/`, `db/`, `base/`, `ipc/`, with or
  without `revolution/`/`dolphin/`; minus pure-computation units such as OSAlloc, OSArena, OSUtf,
  ipc/memory.c) become HLE calls: `hleLookup(name)` or `hleMissing` (fatal; `CF_HLE_MISSING=log`
  logs once and returns 0, for boot work).
- **Every other named DOL function is replaceable by name at run time**: it is emitted as
  `static HleFn hook = hleLookup("name"); if (hook) { hook(c); return; } f_X_orig(c);`, so an
  `HLE(name)` anywhere in the runtime replaces it (the body stays callable through
  `callOriginal`). Auto names (`fn_`, ...) are not hookable unless listed in hooks.txt.
- `tools/recomp/hooks.txt`: hooks on REL functions (`<module>::<name>`), instruction patches.
- Without maps (or for code the maps miss) the recompiler's own **scan** finds functions: the entry
  point, every `bl` target, code addresses in data (vtables, function pointers), lis/addi pairs,
  every relocation target (REL), minus jump-table targets. Jump tables come from the maps
  (`jumptable_` objects) or are detected (DOL: a lis/addi of a data address followed by a `bctr`;
  REL: a LO relocation to the module's data followed by a `bctr`, whose words are ADDR32
  relocations back into the code). Scanned functions run to the next start; branches into the
  middle of a function and address-taken interior code become extra entry points.

## REL modules
The Revolution/Dolphin SDK links relocatable modules at run time (`OSLink`): the game reads a
`.rel` file into a heap buffer and the SDK patches every relocation in place. Code addresses are
therefore only known at run time. Design (generic; `src/recompiler/rel_format.h` is the shared
format description):

- **At recompile time** each module is parsed (header v1-v3, section table, import table,
  relocation lists) and translated at its section offsets. Relocations are indexed by the location
  they patch.
  - `R_PPC_REL24` on a `b`/`bl` is resolved statically: into the DOL (module 0: the addend is the
    absolute address) a direct call of `f_XXXXXXXX`; into the module itself a goto or call; into
    another module a direct call of its function, guarded by `relBase(module, section) != 0`
    (linked), else `rel_unresolved(c, self)` runs this module's `_unresolved` like the console's
    unrelocated branch.
  - `R_PPC_ADDR16_LO/HI/HA` on an instruction's immediate become expressions of the target's
    run-time section base: `relBase(m, s) + addend` (split into lo/hi/ha), constants for DOL targets.
    The generated code never reads its own patched instruction words.
  - Return addresses (`c.lr = ...`), trace entries and jump-table `switch`es use the function's
    own section base (`rb_ = relBase(m, s)`), so guest-visible code addresses (saved LR on the
    stack, vtable entries, function pointers) are the real run-time addresses.
- **At run time** (`src/recomp/rel.cpp`):
  - `HLE(OSLink)`, `HLE(OSLinkFixed)`, `HLE(OSUnlink)` do what the SDK's do: module list in low
    memory (0x800030C8/CC, string table 0x800030D0), offsets made absolute, bss placed and
    cleared, all relocations applied in guest memory (DOL imports, self imports, and both
    directions between the new module and every linked one; OSLinkFixed then trims the import
    table), OSUnlink undoes other modules' imports (data 0, branches to their `_unresolved`).
    Data sections (vtables, `.ctors`, pointers) are thus exactly as on the console.
  - Linking registers the module: `g_relBase[id * 32 + section]` = section address (read by the
    generated code), and its code ranges + the recompiled function table (`RelModuleDesc` in
    `GameModule`, matched by module id; the exec sections' FNV-1a checksum from the file is
    compared before relocation and a mismatch is logged). Unlinking clears both.
  - Indirect calls: `Ctx::call(addr)` looks in the DOL table first; otherwise
    `callOther` -> `lookupRelFunc` (linked module containing addr, binary search by key).
    Calling an address no function starts at is fatal with the module/function name
    (`relSymbolize`). `CF_REL_LOG=1` logs each link/unlink with section addresses.
- **Game module DLL** (setup): `RuntimeApi` carries `relBase`, `callOther`, `relUnresolved`
  (`kModuleAbiVersion` 4).
- Not handled: compressed RELs (none on the City Folk disc), REL14/ADDR14 relocations on
  instructions (none; the recompiler marks them unsupported), hooks on REL functions by name
  through the automatic name lookup (use hooks.txt).

## Exit: stopping the guest threads (branch crash-120)
At exit (window closed, F1 quit, `CF_QUIT_AFTER`) `frame::stop` -> `sched::shutdown` ends every guest
thread with `ThreadExit` (C++ unwinding back to `threadMain`). Threads waiting at a scheduling point
leave at once; the one running guest code only at its next scheduling point. Until 2026-11-09 that
one played on with the others already gone, and they looked terminated: City Folk's main thread
polls a worker thread (0x805A0228) with OSIsThreadTerminated (`fn_800C7C84`), took its half-built
result and called through it (`call to 00010000 (lr=8030852C)` in `fn_80308500`, or `call to null
(lr=8030BC4C)`; ~half the exits at 120 fps, where the main thread is decoupled and usually running).
Now:
- a thread ended by the exit is never shown to the game as terminated: no OSThread state or exit
  value written, joiners not woken, `isTerminated` false (`GuestThread::killed`);
- `shutdown` sets the interrupt flag, so the running thread leaves at its next safe point
  (`Ctx::poll` -> `serviceOnThread` throws), and OSIsThreadTerminated / OSJoinThread from a guest
  thread throw too.
The whole stop takes a few ms. Test aid `CF_SHUTDOWN_LOG=1` (thread list before and after, where
each thread left, `CF_LOG_TIME=1` for times); a `call to <addr>` fatal prints r3, its vtable, r4 and
r31.

## Instruction coverage
Gekko = Broadway: LM's coverage (integer, branch, load/store incl. update and indexed forms,
lmw/stmw, single/double FP, paired singles incl. quantized loads/stores).

## Status (2026-10-27)
RUUE01: 41k functions (24.3k DOL, 16.7k in 187 RELs), 548 jump tables, generated C++ ~80 MB; builds
with the dev target. Boot status: docs/WII.md.
