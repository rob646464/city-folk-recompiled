# Move in from Wild World

City Folk's title flow lets a new resident move over from *Animal Crossing: Wild World* (DS):
Rover asks "Do you want to start fresh, or move over from the DS?", the player answers
"Move from the DS!", the Wii sends a DS program by DS Download Play and that program reads the
Wild World Game Card and sends it back. The port has no DS radio, so it plays the DS: a window
asks for a Wild World save file and a virtual DS runs the program's side of the link with it.
Everything on the Wii side (Rover's dialogue, the link manager, the checks, the resident list,
the import, the catalog conversion, the save) is the game's own code, unchanged.

Code: `src/recomp/hle_mp.cpp` (DS wireless libraries), `src/game/cf_move_in.cpp` (the virtual
DS, the window, the F1 section, two small hooks), `src/game/ww_save.cpp` (save files),
`tools/tests/move_in_test.cpp` (unit test), hooks in `tools/recomp/hooks.txt` ("Move in from
Wild World"), settings `[move_in]`. USA addresses below; every hook is tagged `all`.

## 1. How the game does it (the code is the authority)

### Rover's side

`d_a_npc_sp_player_selectNP` runs Rover's title dialogue (`Script/<lang>/sys_MESS/SYS_Start.bmg`):

| Message | Text (USA English) | Code |
|---|---|---|
| 0x0A / 0x97 / 0xB8 | "...start fresh, or move over from the DS?" Start fresh! / Move from the DS! | choice |
| 0xB5 / 0x99 | "Insert your Wild World Game Card ... touch DS Download Play" OK! / Never mind. | |
| 0x9B (+0xAB) | "Download the Moving Van on your Nintendo DS!" ("press B to quit") | REL 0x5B18: `fn_800841F8(mgr, buffer, 0x40000)` |
| 0x6A | "Communicating... Please wait a moment." | link running |
| 0xAA | "All set! You can turn off the Nintendo DS now!" | result 1 |
| 0xA7 / 0xA8 / 0xA9 | no Game Card / "I don't think you have the right Game Card" / trouble reading it | results 2 / 3 / 4 |
| 0xA2 / 0x9F | "I couldn't find a Nintendo DS" / quit | link failed / B |
| 0xA5 / 0xA4 / 0xA6 | town data corrupted / no town data / nobody lives there | the game's own checks |
| 0x9C / 0x9D | "Who's going to visit from <town>?" (one choice per resident) / "So you're X from Y?" | resident list |
| 0x2D / 0x2E | "X already lives in <town>!" | duplicate check (fn_8013B468) |
| 0x2F | no open houses | |

### The link manager (main.dol fn_80083610 .. fn_80084738, object 0x80584E14)

`fn_800841F8(mgr, buffer, size)` = receive mode 0 (`fn_8008436C` mode 1 sends a player to a DS
(Copper's DS Suitcase, `Exch_Sign.srl`), `fn_80084538` mode 2 receives one back over MP only).
Mode 0 runs two layers:

1. **DS Download Play** of `/DSApp/Vaga_Sign.srl` with game group id **0x400806**
   (`fn_8008389C(mgr, 1, 0x400806, 0xC)`; path table r13-0x7890: [0] Exch_Sign, [1] Vaga_Sign).
   `Vaga_Sign.srl` is the Moving Van: it carries the DS SDK's BACKUP library (Game Card save
   access) and the strings below; `Exch_Sign.srl` (DS Suitcase) carries the Wi-Fi/DWC-DL libraries
   instead. (docs/FEATURE_RESEARCH.md had the two the other way round.) The wrapper fn_800E4344
   loads the .srl from the disc (fn_80086134), starts MPDL, waits until exactly the child with aid 1
   holds the program, boots it, reads its info (MAC) and cleans MPDL up.
2. **MP data** with the same ggid (`fn_80083928`, wrapper fn_800E4018 / fn_800E3EE4 / fn_800E40C4):
   up to 600 frames for the DS program to connect, then the protocol:

```
Wii -> DS   "REQS"
DS  -> Wii  "ACKS"
DS  -> Wii  length              u32 LE, at most the buffer size (0x40000)
DS  -> Wii  data                any packet sizes, until length bytes arrived
DS  -> Wii  CRC-32 of the data  u32 LE (reflected 0xEDB88320, init/xorout ~0; fn_802A9A20)
Wii -> DS   "ACKS"  (CRC matches)  or  "NACK" (DS starts again from "ACKS"; 5 tries)
```

Four-character codes are little-endian words ("REQS" is the value 0x52455153, bytes `53 51 45 52`).
`fn_80083CA8` (Wii sends), `fn_80083A7C` (receive callback), `fn_80083F94` (send done). After the
Wii's final ACKS, `fn_800840B8` looks at the data: exactly 16 bytes equal to `B2mRWeDsApNoRom`,
`B2mRWeDAInvalid` or `B2mRWeDsAFailed` are the DS program's error reports (results 2/3/4,
Rover's 0xA7/0xA8/0xA9), anything else is result 1 (data). The DS-side errors (no card, wrong
card) are therefore decided **on the DS**: the Wii never sees a wrong-region card's data.

### What the DS sends: the Game Card's backup memory

The receive buffer is 0x40000 bytes (cleared by fn_80085CE0), the size of Wild World's 256 KB
flash, and the Wii reads it with the Wild World save layout of **its own disc region**
(fn_80085388 = the region getter fn_801069D0; tables per region):

| | Japan | North America / Europe | Korea |
|---|---|---|---|
| save copy (16-bit LE sum = 0) | 0x12224 | 0x15FE0 | 0x173FC |
| player slot stride (4 slots) | 0x1D10 | 0x228C | 0x249C |
| town name / first slot (copy-relative) | +0x4 / +0xC | +0x4 / +0xC | +0x4 / +0x14 |
| town marker (u16, byte, byte) | 0x24444.. | 0x2BFBC.. | 0x2E7F4.. |
| name length, encoding | 6, 1 byte (Japanese table) | 8, 1 byte (Latin table) | 6, UTF-16 LE |
| player fields: flags, face/hair, colour, player id, town id, name, gender, catalog | 0x17EF 0x1CC6 0x1CC7 0x1CFC 0x1D04 0x1D06 0x1D0C 0x16CC | 0x1C6B 0x223C 0x223D 0x2276 0x2280 0x2282 0x228A 0x1B48 | 0x1D3B 0x243C 0x243D 0x247E 0x248C 0x248E 0x249A 0x1C18 |

(USA 0x8046DC70 / 0x8046DCDC / 0x8046DD18 / 0x8046DD28 / 0x8046DD38.) The character tables are at
0x8046D870 (Japanese) and +0x200 (North America / Europe: 1..26 A-Z, 27..52 a-z, 0x85 space ...);
Korean names are UTF-16 with full-width ASCII folded (fn_80085400). **North America and Europe
share one layout and one character set**; Japan and Korea differ in both.

The program's ARM9 code is compressed on the disc, so how many bytes it sends was not read from it;
everything the Wii reads lies in the first 2 x copy size (0x2BFC0 for North America / Europe), and
the port sends the whole 256 KB backup.

The game then picks a copy (fn_80085D3C: checksums, the town marker byte 2 / 0x1C, a counter),
counts residents (fn_800858F0: player id and town id nonzero, fn_80085894), offers them
(0x9C / 0x9D), and the chosen one is imported by REL function 0x25D8: name + player id + gender
(fn_80137124), face (fn_80085734), hair style through a 16-entry table (fn_80085658), hair colour
(fn_80085610), the flags bit, the town name, and the catalog block (0x123 bytes at field 8,
converted by fn_800C51D8 into the new resident's catalog). Nothing else of the Wild World player
is read: pockets, clothes worn, Bells, savings, the house and the encyclopedias stay behind, which
matches the community descriptions (Nookipedia "Moving Van", read as data only).

### Region

A Wii takes only Wild World Game Cards of its own region: the check is in the Moving Van program
(it reports `B2mRWeDAInvalid`, Rover's "I don't think you have the right Game Card"), and the Wii
could not read another region's layout anyway (it always uses its own). Community sources report
that the European City Folk rev 1 also accepts North American cards (docs/REGION_CONTENT.md
"Wild World moving van"); the port follows that.

## 2. In the port

### DS wireless (hle_mp.cpp)

The RevoEX MP and MPDL libraries (which drive IOS `/dev/net/wd/command`) are answered at their API
(table at the top of the file): MPDL startup/cleanup, the child masks (aid 1 = the DS), child info
(a made-up Nintendo MAC), MP startup (ggid, indication callback at cfg+0x194), send (copied to the
virtual DS, completion `cb(0, arg)`), disconnect, cleanup. Indications (4 connected, 3 data, 5
disconnected) and send completions are queued and delivered from the game's MP link poll
(fn_800E3EE4, hooked): on the game's thread, outside the link manager's mutex, at most four data
packets a frame (the 256 KB arrive in about two seconds at 60 fps). Without a virtual DS (or with
another ggid, e.g. Copper's DS Suitcase) no DS ever shows up and the game times out or the player
quits as on a Wii with no DS around (before this, the MPDL start failed at once).

### The virtual DS (cf_move_in.cpp)

When MPDL starts while the link manager is in receive mode (+0x1B8 = 0; the Moving Van's ggid is
0x400806 on the USA disc, 0x8002CA PAL, 0x000986 Japan, 0xC00055 Korea, so the mode decides) the window "Move in from Wild World" opens (game input waits
while it asks for something). It reads a save file (the last one is offered again), shows the
format, the layout, both copies' checksums, the town, the Game Card region (guessed from the
layout and the file name, a choice between North America and Europe for the shared layout) and
the residents. "Turn on the DS" = the DS downloads the Moving Van (8 s), boots (0.8 s), connects
(1.2 s) and answers REQS with the save. The Wii's ACKS ends it; the window closes a few seconds
later. With a resident picked in the window, the "imported resident exists" test (fn_80085894,
hooked) answers no for the others, so Rover asks "So you're X from Y?"; "Let Rover ask" leaves
his list of every resident.

What the DS sends: the 256 KB image as read from the file; for a Game Card of another region,
`B2mRWeDAInvalid`, unless F1 allows every region: then the image as it is (same layout), or
converted for this game's layout (`ww::convert`: town name, town marker, every slot's fields the
import reads and the catalog block, names re-encoded through the game's character tables with '?'
for missing characters, both copies' checksums fixed).

### Save files (ww_save.cpp)

Raw flash dumps (256 KB; 512 KB: the half with a valid save; shorter ones padded like erased
flash), DeSmuME `.dsv` (cut at its `|<--Snip above here...` footer), No$GBA (uncompressed `SRAM`
block; compressed ones are refused with a message). A save is valid when a copy's 16-bit sum is
0 for one of the three layouts. Never written.

### F1

Enhancements > Gameplay > "Move in from Wild World": **Wild World saves from any region** (off,
`[move_in] any_region`), **Check a save file...** (opens the window to look at a save),
the last file (`[move_in] last_file`).

### Test aids

`CF_OPEN_MENU=<frame>:<kind>[;...]` opens a game menu by kind (0x3A = Nook's catalog,
src/game/cf_move_in.cpp). `CF_MOVE_IN=<file>[,<slot 0-3 | -1>]` answers Rover's DS request with that file at once (window
shown, input not blocked), `CF_MOVE_IN_REGION=<0-3>` sets the Game Card region, `CF_MOVE_IN_ANY=1`
allows every region, `CF_MOVE_IN_LOG=1` / `CF_MP_LOG=1` log the libraries and the protocol.
`cf_move_in_test` (synthetic saves of every layout, .dsv, 512 KB, damaged copies, conversion
round trips, CRC-32; `CF_WW_SAVE=<file>` also lists a real save).

## 3. Testing

Route (USA, fixture `user-town`, `CF_INPUT_CLOCK=frames`): the continue route to Rover's
"You are...?", then the pointer at KPAD (0.44, 0.50) and A every 30 frames: that is the
second-to-last line of every choice box on the way ("Rob64. / **I'm new!** / Never mind.",
"Start fresh! / **Move from the DS!** / Never mind.", "**OK!** / Never mind.", "**Yep!** / Never
mind."; the boxes grow upwards), then the town map's OK! button (0.656, 0.778) on the bus.

Verified 2026-11-03 with the user's own save `Animal Crossing - Wild World (USA) (Rev 1).sav`
(raw 256 KB, both copies valid, town "poo town", one resident "rob", boy, face 5, hair 7, colour 1;
Rev 1 saves have the same layout as Rev 0: the game has one table per region, none per revision):

- the game's own dialogue from "Move from the DS!" through "Download the Moving Van" and
  "Communicating..." to "All set! You can turn off the Nintendo DS now!" (0xAA); log: MPDL startup
  ggid 400806, boot, MP startup, connect, Wii REQS, 262144 bytes + CRC-32 5CA8EA6A, Wii ACKS
  (its CRC matched), link closed;
- "So you're rob from poo town?" (0x9D, the save's resident through the game's own name and town
  conversion) -> "Yep!" -> "Preparing saved data." (the game's save, result word 1) -> Rover and
  Kapp'n's bus ("You're so lucky, rob!") -> the bus stop of the fixture's town "Frunch", the new
  resident standing there ("Head on over to the town hall");
- the resident in the save: a forced save afterwards (`CF_FORCE_SAVE`, the game's own save write)
  has player slot 1 with face 5, hair 7, colour 1 at pd+0x83EA (the import's fields) and 80
  catalog bits at pd+0x83FA (the Wild World catalog block has 152 bits set; fossils, gyroids and
  items City Folk lacks are dropped by fn_800C51D8), slot 0 (Rob64) unchanged;
- the region check: `CF_MOVE_IN_REGION=2` (a European Game Card on the USA game) -> the Moving
  Van sends `B2mRWeDAInvalid` -> "...Hrm? I don't think you have the right Game Card." (0xA8);
- the window (PrintWindow screenshot while Rover waits): format, layout, both checksums, town,
  Game Card region, resident, "Turn on the DS".

Second round (2026-11-03, branch move-in-2):

- **PAL / Japan / Korea builds** (`toolsuild.ps1 -Game <id>`, hooks `all`): each with the
  `user-town` fixture's title folder renamed, the same route (Korea's choice boxes further right:
  x 0.44 / 0.56), a transfer to "All set!", "Preparing saved data" and the bus to Frunch. PAL took
  the USA save as a DeSmuME `.dsv` (North American card accepted by the European game); Japan
  (No$GBA file) and Korea (512 KB file, save in the second half) with `CF_MOVE_IN_ANY=1`, i.e. the
  save converted to the Japanese / Korean layout: the game's own checks and CRC passed. The
  Japanese character table has A-Z / a-z, so "rob" converts without '?'.
- **Emulator formats** generated from the user's save in the scratch folder: `.dsv` (raw + the
  0x7A-byte footer: snip line, six u32, `|-DESMUME SAVE-|`), No$GBA uncompressed and compressed
  (refused with a message), 512 KB doubled and second-half, trimmed to 0x2BFC0 (padded): all read
  as the same save (cf_move_in_test with `CF_WW_SAVE`). These are files written from the formats'
  documented layout, not files saved by the emulators themselves.
- **Two residents**: slot 1 of a scratch copy = slot 0 renamed "ann" (girl, other player id),
  sums fixed. "Let Rover ask": Rover lists "rob. / ann. / Never mind.", the second -> "So you're
  ann from poo town?" -> "You're so lucky, ann!" on the bus.
- **F1**: Enhancements > Gameplay shows "Move in from Wild World" (moved above the long region
  list): "Wild World saves from any region", "Check a save file...", the last file.
- **Town hall** (with the `WARP=` token from branch warps): Pelly greets the moved-in resident
  ("All available houses in Frunch... 3 vacant houses", "Let me just get the paperwork started",
  "Do you know how to open your map?"), i.e. the same flow as a new resident. The next step did
  not work by script: the vacant houses' doors (house 2, reached through the town hall door and
  `TP=`) still say "On sale now! Inquire at town hall for tours!" and Tom Nook (warp into his
  shop) says "You already finished saying hello to everyone?"; warping into the empty house finds
  no Nook. So the house purchase and the save the game makes after it are still open.

Third round (2026-11-03, branch move-in-3):

- **The catalog in the game's own catalog screen.** Tom Nook's catalog is the menu kind 0x3A
  (`fn_8019B064(kind)` = open a menu, what an NPC's menu command calls in fn_8002B3E4; kind 0x3A
  loads the archives d_paper + d_menu_catalog, table 0x804EBE10; "Catalog, please." -> message
  0x16 in NPC_tanukichi.bmg). The moved-in resident's Nook still runs the newcomer dialogue
  ("You already finished saying hello to everyone?"), so the test aid `CF_OPEN_MENU=<frame>:0x3A`
  opens that screen in Nook's Cranny (`WARP=nook`). Tabs shown (screenshots): furniture "exotic
  bureau, exotic wardrobe, regal vanity, blue bureau...", flooring "imperial tile, kitchen tile,
  stone tile, round carpet...", clothes "six-ball shirt, No. 1 shirt, No. 3 shirt, peachy
  shirt...", umbrella "blue dot parasol", headgear "moon hairpin", stationery "maple-leaf paper,
  lined paper, polka-dot paper, four-leaf paper...", gyroids and fossils empty. The same screen for
  Rob64 (the fixture's own resident) lists his own, different entries (orange chair, tape deck,
  cardboard box; exotic rug, opulent rug; work uniform, four-ball shirt; lined paper), so the
  screen shows the resident's own catalog.
- **The same list from the save file**: the catalog bit index is the item id, and the item.bin
  record field at +8 is that id (its name); the Wild World block decoded through the game's own
  tables of fn_800C51D8 (0x80471DC0 +0xF90 / +0x1018 / +0x1BC / +0x10A0) gives 82 candidate
  items, 80 of which are the 80 catalog bits of the moved-in resident in the save, with exactly
  the names on the screens above; the game's own checks (fn_800C27E0 / fn_800C20DC) dropped two
  (work uniform, id 3210). The Wild World block has 152 bits set: the rest have no City Folk item
  in the table (0xFFFF), e.g. fossils and gyroids, which also stay empty in the catalog.
  Scratch scripts: wwcatalog.py, cfcat.py (not committed).
- **Saving**: the HUD's Save button as the moved-in resident answers "Hm... Looks like you can't
  save just yet." (the game blocks saving before the house). The game's own save at the move
  ("Preparing saved data.") stores only the move record; the resident is stored by the forced save
  (`CF_FORCE_SAVE`). **Reload of that save**: Rover's "You are...?" lists "Rob64. / Rob. / I'm
  new! / Never mind." (the moved-in resident, name capitalised by the game), Rob -> "All righty!
  I'll get things ready" -> "Preparing saved data." -> "Thanks for waiting!" and the game stays in
  Rover's room (as docs/NAND.md says of a save forced before the house), so his look after a
  reload is checked only in the save data (face 5, hair 7, colour 1, 80 catalog bits).

Not verified by script: the house purchase and the game's save after it (the user dropped it:
too long to script), other regions' own Wild World saves (only the USA save, converted), files saved by
DeSmuME / No$GBA themselves.
Earlier finding: with a 3 s download and the DS turned on at once (test aid), the link could come
and go before Rover's "press B to quit" line was up and he then said he could not find a DS; the
download takes 8 s now (a DS needs several seconds for the 320 KB program).
