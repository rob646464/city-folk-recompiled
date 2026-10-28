# Nintendo events: the WiiConnect24 distributions

F1 > Online > Nintendo events. From December 2008 to December 2012 Nintendo sent City Folk players
letters with items (Pete brought them) and designs (Wendell), over WiiConnect24. None of them is
on any disc (docs/REGION_CONTENT.md section 3). The port brings them back through the game's own
WiiConnect24 code, with the content from **WiiLink's archive of the City Folk distribution items
and letters**, used with WiiLink's permission. Credit line in the menu: "Distribution archive
courtesy of WiiLink".

Code: `src/recomp/distributions.cpp` (catalogue, archive download/cache, schedules, packages, F1
page), `src/recomp/nwc24_dl.cpp` (KD, NCD, the NAND files; the download side of WiiConnect24),
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
| distributions (items + designs) | 41 | 37 | 33 | 21 |
| of these in the archive | 39 | 35 | 21 | 8 |
| not in the archive | 1,000 Bells, candy (existing items) | ... | top, chocolate heart, Girl's Day updo, tokonoma, student cap, carp banner, ghost umbrella, morning glory, eggplant cow, celebration hat, red/white team hats, hagoita, emperor's cap, blossom lantern, Kintaro wig + apron, tsunokakushi, cucumber horse | flower bouquet, kimbap plate, school caps, red horned hat, samgyetang bowl, white police cap, tteok plate, red nose, colorful hanbok, Hangul T-shirt |

(Counts: see "Testing" for the command that prints them; the table is filled from it.)

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
4. registers download tasks in `/shared2/wc24/nwc24dl.bin`: task 0 = `http://nwcs.wapp.wii.com/ruu/rvforestdl_us.enc`
   (PAL `_eu`, JP `_jp`, KR `_kr`), task 1 = `http://nwcs.wapp.wii.com/nw/f/RUD/E.enc`, both with
   flags 0x0A (= verify with the title's RSA key, decrypt with the title's AES key),
5. later reads the downloaded file from `wc24dl.vff`, opens it as a U8 archive (`info.bin`,
   `item.bin`, `ltr<region><language>.bmg`, `design.bin`) and, when the letter id in `info.bin`
   differs from the last one the town got, keeps the letter + item for Pete (and the design for
   Wendell).

City Folk does steps 2-5 when a town is loaded (player select: `fn_801783B4` starts the
WiiConnect24 job `fn_8017B6F4` on its own thread) and only for a town whose own WiiConnect24
setting is on (save +0x735E4). Task 0's package is handled by `fn_8017B398`; the letter goes to
the save's Nintendo-letter slot through `fn_8010F194`.

The port implements what IOS does:

- **KD** (`/dev/net/kd/request`, `/dev/net/kd/time`) and a minimal **NCD** (`/dev/net/ncd/manage`:
  one wired connection, set up and tested): only while the feature is on. Otherwise the devices
  stay absent, as before, and the game sees WiiConnect24 off.
- **The System Menu's WiiConnect24 files**, created when missing and only while the feature is
  on: `nwc24msg.cfg` + `.cbk` (registered, mail domain, Nintendo's URLs), `nwc24fl.bin`,
  `nwc24dl.bin` (120 empty entries), `mbox/wc24send|recv.ctl` and `.mbx` (empty VFF volumes).
  SYSCONF `NET.WCFG` = 1 while on, 0 while off.
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

Not forged, not patched: the game's code is unchanged (one observation hook, see below), the
title key is the game's own, no signature is created or skipped for a WC24 file.

### The second task (`nw/f/RUD/E.enc`)

Not served (no archive data): KD answers "not found", as the game expects when there is nothing.
Nintendo's server still has `E.enc` / `P.enc` / `J.enc` / `K.enc`; what they hold is not known.

## 3. Schedules (F1 "Delivery")

**Off** (default): no device, no file, no download, WiiConnect24 off.

**In turn** (the main mode when on). Every distribution in play is delivered once per cycle,
evenly spaced by the game's clock (the console clock including the F1 clock offset), and the
cycle repeats forever:

- In play: released, in the archive, distributed in the game's region; with "Include other
  regions' distributions" also the others whose item has a name in the game's language (every
  archive item has all ten languages, so all of them).
- Slots: one delivery every 7 days (default), every 14 days (both counted from Monday
  2000-01-03), or on the 1st of every month. No two deliveries in one week.
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
  delivers nothing; a new cycle delivers it again.
- Letter ids: `4C` + slot number (stable per slot: the game does not take the same letter twice).

**Original dates**: each distribution on its original dates in the game's region (other regions
with the option), every year on the same month/day (default) or only in its original year
(2008-2012; reach it with the clock offset). Overlapping windows: one at a time, earliest window
first, at most one per 7 days; a town gets a distribution once per year. Letter ids: `4F` + index
+ year.

**Only when I choose**: only "Receive now".

**Receive now** (every mode except Off): the chosen distribution is served at KD's next check
(at once), with a fresh letter id (`4D` + counter), then the schedule continues.

**Deliver while playing** (USA, on by default): when KD has stored a package while a town is in
play, the port starts the game's own WiiConnect24 job (`fn_801783B4`, the call player select
makes) so the letter reaches Pete without loading the town again; once per town it also starts it
when the game has not registered its task yet (a new town registers it only when loaded). On a
Wii the letter would come after the next time the town is loaded. PAL/Japan/Korea: the next time
the town is loaded (no game hooks there yet).

Received tracking: USA through a hook on `fn_8010F194` (the game keeps a Nintendo letter; the hook
calls the original and records the letter id's distribution for the town); other bases: when KD
stored the package.

## 4. F1 page

Delivery mode, How often (In turn), Every year (Original dates), Include other regions'
distributions, Deliver while playing (USA); status (archive files, task / key / download box,
last check, next delivery date and item); the table of every distribution with its original
dates per region, its state (available / available (other region) / received / not in archive /
downloading) and "Receive now"; the folder for original `.enc` files with "Receive now" per file
and KD's verdict. Credit line: "Distribution archive courtesy of WiiLink".

## 5. Files and network

- Archive cache: `<data folder>/distributions/wiilink-archive/` (items, designs, letters; each
  checked against the SHA-1 pinned in `distributions_archive.inc`, from commit 402566bc of
  WiiLink24/ACDLCPicker; a changed file is not used). Downloaded from raw.githubusercontent.com,
  only while the feature is on, once.
- Original files: `<data folder>/distributions/*.enc`.
- Nothing from the archive is in this repository.

## 6. Testing

- `cf_distrib_test` (ctest `distrib`): SHA-1, AES-128-OFB (SP 800-38A), RSA modexp, VFF write /
  read / remove on FAT12 and FAT16; `cf_distrib_test <file.enc> <wc24pubk.mod>` checks one file.
- `CF_DISTRIB=off|loop|original|manual` overrides the mode; `CF_NWC24_LOG=1` logs every KD / NCD
  request, the town's WiiConnect24 flag and the task state; `CF_DISTRIB_NOW=<key>[@<s>]` presses
  "Receive now" for a distribution once the task is registered; `CF_DISTRIB_SAVE_DUMP=<frame>`
  dumps the save data (research).

## 7. Gaps

- Letters in languages other than English: the archive has only the North American English text;
  other languages of a package get the English body with a localized greeting.
- Japanese- and Korean-only distributions mostly missing from the archive (see the table).
- The second download (`RUD/E.enc`) is not served.
- Received tracking and live delivery are USA-only (game hooks); other bases deliver at the next
  town load and count a stored package as received.
