# Calendar features: hemisphere and real-time weather

Two options in F1 > Enhancements > Gameplay ("Seasons and weather"), both original / off by
default (branch calendar-1):

- **Hemisphere: Northern (original) / Southern**: the seasons move by six months, dated events
  keep their date. Code: `src/game/cf_calendar.cpp`. Setting `[calendar] hemisphere=north|south`.
- **Real-time weather** (off): the town's (and the city's) weather follows the current weather
  of a real place, from Open-Meteo. Code: `src/game/cf_weather.cpp`, host HTTPS in
  `src/port/http.cpp`. Settings `[weather] real_time, place, latitude, longitude, country`, last
  answer in `weather_cache.txt` next to settings.ini.

Hooks: `tools/recomp/hooks.txt` ("Calendar" block), USA / PAL / Japan rev 1 (the PAL and Japan
functions are the USA ones matched by hash, `config/<id>/us_map.txt`; addresses in
`cf_calendar.cpp versionAddr`). Only the USA build was run.

## How City Folk keeps the date

The game clock is an `OSCalendarTime` at **0x80600B18** (USA; getter `fn_8016D40C`, decoded from
the getter's `lis/addi` in every version): sec, min, hour, mday, **mon 0-11**, year, wday, yday,
msec, usec. `fn_8016D490` refreshes it from `OSGetTime` plus the save's own clock offset
(r13-0x2E10), years clamped to 2000-2035; `fn_8016D8DC` adds days/hours/minutes, `fn_8016DA64` /
`fn_8016DC08` compare dates. About 370 call sites read the clock (`CF_CAL_PROBE=1` lists the ones
that run). Seasonal systems never read a "season" variable of their own: each derives its season
from a date through one of a few small lookups, and those are what the hemisphere option hooks.

### Terms (the 22 seasonal looks)

`fn_8016DD58(time*)` looks the date up in a table of (mon<<8 | day) upper bounds at 0x8047B3E8
and maps the range through 0x80479E44 to a **term** 0-21 (`FgObj/fgObjSeason00-21.brres`, grass
packs `BgData/Pack/pat%dseason%02d.brres`). `fn_801623F8` (date -> term; before 6 AM the day
before counts) computes the stored term at each area load (`fn_80162A1C`, called from the scene
init `fn_801624C8`; stored at common data +0x6D5B8, read by `fn_80162A80`; r13-0x6388 is a debug
override, -1 = from the date). `fn_80162AA8` = term of any date.

| Term | Dates (northern) | Season (0x8047A1D4) |
|---|---|---|
| 0 | Feb 25 - Mar 31 | 0 |
| 1, 2 | Apr 1-3, Apr 4-8 (cherry blossoms) | 1 |
| 3 | Apr 9 - Jul 22 | 1 |
| 4 | Jul 23 - Sep 15 | 2 |
| 5 | Sep 16-30 | 3 |
| 6-8 | Oct 1-4, 5-10, 11-16 | 4 |
| 9-11 | Oct 17-20, 21-25, 26-30 | 5 |
| 12-14 | Oct 31 - Nov 2, Nov 3-9, 10-13 | 6 |
| 15, 16 | Nov 14-19, 20-25 | 7 |
| 17, 18 | Nov 26 - Dec 1, Dec 2-10 (snow falls, no cover) | 8 |
| 19-21 | Dec 11 - Feb 3, Feb 4-17, Feb 18-24 (snow cover) | 9 |

Season 9 = snow on the ground: `fn_80162AF0` returns 'W' (winter models: the town gate, bus
stop and bushes with snow) instead of 'S', `fn_80162B44` (snowmen `fn_80186384`, the player
`d_a_playerNP`, the sky). Term >= 17 (`fn_80162BA4` / `fn_80162BDC` of a date) turns rain into
snow (`fn_801C9FC4`) and gates the winter aurora (`fn_801613FC`) and the rainbow (`fn_80161508`).
`fn_800959E8` / `fn_80095BE4` walk the term index day by day.

### Other seasonal lookups

| Function | Input | Result / users |
|---|---|---|
| `fn_8016DDA8` | time* | 0 spring (Feb 25 - May 31), 1 summer (Jun - Aug), 2 fall (Sep 1 - Nov 25), 3 winter. Villager behaviour and talk (`fn_80122B84`, `fn_80126478`, ... `fn_80135CAC`), `fn_8016DE1C` (now) for `fn_80032988`, `fn_80137730`, `fn_8019EA44` |
| `fn_800BD450` | month, kind | insect spawn table: 0x804E8650[month], 6 kinds of 8 bytes; `fn_800BD46C` (now), `fn_800BD938` (6 AM day), used by `d_insect_fieldNP` |
| `fn_80090E2C` | time* | fish period 0-13 (a jump table on the month; two months split at the 15th); `fn_80090F2C` -> fish tables 0x804E1378[period][time of day] (`fn_80090ED8`: 4 times of day), used by `d_fish_fieldNP` through `fn_80090F88` / `fn_80091054` |
| `fn_800C6024` | month, day | item season 0-3 for shop stock, gifts and prizes (`fn_800C60B4`, about 70 callers: shops, NPCs, the sewing machine, ...) |
| `fn_801C973C` | month, day | weather period 0-17: the weights of the 32 day patterns (below) |

## Hemisphere: what moves and what stays

Southern replaces the date given to the six lookups above by the date six months on (same day,
clamped to the month's length: Aug 31 -> Feb 28, Feb 29 in leap years); everything else sees the
real date. With Northern the hooks call the original with the original arguments (byte-identical
behaviour).

**Moves (+6 months)**: grass and ground, trees, flowers and bushes, cherry blossoms (Oct 1-8), fall
colours (Mar 16 - May 25), snowfall (May 26 - Jun 10) and snow cover (Jun 11 - Aug 24), snowmen,
winter models, the winter aurora (clear nights while the term is winter), rain -> snow, the
weather's seasonal odds (dry summer, rainy season, the Dec 10 snow storm -> Jun 10), insects,
fish, seasonal shop stock and prizes, villagers' season-dependent behaviour and talk. Mushrooms
and other term-based fruit follow the term.

**Keeps its date**: every dated event and holiday (New Year, Festivale, Bunny Day, Halloween,
Harvest Festival, Toy Day, Countdown, birthdays, region holidays, lunar dates), the fixed weather
of event days (`fn_801C99F8`, table 0x8047EC00: clear skies for festivals), meteor showers, the
clock, saves, letters and the game's own clock offset. Toy Day in the south is a green summer
Christmas (no snow cover). New Horizons does the same for calendar holidays.

**Not covered / open**:
- Bug-Off and Fishing Tourney dates: they are event-calendar entries (month-fixed); New Horizons
  moves them in the south, this port does not yet (the event calendar is shared with the region
  events work, docs/REGION_CONTENT.md; coordinate there before hooking it).
- Tanning (Jul 16 - Sep 15) and any other code that compares the raw month directly was not
  found / not hooked.
- Texts that name seasons or months (BBS forecasts "cherry blossoms by April 1st", the
  encyclopedia's "Winter" / "Spring-Summer") are unchanged.
- The stored term changes at the next area load; today's weather pattern is cached per day, so
  switching the option shows its full effect from the next area / the next day.

## Real-time weather

### What the game does

Every day gets one of 32 **day patterns** (`fn_801C9AA0(time, kind)`; kind 0 = town, 1 = the
city): a fixed pattern for event days (`fn_801C99F8`), otherwise a seeded random pick weighted by
the date's weather period (`fn_801C973C` -> 0x805005D8[period], 32 byte weights). Today's and
tomorrow's patterns for town and city are cached in r13-0x2AC4 / -0x2AC0 / -0x2ABC / -0x2AB8
(`fn_801C978C`, date at r13-0x2AD0), so the TV forecast is always right. A pattern is 24 hourly
**states** from 6 AM (0x80500558[pattern][hour - 6]):

| State | Meaning |
|---|---|
| 0 | clear |
| 1 | some clouds |
| 2 | cloudy |
| 3 | rain (5 = snow while the term is >= 17) |
| 4 | heavy rain / storm (6 = heavy snow) |

`fn_801C9E48(time*)` (town) and `fn_801C9EC0(time*)` (city) return the state of an hour, after the
rain -> snow conversion. `dWeather_c` (profile entry at 0x8074C858; create `fn_801C9010`, every
frame `fn_801C92F0`) asks for the current hour (+0x5884) and the next one (+0x5888) and fades
between them at the end of the hour.

### What the port does

The two hourly functions are hooked. While a result younger than 3 hours exists (or the fake
source is set), the answer is the mapped current condition:

| Open-Meteo `weather_code` | State |
|---|---|
| 0, 1 | 0, or 1 when cloud cover >= 50 % |
| 2 | 1 |
| 3, 45, 48 (fog) | 2 |
| 51-57 drizzle, 61, 63, 66 rain, 80, 81 showers | 3 |
| 65, 67, 82 heavy rain, 95, 96, 99 thunderstorm | 4 |
| 71, 73, 77, 85 snow | 5 |
| 75, 86 heavy snow | 6 |

Snow only in the game's snow season (`fn_801C9FC4`, term >= 17, which follows the hemisphere
option), otherwise the same amount of rain; rain in the snow season falls as snow (as the game
does with its own rain). An hour's state is fixed the first time the game asks for it as the
current hour, or as the next hour within its last 10 minutes (when the fade starts): a fetch never
changes the sky mid-hour. The hour in which data first arrives keeps the game's own weather, so
turning the option on never pops. Day patterns, rainbows, the TV forecast and rain watering the
flowers stay the game's own.

Network (background thread, `port::http::get`, Windows WinHTTP, Linux/macOS libcurl loaded at run
time): place search `geocoding-api.open-meteo.com/v1/search?name=...&count=10&language=en` (only
when Search is pressed, optional `countryCode`), current conditions
`api.open-meteo.com/v1/forecast?latitude=<0.1>&longitude=<0.1>&current=weather_code,cloud_cover,...`
every 30 minutes while the option is on (5 minutes after a failure), immediately after the place
changes. Nothing is sent while the option is off; the menu says so and shows
"Weather data by Open-Meteo.com" (CC BY 4.0; non-commercial free tier, docs/FEATURE_RESEARCH.md
section 5.3).

### Open

- Rain falls without visible streaks in our renderer (darker sky, splash rings on the ground).
  Not compared with Dolphin yet, so unverified; the rain is `ef` particles
  (`afm_fld_rain_*`), which may be a rendering gap. The rain scenes also ran slower than real time
  on a busy PC (presents fell behind retraces).
- The thunderstorm rule of FEATURE_RESEARCH (no thunder after September) is not applied: state 4
  is what the game uses for its own heavy-rain hours in any month.
- The city's weather follows the same place (the game gives it its own patterns).
- PAL / Japan builds not run.

## Verification (2026-10-27, USA, `CF_CLOCK_OFFSET=6912000` = Jan 15, 2027)

- Northern, January: snow on the gate, bus stop and bushes (term index 0 = term 19).
- Southern, January: `[cal] term index: Jan 15 -> Jul 15 (south) = 6` (term 3, summer), item
  season 1, weather period 4, insect month Jul, fish period 6, season (4) 1 (summer); the town
  has green summer grass and bushes and no snow; the gate is the summer model.
- Walking route (`town_walk.txt`, frame 19000): Northern January = snow-covered ground and
  snow-capped trees; Southern January = green summer grass, no snow, summer flowers.
- `CF_WEATHER_FAKE=heavyrain`, Southern January: state 4 (heavy rain), same look as rain here.
- `CF_WEATHER_FAKE=snow`, Northern January: `[weather] town hour 0 (now): game 0 (clear) -> 5
  (snow), code 73, snow season 1`: snow falls in town.
- `CF_WEATHER_FAKE=rain`, Southern January: `game 0 (clear) -> 3 (rain), snow season 0`: rain
  (darker, wet ground; see Open).
- Real lookup `CF_WEATHER_TEST=Berlin`: `test search "Berlin": 10 matches, first Berlin, State of
  Berlin, Germany (52.5, 13.4)`, `code 3 (overcast), cloud 95%, 10.6 C`; current hour kept the
  game's own (arrival rule), next hour `game 0 (clear) -> 2 (cloudy)`.
- F1 > Enhancements > Gameplay shows both options, the place and the current conditions.

## Test aids

| Variable | Effect |
|---|---|
| `CF_HEMISPHERE=north\|south` | overrides the F1 setting for one run |
| `CF_CAL_LOG=1` | every hooked lookup whose result changed: input date, date used, result |
| `CF_CAL_PROBE=1` | each distinct caller of the clock getter, once, with the guest frame |
| `CF_WEATHER_FAKE=clear\|cloudy\|overcast\|rain\|heavyrain\|storm\|snow\|heavysnow` | fake current conditions, no network, applied at once (no hour latching) |
| `CF_WEATHER_TEST=<place>` | searches the place, takes the first match, turns the option on and fetches (this run only) |
| `CF_WEATHER_SEARCH=<text>` | types the text into the F1 search box and presses Search once (pick list) |
| `CF_WEATHER_LOG=1` | fetches, searches, and every hourly answer that differs from the game's own |

Route: `--input @tools/routes/town.txt` with `CF_INPUT_CLOCK=frames` (town gate at about frame
14000; `town_walk.txt` walks from 16700), `CF_CLOCK_OFFSET=<s>` for the month,
`CF_SHOT=<dir>,<first>,<last>,<step>`.
