# Function discovery

The recompiler needs, for `main.dol` and each of the 187 RELs, every function (start, size),
every jump table, and the real names of the SDK functions the HLE layer replaces. City Folk's
decomp was for rev 0 and is offline; our discs are rev 1, so the tables are generated from the
discs. Everything in `config/<id>/` is address/name data only (no game data, no code).

## What is in config/

Per version (`RUUE01` USA, `RUUP01` PAL, `RUUJ01` Japan, `RUUK01` Korea):

| File | Content |
|---|---|
| `symbols.txt` | main.dol, decomp-toolkit symbol format (the format LM's recompiler reads, `loadSymbols`): functions (`type:function size:`), interior entry points (`type:label` inside a function: the recompiler makes them alternate entries), jump tables (`jumptable_<addr>` objects), data objects. Header comment lines start with `//`. |
| `rels/<module>.txt` | one per REL, same line format with **section-relative offsets** (`.text:0x000000CC` = offset in the module's `.text`). Header: module id, file, sha1, sections (`name:index:size:x/d/b`), prolog/epilog/unresolved, bss size/alignment, `fix_size`, the modules it relocates against. REL jump tables are `jumptable_<module>_<section>_<offset>`. |
| `modules.txt` | one line per REL: id, name, sha1, sections, imported module ids. |
| `names.txt` | every named DOL function: `dol <addr> <name> <confidence> <sources> <unit>`. The names in `symbols.txt` come from here. |
| `splits.txt` | the library translation units (from the reference maps' units of the named functions), `.text` ranges, decomp-toolkit splits format. Partial: library code only. |
| `coverage.txt` | per module: functions, labels, jump tables, named, code bytes, bytes in functions, zero padding, other bytes outside functions, and how many entries the passes added (split/label/new). |
| `us_map.txt`, `rels_us_map.txt` (PAL, Japan, Korea) | every function matched to the USA one: `<addr> <USA addr> <method> <name>`, `<module> <offset> <USA offset> <method>`. |

The SDK functions the HLE needs, with their addresses in all three versions: docs/SDK_NAMES.md.

## How the tables are built

`tools/discovery/` (Python 3, run on a dev machine with the user's own extracted discs):

1. **decomp-toolkit** (`discover.py`, `dtk dol split`; encounter/decomp-toolkit, MIT, a
   dev-only tool, not in the repo: download a release binary) analyses main.dol and all RELs
   together: control flow from the entry point, every `bl`/`b` target, REL relocations
   (`R_PPC_REL24` to module/section/offset across modules), `.ctors`/`.dtors`, data pointers,
   and its built-in signatures (runtime, TRK, some OS/IPC functions).
2. **Completion passes** (`analysis.py`):
   - jump tables: every `lwzx; mtctr; bctr` switch (DOL: `lis/addi` base, REL: the
     `ADDR16_HA/LO` relocations of the base, entries are `ADDR32` relocations), bound from the
     `cmplwi`; dtk names some tables `@<n>`/`lbl_` and misses a few, they are renamed/added as
     `jumptable_`. Switch cases dtk had split off as a separate function after the `bctr` are
     merged back (one case: `fn_8033AE50`).
   - entry points: every call/branch target (`bl`, `b`/`bc` leaving a function, `REL24` from
     any module), every code address in data (DOL data words, REL `ADDR32`/`ADDR16` relocations
     outside jump tables) and every `lis/addi` code address is a function start. Inside a
     function it is split there when the previous instruction ends the flow and no branch
     crosses the cut; otherwise it becomes a label (alternate entry). Address-shaped constants
     in the middle of straight-line code (round values like `0x80300000`) are ignored (the
     recompiler adds entries for materialised addresses itself, LM `addAddressTakenEntries`).
   - code in gaps between functions becomes a function.
3. **Names** for the DOL (`sdk_names.py`, `strnames.py`), see below.
4. **PAL / Japan / Korea** (`version_map.py`): their own tables come from steps 1-2 on their discs; the
   USA names are carried over by matching functions (below).
5. **Checks** (`verify.py`): on the final tables, against the disc: every `bl`, every `b`/`bc`
   leaving its function and every `REL24` relocation lands on a function start or label; every
   non-zero code word is inside a function (except the TRK vectors, below); REL code-address
   relocations are function starts or jump-table cases. Exit code 1 on any bad branch.

## Names

Anchors: the functions decomp-toolkit's signatures name (356, `dtk`). Then, for each of seven
public CC0 Wii decomps that link the same RVL SDK / NintendoWare / EGG / MSL libraries, their
committed `config/*/symbols.txt` + `splits.txt` (address, size and name data only; no source
read or copied), restricted to library units/names:

| Reference | Game | Why |
|---|---|---|
| SMGCommunity/Petari `RMGE01` | Super Mario Galaxy | SDK of the same period (OSInit 0x478 as ours) |
| doldecomp/mkw `RMCP01` | Mario Kart Wii | 2008 SDK, NW4R, EGG, DWC/GameSpy |
| doldecomp/brawl `RSBE01_02` | Super Smash Bros. Brawl | 2008 SDK, BTE, NWC24 |
| zeldaret/ss `SOUE01` | Skyward Sword | well-named NW4R (snd, g3d, lyt, ef), EGG, HBM |
| doldecomp/ogws `RSPE01_01` | Wii Sports | NW4R, RVLFaceLib, HBM |
| zeldaret/tp `RZDE01_02` | Twilight Princess (Wii) | SDK, HBM |
| SeekyCt/spm-decomp `EU0` | Super Paper Mario | SDK |

Methods, in order (confidence in `names.txt`):
- `high`: walking from an anchor through our function list and the reference's library
  functions together while the function sizes are equal (link order is kept by the linker,
  function order inside a unit too); after a size mismatch only resynchronising on 3 equal
  sizes in a row (or 2 that together are at least 0x200 bytes). Seeds also come from unique
  5-function size patterns (k-grams, at least 0x180 bytes, more than two distinct sizes).
- `mid`: one function between two size-matched neighbours whose own size differs (another SDK
  revision of the same function).
- `gap`: between two of our named functions that a reference has in the same order, the
  functions in between: by position when the counts agree (and any already named ones agree),
  otherwise by an order-preserving alignment of equal sizes (sizes >= 0x20 only).
- `str`: `strnames.py`, the function's own `X() was called` trace string (DWC). OSPanic/assert
  texts are not used: they name the API entry (`DVDReadAsync` inside `DVDReadAsyncPrio`) and
  agreed with the reference names only 45% of the time.
- the references vote; disagreements are logged (`--report`), a name is not given twice unless
  some reference uses it for several functions (static functions such as `Callback`).
- `manual`: `config/<id>/names_manual.txt` (`dol <addr> <name> <note>`), applied last
  (none yet).
- `map` (PAL/Japan/Korea): from the USA name of the matched function.

Spot checks (disassembly): `OSGetTime` (mftbu/mftb loop), `SCGetAspectRatio`,
`VISetNextFrameBuffer`, `IOS_Close(Async)`, `PADRead`, `AXSetVoiceState/Addr`, `KPADRead`.

## Version matching (PAL, Japan, Korea)

Each function is fingerprinted by its instructions with addresses masked (`b`/`bl` targets,
16-bit immediates of `addi/addis/ori/oris` and of loads/stores, which hold relocated or
r13/r2-relative addresses). Both function lists keep their link order, so they are aligned as
sequences (difflib): equal runs pair as `hash`, a changed run of equal length pairs in order
(`order`), other changed runs pair by equal sizes in order (`size`). RELs: module by module.

## Coverage (2026-10-27)

| | USA RUUE01 | PAL RUUP01 | Japan RUUJ01 | Korea RUUK01 |
|---|---|---|---|---|
| DOL functions | 24195 | 24194 | 24191 | 24251 |
| DOL named | 6149 (dtk 356, high 5328, mid 49, gap 412, str 4) | 6149 carried (+ own dtk) | 6149 carried | 6148 carried (no `KPRInitRegionUS`) |
| DOL interior entry labels | 91 (mostly `_savegpr_N`/`_restgpr_N`, `_savefpr_N`) | 92 | 88 | 87 |
| DOL jump tables | 395 | 395 | 395 | 401 |
| DOL code bytes in functions | 4581296 / 4589504 | 4581268 / 4589472 | 4581092 / 4589280 | 4616936 / 4628768 |
| DOL bytes outside functions | 6556 zero padding + 1652 TRK vectors | same | same | 10180 zero padding + 1652 TRK vectors |
| REL functions (187 modules) | 16658 | 16658 | 16658 | 16659 |
| REL jump tables | 161 | 161 | 161 | 161 |
| REL code bytes outside functions | 0 | 0 | 0 | 0 |
| Matched to USA (DOL / RELs) | - | 24187 / 16658 (hash 24168, order 19) | 24180 / 16658 (hash 24144, order 35, size 1) | 24097 / 16658 (DOL hash 23683, order 408, size 6; RELs hash 16618, order 40) |

`verify.py` passes on all four (no bad branch, every non-zero code word in a function). Every
HLE target in docs/SDK_NAMES.md has an address in RUUK01.

### Korea (RUUK01) against Japan and USA

The Korean disc (2010, the only Korean release, revision 1) is a later build than the other
three (newest library build string: ENC, Jun 17 2009; RevoEX 2.1plus5 of Apr 16 2009, the
others have 2.1plus4 of Aug 2008): its DOL is 93 KB larger and matches the USA DOL slightly better than the Japanese one
(24097 vs 24095 functions matched). Function differences (`scratchpad` comparison with
`version_map.align`, base Japan; the USA comparison gives the same runs):

| Where (RUUK01) | Functions | What |
|---|---|---|
| 0x800C982C-0x800CB290 | 5 new (Japan has 13 of its own there) | game code (not identified; Japan's own functions there are gone) |
| 0x801F990C | 1 new; `KPRInitRegionUS` gone | KPR (USB keyboard input) region init: a Korean one replaces the US/JP one |
| 0x80220400-0x80222394 | 11 new (Japan has 6 of its own, USA others) | unnamed library code between VF and IPC, region-specific in every version (probably the ENC character-encoding library: Korea links a newer ENC, `<< RVL_SDK - ENC` Jun 17 2009 vs Aug 8 2007; Japan and USA each have their own functions here too) |
| 0x802258F0-0x80225FE4 | 6 new | after the IPC profile code (unnamed library) |
| 0x802E72A4-0x80321260 | ~110 new, ~100 changed in place (`order`) | DWC / RevoEX network libraries (unnamed in every version): RevoEX 2.1plus5 (Apr 2009) instead of 2.1plus4; the HBM and GameSpy parts around it are unchanged |
| 0x803760DC | 1 new | GameSpy server browsing (after `SBListThink`) |
| 0x8040131C-0x80402390 | 9 new | after WENC, before TPL: probably `RVL_SDK - FA` (Mar 4 2009), a library only the Korean DOL links |

Every SDK library the HLE replaces (OS, GX, VI, DVD, IPC/IOS, NAND, SC, WPAD/KPAD, AX/AI/DSP,
NWC24 ...) is hash-identical to the other versions apart from addresses. The 187 RELs keep
their ids, names and sections; none is byte-identical to any other disc. Against Japan, 30
modules have changed functions, all in text-handling spots of menus/NPCs: `d_menu_friend01-05NP`,
`d_menu_hanNP` (3), `d_menu_desi00-03NP`, `d_menu_writeNP`, `d_menu_chatNP`, `d_menu_noticeNP`,
`d_menu_musicNP`, `d_menu_inscNP`, `d_menu_star01NP`, `d_paperNP` (3), `d_a_ftrNP`,
`d_a_npc_sp_catherineNP`, `_fukoNP`, `_kinuyoNP`, `_player_selectNP`, `_resetsanNP`,
`_seiichiNP`, `_totakekeNP`, `d_a_rmobj_telNP`, and `d_a_npc_sp_gatekeeper_aNP` (one function
more: 679). `d_insect_fieldNP` differs from Japan in 20 functions but is hash-identical to the
USA one (Japan's insect spawning code is its own; Korea took the international one).

Named DOL functions by library (USA): RVL SDK (os, gx, vi, dvd, nand, fs/isfs, ipc, es, sc,
wpad, kpad, wud, bte, ax, axfx, ai, dsp, arc, mem, mtx, si, exi, pad, nwc24, so, vf, ...),
NintendoWare (g3d, snd, lyt, ut, ef, math), EGG, MSL C, runtime, TRK, RVLFaceLib, Home Button
(nw4hbm), GameSpy (DWC's GP/gt2/qr2/serverbrowsing): counts per unit in docs/SDK_NAMES.md.
The REL tables carry decomp-toolkit's names only (`_prolog`, `_epilog`, `_unresolved`,
`__register_global_object`, `__destroy_global_chain` and constructors/destructors: 659); the
game code itself is unnamed (`fn_<module>_<offset>`).

Largest modules (functions): d_a_playerNP 953, d_insect_fieldNP 942, d_insect_museumNP 733,
d_a_ftr_actNP 719, d_a_npc_sp_gatekeeper_aNP 678, d_fish_museumNP 592, d_a_npc_outNP 418.
Per module: `config/<id>/coverage.txt`.

## Things to know (for the engine)

- RELs are uncompressed, version 3, one executable section (`.text`, index 1), then
  `.ctors`, `.dtors`, `.rodata`, `.data` (missing in some), `.bss`; prolog/epilog/unresolved in
  `.text`. There is no `.sel` file: imports from the DOL (module 0) are absolute addresses.
- **RELs import from other RELs**, not only from the DOL: e.g. module 115 `d_ftr_managerNP` is
  imported by 18 modules, 111 `d_fgobj_managerNP` by 11, 123 `d_str_managerNP` by 10, 99
  `d_a_strNP`, 3 `d_menu_itemNP`, 15 `d_a_ftrNP`, 139 `d_menu_friendNP` by 7-8. Module 1
  `d_profileNP` (the actor profile table) relocates against **every** module 12-187 (data
  pointers into each actor's profile), so OSLink's "fix up imports of modules linked later"
  path is used constantly. Calls between RELs are `REL24` relocations (49738 in all).
- The DOL has an extra small-data bss section (`.sbss2`, 0x80753C60, 0x60 bytes; dtk lists two
  `.sbss`). `_SDA_BASE_` 0x80751940, `_SDA2_BASE_` 0x80757FC0 (USA).
- 0x800044C0-0x800063F4 (`gTRKInterruptVectorTable`) are the debugger's exception vectors,
  copied to low memory by TRK init and never called: left out of the function table (1652
  non-zero bytes). `_rom_copy_info` / `_bss_init_info` are data inside `.init`.
- A few DOL functions are entered in the middle (recorded as labels): `entry_800B886C` and
  `entry_8016CFBC` are called from RELs (d_a_playerNP, d_a_npc_sp_sisyoNP), `entry_8037B37C` is
  the `bl`-to-next-instruction address trick in `OSExceptionVector`, five more come from
  address-shaped values in data that happen to follow a return (harmless extra entries). Seven more call targets inside dtk functions were clean cuts and became
  their own functions (e.g. `fn_800F6818`, reached from d_a_npc_sp_pyontaroNP; the code before
  it falls into it with `b`).
- Two copies of the VF (SD FAT) library are linked (a small one near 0x802058A8 and the full one
  0x8041xxxx-0x8043xxxx); a few VF names can only go to one of them.
- MSL's printf and scanf both have static `parse_format`; duplicate names in `symbols.txt` are
  such statics (16 names, e.g. `Callback`, `OnShutdown`, `__sinit_\lyt_*_cpp`). HLE by name
  should use names that are unique (all HLE targets in docs/SDK_NAMES.md are).
- Not linked into City Folk (verified by position in the reference order): `PADInit` (PAD is
  linked without its init; no GameCube controller), `OSJamMessage`, `GXSetDrawDone`/
  `GXWaitDrawDone`/FIFO save functions, `GXPeek*/GXPoke*`, `DVDOpenDir/ReadDir`, `SCFlush`,
  `KPADSetBtnRepeat/SetSensorHeight/CalibrateDPD`, `AISetDSPSampleRate`, `DSPAssertInt`, ...
  (docs/SDK_NAMES.md lists each with the evidence).

## Known gaps

- Libraries no reference has: NCD, SO (most), SSL, NHTTP, DWC (except trace-string names and
  GameSpy), MP/MPDL, PMIC, KBD/KPR, ENC, NETAES, RevoEX. Their functions stay `fn_` unless a
  manual name is added; docs/SDK_NAMES.md lists the ones the HLE wants.
- Game code (DOL and RELs) is unnamed. Names found later (hooks) go into
  `config/RUUE01/names_manual.txt`; re-run the pipeline to carry them to PAL/Japan/Korea.
- `mid`/`gap` names rest on order and neighbours, not on the instructions; a wrong one would
  show as an HLE that misbehaves: check the disassembly (`disasm.py`) before relying on one.
- REL function names are not carried between versions (they are `fn_` anyway), but every REL
  function is matched (`rels_us_map.txt`) for hooks written against USA offsets.

## Re-running

```
pip install capstone                    # only for disasm.py
python tools/discovery/run_all.py --dtk <path to dtk.exe> --refs <dir for reference maps> --fetch-refs ^
    [--us "US DATA/DATA"] [--pal "PAL DATA/DATA"] [--jp "JAPAN DATA/DATA"] [--kr "KOREA DATA/DATA"] [--work build/discovery]
```

Runs, for USA: `discover.py` (dtk + passes) -> `sdk_names.py` -> `strnames.py --apply` ->
`discover.py --skip-dtk` (names merged) -> `verify.py`; for PAL, Japan and Korea: `discover.py` ->
`version_map.py` -> `discover.py --skip-dtk` -> `verify.py`; then `sdk_report.py`
(docs/SDK_NAMES.md). About 3 minutes. `--fetch-refs` downloads the reference maps (raw files of
the repositories in `sdk_names.py` REFS) into `--refs`; keep them outside the repo. Tested with
decomp-toolkit 1.8.4. dtk on Windows wants drive-less absolute paths in its YAML (`discover.py`
writes them so; the discs must be on the same drive as the work dir).

Dev aids: `disasm.py <main.dol|x.rel> <addr|offset> [n]` (with relocations for RELs),
`compare.py --refs <dir> --ref <name> --name <ref function> --addr <our addr>` (our functions
next to a reference's, for names_manual.txt), `sdk_report.py --refs <dir>` (why a wanted name
is missing).
