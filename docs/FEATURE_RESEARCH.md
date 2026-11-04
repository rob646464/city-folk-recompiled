# Feature research

Specs for the ROADMAP game features. Confidence tags: **[disc]** checked in our extracted discs;
**[manual]** Nintendo's NA instruction booklet; **[web-high/medium/low]** fan wikis and forums
(untrusted data, not verified in the game); **[spec]** our proposal. Nothing here was tested in
a running game.

## 1. Controllers

What the game supports:
- **Wii Remote alone, held vertically**, and **Wii Remote + Nunchuk ("Nunchuk Style")**
  [manual p. 5]. No horizontal mode. Remote alone: hold A and point to walk (pointer + A = move)
  [web-high].
- **Not supported: Classic Controller, GameCube controller** [web-medium: pre-release press,
  the manual lists only the two styles]. A fan mod (City Folk Deluxe) adds a Classic Controller
  layout through Riivolution, so the game has no layout of its own for it.
- **USB keyboard** for letters and chat [manual p. 9] (`/dev/usb/kbd`, RVL KBD library [disc]).
- **Wii Speak** (online only) [manual p. 22]. **Nintendo DS** for the Wild World move and DS
  Suitcase (section 6).
- SDK [disc]: KPAD + WPAD, EGG `ControllerRumbleMgr` (rumble), HBM (Home Button menu,
  `homebutton::RemoteSpk` = Home menu speaker sounds).

Nunchuk Style [manual pp. 5-6]:

| Input | Action |
|---|---|
| Control Stick | select, move |
| C | talk, use tools, read signs, open doors |
| Z (B on the remote) | cancel, pick up items, pick flowers; hold while moving = run |
| A | confirm, talk, use tools, read signs, open doors |
| Pointer (+ A) | select; point + A = walk to the point |
| D-pad outdoors | Up: change view (look at the sky); Left/Right: change tool; Down: put tool away |
| D-pad indoors | change view |
| 1 | take a photo |
| 2 | Photos menu |
| - and + | open menus, scroll through menus (StrategyWiki: - = pockets, + = map [web-medium]) |
| HOME | HOME Menu |

Furniture: hold A or C to grab, push/pull with movement, tilt the stick left/right to rotate
[manual p. 7].

Pointer-only parts [manual pp. 7-9; web-medium]:
- The save / emotion / menu icons on the field appear only while pointing at the screen.
- Pockets: point-and-hold drag to equip (drag onto the player), drop, attach to a letter;
  submenus by pointing at an item and pressing A.
- Text entry: on-screen keyboard (or "cell-phone style" layout) by pointing.
- Pattern editor: pointer.
- Shops, catalog, museum menus: pointer (assumed: same UI framework `d_menu_*`).
So the retail game has **no stick/D-pad navigation in menus**; "normal controller navigation"
is new work, not a hidden mode. The Deluxe mod's Classic layout is a model for the mapping (left
stick walks, right stick moves the cursor, a shoulder button toggles the cursor, A confirm,
B cancel/run/pick up, X/Y map/pockets and next/previous tab, -/+ photo/photo menu) [web-medium].

Motion: StrategyWiki lists "shake remote" as a way to use a tool [web-medium]; fishing/net
swings by motion are reported by a review [web-low]. Unverified: check the KPAD acceleration
reads in the player REL before relying on it. Rumble: fishing bite (players turn rumble off and
listen instead) [web-medium]; other rumble uses unknown.

Remote speaker: no source names game sounds on the Wii Remote speaker; the only speaker code
seen is the HOME menu's (`HomeButton2/SpeakerSe.arc`, `homebutton::RemoteSpk`) [disc]. Whether
`rv_forest.brsar` routes any sound to the remote (sound info output-line flags) is unverified.

Port design consequences [spec]:
1. Pointer source per port (mouse / right stick / gyro) feeds the WPAD DPD (IR) data; the game
   does the rest.
2. Menu navigation without the pointer must be built on top: a virtual cursor that snaps to
   menu items (the layouts' pane rectangles from the `d_menu_*` brlyt files), moved by D-pad /
   stick, A = point + A. This is the least invasive: no menu code changes, the game still sees
   a pointer.
3. Remote-alone style (A + point to walk) and Nunchuk style both have to keep working; a
   gamepad maps to Nunchuk style (left stick = Nunchuk stick, triggers = Z/C).

## 2. Grass wear

Implemented and traced in the game: docs/FEATURES.md "Grass wear" (the game's real rules differ
from the web sources below: wear 4-7, or 8-11 under a flag, once per tile per load; regrowth
1 + neighbour mean / 85 per day).

City Folk [web-medium-high: Nookipedia "Animal tracks", from fan research; storage from ACSE
(MIT, Cuyler36) `SaveDataManager.cs`]:
- **Per tile**, one byte, 0 = bare dirt ... 255 = healthy grass. Town = 5 x 5 acres x 16 x 16
  tiles: **0x1900 bytes at save offset 0x6BCB6** (in ACSE's town-save layout; order inside an
  acre: 4 rows x 2 columns of 8 x 4-tile blocks, index `blockY*64 + blockX*32 + y*8 + x`).
  Grass shape type byte at 0x6D5B7 (0 triangle, 1 circle, 2 square).
- **Wear:** the first time a player steps on a tile in each "town load" it loses a random amount
  by season: spring 4-7, summer 4-8, fall 4-9, winter 8-11. A new round can happen at every
  load: game start, a WFC session, return from another town, the 6 AM new-day message. Running
  adds no extra wear; repeat steps in one session add none; patterns on the ground don't matter.
  Villagers: not reported (unverified).
- **Regrowth at 6 AM daily:** +4 base, +5 more if the tile has a flower, tree, stump or sapling,
  -1 next to unhealthy tiles. Weeds, items, buried items, rain/snow and watering have no effect.
  An isolated bare tile heals in about 29 days; a path walked daily never heals.
- Snow covers grass from early December (full Dec 11) to Feb 25; grass lightens in September
  and browns Oct-Nov [web-medium].
- Wild World has **no** wear (ACSE: no wear block; only a grass type) [web-high]; City Folk
  introduced it.
- Code [disc]: `/BgData/Grass/grass_first.brres`, `dBG::grassBank_c`, textures `m_grass`,
  `tex_grass`, `pl_grass` (main.dol, `d_bgo_drawNP.rel`, `d_fg_drawNP.rel`).

New Leaf [web-medium-low, Nookipedia marks it as needing research]: wear much subtler; no
per-load extra rounds; regrowth boosted 3 stages the day after snow and 1 stage the day after
rain; dirt also from public works sites, villager plots, riverbanks. New Horizons removed wear.

Spec for the F1 option (Enhancements > Gameplay "Grass wear") [spec]:
- **Original**: unchanged.
- **New Leaf style**: (a) wear only on the first step per tile per **day** (not per load);
  (b) wear per step halved (spring 2-3, summer 2-4, fall 2-4, winter 4-5, rounded); (c) daily
  regrowth as original plus +3 stages after a snow day and +1 after a rain day (a "stage" =
  the game's visual step; until the drawing thresholds are known, take one stage = 16 health).
  Values in (b) and (c) are our reading of "much subtler" and must be tuned by eye: no source gives New Leaf's
  numbers.
- **Off**: never apply wear (skip the decrement), regrowth as original so existing paths heal.
- Implementation: hook the wear decrement and the 6 AM regrowth routine (find them through
  writes to the town's wear block; LM-style `LM_WATCH` on the block).

## 3. Skin tone

Implemented and traced in the game: docs/FEATURES.md "Skin tone" (tan at player data + 0x83F8 =
save + 0x8418 for slot 0, 0..15, tone = tan >> 1: confirmed).

[disc] The game has **8 skin palettes**: `Plyr/Pltt/Palette.arc` 0.brplt-7.brplt, each named
`skin`, 16 RGB5A3 colours, darkening step by step (base tone 0 lightest, 7 darkest). Palettes
8-71 are 8 groups of 8: one colour family per group with the last colours darkening across its
8 entries, consistent with hair/face palettes having one variant per tan level (unverified).
Palettes 72-91 are other (eyes, accessories?).

[web-medium, ACSE `PlayerInfo.cs`/`Player.cs`] Per player: tan byte at player offset **+0x8418**
(players 0x86C0 apart; up to 4 residents); ACSE reads `value >> 1` as the tan level 0-7 ("Not
100% sure", copied from ACToolkit), which matches the 8 palettes. Nearby: face +0x840A, hair
+0x840B, hair colour +0x840C, shoe colour +0x8416.

Tanning [web-medium, Nookipedia "Skin tone"]: Jul 16 - Sep 15, the skin darkens by the next day
with time spent in the sun (City Folk: tones by hours outside: 0, 1+, 2+, 3+, 4+, 5+, i.e. 6
steps per Nookipedia vs 8 palettes on disc); an umbrella or certain hats prevent it; out of the
sun or after summer it fades one step per day. New Leaf: 8 tones, tan all year on the island.
Choosing a tone freely began with Happy Home Designer / New Horizons. The City Folk Deluxe mod
disables tanning and offers a tone at character creation.

Spec (F1 > Enhancements > Gameplay "Skin tone", applies to the player being played only)
[spec]: Original (game tans) / fixed tone 0-7 shown as 8 swatches from palettes 0-7. A fixed tone
writes the tan byte for the current player (`level << 1`) and blocks the game's daily tan update
for that player only; other residents keep the original behaviour. Store the choice per player
(by player name + town id) in settings.ini so it follows the player, not the save slot.
Verify in game: the byte's encoding (the low bit, the update routine), that palettes 0-7 are the
ones selected, and that hair/face palettes follow.

## 4. Hemisphere

City Folk has **no** hemisphere option, and neither has New Leaf: hemispheres exist only in
New Horizons [web-high: Nookipedia "Hemisphere"]. **The ROADMAP's "New Leaf is the reference"
should read New Horizons.**

What is seasonal in City Folk [web-medium unless noted]:

| Thing | Northern (original) | Southern [spec, New Horizons rule] |
|---|---|---|
| Snow on the ground | snow falls from late Nov, heavy Dec 10, full cover Dec 11, melts Feb 25; snow warning 2 days before | Jun 10 / Jun 11 - Aug 25 |
| Cherry blossoms | Apr 1-8 (no festival) | Oct 1-8 |
| Fall colours | Sep dull, Oct orange, Nov red-brown (Sep 1 - Nov 25) | Mar 1 - May 25 |
| Mushrooms | November | May |
| Grass colour | per season (above) | +6 months |
| Rain | rare late winter, moderate spring, heavy + thunder early summer, dry mid/late summer, low-moderate fall, no thunder after Sep | +6 months |
| Aurora | clear winter nights 8-10 PM | +6 months |
| Tanning | Jul 16 - Sep 15 | Jan 16 - Mar 15 |
| Fish / insects | monthly tables (64 fish, 64 bugs; Nookipedia "Fish/City Folk", "Bugs/City Folk") | month + 6 |
| Bug-Off | 3rd Saturday Jun-Sep | 3rd Saturday Dec-Mar |
| Fishing Tourney | Oct-May | Apr-Nov |
| Fireworks | Sundays in August | keep August (New Horizons keeps it) |
| Seasonal shop stock / clothing | by season (unverified detail) | +6 months |
| BBS weather forecasts (`BBS_event` #2-#7: snow, cherry blossoms "by April 1st", rainy season "end of June", flurries "in November") | by date | shift with the season; the texts name months, so either keep them unshifted or accept wrong month names |
| Encyclopedia season words (`SYS_2D_fish_time`/`bug_time`: "Winter", "Spring-Summer") | text | wrong in the south unless swapped (season names exist in the same file; a text swap table is enough) |

Keep their date (New Horizons keeps calendar holidays; it changes some themes): New Year,
Valentine's, Festivale, Bunny Day, April Fool's, Halloween, Harvest Festival, Toy Day, Countdown,
birthdays, Mother's/Father's Day, region holidays (REGION_CONTENT.md), lunar events (Autumn
Moon, Lunar New Year, Daeboreum), meteor showers, Flea Market. Toy Day in the south: New
Horizons makes it a summer day; City Folk would still show snow decorations only if snow is on
the ground, so with shifted snow it will be a green Christmas automatically.

Implementation [spec]: give seasonal subsystems a "season date" (real date + 6 months, same day
of month, clamped Feb 29 -> Feb 28) while events, birthdays, the clock display and save
timestamps use the real date. Needs the list of date readers per subsystem (the engine agent
finds the callers of the game's calendar functions); Southern off = byte-identical.

## 5. Real-time weather

### 5.1 What City Folk can show

[web-medium, Nookipedia "Weather"; classes from main.dol/RELs [disc]]
- Precipitation: none, **rain**, **thunderstorm** (rain + occasional lightning, cosmetic), **snow**.
- Cloud cover levels: clear, contrail, wisp, light, moderate, dense, complete (rain), complete
  (storm/snow). Clouds can change every hour and fade in over the last 10 minutes of the hour;
  rain/snow can change hourly but often last most of the day.
- Sky events: rainbow after heavy rain/snow late in the day; meteor showers on clear nights from
  7 PM (announced on the BBS); aurora on clear winter nights 8-10 PM; the sky clears for
  fireworks. No fog.
- The TV's next-day forecast is always right: weather is decided at least a day ahead.
- The city can have its own weather; rain there waters the town's flowers on return.
- Code [disc]: `dWeather_c`, `dWeatherSnowDraw_c`, `dWeatherSakuraDraw_c` (petals),
  `dWeatherConfettiDraw_c`, `Weather/weather_obj.brres` (snow, petals, confetti), `d_skyNP.rel`
  (`Sky/bg_cloud.brres`), `d_sky_rainbowNP.rel`, `d_sky_auroraNP.rel`, rain effects
  `afm_fld_rain_*`, TV sound `SoundTv_255_weather`.
- How the weather is chosen and where it is saved: unknown (ACSE has no City Folk weather
  offset). Find it from the TV forecast / `dWeather_c` state.

### 5.2 Picking the place (no ZIP-only design)

Postal codes are not a usable primary key worldwide. Tested live against Open-Meteo geocoding
(2026-10-27) [checked]:

| Query | Result |
|---|---|
| `Berlin` | Berlin DE first, then US Berlins (fuzzy: "Brunswick, Maryland" 4th) |
| `Tokyo`, `Seoul`, `Sao Paulo` (no accent), `Saint-Étienne` (language=fr), `Dublin` (countryCode=IE), `Nairobi`, `Hong Kong` | correct first hit |
| `東京`, `서울` (native script) | **no result** |
| US ZIP `90210`, FR `75001`, NL `1011` | correct (Beverly Hills, Paris, Amsterdam) |
| UK `SW1A` | "Westminster Abbey"; full `SW1A 1AA`: none |
| DE `10115`, AU `2000`/`3000`, CA `H2X` / `M5V 2T6`, JP `100-0001`/`1000001`, BR `01310-100`, IE `D02`, IT `00118` | **no result** |
| 2 characters (`Sp`) | no result (docs: 1 char none, 2 exact, 3+ fuzzy) |

So: **search by town/city name** (English or local Latin-script spelling), postal codes only as
a bonus where they work. Endpoint: `https://geocoding-api.open-meteo.com/v1/search?name=<text>&count=10&language=<ui language>&format=json`
(optional `countryCode=<ISO-3166 alpha-2>`). Result fields used: `name`, `admin1` (region),
`country`, `country_code`, `latitude`, `longitude`, `population`, `feature_code`.

Spec [spec]:
- F1 > Enhancements > General "Real-time weather" (off). When turned on: a search box, a
  **Search** button (no search-as-you-type: one request per button press), a pick list of up to
  10 matches shown as "name, region, country" (population order as returned), a country filter
  drop-down (optional, adds `countryCode`).
- Store only the chosen **display name** and **coordinates rounded to 0.1°** (about 10 km) in
  settings.ini. The search text is sent to the geocoder only when the user presses Search.
- No IP geolocation. At most an explicit button "Use my approximate location", whose tooltip
  says it sends the computer's IP address to a lookup service (not implemented by default;
  Open-Meteo has no IP lookup, it would be another service).
- If nothing is found: message "No match: try the nearest larger town, or the English name".

### 5.3 Current conditions

- Endpoint: `https://api.open-meteo.com/v1/forecast?latitude=<lat 0.1°>&longitude=<lon 0.1°>&current=weather_code,cloud_cover,precipitation,rain,showers,snowfall,is_day,temperature_2m&daily=weather_code&forecast_days=2&timezone=auto`
  (tested live: returns `current` with 15-minute `interval` 900, `weather_code`, `cloud_cover` in
  %). The `daily` part lets the TV's tomorrow forecast come true as well.
- **Refresh**: every 30 minutes while the game runs and the option is on (48 calls/day at most,
  far under the free limits), plus once at startup and after the place changes; cache the last
  result with its time in the settings folder; use a cached result for up to 3 hours; with no
  result younger than 3 hours (offline, error) the game's own weather runs unchanged.
- Apply changes at the game's own hourly change point (the cloud fade in the last 10 minutes of
  the hour), never mid-hour, so no weather pops.
- Terms [checked 2026-10-27, open-meteo.com/en/terms]: free API for **non-commercial** use
  only (no ads/subscriptions; a free, open port qualifies, a paid product would not); limits
  < 10,000 calls/day, 5,000/hour, 600/minute, 300,000/month, applied per IP address; no API key
  for the free tier; data **CC BY 4.0**: show "Weather data by Open-Meteo.com" with a link in
  the option's tooltip/about text. Geocoding data is from GeoNames (CC BY 4.0 too [web-medium]).
- Privacy: the request carries the rounded coordinates and the user's IP; nothing is sent while
  the option is off. Tooltip says so.
- Alternatives for postal codes (only if users ask): Zippopotam.us (`api.zippopotam.us/<cc>/<code>`,
  ~60 countries, terms not found) [web-medium]; Nominatim (OSM): max 1 request/s per
  application, identifying User-Agent, attribution, "moderate" user numbers only [web-high].

### 5.4 Mapping real conditions to the game

WMO codes (Open-Meteo docs) -> game state [spec]:

| WMO `weather_code` | Game |
|---|---|
| 0 | clear (cloud level from `cloud_cover`: 0-10 % clear, 10-25 % wisp/contrail) |
| 1, 2, 3 | cloudy, level by `cloud_cover`: 25-45 light, 45-70 moderate, 70-90 dense, 90+ complete |
| 45, 48 (fog) | dense clouds (no fog in the game) |
| 51-57 (drizzle), 61-67 (rain, freezing rain), 80-82 (showers) | rain |
| 95, 96, 99 (thunderstorm) | thunderstorm where the game allows thunder (Nookipedia: not after September); otherwise rain |
| 71-77 (snow), 85-86 (snow showers) | snow |

Season limits [spec; check what the weather code accepts]:
- Real snow outside the game's snow season: show snow only if the game can draw falling snow
  without ground cover (it does in late November [web-medium]); otherwise rain.
- Real rain while the ground is snow-covered (Dec 11 - Feb 24 northern): show snow (City Folk
  never rains on snow cover, as far as known: unverified).
- Leave rainbows, meteor showers, aurora and the fireworks override to the game; they follow
  from the weather it sees.
- Hemisphere toggle and real weather combine naturally: real weather already belongs to the
  user's hemisphere; snow cover dates come from the hemisphere setting.

## 6. Other features that touch the port

- **SD card** [manual p. 11; disc]: photos (button 1; only one unsaved photo is held, lost when
  the game ends) saved as JPEG through Photos > SD Card. Paths in main.dol: `\DCIM`,
  `%s\RUU_????.JPG`, `%s\%s%04d.JPG`, `%s\????????.JPG` (TMC JPEG library, `image/jpeg`); devices
  `/dev/sdio/slot0`, `slot1`, `slotw`. Photos can also go to the Wii Message Board (WC24). The
  revisions' fix: saving JPGs; no SDHC support (cards up to 2 GB) [web-medium]. Patterns and
  letters are not saved to SD [web-medium]. Port: SD HLE to a folder (`%APPDATA%\...\sd\`),
  F1 button "Open photo folder"; card always present, size reported as 2 GB.
- **Save** [manual p. 5; disc]: NAND `rvforest.dat`, `banner.bin`; one town per console;
  36 blocks; save in bed or from the save icon; autosave when going out/coming back. Miis:
  `/shared2/menu/FaceLib/RFL_DB.dat` (Mii masks at Shampoodle, `d_menu_mii`), so the NAND HLE
  needs a Mii database (empty or user-imported).
- **DS transfer** (done: docs/MOVE_IN.md; the code shows `Vaga_Sign.srl` = Wild World "Move from
  DS!" (the Moving Van, ggid 0x400806) and `Exch_Sign.srl` = DS Suitcase, the other way round from
  this first guess: `files/DSApp/Exch_Sign.srl` = Wild World "Move from DS!", `Vaga_Sign.srl` =
  DS Suitcase; DS Download Play binaries, header `NTRJ01`, signed) [disc; manual p. 4]: moves face,
  name and part of the catalog from Wild World (not pockets/Bells/items); the DS Suitcase carries
  a player to a friend's Wii via a DS. Uses Wii DS-wireless (`/dev/net/wd/command`) [disc]. Port:
  not emulatable without DS hardware; options: report "no DS found", or later an importer for
  Wild World save files (`.sav`) feeding the same transfer path. Low priority.
- **Wii Speak**: online only (docs/ONLINE.md).
- **Clock and time travel** [manual p. 5; web-medium]: the game uses the Wii clock by default
  and has its own "Set the clock" ("Other things"); dates run to Dec 31, 2035 then wrap to 2000.
  Changing the time can spoil turnips and lose mailed presents. Nookipedia claims changing the
  in-game clock affects fruit/weed growth, wilting and interest while the system clock does not
  (unverified). Consequences of skipping time: weeds, bed head, villagers moving out without
  notice, Resetti after resets. Port: RTC from the host clock (local time), plus the ROADMAP's
  optional F1 offset; the game's own clock setting keeps working as on the console.
- **HOME button menu** [disc]: full HBM in `HomeButton2/` (layouts per language incl. KOR, CHN,
  NED; sounds; speaker sounds); the HOME menu also holds Wii Remote volume/rumble settings. Port:
  HOME opens the game's HBM (Wii Menu / Reset buttons map to quit / reset), F1 stays separate.
- **16:9 and 480p** [disc: SYSCONF keys `IPL.AR`, `IPL.PGS`, `IPL.E60`, `IPL.LNG`, `IPL.SADR`,
  ...; web-medium]: native 16:9 and progressive scan from the Wii settings; the port reports
  16:9 from F1 (docs/WII.md).
- **Language** [disc]: US disc ENG/FRA/SPA, PAL ENG/FRA/GER/ITA/SPA, JP JPN, chosen from the Wii
  language setting (section 1.5 of REGION_CONTENT.md); the PAL HOME menu also has Dutch (game
  falls back to English).
