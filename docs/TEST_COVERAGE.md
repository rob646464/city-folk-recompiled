# Test coverage before the first release

What has been run in the port and what never was, per area, so the remaining gaps can be tested
before release. Three test agents share this file (gameplay, options, regions): each keeps to its
own section. "Verified" = seen working in a port run (route, screenshots, log), with the branch or
doc that says so; "never run" = nothing in CLAUDE.md, docs/ or the git log shows a run.

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
| Museum donation | Donation! -> common butterfly (pointer `@-0.45/0.17`) -> Donate, with `[wiimote] remote1_snap=off` | ok: "I daresay that is a common butterfly. STOP! Do NOT free it!", "Our thanks for the donation", "Do you wish to donate anything else?" Yep -> the screen again. With "Snap pointer to menu items" on Auto (default) the scripted pointer clicks on that screen selected nothing in two runs, and the cursor is not drawn until the pointer moves (see Open) | |
| Quitting without saving (Resetti) | run 1: continue `user-town-clean`, play ~40 s (and once ~5 min, in and out of the house), quit (no save); run 2: continue that NAND (clock 10:00 again, 10:30, 11:30) | no Resetti in run 2. The session-start save ("Preparing saved data") is committed (`[save] result word 1`, rvforest.dat rewritten) and sets file byte 0x8670 (player 1 + 0x8650) 00 -> 02; Dolphin does exactly the same: Dolphin's own run-1 save is byte-identical to the port's apart from two random words, and Dolphin shows no Resetti either, from its own save or from the port's | none needed (matches Dolphin) |

Every log of these runs (about 60) was free of `FATAL`, `[hle] MISSING` and `[hang]`.

### Open (gameplay)

- **Museum screens with "Snap pointer to menu items" on Auto**: scripted `@x/y` clicks on the
  "Donate what?" screen selected nothing (twice) and the cursor was not drawn until the pointer
  moved; with snap off the same clicks donate. Not checked with a real mouse or a controller; the
  menu-nav work never reached the museum (docs/CONTROLS.md "Snap pointer to menu items").
- **Not reached yet**: catching a fish (no nibble at the pond by the house) or a bug, Able
  Sisters / GracieGrace purchases (the scripted walk did not face a display), catalog ordering,
  letters to villagers and their replies, K.K. Slider's show and Dr. Shrunk's emotions (the
  Club LOL ticket and the newcomers' show were reached, not the theatre), the HRA score letter,
  fossil assessment, a villager moving in or out, the auction's bidding days.
- **Resetti**: what makes him come after an unsaved quit was not found; the port and Dolphin agree
  for the sessions above (run 2 of every test started without a fixture copy: check the run log
  for `[nand] fixture ... copied`).
