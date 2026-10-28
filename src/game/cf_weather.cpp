// Real-time weather (F1 > Enhancements > Gameplay "Real-time weather", off by default;
// docs/CALENDAR.md "Real-time weather").
//
// How City Folk decides its weather (USA addresses; every version through VA(), recomp/version.h):
// each day gets one of 32 day patterns (fn_801C9AA0: a fixed pattern for event days from
// 0x8047EC00 via fn_801C99F8, otherwise a seeded random pick weighted by the weather period of the
// date, fn_801C973C -> 0x805005D8[period]); today's and tomorrow's patterns for the town and the
// city are cached in r13-0x2AC4 / -0x2AC0 / -0x2ABC / -0x2AB8 (fn_801C978C). A pattern is 24 hourly
// states from 6 AM (0x80500558[pattern][hour - 6]): 0 clear, 1 some clouds, 2 cloudy, 3 rain,
// 4 heavy rain; fn_801C9E48 (town) / fn_801C9EC0 (city) return the state of a given hour and turn
// 3/4 into 5/6 (snow / heavy snow) while the term is 17 or later (Nov 26 - Feb 24, fn_801C9FC4).
// dWeather_c (fn_801C9010 create, fn_801C92F0 every frame) asks for the current hour and the next
// one (+0x5884 / +0x5888) and fades to the next state over the end of the hour.
//
// The hook replaces those two hourly answers while real data is available: the current
// conditions from Open-Meteo, mapped to a state the game can show in the current season (snow
// only while the game itself would turn rain into snow, rain otherwise; rain in the snow season
// becomes snow, as the game does). An hour's state is fixed the first time the game asks for it as
// the current hour, or as the next hour in its last 10 minutes (when the fade starts), so a new
// fetch never changes the sky in the middle of an hour; the hour in which the data first arrives
// keeps the game's own weather. With no result younger than 3 hours (offline, error, option off)
// the game's own weather runs unchanged. Patterns (and with them rainbows, the TV forecast,
// watering by rain) stay the game's.
//
// Network: Open-Meteo geocoding (place search, only when the user presses Search) and forecast
// ("current" block), no key, on a background thread, every 30 minutes while the option is on;
// the place is stored as its name plus coordinates rounded to 0.1 degrees ([weather] in
// settings.ini), the last answer in weather_cache.txt next to it. Data CC BY 4.0, "Weather data by
// Open-Meteo.com" shown in the menu.
//
// Test aids: CF_WEATHER_FAKE=clear|cloudy|overcast|rain|heavyrain|storm|snow|heavysnow (no network;
// applied at once, no hour latching), CF_WEATHER_TEST=<place name> (searches, takes the first match,
// fetches and logs, for this run only), CF_WEATHER_SEARCH=<text> (types it into the F1 search box
// and presses Search once: the pick list), CF_WEATHER_LOG=1 (fetches, and every hourly answer that
// differs from the game's own).
#include "game/cf_calendar.h"
#include "recomp/version.h"

#include "port/http.h"
#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/menu.h"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace recomp::calendar {
namespace {

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
const bool sLog = envFlag("CF_WEATHER_LOG");

// ---- settings --------------------------------------------------------------------------------
struct Place {
    std::string name;  // "Berlin, Land Berlin, Germany"
    double lat = 0, lon = 0;
    bool set = false;
};

std::mutex sMu;  // guards everything below that the menu, the worker and the guest share
bool sEnabled = false;
Place sPlace;
std::string sCountry;  // optional ISO 3166 alpha-2 filter for the search

double round1(double v) { return std::round(v * 10.0) / 10.0; }

void resetConfig() {
    std::lock_guard lk(sMu);
    sEnabled = false;
    sPlace = {};
    sCountry.clear();
}
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "weather") return;
    std::lock_guard lk(sMu);
    if (key == "real_time") sEnabled = value == "true" || value == "1";
    else if (key == "place") sPlace.name = value;
    else if (key == "latitude") {
        sPlace.lat = round1(std::atof(value.c_str()));
        sPlace.set = !value.empty();
    } else if (key == "longitude") sPlace.lon = round1(std::atof(value.c_str()));
    else if (key == "country") sCountry = value;
}
void saveConfig(std::ostream& out) {
    std::lock_guard lk(sMu);
    out << "\n[weather]\nreal_time=" << (sEnabled ? "true" : "false") << "\nplace=" << sPlace.name << "\n";
    if (sPlace.set) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "latitude=%.1f\nlongitude=%.1f\n", sPlace.lat, sPlace.lon);
        out << buf;
    } else {
        out << "latitude=\nlongitude=\n";
    }
    out << "country=" << sCountry << "\n";
}
[[maybe_unused]] const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

// ---- conditions ------------------------------------------------------------------------------
struct Conditions {
    bool valid = false;
    long long fetched = 0;  // unix seconds
    int code = 0;           // WMO weather code
    int cloud = 0;          // %
    int isDay = 1;
    double temp = 0, precip = 0, snowfall = 0;
    double lat = 0, lon = 0;
};
Conditions sCond;           // under sMu
std::string sStatus;        // last fetch result in plain words (menu)
long long sLastAttempt = 0; // unix seconds
bool sRefetch = false;      // place changed: fetch at once

struct SearchHit {
    std::string label;
    double lat, lon;
};
std::vector<SearchHit> sHits;
std::string sSearchQuery;  // pending search ("" none)
std::string sSearchStatus;
bool sSearching = false;

long long unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* fakeName() {
    static const char* v = std::getenv("CF_WEATHER_FAKE");
    return v && *v ? v : nullptr;
}

std::string cachePath() {
    const std::filesystem::path p = std::filesystem::u8path(port::settingsPath());
    const auto s = (p.parent_path() / "weather_cache.txt").u8string();
    return std::string(s.begin(), s.end());
}

void saveCache(const Conditions& w) {
    std::ofstream f(std::filesystem::u8path(cachePath()), std::ios::trunc);
    if (!f) return;
    f << "fetched=" << w.fetched << "\ncode=" << w.code << "\ncloud=" << w.cloud << "\nis_day=" << w.isDay
      << "\ntemp=" << w.temp << "\nprecip=" << w.precip << "\nsnowfall=" << w.snowfall << "\nlat=" << w.lat
      << "\nlon=" << w.lon << "\n";
}

void loadCache() {
    std::ifstream f(std::filesystem::u8path(cachePath()));
    if (!f) return;
    Conditions w;
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "fetched") w.fetched = std::atoll(v.c_str());
        else if (k == "code") w.code = std::atoi(v.c_str());
        else if (k == "cloud") w.cloud = std::atoi(v.c_str());
        else if (k == "is_day") w.isDay = std::atoi(v.c_str());
        else if (k == "temp") w.temp = std::atof(v.c_str());
        else if (k == "precip") w.precip = std::atof(v.c_str());
        else if (k == "snowfall") w.snowfall = std::atof(v.c_str());
        else if (k == "lat") w.lat = std::atof(v.c_str());
        else if (k == "lon") w.lon = std::atof(v.c_str());
    }
    w.valid = w.fetched > 0;
    std::lock_guard lk(sMu);
    // Only for the place still chosen.
    if (w.valid && sPlace.set && std::fabs(w.lat - sPlace.lat) < 0.05 && std::fabs(w.lon - sPlace.lon) < 0.05) sCond = w;
}

const char* describe(int code) {
    switch (code) {
    case 0: return "clear sky";
    case 1: return "mainly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45:
    case 48: return "fog";
    case 51:
    case 53:
    case 55: return "drizzle";
    case 56:
    case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66:
    case 67: return "freezing rain";
    case 71: return "light snow";
    case 73: return "snow";
    case 75: return "heavy snow";
    case 77: return "snow grains";
    case 80:
    case 81: return "rain showers";
    case 82: return "violent rain showers";
    case 85: return "snow showers";
    case 86: return "heavy snow showers";
    case 95: return "thunderstorm";
    case 96:
    case 99: return "thunderstorm with hail";
    default: return "unknown";
    }
}

// WMO code + cloud cover -> game hour state (0 clear, 1 some clouds, 2 cloudy, 3 rain, 4 heavy
// rain, 5 snow, 6 heavy snow) for the season: snow only when the game would show snow itself.
int mapState(const Conditions& w, bool snowSeason) {
    int s;
    switch (w.code) {
    case 0:
    case 1: s = w.cloud >= 50 ? 1 : 0; break;
    case 2: s = 1; break;
    case 3:
    case 45:
    case 48: s = 2; break;
    case 51: case 53: case 55: case 56: case 57: case 61: case 63: case 66: case 80: case 81: s = 3; break;
    case 65: case 67: case 82: case 95: case 96: case 99: s = 4; break;
    case 71: case 73: case 77: case 85: s = 5; break;
    case 75: case 86: s = 6; break;
    default: s = w.cloud >= 70 ? 2 : w.cloud >= 30 ? 1 : 0; break;
    }
    if (s == 3 || s == 4) return snowSeason ? s + 2 : s;
    if (s == 5 || s == 6) return snowSeason ? s : s - 2;
    return s;
}
const char* kStateName[7] = {"clear", "some clouds", "cloudy", "rain", "heavy rain", "snow", "heavy snow"};

Conditions fakeConditions() {
    Conditions w;
    w.valid = true;
    w.fetched = unixNow();
    const std::string f = fakeName();
    struct F {
        const char* n;
        int code, cloud;
    };
    static constexpr F kF[] = {{"clear", 0, 0},     {"cloudy", 2, 50},    {"overcast", 3, 100}, {"rain", 63, 100},
                               {"heavyrain", 65, 100}, {"storm", 95, 100}, {"snow", 73, 100},    {"heavysnow", 75, 100}};
    for (const F& x : kF)
        if (f == x.n) {
            w.code = x.code;
            w.cloud = x.cloud;
        }
    return w;
}

// ---- network ---------------------------------------------------------------------------------
bool fetchConditions(double lat, double lon, Conditions& out, std::string& err) {
    char url[512];
    std::snprintf(url, sizeof(url),
                  "https://api.open-meteo.com/v1/forecast?latitude=%.1f&longitude=%.1f"
                  "&current=weather_code,cloud_cover,precipitation,rain,showers,snowfall,is_day,temperature_2m"
                  "&timezone=auto",
                  lat, lon);
    const port::http::Result r = port::http::get(url, 10000, 256 * 1024);
    if (!r.error.empty()) {
        err = r.error;
        return false;
    }
    if (r.status != 200) {
        err = "Open-Meteo answered HTTP " + std::to_string(r.status);
        return false;
    }
    port::http::Json j;
    if (!port::http::parseJson(r.body, j) || j["current"].type != port::http::Json::Object) {
        err = "unexpected answer from Open-Meteo";
        return false;
    }
    const auto& cur = j["current"];
    out.valid = true;
    out.fetched = unixNow();
    out.code = int(cur["weather_code"].num(0));
    out.cloud = int(cur["cloud_cover"].num(0));
    out.isDay = int(cur["is_day"].num(1));
    out.temp = cur["temperature_2m"].num(0);
    out.precip = cur["precipitation"].num(0);
    out.snowfall = cur["snowfall"].num(0);
    out.lat = lat;
    out.lon = lon;
    return true;
}

bool searchPlaces(const std::string& query, const std::string& country, std::vector<SearchHit>& hits, std::string& err) {
    std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=" + port::http::urlEncode(query) +
                      "&count=10&language=en&format=json";
    if (country.size() == 2) url += "&countryCode=" + port::http::urlEncode(country);
    const port::http::Result r = port::http::get(url, 10000, 256 * 1024);
    if (!r.error.empty()) {
        err = r.error;
        return false;
    }
    if (r.status != 200) {
        err = "Open-Meteo answered HTTP " + std::to_string(r.status);
        return false;
    }
    port::http::Json j;
    if (!port::http::parseJson(r.body, j)) {
        err = "unexpected answer from Open-Meteo";
        return false;
    }
    hits.clear();
    for (const auto& it : j["results"].a) {
        std::string label = it["name"].str();
        if (!it["admin1"].str().empty() && it["admin1"].str() != label) label += ", " + it["admin1"].str();
        if (!it["country"].str().empty()) label += ", " + it["country"].str();
        hits.push_back({label, round1(it["latitude"].num()), round1(it["longitude"].num())});
    }
    return true;
}

// ---- worker thread ---------------------------------------------------------------------------
std::once_flag sWorkerOnce;

void worker() {
    using namespace std::chrono_literals;
    loadCache();
    if (const char* test = std::getenv("CF_WEATHER_TEST"); test && *test) {
        std::vector<SearchHit> hits;
        std::string err;
        if (searchPlaces(test, "", hits, err) && !hits.empty()) {
            log("[weather] test search \"%s\": %zu matches, first %s (%.1f, %.1f)", test, hits.size(),
                hits[0].label.c_str(), hits[0].lat, hits[0].lon);
            std::lock_guard lk(sMu);
            sEnabled = true;
            sPlace = {hits[0].label, hits[0].lat, hits[0].lon, true};
            sRefetch = true;
        } else {
            log("[weather] test search \"%s\" failed: %s", test, err.empty() ? "no match" : err.c_str());
        }
    }
    for (;;) {
        std::string query, country;
        bool fetch = false;
        Place place;
        {
            std::lock_guard lk(sMu);
            if (!sSearchQuery.empty()) {
                query.swap(sSearchQuery);
                country = sCountry;
            }
            const long long now = unixNow();
            place = sPlace;
            if (sEnabled && place.set && !fakeName()) {
                const bool stale = !sCond.valid || now - sCond.fetched >= 30 * 60 || std::fabs(sCond.lat - place.lat) > 0.05 ||
                                   std::fabs(sCond.lon - place.lon) > 0.05;
                const bool retryOk = now - sLastAttempt >= 5 * 60;
                fetch = sRefetch || (stale && retryOk);
            }
            if (fetch) {
                sRefetch = false;
                sLastAttempt = now;
            }
        }
        if (!query.empty()) {
            std::vector<SearchHit> hits;
            std::string err;
            const bool ok = searchPlaces(query, country, hits, err);
            std::lock_guard lk(sMu);
            sSearching = false;
            if (!ok) sSearchStatus = "Search failed: " + err;
            else if (hits.empty()) sSearchStatus = "No match: try the nearest larger town, or the English name";
            else sSearchStatus.clear();
            sHits = std::move(hits);
            if (sLog) log("[weather] search \"%s\": %zu matches %s", query.c_str(), sHits.size(), err.c_str());
        }
        if (fetch) {
            Conditions w;
            std::string err;
            const bool ok = fetchConditions(place.lat, place.lon, w, err);
            {
                std::lock_guard lk(sMu);
                if (ok) {
                    sCond = w;
                    sStatus.clear();
                } else {
                    sStatus = err;
                }
            }
            if (ok) {
                saveCache(w);
                log("[weather] %s (%.1f, %.1f): code %d (%s), cloud %d%%, %.1f C, precipitation %.1f mm, snowfall %.1f cm",
                    place.name.c_str(), place.lat, place.lon, w.code, describe(w.code), w.cloud, w.temp, w.precip, w.snowfall);
            } else {
                log("[weather] fetch for %.1f, %.1f failed: %s", place.lat, place.lon, err.c_str());
            }
        }
        std::this_thread::sleep_for(1s);
    }
}

void startWorker() {
    std::call_once(sWorkerOnce, [] { std::thread(worker).detach(); });
}

// The worker starts the first time the game asks for the weather while the option is on (or the
// menu searches), so nothing happens on the network while the option is off.
const bool sTestRun = [] {
    const char* t = std::getenv("CF_WEATHER_TEST");
    return t && *t;
}();

// ---- guest side ------------------------------------------------------------------------------
// Hours since 2000-01-01 of an OSCalendarTime (mon 0-11).
long hourKey(int year, int mon, int mday, int hour) {
    // days from civil (Howard Hinnant), month 1-12
    int y = year, m = mon + 1;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = unsigned((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + mday - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = long(era) * 146097 + long(doe) - 730425;  // 2000-03-01 based -> 2000-01-01
    return days * 24 + hour;
}
long timeKey(u32 t) {
    return hourKey(int(rd32(t + 0x14)), int(rd32(t + 0x10)), int(rd32(t + 0xC)), int(rd32(t + 8)));
}

u32 gameClock() {
    const u32 f = VA(0x8016D40C);
    const u32 lis = rd32(f), addi = rd32(f + 4);
    return ((lis & 0xFFFF) << 16) + u32(int32_t(int16_t(addi & 0xFFFF)));
}

std::map<long, int> sLatched[2];  // per kind (town, city): hour key -> state (guest thread only)
long sActiveSince[2] = {-1, -1};

// True and the conditions when real data may be used now.
bool activeConditions(Conditions& w) {
    if (fakeName()) {
        static const Conditions kFake = fakeConditions();
        w = kFake;
        return true;
    }
    if (sTestRun) startWorker();
    std::lock_guard lk(sMu);
    if (!sEnabled) return false;
    startWorker();
    if (!sCond.valid || !sPlace.set || unixNow() - sCond.fetched > 3 * 3600) return false;
    if (std::fabs(sCond.lat - sPlace.lat) > 0.05 || std::fabs(sCond.lon - sPlace.lon) > 0.05) return false;
    w = sCond;
    return true;
}

void hourHook(Ctx& c, const char* name, int kind) {
    const u32 t = c.r[3];
    const long key = timeKey(t);
    callOriginal(c, name);
    const int orig = int(c.r[3]);
    auto& latched = sLatched[kind];
    if (auto it = latched.find(key); it != latched.end()) {
        c.r[3] = u32(it->second);
        return;
    }
    Conditions w;
    if (!activeConditions(w)) {
        sActiveSince[kind] = -1;
        return;
    }
    const u32 clk = gameClock();
    const long now = timeKey(clk);
    const int minute = int(rd32(clk + 4));
    for (auto it = latched.begin(); it != latched.end();) it = it->first < now - 2 ? latched.erase(it) : std::next(it);
    const bool snowSeason = guestCall(c, VA(0x801C9FC4)) != 0;
    const int real = mapState(w, snowSeason);
    int state = orig;
    if (fakeName()) {
        state = real;  // test aid: at once, every hour asked
    } else if (key == now) {
        if (sActiveSince[kind] < 0) sActiveSince[kind] = now;
        state = sActiveSince[kind] == now && minute > 0 ? orig : real;  // the arrival hour keeps the game's own
        latched[key] = state;
    } else if (key == now + 1) {
        state = real;
        if (minute >= 50) latched[key] = state;
    }
    c.r[3] = u32(state);
    if (sLog && state != orig) {
        // Once per (town/city, hour asked, answer).
        static std::map<long, int> sLogged[2];
        if (auto [it, fresh] = sLogged[kind].emplace(key, state); fresh || it->second != state) {
            it->second = state;
            log("[weather] %s hour %ld (%s): game %d (%s) -> %d (%s), code %d, snow season %d, frame %u",
                kind ? "city" : "town", key - now, key == now ? "now" : "next", orig, orig < 7 ? kStateName[orig] : "?",
                state, state < 7 ? kStateName[state] : "?", w.code, snowSeason ? 1 : 0, game::guestFrames());
        }
    }
}

}  // namespace

bool drawWeatherMenu() {
    bool dirty = false;
    std::lock_guard lk(sMu);
    bool on = sEnabled;
    if (recomp::menu::Checkbox("Real-time weather", &on)) {
        sEnabled = on;
        dirty = true;
        if (on) sRefetch = true;
    }
    ImGui::SetItemTooltip(
        "The town's weather follows the current weather of a real place (clear, cloudy, rain, snow), within\n"
        "what the game can show in the current season: snow only in the game's snow season (rain otherwise),\n"
        "rain in the snow season falls as snow. Changes take effect at the next full hour, never mid-hour.\n"
        "Checked every 30 minutes; with no data younger than 3 hours (offline) the game's own weather runs.\n"
        "While this is on, the place's coordinates (rounded to 0.1 degrees, about 10 km) and your IP address\n"
        "are sent to Open-Meteo.com; nothing is sent while it is off.\n"
        "Weather data by Open-Meteo.com (https://open-meteo.com, CC BY 4.0).");
    if (fakeName()) ImGui::TextDisabled("(CF_WEATHER_FAKE=%s overrides this for the current run)", fakeName());
    if (!on) {
        ImGui::TextDisabled("Off: nothing is sent anywhere.");
        return dirty;
    }
    ImGui::Indent();
    ImGui::TextWrapped("The coordinates below are sent to Open-Meteo.com only while this option is on.");
    static char sQuery[128] = "";
    static char sCc[3] = "";
    if (sCountry.size() <= 2 && sCc[0] == 0 && !sCountry.empty()) std::snprintf(sCc, sizeof(sCc), "%s", sCountry.c_str());
    ImGui::SetNextItemWidth(220 * ImGui::GetFontSize() / 13.0f);
    // Test aid CF_WEATHER_SEARCH=<text>: types the text into the box and presses Search once.
    static const char* sTestSearch = std::getenv("CF_WEATHER_SEARCH");
    bool testPress = false;
    if (sTestSearch && *sTestSearch) {
        std::snprintf(sQuery, sizeof(sQuery), "%s", sTestSearch);
        sTestSearch = nullptr;
        testPress = true;
    }
    const bool enter = ImGui::InputTextWithHint("##weather_place", "Town or city", sQuery, sizeof(sQuery),
                                                ImGuiInputTextFlags_EnterReturnsTrue) ||
                       testPress;
    ImGui::SetItemTooltip("Town or city name (English or local Latin spelling); postal codes work in some countries.\n"
                          "Sent to Open-Meteo's place search only when you press Search.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(32 * ImGui::GetFontSize() / 13.0f);
    if (ImGui::InputTextWithHint("##weather_cc", "CC", sCc, sizeof(sCc), ImGuiInputTextFlags_CharsUppercase)) {
        sCountry = sCc;
        dirty = true;
    }
    ImGui::SetItemTooltip("Optional two-letter country code (e.g. DE, US, AU) to narrow the search.");
    ImGui::SameLine();
    if ((recomp::menu::Button(sSearching ? "Searching...###weather_search" : "Search###weather_search") || enter) &&
        !sSearching && std::strlen(sQuery) >= 2) {
        sSearchQuery = sQuery;
        sSearching = true;
        sSearchStatus.clear();
        startWorker();
    }
    if (!sSearchStatus.empty()) ImGui::TextWrapped("%s", sSearchStatus.c_str());
    for (size_t i = 0; i < sHits.size(); i++) {
        char label[320];
        std::snprintf(label, sizeof(label), "%s  (%.1f, %.1f)##weather_hit%zu", sHits[i].label.c_str(), sHits[i].lat,
                      sHits[i].lon, i);
        if (ImGui::Selectable(label, sPlace.set && sPlace.name == sHits[i].label)) {
            sPlace = {sHits[i].label, sHits[i].lat, sHits[i].lon, true};
            sRefetch = true;
            sHits.clear();
            dirty = true;
            startWorker();
            break;
        }
    }
    if (sPlace.set) ImGui::Text("Place: %s (%.1f, %.1f)", sPlace.name.c_str(), sPlace.lat, sPlace.lon);
    else ImGui::TextDisabled("No place chosen yet: search for your town or city.");
    if (sCond.valid && sPlace.set) {
        const long long age = (unixNow() - sCond.fetched) / 60;
        ImGui::Text("Now there: %s, %.0f C, cloud cover %d%% (updated %lld min ago)%s", describe(sCond.code), sCond.temp,
                    sCond.cloud, age, age > 180 ? ": too old, the game's own weather runs" : "");
    }
    if (!sStatus.empty()) ImGui::TextWrapped("Last update failed: %s (the game's own weather runs while there is no data)", sStatus.c_str());
    ImGui::TextDisabled("Weather data by Open-Meteo.com");
    ImGui::SetItemTooltip("https://open-meteo.com (CC BY 4.0). Place search data from GeoNames (CC BY 4.0).");
    ImGui::Unindent();
    return dirty;
}

}  // namespace recomp::calendar

namespace recomp {
namespace {
void hTown(Ctx& c, const char* n) { calendar::hourHook(c, n, 0); }
void hCity(Ctx& c, const char* n) { calendar::hourHook(c, n, 1); }
}  // namespace
HLE(fn_801C9E48) { hTown(c, "fn_801C9E48"); }  // hooks.txt "all"
HLE(fn_801C9EC0) { hCity(c, "fn_801C9EC0"); }
}  // namespace recomp
