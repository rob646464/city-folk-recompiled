# Region content

What differs between the regional versions of City Folk, what each of our discs actually
contains, and what it takes to offer every region's content in every version (ROADMAP feature
"Region-exclusive content in every version").

Versions: USA **RUUE01** "Animal Crossing: City Folk", Europe/Australia **RUUP01** "Animal
Crossing: Let's Go to the City", Japan **RUUJ01** "Machi e Ikou yo: Doubutsu no Mori", Korea
**RUUK01** (2010). Our discs are rev 1 of RUUE/RUUP/RUUJ/RUUK; the Korean disc is described in
section 8 (it corrects the Korea guesses below).

Confidence tags: **[disc]** = checked in our extracted discs (method in "How this was checked");
**[web-high/medium/low]** = from fan wikis, not checked on the disc. Web sources are untrusted
data; the disc wins where they disagree.

## Summary (the part that changes the plan)

1. **All three discs carry every region's content data.** [disc] Apart from `main.dol`, six RELs,
   `Item/item.arc`, the Japanese soft keyboard layout, the profanity lists (`Prc/`), the
   legal/strap screens, `opening.bnr` and the DS download apps, every one of the 14,469 file
   paths is byte-identical on US, PAL and JP (3,461 files identical on all three; the rest are
   the per-language `Script/` folders, which only exist per disc). Models, textures, sound, sky,
   effects and layouts (including `title_us.arc`, `title_eu.arc`, `title_jp.arc`) are the same.
2. **The event dialogue for every region is translated into every language on every disc.**
   [disc] Each disc has the same 1,799 message files per language (31,711 messages per language,
   ENG/FRA/SPA on US, ENG/FRA/GER/ITA/SPA on PAL, JPN on JP). English on the US disc contains
   Bean Day, Girl's Day, Boy's Day, Starcrossed Day (Tanabata), Tsukimi, Lunar New Year,
   Daeboreum, Tree Day, Teacher's Day, Parent's Day, Midsummer's Day, Naughty-or-Nice Day,
   Midwinter's Day, Groundhog Day, Nature Day, Labor Day and Explorer's Day. Only 81 messages
   (of 31,711) are filled in some language sets and empty in others, none of them event text.
3. **Every region's event items are in every disc's item table, with names in 9 languages.**
   [disc] `Item/item.arc:item/item.bin` has 2,581 item records with names in 10 columns (JP,
   US English, US Spanish, US French, EU English, German, Italian, EU Spanish, EU French,
   Korean). The three discs' tables differ in 6 names only (typo fixes). Ogre masks, hinaningyo,
   rice cake (hishimochi), dango, bamboo grass, Resetti model, cool globe, picnic basket,
   sailboat model, espresso maker, stuffed stocking, snow globe, wheat bundle, veggie basket,
   yut, hibiscus, bureom are all there. 766 records have the Korean placeholder name
   `ブランクかんこく` ("blank Korean"), including every Korean event item: the Korean disc must
   carry its own table.
4. **The RELs are region-neutral; the region is a runtime value in main.dol.** [disc] 181 of
   187 RELs have byte-identical sections on US/PAL/JP (only header/relocation bookkeeping
   differs); the NPC/event RELs (Tortimer `d_a_npc_sp_event_sonchoNP.rel`, Franklin, Jingle,
   etc.) are among them. US `main.dol` has a region getter at **0x801069D0** (`li r3,1; blr`;
   PAL `li r3,2` at 0x80106B30, JP `li r3,0` at 0x80106B64; Korea presumably 3) with 13 direct
   callers in main.dol plus calls from `d_menu_noticeNP.rel`. Its callers index region tables
   (e.g. 0x800D20A4: `table[region*16 + n]`), branch on it (0x80145A4C: `region == 1`), and
   compare it with a region nibble stored in the save (0x80104970, save byte +0x35C2 & 0xF).
   The villager "moon" dialogue table at 0x804A2288 orders its variants JPN, KOR, USA, EUR.
   The `main.dol` files still differ in size (US 5,667,072 / PAL 5,678,336 / JP 5,684,544
   bytes), so some region code may be compiled in or out; that is unverified.
5. Consequence for the port: **region-exclusive events and items need no data copied from
   other discs and no new translation** (except Korean-language text, which nobody asked for).
   They need event-calendar hooks: make the event code see another region where the user enabled
   that region's events, per event, never globally (the region also goes into saves, online
   matching and the WiiConnect24 URL). The ROADMAP's "Needs the matching discs where content is
   only on another region's disc (greyed out otherwise)" does not apply to holidays/items; it
   applies only to **Nintendo-distributed items**, which were never on any disc (section 3).

## 1. Holidays and events

### 1.1 Events that exist only in some regions

Dates and rewards [web-high, Nookipedia: Event/City Folk and each event's page]; dialogue and
items [disc] (file = where the dialogue lives, on every disc, every language; item = item.bin
record index and US English name).

| Event | Region | Date rule | Host / reward | Dialogue on disc | Reward item on disc |
|---|---|---|---|---|---|
| Groundhog Day | US | Feb 2 | Tortimer, Resetti model | `npc_SP/NPC_soncho_1.bmg`, `BBS_event` #0x28 | #2294 Resetti model |
| Nature Day | US | Apr 22 | Tortimer, cool globe | soncho_1, BBS #0x29 | #2295 cool globe |
| Labor Day | US | 1st Monday of Sep | Tortimer, picnic basket | soncho_1, BBS #0x2A | #2296 picnic basket |
| Explorer's Day | US | 2nd Monday of Oct | Tortimer, sailboat model | soncho_1, BBS #0x2B | #2297 sailboat model |
| Midsummer's Day | EU | Jun 21 | Tortimer, espresso maker | soncho_1, BBS #0x2D | #2299 espresso maker |
| Naughty-or-Nice Day | EU | Dec 6 (BBS on Dec 5: "Tomorrow is...") | Tortimer, stuffed stocking | soncho_1, BBS #0x2E | #2300 stuffed stocking |
| Midwinter's Day | EU | Dec 21 | Tortimer, snow globe | soncho_1, BBS #0x2F | #2301 snow globe |
| Bean Day (Setsubun) | JP | Feb 3 | Tortimer, red or blue ogre mask | soncho_1, BBS #0x22 | #1384/#1385 red/blue ogre mask |
| Girl's Day (Hinamatsuri) | JP | Mar 3; Nook sells hinaningyo Mar 1-3 (2,560 Bells) [web-high] | Tortimer, rice cake | soncho_1, BBS #0x23 | #2291 rice cake, #2276 hinaningyo |
| Boy's Day (Kodomo no Hi) | JP | May 5 | Tortimer, newsprint helmet | soncho_1, BBS #0x24 | #1294 newsprint helmet |
| Starcrossed Day (Tanabata) | JP | Jul 7 | Tortimer, bamboo grass | soncho_1 | #2293 bamboo grass |
| Lunar New Year (Seollal) | KR | 1st day of the lunar year | Tortimer, yut | soncho_1 | #2303 yut (no Korean name on our discs) |
| Daeboreum | KR | 15th day of the 1st lunar month | Tortimer, bureom; villagers' moon talk | soncho_1, `*_FreeA_MoonKOR.bmg` (6 personalities) | #2305 bureom |
| Tree Day (Sikmogil) | KR | Apr 5 (BBS the day before) | Tortimer, hibiscus | soncho_1, BBS #0x32 | #2304 hibiscus |
| Parent's Day | KR | May 8, replaces Mother's Day [web-high] | letter from the parents | BBS #0x33, `MAIL_ETC_MotherEvent` #7/#8 | carnation [web] |
| Teacher's Day | KR | May 15, replaces Father's Day [web-high] | Tortimer (red carnation [web]) | soncho_1, BBS #0x34 | #1466 red carnation |

[disc] The Korean-only events are fully written in English, French, Spanish, German, Italian and
Japanese on our discs even though no disc of ours ever shows them. The KR moon talk
(`*_FreeA_MoonKOR.bmg`) is Daeboreum's full moon, not the autumn moon.

### 1.2 Shared events that differ by region

| Event | Difference | Source |
|---|---|---|
| Autumn Moon (15th day of the 8th lunar month) | Gift: wheat bundle (US, #2298), veggie basket (EU, #2302), dango (JP "Tsukimi", #2292); not held in KR. Villager talk: `*_FreeA_MoonUSA/EUR/JPN.bmg`, all on every disc. | [web-high] dates/gifts, [disc] text+items |
| Mother's Day | US/JP: 2nd Sunday of May. KR: May 8 (Parent's Day). EU by system language: English = Mothering Sunday (4th Sunday of Lent), German/Italian = 2nd Sunday of May, French = last Sunday of May, Spanish = 1st Sunday of May. | [web-medium] (mapping read from flag icons) |
| Father's Day | US/JP/EU-English/EU-French: 3rd Sunday of June; EU-Spanish/Italian: Mar 19; EU-German: Ascension Day; KR: replaced by Teacher's Day. | [web-medium] |
| Bulletin board / letters | UK English rewrites (e.g. "pants" -> "trousers"); 859 US-ENG vs PAL-ENG messages differ. NA French/Spanish are separate translations from EU French/Spanish (9,556 / 8,116 messages differ). | [disc] counts, [web-medium] examples |

The game keys the per-language holiday rules off its internal language index (below), so these
can be offered as "use the date rule of language X" without new text.

### 1.3 Shared events with no reported regional difference

New Year's Day (Jan 1), Valentine's Day (Feb 14), Leap Day (Feb 29, BBS only), Festivale
(Monday before Ash Wednesday, Pavé), April Fool's Day (Apr 1), Bunny Day (Easter Sunday, Zipper),
cherry blossoms (Apr 1-8, no festival in City Folk), Fireworks (Sundays in August, 7 PM), Bug-Off
(Nat, 3rd Saturday Jun-Sep), Fishing Tourney (Chip, Oct-May), Flea Market (4th Sunday of most
months), Halloween (Oct 31, Jack), Harvest Festival (4th Thursday of November, Franklin), Toy Day
(Dec 24, Jingle), Countdown (Dec 31), birthdays. [web-medium]; all have BBS notices in every
language [disc]. Whether Festivale/Bunny Day/Halloween/Harvest Festival/Toy Day run in the JP and
KR builds is unverified (the RELs are the same code; what decides it is the region value).

### 1.4 Other region differences

| What | Detail | Confidence |
|---|---|---|
| Villager house exteriors | Five preset sets per town, two region-specific: JP/KR Zen + Farmhouse, US Ornate + Pink, EU Brick + Log. `d_a_str_fieldNP.rel` selects `obj_house_usa` / `obj_house_eur` / `obj_house_jpn` (no `_kor`: KR uses JP's), all models on every disc. | [disc] model names, [web-medium] mapping |
| Town/city sign textures | `Str/Brres/` has town textures and field structures for every region+language: Usa{Eng,Fra,Spa}, Eur{Eng,Fra,Ger,Ita,Spa}, JpnJpn, **KorKor** (summer S / winter W), shop "sold out" signs `tex_JpnJpn_*`, `tex_KorKor_*` in `FgObj/fgItem.brres`. Identical on all discs. | [disc] |
| Title screen | `Layout/title_us.arc`, `title_eu.arc`, `title_jp.arc` on every disc; `d_title_demoNP.rel` differs only in that file name. No `title_kr`. | [disc] |
| TV test card | EU shows a PAL test card, others NTSC. | [web-high] |
| Villagers | Same 210 villagers; names localized (e.g. Spork = "Crackle" in UK English). | [web-high] |
| Item names | UK vs US English names differ (mush hanger / mushroom rack, aubergine cow, ...); item.bin has both columns. | [disc] |
| Rover's kanji/hiragana question, JP mailbox | `SYS_Bus.bmg` #2-#4 exist only in Japanese; `NPC_tel.bmg` has an English kanji/hiragana question that never shows. | [disc] |
| Japanese soft keyboard | `Layout/sofkeybd.arc` differs on JP (kana). | [disc] |
| Profanity filter lists | `Prc/E.bin` (US, 1,226 B), `P.bin` (PAL, 4,679 B), `J.bin` (JP, 428 B), compressed (`PL` header); updatable over WiiConnect24 (`prc_list.bin`). | [disc] files, purpose [inferred] |
| Insect code | `d_insect_fieldNP.rel` section 1 is 0x1C0 bytes larger on US than PAL/JP: some US-only insect behaviour or fix; content unknown. | [disc] size, meaning unknown |
| Wild World moving van | EU rev 1 accepts NA Wild World cards (fix for Australia). | [web-medium] |
| Revisions | US rev 0/1, EU rev 0/1/2, JP rev 0/1/later, KR one build; `MMSRevision.txt` US 483450, EU 483469, JP 479175, KR 8465 [web-high]; our US disc reads 483450 [disc]. Rev differences unknown. | mixed |

### 1.5 The game's language index (item names, text folder)

[disc] US `main.dol` 0x801069D8 maps the system language (SC, via 0x803AD6C8) to the game's
language index: English -> 1, Spanish -> 2, French -> 3, anything else -> 1. PAL 0x80106B38:
English -> 4, Spanish -> 5, French -> 6, Italian -> 7, German -> 8, Dutch/other -> 4. JP is
presumably 0, KR 9. The 10 name columns in item.bin are ordered JP, US-EN, US-ES, US-FR, EU-EN,
DE, IT, EU-ES, EU-FR, KR [disc, read from the names]; the language-index-to-column order for
indices 5-8 does not obviously match that column order: check the item-name accessor before
relying on it.

## 2. Furniture, clothing, items

[disc] The item table is effectively the same on all discs (6 name differences). So **every item
of every region exists in every version**, with names in all 9 non-Korean languages; what makes
an item "exclusive" is only how it is obtained (event gift, seasonal shop stock, distribution).

Region-exclusive by obtaining, all present on every disc:
- Event gifts in 1.1 and 1.2 (17 items).
- Hinaningyo, sold by Nook Mar 1-3 in JP only [web-high].
- Kadomatsu (#2274) and kagamimochi (#2275) have no Korean name (like the JP-only items); how they
  are obtained per region is unverified.

Unused items on every disc (not regional) [web-medium]: Wild World leftovers (Nintendo bench,
Pikmin, Blue Falcon, brown pot), "not used" placeholders (e.g. #2318/#2319), unobtainable
invisible chair, wilted golden roses.

## 3. Nintendo-distributed items (WiiConnect24)

[web-high, Nookipedia "Distributed item/City Folk"; their list may be incomplete] Delivered by
Pete as a letter from Nintendo while WiiConnect24 was on; designs came from Wendell. 91 unique
items (59 furniture, 21 hats, 1 accessory, 2 umbrellas, 3 wallpapers, 5 floors) plus 10
designs; Dec 2008 - Dec 2012 (WiiConnect24 ended June 27/28, 2013).

[disc] **None of the distributed items is on any disc**: names such as Eiffel Tower, carp banner,
tokonoma, Jingle TV, anniversary cake, Golden chair are absent from item.bin and from every file.
The game downloads them: US `main.dol` has `http://nwcs.wapp.wii.com/ruu/rvforestdl_us.enc`
(PAL `_eu`, JP `_jp`; KR presumably `_kr`), stored through the NWC24 download task
(`/shared2/wc24/nwc24dl.bin`, `wc24dl.vff`) and verified with `wc24pubk.mod`. The `.enc`
archives (encrypted U8, key files `rvforestdl.aes.bin` / `rvforestdl.pem.bin` per a WiiLink
repository README [web-medium]) carry the item's model, texture, names and the letter.

Per region [web-high; date ranges summarised]:

| Region | Exclusive items (examples) | Count (approx.) |
|---|---|---|
| JP only | top, chocolate heart, Girl's Day updo, tokonoma, student cap, carp banner, ghost umbrella, morning glory, eggplant cow, celebration hat, red/white team hats, hagoita, emperor's cap, blossom lantern, Kintaro wig, tsunokakushi, cucumber horse, sporty wall/floor | 21 |
| US only | Nintendo DSi B, 1,000 Bells, ladder shades, hot dog hat, dolphin model, hopscotch floor, pile of leaves, election poster, shopping cart, Nintendo DS Lite, snowman head, fedora chair, cupid bench, wildflower floor, guard's helmet, banana split hat, candy, Kapp'n model | 18 |
| EU only | flamenco hat, green headgear, sand castle, Eiffel Tower, Labrador/Chihuahua/Dachshund/Dalmatian models, tam o'shanter | 9 |
| KR only | flower bouquet, kimbap plate, white school cap, school cap, red horned hat, samgyetang bowl, shaved ice lamp, white police cap, maple umbrella, tteok plate, red nose | 11 |
| Several | Red Pikmin, shamrock hat, DSi W, bus model, GameCube dresser, anniversary cake, Wii locker, festive wreath, Pavé clock, egg TV, Gracie dresser, sweets player, snowman vanity, mushroom rack, Jingle TV, the 12 golden items (2011), the 10 creepy items (2012) | ~40 |

What bringing them back needs:
- **The files themselves.** They are not on the user's disc, so the setup cannot derive them.
  Options: (a) WiiLink's City Folk distribution service, which serves original files to consoles
  today [web-high: wiilink.ca/services/animalcrossing]; the port's WiiConnect24 HLE would fetch
  them like a console (docs/ONLINE.md); (b) a user-supplied folder of `.enc` files. We must not
  ship them (Nintendo data).
- **Each region's DLC file only carries that region's languages** [inferred: unverified]. A US
  player receiving a JP-only or KR-only item would see a Japanese/Korean name and letter. That is
  where **new English text** would come from us: names for ~21 JP-only + ~11 KR-only items (each
  name max 16 UTF-16 characters, item.bin's field size) and one Nintendo letter each (letters are
  about 5-8 lines of 24 characters). Roughly 32 names + 32 letters, about 6,000 characters of
  English; the same again per other language if wanted. Nookipedia's English names can serve as
  a reference for the names (they are the community's translations, not Nintendo's).
- Distributed designs (Wendell) are in the same files.
- Distributions were tied to dates; the port can offer them as "letters from Nintendo" on a
  calendar (original dates, yearly) or on request (debug/Unused tab).

## 4. NPC and villager differences

- Special NPCs: no region-locked NPC found. Franklin (Harvest Festival), Jingle, Pavé, Zipper,
  Jack exist in every region's data; Korean and Japanese events use Tortimer [disc: dialogue].
- `STR_SPNpc_name.bmg` (53 special NPC names) is the same list in every language [disc].
- Region differences in Dr. Shrunk's jokes, Kapp'n's songs, K.K. songs: none reported
  [web: not found]; `Sound/rv_forest.brsar` is identical on all discs [disc], so music cannot
  differ by data.

## 5. City and shops

No city/shop difference reported (Club LOL, Shampoodle, GracieGrace, Kicks, Redd, auction house,
theater, HRA) [web: not found]. City field data and `Str/` structures are identical on all discs
[disc]; only signage textures exist per language.

## 6. How this was checked

Readers in `tools/research/` (`u8.py` list/extract U8 archives, `bmg.py` dump messages,
`items.py` dump item.bin as TSV; run them on the extracted disc, write output outside the repo).
One-off scratch scripts not committed: whole-disc MD5 diff, REL section diff, a DOL
disassembly helper (capstone).

- **File trees:** `sys/fst.bin` trees and file hashes of `US DATA/DATA`, `PAL DATA/DATA`,
  `JAPAN DATA/DATA`. Script folders: `Script/<LANG>/` with `MsgList.txt`, `MMSRevision.txt`,
  `npc_1_FU` ... `npc_6_KO` (villager personalities FU/GE/TA/BO/HA/KO, 285 files each), `npc_SP`
  (72 special NPC files), `sys_MESS` (14), and `script.arc` (U8: `sys_STRING`, `sys_2D`,
  `sys_SELECT`, `sys_BBS`, `sys_MAIL1`, `sys_MAIL2`).
- **BMG layout:** `MESGbmg1`, encoding byte 2 (UTF-16BE), sections INF1 (entry size 0x14:
  u32 text offset + 16 attribute bytes), DAT1 (text), MID1 (message ids). Message 0 holds the
  source `.gmm` name. Control codes: `0x1A, len, group, ...` (e.g. `{ff0000xx00}` colour,
  `{0600...}` pauses, `{0500xx}` player/town name, `{04...}` item/variable placeholders whose
  argument is a Japanese description string).
- **item.bin:** u32 count 0x0A15 (2,581), u32 record size 0x18C, records from 0x20 starting
  `BITM`; record +0x12: 10 names of 0x22 bytes (UTF-16BE, 16 chars + NUL), then attributes.
- **Region getter / tables:** found by matching the main.dol call targets of identical REL call
  sites across the three DOLs and keeping the tiny functions whose bodies differ.

## 7. Bringing exclusive content to another version: checklist per kind

| Kind | Data | Text | Code |
|---|---|---|---|
| Region events (1.1) | on every disc | every language (none for Korean language) | hook the event calendar / region checks so the chosen events run; Tortimer's gift = existing item ids |
| Autumn Moon gift variant | on every disc | every language | pick variant by user setting instead of region |
| Mother's/Father's Day rules | n/a | every language | date rule by user setting |
| House exterior sets | on every disc | none | allow other regions' sets for new towns (existing towns keep theirs) |
| Title screen / test card | on every disc | none | cosmetic option |
| Distributed items | **not on any disc**; WiiLink service or user files | JP/KR-only items: ~32 names + ~32 letters to translate | WiiConnect24 download HLE + DLC decryption, or a local loader |
| Korean-language anything | needs the KR disc (text, Hangul font, KR names) | Korean | not planned |

Rules for the hooks (engine): never change the global region (it is saved, compared on load and
selects online/download URLs); default = the disc's own events only; each other-region event set
is a separate F1 toggle (Enhancements > Gameplay "Holidays from other regions: US / EU / JP /
KR"); where two enabled events fall on one day (e.g. JP Bean Day Feb 3 and a lunar KR event,
or Mother's Day and KR Parent's Day in the same week) a priority rule is needed: the disc's own
event wins, the other event is skipped that year. Which events can share a day has to be checked
against the event scheduler once it is found (unverified).

## 8. Korean disc (RUUK01)

[disc] unless tagged. Branch korea-1, 2026-10-27: the user's Korean disc, extracted at
`KOREA DATA/DATA` and as the original RVZ. This section supersedes the earlier guesses about
Korea above ("we don't have it", "Korea presumably 3/9", "the Korean disc must carry its own
table", "Korean-language anything needs the KR disc (Hangul font)"): where they disagree, this
section wins. Scratch scripts (not committed): whole-tree MD5 diff of the four discs, item.bin and
BMG comparisons with `tools/research/` readers, a brfnt character-map reader.

### 8.1 Which disc it is (revision)

- Title "타운으로 놀러가요 동물의 숲" (Tauneuro Nolleogayo: Dongmurui Sup), (c)2008-2010, game id
  `RUUK01`, maker 01. **Revision 1**: the outer disc header (image header, `DATA/disc/header.bin`)
  has version byte 1, DolphinTool reports Revision 1 and the RVZ's MD5 matches GameTDB's RUUK01
  Rev 1. Only the game partition's own header (`DATA/sys/boot.bin`) says 0; on US/PAL/JP both say 1.
  Setup reads the outer header (docs/SETUP.md "Discs").
- Other evidence of the build: TMD title version 0 (also 0 on the other three, so it does not
  separate revisions), title id 00010000-5255554B, **IOS 48** (the other three: IOS 38),
  apploader dated 2008/02/29 (others 2007/10/03), the Korean `MMSRevision.txt` reads 8465
  (the KR disc's JPN folder 479175, as on the JP disc). The DOL's newest library build strings:
  `RVL_SDK - ENC` Jun 17 2009, `NWC24`/`MP`/`MPDL` Apr 16 2009, RevoEX 2.1plus5 (REL
  090416104702), a Korea-only `RVL_SDK - FA` (Mar 4 2009), `SC` Feb 22 2008 (others: ENC/SC
  Aug 8 2007, NWC24/MP Aug 6 2008, RevoEX 2.1plus4). Build path in `RVFOREST1NP.str`:
  `d:\Project\Revolution\RVFOREST1\KR\PRD\RVL\bin\` (JP disc: `...\JP\...`).
- Code: docs/DISCOVERY.md "Korea (RUUK01) against Japan and USA" (function differences: KPR,
  ENC, DWC/RevoEX, text spots in 30 RELs; `d_insect_fieldNP` is the international one, not
  Japan's).

### 8.2 Region and language values

- Region getter (US 0x801069D0) at **0x80106A50: `li r3,3`** (JP 0, US 1, EU 2, KR 3, as guessed).
- Language index function (US 0x801069D8) at **0x80106A58: `li r3,9`**: the Korean game always
  uses index 9 (Korean), whatever the console language. The next function maps the index through
  a 10-entry table at 0x8047EDE8 (out of range -> 4).
- The setup gives it a Korean console (country 136, language Korean, setting.txt `AREA=KOR`,
  `GAME=KR`); the game boots to Rover's bus in Korean (docs/SETUP.md "Korea").

### 8.3 File tree against the other discs

7,251 files (US 9,055, PAL 12,653, JP 5,451). 3,451 paths are byte-identical on all four discs
(models, textures, sky, effects, sound including `Sound/rv_forest.brsar`, `Str/`, `FgObj/`, NPC
models, the town sign textures incl. `KorKor`). Differences:

| What | KR disc | Notes |
|---|---|---|
| `Script/KOR/` (1,799 files + `script.arc`) | **only on KR** | the Korean text (8.5) |
| `Script/JPN/` | byte-identical to the JP disc's | the Korean disc carries the full Japanese text too (unused by its fixed language index 9; unverified whether anything reads it) |
| `Layout/title_kr.arc` | **only on KR** | the Korean logo layout (shown at the title, 8.2) |
| `Strap/strapSampleKR.arc` | **only on KR** (no `strapSample.arc`) | Korean wrist-strap screen |
| `Prc/K.bin` (1,443 B) | **only on KR** (no `J.bin`) | Korean profanity list |
| `Font/Anml_HummingStd_B_28_I4.brfnt` / `.brfna` | differs | same font plus 6 symbols (U+2033 ″, U+2103 ℃, U+226A/B ≪≫, U+300A/B 《》): 4,347 codes vs 4,341. **Every disc's font already has the 2,355 Hangul syllables and 94 jamo** (plus 167 kana, 1,022 kanji): Korean text renders with the US/PAL/JP font; only those 6 symbols would show as missing |
| `Layout/keyboard_free.arc` (44,832 vs 29,920 B), `Layout/sofkeybd.arc` (690,560 B; US/PAL 675,520, JP 656,320), `Layout/board.arc` (78,688 vs 74,976 B) | differ | Korean text-entry keyboards (Hangul composition) and the bulletin board layout |
| `Item/item.arc` | differs only in `item/item.bin` | Korean names (8.4) |
| `Item/Ftr/Ftr412.brres` | differs | the bath pail's texture: `int_kns_bth_pail` (US/PAL/JP) replaced by `int_nsb_bth_pail` (a Korea-safe print; the JP/US one is identical) |
| `Npc/Normal/Setup/pack.bin` | 2 bytes (0x11E08) | one field next to the UTF-16 string "Karin": 0xFF12 -> 0x0032; meaning unknown |
| `DSApp/*.srl`, `opening.bnr`, `RVFOREST1NP.str`, `rels/*.rel` | differ | Korean DS download apps and banner; module path strings; code (DISCOVERY.md) |

Nothing else is Korea-only: there are no Korean-only models, textures, sounds or events on the
disc beyond the files listed.

### 8.4 item.bin: Korean names

Same 2,581 records and field layout. The Korean column (10th) on the KR disc: **2,392 Hangul
names** (US/PAL/JP discs: 1,810 Hangul names + 766 `ブランクかんこく` placeholders). 583
placeholders got a name, 99 existing Korean names were corrected (full-width digits to ASCII,
spacing, renames such as T.K.매치 -> T.K.행진곡). 183 records stay `ブランクかんこく`: 178 are
records that are blank in every language, plus 5 real items without a Korean name (paper bag #14,
bulletin paper #383, grape hat #1324, laurel wreath #1368, cookie #2563; presumably not offered
in Korea, unverified). Other columns: 6 differences, the same typo fixes as on the US/PAL discs
(EU French #227, #234-#236, Italian #1445, #236), and 2 records with changed attributes (#236
mush stand, #1391 King Tut mask; also as on US/PAL).

Every region-event item now has a Korean name, e.g. #1384/#1385 빨간/파란 도깨비 가면 (ogre
masks), #2276 인형 장식 (hinaningyo), #2291 무지개떡 (rice cake), #2292 달맞이떡 (dango), #2293
소원성취 대나무 (bamboo grass), #2294 도루묵씨 모형 (Resetti model), #2298 보리 다발 (wheat
bundle), #2302 채소 바구니 (veggie basket), #2303 윷놀이 세트 (yut), #2304 무궁화 (hibiscus),
#2305 부럼 (bureom), #1466 빨간 카네이션 (red carnation), #2274/#2275 새해맞이 대나무 / 찰떡
(kadomatsu / kagamimochi).

**No item has a Korean name but no English one**: every KR item already has US and EU English
names on every disc. There are no Korea-only items on the disc (the Korea-only items were
distributed, section 3).

### 8.5 Korean text (Script/KOR)

Same 2,230 message files as the other languages (loose and in `script.arc`), the same 36,840
messages per language as US English, same `MsgList.txt` size. The Korean set is
complete for every region's events: `NPC_soncho_1.bmg` (Tortimer's event lines, 581 messages),
the BBS notices and the moon talk for all four regions (`*_FreeA_Moon{USA,EUR,JPN,KOR}.bmg`) are
filled. Differences with English (message 0, the source name, not counted):
- 370 messages are filled in English and empty in Korean. Mostly content Korea does not use:
  Mother's Day letters `MAIL_ETC_MotherAlways/Season/Day` (130; Korea has Parent's Day instead,
  `MAIL_ETC_MotherEvent` 10 more), the mysterious letter talk `*_ApA_Letter` (24), Harvest
  Festival talk `*_FreeH_Harvest` (22), some letters of the KO personality, a few `NPC_tel`,
  `SYS_Start`, `NPC_jonny` lines. 246 of the 370 are filled in the Japanese set on the same disc.
  Whether Harvest Festival / Mother's Day run on the Korean game is therefore probably "no"
  (unverified in play).
- 49 messages are filled in Korean and empty in English: the Korean chat phrases
  (`SYS_2D_Chat` #91-#107), Korean letter "From"/"To" formats and town-name suffixes (마을), and
  `STR_Mailcheck` #27-#31 (Korean word lists of the letter checker). None is event or item text:
  **nothing on the Korean disc needs translating for English players.**

### 8.6 What this gives the "Content from other regions" options

- Korean event dialogue and items need no Korean disc for English/other-language players: the
  text exists in every language on every disc (section 1.1) and the items have English names.
- Korean-language play needs the Korean disc: the Korean text, Korean item names, the Korean
  keyboards and title are only there. With the Korean disc installed as the base the game runs in
  Korean (supported region since korea-1).
- Korean item names on other discs (e.g. a "show Korean names" option, or the Korean column for a
  KR save) would come from the Korean disc's `item.bin` column 10; the fonts of every disc can
  draw them.
- Korea's own region rules (region 3: Korean house sets = JP's, KR events, Parent's/Teacher's
  Day, no autumn moon) are what the US/EU/JP games would have to emulate per event; the Korean
  game's code is the reference for those rules (same RELs, region value 3).
- Still not on any disc: the Korea-only distributed items (section 3, ~11 items; their names and
  letters would be Korean only).

## 9. What the region decides in the code (branch region-1)

[disc] USA rev 1 addresses unless noted; PAL/JP/KR addresses from `config/<id>/us_map.txt`, the
code being the same. Found with `tools/research/callers.py` (every `b`/`bl` to an address in
main.dol and every REL relocation against it) plus a disassembly of each caller. Two values
matter, and they are different things:

- the **disc region** from the getter fn_801069D0 (`li r3,N`: JP 0, US 1, EU 2, KR 3);
- the **town's language index** stored in the town data when the town is made (fn_80154F80):
  byte **+0x735C2** of the save block fn_8010DD60/fn_8010E308 return, high nibble = language
  index (0 JP, 1-3 USA English/Spanish/French, 4-8 Europe English/Spanish/French/Italian/German,
  9 Korea; from fn_801069D8 = the console language through the disc's map), low nibble = its
  region group (table 0x80475E88: 0,1,1,1,2,2,2,2,2,3). **The holidays, the house set and the
  sign textures follow the town byte, not the disc region**; the disc region is used for the
  rest below. fn_801048C0 compares the byte's low nibble with the disc region.

### 9.1 Callers of the disc region getter

| Caller | What it decides | Kind | Option |
|---|---|---|---|
| fn_801456B4 +0x398, +0x41C, +0x430, +0x444 (called by d_shop_fgNP and fn_8014406C) | Nook's seasonal item of the day: Dec 1-23 festive tree, or (USA, 50 %) festive flag / candle (item category 0x2B, #2279/#2280); Dec 24 tree (category 0x2A); Dec 26-31 kadomatsu / kagamimochi (JP, category 0x52, #2274/#2275) or flag / candle (USA), nothing elsewhere; Mar 1-3 hinaningyo (JP, 0x53, #2276) | shop stock | yes (10.3) |
| fn_800D2084 +0x20 (from fn_800D11C8 / fn_800D17C0, `MAIL_ETC_MotherEvent`) | event letter templates per region: table 0x804EC5D0[region*4 + kind], counts by language group (0x80474040); the Mother's Day / Korean Parent's Day letter from your mother | letters | follows the Mother's/Father's Day rule (10.2) |
| fn_80104C30 | profanity list `/PRC/J.bin`, `E.bin`, `P.bin`, `K.bin` (table 0x804EDBA0) | text filter | no |
| fn_801048C0 (no direct caller; vtable) | town byte's region nibble against the disc region; on a mismatch a default (0x805EBF68 +4/+8) is used for the following open (fn_801F80E8) | save-region check | never changed |
| fn_80103710 (search by name, via fn_80103D34 / fn_80103E48) | JP: kana-aware comparison (fn_8010363C), US/EU: fn_801036B4 | text sort/search | no |
| fn_801038A8 (from fn_80103AA0) | JP only: folds dakuten/handakuten kana before matching | text search | no |
| fn_8015680C via fn_80156698 / fn_8015670C (+ language index) | number formatting: digit grouping and the thousands separator (',' '.' ' ') of Bells and other numbers | number display | no |
| fn_8015C0E8 (from d_a_npc_sp_player_selectNP) | USA/Europe: a name string goes through fn_80158FBC (letter case) before display | text | no |
| wrapper fn_80085388 -> 15 functions fn_80085484 ... fn_80085CEC (all called by d_a_npc_sp_player_selectNP) | per-region record layouts (table 0x804DEA98: field offsets) and character sets (full-width to ASCII, JP/KR paths) for reading an imported character: the Wild World (DS) character move-in | DS transfer | no |
| wrapper fn_800C7D5C -> fn_800C8CA0, fn_800CA40C, fn_800CA488, fn_800CA5C4 | software keyboard layout (`KB_base_prtbl_00` etc.; JP kana pages, KR Hangul) | keyboard | no |
| d_menu_hanNP fn_148_10CC, fn_148_12FC | text-entry pages of the keyboard menu (JP-specific pages when region 0) | keyboard | no |
| d_menu_noticeNP fn_169_B8C (two calls) | USA/Europe skip a check (fn_8011AF64) that Japan/Korea make in the notice menu | menu UI | no |
| d_menu_time00NP fn_181_3EC0 (wrapper) | the date-entry menu (field order) | menu UI | no |

No disc-region caller selects events, NPCs, items in the catalog, fish or insects. The online
URLs (`rvforestdl_us/eu/jp`) are string constants per disc, not chosen at run time.

### 9.2 Readers of the town byte +0x735C2

| Reader | What it decides | Kind |
|---|---|---|
| fn_80089BD4 (in fn_80089890, the event test) | the event table's language mask (10.1) | event calendar |
| fn_800895E0 / fn_800897B0 (events #45/#46) | Mother's Day and Father's Day date rules by language (10.2) | event calendar |
| fn_80169890 (group) / fn_801698B4 (language) / fn_801698E0 / fn_80169918 | `/Str/Brres/field<Rgn><Lng>StrBrres<S/W>.arc` (fn_80166FC4: villager houses, shops' signs, bulletin board) and `<Rgn><Lng>TownTex<S/W>.brres` (fn_80166DC8: town signs); d_a_str_fieldNP fn_102_1E10: `obj_house_usa/eur/jpn` house-type data from obj_npc_common; d_str_managerNP | structures (10.4) |
| fn_800C0F88 (from fn_800C2008) | the region group 1-4 of the town for another table | other |
| fn_800D11C8 (+0x78), fn_800EC19C (+0x25C, bulletin board `BBS_event`) | language-specific letter and notice variants | letters / bulletin board |
| d_fg_drawNP fn_112_9B4 (+0x510, +0x598) | field items 0x39/0x3A drawn with a language-specific texture | field items |

The language index getter fn_801069D8 itself has 49 callers (item and message names by language
column, number/date formats, keyboards, the town byte at creation); it is the player's language
and is never changed by the port.

## 10. Content from other regions in the port

F1 > Enhancements > Gameplay "Content from other regions" (`src/game/cf_region.cpp`, settings
in `[region]` of settings.ini through a settings extension; test aid
`CF_REGION=<key>,...|all[,moon=N][,parents=N][,houses=N]`, log `CF_REGION_LOG=1`). Everything off
by default = the game as it shipped. Never changed: the disc region, the town byte, the save,
online/region checks. USA, Europe, Japan and Korea discs (each version's own function names in
tools/recomp/hooks.txt).

### 10.1 Events

The event table (USA/PAL 0x8046EEF0, JP 0x8046EE30, KR 0x80478870; identical on all discs):
47 records of 12 bytes `{u32 date test; u16 language mask; u8 kind[3]; u8 month, day; pad}`.
fn_80089890(&event, date) = "this event runs on this date" = mask test (fn_80089BD4) + the
record's date test; every user of the calendar goes through it (today's list fn_80089D3C at
0x805855E8, Tortimer fn_8008A6A0, bulletin board fn_800EC19C, shops, letters). The hook answers
it per event:

| # | Event | Mask (languages) | Date test |
|---|---|---|---|
| 0-9 / 10-13 | monthly / tourney days | all | fn_80088C34 / fn_80088BCC |
| 14-17, 22, 24, 25 | the computed-date shared events (Bug-Off, fireworks, Festivale, Harvest Festival, Bunny Day...; which record is which not checked) | all | various |
| 18-21, 23 | Valentine's Feb 14, Countdown Dec 31, New Year Jan 1, Halloween Oct 31, Toy Day Dec 24 | all | fixed |
| 26 | Bean Day Feb 3 | JP | fixed |
| 27 | Girl's Day Mar 3 | JP | fixed |
| 28 | Boy's Day May 5 | JP | fixed |
| 29 | Tsukimi (JP autumn moon) | JP | lunar table 0x8046E830 (2000-2035) |
| 30 | Tanabata Jul 7 | JP | fixed |
| 31 | Groundhog Day Feb 2 | USA | fixed |
| 32 | Nature Day Apr 22 | USA | fixed |
| 33 | Labor Day | USA | 1st Monday of September |
| 34 | Explorer's Day | USA | 2nd Monday of October |
| 35 | Autumn Moon (wheat bundle) | USA | table 0x8046E950 |
| 36 | Midsummer's Day Jun 21 | Europe | fixed |
| 37 | Naughty-or-Nice Day Dec 6 | Europe | fixed |
| 38 | Midwinter's Day Dec 21 | Europe | fixed |
| 39 | Autumn Moon (veggie basket) | Europe | same table as #35 |
| 40 | Lunar New Year | Korea | lunar table 0x8046EA70 |
| 41 | Tree Day Apr 5 | Korea | fixed |
| 42 | Teacher's Day May 15 | Korea | fixed |
| 43 | Daeboreum | Korea | lunar new year + 14 days |
| 44 | April Fool's Apr 1 | all | fixed |
| 45 / 46 | Mother's Day / Father's Day | all | by town language (10.2) |

Kinds (bytes 6-7): `01 06` = Tortimer's holiday (all region events and April Fool's),
`00 06` = notice-only days, `02 06` monthly. The hook: an enabled event of another region runs on
its own date; the town's own events unchanged. Priority: if an own event of the same kind runs
that day, or an earlier enabled foreign one, the foreign event is skipped that year (no two
Tortimer days at once). Autumn Moon is a selector (own / USA / Europe / Japan Tsukimi): a variant
other than the town's own replaces it. All the dialogue (Tortimer `NPC_soncho_1`, the bulletin
board `BBS_event`, villagers' `*_FreeA_Moon*`) and the gift items exist in every language on every
disc (sections 1, 8.5), so no event is left off for missing text.

### 10.2 Mother's and Father's Day

Rules (fn_800895E0 / fn_800897B0, by the town's language index): 1-3/0/7 (Mother's: 2nd Sunday
of May), 4 English EU (Mothering Sunday table 0x8046EB90; a Dutch console: 2nd Sunday of May), 5
Spanish EU (1st Sunday of May), 6 French EU (table 0x8046ECB0), 9 Korea (May 8 = Parent's Day);
Father's: 5/7 March 19, 8 German Ascension Day (table 0x8046EDD0), 9 none, else 3rd Sunday of June.
The option runs both with the chosen language in the nibble for the call only, and the Mother's
Day letter templates (fn_800D2084) with that language's region.

### 10.3 Nook's seasonal items

Per call site of the disc region getter in fn_801456B4 (9.1): "Japan's New Year decorations" (Dec
26-31 kadomatsu / kagamimochi), "Hinaningyo" (Mar 1-3), "Festive flag and candle" (North America's
December stock). With both Dec 26-31 options on, Japan's item wins (the game asks for Japan
first).

### 10.4 Villager house styles

Answer for the user's question "how is the town's set chosen and stored": **not stored**. The five
house models of a town are `TN_obj_house0-4.brres` + `obj_house_tex.brres` inside the town's field
archive, which is picked by the town byte (region + language); a villager's house only stores its
style index 0-4. Every region's archive has its own five (all five differ in data between USA,
Europe and Japan; Korea's are Japan's). Mixing sets of several regions in one town is not possible
with the game's data (one texture file per set, same member names); the option picks the set of
one region for the whole town: the port serves the town's own archive with those six members from
`field{UsaEng|EurEng|JpnJpn}StrBrres<S/W>.arc` (a disc overlay built in memory, ~3 MB, shop signs
stay in the town's language) and answers the house-type data lookup of d_a_str_fieldNP
(`obj_house_usa/eur/jpn`) with that region. Works for new and existing towns from the next load,
switching back restores the town's own houses.

The player's own house: its exterior models `obj_myhome0-4.brres` are byte-identical in every
region's archive (arcdiff of the field archives: only obj_house_tex, TN_obj_house0-4, the
shops `obj_nc/ng/nk/nw`, `obj_cdbbs`, `obj_tailor` differ), so Nook's remodels look the same in
every region; roof colours etc. come from the same data.

### 10.5 Title screen logo

d_title_demoNP opens its disc's `/Layout/title_<us|eu|jp|kr>.arc` (the only difference of that
REL between versions). "Title screen logo" serves the chosen region's file under that name (disc
overlay); every disc has the US/EU/JP files, `title_kr.arc` only the Korean disc (with another
disc that choice keeps the disc's own). Verified: "Let's Go to the City" logo on the USA disc.

### 10.6 Verification (USA disc, 2026-10-28)

Runs from the user's town save (`CF_FIXTURE=user-town`, `tools/routes/continue.txt`, then the
holiday Tortimer found by his actor and the player moved in front of him, `CF_TELEPORT=<f>:tortimer`,
A presses + D-pad for the choices; scratch route files). On the move-in day (a new game) no
holiday Tortimer appears, also not for the town's own holidays: check events from a save.

| Date | Setting | Result |
|---|---|---|
| 2027-02-03 | Bean Day on | Tortimer at the town hall: "It's Bean Day!", the red/blue question, gives the **blue ogre mask**, Bean Day lines (English) |
| 2027-02-03 | all off | no holiday Tortimer, today's event list empty (retail) |
| 2026-12-06 | Naughty-or-Nice Day on (Europe) | "Today is Naughty-or-Nice Day", nice/naughty question, **stuffed stocking** |
| 2026-07-07 | Tanabata on | "Today is Starcrossed Day", **bamboo grass**, the wish-paper lines |
| 2026-09-25 20:00 | Autumn Moon = Japan | "Today is the Tsukimi moon-viewing", **dango** (the town's own USA moon is 2026-09-26 in the game's table) |
| 2026-10-12 | all off | the town's own Explorer's Day Tortimer as before |
| 2027-02-07 | Lunar New Year on (Korea) | "Today is Lunar New Year", **yut** |
| 2026-06-21 | Midsummer's Day on (Europe) | in today's event list (with the town's own Father's Day, #46: June 21 2026 is the 3rd Sunday), but no Tortimer that day; not checked on another year (open) |
| 2026-12-27 | Japan's New Year decorations | the shop's "region == Japan" question (0x80145AD0) answered yes (log); Nook's interior not looked at |
| 2026-10-29 | Villager houses own / Europe / Japan | the same villager's house (same style index) drawn as the USA Pink house, the European Brick house and the Japanese one; signs unchanged |
| title | Title logo = Europe | "Let's Go to the City" logo |

Lunar dates come from the game's tables (Lunar New Year 2027 = Feb 7 there, not Feb 6). Rover's
greeting differs after a long absence (the continue route only fits dates within about four
months of the save; earlier dates work too).

### 10.7 Not done yet / later

- The EU TV test card: cosmetic, not started.
- Korean Parent's Day letter: the template switch follows the Mother's Day rule; not checked in
  play.
- Distributed items (section 3): no disc has them (WiiLink / user files, translation of JP/KR-only
  names and letters). Korean-language text: only with the Korean disc as the base.
- Town sign textures per language (`TownTex`) and shop signs: per language, left as they are.
