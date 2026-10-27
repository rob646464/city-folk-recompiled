// Hemisphere toggle (F1 > Enhancements > Gameplay "Hemisphere"; docs/CALENDAR.md).
//
// City Folk keeps the game clock as an OSCalendarTime at 0x80600B18 (USA; getter fn_8016D40C;
// sec, min, hour, mday, mon 0-11, year, wday, yday, msec, usec), refreshed from OSGetTime plus
// the save's clock offset (fn_8016D490). Seasonal systems do not read a "season" variable of
// their own: each derives its season from a date through one of a few small lookup functions,
// and those are hooked here. Southern feeds them the date six months on (same day, clamped to the
// month's length), so everything they decide moves by half a year, while the clock itself,
// events, birthdays, holidays, letters, shops' opening days and saves keep the real date.
//
//   fn_8016DD58(time*)      term index: the (mon<<8|day) table at 0x8047B3E8 (23 ranges) ->
//                           0x80479E44 -> one of the 22 "terms" (fgObjSeason00-21: grass, trees,
//                           flowers, fruit trees, cherry blossoms, fall colours, snow cover).
//                           Called by fn_801623F8 (date -> term; 6 AM day start) for the stored
//                           term (fn_80162A1C at area load, byte +0x6D5B8 of the common data,
//                           read by fn_80162A80), and by the term-of-a-date checks (fn_80162AA8:
//                           aurora on winter nights, rainbow, rain -> snow), and by fn_800959E8 /
//                           fn_80095BE4 (day-by-day term walk). Term -> season 0-9 (0x8047A1D4),
//                           season 9 = snow on the ground ('W' models, snowmen), term >= 17
//                           (Nov 26 - Feb 24) turns rain into snow (fn_80162BA4 / fn_801C9FC4).
//   fn_8016DDA8(time*)      spring / summer / fall / winter (Feb 25 - May 31 / Jun - Aug / Sep 1
//                           - Nov 25 / Nov 26 - Feb 24): villager talk and behaviour (fn_8012xxxx),
//                           fn_80032988 / fn_80137730 / fn_8019EA44 (via fn_8016DE1C).
//   fn_800BD450(mon, kind)  insect spawn table of a month (0x804E8650[mon], 6 kinds).
//   fn_80090E2C(time*)      fish period 0-13 (months, July / August split at the 15th): the
//                           fish spawn tables (fn_80090F2C, period x time of day).
//   fn_800C6024(mon, day)   item season for shop stock / gifts / prizes (fn_800C60B4).
//   fn_801C973C(mon, day)   weather period 0-17 (0x80500620): the day-weather pattern weights
//                           (fn_801C9AA0); rain/snow probabilities, Dec 10 snow storm, dry
//                           mid-summer, ... (weather itself: cf_weather.cpp).
//
// What keeps its date (not hooked): every dated event and holiday (the event calendar), the
// fixed-weather days of events (fn_801C99F8: clear skies for festivals), birthdays, the clock,
// the BBS / TV texts that name months, meteor showers (by date), tanning and anything else that
// compares the raw date. See docs/CALENDAR.md for the full list and what is not covered yet.
//
// Test aids: CF_HEMISPHERE=north|south (overrides F1 for one run), CF_CAL_LOG=1 (every hooked
// lookup whose result changed: input date, date used, result), CF_CAL_PROBE=1 (each distinct
// caller of the clock getter once, with the guest frame: how the lookups above were found).
#include "game/cf_calendar.h"

#include "port/dol.h"
#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/menu.h"

#include <imgui.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <ostream>
#include <set>
#include <string>

namespace recomp::calendar {
namespace {

// ---- settings ([calendar] hemisphere=north|south) -------------------------------------------
std::atomic<bool> sSouthern{false};

void resetConfig() { sSouthern = false; }
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section == "calendar" && key == "hemisphere") sSouthern = value == "south";
}
void saveConfig(std::ostream& out) { out << "\n[calendar]\nhemisphere=" << (sSouthern ? "south" : "north") << "\n"; }
[[maybe_unused]] const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

int envHemisphere() {  // 0 none, 1 north, 2 south
    static const int v = [] {
        const char* e = std::getenv("CF_HEMISPHERE");
        if (!e) return 0;
        if (!std::strcmp(e, "south") || !std::strcmp(e, "1")) return 2;
        if (!std::strcmp(e, "north") || !std::strcmp(e, "0")) return 1;
        return 0;
    }();
    return v;
}

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
const bool sLog = envFlag("CF_CAL_LOG");
const bool sProbe = envFlag("CF_CAL_PROBE");

constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

const char* kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Logs a lookup when its result differs from the last one of the same hook.
void logLookup(const char* what, int mon, int day, int mon2, int day2, u32 result) {
    if (!sLog) return;
    static std::map<std::string, u32> sLast;
    auto [it, fresh] = sLast.emplace(what, result);
    if (!fresh && it->second == result) return;
    it->second = result;
    log("[cal] %s: %s %d -> %s %d (%s) = %u at frame %u", what, kMon[mon % 12], day, kMon[mon2 % 12], day2,
        southern() ? "south" : "north", result, game::guestFrames());
}

// Scratch copy of an OSCalendarTime (0x28 bytes) per hook.
u32 scratch(int slot) {
    static u32 sBase = hleAlloc(0x40 * 8, 32);
    return sBase + 0x40 * u32(slot);
}

// time* in r3 -> copy with the season date in r3, then the original. Returns the result.
void timeHook(Ctx& c, const char* name, const char* what, int slot) {
    const u32 t = c.r[3];
    const int mon = int(rd32(t + 0x10)), day = int(rd32(t + 0xC)), year = int(rd32(t + 0x14));
    if (!southern() || mon < 0 || mon > 11) {
        callOriginal(c, name);
        logLookup(what, mon, day, mon, day, c.r[3]);
        return;
    }
    int m2 = mon, d2 = day;
    seasonDate(m2, d2, isLeap(mon >= 6 ? year + 1 : year));
    const u32 s = scratch(slot);
    for (u32 i = 0; i < 0x28; i += 4) wr32(s + i, rd32(t + i));
    wr32(s + 0x10, u32(m2));
    wr32(s + 0xC, u32(d2));
    wr32(s + 0x14, u32(mon >= 6 ? year + 1 : year));
    c.r[3] = s;
    callOriginal(c, name);
    logLookup(what, mon, day, m2, d2, c.r[3]);
}

// (month, day) in r3/r4 (or month only).
void monthDayHook(Ctx& c, const char* name, const char* what, bool hasDay) {
    const int mon = int(c.r[3] & 0xFF), day = hasDay ? int(c.r[4] & 0xFF) : 1;
    int m2 = mon, d2 = day;
    if (southern() && mon <= 11) {
        seasonDate(m2, d2);
        c.r[3] = u32(m2);
        if (hasDay) c.r[4] = u32(d2);
    }
    callOriginal(c, name);
    logLookup(what, mon, hasDay ? day : 1, m2, hasDay ? d2 : 1, c.r[3]);
}

// The game clock (fn_8016D40C: lis/addi of the OSCalendarTime), decoded from the getter itself
// so it is right in every version.
u32 sClock = 0;
u32 clockAddr() {
    if (!sClock) {
        const u32 f = versionAddr(0x8016D40C);
        const u32 lis = rd32(f), addi = rd32(f + 4);
        sClock = ((lis & 0xFFFF) << 16) + u32(int32_t(int16_t(addi & 0xFFFF)));
    }
    return sClock;
}
void clockGetter(Ctx& c) {
    if (sProbe) {
        static std::set<u32> sSeen;
        if (sSeen.insert(c.lr).second) log("[cal-probe] clock read from %08X at frame %u", c.lr, game::guestFrames());
    }
    c.r[3] = clockAddr();
}

}  // namespace

bool southern() {
    const int e = envHemisphere();
    return e ? e == 2 : sSouthern.load();
}

void seasonDate(int& month, int& day, bool leapYear) {
    if (!southern()) return;
    month = (month + 6) % 12;
    const int dim = month == 1 && leapYear ? 29 : kDays[month];
    if (day > dim) day = dim;
}

u32 versionAddr(u32 usa) {
    struct Row {
        u32 us, pal, jp;
    };
    static constexpr Row kRows[] = {
        {0x8016D40C, 0x8016D0E0, 0x8016CFFC},  // clock getter
        {0x8016DD58, 0x8016DA2C, 0x8016D948},  // term index
        {0x8016DDA8, 0x8016DA7C, 0x8016D998},  // 4 seasons
        {0x80090E2C, 0x80090E4C, 0x80090DB4},  // fish period
        {0x800BD450, 0x800BD470, 0x800BD3D8},  // insect table
        {0x800C6024, 0x800C6044, 0x800C5FAC},  // item season
        {0x801C973C, 0x801C9430, 0x801C94DC},  // weather period
        {0x801C9E48, 0x801C9B3C, 0x801C9BE8},  // town hour weather
        {0x801C9EC0, 0x801C9BB4, 0x801C9C60},  // city hour weather
        {0x801C9FC4, 0x801C9CB8, 0x801C9D64},  // rain -> snow check
    };
    const auto v = port::dol::version();
    for (const Row& r : kRows)
        if (r.us == usa) return v == port::dol::Version::RUUP01 ? r.pal : v == port::dol::Version::RUUJ01 ? r.jp : r.us;
    return usa;
}

bool drawHemisphereMenu() {
    bool dirty = false;
    int h = sSouthern ? 1 : 0;
    ImGui::TextUnformatted("Hemisphere:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Northern (original)", &h, 0)) dirty = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Southern", &h, 1)) dirty = true;
    if (dirty) sSouthern = h == 1;
    ImGui::SetItemTooltip(
        "Southern moves the seasons by six months, as in New Horizons' southern hemisphere: grass, trees,\n"
        "flowers, cherry blossoms, fall colours, snow on the ground and snowfall, insects and fish, the\n"
        "weather's seasonal odds, seasonal shop stock and villager talk about the season.\n"
        "Dated events and holidays keep their date (New Year, Toy Day, Halloween, birthdays, ...),\n"
        "so Toy Day is a green summer Christmas. The clock and saves are not touched.\n"
        "Applies from the next area you enter (the weather from the next day).");
    if (envHemisphere()) ImGui::TextDisabled("(CF_HEMISPHERE overrides this for the current run)");
    return dirty;
}

}  // namespace recomp::calendar

// ---- hooks (USA / PAL / Japan names: tools/recomp/hooks.txt) ---------------------------------
namespace recomp {
using namespace recomp::calendar;

#define CAL_HOOK3(us, pal, jp, body)   \
    HLE(us) { body(c, #us); }          \
    HLE(pal) { body(c, #pal); }        \
    HLE(jp) { body(c, #jp); }

namespace {
void hTerm(Ctx& c, const char* n) { timeHook(c, n, "term index", 0); }
void hSeason4(Ctx& c, const char* n) { timeHook(c, n, "season (4)", 1); }
void hFish(Ctx& c, const char* n) { timeHook(c, n, "fish period", 2); }
void hInsect(Ctx& c, const char* n) {
    // Returns a pointer into the month table: log the month instead of the pointer.
    const int mon = int(c.r[3]);
    int m2 = mon, d2 = 1;
    if (southern() && mon >= 0 && mon <= 11) {
        seasonDate(m2, d2);
        c.r[3] = u32(m2);
    }
    callOriginal(c, n);
    logLookup("insect month", mon, 1, m2, 1, u32(m2));
}
void hItemSeason(Ctx& c, const char* n) { monthDayHook(c, n, "item season", true); }
void hWeatherPeriod(Ctx& c, const char* n) { monthDayHook(c, n, "weather period", true); }
void hClock(Ctx& c, const char*) { clockGetter(c); }
}  // namespace

CAL_HOOK3(fn_8016D40C, fn_8016D0E0, fn_8016CFFC, hClock)
CAL_HOOK3(fn_8016DD58, fn_8016DA2C, fn_8016D948, hTerm)
CAL_HOOK3(fn_8016DDA8, fn_8016DA7C, fn_8016D998, hSeason4)
CAL_HOOK3(fn_80090E2C, fn_80090E4C, fn_80090DB4, hFish)
CAL_HOOK3(fn_800BD450, fn_800BD470, fn_800BD3D8, hInsect)
CAL_HOOK3(fn_800C6024, fn_800C6044, fn_800C5FAC, hItemSeason)
CAL_HOOK3(fn_801C973C, fn_801C9430, fn_801C94DC, hWeatherPeriod)

}  // namespace recomp
