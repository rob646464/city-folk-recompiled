// F1 > Visuals > Enhanced world (docs/RENDERING.md "Enhanced world"; Aurora 0350 world effects,
// include/aurora/world_fx.h): water reflections, wet ground and rain ripples, snow sparkle, haze on
// grey days, seasonal particles, a colour grade and a tilt-shift "diorama" blur. Off by default;
// off, nothing here writes to the GX stream (the game's frames are byte-identical).
//
// How the game draws a frame (USA rev 1, traced): nw4r::g3d::ScnRoot::DrawOpa / DrawXlu of the main
// scene (town, sky, characters; lr 0x80E6C7C0 / C4 in the scene REL), nw4r::ef::EffectSystem::Draw
// (particles, rain), the HUD layouts (nw4r::lyt::Layout::Draw), a second ScnRoot pass for 3D drawn
// over the HUD, more layouts, the pointer, GXCopyDisp. So:
//  - every g3d material draw (nw4r::g3d::Draw1Mat1ShpDirectly, r3 = ResMat -> ResMatData, name at
//    data + s32 [data + 8]) of the frame's FIRST ScnRoot pass gets a surface kind
//    (GXSetSurfaceAurora): water for the river / pond / sea materials, ground for walkable ground,
//    world for everything else (the effects' camera);
//  - the world effects end (GXWorldFxEndAurora) goes in before the first layout after that pass
//    (or the second ScnRoot pass), with the main camera loaded into the application XF memory
//    (the ScnRoot's current camera: index u8 [root + 0xF4], CameraData at root + 0xF8 + 0x10C n,
//    whose first member is the world -> view matrix; the camera target at + 0x8C). Both XF loads
//    are noted for frame interpolation like model matrices, so the in-between frames get the
//    in-between camera.
// Weather (dWeather_c, per-frame fn_801C92F0: current hourly state at +0x5884, next at +0x5888;
// docs/CALENDAR.md), season (the game clock + the hemisphere option) and the hour drive the
// amounts in the configuration.
//
// Test aids: CF_WFX=1 (on for one run, every sub-option at its default), CF_WFX_SET="key=value;..."
// (any [enhanced_world] key), CF_WFX_MAT_LOG=1 (each material name once, with its surface kind),
// CF_WFX_LOG=1 (camera / weather / amounts every 120 frames), CF_WFX_FREEZE=1 (effect time fixed:
// deterministic shots), CF_WFX_AB=1 (effects only on odd dump frames: consecutive shots compare
// with / without), AURORA_WORLD_FX_LOG=1 (Aurora's scene-end line).
#include "game/cf_calendar.h"

#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"

#include <aurora/world_fx.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>

namespace recomp {
namespace world {

// World units per metre-ish (particle sizes, ripple cells, puddle noise).
constexpr float kWorldScale = 20.0f;
void endWorld();

// ---- settings ([enhanced_world]) --------------------------------------------------------------
struct Settings {
    bool enabled = false;
    bool reflections = true;
    float reflectionStrength = 0.8f;  // 0..1.5
    int reflectionQuality = 1;        // 0 low, 1 medium, 2 high
    float rippleDistortion = 1.0f;    // 0..2
    bool wetGround = true;
    bool rainRipples = true;
    bool snowSparkle = true;
    bool haze = true;
    float hazeStrength = 1.0f;  // 0..2
    bool particles = true;
    float particleDensity = 1.0f;  // 0..2
    bool tiltShift = false;
    float tiltStrength = 0.6f;  // 0..1
    float tiltBand = 0.16f;     // 0.05..0.5
    int grade = 0;              // 0 original, 1 New Leaf warmth, 2 soft film, 3 vivid
    float gradeStrength = 1.0f;
};

namespace {
std::mutex sMu;
Settings sSet;

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

bool parseBool(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }
void setKey(Settings& s, const std::string& key, const std::string& v) {
    const float f = float(std::atof(v.c_str()));
    const int i = std::atoi(v.c_str());
    if (key == "enabled") s.enabled = parseBool(v);
    else if (key == "reflections") s.reflections = parseBool(v);
    else if (key == "reflection_strength") s.reflectionStrength = std::clamp(f, 0.0f, 1.5f);
    else if (key == "reflection_quality") s.reflectionQuality = std::clamp(i, 0, 2);
    else if (key == "ripple_distortion") s.rippleDistortion = std::clamp(f, 0.0f, 2.0f);
    else if (key == "wet_ground") s.wetGround = parseBool(v);
    else if (key == "rain_ripples") s.rainRipples = parseBool(v);
    else if (key == "snow_sparkle") s.snowSparkle = parseBool(v);
    else if (key == "haze") s.haze = parseBool(v);
    else if (key == "haze_strength") s.hazeStrength = std::clamp(f, 0.0f, 2.0f);
    else if (key == "particles") s.particles = parseBool(v);
    else if (key == "particle_density") s.particleDensity = std::clamp(f, 0.0f, 2.0f);
    else if (key == "tilt_shift") s.tiltShift = parseBool(v);
    else if (key == "tilt_strength") s.tiltStrength = std::clamp(f, 0.0f, 1.0f);
    else if (key == "tilt_band") s.tiltBand = std::clamp(f, 0.05f, 0.5f);
    else if (key == "grade") s.grade = std::clamp(i, 0, 3);
    else if (key == "grade_strength") s.gradeStrength = std::clamp(f, 0.0f, 1.0f);
}

void resetConfig() {
    std::lock_guard lk(sMu);
    sSet = Settings{};
}
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "enhanced_world") return;
    std::lock_guard lk(sMu);
    setKey(sSet, key, value);
}
void saveConfig(std::ostream& out) {
    std::lock_guard lk(sMu);
    const Settings& s = sSet;
    out << "\n[enhanced_world]\n"
        << "enabled=" << (s.enabled ? 1 : 0) << "\nreflections=" << (s.reflections ? 1 : 0)
        << "\nreflection_strength=" << s.reflectionStrength << "\nreflection_quality=" << s.reflectionQuality
        << "\nripple_distortion=" << s.rippleDistortion << "\nwet_ground=" << (s.wetGround ? 1 : 0)
        << "\nrain_ripples=" << (s.rainRipples ? 1 : 0) << "\nsnow_sparkle=" << (s.snowSparkle ? 1 : 0)
        << "\nhaze=" << (s.haze ? 1 : 0) << "\nhaze_strength=" << s.hazeStrength << "\nparticles=" << (s.particles ? 1 : 0)
        << "\nparticle_density=" << s.particleDensity << "\ntilt_shift=" << (s.tiltShift ? 1 : 0)
        << "\ntilt_strength=" << s.tiltStrength << "\ntilt_band=" << s.tiltBand << "\ngrade=" << s.grade
        << "\ngrade_strength=" << s.gradeStrength << "\n";
}
[[maybe_unused]] const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

// The settings in effect (test aids applied on top for this run).
Settings current() {
    Settings s;
    {
        std::lock_guard lk(sMu);
        s = sSet;
    }
    static const bool sForce = envFlag("CF_WFX");
    if (sForce) s.enabled = true;
    if (const char* e = std::getenv("CF_WFX_SET")) {
        std::string all = e;
        size_t pos = 0;
        while (pos < all.size()) {
            const size_t semi = all.find(';', pos);
            const std::string kv = all.substr(pos, semi - pos);
            if (const size_t eq = kv.find('='); eq != std::string::npos) setKey(s, kv.substr(0, eq), kv.substr(eq + 1));
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
    }
    return s;
}

const bool sMatLog = envFlag("CF_WFX_MAT_LOG");
const bool sLog = envFlag("CF_WFX_LOG");

// ---- frame structure ------------------------------------------------------------------------
u32 sFrame = ~0u;
int sPass = 0;          // ScnRoot DrawOpa calls this frame
bool sMain = false;     // inside the first ScnRoot pass (DrawOpa or DrawXlu)
bool sPendingEnd = false;
bool sOn = false;       // effects on this frame (sampled at the first pass)
u32 sMainRoot = 0;      // the main pass's ScnRoot
u8 sKind = 0xFF;        // surface kind last emitted
u32 sWaterDraws = 0, sGroundDraws = 0;  // this frame's main pass
u32 sLastOutdoor = 0;   // guest frame of the last frame with ground or water

// ---- weather (dWeather_c) --------------------------------------------------------------------
std::atomic<u32> sWeatherObj{0};
std::atomic<u32> sWeatherFrame{0};

void weatherHook(Ctx& c, const char* name) {
    sWeatherObj.store(c.r[3], std::memory_order_relaxed);
    sWeatherFrame.store(game::guestFrames(), std::memory_order_relaxed);
    callOriginal(c, name);
}

int weatherState() {
    const u32 obj = sWeatherObj.load(std::memory_order_relaxed);
    // (only while the weather object runs: it is not there indoors / on the title screen)
    if (!obj || game::guestFrames() - sWeatherFrame.load(std::memory_order_relaxed) > 30) return -1;
    const u32 s = rd32(obj + 0x5884);
    return s <= 6 ? int(s) : -1;
}

// ---- materials -------------------------------------------------------------------------------
std::string matName(u32 resMatPtr) {
    const u32 mat = rd32(resMatPtr);
    if (mat < 0x80000000u || mat >= 0x94000000u) return {};
    const u32 s = mat + rd32(mat + 8);
    if (s < 0x80000000u || s >= 0x94000000u) return {};
    char buf[49] = {};
    for (int i = 0; i < 48; i++) {
        const char ch = char(rd8(s + u32(i)));
        if (!ch) break;
        buf[i] = (ch >= 0x20 && ch < 0x7F) ? ch : '?';
    }
    return buf;
}

u8 kindOf(const std::string& n) {
    // Water: the sea, rivers and ponds (their pattern layer). Not: the river's foam edge, the sea's
    // white surf, wave shadows, the additive highlight layer, waterfalls and their ripples / spray.
    if (n == "m_ocean" || n == "m_river" || n == "m_river2_pattern" || n == "m_pond" || n == "m_water")
        return AURORA_SURFACE_WATER;
    // Walkable ground: grass (worn to soil by the grass wear), soil, sand, stone paths and plazas.
    if (n == "m_grass" || n == "m_soil" || n == "m_soil_sand" || n == "m_soil_stone" || n == "m_stone" ||
        n == "m_Es_G_soil" || n == "m_outgrass" || n == "m_buildsite" || n.rfind("m_plaza", 0) == 0 ||
        n.rfind("m_road", 0) == 0 || n.rfind("m_ground", 0) == 0)
        return AURORA_SURFACE_GROUND;
    return AURORA_SURFACE_WORLD;
}

struct MatEntry {
    u32 nameWord = 0;
    u8 kind = 0;
};
std::unordered_map<u32, MatEntry> sMatCache;  // ResMatData -> kind (re-checked by the name's first word)

u8 kindFor(u32 resMatPtr) {
    const u32 mat = rd32(resMatPtr);
    if (mat < 0x80000000u || mat >= 0x94000000u) return AURORA_SURFACE_WORLD;
    const u32 s = mat + rd32(mat + 8);
    const u32 word = (s >= 0x80000000u && s < 0x94000000u) ? rd32(s) : 0;
    if (auto it = sMatCache.find(mat); it != sMatCache.end() && it->second.nameWord == word) return it->second.kind;
    const std::string n = matName(resMatPtr);
    const u8 k = kindOf(n);
    if (sMatCache.size() > 8192) sMatCache.clear();
    sMatCache[mat] = {word, k};
    if (sMatLog) {
        static std::set<std::string> sSeen;
        if (sSeen.insert(n).second)
            log("[wfx] material %s -> %s (frame %u)", n.c_str(),
                k == AURORA_SURFACE_WATER ? "water" : k == AURORA_SURFACE_GROUND ? "ground" : "world", game::guestFrames());
    }
    return k;
}

void emitKind(u8 k) {
    if (k == sKind) return;
    GXSetSurfaceAurora(k);
    sKind = k;
}

// ---- camera ----------------------------------------------------------------------------------
float rdf(u32 a) { return std::bit_cast<float>(rd32(a)); }

bool mainCamera(float view[12], float target[3]) {
    const u32 root = sMainRoot;
    if (root < 0x80000000u || root >= 0x94000000u) return false;
    const u32 idx = rd8(root + 0xF4);
    if (idx >= 32) return false;
    const u32 data = root + 0xF8 + idx * 0x10C;
    for (int i = 0; i < 12; i++) view[i] = rdf(data + 4 * u32(i));
    for (int i = 0; i < 3; i++) target[i] = rdf(data + 0x8C + 4 * u32(i));
    const float r0 = view[0] * view[0] + view[1] * view[1] + view[2] * view[2];
    if (!(r0 > 0.5f && r0 < 2.0f)) return false;
    for (int i = 0; i < 12; i++)
        if (!std::isfinite(view[i])) return false;
    for (int i = 0; i < 3; i++)
        if (!std::isfinite(target[i])) return false;
    return true;
}

// ---- amounts from weather, season and hour ---------------------------------------------------
struct Rgb {
    float r, g, b;
};
Rgb mixc(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
float sstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Sky colours by hour (0..24, fractional): night, dawn, day, dusk.
void skyAt(float h, Rgb& zen, Rgb& hor, float& night) {
    const Rgb nZ{0.03f, 0.05f, 0.13f}, nH{0.09f, 0.11f, 0.22f};
    const Rgb dawnZ{0.42f, 0.52f, 0.78f}, dawnH{0.98f, 0.76f, 0.58f};
    const Rgb dayZ{0.33f, 0.56f, 0.92f}, dayH{0.72f, 0.84f, 0.96f};
    const Rgb duskZ{0.34f, 0.36f, 0.62f}, duskH{0.98f, 0.60f, 0.38f};
    if (h < 4.5f || h >= 20.5f) {
        zen = nZ, hor = nH, night = 1.0f;
    } else if (h < 6.0f) {
        const float t = sstep(4.5f, 6.0f, h);
        zen = mixc(nZ, dawnZ, t), hor = mixc(nH, dawnH, t), night = 1.0f - t;
    } else if (h < 8.0f) {
        const float t = sstep(6.0f, 8.0f, h);
        zen = mixc(dawnZ, dayZ, t), hor = mixc(dawnH, dayH, t), night = 0.0f;
    } else if (h < 16.5f) {
        zen = dayZ, hor = dayH, night = 0.0f;
    } else if (h < 18.5f) {
        const float t = sstep(16.5f, 18.5f, h);
        zen = mixc(dayZ, duskZ, t), hor = mixc(dayH, duskH, t), night = 0.0f;
    } else {
        const float t = sstep(18.5f, 20.5f, h);
        zen = mixc(duskZ, nZ, t), hor = mixc(duskH, nH, t), night = t;
    }
}

int monthDay(int mon, int day) { return mon * 100 + day; }  // mon 0-11

struct Amounts {
    float wetness = 0, rain = 0, sparkle = 0, haze = 0, moon = 0;
    Rgb zen{}, hor{}, hazeCol{}, ambient{1, 1, 1};
    int particles = 0;
    float hour = 12;
    int state = -1;
    int md = 0;
};

Amounts sAmt;
float sWet = 0.0f, sRain = 0.0f;
std::chrono::steady_clock::time_point sLastTick{};
bool sTicked = false;

void updateAmounts(bool outdoor) {
    const auto now = std::chrono::steady_clock::now();
    float dt = sTicked ? std::chrono::duration<float>(now - sLastTick).count() : 0.0f;
    dt = std::clamp(dt, 0.0f, 0.25f);
    const bool arrived = sTicked && game::guestFrames() - sLastOutdoor > 120;  // first outdoor frame after a while
    sLastTick = now;
    sTicked = true;

    Amounts a;
    const u32 clk = [] {
        const u32 f = calendar::versionAddr(0x8016D40C);
        const u32 lis = rd32(f), addi = rd32(f + 4);
        return ((lis & 0xFFFF) << 16) + u32(int32_t(int16_t(addi & 0xFFFF)));
    }();
    const int minute = int(rd32(clk + 4)), hour = int(rd32(clk + 8));
    int mon = int(rd32(clk + 0x10)), day = int(rd32(clk + 0xC));
    if (mon < 0 || mon > 11) mon = 0;
    calendar::seasonDate(mon, day);
    a.md = monthDay(mon, day);
    a.hour = std::clamp(float(hour) + float(minute) / 60.0f, 0.0f, 24.0f);
    a.state = weatherState();
    const int st = a.state < 0 ? 0 : a.state;
    const bool raining = st == 3 || st == 4;
    const bool snowing = st == 5 || st == 6;

    // wetness: up over ~40 s of rain, dries over ~4 minutes after; already wet on arrival in rain
    const float rainTarget = st == 4 ? 1.0f : st == 3 ? 0.6f : 0.0f;
    sRain += (rainTarget - sRain) * std::min(dt * 0.8f, 1.0f);
    if (arrived && raining) sWet = 1.0f, sRain = rainTarget;
    if (raining) sWet = std::min(1.0f, sWet + dt / 40.0f);
    else sWet = std::max(0.0f, sWet - dt / 240.0f);
    a.wetness = sWet;
    a.rain = sRain;

    float night = 0;
    skyAt(a.hour, a.zen, a.hor, night);
    const Rgb grey{0.62f, 0.64f, 0.68f}, darkGrey{0.40f, 0.42f, 0.47f};
    const float overcast = st == 2 ? 0.6f : (raining || snowing) ? 1.0f : st == 1 ? 0.2f : 0.0f;
    const Rgb og = st == 4 || st == 6 ? darkGrey : grey;
    const Rgb ogN = mixc(og, Rgb{0.06f, 0.07f, 0.1f}, night);
    a.zen = mixc(a.zen, ogN, overcast * 0.85f);
    a.hor = mixc(a.hor, ogN, overcast * 0.7f);
    a.moon = night * (1.0f - overcast);
    // snow sparkle: snow cover (Dec 11 - Feb 24, as the game's terms 19-21) in daylight with the sun out
    const bool snowCover = a.md >= monthDay(11, 11) || a.md <= monthDay(1, 24);
    const float sun = sstep(6.5f, 8.0f, a.hour) * (1.0f - sstep(16.0f, 17.5f, a.hour));
    a.sparkle = snowCover ? sun * (st == 0 ? 1.0f : st == 1 ? 0.6f : st == 2 ? 0.2f : 0.0f) : 0.0f;
    // haze on grey days (and a trace at dawn / dusk)
    const float hz = st == 2 ? 0.22f : st == 3 ? 0.35f : st == 4 ? 0.5f : st == 5 ? 0.4f : st == 6 ? 0.55f : 0.0f;
    const float twilight = (sstep(4.5f, 6.0f, a.hour) * (1.0f - sstep(6.5f, 8.0f, a.hour))) +
                           (sstep(16.5f, 18.0f, a.hour) * (1.0f - sstep(19.0f, 20.5f, a.hour)));
    a.haze = std::max(hz, 0.08f * twilight);
    a.hazeCol = mixc(a.hor, Rgb{0.72f, 0.74f, 0.78f}, overcast * 0.6f);
    a.hazeCol = mixc(a.hazeCol, Rgb{0.08f, 0.09f, 0.13f}, night * 0.7f);
    // particles: cherry blossoms (Apr 1-8: the game's terms 1-2), falling leaves (Oct 17 - Nov 25:
    // fall colours, terms 9-16), snowflakes (Nov 26 - Feb 24: the snow season, terms 17-21)
    if (a.md >= monthDay(3, 1) && a.md <= monthDay(3, 8)) a.particles = AURORA_WORLD_FX_PARTICLES_PETALS;
    else if (a.md >= monthDay(9, 17) && a.md <= monthDay(10, 25)) a.particles = AURORA_WORLD_FX_PARTICLES_LEAVES;
    else if (a.md >= monthDay(10, 26) || a.md <= monthDay(1, 24)) a.particles = AURORA_WORLD_FX_PARTICLES_SNOW;
    const float light = 1.0f - 0.62f * night;
    a.ambient = mixc(Rgb{light, light, light}, Rgb{0.8f, 0.82f, 0.9f}, overcast * 0.4f);
    if (night > 0.0f) a.ambient = mixc(a.ambient, Rgb{0.30f, 0.34f, 0.48f}, night * 0.6f);
    if (!outdoor) a.particles = 0;
    sAmt = a;
}

void applyConfig(const Settings& s, bool outdoor, bool on) {
    AuroraWorldFxConfig c{};
    c.enabled = on;
    if (on) {
        const Amounts& a = sAmt;
        c.reflections = s.reflections;
        c.reflectionStrength = s.reflectionStrength;
        c.reflectionQuality = u32(s.reflectionQuality);
        c.rippleDistortion = s.rippleDistortion;
        c.wetGround = s.wetGround && outdoor;
        c.wetness = a.wetness;
        c.rainRipples = s.rainRipples && outdoor;
        c.rain = a.rain;
        c.snowSparkle = s.snowSparkle && outdoor;
        c.sparkle = a.sparkle;
        c.haze = s.haze && outdoor;
        c.hazeDensity = std::min(a.haze * s.hazeStrength, 1.0f);
        c.hazeStart = 1.15f;
        c.hazeColor[0] = a.hazeCol.r, c.hazeColor[1] = a.hazeCol.g, c.hazeColor[2] = a.hazeCol.b;
        c.skyZenith[0] = a.zen.r, c.skyZenith[1] = a.zen.g, c.skyZenith[2] = a.zen.b;
        c.skyHorizon[0] = a.hor.r, c.skyHorizon[1] = a.hor.g, c.skyHorizon[2] = a.hor.b;
        c.moon = a.moon;
        c.moonDir[0] = 0.0f, c.moonDir[1] = 0.45f, c.moonDir[2] = -0.89f;
        c.ambient[0] = a.ambient.r, c.ambient[1] = a.ambient.g, c.ambient[2] = a.ambient.b;
        c.tiltShift = s.tiltShift;
        c.tiltStrength = s.tiltStrength;
        c.tiltBand = s.tiltBand;
        c.grade = u32(s.grade);
        c.gradeStrength = s.gradeStrength;
        c.particles = s.particles ? u32(a.particles) : 0u;
        c.particleDensity = s.particleDensity;
        c.wind[0] = 0.35f * kWorldScale, c.wind[1] = 0.12f * kWorldScale;
        c.worldScale = kWorldScale;
        c.freezeTime = envFlag("CF_WFX_FREEZE");
    }
    aurora_world_fx_set_config(&c);
}

}  // namespace

// cf_tags.cpp: ScnRoot::DrawOpa / DrawXlu of `root` (begin = before the original).
void onScnRoot(u32 root, bool xlu, bool begin) {
    const u32 f = game::guestFrames();
    if (f != sFrame) {
        sFrame = f;
        sPass = 0;
        sPendingEnd = false;
        sKind = 0xFF;
        sWaterDraws = sGroundDraws = 0;
        const Settings s = current();
        static const bool sAb = envFlag("CF_WFX_AB");
        static bool sApplied = false;  // the effects' configuration was last sent on
        sOn = s.enabled && (!sAb || (f & 1u));
        if (!s.enabled && sApplied) {
            applyConfig(s, false, false);
            sApplied = false;
        }
        if (sOn) sApplied = true;
    }
    if (!sOn) return;
    if (begin && !xlu && ++sPass == 2 && sPendingEnd) endWorld();  // a second pass (3D over the HUD)
    const bool main = sPass == 1;
    if (begin && main && !xlu) sMainRoot = root;
    sMain = begin && main;
    if (!begin && main) {
        emitKind(AURORA_SURFACE_NONE);
        if (xlu) sPendingEnd = true;
    }
}

void endWorld() {
    if (!sPendingEnd) return;
    sPendingEnd = false;
    const Settings s = current();
    const bool outdoor = sWaterDraws + sGroundDraws > 0;
    if (outdoor) sLastOutdoor = game::guestFrames();
    updateAmounts(outdoor);
    applyConfig(s, outdoor, true);
    float view[12], target[3];
    if (!mainCamera(view, target)) {
        if (sLog) log("[wfx] no main camera (root %08X)", sMainRoot);
        GXWorldFxEndAurora();
        return;
    }
    float vf[12];
    std::memcpy(vf, view, sizeof(vf));
    for (int r = 0; r < 3; r++) vf[r * 4 + 3] = view[r * 4] * target[0] + view[r * 4 + 1] * target[1] + view[r * 4 + 2] * target[2] + view[r * 4 + 3];
    {
        interp::TagScope tag(interp::TagCat::Object, 0xCF57F000u, 0x57F0u, 0);
        u32 at = interp::mark();
        GXLoadAppXfAurora(AURORA_WORLD_FX_XF_VIEW, view, 12);
        interp::note(at, interp::Kind::PosMtx, 0xCF57F000u, AURORA_WORLD_FX_XF_VIEW, 12);
        at = interp::mark();
        GXLoadAppXfAurora(AURORA_WORLD_FX_XF_FOCUS, vf, 12);
        interp::note(at, interp::Kind::PosMtx, 0xCF57F004u, AURORA_WORLD_FX_XF_FOCUS, 12);
    }
    GXWorldFxEndAurora();
    if (sLog && game::guestFrames() % 120 == 0) {
        const Amounts& a = sAmt;
        log("[wfx] f%u root %08X water %u ground %u outdoor %d | view %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f "
            "target %.1f %.1f %.1f (view %.1f %.1f %.1f) | weather %d hour %.2f date %d wet %.2f rain %.2f sparkle %.2f haze %.2f "
            "moon %.2f particles %d",
            game::guestFrames(), sMainRoot, sWaterDraws, sGroundDraws, int(outdoor), view[0], view[1], view[2], view[3], view[4],
            view[5], view[6], view[7], view[8], view[9], view[10], view[11], target[0], target[1], target[2], vf[3], vf[7], vf[11],
            a.state, a.hour, a.md, a.wetness, a.rain, a.sparkle, a.haze, a.moon, a.particles);
    }
}

// F1 > Visuals > Enhanced world.
bool drawMenu() {
    bool dirty = false;
    std::lock_guard lk(sMu);
    Settings& s = sSet;
    dirty |= menu::Checkbox("Enhanced world", &s.enabled);
    ImGui::SetItemTooltip(
        "Effects layered on the finished 3D picture, before the HUD: reflections on rivers, ponds and the\n"
        "sea, wet ground and rain ripples while it rains, sparkling snow in sunlight, a light haze on grey\n"
        "days, seasonal particles, and optional colour grades / tilt-shift. Off: the game's own picture,\n"
        "unchanged. Costs some GPU time with water or rain on screen (see each option).");
    if (envFlag("CF_WFX")) ImGui::TextDisabled("(CF_WFX turns this on for the current run)");
    ImGui::BeginDisabled(!s.enabled);
    ImGui::SeparatorText("Water");
    dirty |= menu::Checkbox("Water reflections", &s.reflections);
    ImGui::SetItemTooltip(
        "Rivers, ponds and the sea reflect what is around them: the bank, trees, houses, characters (traced\n"
        "through the picture) and the sky and moon where the reflection leaves the picture. The game's own\n"
        "water pattern bends the reflection. Cost: one pass over the water on screen (about 0.1-0.4 ms at\n"
        "1080p, 0.4-1.2 ms at 4K with a lot of water).");
    ImGui::BeginDisabled(!s.reflections);
    dirty |= menu::SliderFloat("Reflection strength", &s.reflectionStrength, 0.0f, 1.5f, "%.2f");
    static const char* kQ[] = {"Low (16 steps)", "Medium (24 steps)", "High (40 steps)"};
    dirty |= ImGui::Combo("Reflection quality", &s.reflectionQuality, kQ, 3);
    ImGui::SetItemTooltip("Steps of the reflection trace: more finds thin things (fences, trunks) more reliably.");
    dirty |= menu::SliderFloat("Ripple distortion", &s.rippleDistortion, 0.0f, 2.0f, "%.2f");
    ImGui::SetItemTooltip("How much the game's own water ripples bend the reflection (0 = a flat mirror).");
    ImGui::EndDisabled();
    ImGui::SeparatorText("Weather");
    dirty |= menu::Checkbox("Wet ground in rain", &s.wetGround);
    ImGui::SetItemTooltip(
        "While it rains, paths and bare ground darken and get puddles that reflect lamps, houses and the\n"
        "sky; grass stays mostly dry-looking. The ground dries over a few minutes after the rain stops.\n"
        "Cost: with rain only, about 0.2-0.6 ms at 1080p.");
    dirty |= menu::Checkbox("Rain ripples", &s.rainRipples);
    ImGui::SetItemTooltip("Rings from raindrops on rivers, ponds, the sea and puddles while it rains.");
    dirty |= menu::Checkbox("Snow sparkle", &s.snowSparkle);
    ImGui::SetItemTooltip("Tiny glints on snow-covered ground in sunlight (snow season, daytime, sun out).");
    dirty |= menu::Checkbox("Haze on grey days", &s.haze);
    ImGui::SetItemTooltip("A subtle depth-based haze towards the horizon on cloudy, rainy and snowy days (and a\n"
                          "trace at dawn and dusk). Nothing on clear days.");
    ImGui::BeginDisabled(!s.haze);
    dirty |= menu::SliderFloat("Haze strength", &s.hazeStrength, 0.0f, 2.0f, "%.2f");
    ImGui::EndDisabled();
    ImGui::SeparatorText("Seasons");
    dirty |= menu::Checkbox("Seasonal particles", &s.particles);
    ImGui::SetItemTooltip(
        "Drawn on top of the world (not game objects): cherry blossom petals during the cherry blossom days\n"
        "(April 1-8), falling leaves in late autumn (October 17 - November 25), drifting snowflakes in the\n"
        "snow season (November 26 - February 24); the hemisphere option moves them by six months. Outdoors\n"
        "only. Smooth at any frame rate.");
    ImGui::BeginDisabled(!s.particles);
    dirty |= menu::SliderFloat("Particle density", &s.particleDensity, 0.0f, 2.0f, "%.2f");
    ImGui::EndDisabled();
    ImGui::SeparatorText("Look");
    static const char* kGrades[] = {"Original", "New Leaf warmth", "Soft film", "Vivid"};
    dirty |= ImGui::Combo("Colour preset", &s.grade, kGrades, 4);
    ImGui::SetItemTooltip(
        "A colour grade of the 3D picture (the HUD keeps its colours). Original = no grade.\n"
        "New Leaf warmth: warmer light, lifted shadows, a little more colour. Soft film: lower contrast,\n"
        "muted colour, cool shadows and warm highlights. Vivid: stronger colour and contrast.");
    ImGui::BeginDisabled(s.grade == 0);
    dirty |= menu::SliderFloat("Preset strength", &s.gradeStrength, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    dirty |= menu::Checkbox("Tilt-shift (diorama)", &s.tiltShift);
    ImGui::SetItemTooltip(
        "Blurs the top and bottom of the 3D picture and keeps the player's row sharp, so the town looks\n"
        "like a miniature model (a just-for-fun look, off by default). Cost: two blur passes over the\n"
        "picture (about 0.2 ms at 1080p, 0.7 ms at 4K).");
    ImGui::BeginDisabled(!s.tiltShift);
    dirty |= menu::SliderFloat("Tilt-shift strength", &s.tiltStrength, 0.0f, 1.0f, "%.2f");
    dirty |= menu::SliderFloat("Sharp band", &s.tiltBand, 0.05f, 0.5f, "%.2f");
    ImGui::SetItemTooltip("Half height of the sharp band around the player, as a share of the picture height.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    return dirty;
}

}  // namespace world

HLE(Draw1Mat1ShpDirectly__Q24nw4r3g3dFQ34nw4r3g3d6ResMatQ34nw4r3g3d6ResShpPCQ34nw4r4math5MTX34PCQ34nw4r4math5MTX34UlPQ34nw4r3g3d16Draw1Mat1ShpSwapPQ44nw4r3g3d8G3DState8IndMtxOp) {
    if (world::sOn && world::sMain) {
        const u8 k = world::kindFor(c.r[3]);
        if (k == AURORA_SURFACE_WATER) world::sWaterDraws++;
        else if (k == AURORA_SURFACE_GROUND) world::sGroundDraws++;
        world::emitKind(k);
    }
    callOriginal(c, "Draw1Mat1ShpDirectly__Q24nw4r3g3dFQ34nw4r3g3d6ResMatQ34nw4r3g3d6ResShpPCQ34nw4r4math5MTX34PCQ34nw4r4math5MTX34UlPQ34nw4r3g3d16Draw1Mat1ShpSwapPQ44nw4r3g3d8G3DState8IndMtxOp");
}

// The first HUD layout after the main pass ends the world.
HLE(Draw__Q34nw4r3lyt6LayoutFRCQ34nw4r3lyt8DrawInfo) {
    if (world::sOn && world::sPendingEnd) world::endWorld();
    callOriginal(c, "Draw__Q34nw4r3lyt6LayoutFRCQ34nw4r3lyt8DrawInfo");
}

// dWeather_c every frame (USA / Europe / Japan / Korea).
HLE(fn_801C92F0) { world::weatherHook(c, "fn_801C92F0"); }
HLE(fn_801C8FE4) { world::weatherHook(c, "fn_801C8FE4"); }
HLE(fn_801C9090) { world::weatherHook(c, "fn_801C9090"); }
HLE(fn_801C90C8) { world::weatherHook(c, "fn_801C90C8"); }

}  // namespace recomp
