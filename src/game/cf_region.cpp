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
#include "recomp/version.h"

#include "port/disc_folder.h"
#include "port/settings.h"

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <mutex>
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

// Other on/off options (shop stock, ...).
enum Flag { kShopNewYear, kShopHina, kShopFestive, kFlagCount };
struct FlagDef {
    const char* key;
    const char* name;
    const char* tip;
};
const FlagDef kFlags[kFlagCount] = {
    {"shop_new_year_jp", "Japan's New Year decorations at Nook's (Dec 26-31)",
     "From December 26 to 31 Nook's seasonal item is the kadomatsu or the kagamimochi, as in the "
     "Japanese game (North America: festive flag / candle; Europe and Korea: none)."},
    {"shop_hinaningyo", "Hinaningyo at Nook's (March 1-3)",
     "For Girl's Day Nook sells the hinaningyo doll set from March 1 to 3, as in the Japanese game."},
    {"shop_festive_flag_candle", "Festive flag and candle at Nook's (December)",
     "North America's extra December stock: the festive flag or festive candle instead of a festive "
     "tree on some days from December 1 to 23, and from December 26 to 31."},
};

struct Config {
    bool event[kEventCount] = {};
    bool flag[kFlagCount] = {};
    int moon = 0;     // index into kMoonRecord
    int parents = 0;  // index into kParentLang
    int houses = 0;   // villager house set: 0 the town's own, 1 North America, 2 Europe, 3 Japan/Korea
    int title = 0;    // title screen logo: 0 the disc's own, 1 USA, 2 Europe, 3 Japan, 4 Korea
};
Config sCfg;

void resetConfig() { sCfg = Config{}; }
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "region") return;
    const bool on = value == "true" || value == "1";
    for (int i = 0; i < kEventCount; i++)
        if (key == kEvents[i].key) sCfg.event[i] = on;
    for (int i = 0; i < kFlagCount; i++)
        if (key == kFlags[i].key) sCfg.flag[i] = on;
    if (key == "autumn_moon") sCfg.moon = std::atoi(value.c_str());
    if (key == "parents_days") sCfg.parents = std::atoi(value.c_str());
    if (key == "villager_houses") sCfg.houses = std::atoi(value.c_str());
    if (sCfg.houses < 0 || sCfg.houses > 3) sCfg.houses = 0;
    if (key == "title_screen") sCfg.title = std::atoi(value.c_str());
    if (sCfg.title < 0 || sCfg.title > 4) sCfg.title = 0;
    if (sCfg.moon < 0 || sCfg.moon > 3) sCfg.moon = 0;
    if (sCfg.parents < 0 || sCfg.parents > 7) sCfg.parents = 0;
}
void saveConfig(std::ostream& out) {
    out << "\n[region]\n";
    for (int i = 0; i < kEventCount; i++) out << kEvents[i].key << " = " << (sCfg.event[i] ? "true" : "false") << "\n";
    for (int i = 0; i < kFlagCount; i++) out << kFlags[i].key << " = " << (sCfg.flag[i] ? "true" : "false") << "\n";
    out << "autumn_moon = " << sCfg.moon << "\n";
    out << "parents_days = " << sCfg.parents << "\n";
    out << "villager_houses = " << sCfg.houses << "\n";
    out << "title_screen = " << sCfg.title << "\n";
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
        if (tok == "all") {
            for (bool& b : e.cfg.event) b = true;
            for (bool& b : e.cfg.flag) b = true;
        }
        else if (tok.rfind("moon=", 0) == 0)
            e.cfg.moon = std::atoi(tok.c_str() + 5) & 3;
        else if (tok.rfind("parents=", 0) == 0)
            e.cfg.parents = std::atoi(tok.c_str() + 8) & 7;
        else if (tok.rfind("houses=", 0) == 0)
            e.cfg.houses = std::atoi(tok.c_str() + 7) & 3;
        else if (tok.rfind("title=", 0) == 0)
            e.cfg.title = std::atoi(tok.c_str() + 6) % 5;
        else
            for (int i = 0; i < kEventCount; i++)
                if (tok == kEvents[i].key) e.cfg.event[i] = true;
        for (int i = 0; i < kFlagCount; i++)
            if (tok == kFlags[i].key) e.cfg.flag[i] = true;
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
    u32 shopFn;     // Nook's seasonal item choice (US fn_801456B4); region getter calls at +0x398.. +0x444
    u32 letterFn;   // event letter templates by region (US fn_800D2084, MAIL_ETC_MotherEvent); getter at +0x20
};
constexpr u32 kTownLangOffset = 0x735C2;
constexpr int kTableSize = 47;

// USA addresses, mapped to the running version by VA() (recomp/version.h; the table is identical
// on every disc, only its address moves).
const Addrs* addrs() {
    static const Addrs a{VA(0x8046EEF0), VA(0x8010DD60), VA(0x801456B4), VA(0x800D2084)};
    return a.table ? &a : nullptr;
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
    const u32 lr = c.lr;
    const int d = decide(c, index, date);
    if (sLog && d >= 0) {  // who asks about a foreign event (once per caller)
        static std::vector<u64> sSeen;
        const u64 k = (u64(lr) << 8) | u32(index);
        if (std::find(sSeen.begin(), sSeen.end(), k) == sSeen.end()) {
            sSeen.push_back(k);
            log("[region] event %d asked by %08X -> %d", index, lr, d);
        }
    }
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

namespace {
// The region getter (US fn_801069D0: li r3, <region>; blr) answered per call site: only the
// listed callers see another region, every other caller (save check, online URLs, profanity
// list, keyboards, ...) the disc's own.
//   Nook's seasonal item (shopFn): +0x398 "region == USA" for Dec 1-23 (festive flag/candle
//   or tree), +0x41C "== Japan" Dec 26-31 (kadomatsu/kagamimochi), +0x430 "== USA" Dec 26-31
//   (flag/candle), +0x444 "== Japan" for the Mar 1-3 hinaningyo check.
//   Event letters (letterFn +0x20): the region of the Mother's/Father's Day rule chosen above.
int regionGroupOfLang(int lang) { return lang == 0 ? 0 : lang <= 3 ? 1 : lang <= 8 ? 2 : 3; }
void getterHook(Ctx& c, const char* orig) {
    const u32 lr = c.lr;
    callOriginal(c, orig);
    const Addrs* a = addrs();
    if (!a) return;
    const Config& k = cfg();
    const u32 own = c.r[3];
    const u32 site = lr - 4;
    if (site - a->shopFn < 0x1000) {
        const u32 off = site - a->shopFn;
        if ((off == 0x398 || off == 0x430) && k.flag[kShopFestive]) c.r[3] = 1;
        if (off == 0x41C && k.flag[kShopNewYear]) c.r[3] = 0;
        if (off == 0x444 && k.flag[kShopHina]) c.r[3] = 0;
        // Dec 26-31 with both options on a European/Korean town: Japan's item (the game asks
        // "Japan?" first), as a Japanese game would.
    } else if (site == a->letterFn + 0x20 && kParentLang[k.parents] >= 0) {
        c.r[3] = u32(regionGroupOfLang(kParentLang[k.parents]));
    }
    if (sLog && c.r[3] != own) log("[region] region getter at %08X: %u instead of %u", site, c.r[3], own);
}
}  // namespace

HLE(fn_801069D0) { getterHook(c, "fn_801069D0"); }

// Every version by the USA names (hooks.txt "all"): fn_80089890 "event runs on date", fn_800895E0 Mother's Day,
// fn_800897B0 Father's Day.
HLE(fn_80089890) { eventHook(c, "fn_80089890"); }
HLE(fn_800895E0) { parentsHook(c, "fn_800895E0"); }
HLE(fn_800897B0) { parentsHook(c, "fn_800897B0"); }

// ---- Villager houses -------------------------------------------------------------------------
//
// The town's structures come from one archive per region and language,
// /Str/Brres/field<Usa|Eur|Jpn|Kor><Eng|Fra|Spa|Ger|Ita|Jpn|Kor>StrBrres<S|W>.arc (fn_80166FC4,
// region and language from the town's +0x735C2 byte), with the five villager house models
// TN_obj_house0-4.brres and their textures obj_house_tex.brres; the shops' signs etc. in it are
// per language. Every disc has every region's archive. The house set is not saved per house: a
// villager's house style is an index 0-4, and the models behind the indices are the town
// region's. So "another region's houses" = the town's own archive with those six members taken
// from the other region's archive, served as a disc overlay (shop signs stay in the town's
// language), plus the house-type data the field actor (d_a_str_fieldNP fn_102_1E10) picks from
// obj_npc_common by the town's region group (obj_house_usa / _eur / _jpn): fn_80169890 answers
// the chosen region for calls from that module. It applies to new and existing towns alike
// (nothing is written to the save), from the next time the town is loaded.

namespace {
const char* const kHouseName[] = {"The town's own", "North America (Ornate, Pink and three shared)",
                                  "Europe (Brick, Log and three shared)", "Japan / Korea (Zen, Farmhouse and three shared)"};
const char* const kHouseDonor[] = {nullptr, "UsaEng", "EurEng", "JpnJpn"};
const char* const kHouseRegion[] = {nullptr, "usa", "eur", "jpn"};
const char* const kHouseMembers[] = {"TN_obj_house0.brres", "TN_obj_house1.brres", "TN_obj_house2.brres",
                                     "TN_obj_house3.brres", "TN_obj_house4.brres", "obj_house_tex.brres"};
constexpr s32 kOverlayBase = 0x40000000;

struct Overlay {
    std::string key;  // lower-case path + option
    std::vector<u8> data;
};
std::mutex sOvM;
std::vector<Overlay> sOverlays;

std::string lower(const char* s) {
    std::string r = s;
    for (char& ch : r) ch = char(std::tolower(u8(ch)));
    return r;
}

bool readDiscFile(const std::string& path, std::vector<u8>& out) {
    if (!port::discfolder::active()) return false;
    const s32 n = port::discfolder::pathToEntry(path.c_str());
    const port::discfolder::Entry* e = n >= 0 ? port::discfolder::entry(n) : nullptr;
    if (!e || e->isDir) return false;
    out.resize(e->length);
    return port::discfolder::read(n, out.data(), e->length, 0) == s64(e->length);
}

u32 be32(const std::vector<u8>& d, size_t o) { return u32(d[o]) << 24 | u32(d[o + 1]) << 16 | u32(d[o + 2]) << 8 | d[o + 3]; }
void putBe32(std::vector<u8>& d, size_t o, u32 v) {
    d[o] = u8(v >> 24);
    d[o + 1] = u8(v >> 16);
    d[o + 2] = u8(v >> 8);
    d[o + 3] = u8(v);
}

struct U8File {
    std::string name;  // base name
    u32 node, off, size;
};
bool parseU8(const std::vector<u8>& d, std::vector<U8File>& files, u32& dataStart) {
    if (d.size() < 0x20 || be32(d, 0) != 0x55AA382D) return false;
    const u32 root = be32(d, 4);
    dataStart = be32(d, 12);
    if (root + 12 > d.size()) return false;
    const u32 n = be32(d, root + 8);
    const u32 strtab = root + n * 12;
    if (strtab > d.size()) return false;
    for (u32 i = 1; i < n; i++) {
        const u32 o = root + i * 12;
        if (d[o] != 0) continue;
        const u32 nameOff = be32(d, o) & 0xFFFFFF;
        std::string name;
        for (u32 k = strtab + nameOff; k < d.size() && d[k]; k++) name += char(d[k]);
        files.push_back({name, o, be32(d, o + 4), be32(d, o + 8)});
    }
    return true;
}

// The town's archive with the house members of `donor`; empty on any problem.
std::vector<u8> buildHouseArchive(const std::vector<u8>& own, const std::vector<u8>& donor) {
    std::vector<U8File> of, df;
    u32 oData = 0, dData = 0;
    if (!parseU8(own, of, oData) || !parseU8(donor, df, dData)) return {};
    std::vector<u8> out(own.begin(), own.begin() + oData);
    int replaced = 0;
    for (const U8File& f : of) {
        const u8* src = own.data() + f.off;
        u32 size = f.size;
        for (const char* m : kHouseMembers)
            if (f.name == m)
                for (const U8File& g : df)
                    if (g.name == f.name && g.off + g.size <= donor.size()) {
                        src = donor.data() + g.off;
                        size = g.size;
                        replaced++;
                    }
        if (f.off + f.size > own.size()) return {};
        out.resize((out.size() + 31) & ~size_t(31), 0);
        putBe32(out, f.node + 4, u32(out.size()));
        putBe32(out, f.node + 8, size);
        out.insert(out.end(), src, src + size);
    }
    if (replaced != 6) return {};
    out.resize((out.size() + 31) & ~size_t(31), 0);
    return out;
}

int housesOption() { return cfg().houses; }
bool isOverlayEntryLocked(s32 e) { return e >= kOverlayBase && size_t(e - kOverlayBase) < sOverlays.size(); }


// Title screen logo of another region: d_title_demoNP opens its disc's /Layout/title_<us|eu|jp|kr>.arc
// (the only difference of that REL between versions); every disc has title_us/eu/jp, the Korean
// one has title_kr too. The chosen file is served under the opened name.
s32 titleOverlay(const std::string& p) {
    const int t = cfg().title;
    static const char* const kTitle[] = {nullptr, "us", "eu", "jp", "kr"};
    if (!t || p.size() != std::strlen("/layout/title_us.arc") || p.compare(0, 14, "/layout/title_") != 0 ||
        p.compare(16, 4, ".arc") != 0 || p.compare(14, 2, kTitle[t]) == 0)
        return -1;
    const std::string want = std::string("/Layout/title_") + kTitle[t] + ".arc";
    const std::string key = p + "#title" + char('0' + t);
    std::lock_guard lk(sOvM);
    for (size_t i = 0; i < sOverlays.size(); i++)
        if (sOverlays[i].key == key) return kOverlayBase + s32(i);
    std::vector<u8> data;
    if (!readDiscFile(want, data)) return -1;  // e.g. title_kr on a non-Korean disc
    log("[region] title screen: %s served as %s", want.c_str(), p.c_str());
    sOverlays.push_back({key, std::move(data)});
    return kOverlayBase + s32(sOverlays.size() - 1);
}

}  // namespace

s32 overlayEntry(const char* path) {
    if (!path) return -1;
    const int h = housesOption();
    if (!h && !cfg().title) return -1;
    std::string p = lower(path);
    if (p.empty() || p[0] != '/') p = "/" + p;  // relative paths are relative to the root here
    if (cfg().title) {
        const s32 e = titleOverlay(p);
        if (e >= 0) return e;
    }
    if (!h) return -1;
    // "/str/brres/field" + region(3) + language(3) + "strbrres" + season + ".arc"
    static const std::string pre = "/str/brres/field";
    if (p.size() != pre.size() + 6 + 8 + 1 + 4 || p.compare(0, pre.size(), pre) != 0) return -1;
    const std::string rgn = p.substr(pre.size(), 3);
    const char season = char(std::toupper(u8(p[pre.size() + 6 + 8])));
    if (p.compare(pre.size() + 6, 8, "strbrres") != 0 || p.compare(p.size() - 4, 4, ".arc") != 0) return -1;
    if (rgn == kHouseRegion[h] || (h == 3 && rgn == "kor")) return -1;  // already that region's houses
    const std::string key = p + char('0' + h);
    std::lock_guard lk(sOvM);
    for (size_t i = 0; i < sOverlays.size(); i++)
        if (sOverlays[i].key == key) return kOverlayBase + s32(i);
    std::vector<u8> own, donor;
    const std::string donorPath = std::string("/Str/Brres/field") + kHouseDonor[h] + "StrBrres" + season + ".arc";
    if (!readDiscFile(path, own) || !readDiscFile(donorPath, donor)) return -1;
    std::vector<u8> built = buildHouseArchive(own, donor);
    if (built.empty()) {
        log("[region] villager houses: could not combine %s with %s", path, donorPath.c_str());
        return -1;
    }
    log("[region] villager houses: %s served with the houses of %s (%zu bytes)", path, donorPath.c_str(), built.size());
    sOverlays.push_back({key, std::move(built)});
    return kOverlayBase + s32(sOverlays.size() - 1);
}
bool isOverlayEntry(s32 e) {
    std::lock_guard lk(sOvM);
    return isOverlayEntryLocked(e);
}
u32 overlayLength(s32 e) {
    std::lock_guard lk(sOvM);
    return isOverlayEntryLocked(e) ? u32(sOverlays[size_t(e - kOverlayBase)].data.size()) : 0;
}
s32 overlayRead(s32 e, void* dst, s32 len, s32 off) {
    std::lock_guard lk(sOvM);
    if (!isOverlayEntryLocked(e) || off < 0 || len < 0) return -1;
    const std::vector<u8>& d = sOverlays[size_t(e - kOverlayBase)].data;
    if (size_t(off) >= d.size()) return 0;
    const size_t n = std::min(size_t(len), d.size() - size_t(off));
    std::memcpy(dst, d.data() + off, n);
    return s32(n);
}

namespace {
// The house-type data of d_a_str_fieldNP (module 102): its one call of the town region getter.
void townRegionHook(Ctx& c, const char* orig) {
    const u32 lr = c.lr;
    callOriginal(c, orig);
    const int h = housesOption();
    if (!h) return;
    const u32 text = relBase(102, 1);
    if (text && lr - text < 0x5108) c.r[3] = h == 1 ? 1u : h == 2 ? 2u : 0u;
}
}  // namespace
HLE(fn_80169890) { townRegionHook(c, "fn_80169890"); }

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
                if (vt >= r.lo && vt < r.hi && rd32(a) - 1 < 0x3FF && (vt & 3) == 0) {
                    const float x = rdf32(a + 0x64), z = rdf32(a + 0x6C);
                    if (x > 1 && x < 20000 && z > 1 && z < 20000) out.push_back({a, r.mod});
                }
        }
    };
    scan(0x80004000, 0x81800000);
    scan(0x90000000, 0x94000000);
    return out;
}

// CF_ACTOR_LOG=<every n frames>: player / holiday Tortimer actors and positions.
// CF_TELEPORT=<frame>:<x>,<y>,<z>|tortimer|mod<N>[:dx,dz][;...]: the player moved there
// (tortimer = 48 units in front of the holiday Tortimer; mod<N>[#k][t<type>][:dx,dz] = next to the k-th / last actor of
// module N, e.g. mod13 = the bulletin board's bird).
void actorAids(u32 frame) {
    static const u32 every = [] {
        const char* v = std::getenv("CF_ACTOR_LOG");
        return v ? u32(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (every && frame % every == 0) {
        // CF_ACTOR_MODULES=<id>,...: more actor modules to list (default: player 80, holiday
        // Tortimer 36, Tortimer 35).
        static const std::vector<u32> mods = [] {
            std::vector<u32> m = {80, 36, 35};
            if (const char* v = std::getenv("CF_ACTOR_MODULES"))
                for (const char* p = v; *p;) {
                    char* end = nullptr;
                    m.push_back(u32(std::strtoul(p, &end, 10)));
                    if (end == p) break;
                    p = *end == ',' ? end + 1 : end;
                }
            return m;
        }();
        std::vector<ActorHit> hits;
        for (u32 m : mods)
            for (const ActorHit& h : findActors({m})) hits.push_back(h);
        for (const ActorHit& h : hits)
            log("[actor] f%u module %u %08X id %X pos %.1f %.1f %.1f", frame, h.module, h.addr, rd32(h.addr),
                rdf32(h.addr + 0x64), rdf32(h.addr + 0x68), rdf32(h.addr + 0x6C));
        // Today's event list (fn_80089D3C, USA 0x805855E8 via VA: 10 x {event, active}).
        if (addrs()) {
            std::string s;
            for (u32 i = 0; i < 10; i++)
                if (rd32(VA(0x805855E8) + 8 * i + 4)) s += " " + std::to_string(rd32(VA(0x805855E8) + 8 * i));
            log("[actor] f%u today's events:%s", frame, s.empty() ? " none" : s.c_str());
        }
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
            const auto hits = findActors({80});
            u32 mod = 0;
            float dx = 0, dz = 48;
            if (!std::strncmp(arg, "tortimer", 8)) mod = 36;
            int pick = -1;
            u32 pickType = 0;
            if (!std::strncmp(arg, "mod", 3)) {  // mod<N>[#k][:dx,dz]
                char* e2 = nullptr;
                mod = u32(std::strtoul(arg + 3, &e2, 10));
                for (int n = 0; n < 2; n++) {  // "#k" and "t<type>" (structure type, +0x8) in either order
                    if (*e2 == '#') pick = int(std::strtol(e2 + 1, &e2, 10));
                    if (*e2 == 't') pickType = u32(std::strtoul(e2 + 1, &e2, 16));
                }
                if (*e2 == ':') std::sscanf(e2 + 1, "%f,%f", &dx, &dz);
            }
            if (mod) {
                int k = 0;
                for (const ActorHit& h : findActors({mod})) {
                    if (pickType && rd32(h.addr + 8) >> 16 != pickType) continue;
                    if (pick < 0 || k++ == pick) {
                        x = rdf32(h.addr + 0x64) + dx;
                        y = rdf32(h.addr + 0x68);
                        z = rdf32(h.addr + 0x6C) + dz;
                        ok = true;
                        log("[actor] f%u target %08X id %X type %04X", frame, h.addr, rd32(h.addr), rd32(h.addr + 8) >> 16);
                    }
                }
            } else {
                ok = std::sscanf(arg, "%f,%f,%f", &x, &y, &z) == 3;
            }
            for (const ActorHit& h : hits)
                if (ok && h.module == 80 && rd32(h.addr + 0x60) == relBase(80, 5) + 0x1280) {  // the player class vtable
                    for (u32 o : {0x64u, 0x70u}) {
                        wrf32(h.addr + o, x);
                        wrf32(h.addr + o + 4, y);
                        wrf32(h.addr + o + 8, z);
                    }
                    log("[actor] f%u player %08X moved to %.1f %.1f %.1f", frame, h.addr, x, y, z);
                    break;
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
        "is a separate event above). The Mother's Day letter from your mom follows the same "
        "country.");
    static const char* const kTitleName[] = {"The disc's own", "North America (City Folk)", "Europe (Let's Go to the City)",
                                             "Japan (Machi e Ikou yo)", "Korea (needs the Korean disc)"};
    if (ImGui::Combo("Title screen logo", &sCfg.title, kTitleName, 5)) dirty = true;
    ImGui::SetItemTooltip(
        "The title screen's logo layout of another region (every disc carries the North American, "
        "European and Japanese ones; the Korean one is only on the Korean disc, so with another disc "
        "that choice keeps the disc's own). Takes effect the next time the title screen loads.");
    if (ImGui::Combo("Villager house styles", &sCfg.houses, kHouseName, 4)) dirty = true;
    ImGui::SetItemTooltip(
        "The five villager house exteriors come in a set per region: North America (two of them "
        "Ornate and Pink), Europe (Brick and Log), Japan and Korea (Zen and Farmhouse). Every "
        "villager's house is drawn from the chosen region's set; shop signs stay in your language. "
        "Not saved in the town: it applies to new and existing towns from the next time the town "
        "loads, and switching back restores the town's own look.");
    ImGui::TextUnformatted("Nook's seasonal items:");
    for (int i = kShopNewYear; i <= kShopFestive; i++) {
        dirty |= recomp::menu::Checkbox(kFlags[i].name, &sCfg.flag[i]);
        ImGui::SetItemTooltip("%s", kFlags[i].tip);
    }
    return dirty;
}

}  // namespace recomp::region
