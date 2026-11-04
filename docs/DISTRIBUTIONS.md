# Nintendo events: the WiiConnect24 distributions

F1 > Online > Nintendo events. From December 2008 to December 2012 Nintendo sent City Folk players
letters with items (Pete brought them) and designs (Wendell), over WiiConnect24. None of them is
on any disc (docs/REGION_CONTENT.md section 3). The port brings them back through the game's own
WiiConnect24 code, with the content from **WiiLink's archive of the City Folk distribution items
and letters**, used with WiiLink's permission. Credit line in the menu: "Distribution archive
courtesy of WiiLink".

Code: `src/recomp/distributions.cpp` (catalogue, archive download/cache, schedules, packages, F1
page), `src/recomp/distrib_record.cpp` (the per-town record in state.ini), `src/recomp/nwc24_dl.cpp` (KD, NCD, the NAND files; the download side of WiiConnect24),
`src/recomp/vff.cpp` (VFF volumes), `src/recomp/wc24_crypto.cpp` (SHA-1, AES-128-OFB, RSA-2048),
`src/port/http_get.cpp` (one HTTP GET). Settings: `[nintendo_events]` in settings.ini
(`src/port/settings.h` NintendoEventSettings). Tests: `cf_distrib_test`.

## 1. What WiiLink's archive contains

WiiLink (the service at wiilink.ca/services/animalcrossing, run for WiiLink by a third party)
serves City Folk distributions to Wii consoles today. Its generator is the public repository
**WiiLink24/ACDLCPicker** (GitHub; GPL-3.0 code, which we do not use; only its data files are
downloaded at run time). It holds:

| Folder | What | Count |
|---|---|---|
| `items/` | `item.bin` of a distribution: 8 KiB "BITM" records (price, item id, the name in all ten languages, model and texture). Originally found by Aurum (SunakazeKun) in the files of a Nintendo channel, which carried every released distribution item and three unreleased ones. | 74 files (72 unique; 2 duplicate names) |
| `designs/` | `design.bin` (8 KiB "BITM" pattern records) | 7: Mayor's flag (NA), flamenco dress (EU), dirndl dress (EU), guard's uniform (NA), student uniform (JP), red padded vest (JP), Victory Korea T-shirt (KR) |
| `stuff/` | Nintendo's **English (North American)** letter text of each distribution, as plain text (header, body, footer). Letters in other languages are not in the archive. | 61 letters (one empty) |
| `src/`, `acwc24.py`, `dlc.py` | the generator: picks one item + one design twice a month (1st and 15th), builds a package with a WiiLink letter and signs it with WiiLink's own key | (not used) |

What the live service sends: one package per region at `nwcs.wapp.wii.com/ruu/rvforestdl_<us|eu|jp|kr>.enc`
(their DNS points that host to their server; the same item for every region, letters in each
region's languages, English text). Only the current pick is ever online; there is no per-item
download. Nintendo's own server still answers with its last file (436 bytes: an `info.bin`
only, no item).

Per region, the archive has (catalogue in `distributions.cpp`, dates from Nookipedia's list,
which we treat as data):

| | North America | Europe | Japan | South Korea |
|---|---|---|---|---|
| distributions (an item and its design count as one) | 52 | 47 | 52 | 17 |
| of these in the archive | 50 | 47 (all) | 36 | 7 |
| not in the archive | 1,000 Bells, candy (both ordinary items) | none | top, chocolate heart, Girl's Day updo, tokonoma, carp banner, ghost umbrella, morning glory, eggplant cow, red/white team hats, hagoita, emperor's cap, blossom lantern, Kintaro wig + apron, tsunokakushi, cucumber horse | flower bouquet, kimbap plate, white school cap, school cap, samgyetang bowl, colorful hanbok, Hangul T-shirt, white police cap, tteok plate, red nose |

(Printed by `CF_DISTRIB_DESCRIBE=<n>`, "Testing". Japan's student cap / celebration hat and
Korea's red horned hat count as in the archive through their designs only: the hat itself is
missing, the design is there.) Every archive item carries its name in all ten languages, so with
"Include other regions' distributions" every region gets all 71 released ones in the archive.

Never distributed but in the archive (offered with "Receive now" only, never scheduled): red
headgear, blue Pikmin, yellow Pikmin (Nintendo's unreleased items), Blue Falcon, Nintendo bench,
silver roses, Pikmin.

Original dates: every window per region is in the catalogue (e.g. red Pikmin JP 2008-12-24, EU
2009-02-06, NA 2009-03-25; the Golden Series monthly through 2011; the Creepy Series twice a month
June-October 2012).

## 2. How delivery works (the game's own path)

On a Wii, WiiConnect24 downloads are done by **KD**, a daemon inside IOS, not by the game. The
game (its NWC24 library, recompiled game code here) only:

1. checks that WiiConnect24 is usable (`NET.WCFG` bit 0 in SYSCONF, the WiiConnect24 user
   agreement, a network connection in NCD's settings, KD's scheduler state),
2. installs its key file `wc24pubk.mod` in its data folder (RSA-2048 modulus at 0, AES-128 key at
   0x200; the game carries both in main.dol),
3. creates `wc24dl.vff` (a FAT volume file, 0x5A000 bytes) in its data folder,
4. registers download tasks in `/shared2/wc24/nwc24dl.bin` (`fn_800EA9F0`, values from tables in
   main.dol): the distribution `http://nwcs.wapp.wii.com/ruu/rvforestdl_us.enc` (PAL `_eu`, JP
   `_jp`, KR `_kr`) stored as **`game_data.bin`** in `wc24dl.vff`, every 720 minutes; and
   `http://nwcs.wapp.wii.com/nw/f/RUD/E.enc` stored as **`prc_list.bin`** (the profanity list
   update), every 1440 minutes; both with flags 0x0A (= verify with the title's RSA key, decrypt
   with the title's AES key). The distribution task is registered only for a town whose own
   WiiConnect24 setting is on; the profanity task for every town,
5. later reads the downloaded file from `wc24dl.vff`, opens it as a U8 archive (`info.bin`,
   `item.bin`, `ltr<region><language>.bmg`, `design.bin`) and, when the letter id in `info.bin`
   differs from the last one the town got, keeps the letter + item for Pete (and the design for
   Wendell).

City Folk does steps 2-5 when a town is loaded (player select: `fn_801783B4` starts the
WiiConnect24 job `fn_8017B6F4` on its own thread) and only for a town whose own WiiConnect24
setting is on (save +0x735E4). Task 0's package is handled by `fn_8017B398`; the letter goes to
the save's Nintendo-letter slot through `fn_8010F194`.

The port implements what IOS does:

- **KD** (`/dev/net/kd/request`, `/dev/net/kd/time`, `nwc24_dl.cpp`) and a minimal **NCD**
  (`/dev/net/ncd/manage`: ReadConfig 0x03 answers one wired connection, set up and tested): only
  while the feature is on. Otherwise main's `KdDevice` (ios_devices.cpp: every request accepted,
  WiiConnect24 off) answers and NCD stays absent, so saving works exactly as before.
- **The System Menu's WiiConnect24 files** come from `sysconf.cpp prepareWc24` (main), on or off.
  SYSCONF `NET.WCFG` = 1 while the feature is on, 0 while off. With it on, the game's boot check
  (`fn_80178640`) reports WiiConnect24 available (state 1 at r13-0x2D74) instead of error 109020.
- **The town's own setting**: a town started with WiiConnect24 off keeps it off (save +0x735E4;
  the HUD then shows no "W" next to the clock) and the game ignores Nintendo's letters. With
  "Turn WiiConnect24 on for my town" (default on) the port calls the game's own setter
  `fn_80177DE0` (what Rover's question at player creation and the attic phone call; it sets the
  flag only while the boot check said "available") once a town is in play; saved with the town.
- **VFF**: the game's VF library formats `wc24dl.vff` itself (media byte F0, a folder `mb`) and
  keeps long (VFAT) names, so KD writes `game_data.bin` with long-name entries and an 8.3 alias
  (`vff.cpp`); the game's own VF code reads it back (verified).
- **KD's download check**: every 60 s, when the game resumes KD's scheduler after using its files,
  and at once for "Receive now". For each type-3 task of the running title it asks the "server"
  (`distrib::serve`) for the URL. A WC24 file (`.enc`) goes through KD's checks exactly as on a
  Wii: the RSA-2048 signature (PKCS#1 v1.5, SHA-1 over the decrypted payload) must verify with the
  title's own `wc24pubk.mod`, the payload is decrypted with its AES key (OFB, IV at 0x30); a file
  that does not verify is refused (KD error 107245) and nothing is stored. The result is written
  into `wc24dl.vff` under the task's file name; `nwc24dl.bin`'s entry gets its result, time and
  next-download time. KD stores only while the game has not suspended the scheduler (the game
  suspends it while it reads its WiiConnect24 files).

### The signature: what verifies and what does not

Checked with the game's own key (from main.dol, all four regions carry the same key):

| File | Signature with the game's key |
|---|---|
| Nintendo's last file still on `nwcs.wapp.wii.com` (us/eu/jp/kr, 436 bytes, no item) | **verifies** |
| An original Nintendo package preserved by the Internet Archive (EU, October 2012: creepy clock, letter id 50) | **verifies** |
| WiiLink's live packages (all four regions) | **does not verify**: they are signed with WiiLink's own key (`rvforestdl.pem` in their deployment), not Nintendo's |

So the archive is not a set of original signed packages: it is the original **contents** (item
records, designs, Nintendo's English letter texts). The port therefore builds each package itself
from the archive files: the same U8 layout as Nintendo's packages (`info.bin` 1, 1, letter id,
design id, 0; `item.bin`; one BMG letter per language of the region with Nintendo's message
layout and attributes; `design.bin`), and KD stores it in `wc24dl.vff` **without** the WC24
transport step (there is no Nintendo signature to check, and the port never makes one). Everything
after that is the game's own code: it reads the package from `wc24dl.vff`, checks the letter id,
keeps the letter and item, and Pete delivers it. Original `.enc` files the player puts into
`<data folder>/distributions/` take the full path with the signature check, so only genuine
Nintendo files are accepted from there.

Not forged, not patched: the game's code is unchanged (two observation hooks that call the
originals, see below), the title key is the game's own, no signature is created, and no WC24 file
gets past a failed check.

### The second task (`nw/f/RUD/E.enc`)

Stored as `prc_list.bin`: the profanity list update (the game's own list is `Prc/*.bin` on the
disc). Not served (no archive data): KD answers "not found", as when there is nothing. Nintendo's
server still has `E.enc` / `P.enc` / `J.enc` / `K.enc`.

## 3. Schedules (F1 "Delivery")

**Off** (default): no device, no file, no download, WiiConnect24 off.

**In turn** (the main mode when on). Every distribution in play is delivered once per cycle,
evenly spaced by the game's clock (the console clock including the F1 clock offset), and the
cycle repeats forever:

- In play: released, in the archive, distributed in the game's region; with "Include other
  regions' distributions" also the others whose item has a name in the game's language (every
  archive item has all ten languages, so all of them).
- Slots: one delivery every 7 days (default), every 14 days (both counted from Monday
  2000-01-03), or on the 1st of every month. One delivery per slot: when one came in the slot
  already (a "Receive now"), the slot's own waits for the next slot, so no two in one week (the
  game keeps one Nintendo letter at a time).
- Order: computed, the same for everyone (a function of the catalogue and the date only, no hand
  list, no year limit). Walking the slots from 2000: when the cycle's remaining set is empty a new
  cycle starts with all of them. In each slot: a seasonal distribution (themed for its time of
  year: holidays, seasons) whose original month (its first window in the game's region, else its
  first window) is the slot's month goes first (earliest-first by a fixed hash order); otherwise a
  non-seasonal one in a fixed spread order (FNV-1a hash of its key, which mixes the series); when
  only seasonal ones remain and none fits the month, the one whose month comes next. So the carp
  banner comes in May, Christmas items in December, summer items in summer, wherever a cycle is.
- Missed time (clock offset jumps, not playing for weeks): KD serves only the **current** slot's
  distribution; missed slots are skipped (their distributions come again next cycle).
- Each town's record (`<data folder>/distributions/state.ini`): the distribution and the cycle it
  came in. A slot whose distribution the town already got in this cycle (e.g. by "Receive now")
  delivers nothing; a new cycle delivers it again. This rule is unchanged by "Receive now once"
  (below): a "Receive now" still counts as this cycle's delivery of that distribution and uses up
  its slot, exactly as before, and the next cycle brings it again (a duplicate, so an item that was
  sold comes back in its turn, at most one letter per 7 days).
- Letter ids: `4C` + slot number (stable per slot: the game does not take the same letter twice).
  KD's "If-Modified-Since": the package stored last is not stored again.
- Example (USA, weekly, from 2026-10-26): Kapp'n model, shopping cart, anniversary cake, creepy
  crystal, golden bench, golden clock, snowman head (Dec 7), red Pikmin, golden table, golden
  carpet, snowman vanity (Jan 4), creepy skeleton (`CF_DISTRIB_DESCRIBE=12`).

**Original dates**: each distribution on its original dates in the game's region (other regions
with the option), every year on the same month/day (default) or only in its original year
(2008-2012; reach it with the clock offset). Overlapping windows: one at a time, earliest window
first, at most one per 7 days; a town gets a distribution once per year. Letter ids: `4F` + index
+ year.

**Only when I choose**: only "Receive now".

**Receive now** (every mode except Off): the chosen distribution is served at KD's next check
(at once), with a fresh letter id (`4D` + counter), then the schedule continues. **Once per
distribution per town** (user decision, anti-cheat): a fresh letter id every time let players
receive the same item over and over and sell it at Nook's. See "Receive now once per town" below.

### Receive now once per town

- A distribution this town has received through "Receive now" is greyed out in the F1 table
  (name and button; state "received (Receive now)"), its tooltip says "Received on <date>" (the
  game clock's date when the game kept the letter). The others stay available, one time each.
  "Receive now" is also off while the same one is on its way (requested, or stored in
  `wc24dl.vff` and not yet kept by the game) and until the town has been seen in this session
  ("Load your town first").
- Checked three times: the F1 button, `receiveNow` (refused, `[distrib] receive now refused: ...`)
  and KD's request (`serve`: a request for a distribution the town already has is dropped).
- Counted when the game keeps the letter: USA through the `fn_8010F194` hook (letter id `4D...`),
  other bases when KD stored the package (as their "received" record). Original `.enc` files
  from the folder count per file (SHA-1 of the file, `enc:<sha1>`), when KD stored them.
- **The schedules are independent**: "In turn" and "Original dates" deliveries never write or
  clear the "Receive now" record, and "Receive now" affects the schedules only as before (it
  counts as this cycle's delivery and uses up the slot: see "In turn" above). The schedule keeps
  bringing duplicates in their turn (pacing unchanged), so nobody loses an item for good.
- **Where it is stored**: `state.ini`, per town, keyed by the town's own identity from the save:
  the town id (u16, made when the town was created) + the town name, read from the first
  resident's copy of them in the town data (`rd32(0x8074E960)` = the save's contents, resident
  slots at +0x20 + slot * 0x86C0, town struct at +0x7EC2 of each: u16 id + UTF-16 name; in the
  user-town fixture's save file at 0x7EE2: `F177`, "Frunch"). Key `<id>-<name as UTF-16 hex>`
  (`town_id.cpp keyInSave` + `distrib_record.cpp townKey`; the same key as `town_wc24.txt`, docs/ONLINE.md), section `[received_now.<key>]` (`<distribution>=<time>`),
  readable names in `[towns]`. The data folder (incl. portable mode and F1 > Game > Data folder
  moves) carries the file, so a copied or renamed data folder keeps the record, restarting keeps
  it, and a different town (another save) gets its own record. The town is identified every 60
  frames while a town is loaded (all four versions; not while in an online room, where the town
  data may be another player's) and when the game keeps a letter. `state.ini` is written whole
  (temporary file + rename).
- **Records from before this change** (`format` missing in `[state]`): one record `[town.town]`
  for the install. At the first start a distribution counts as received through "Receive now"
  when that record has it and a "Receive now" letter id (`4D...`) was served for it
  (`[served]`), logged as `[distrib] record from an older version: N ...`. The first town
  identified on that install takes the old record (its received/cycle entries and the "Receive
  now" ones), since City Folk has one town per console.
- **Limits**: an offline game cannot fully stop save editing or someone editing / deleting
  `state.ini` (or using another data folder with a copy of the save). The goal is closing the easy
  loop (press, sell, press again), not a tamper-proof record. A letter the game kept counts even
  if the player then quits without saving (the schedule brings it again). Older builds of the port
  that rewrite `state.ini` drop the `[received_now.*]` sections.
- **Developer builds only** (`CF_DEV_TOOLS`, cfrecomp; never in the shipped CityFolkRecomp):
  "Forget received" next to the town line in F1 > Online > Nintendo events (clears this town's
  "Receive now" record; the schedule's record stays) and `CF_DISTRIB_FORGET=1|<key>` (the same,
  all or one distribution, once when the town is first seen).

**Deliver while playing** (USA, on by default): when KD has stored a package while a town is in
play, the port starts the game's own WiiConnect24 job (`fn_801783B4`, the call player select
makes), so the game takes the letter at once; once per town it also starts it when the game has
not registered its distribution task yet (a town whose setting was just switched on registers it
only at its next load). Pete then hands the letter over **when the town is next loaded** (he
greets the player at the house on arrival; seen in the test below), so the player must save
first. Without the option the game reads the package at that load itself (the job at player
select runs before the town), with the same result; the option only saves one reload for a town
whose WiiConnect24 setting was just switched on. PAL/Japan/Korea: always at the next load (no game
hooks there yet).

Pete: the letter kept in the save's Nintendo-letter slot (save +0xFC980: letter id, then the
letter) is handed over by Pete (`d_a_npc_sp_perioNP`, "Perio" is Pete's Japanese name): on the
next town load he waits at the player's house, says "You have a letter from Nintendo!", and his
`fn_80104520` copies the slot into the player's letters (pockets > Letters: "From Nintendo", with
the item as its present, "Remove Present"). The id stays in the slot: the same package read again
gives no second letter. Hooked (USA) only to log `[distrib] Pete handed Nintendo letter ...`.

Received tracking: USA through a hook on `fn_8010F194` (the game keeps a Nintendo letter; the hook
calls the original and records the letter id's distribution for the town); other bases: when KD
stored the package.

## 4. F1 page

F1 > Online > Nintendo events (next to Play together): Delivery mode, How often (In turn), Every
year (Original dates), Include other regions' distributions, Turn WiiConnect24 on for my town and
Deliver while playing (USA); status (archive files, task / key / download box, last check, next
delivery date and item); the town line ("Town: Frunch (F177). "Receive now" works once per
distribution for each town."; dev builds: "Forget received"); the table of every distribution
with its original dates per region, its state (available / available (other region) / received /
received (Receive now) / not in archive / downloading) and "Receive now" (greyed with "Received on
<date>" once this town got it that way); the folder for original `.enc` files with "Receive now"
per file and KD's verdict.
Credit line: "Distribution archive courtesy of WiiLink". `CF_MENU_ID_CHECK` sweep: 0 conflicts.

## 5. Files and network

- Archive cache: `<data folder>/distributions/wiilink-archive/` (items, designs, letters; each
  checked against the SHA-1 pinned in `distributions_archive.inc`, from commit 402566bc of
  WiiLink24/ACDLCPicker; a changed file is not used). Downloaded from raw.githubusercontent.com,
  only while the feature is on, once (141 files, ~1 MB).
- State: `<data folder>/distributions/state.ini` (`src/recomp/distrib_record.cpp`): format, the
  current town's key, per town (keyed by town id + name) the received distributions with time and
  cycle and the ones received through "Receive now", town names, last stored letter id, "Receive
  now" counter, served letter ids.
- Original files: `<data folder>/distributions/*.enc`.
- Nothing from the archive is in this repository.

## 6. Testing

- `cf_distrib_test` (ctest `distrib`): SHA-1, AES-128-OFB (SP 800-38A), RSA modexp, VFF write /
  read / remove with long names on FAT12 and FAT16, the per-town record (town keys, an older
  `state.ini` upgraded and adopted by the first town, a second town on its own, round trip);
  `cf_distrib_test <file.enc> <wc24pubk.mod>` checks one file.
- `CF_DISTRIB=off|loop|original|manual` overrides the mode; `CF_NWC24_LOG=1` logs every KD / NCD
  request and, every 600 frames, the town's WiiConnect24 flag, the task state, the game's
  WiiConnect24 check and the Nintendo-letter slot; `CF_DISTRIB_NOW=<key>[@<s>][,<key>[@<s>]...]`
  presses "Receive now" once the task is registered and the town is known, several in order (the
  same key twice: the second is refused); `[distrib] town ...` logs the identified town and what it
  received through "Receive now"; `CF_DISTRIB_FORGET=1|<key>` (developer builds) forgets that
  record; `CF_DISTRIB_DESCRIBE=<n>` logs the per-region counts and the
  next n loop slots; `CF_DISTRIB_SAVE_DUMP=<frame>` dumps the save data (research).
- Route `tools/routes/mailbox.txt` (user-town fixture): continue, walk to the mailbox, open it,
  pockets, letters, read the first one.

### Verified on the USA disc (2026-11-02, user-town fixture, scratch folders)

1. **Receive now** (`CF_DISTRIB=manual CF_DISTRIB_NOW=creepy_clock`, console clock 08:50, continue
   route, `CF_FORCE_SAVE=7000`): the town's WiiConnect24 setting was off and was switched on with
   the game's setter; the archive (141 files) was downloaded and checked; the game registered its
   task (`game_data.bin`), KD stored the package, the game's job took Nintendo letter `4D000001`
   (creepy clock); the save completed (result word 1). Loading that save again: Pete waits at the
   house, "You have a letter from Nintendo!", `fn_80104520` result 1; pockets > Letters: "From
   Nintendo" with a present; read: the archive's letter on the Halloween stationery ("This clock
   must be haunted... From Jack").
2. **Original dates with the clock offset** (`mode=2`, `original_yearly=false`, clock offset to
   2009-07-25 09:05): KD served the Dolphin model (NA 2009-07-23 to 08-03), the game took letter
   `4F0157D9`.
3. **In turn** (`CF_DISTRIB=loop`, 2026-10-28): the week of 2026-10-26 served the Kapp'n model
   (slot 1399, letter `4C000577`); stored once, then "not modified".
4. **Off** (default settings, same fixture, continue + `CF_FORCE_SAVE`): no KD/NCD of this module
   opened, no `[nwc24]`/`[distrib]` line, no distributions folder, no network access by this
   feature, `wc24dl.vff` byte-identical to the fixture's, no distribution task; the save completed
   (result word 1).

## 7. Gaps

- Letters in languages other than English: the archive has only the North American English text;
  other languages of a package get the English body with a localized greeting (French, Spanish,
  German, Italian salutations; Japanese and Korean packages use the English greeting).
- Japanese- and Korean-only distributions mostly missing from the archive (see the table); the
  three Korean / Japanese hats whose design is in the archive come as the design only.
- WiiLink's live packages and the archive are not Nintendo-signed (see "The signature"):
  archive packages are built locally and stored without the WC24 transport step. Only genuine
  `.enc` files from the player's folder go through the signature check (verified with the
  preserved October 2012 package outside the game: it verifies; a WiiLink file does not).
- The second download (`RUD/E.enc` -> `prc_list.bin`, profanity list) is not served.
- Pete hands a letter over only at a town load; a letter taken while playing waits for the next
  load (save first). Received tracking and live delivery are USA-only (game hooks); other bases
  deliver at the next town load and count a stored package as received. Not tested on PAL /
  Japan / Korea.
- Per-town tracking: keyed by the town id + name from the save since "Receive now once" (before:
  one key "town" for the install, adopted by the first town identified).
- The F1 page itself was not screenshotted (frame shots capture the game image only).
