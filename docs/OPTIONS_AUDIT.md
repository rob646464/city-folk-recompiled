# Options and setup audit (November 2026, before the first release)

Branch test-options. Every F1 tab, sub-tab and widget of the **shipped program** (CityFolkRecomp,
not only the cfrecomp dev build) was looked at, and the install / setup flow was run end to end
from scratch APPDATA folders, with installed copies (`cmake --install`) in portable, deep and
non-ASCII folders. The game was the USA disc unless a row says otherwise; town runs used a copy of
the `user-town-clean` fixture. Every run was muted, off screen, with the physical controllers hidden
(a scripted virtual pad where a pad was the test). Evidence (logs, frame dumps, window captures by
PrintWindow) stayed in the agent's scratch folder.

How options were driven: `CF_MENU_CLICK` (the menu's own widgets, ImGui item activation),
`CF_SETTINGS_AT` (sliders and combos the click aid cannot reach), `CF_MENU_ID_CHECK=exit` (the ID
conflict sweep, with a window capture of every page and scroll position), `CF_VIRTUAL_PAD` +
`CF_CONTROLS_CAPTURE` (pad assignment, inputs, rebinding), settings.ini files written by hand.

Status: **OK** works as described; **fixed** a problem was found and fixed on this branch (commit
subject in the row); **note** works, with a remark; **owner** the feature's own branch verified it
in depth (named), here only sweep / persistence / tooltip.

## Every option: persistence and IDs

- **Round trip of every settings.ini key**: a settings.ini with every one of the 330 keys changed to
  another valid value (all sections, the extensions' too: controls, wiimote, wiimote_hid, system,
  region, calendar, weather, enhanced_world, texture_packs; shader-variant keys kept to avoid a cold
  compile) loaded, the menu changed a value twice (dirty), closed: **fixed** - the 22 keys of
  `[region]` came back at their defaults. The section was written as `key = value` and the loader
  did not trim, so "Content from other regions" was lost at every restart (and never applied to a
  visitor in online visits). Now every key loads and saves back unchanged ("Settings: Content from
  other regions choices survive a restart").
- **Dead settings**: 108 Luigi's Mansion keys (ghost/world lights, Hidden Mansion, GBH, unused
  content, debug, silly, difficulty, three flashlight-shadow flags) were written into every
  player's settings.ini but read by nothing: **fixed**, removed (old files still load).
- **ID conflict sweep** (`CF_MENU_ID_CHECK=exit`, all pages incl. scrolled ones, title screen and in
  town, before and after merging main's Save data section): 0 conflicts. The sweep's page list
  named an "Enhancements > Silly" sub-tab that does not exist in City Folk: **fixed**. Not reached
  by the sweep: Online's collapsed headers (Rooms on this network, Direct connect, Friends) and the
  Mii editor's part tabs (Face ... Mole), which open only with a Mii selected.
- **Developer-only content**: the shipped build has no Enhancements > Debug tab (warps), no warp
  hooks (`cf_warp.cpp` behind `CF_DEV_TOOLS`), no "Forget received" (Nintendo events) and no region
  test controls; the sweep's page list for the shipped build ends at Unused content. The `CF_*` test
  aids (environment variables) stay in the shipped program, as in LM.

## Visuals

| Option | Default | Checked how | Result |
|---|---|---|---|
| Menu size | Auto | sweep at Auto (100 % at 720p); code is LM's ui_scale | OK |
| Show "Press F1" hint | on | fresh install: `[ui] "Press F1" hint shown` | OK |
| Frame rate, Match display, Unlimited, VRR | Original (60), off, off, Auto | persistence; stored value 60 (the slider clamps to the game's rate) | OK (interpolation owner branches) |
| Aspect ratio | Match window | fixed ratios live via `CF_SETTINGS_AT` (21:9, 32:9, 16:9, Match window) with window captures | **fixed**: a fixed ratio wider than 16:9 in a window of another shape was stretched to the window (squeezed picture, no black bars as the tooltip says); now fitted at that ratio with bars ("Widescreen: fixed ratios get black bars..."). **fixed** tooltip: Original 4:3 only takes effect at the next start (the game reads the console's TV shape at boot), now said with a "(next launch)" note. Note: the wrist-strap screen's 2D layout shows the game's blue clear colour beside it at 21:9 (as in any ultrawide window) |
| Mouse pointer in a fitted picture | - | code: Aurora centres a fitted frame (calculate_present_viewport) | **fixed**: the mouse was mapped over the whole window, so with bars (Original 4:3, a fixed ratio, a 16:10 window / Steam Deck at default settings) the game's pointer drifted away from the cursor. Now mapped through the picture rectangle (not testable without a real mouse here) |
| Keep HUD at 16:9 | on | clicked live at 21:9 (`[menu-click]`), settings saved | OK (anchoring verified on branch ultrawide-pointer) |
| Field of view | 1.00 | 1.4 live via `CF_SETTINGS_AT` in town, frame dumps | note: works; at 1.4 the town's sky backdrop ends before the top corners (black). Tooltip says so now |
| Render resolution | Match window | persistence (3x); `CF_SETTINGS_AT` applies it like the slider (branch enh-world) | OK |
| Fullscreen | off | not run (would take over the screen) | not tested |
| VSync | on | persistence; "(next launch)" | OK |
| Anti-aliasing | Off | settings.ini `msaa=4`: `[INFO: aurora::gpu] MSAA 4x` at the next start, then a full shader preparation (new pipeline variants, ~25-60 s here) | OK |
| CRT filter (preset, scanlines, signal, advanced) | off | on / off live via `CF_SETTINGS_AT` in town, window captures; all CRT keys round trip | OK |
| Rendering: Display copy filter | **Auto** (since 2026-11-11, user decision) | the console's vertical deflicker blur only while rendering at the game's own resolution (render height within 10% of the EFB's), off above it; On / Off to force; the game's display gamma in every mode; the CRT filter turns the blur on | `display_copy_filter_mode` 0/1/2 replaces the bool `display_copy_filter` (old files get Auto) |
| Rendering: Integer-accurate TEV, Early depth, Texture copy filter, EFB copies to RAM, Snapshot vertex data | on | persistence; `CF_GX_AUDIT` / `CF_COPY_LOG` in title and town | **fixed** tooltips: all described Luigi's Mansion (Luigi's mouth / Poltergust hose flicker, Game Boy Horror, Mario painting, GameCube). Note: the display copy uses the game's vertical filter (`GXSetCopyFilter vf=1`), so that toggle shows; no GXCopyTex in the title or the town (copy filter / copies to RAM matter for the few copies, e.g. Mii pictures) |
| Rendering: EFB color depth, 6-bit blending, Smooth color banding | on, on, off | `CF_GX_AUDIT`: the game never calls GXSetPixelFmt (RGB8 EFB) in title, bus, town | note: no effect in City Folk; tooltip says so now |
| Rendering: Shadow map at render resolution, Widescreen shadows, Shadows at any distance | (were on) | code: nothing in City Folk reads them (LM's flashlight / lightning shadow map) | **fixed**: removed from the menu and settings |
| Enhanced lighting (every option) | off | persistence (master kept off: a cold compile); sweep | owner (branches enh-light, enh-light-2/3, stutter-ao, furniture-lights, shadow-casters) |
| Enhanced world | off | clicked on live in town: falling leaves (Oct 27) in the frame dumps; all keys round trip | OK |
| Texture packs | "Use texture packs" on | sweep; persistence | note: on by default, but with no pack installed nothing changes; the `texture_packs` folder is created at start (LM creates nothing until used). Owner branch texture-packs |

## Sound

| Option | Default | Checked how | Result |
|---|---|---|---|
| Master / music / effects / voices / Wii Remote speaker volume | 100 % | persistence; tooltips read | owner (audio-2: per-voice slider checks with `CF_SETTINGS_AT`). **fixed**: voices and Remote speaker volume were not clamped when read from settings.ini |
| Output, Swap left/right, High-quality resampler, Reverb, Voice filters | Stereo, off, on, on, on | persistence | owner (ax-audio, audio-2) |

## Controls

| Option | Default | Checked how | Result |
|---|---|---|---|
| Ports: device per Wii Remote, + Keyboard | Auto, keyboard on port 1 | `CF_VIRTUAL_PAD` attached an Xbox 360 pad: `[controls] port 1: Virtual test pad ... + keyboard` | OK (hot-plug onto Auto) |
| Wii Remotes: connected / extension / pointer source | Auto, Nunchuk, Mouse + right stick (1), Right stick (2-4) | persistence of every combination (on/off, none, stick+gyro) | OK |
| Bindings: capture, persistence, defaults | keyboard + per-model pad profiles | `CF_CONTROLS_CAPTURE=pad:a:2` + the virtual pad's North during the capture: `a.2=button:north` saved; after a restart North gives A (+ 2, its default: shown as a conflict in red in the menu) | OK |
| Pad defaults | (branch default-binds) Classic Controller layout by position: A right face button + right shoulder, B bottom + right trigger, + top, - left, 1 Back, 2 Start, HOME Guide, Z left trigger, Toggle pointer left shoulder, Recenter right stick press, D-pad, sticks | virtual pad through the whole layout in the saved town (tools/routes/pad_layout.txt; docs/CONTROLS.md "Default gamepad layout"); 1.x profiles: old defaults migrated, customized kept | OK (was: A south, B east + right trigger, 1 west, 2 north, C LB, Z LT) |
| Pointer from the right stick | on | virtual pad right stick: pointer back on, moved to the edge | OK |
| Pointer timeout | 3 s | `[wiimote] remote 1: pointer timed out (hidden)` 3 s after the last move, `back` on the next move | OK |
| Snap pointer to menu items | Auto | `CF_NAV_LOG` lists Rover's choice-box items in a scripted continue | OK (full rules checked by menu-nav / menu-nav-2: 135 item sets) |
| Hide the mouse cursor, Absolute stick, Pointer speed, Gyro, Sensor bar distance | on, off, 1.4, 3.0, 2.0 m | persistence | not run with a real mouse / gyro pad |
| Sticks, triggers & rumble (dead zone, sensitivity, invert, trigger points, rumble) | per profile | persistence of the pad profile keys | OK (no rumble test, user rule) |
| Real Wii Remotes | Emulated | sweep | **fixed** help text: the console settings are in F1 > Game > Console (it said Enhancements > General > Console). Owner hid-remotes (never run with a real Remote) |
| DSU | off | persistence | owner |

## Enhancements

| Option | Default | Checked how | Result |
|---|---|---|---|
| Show FPS | off | clicked live in town: "60 fps" in the window capture | OK |
| Pause when window loses focus | on | - | not testable (automated runs never pause by design) |
| Grass wear, Regrow all grass now | Original | persistence; sweep at the title | **fixed**: the Regrow button did nothing outside a loaded town; now greyed out there with the reason |
| Skin tone | Game | sweep, tooltip | **fixed**: buttons 2-7 said "Fixed tone 3 of 8 ()" |
| Hemisphere, Real-time weather | Northern, off | persistence | owner (calendar-1); weather left off (network) |
| Move in from Wild World | off | code | **fixed**: a save file with a non-ASCII path could not be opened (UTF-8 path read as ANSI). Owner move-in, move-in-2/3 |
| Content from other regions | all off | round trip | **fixed**: lost at every restart (above) |
| Unused content (Nook's item) | none | persistence (C0AC) | owner unused-items |

## Game

| Option | Default | Checked how | Result |
|---|---|---|---|
| Installed discs, version to play | - | four discs installed; each version started from that install by its `base=` (title in its language) | OK |
| Add or change discs... | - | not clicked (relaunches into setup; setup itself tested below) | - |
| Console: language, sensor bar, pointer sensitivity, EuRGB60, clock offset | Default, below, 3, on, 0 | persistence | **fixed**: sensor bar and clock offset (days) had no tooltip. Owner nand-sys |
| Data folder: make portable / move back | system folder | clicked both ways in an installed copy (move, restart, game runs): everything moved, the old place emptied | OK |
| Updates: check at startup, Check for updates | on | installed copy: startup check and the F1 button against api.github.com (HTTP 404) and with no network | OK. **fixed** message: "No published release was found yet (none is public)" (the old "not public yet" would be wrong for a public repository without a release) |
| Save data (import / export) | - | sweep only (merged from main during the audit) | owner save-import |
| Restore all defaults | - | clicked in town after changing aspect, FOV, HUD, CRT, Enhanced world, FPS: aspect back to Match window at once, settings.ini written | OK |
| Quit game | - | text | **fixed**: asked "Quit Luigi's Mansion?" |

## Miis, Online

Sweep and persistence only (owner branches mii-maker, online-1, online-voice, distrib-1,
distrib-once). Nintendo events' "Receive now" is greyed out while delivery is Off (as intended).

## Install and setup (shipped program)

| What | Checked how | Result |
|---|---|---|
| Fresh install | empty scratch APPDATA, `--setup-auto --disc "US DATA\DATA"` | OK: module built (422 s here with other agents' builds running; ~60 s on a quiet PC), title reached, install.ini written |
| First-run defaults | settings.ini written by the menu after a fresh install (330 keys) | OK, every option off / original as documented. Notes: texture packs master on (no packs = no effect); `[enhanced_lighting]` keeps LM's flashlight / volumetric keys (read by the shared enhanced-lighting code, no menu) |
| USA + PAL + Japan + Korea | `--usa-disc` (folder), `--pal-disc "PAL DATA"` (Dolphin whole-disc folder), `--jpn-disc` (folder), `--kor-disc` (the RVZ) | OK: three more modules built (124-153 s each), each version boots from that install |
| Disc check with a modified copy | copy of `US DATA/DATA` with 4 KB of `Layout/title_us.arc` changed | **fixed**: accepted silently (only main.dol and the RELs were checked), then `OSPanic TPL.c:25: invalid version number for texture palette` at the title. Now every game file is checked once per disc against the retail tables: warning on the setup screen, "Play anyway" remembered, warning in F1 > Game, "probable cause" in the fatal log and the recovery reason (docs/SETUP.md "Disc file check"). All four clean discs pass (the Korean RVZ against tables made from the extracted folder) |
| Recovery prompt after a crash | the crash above, then a start with `CF_RECOVERY=3` | **fixed**: "Put saves aside" looked for GameCube memory card folders and did nothing; now moves the NAND save folders aside (`nand saves (put aside <time>)`), and the game starts as new |
| Portable mode | installed copy + `portable.txt`, empty APPDATA | OK: settings, install.ini, game module, NAND, caches, imgui.ini, logs all in `portable\`; APPDATA untouched |
| Long paths | portable installed copies 230 / 236 / 240 / 247 characters deep (long path policy on here) | OK at 230 (NAND paths past 260 work). Note: at 236+ Windows itself refuses to start the program (STATUS_NAME_TOO_LONG, no window or message); PowerShell started the 247 one through 8.3 names, where it ran |
| Non-ASCII path | installed copy in `Tëst 日本 ünï\City Folk`, APPDATA `Äppdätä 日本` | **fixed**: aborted at the first NAND access (path::string() throws outside the code page; also in NAND errors, the WiiConnect24 temporary file, the Nintendo events page, Mii import messages, recovery backups), and last_run_state.ini was never written there. Now boots to the title, run state written |
| Updater | above | OK |
| Package (`cmake --install`) | file list | **fixed**: only LLVM's licence was included; THIRD-PARTY.txt + licenses/ (Aurora, ImGui, SDL3, libpng, zlib, FreeType, fmt, xxHash, Zstandard, Abseil, Tracy) now. Open: licence texts for the prebuilt Dawn, DXC and nod DLLs (no file in their downloads; `packaging/licenses/<name>.txt` installs them once added). No game data in the package (the embedded tables are addresses, names and checksums) |

## Not covered here

Fullscreen, focus-loss pause, a real mouse / gyro / Wii Remote / rumble, Enhanced lighting
toggled live (cold shader compiles), online play, the Mii editor's part tabs, Add or change discs
from the menu, macOS / Linux packages.
