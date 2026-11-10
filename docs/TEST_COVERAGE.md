# Test coverage before the first release

Each test branch adds its own section (what was run, how, the result, what was fixed). Scratch
folders only (APPDATA, NAND, card, cache copies), muted, off screen, physical controllers hidden.

## Other versions and online regression (branch test-regions, 2026-11-09)

Builds: dev builds of all four versions from main d8f7225 + this branch (`tools\build.ps1 [-Game
RUUP01|RUUJ01|RUUK01]`, `-j6`). Saves: copies of `user-town-clean` with the title folder (and its
`~attributes.txt` paths) renamed to `52555550` / `5255554a` / `5255554b` (docs/SETUP.md "The USA
save loads on every version"). Clock 2026-10-27 10:00 (day runs) / 21:30 (night runs). The PC was
shared with other agents' builds and game instances throughout (guest 25-40 fps), so every route
is anchored to module links / guest frames.

### Static check

- Every `0x80xxxxxx` / `0x81xxxxxx` literal in src/ outside `VA()` (`u` suffix included) listed and
  read: low-memory globals and memory ranges only, apart from
  - `cf_tags.cpp` ef `DrawQuad` per-particle scope: the caller range `0x80292388..0x802930BC` was
    a raw USA range, so on PAL/JP/KR every billboard particle fell back to the emitter scope
    (particles paired by order at 120 fps). **Fixed**: `VA()` on both ends (PAL 802921D8..80292F0C,
    JP 80292404..80293138, KR 80293944..80294678).
  - `distributions.cpp` (Nintendo events): its two game hooks (`fn_8010F194` the game keeps a
    Nintendo letter, `fn_80104520` Pete hands it over) were USA-only in hooks.txt, and live
    delivery, the town WiiConnect24 switch and the job poll used raw USA addresses behind a
    `running == USA` test; the F1 options "Turn WiiConnect24 on for my town" / "Deliver while
    playing" were hidden on the other versions, and a stored package counted as received even if
    the game never kept it. **Fixed**: hooks tagged `all` (both functions identical apart from
    addresses in all four, `gen_version_map.py diff`), addresses through `VA()`, the USA test and
    the store-time counting removed, options shown on every version.
  - Research-only probes (`CF_MAIL_PROBE`: Pelly hooks without `all`, a USA scene-name dump) left
    as they are.
- `gen_version_map.py diff` / `reldiff` of every function hooked by today's features (shadow
  casters' culling, pointer range, Mii icon queue, Nook stock and save starters, value function,
  wipe, warps, MP poll, Wii Speak echo suppressor, weather, job starter, furniture helpers,
  d_a_strNP light controller, field-object draw, stage bracket, player limit test): identical
  apart from addresses in PAL / JP / KR. `version_addrs.inc` regenerated (77 addresses, none
  unmapped).

### Per-version runs

Day run (`day.txt`: continue, warp house1 + candle, warp Nook's Cranny, warp town plaza + walk;
120 fps, Enhanced lighting on, Nook item C0AC, Nintendo events "Only when I choose" with
`CF_DISTRIB_NOW=creepy_clock@150,creepy_clock@330`, Mii editor `random / name=Robo / save`,
`CF_FORCE_SAVE`, `CF_JUDDER_LOG`). Night run (`night.txt`: 21:30, 21:9, Enhanced lighting: city
plaza walk, villager house 1, back to the town). PAL language runs: continue with
`[system] language=2 eurgb60=false` and `language=5 eurgb60=true`.

| Feature (how checked) | USA | PAL | Japan | Korea |
|---|---|---|---|---|
| Boot, title, continue into the saved town | yes | yes | yes | yes |
| Mii maker: editor Mii saved to RFL_DB.dat, handed to RFL (`[mii] database handed ... 1 Miis`), preview drawn by RFLMakeIcon | yes | yes | yes | yes |
| Unused items at Nook's: bench in slot 18 of the stock, on the shop's grid (9,8), restored before every save and back after (save starters), item names written per language column | yes | yes | yes | yes |
| Receive now once per town: live delivery, `fn_8010F194` "the game took Nintendo letter 4D000001", second press refused | yes | yes (fixed) | yes (fixed) | yes (fixed) |
| Town WiiConnect24 switched on by the Nintendo events option | yes | yes (fixed) | yes (fixed) | yes (fixed) |
| Furniture lights: house actors found, candle analysed as a flickering fire light | yes | yes | yes | yes |
| City night lights: daSTR_c controllers, lit materials -> lights (`[citylight]` 50 of ~145 lit materials drawn in the plaza, street lamps on) | yes | yes | yes | yes |
| Off-screen shadow casters (`[casters]` objects kept as casters, sun direction) | yes | yes | yes | yes |
| Town interpolation at 120 (judder detector: blend defects) | 0 | 0 | 0 | 0 |
| Circle wipe at 21:9 (`[ws] circle wipe widened`, the circle and the black frame cover the full width in the shots) | yes | yes | yes | yes |
| Save (`CF_FORCE_SAVE` result 1) | yes | yes | yes | yes |
| PAL: German at 50 Hz (TV format 1), Italian at 60 Hz (TV format 5): strap screen, title, Rover, HUD in that language | - | yes | - | - |
| Move in from Wild World | not re-run | not re-run | not re-run | not re-run |
| Face seams (g3d CP writes followed while capturing) | generic GX code, no game address | | | |

Notes:
- Seven `ARCOpen(): file '%s' was not found` warnings when the game opens a Nintendo package, on
  USA as on the others: the game also looks for files the archive packages do not carry
  (`npc%d.bin` and others); the letter is kept and delivered.
- Move in: not re-run here. Rover only asks "You are...?" (where "I'm new!" leads to the DS
  transfer) on another day than the save's last play, and with the PC's load the scripted pointer
  never caught that box in time (runs on PAL/JP/KR answered "Rob64."). Last verified on all four
  by branch move-in-2 (2026-11-03); its code (cf_move_in.cpp, hle_mp.cpp, ww_save.cpp) is
  unchanged since, and its hooks (`fn_800E3EE4`, `fn_80085894`) are identical apart from addresses
  in all four.
- The PAL language runs also confirm the item-name column order the unused-items names use
  (DE "Umzugskartons", IT "scatoloni" in their columns).
- The judder detector's policy "jump" counts (a tagged draw shown at the game frame) are small
  and appear on USA too; blend defects 0 on all four.

### Online regression (USA, two instances, copies of test_builds/online-letter-test-2)

Host `Frunch-rob64`, visitor `test-tester` (own room / letters-link ports 29960/29961, 29963/29964,
USB keyboard on, `CF_ONLINE_LOG`), routes `tools/routes/visit_host2.txt` / `visit_guest2.txt` (new: host B
through Copper's follow-up question and runs out of the gatehouse; visitor picks the town by pointer
and later goes home through the gate) and `letter_other_town_*.txt` (their save step now uses the
HUD's Save button at the top right, as save_quit.txt; the old bottom-right click never opened it).
The letters link needs each friend's address: with ports other than the profiles' own, the copies'
`friends.txt` `ep` lines were pointed at the other instance (otherwise the room exchanges them).

| Check | Result |
|---|---|
| Visit: room, login, hosting (status 6), the visitor picks Frunch, matched, the visit protocol, both players in Frunch | yes |
| Keyboard chat: `CF_KBD_TYPE` "Hello rob" / "Hi again" as bubbles over the visitor on both screens | yes |
| Voice: host `CF_MIC_WAV` speech, "Now starting Mic Chat", `_VCT` packets both logged, visitor's recorded output vs the speech: envelope correlation 0.72-0.83 while in the visit (0.2-0.3 before / after) | yes |
| Visitor goes home through Copper: visit over, saves (result 1) on both sides, the visitor back in its own town | yes |
| Letters both ways (each town writes to the other, Pelly "Other town.", HUD Save and quit: `[so] SOStartup outside an online room ... at once`, letter queued, save result 1; next start of both: "delivered over the letters link" / "received" both ways, then "taken by the game" and the mailbox's new-mail flag) | yes |
| Nintendo events Receive now once (USA day run above, and the profile copy) | yes |

Nothing regressed in the online paths after the merges; the first attempts failed only on route
timing (the host's extra A presses reopened Copper's menu, "Please halt conversations.").

## F1 options, setup and installation of the shipped program (branch test-options, 2026-11-09)

Full table: docs/OPTIONS_AUDIT.md. The shipped program (CityFolkRecomp) from main d8f7225 + this
branch (main af9103f..e7b12d1 merged during the audit), fresh scratch APPDATA installs
(`--setup-auto`) and installed copies (`cmake --install`); the PC was shared with other agents'
builds (module builds 2-7 min instead of ~1).

| Area | Result |
|---|---|
| ID conflict sweep of every page (title and town, after merging main) | 0 conflicts |
| Every settings.ini key round trip (extensions included) | **fixed**: `[region]` never loaded back (`key = value` not trimmed); 108 dead LM keys removed |
| Live changes in town (aspect, HUD, FOV, CRT, Enhanced world, Show FPS, Restore defaults) | OK; **fixed** fixed aspect ratios squeezed instead of letterboxed, mouse pointer mapping in fitted pictures |
| Tooltips | **fixed** LM text on the Rendering tab, quit dialog, Real Wii Remotes help; dead shadow options removed |
| Controls with a virtual pad: hot-plug, defaults, rebinding capture + restart, stick pointer, pointer timeout | OK |
| Fresh install, four discs (folder, whole-disc folder, RVZ), each version from one install | OK |
| Modified disc copy | **fixed**: new disc file check (warning, Play anyway, probable cause on fatal errors) |
| Recovery prompt | **fixed**: "Put saves aside" acts on the NAND saves |
| Portable mode, data folder move both ways, long paths (230 chars) | OK; Windows cannot start the program 236+ characters deep |
| Non-ASCII install and data folders | **fixed**: crash at the first NAND access, run state not written |
| Updater (no network, GitHub without a release) | OK, message reworded |
| Package contents | **fixed**: third-party licence texts; open: Dawn / DXC / nod texts |
