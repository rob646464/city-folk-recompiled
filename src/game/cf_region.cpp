// Content from other regions (F1 > Enhancements > Gameplay "Content from other regions";
// docs/REGION_CONTENT.md "Content from other regions in the port").
//
// Every disc carries every region's holidays, dialogue and items. What decides which run is the
// TOWN's language index, stored when the town is made: save byte +0x735C2 of the town data
// (fn_80154F80: high nibble = language index 0 JP, 1-3 USA English/Spanish/French, 4-8 Europe
// English/Spanish/French/Italian/German, 9 Korea; low nibble = its region group 0-3).
//
// Events. main.dol has one event table (USA/PAL 0x8046EEF0, JP 0x8046EE30; identical on all
// three discs): 47 records of 12 bytes {u32 date test function; u16 language mask (bit n = town
// language n); u8 kind[3]; u8 month, day (for the fixed-date test); u8 pad}. fn_80089890
// (event, date) = "the event runs on this date": fn_80089BD4 tests the mask against the town's
// language, then the record's date function runs. Every user of the calendar (today's event
// list fn_80089D3C, Tortimer, the bulletin board, the shops) goes through it. The hook below
// answers it per event: an enabled event of another region runs on its own date; the town's
// own events are unchanged; the town's region, the save and every online/region check are
// never touched.
//
// Priority rule (two events of one kind on one day, e.g. a lunar Korean holiday on Bean Day):
// the town's own event wins, then the earlier one in the table; the other is skipped that year.
//
// Autumn Moon: three variants of one holiday (USA #35 wheat bundle, Europe #39 veggie basket,
// Japan #29 Tsukimi dango; Korea has none). A variant other than the town's own replaces the
// own one (never two moon days).
//
// Mother's/Father's Day: one event each for every language (#45 fn_800895E0, #46 fn_800897B0);
// their date rule reads the town's language nibble. The hook runs them with the chosen
// language in that nibble for the duration of the call.
#include "game/cf_region.h"

#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"

#include "port/settings.h"

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <initializer_list>
#include <string>
#include <vector>

namespace recomp::region {
namespace {

// ---- Settings ([region] in settings.ini) ---------------------------------------------------

enum Region { kJP, kUS, kEU, kKR };
const char* const kRegionName[] = {"Japan", "North America", "Europe", "Korea"};

struct EventDef {
    int index;          // event table record
    Region region;
    const char* key;    // settings.ini key
    const char* name;   // menu label
    const char* when;   // tooltip: date and host
};
// Region-exclusive events (table records 26-43 without the Autumn Moon variants).
const EventDef kEvents[] = {
    {26, kJP, "bean_day", "Bean Day (Setsubun)", "February 3. Tortimer hands out a red or blue ogre mask."},
    {27, kJP, "girls_day", "Girl's Day (Hinamatsuri)", "March 3. Tortimer gives a rice cake."},
    {28, kJP, "boys_day", "Boy's Day (Kodomo no Hi)", "May 5. Tortimer gives a newsprint helmet."},
    {30, kJP, "tanabata", "Starcrossed Day (Tanabata)", "July 7. Tortimer gives bamboo grass."},
    {31, kUS, "groundhog_day", "Groundhog Day", "February 2. Tortimer gives a Resetti model."},
    {32, kUS, "nature_day", "Nature Day", "April 22. Tortimer gives a cool globe."},
    {33, kUS, "labor_day", "Labor Day", "First Monday of September. Tortimer gives a picnic basket."},
    {34, kUS, "explorers_day", "Explorer's Day", "Second Monday of October. Tortimer gives a sailboat model."},
    {36, kEU, "midsummers_day", "Midsummer's Day", "June 21. Tortimer gives an espresso maker."},
    {37, kEU, "naughty_or_nice_day", "Naughty-or-Nice Day", "December 6. Tortimer gives a stuffed stocking."},
    {38, kEU, "midwinters_day", "Midwinter's Day", "December 21. Tortimer gives a snow globe."},
    {40, kKR, "lunar_new_year", "Lunar New Year (Seollal)", "First day of the lunar year (late January or February). Tortimer gives a yut set."},
    {41, kKR, "tree_day", "Tree Day (Sikmogil)", "April 5. Tortimer gives a hibiscus."},
    {42, kKR, "teachers_day", "Teacher's Day", "May 15. Tortimer gives a red carnation."},
    {43, kKR, "daeboreum", "Daeboreum", "15th day of the first lunar month. Tortimer gives bureom nuts; villagers talk about the full moon."},
};
constexpr int kEventCount = int(sizeof kEvents / sizeof kEvents[0]);

// Autumn Moon variants: 0 = the town's own; else the record to use instead.
const int kMoonRecord[] = {-1, 35, 39, 29};
const char* const kMoonName[] = {"The town's own", "North America (wheat bundle)", "Europe (veggie basket)",
                                 "Japan: Tsukimi (dango)"};
// Mother's/Father's Day rules by language index (fn_800895E0 / fn_800897B0).
const int kParentLang[] = {-1, 1, 4, 5, 6, 7, 8, 9};
const char* const kParentName[] = {
    "The town's own",
    "North America / Japan (May, 2nd Sunday / June, 3rd Sunday)",
    "UK English (Mothering Sunday / June, 3rd Sunday)",
    "Spain (May, 1st Sunday / March 19)",
    "France (May, last Sunday / June, 3rd Sunday)",
    "Italy (May, 2nd Sunday / March 19)",
    "Germany (May, 2nd Sunday / Ascension Day)",
    "Korea (Parent's Day May 8 / none)",
};

struct Config {
    bool event[kEventCount] = {};
    int moon = 0;     // index into kMoonRecord
    int parents = 0;  // index into kParentLang
};
Config sCfg;

void resetConfig() { sCfg = Config{}; }
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "region") return;
    const bool on = value == "true" || value == "1";
    for (int i = 0; i < kEventCount; i++)
        if (key == kEvents[i].key) sCfg.event[i] = on;
    if (key == "autumn_moon") sCfg.moon = std::atoi(value.c_str());
    if (key == "parents_days") sCfg.parents = std::atoi(value.c_str());
    if (sCfg.moon < 0 || sCfg.moon > 3) sCfg.moon = 0;
    if (sCfg.parents < 0 || sCfg.parents > 7) sCfg.parents = 0;
}
void saveConfig(std::ostream& out) {
    out << "\n[region]\n";
    for (int i = 0; i < kEventCount; i++) out << kEvents[i].key << " = " << (sCfg.event[i] ? "true" : "false") << "\n";
    out << "autumn_moon = " << sCfg.moon << "\n";
    out << "parents_days = " << sCfg.parents << "\n";
}
const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

// Test aid CF_REGION=<key>,<key>,...|all[,moon=<0-3>][,parents=<0-7>]: overrides the settings
// for this run (settings.ini untouched).
struct EnvOverride {
    bool set = false;
    Config cfg;
};
const EnvOverride sEnv = [] {
    EnvOverride e;
    const char* v = std::getenv("CF_REGION");
    if (!v) return e;
    e.set = true;
    std::string s = v;
    size_t p = 0;
    while (p <= s.size()) {
        const size_t q = s.find(',', p);
        const std::string tok = s.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (tok == "all")
            for (bool& b : e.cfg.event) b = true;
        else if (tok.rfind("moon=", 0) == 0)
            e.cfg.moon = std::atoi(tok.c_str() + 5) & 3;
        else if (tok.rfind("parents=", 0) == 0)
            e.cfg.parents = std::atoi(tok.c_str() + 8) & 7;
        else
            for (int i = 0; i < kEventCount; i++)
                if (tok == kEvents[i].key) e.cfg.event[i] = true;
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return e;
}();
const Config& cfg() { return sEnv.set ? sEnv.cfg : sCfg; }

const bool sLog = [] {
    const char* v = std::getenv("CF_REGION_LOG");
    return v && *v && *v != '0';
}();

// ---- Version addresses ----------------------------------------------------------------------

struct Addrs {
    u32 table;      // event table
    u32 townData;   // fn returning the town data the event code reads (+0x735C2)
};
// USA RUUE01 / Europe RUUP01 / Japan RUUJ01 rev 1 (config/<id>/us_map.txt).
constexpr Addrs kAddrUS{0x8046EEF0, 0x8010DD60};
constexpr Addrs kAddrPAL{0x8046EEF0, 0x8010DA10};
constexpr Addrs kAddrJPN{0x8046EE30, 0x8010D950};
constexpr u32 kTownLangOffset = 0x735C2;
constexpr int kTableSize = 47;

const Addrs* addrs() {
    static const Addrs* a = [] () -> const Addrs* {
        const char* id = g_module ? g_module->gameId : "";
        if (!std::strcmp(id, "RUUE01")) return &kAddrUS;
        if (!std::strcmp(id, "RUUP01")) return &kAddrPAL;
        if (!std::strcmp(id, "RUUJ01")) return &kAddrJPN;
        return nullptr;
    }();
    return a;
}

u32 record(int index) { return addrs()->table + 12 * u32(index); }

bool anyEnabled() {
    const Config& c = cfg();
    if (c.moon || c.parents) return true;
    for (bool b : c.event)
        if (b) return true;
    return false;
}

// The town's language index (high nibble of +0x735C2). guestCall clobbers r3-r12: callers keep
// what they need.
u32 townLanguage(Ctx& c) {
    const u32 town = guestCall(c, addrs()->townData);
    return (rd8(town + kTownLangOffset) >> 4) & 0xF;
}

bool isOwn(int index, u32 lang) { return (rd16(record(index) + 4) >> lang) & 1; }

// The record's date test on its own (no region check): fn(date, &month, &day).
bool dateMatches(Ctx& c, int index, u32 date) {
    const u32 rec = record(index);
    const u32 fn = rd32(rec);
    if (!fn) return false;
    return guestCall(c, fn, date, rec + 9, rec + 10) != 0;
}

bool isMoon(int index) { return index == 29 || index == 35 || index == 39; }

// Is a foreign record switched on? (own records: see ownSuppressed)
bool foreignEnabled(int index) {
    const Config& c = cfg();
    if (isMoon(index)) return c.moon && kMoonRecord[c.moon] == index;
    for (int i = 0; i < kEventCount; i++)
        if (kEvents[i].index == index) return c.event[i];
    return false;
}
// An own record replaced by a setting (the own Autumn Moon when another variant is chosen).
bool ownSuppressed(int index) {
    const Config& c = cfg();
    return isMoon(index) && c.moon && kMoonRecord[c.moon] != index;
}
bool sameKind(int a, int b) {
    const u32 ra = record(a), rb = record(b);
    return rd8(ra + 6) == rd8(rb + 6) && rd8(ra + 7) == rd8(rb + 7);
}

// -1 = the original answer; 0/1 = ours.
int decide(Ctx& c, int index, u32 date) {
    if (index < 0 || index >= kTableSize) return -1;
    const u32 lang = townLanguage(c);
    if (lang > 9) return -1;
    if (isOwn(index, lang)) return ownSuppressed(index) ? 0 : -1;
    if (!foreignEnabled(index)) return -1;
    if (!dateMatches(c, index, date)) return 0;
    // Priority: the town's own events of the same kind, then earlier enabled foreign ones.
    for (int j = 0; j < kTableSize; j++) {
        if (j == index || !sameKind(index, j)) continue;
        const bool own = isOwn(j, lang);
        if (own ? ownSuppressed(j) : (j > index || !foreignEnabled(j))) continue;
        if (dateMatches(c, j, date)) {
            if (sLog) log("[region] event %d skipped: event %d (%s) runs that day", index, j, own ? "own" : "foreign");
            return 0;
        }
    }
    return 1;
}

void eventHook(Ctx& c, const char* orig) {
    if (!addrs() || !anyEnabled()) return callOriginal(c, orig);
    const u32 idxPtr = c.r[3], date = c.r[4];
    const int index = int(rd32(idxPtr));
    const int d = decide(c, index, date);
    if (d < 0) {
        c.r[3] = idxPtr;
        c.r[4] = date;
        return callOriginal(c, orig);
    }
    if (sLog && d) {
        static u32 sLastKey = ~0u;
        const u32 key = (u32(index) << 16) ^ rd32(date + 0xC) ^ (rd32(date + 0x10) << 8);
        if (key != sLastKey) {
            sLastKey = key;
            log("[region] event %d runs on %d/%d (other region)", index, int(rd32(date + 0x10)) + 1, int(rd32(date + 0xC)));
        }
    }
    c.r[3] = u32(d);
}

// Mother's/Father's Day date rules with another language's rule: the nibble is swapped for
// the call only.
void parentsHook(Ctx& c, const char* orig) {
    const int lang = addrs() ? kParentLang[cfg().parents] : -1;
    if (lang < 0) return callOriginal(c, orig);
    const u32 date = c.r[3];
    const u32 town = guestCall(c, addrs()->townData);
    const u32 a = town + kTownLangOffset;
    const u32 saved = rd8(a);
    wr8(a, (saved & 0x0F) | (u32(lang) << 4));
    c.r[3] = date;
    callOriginal(c, orig);
    wr8(a, saved);
}

}  // namespace

// USA, Europe, Japan (hooks.txt): fn_80089890 "event runs on date", fn_800895E0 Mother's Day,
// fn_800897B0 Father's Day.
HLE(fn_80089890) { eventHook(c, "fn_80089890"); }
HLE(fn_800898B0) { eventHook(c, "fn_800898B0"); }
HLE(fn_80089818) { eventHook(c, "fn_80089818"); }
HLE(fn_800895E0) { parentsHook(c, "fn_800895E0"); }
HLE(fn_80089600) { parentsHook(c, "fn_80089600"); }
HLE(fn_80089568) { parentsHook(c, "fn_80089568"); }
HLE(fn_800897B0) { parentsHook(c, "fn_800897B0"); }
HLE(fn_800897D0) { parentsHook(c, "fn_800897D0"); }
HLE(fn_80089738) { parentsHook(c, "fn_80089738"); }

// ---- Test aids -------------------------------------------------------------------------------

namespace {

// CF_MEM_DUMP=<dir>,<frame>[,<frame>...]: MEM1 (24 MiB) and MEM2 (64 MiB) written to
// <dir>/mem1_<frame>.bin / mem2_<frame>.bin at those guest frames (finding game state).
void memDump(u32 frame) {
    static const char* env = std::getenv("CF_MEM_DUMP");
    if (!env) return;
    std::string s = env;
    const size_t comma = s.find(',');
    if (comma == std::string::npos) return;
    const std::string dir = s.substr(0, comma);
    bool hit = false;
    for (size_t p = comma + 1; p < s.size();) {
        if (u32(std::strtoul(s.c_str() + p, nullptr, 10)) == frame) hit = true;
        const size_t q = s.find(',', p);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    if (!hit) return;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/mem1_%u.bin", dir.c_str(), frame);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(hostp(0x80000000), 1, 0x01800000, f);
        std::fclose(f);
    }
    std::snprintf(path, sizeof path, "%s/mem2_%u.bin", dir.c_str(), frame);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(hostp(0x90000000), 1, 0x04000000, f);
        std::fclose(f);
    }
    log("[region] memory dumped at frame %u", frame);
}

// Actors (fBase-style objects): +0x00 profile id, +0x60 vtable (in the actor REL's data
// section 5), +0x64 position x/y/z, +0x70 previous position, +0x7C angles (s16). Found by the
// vtable pointer: the player (d_a_playerNP, module 80) and the holiday Tortimer
// (d_a_npc_sp_event_sonchoNP, module 36), USA rev 1 module ids.
struct ActorHit {
    u32 addr;
    u32 module;
};
std::vector<ActorHit> findActors(std::initializer_list<u32> modules) {
    std::vector<ActorHit> out;
    struct Range { u32 lo, hi, mod; };
    std::vector<Range> rs;
    for (u32 m : modules) {
        const u32 b = relBase(m, 5);
        if (b) rs.push_back({b, b + 0x4000, m});
    }
    if (rs.empty()) return out;
    auto scan = [&](u32 lo, u32 hi) {
        for (u32 a = lo; a + 0x70 < hi; a += 4) {
            const u32 vt = rd32(a + 0x60);
            for (const Range& r : rs)
                if (vt >= r.lo && vt < r.hi && rd32(a) < 0x400 && (vt & 3) == 0) {
                    const float x = rdf32(a + 0x64), z = rdf32(a + 0x6C);
                    if (x > 0 && x < 20000 && z > 0 && z < 20000) out.push_back({a, r.mod});
                }
        }
    };
    scan(0x80004000, 0x81800000);
    scan(0x90000000, 0x94000000);
    return out;
}

// CF_ACTOR_LOG=<every n frames>: player / holiday Tortimer actors and positions.
// CF_TELEPORT=<frame>:<x>,<y>,<z>|tortimer[;...]: the player moved there (tortimer = 48 units
// in front of the holiday Tortimer, if one exists).
void actorAids(u32 frame) {
    static const u32 every = [] {
        const char* v = std::getenv("CF_ACTOR_LOG");
        return v ? u32(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (every && frame % every == 0) {
        for (const ActorHit& h : findActors({80, 36}))
            log("[actor] f%u %s %08X id %X pos %.1f %.1f %.1f", frame, h.module == 80 ? "player" : "tortimer-event",
                h.addr, rd32(h.addr), rdf32(h.addr + 0x64), rdf32(h.addr + 0x68), rdf32(h.addr + 0x6C));
    }
    static const char* tp = std::getenv("CF_TELEPORT");
    if (!tp) return;
    for (const char* p = tp; *p;) {
        char* end = nullptr;
        const u32 f = u32(std::strtoul(p, &end, 10));
        if (*end != ':') break;
        const char* arg = end + 1;
        const char* next = std::strchr(arg, ';');
        if (f == frame) {
            float x = 0, y = 0, z = 0;
            bool ok = false;
            const auto hits = findActors({80, 36});
            if (!std::strncmp(arg, "tortimer", 8)) {
                for (const ActorHit& h : hits)
                    if (h.module == 36) {
                        x = rdf32(h.addr + 0x64);
                        y = rdf32(h.addr + 0x68);
                        z = rdf32(h.addr + 0x6C) + 48.0f;
                        ok = true;
                    }
            } else {
                ok = std::sscanf(arg, "%f,%f,%f", &x, &y, &z) == 3;
            }
            for (const ActorHit& h : hits)
                if (ok && h.module == 80) {
                    for (u32 o : {0x64u, 0x70u}) {
                        wrf32(h.addr + o, x);
                        wrf32(h.addr + o + 4, y);
                        wrf32(h.addr + o + 8, z);
                    }
                    log("[actor] f%u player %08X moved to %.1f %.1f %.1f", frame, h.addr, x, y, z);
                }
            if (!ok) log("[actor] f%u teleport target not found (%s)", frame, arg);
        }
        if (!next) break;
        p = next + 1;
    }
}

}  // namespace

void onGuestFrame(u32 frame) {
    memDump(frame);
    actorAids(frame);
}

// ---- Menu ------------------------------------------------------------------------------------

bool menuSection() {
    bool dirty = false;
    ImGui::SeparatorText("Content from other regions");
    ImGui::TextWrapped(
        "Every disc carries every region's holidays, dialogue and items. These options let this game "
        "also hold the other regions' ones, with their own dialogue in your language. Off = the game "
        "as it shipped. The town keeps its region: saves, online play and the town's own events are "
        "unchanged. When two holidays fall on one day, the town's own one wins.");
    if (sEnv.set) ImGui::TextDisabled("(CF_REGION is set: these settings are overridden for this run)");

    bool all = true;
    for (bool b : sCfg.event) all = all && b;
    if (recomp::menu::Checkbox("All events from other regions", &all)) {
        for (bool& b : sCfg.event) b = all;
        dirty = true;
    }
    ImGui::SetItemTooltip("Every region-exclusive holiday below. The town's own region's holidays always run.");
    for (int r = kJP; r <= kKR; r++) {
        ImGui::PushID(r);
        recomp::menu::openTreesForCheck();
        if (ImGui::TreeNodeEx(kRegionName[r], ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < kEventCount; i++) {
                if (kEvents[i].region != r) continue;
                dirty |= recomp::menu::Checkbox(kEvents[i].name, &sCfg.event[i]);
                ImGui::SetItemTooltip("%s", kEvents[i].when);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (ImGui::Combo("Autumn Moon", &sCfg.moon, kMoonName, 4)) dirty = true;
    ImGui::SetItemTooltip(
        "The autumn full moon holiday of another region instead of the town's own: Tortimer's gift "
        "(wheat bundle, veggie basket or dango) and the villagers' moon talk. Japan's Tsukimi follows "
        "the lunar calendar, so its date can differ from the others' by a few days.");
    if (ImGui::Combo("Mother's and Father's Day dates", &sCfg.parents, kParentName, 8)) dirty = true;
    ImGui::SetItemTooltip(
        "Which country's dates the game uses for Mother's Day and Father's Day. The Korean rule "
        "moves Mother's Day to May 8 (Parent's Day) and has no Father's Day (Korea's Teacher's Day "
        "is a separate event above).");
    return dirty;
}

}  // namespace recomp::region
