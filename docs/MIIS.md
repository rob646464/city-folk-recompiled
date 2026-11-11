# Miis: the Mii maker (F1 > Miis)

City Folk uses Miis for one thing: Harriet's "Makeover!" at Shampoodle in the city, which turns the
player's face into a Mii mask. On a Wii the Miis come from the Mii Channel. The port has no Mii
Channel, so F1 > Miis is a Mii maker of its own: make, edit, import and export the Miis of the
emulated console, with a 3D preview drawn by the game's own Mii library. Code:
`src/game/cf_mii.cpp` (game glue, menu page), `src/game/cf_mii_data.cpp` (file formats);
NAND background in docs/NAND.md "Miis".

## Where the Miis live

The console's Mii database is the NAND file `/shared2/menu/FaceLib/RFL_DB.dat`, i.e.
`<data folder>\nand\shared2\menu\FaceLib\RFL_DB.dat` (portable mode and every data folder have their
own; tests use `CF_NAND_DIR`). Without the file the game's RFL library uses its empty-database
fallback (as on a console whose Mii Channel was never opened). The first save in F1 > Miis creates
the file; a player's own `RFL_DB.dat` (from a Wii NAND dump or Dolphin's `Wii\shared2\menu\FaceLib\`)
can be copied there or imported.

The editor's Miis made on this "console" get a console (system) ID stored in settings.ini
`[mii] system_id` (random, made once; RFL only needs it non-zero) and Mii IDs as the Mii Channel
makes them (bit 31 set = an ordinary Mii, the time in 4-second steps since 2006 in the low 29 bits).

## The file format (read from the game's RFL code)

Everything below was read from City Folk's own RFL library (USA rev 1; the same SDK library in all
four versions), not from Mii code of any other project:

| What | Where in RFL | Format |
|---|---|---|
| Database layout | RFLiFormatAsync, RFLiClearDBBuffer, RFLiGetCharData | `RNOD`, 100 records of 0x4A bytes at 0x4 (empty slot: Mii ID and system ID 0), a word at 0x1CEC with bit 31 set, the parade table `RNHD` at 0x1D00 (two list heads 0xFFFF, 10000 entries of 12 bytes with links 0x7FFF), the CRC at 0x1F1DE; RFL reads 0x1F1E0 bytes at boot. After it the hidden-Mii area: 10000 x 0x40 bytes (file 0xBB5E0 bytes as RFLiFormatAsync writes it) |
| CRC | RFLiCalculateCRC, RFLiCreateHeaderCRCAsync, bootloadCheckCRCCb_ | CRC-16, polynomial 0x1021, initial 0, over the message plus two zero bytes (CRC-16/XMODEM); the database CRC covers 0..0x1F1DE and is stored big-endian at 0x1F1DE; a bad CRC = broken database (RFL flags it) |
| Record (RFLiCharData, 74 bytes) | convertRaw2InfoCore_, convertInfo2RawCore_ | 0x00 u16 gender (bit 14), birth month (13-10), day (9-5), favourite colour (4-1), favourite (0); 0x02 name, 10 UTF-16BE; 0x16 height, 0x17 weight; 0x18 Mii ID; 0x1C system ID; 0x20 face shape (15-13), skin (12-10), features (9-6), "don't mingle" (2), downloaded (0); 0x22 hair (15-9), colour (8-6), part flipped (5); 0x24 eyebrow type (15-11), rotation (10-6); 0x26 eyebrow colour (15-13), size (12-9), height (8-4), spacing (3-0); 0x28 eye type (15-10), rotation (9-5), height (4-0); 0x2A eye colour (15-13), size (12-9), spacing (8-5); 0x2C nose type (15-12), size (11-8), height (7-3); 0x2E mouth type (15-11), colour (10-9), size (8-5), height (4-0); 0x30 glasses type (15-12), colour (11-9), size (8-5), height (4-0); 0x32 mustache (15-14), beard (13-12), colour (11-9), mustache size (8-5), height (4-0); 0x34 mole on (15), size (14-11), height (10-6), sideways (5-1); 0x36 creator, 10 UTF-16BE |
| Limits | RFLiCheckValidInfo | face 0-7, skin 0-5, features 0-11, hair 0-71, colours 0-7, eyebrows 0-23 (rotation 0-11, size 0-8, height 3-18, spacing 0-12), eyes 0-47 (colour 0-5, size 0-7, rotation 0-7, height 0-18, spacing 0-12), nose 0-11 (size 0-8, height 0-18), mouth 0-23 (colour 0-2, size 0-8, height 0-18), glasses 0-8 (colour 0-5, size 0-7, height 0-20), mustache/beard 0-3 (size 0-8, height 0-16), mole size 0-8, height 0-30, sideways 0-16; height/weight up to 128; a name; month and day both set or both 0, a real date (February 29 allowed); a special Mii (ID bit 31 clear) may not have "don't mingle" |
| What RFL shows | RFLiGetCharData, RFLiIsValidOnNAND | a slot with an ID, valid by the limits, not a temporary Mii (ID bit 29) |
| Store data (.rsd) | fn_802C4D0C, RFLAddMiddleDBStoreData | the record + its CRC-16 (76 bytes; CRC over all 76 = 0) |

The editor writes only the 100 records and the CRC into the file image it read, so everything else a
real console's file holds (parade list, hidden Miis) is kept as it was.

## Saving, and how the running game sees it

RFL reads the database once, at boot (RFLInitRes -> RFLiBootLoadDatabaseAsync), into a buffer in
its heap and answers everything later from that buffer; City Folk never opens the file again. So
"Save to console" does two things:

1. The file: written to a temporary file and renamed over the old one, under the NAND file system's
   lock, and refused (with a message) while the game has the file open (`ios::writeHostFile`).
   A save is never half written.
2. The game: at a safe point of a guest frame (RFL initialised and idle: no asynchronous file or
   resource operation, `RFLiIsWorking`; and Shampoodle's Mii list `d_menu_miiNP` not open, so a list
   the game is showing never changes under it) the saved bytes replace RFL's buffer (the 0x1F1E0
   bytes RFL read at boot) and RFL's "file not found" / "needs repair" / "broken" flags (manager
   +0x1B3C bits 0-3, offset read from RFLiNotFoundError at run time) are cleared, because the file
   now exists and is valid. RFL is then exactly as if it had read this file at boot. Verified: a
   console without the file boots with flag 01 (not found); after the save `[mii] database handed
   to the game's RFL at frame ... (2 Miis; RFL flags 01 -> 00)` and Harriet offers the makeover with
   those Miis without a restart (without the hand-over she says there are no Miis).

The game's own Mii list (its Mii manager at USA 0x80587980, filled from RFL by a worker thread,
fn_8008B740) is refreshed when a scene loads: Miis saved while the player is already in Shampoodle
reach Harriet after leaving the shop and coming back (seen: saved after the warp into the shop ->
"there are no Miis"; saved before entering -> the Mii list with them). The hint says so.

A Mii mask the player already owns is a copy of the Mii in the town save (the game's Mii manager
keeps RFL store data, fn_8008B3A8 -> RFLAddMiddleDBStoreData), as on the console: editing or
deleting the Mii later does not change a mask already made.

If the file changes on disk while the game runs (copied in by hand), the page reloads it when
nothing is being edited; the game sees such a file only at the next start (or the next save from
the page).

## The preview

Drawn by the game's RFL with the disc's Mii parts (`Face/RFLRes01.arc`, which the game loads):

- The edited Mii goes into a one-Mii RFL "middle database" (RFLInitMiddleDB type 4 + RFLAddMiddleDBStoreData,
  the way the game keeps a worn mask, fn_8008B3A8).
- RFLMakeIcon (data source 7 = middle DB) builds the 3D head (RFLiInitCharModel, the face texture by
  RFLiMakeTexture), draws it into the EFB with RFL's own camera, light and draw calls, and copies it to
  an RGB5A3 texture with GXCopyTex (RFLiSetupCopyTex first copies the region with clear to empty it,
  then the finished icon). This is the path and the moment the game uses for its own Mii icons: its
  icon queue fn_8000A57C at the start of the stage draw (d_s_stageNP fn_125_C0); the hook runs ours right
  after the game's, with the game's arguments (transparent background, opaque + translucent parts).
- The turn: RFLSetMtx is hooked; while our icon draws, a rotation about the head's vertical axis is
  multiplied into the model matrix RFLiMakeIcon sets. Expressions (normal, smile, angry, sad,
  surprised, blink) are RFLMakeIcon's own expression argument.
- The texture buffer (in the HLE heap, outside the game's memory) is a watched EFB copy range, so
  Aurora reads it back into guest memory even with "EFB copies to RAM" off; the page decodes the
  RGB5A3 tiles and shows them as an ImGui texture, updated in place (Aurora 0410
  `aurora_imgui_update_texture`).
- Size: 256 px when RFL's heap has room for the model RFLiMakeIcon allocates there (checked first with
  MEMGetAllocatableSizeForExpHeapEx; ~149 KB free in town), else 128 or 64.
- A copy that never lands (with frame interpolation on a loaded GPU fewer game frames are shown
  than made, and the EFB copies of a frame never shown are never made; seen at 15-19 fps shown
  with other game instances on the GPU) frees its buffer after 60 frames and the newest request is
  drawn again (`[mii] preview N did not land: drawn again` with CF_MII_LOG), so the preview never
  stays empty.

The preview needs a scene with the stage draw (the title town, the town, buildings, the city); on
other screens the page says so.

### Windows wider than 16:9 (the black-face bug)

User report (main 0c37043, 4K, Enhanced lighting Ultra): "Mii maker is not working": the preview
head had no face, a black hole with only the nose and a strip of forehead (from behind: black
polygons with a few skin fragments). Not a lighting regression: it needs a window wider than 16:9
with F1 > Visuals > Aspect "Match window" (the user's maximized window was 3840x2131, aspect 1.802,
widescreen factor 0.987). Reproduced in town at 3840x2131 with the user's settings; Enhanced
lighting, Enhanced world, EFB copies to RAM, 60/120 fps and texture packs switched live
(CF_SETTINGS_AT) changed nothing, a fixed 16:9 aspect or a 3840x2160 window gave the right face.

Cause (GX HLE, `hle_gx.cpp`): RFL records each head shape as a display list
(RFLiInitCharModel -> RFLiInitCharModelRes -> RFLiInitShapeRes: GXBeginDisplayList, GXBegin and
the vertices, GXEndDisplayList) whenever it builds a model, with whatever projection and viewport
are current (the HUD's orthographic one in town). With a widescreen factor below 1 the HLE treated
those GXBegin calls like 2D HUD draws: held and given the centered projection, and `prepareDraw`
emitted that projection (and scissor) into the list being recorded. Every later call of the list
then switched to the HUD's projection in the middle of the head, so the face mask (and parts of
the hair) landed somewhere else: the hole. Whether a list got the projection depended on the state
left by the draws before it: the title town and the game's own "Choose a Mii" icons happened to
be clean on main, and an interim fix that changed that state (RFL's renders drawn unadjusted)
broke the list's first icon the same way.

Fix: nothing widescreen-related is emitted or held while a display list is recorded
(`prepareDraw` returns at once, GXBegin does not hold); the projection for a list's draws is chosen
where the game calls it, as before. A list holds only what the game writes, as on the console.
That covers every Mii head (F1 preview, the game's icons, the worn mask) and any other game list
recorded under a 2D projection. Test aid `CF_DL_WS_OLD=1` restores the old behaviour (A/B).

## Import and export

- Import (Import...): a whole `RFL_DB.dat` (a Wii NAND dump's or Dolphin's): "Add the new ones" (keeps
  the console's Miis, adds those whose Mii ID + console ID is not here yet, up to 100) or "Replace
  all" (the console's Miis become the file's, in the file's slots); a single Wii Mii: `.mii` /
  `.miigx` (the 74-byte record) or `.rsd` (76 bytes with CRC, checked). A Mii already here (same IDs)
  is replaced by the file's version. Every imported Mii is checked against RFL's limits first; one RFL
  would reject is refused with the reason. Imports are edits: they reach the file and the game with
  "Save to console".
- Export: the selected Mii as `.mii` (74 bytes, the format of the Wii Mii exchange files); "Export
  all..." writes the editor's database (including unsaved edits) as an `RFL_DB.dat` for Dolphin or a Wii.
- 3DS / Wii U / Switch Miis (CFSD / FFSD / Ver3 store data, QR codes) are **not** imported: QR
  payloads need Nintendo's keys, and the plain Ver3 data uses part numbers that are not the Wii's
  (more parts, other order); a correct conversion needs a verified part-by-part mapping table, which
  this branch does not have. Not done rather than done approximately without a check.

## Shampoodle hint

Harriet's makeover asks the game's own Mii list; with no Miis she says "there are no Miis to base
your makeover on ... create a Mii in the Mii Channel". The port does not change that text: while the
player is in Shampoodle (Harriet's module `d_a_npc_sp_catherineNP` linked) or the Mii list is open
and the console has no Miis, a small notice at the top of the screen says the makeover needs a Mii
and points to F1 > Miis ("... and save, then leave the shop and come back"). Hidden while the F1 menu is open. (Harriet's "no Miis" decision comes
from the game's Mii manager, which builds its list from RFL outside her code, so the hint covers
the whole visit rather than that one line.)

## The game's makeover (what was verified)

Route `tools/routes/mii_mask.txt` (user-town fixture, `CF_INPUT_CLOCK=frames`): continue, warp to
Shampoodle (`WARP=city.shampoodle`), Harriet's first-visit greeting, talk: "Hairstyle! / Makeover! /
Never mind." -> Makeover (D-pad down twice from the pointer's choice, A) -> "free to first timers!
Wanna try?" -> Yeah! -> "take a seat in front of the mirror": the barber chairs face the mirror on
the right wall; the route puts the player on that side of the lower chair (room position
352,0,380) and walks into it (sits) -> "Can you pick out a makeover model from the album?" -> the
game's Mii list "Choose a Mii" (`d_menu_miiNP`) shows the console's Miis with icons the game's
RFLMakeIcon drew from our database -> the first Mii (pointer + A), Confirm -> "So, you'd like to try
the Robo style?" -> "That's the one!" -> the makeover -> "Ta-daaa! All finished!": the player
wears the Mii head (`RFLGetAdditionalInfo` from d_a_playerNP fn_80_2B238). Verified 2026-11-03 on
USA with two Miis made by `CF_MII_AT` before the warp (Robo = the same random Mii as the preview
shots: same face in the preview, the list icon and the mask). Without Miis the same route ends in
Harriet's "there are no Miis ... create a Mii in the Mii Channel" and the hint. Run:
`tools/routes/run.ps1 -Name mask -Route tools/routes/mii_mask.txt -Seconds 560 -Shots 9500,14500,40
outes
un.ps1 -Name mask -Route tools
outes\mii_mask.txt -Seconds 560 -Shots 9500,14500,40
-Env "CF_FIXTURE=user-town;CF_MII_SEED=7;CF_MII_AT=4000:random|4010:name=Robo|4100:random|4200:save"`
(on a loaded PC give it more seconds; the makeover is ~frame 11300).

## Test aids

- `CF_MII_LOG=1`: database reads/saves, hand-overs to RFL, preview renders and read-backs, the Mii
  list opening/closing, RFL queries while Harriet is there, the hint.
- `CF_MII_AT=<frame>:<cmd>|<frame>:<cmd>...` (`;` also separates): editor commands at guest frames,
  run on the host side as the menu would: `new`, `random`, `name=<text>`, `set=<field index>=<value>`
  (cf_mii_data.h `F` order), `select=<slot>`, `yaw=<degrees>`, `expr=<0-5>`, `save`, `delete`,
  `duplicate`, `import=<path>[,merge|,replace]`, `export=<path>`, `exportdb=<path>`,
  `shot=<path.ppm>` (the current preview over dark grey), `open` (opens F1 > Miis). The preview keeps
  rendering while a script runs with the menu closed.
- `CF_MII_SEED=<n>`: deterministic Random button.
- `CF_DL_WS_OLD=1` (GX HLE): widescreen projections emitted into display lists being recorded
  again (the black-face bug, "Windows wider than 16:9"), for A/B runs. Reproduce with
  `--window 3840x2131`, `[video] resolution_scale=0`, aspect "Match window", in town.
- Example (town, two Miis, saved, preview shots turned and smiling):
  `CF_MII_AT=5400:random|5410:name=Robo|5600:shot=p0.ppm|5610:yaw=45|5700:shot=p1.ppm|5710:expr=1|5800:shot=p2.ppm|5900:save`
