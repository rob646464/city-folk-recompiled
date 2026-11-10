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

## Gameplay (branch test-gameplay)

USA disc, `cfrecomp` developer build, fixtures `user-town-clean` / `user-town2`, scratch
APPDATA / NAND / card / cache copies, muted, off screen, physical controllers hidden. Test runs
are scripted (`tools/routes/run.ps1`, route files with `WARP=` / `TP=`, `CF_GIVE_ITEM`,
`CF_CLOCK_START`, `CF_CLOCK_JUMP`), not played by hand.

### Before this branch (from CLAUDE.md status lines, docs, git log)

| System | State before test-gameplay | Where |
| --- | --- | --- |
| Boot, title, new game to the town gate, continue a saved town | verified | play-1, routes `new_game.txt`, `continue*.txt` |
| Walking, running, HUD map, pockets, item submenu, toolbar | verified | play-1, menu-nav, menu-nav-2 |
| HUD "Save and quit", first save, reload; forced save | verified | play-1, routes-3 (`save_quit.txt`), save-crash |
| Mr. Resetti's first-reload greeting | seen (fixture `user-town`) | CLAUDE.md "Test runs" |
| Resetti after quitting without saving | never run | |
| Saving in bed | never run | |
| Writing a letter (stationery -> address book -> Confirm) | verified up to Confirm (letter to a villager on weathered paper) | unused-items |
| Sending letters through Pelly, letters to other towns | verified (WiiConnect24 / online path) | online-1 |
| Villagers' replies to letters | never run | |
| Villager conversations and requests (errands, item swaps) | never run (only dialogue choices of Rover / Tortimer / shopkeepers) | |
| Villager moving in / out | never run (only Move In from Wild World, Pelly's greeting) | move-in |
| Nook's: buying from the shelf, selling to Nook, daily stock | verified (unused-items sample) | docs/FEATURES.md "Unused items at Nook's" |
| Able Sisters: buying | never run (room rendered) | routes-2/4 |
| Catalog: ordering, delivery by mail | never run (catalog screen opened by `CF_OPEN_MENU` only) | move-in-3 |
| Tools: net, fishing rod, shovel, axe, watering can, slingshot | never run | |
| Catching a bug / a fish, donating to Blathers, fossil assessment | never run (museum rooms rendered) | warps, routes-4 |
| Digging fossils / buried items, hitting rocks (money rock) | never run | |
| Planting trees / flowers, growth over days | never run (grass regrowth only) | features-1 |
| Balloons (slingshot) | never run | |
| Rain, snow, seasons | verified (render, real-time weather, hemisphere lookups) | calendar-1, enh-light, enh-world |
| Snowmen | never run | |
| Tortimer's holidays (region events, Explorer's Day) | verified | region-1 |
| Fireworks show (sky effect) | seen (enhanced lighting of the bursts) | enh-light-2 |
| Halloween (Jack), Toy Day (Jingle), New Year's countdown, Fishing Tourney (Chip), Bug-Off (Nat), Harvest Festival, Flea Market | never run | |
| City: every building entered and rendered (GracieGrace, HRA, auction, Club LOL, Shampoodle, Katrina, Redd's, Resetti center) | verified (warps + Dolphin comparison) | warps, routes-4 |
| City: Shampoodle Mii mask | verified | mii-maker |
| GracieGrace buying, the auction, Dr. Shrunk's emotions, K.K. Slider on Saturday night | never run | |
| HRA evaluation | never run | |
| Nintendo event letters (Pete) | verified | distrib-1 |
| Online visit, Wii Speak, keyboard chat | verified | online-1, online-voice |

### Tested on this branch (2026-11-09)

How: `user-town-clean` (town Frunch, player rob64, 2026-10-27 10:00 unless a date is given),
continue fast-forwarded (`CF_FF=1-4400`), then a scratch route; tools and items put into the
pockets with `CF_GIVE_ITEM` (codes: 0x9000 + item number x 4 from item.bin; net CEA0, fishing
rod CEA4, watering can CEAC, shovel CEB0, axe CE80, slingshot CEEC, sea bass 9BB0, common
butterfly 9960, fossil 9018, painting CD40, cedar sapling 9020, red roses B2A8, 10,000 Bells
91DC), places by `WARP=`, NPCs by `TP=mod<N>:0/30` (actor modules: Blathers 48, Lyle 61, Katrina
59, Jack 64, Jingle 62, Chip 63, Nat 28, Franklin 50, holiday Tortimer 36, villagers outside 22),
dates by `CF_CLOCK_START`. Screenshots every 50-150 frames; every log checked for
`FATAL` / `[hle] MISSING` / `[hang]` (none in any run). The pockets' item menu is
Drop / Carry / (Bury, Plant, Wear, Add to Wallet, ...) / Quit; "Carry" equips a tool.

| System | Tested how | Result | Fix |
| --- | --- | --- | --- |
| Equipping tools | pockets -> tool -> Carry, for all six tools | ok: the tool in hand, the previous one back in its slot | |
| Shovel | Carry, A in front of the house | ok: hole dug | |
| Axe | Carry, A beside a tree | ok: swing | |
| Watering can | Carry, A | ok: watering animation and splash | |
| Fishing rod | Carry, walk to the river by the house, A (cast), wait, A | ok: cast, bobber on the water, recast; nibbles/bites rumble the Remote (`[wpad] motor on`) | |
| Slingshot | Carry, A | ok: aiming view of the sky | |
| Net | Carry, A | ok: swing | |
| Planting | cedar sapling and red roses -> Plant | ok: sapling and roses in the ground | |
| Hitting a rock | shovel, walk to the rock left of the house, A x 10 | ok: swings at the rock; no Bells (not that day's money rock) | |
| Snow | 2027-01-10, `CF_WEATHER_FAKE=snow`, walk from the house to the plaza and around | ok: falling snow, snow-covered ground, houses and trees; no snowballs on the way (no snowman built) | |
| Bells bags | 10,000 Bells -> Add to Wallet | ok: wallet 0 -> 10,000 | |
| Camera / photos | 1 in the field (shutter effect), toolbar Photos icon | **bug: every photo was black** in the Photos window (fixed: the photo of the town in front of the house shows, `[efb] ... readbacks 1 writebacks 1`). The photo is an EFB copy (`fn_802B7A7C` -> GXCopyTex, 640x456 RGB565 at 0x814E02C0) that the CPU reads without a DCInvalidateRange, the port's cue for reading a copy back (`[efb] ... readbacks 0`); with `CF_EFB_READBACK_ALL=1` the photo shows the town. Without a photo: "You have no photos. Close this window and press 1 to take a photo.", SD Card button (no SD card in the port) | `fn_802B7A7C` hooked (cf_game.cpp): its buffer is marked CPU-read before the copy |
| Villager conversation | `TP=mod22#0:0/60`, walk up (STICKUP 14-25 frames: shorter turns do not face the villager), A | ok: Pecan's talk about Halloween and candy; Bluebear's long-absence talk on her own (Harvest Festival run) | test aid `TP=mod<N>` fixed: it matched an unrelated object at 176,0,96 and put the player outside the town |
| Test runs in a batch | several `tools/routes/run.ps1` calls in one PowerShell | **tool bug**: each run inherited the previous runs' `-Env` (CF_FIXTURE, CF_POKE, CF_GIVE_ITEM...), e.g. a run meant to start without a fixture copied one | run.ps1 restores the environment after each run |
| Halloween | 2026-10-31 19:00, plaza | ok: event 21, Jack walking the town at night | |
| Toy Day | 2026-12-24 20:30, `TP` to Jingle, A | ok: event 23, snow, Jingle's talk ("Happy holidays! I'm Jingle...", the gift question) | |
| Fishing (catching) | rod at the pond by the house (2 x 1500 frames, three runs) and at the beach (`WARP=town.beach`, 4 casts x 900 frames), `CF_WPAD_LOG`, `CF_AUTO_A_MOTOR=4` | no bite: the only rumbles are the cast's (7 short pulses + 2 at the splash, at most 3 frames each); fish shadows are in the sea, but none came to a bobber cast straight ahead. New test aid `CF_AUTO_A_MOTOR` presses A on a long rumble (the bite) for a route that casts at a shadow | test aid only |
| New Year's countdown | 2026-12-31 23:58, plaza, past midnight | ok: event 19, countdown board 00:00:04 -> midnight scene, fireworks, "Happy New Year!", plaza lights after | |
| Fishing Tourney | 2026-11-21 (3rd Saturday; Nov 14 has none), plaza | ok: event 14, Chip and the tourney tent | |
| Bug-Off | 2027-06-19 (3rd Saturday), plaza | ok: event 15, Nat and the tent | |
| Fireworks show | 2027-08-01 19:30 (Sunday), plaza | ok: event 16, Tortimer, firework flashes colouring the plaza | |
| Harvest Festival | 2026-11-26, plaza | ok: event 22, tables with food, Tortimer; a villager's long-absence talk | |
| Auction house | `WARP=city.auction`, A | ok: Lloid, "next bidding period ... October 31st at 6 a.m." (no bidding that day) | |
| GracieGrace, Able Sisters | warp, A | ok: Labelle's / Mabel's greetings | |
| Katrina | warp, `TP`, A | ok: her talk, the fortune / charm / no choice, "Your pockets hold less than 100 Bells" | |
| HRA (Lyle) | warp, `TP` to Lyle, A through the first choices | ok: Lyle's introduction, "I'd love to join!", the evaluation rules explained; no score yet (it comes later by letter) | |
| Club LOL | Saturday 20:30, `WARP=city.theatre`, up the carpet to the clerk, A | ok: "Welcome to the Marquee! ... tickets are 800 Bells", Sounds good! -> 800 Bells paid, the newcomers' special performance, "please make your way into the theater"; warping straight into the show room shows the stage with the curtain closed (no show without the ticket) | |
| Wallet | `CF_POKE=5700:90E27D35:01,5700:90E27D36:86,5700:90E27D37:9F` (u32 at 0x90E27D34 with `user-town-clean` and run.ps1's seed; found by diffing memory dumps before / after "Add to Wallet") | ok: 99,999 Bells in the HUD and spent at Club LOL | |
| Bed (house loft) | warp, A at the bed | the bed offers "Swap this for another bed?" (no save at a bed in City Folk; saving is the HUD button, verified before) | |
| Museum (Blathers) | warp, `TP`, A | ok: greeting, menu Donation! / Check a fossil! / Where am I? / Never mind., "Donate what?" pockets screen with the donatable items lit | |
| Museum donation | Donation! -> common butterfly (pointer `@-0.45/0.17`) -> Donate, with `[wiimote] remote1_snap=off` | ok: "I daresay that is a common butterfly. STOP! Do NOT free it!", "Our thanks for the donation", "Do you wish to donate anything else?" Yep -> the screen again. With "Snap pointer to menu items" on Auto (default) the scripted pointer clicks on that screen selected nothing in two runs, and the cursor is not drawn until the pointer moves | branch snap-donate: the route's `STICKUP*4` (walking up for the second talk) landed while the picker was still open, Auto took it as menu navigation and kept the pointer on Quit, so the later clicks went elsewhere; the cursor was missing because the picker opened while the route pointed away (`@off`). Timed from the picker's link, scripted clicks donate with Auto and Off alike. The real bug found there: by D-pad / stick only Quit and Donate were reachable (the slots were left out as "behind" the hit-tested buttons; docs/CONTROLS.md "Screens drawn but not hit-tested"): fixed, `tools/routes/museum_donate.txt` donates by D-pad, `nook_sell.txt` sells |
| Quitting without saving (Resetti) | run 1: continue `user-town-clean`, play ~40 s (and once ~5 min, in and out of the house), quit (no save); run 2: continue that NAND (clock 10:00 again, 10:30, 11:30) | no Resetti in run 2. The session-start save ("Preparing saved data") is committed (`[save] result word 1`, rvforest.dat rewritten) and sets file byte 0x8670 (player 1 + 0x8650) 00 -> 02; Dolphin does exactly the same: Dolphin's own run-1 save is byte-identical to the port's apart from two random words, and Dolphin shows no Resetti either, from its own save or from the port's | none needed (matches Dolphin) |

Every log of these runs (about 60) was free of `FATAL`, `[hle] MISSING` and `[hang]`.

### Open (gameplay)

- ~~Museum screens with "Snap pointer to menu items" on Auto~~: done on branch snap-donate (see
  the museum donation row; docs/CONTROLS.md "Scripted routes and the snap"). Still open there: the
  recycle box, giving an item to a villager, sending an item with a letter (the same pockets
  picker family, not reached by script), a real mouse / controller.
- **Not reached yet**: catching a fish (no nibble at the pond by the house) or a bug, Able
  Sisters / GracieGrace purchases (the scripted walk did not face a display), catalog ordering,
  letters to villagers and their replies, K.K. Slider's show and Dr. Shrunk's emotions (the
  Club LOL ticket and the newcomers' show were reached, not the theatre), the HRA score letter,
  fossil assessment, a villager moving in or out, the auction's bidding days.
- **Resetti**: what makes him come after an unsaved quit was not found; the port and Dolphin agree
  for the sessions above (run 2 of every test started without a fixture copy: check the run log
  for `[nand] fixture ... copied`).
