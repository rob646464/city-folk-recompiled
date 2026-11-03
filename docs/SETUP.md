# First-run setup, discs and the game module

End users run one program, `CityFolkRecomp.exe`. It contains the runtime (memory, scheduler,
HLE SDK, Aurora, menu, audio), the recompiler and a setup screen. The game itself is a DLL that
setup builds on the user's machine from the user's own disc (main.dol **and all 187 REL
modules**), with a bundled clang + lld. Nothing derived from the game is shipped or committed.
The program, the updater, portable mode and long-path handling come from the Luigi's Mansion
port (LM `docs/SETUP.md`); this file describes the City Folk version.

## Release layout

`cmake --install build/<preset> --prefix <dir>` produces:

```
CityFolkRecomp.exe        runtime + recompiler + setup UI (+ the function tables of all four releases)
*.dll                     SDL3, Dawn (+ dxcompiler/dxil), nod, libpng, zlib
llvm/bin/clang.exe        bundled compiler (from CF_LLVM_DIR, default: the LLVM that built the exe)
llvm/bin/lld-link.exe     bundled linker
llvm/LLVM-LICENSE.TXT     Apache-2.0 WITH LLVM-exception (packaging/LLVM-LICENSE.TXT)
pipelines.bin             shader/pipeline precache list (data/pipelines.bin; docs/RENDERING.md "Pipeline precache")
README.txt                player notes (packaging/<platform>/README.txt)
```

The LLVM binaries are copied at install time, never committed. Toolchain lookup order:
`<exe dir>/llvm/bin`, `CF_LLVM_DIR`, the LLVM that built the exe (dev builds, `CF_DEV_LLVM_DIR`),
then `PATH`.

Linux adds `CityFolkRecompiled.sh` (launcher: portable-mode check, `last_run.log` /
`previous_run.log` in the data folder, exit status), `llvm/bin/clang` + `llvm/bin/ld.lld` (the real
binaries behind the release's symlinks) and `llvm/lib/libxml2.so.2` (a stub: the official lld links
libxml2 only for lld-link manifests; packaging/linux/libxml2_stub.c). macOS: `llvm/bin/ld64.lld`.

## Packages and releases (CI)

Same pipeline as LM (LM .github/workflows/build.yml, tools/release_windows.ps1, docs/LINUX.md,
docs/MACOS.md), with City Folk's names:

- **GitHub Actions** (`.github/workflows/build.yml`): on pushes to `main`, `ci/**` (push any branch
  as `ci/<name>` to have it built) and `perf-ci`, on tags `v*`, pull requests and
  `workflow_dispatch` (works once the file is on main). Three jobs build only `CityFolkRecomp`
  (no disc: `CF_DISC=/nonexistent`, the cfrecomp dev build is skipped) and upload
  `CityFolkRecompiled-<version>-windows-x64.zip` / `-linux-x86_64.tar.gz` / `-macos-arm64.tar.gz`
  as artifacts (version = `project(cfport ... VERSION)` in CMakeLists.txt). Windows bundles the
  runner's LLVM and checks the package (exe, pipelines.bin, README, clang, lld-link) and that
  `CityFolkRecomp.exe --check-disc` loads and refuses a missing disc (exit 3). Linux (Ubuntu
  24.04) and macOS (macos-26, Apple silicon, unsigned) bundle the official LLVM 23.1.4 release
  (sha256-pinned, cached).
- `tools/package_linux.sh <LLVM bin> [out]`: configure `linux-clang`, build, `cmake --install
  --strip`, file checks, glibc/libstdc++ versions and NEEDED libraries of the exe, clang and ld.lld;
  **fails** on any symbol newer than GLIBC_2.34 and on names the exe both exports and imports (a
  glibc shim reaching itself through the PLT loops forever: LM issues #2/#11, hypotf); module
  smoke test (the bundled clang/ld.lld build a freestanding module with setup's flags,
  `cf_module_init` loads via ctypes); .tar.gz + .zip. The exe needs only libc/libm/libgcc_s plus
  libsqlite3, libzstd and libz from the system (libstdc++ is static).
- `tools/package_macos.sh <LLVM bin> [out]`: the same for macOS: only /System and /usr/lib
  libraries, valid ad-hoc signatures, module smoke test (dylib).
- `tools/release_linux.sh <version> [out]` (WSL or Linux): clean clone of HEAD in `~/cf-release`,
  apt packages, an LLVM-*-Linux-X64 found under ~ or /opt (or `CF_LINUX_LLVM`), package_linux.sh,
  copies `CityFolkRecompiled-<version>-linux-x86_64.*` out.
- `tools/release_windows.ps1 [-SkipLinux] [-SkipMac] [-DryRun] [-MacCheckOnly]`: builds and
  packages Windows, Linux through WSL, fetches the macOS artifact GitHub Actions built from the same
  commit (`gh run list --commit`), fills `packaging/release_notes/v<version>.md` (download table:
  the updater's checksum fallback) and publishes release `v<version>` on
  rob646464/city-folk-recompiled with `gh`. **`-DryRun`** does everything up to the release
  (packages and notes in `out/`, placeholder notes when the file is missing) but never pushes,
  starts a workflow, tags or publishes; it prints the `gh release create` command instead.

No releases or tags exist yet; the repository is private (the updater reports "not public yet").

## Discs

Supported: **revision 1** of the USA (`RUUE01`), European (`RUUP01`), Japanese (`RUUJ01`) and
Korean (`RUUK01`, the only Korean release) disc. One disc of any region is a complete install (every disc carries every region's content,
docs/REGION_CONTENT.md); several can be installed side by side and the player picks the one to
play.

Accepted forms (`src/setup/disc.cpp` for setup, `src/port/disc_folder.cpp` for the running game;
both read only the **game partition**, the update partition is ignored):

| Form | How it is read |
|---|---|
| Wii ISO, encrypted (a normal dump) | nod: partition decrypted on the fly |
| Wii ISO, decrypted (partition data stored unencrypted, header byte 0x61 = 1) | nod |
| WBFS, RVZ, WIA, CISO, GCZ, NFS | nod (RVZ/WIA store the partition decrypted; nod rebuilds what it needs) |
| Extracted game partition folder (`sys/` + `files/`, e.g. `US DATA/DATA`) | host files |
| Dolphin "Extract Entire Disc" folder (holds `DATA/`, `UPDATE/`, ...) or a folder holding one partition folder named `P-...`/`P<n>` | `port::discfolder::resolve` picks the game partition |
| A file inside the partition's `sys/` (e.g. `main.dol`, what a file dialog can pick) | its partition folder |

Identification, at every pick and every start (`Disc::open`):
1. the game partition's header (`sys/boot.bin`): Wii magic, game ID (`RUUE01`/`RUUP01`/`RUUJ01`/
   `RUUK01`; other IDs: "not City Folk" / "a release this port does not support");
2. the SHA-1 of `main.dol` (`port::dol::identify`). A disc is known by its hashes; the revision
   only names an unknown one: when main.dol is not a known build and the revision is not 1, "This
   is revision N of <id>; this port knows only revision 1 of it" (a later Korean or other revision
   needs its hashes added: `dol.cpp`, `config/<id>/modules.txt`). The revision is byte 7 of the
   **outer disc header** (the image's header via nod, or `disc/header.bin` of an extracted partition
   folder; `sys/boot.bin` only when there is none): the Korean rev 1 disc's game partition header
   says 0 (its outer header and Redump/GameTDB say 1; the other three have 1 in both). The game sees
   the partition header at 0x80000000, so `OSInit` writes 0 there for Korea
   (`port::partitionRevision`);
3. the SHA-1 of **every REL** in `files/rels` against the version's `config/<id>/modules.txt`
   (embedded in the program): a missing or changed REL refuses the disc ("N REL modules differ ...
   modified or a bad dump").

Reading and hashing main.dol + 187 RELs takes 30-100 ms (folder, ISO, RVZ, WBFS, CISO, GCZ) and
about 250-300 ms for LZMA WIA. A GameCube disc, another Wii game or a file nod cannot read gets
its own message. `CityFolkRecomp --check-disc <path>` runs the same identification without a
window and exits (0 = supported; prints `OK: ...` or `REFUSED: <message>`).

At run time the game's DVD calls (`src/recomp/hle_dvd.cpp`) are served by `port::discfolder`
for both folders and images: entry numbers, directory order and names come from the partition's
`fst.bin`, file data from `files/` or from nod's partition reader. Aurora's own DVD layer (nod
behind the SDK's DVD API) is no longer used for the game: it failed reads that reach the end of
a file, which the game retried forever (a stream read at the title).

## Setup flow

1. **Pick discs.** The setup screen (Dear ImGui on the Aurora window) has one row per region
   (USA, Europe, Japan, Korea) with *Disc image...* (`SDL_ShowOpenFileDialog`) and *Extracted folder...*
   (`SDL_ShowOpenFolderDialog`); Linux also has a path field. A pick is identified at once; a
   disc of another region than its row moves to its own row ("moved here from the USA row").
   With more than one disc a *Version to play* choice appears (default: the first one installed).
2. **Install** (worker thread, progress bar, cancellable): for each disc whose module is missing
   or stale (the version to play first), recompile its main.dol and every REL with that
   version's function tables (`config/<id>/symbols.txt`, `splits.txt`, `rels/*.txt`, embedded),
   write the 64 generated sources plus the freestanding module sources (`ctx.h`, `module_abi.h`,
   `module_rt.cpp`) to `game/<id>/build`, compile them in parallel (one clang per core, largest
   first), link with lld-link, then delete the generated sources (`CF_KEEP_BUILD=1` keeps them).
3. **Compile shaders** (Aurora precache, as in LM), then **launch**: open the disc in
   `port::discfolder`, check the DOL again, load the module, boot.

Later launches skip the screen when the configured discs are readable, pass identification and
every module is current; stale modules are rebuilt automatically. `--setup` (or F1 > Game > *Add
or change discs...*) opens the screen again. F1 > Game lists the installed discs and switches the
version to play (restart).

Timings (2026-10-27, i9-12900KS, 24 threads): recompiling main.dol + 187 RELs 1.4-1.9 s,
compiling the 64 sources 55-67 s on a quiet PC (175-200 s while other builds ran), linking
0.3-0.5 s: **about one minute** per module (USA from the installed copy 56.6 s, PAL 68.9 s). The
whole first run of the installed copy (setup, shaders, 45 s of play) took 104 s. A later start
(identify + key check) adds 30-300 ms.

### What lives where (`%APPDATA%\Animal Crossing City Folk Recompiled`)

Linux: the XDG config/data/cache folders of the same name. Portable mode: `<program
folder>/portable`, same layout.

| Path | Contents |
|------|----------|
| `install.ini` | `usa_disc=`, `pal_disc=`, `jpn_disc=`, `kor_disc=` (paths as picked; `*_disc_rel` in portable mode), `base=USA\|PAL\|JPN\|KOR` |
| `settings.ini` | F1 menu settings |
| `game/RUUE01/module.dll`, `game/RUUP01/...`, `game/RUUJ01/...`, `game/RUUK01/...` | game modules |
| `game/<id>/module.key` | cache key (below) |
| `game/<id>/recompiler.log` | recompiler summary |
| `pipeline_cache.db`, `dawn_cache.db` | GPU caches (Aurora) |
| `last_run.log`, `previous_run.log`, `last_run_state.ini` | run logs and start-up recovery (as in LM) |

The NAND (saves, SYSCONF) lives in its own folder (docs/NAND.md).

A module's key is the SHA-1 of: the module ABI version, the program version, the game ID and
DOL SHA-1, the compile and link flags, and the embedded inputs of that version (symbols.txt,
splits.txt, modules.txt with every REL's SHA-1, the REL maps, hooks.txt, ctx.h, module_abi.h,
module_rt.cpp, recompiler.cpp). Since the RELs were checked against modules.txt, the key covers
them too. Any change rebuilds the module at the next start.

### Portable mode

As in LM (`port::dataLocation`, `src/port/install.cpp`): `portable.txt` or a `portable` folder next
to the program, or `CF_PORTABLE=1` (`CF_PORTABLE=0` forces the system folders), keeps everything in
`<program folder>/portable`; F1 > Game > Data folder moves the data in or out (request file
`.cf-data-move`, copies `portable.cf-tmp` / `portable.cf-old`, `<dir>.cf-tmp`). A disc not found at
its saved path is looked for at its path relative to the program folder, then by name next to the
program and in `portable/` (images and extracted folders).

### Command line (testing)

`--usa-disc` / `--pal-disc` / `--jpn-disc` / `--kor-disc <path>` prefill a row, `--disc <path>` puts a disc of
any region in its row and makes it the version to play, `--setup-auto` starts installing without a
click, `--setup` forces the screen, `--check-disc <path>` identifies and exits. Point `APPDATA` at
an empty folder for a fresh install, `CF_CACHE_DIR`/`CF_NAND_DIR`/`CF_CARD_DIR` at scratch folders.

## Updates

`src/port/update.cpp` (LM's updater; F1 > Game > *Updates*): GitHub releases of
**rob646464/city-folk-recompiled**, tags `v<version>`, assets
`CityFolkRecompiled-<version>-windows-x64.zip` / `-linux-x86_64.tar.gz` / `-macos-arm64.tar.gz`
(the `cmake --install` layout), SHA-256 from the API's asset digest or the release notes' table.
The repository is private for now: the unauthenticated check gets HTTP 404, reported as "No
published release was found (the project's releases are not public yet)." in F1 > Game and as an
`[update] check failed` log line; nothing else happens and the game runs normally. The startup
check runs only in a release folder (`llvm/bin/clang` next to the program; `pipelines.bin` is not
required) and not with `CF_UPDATE_CHECK=0`. Staging in `<install>/upd`,
`.old` renames, rollback, cleanup and the portable-mode rules are LM's (LM docs/SETUP.md
"Updates", "Long paths").

## Long paths (Windows)

As in LM: `longPathAware` manifest, `port::longPath` (`\\?\`) for the program's own file calls
(updater, module build, install.ini, settings, disc identification of extracted folders), clang/
lld-link deeper than 250 characters hard-linked into `<dataDir>\llvm`, game folders over 234
characters refused with a message, a warning for a deep data folder with long paths off.

## Module boundary, developer builds

As in LM (`recomp/module_abi.h`, `module_rt.cpp`, freestanding `ctx.h`; REL support adds
`relBase`/`callOther`/`relUnresolved` to `RuntimeApi`, docs/RECOMPILER.md). The dev target
`cfrecomp` recompiles at build time (`CF_DISC`, `CF_GAME_ID`) and links statically; `--disc`
accepts the same folders and images as setup.

## Test images (how the formats were checked)

We only have extracted partitions. Test images were built in a scratch folder (never committed,
deleted afterwards) from `US DATA/DATA`: a decrypted Wii ISO (a small Python script: one data
partition with the original ticket/TMD/certificates, files packed after the FST, valid H0-H3
hashes, header byte 0x61 = 1), an encrypted ISO from it by reading it through nod with
`NOD_PARTITION_ENCRYPTION_FORCE_ENCRYPTED` (nod encrypts with its own keys), RVZ/WIA/GCZ from the
encrypted ISO with DolphinTool (`convert`; `verify` found only the expected bad signature), WBFS
and CISO written by the same script. Every form, plus the extracted folder, its parent folder and
`sys/main.dol`, was identified by setup and booted the game to the title screen with no DVD
errors (the WIA image reached the title a few hundred frames later: LZMA). Refusals checked with
patched copies: revision 0, another game ID, one changed main.dol byte, one changed REL byte, a
non-disc file, the update partition folder, a GameCube game folder.

## PAL and Japan

Setup recompiles the European and Japanese discs with their own tables (`config/RUUP01`,
`config/RUUJ01`; PAL 41010 functions, the same 187 RELs). Checked 2026-10-27 from a fresh
APPDATA with `PAL DATA` and `JAPAN DATA`: module built (69 s PAL, 205 s Japan with other builds
on the PC), wrist-strap screen in the disc's language, title ("Let's Go to the City" / 街へいこうよ
どうぶつの森, "Press A"), and with scripted A presses into the new-game bus: Rover's dialogue up to
the "Start fresh! / Move from DS!" choice (English / Japanese text), no DVD, HLE or REL errors.
Version-specific runtime state: the disc header and title id (`RUUP`/`RUUJ`) follow the running
version (`hle_os.cpp`, `ios_devices.cpp`), SYSCONF/setting.txt give a European or Japanese console
(`sysconf.cpp`: Japan = country 1, `AREA=JPN`, `GAME=JP`, language Japanese only), PAL = VI_PAL.

What is still USA- or LM-shaped (not fixed here, none of it stops the boot):
- `hle_vi.cpp` keeps LM's European-game code: `frameStamp()` reads LM PAL's frame-pacer count at
  0x80390864 for City Folk PAL too (unrelated memory there; out-of-range values fall back to 2
  fields per frame, an in-range one would mistime interpolation), `presentOnCopy()` is forced for
  PAL (City Folk switches to presenting at GXCopyDisp on every version anyway), and
  `HLE(palfn_80007844)` is LM's (never bound in City Folk). Owner: VI/interpolation.
- The European game runs at 60 Hz (EuRGB60, F1 > Game > Console, `[system] eurgb60`, default on)
  or 50 Hz with it off; the VI rate follows the TV mode the game configures (branch render-3, docs/WII.md
  "Rendering status (branch render-3)"): 59.5 presents/s by default, 50 retraces/s with it off.
- The European game runs at 50 Hz unless the console's EuRGB60 is on (`[system] eurgb60`, default
  off, as a new European Wii): 49 presents/s measured vs 59 for USA/Japan. A default of 60 Hz for
  PAL may be what players expect.
- Debug-only LM addresses (the message-queue trace filter and the watchdog's queue dump at LM's
  0x803A3608) were removed on branch parity-1.

## Version-aware code

Host code (src/game/, the City Folk hooks in src/recomp/) is written against the USA game
(RUUE01 rev 1) and runs on all four versions through two mechanisms (branch parity-1, after LM's
`version.h`):

- **Hooks by USA name, `all` tag** (`tools/recomp/hooks.txt`): `fn_8016D40C all` hooks the USA
  function and, on the other versions, the function `config/<id>/us_map.txt` matches to it; the
  recompiler renames that function to the USA name in the version's build (a function of the
  version that already had the name gets a `_<id>` suffix), so one `HLE(fn_8016D40C)` and
  `callOriginal(c, "fn_8016D40C")` serve every version. REL functions: `<module>::fn_<id>_<USA
  offset> all` (rels_us_map.txt). Patches: `patch <id> <USA address> <word> all`, mapped into the
  hash-matched function at the same offset. A build fails when an `all` entry matches nothing.
  The inputs (`--us-map`/`--rel-us-map` of cfrecompile, embedded `kUsMap_<id>`/`kRelUsMap_<id>`
  for setup) are part of the module key. Entries without `all` keep the old meaning (a `pal` /
  `jp` / `kr` line names that version's own function). Named SDK/NW4R/DWC functions already have
  the same name everywhere (every HLE / hook name checked in all four symbols.txt).
- **`VA(0x...)` for USA guest addresses** (`src/recomp/version.h`): functions host code calls,
  return addresses it compares, static objects it reads. `tools/recomp/gen_version_map.py` scans
  src/ for `VA(...)` and writes `src/recomp/version_addrs.inc` (USA, PAL, Japan, Korea): code
  addresses through the matched function + offset (hash match, or the same instruction at that
  offset), data addresses decoded from every matched place the USA code builds them (lis+addi /
  load / store, r13/r2 small data; the vote is printed). It exits non-zero on an address it cannot
  map; re-run it after adding a `VA(...)`. `VOFF(us, pal, jp, kr)` is there for struct offsets
  that differ (none so far).
- **Struct offsets are checked, not mapped** (the matcher masks load/store displacements):
  `gen_version_map.py diff <USA functions...>` lists the instructions of matched functions that
  differ other than in addresses, `reldiff <module> <offsets...>` the same for REL functions,
  `field <function> <offset>...` the instructions using one displacement. Checked identical in all
  four apart from addresses: the grass manager and tile functions, the player-data/tan/palette
  functions, d_a_playerNP's skin/face/setup/step functions, the calendar/weather lookups, the save
  and WiiConnect24 entry points, Random, the surround call site, NW4R snd (Voice/Channel/
  SeqTrack/StrmPlayer/WsdPlayer callbacks, InitParam, AttachSoundPlayer, GetBasicPlayer,
  SetupSoundPlayer, __AXGetPBs), lyt Pane::Draw/LoadMtx/CalculateMtx, g3d SetCameraProjMtx, the
  online-system entry points and most DWC functions. One difference found: an address operand in
  fn_800A7F9C (Japan/Korea; not a field the port reads).
- Dev builds of the other versions: `toolsuild.ps1 -Game RUUP01|RUUJ01|RUUK01 -Disc "<that
  disc>"` (build folder `build/windows-clang-<id>`), runs with `tools
outes
un.ps1 -Build
  windows-clang-<id> -Disc ...` (`-Settings "[audio]|output=3"` writes settings read at boot).
- **The USA save loads on every version**: the `user-town` fixture with its title folder renamed
  (`52555545` -> `52555550` / `5255554a` / `5255554b`, passed as `CF_FIXTURE=<folder>
and`) is
  "Preparing saved data" -> the town on PAL, Japan and Korea (the clock shows 27/10 on PAL, the
  Korean title shows the saved town). Continue routes per version: Korea's choice boxes sit further
  right ("좋아!" at `@0.44/0.14`, the player's name at `@0.56/0.33`); PAL and Japan take the USA
  positions.

### Per-version status (2026-11-02, branch parity-1)

One run per version: saved town (above), CF_RNG_SEED, south hemisphere, fake weather, skin tone
forced to 8 at frame 5300, grass wear x10 while walking/running, forced save at 6200, CF_REGION=all,
discrete 5.1 output, CF_AUDIO_LOG, CF_LYT_LOG (logs and shots in build/runs/feat-<ver>).

| Feature (how checked) | USA | PAL | Japan | Korea |
|---|---|---|---|---|
| Boot, title, continue into the saved town | yes | yes (50 Hz) | yes | yes |
| Hemisphere (`[cal]` lookups moved to April) | yes | yes | yes | yes |
| Real-time weather hook (`[weather]` fake answers; rain, since April is not snow season) | yes | yes | yes | yes |
| Skin tone (palettes read, player rebound to tone 8, shots) | yes | yes | yes | yes |
| Grass wear (`[grass]` step wear on the player's tiles) | yes | yes | yes | yes |
| Save path (CF_FORCE_SAVE: write request -> result 1, /tmp safe copy) | yes | yes | yes | yes |
| CF_RNG_SEED (boot seed at the mapped return address) | yes | yes | yes | yes |
| Surround: patch 30 keeps DPL2 (`AXSetMode(2)`) | yes | yes | yes | yes |
| Volume sliders: voices classified by sound player | yes | yes | yes | yes |
| HUD anchoring: the clock / memo / camera panes found by name | yes | yes | yes | yes |
| Region content: events of other regions (`[region] event`) | yes | yes | yes | yes |
| Region getter per call site (Nook's stock, letters) | not reached | not reached | not reached | not reached |
| Interpolation tags, Wii Remote HLE, NAND/IOS, DWC HLE | by SDK/NW4R name, same code (pointer + Nunchuk routes drove all four) | | | |
| Online test actions (CF_ONLINE_TEST steps, state logs) | VA, not run here | VA, not run | VA, not run | VA, not run |

Not checked here: a new game into town on PAL/Japan/Korea (the town-name / name keyboards differ:
JP kana, KR Hangul; the PAL route's language choice is not scripted), the
New Leaf grass mode and the real Open-Meteo lookup per version (same code paths as above), online
play between versions. Nothing found that cannot work on a version.

## Korea

The Korean disc (`RUUK01` rev 1, "Tauneuro Nolleogayo: Dongmurui Sup", 2010) is the fourth row.
Its tables are `config/RUUK01` (docs/DISCOVERY.md "Korea (RUUK01) against Japan and USA"; DOL
24251 functions, the same 187 RELs). Checked 2026-10-27 from a fresh APPDATA with the user's RVZ
(`--setup-auto --disc <rvz>`), and `--check-disc` with the RVZ, the extracted `KOREA DATA/DATA`
and its parent folder: module built, Korean wrist-strap screen, Korean title (타운으로 놀러가요
동물의 숲, (c)2008-2010), and with scripted A presses Rover's bus dialogue in Korean up to the
"처음부터 시작! / DS에서 이사!" (start fresh / move from DS) choice, no DVD, HLE or REL errors
(game frame ~7000, 60 fps). Console: SYSCONF country 136 (South Korea), language Korean (the
only console language offered), setting.txt `AREA=KOR`, `MODEL=RVL-001(KOR)`, `CODE=LKM`,
`GAME=KR`, NTSC; title id 00010000-5255554B (`RUUK`, NAND `/title/00010000/5255554b`). The game's
region getter returns 3 and its language index is always 9 (Korean) on this disc. Korea also
uses IOS 48 (the other three IOS 38; the TMD's IOS is not emulated, nothing depends on it).
