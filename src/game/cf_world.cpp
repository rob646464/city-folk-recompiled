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
//  - the world effects end (GXWorldFxEndAurora) goes in at the first layout root pane after that
//    pass (cf_hud.cpp, after enhanced lighting's scene end) or at the second ScnRoot pass, with the
//    main camera loaded into the application XF memory (the ScnRoot's current camera: index u8
//    [root + 0xF4], CameraData at root + 0xF8 + 0x10C n, whose first member is the world -> view
//    matrix), rotated so the world's up axis (+z in the town) is +y. The XF load is noted for frame
//    interpolation like a model matrix, so the in-between frames get the in-between camera.
//  - every Aurora write first closes the game's lazily ended immediate-mode draw (gx::endPendingDraw).
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
#include "game/cf_light.h"

#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/version.h"
#include "recomp/interp.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"

#include <aurora/world_fx.h>
#include <imgui.h>

#include <algorithm>
#include <array>
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

// World units per "metre" of the effects (particle sizes and their volume, ripple cells, puddle
// noise) while the port gives Aurora the effects' centre (0440, the town's log): Aurora 0442 then
// uses it instead of a scale from the centre's distance, which changed with the camera's height on
// slopes and steps (the leaf field slid and zoomed: branch leaves-slope). 14.7 = what the distance
// gave on flat ground in the town (centre 296 units away, projection p11 5.03: 2 x 296 / 5.03 / 8),
// so the look there is unchanged. Elsewhere (no centre given) Aurora keeps the distance scale.
constexpr float kWorldScale = 14.7f;
// Top of the particle volume above the effects' centre, effect metres (Aurora 0443; 19 = the old
// box, ~280 units: about the camera's height in the town). Looking at the sky (D-pad Up: the camera
// sinks to ~180 units and the log curves away under it, so its upper half shows sky), the rays
// rise in the centre's frame and left the old box within a few hundred units; a taller box keeps
// leaves / snow / petals against the sky. Test aid CF_WFX_TOP=<metres> (0 = the old box).
const float kParticleTop = [] {
    const char* v = std::getenv("CF_WFX_TOP");
    return v ? float(std::atof(v)) : 34.0f;
}();
void endWorld();
void onLayoutDraw();
bool characterBegin(u32 obj);
void characterEnd();

// ---- settings ([enhanced_world]) --------------------------------------------------------------
struct Settings {
    bool enabled = false;
    bool reflections = true;
    float reflectionStrength = 0.6f;  // 0..1.5
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
            const size_t semi = all.find_first_of(";,", pos);
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
bool sReflOn = false;   // water reflections on this frame (planar character reflections, Aurora 0570)
int sCharDepth = 0;     // inside a character's draws (characterScope)
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

// CF_WFX_KINDS="<material>=water|ground|world;...": test aid, overrides the classification below
// (to try a material's kind before adding it to the list).
const std::unordered_map<std::string, u8>& kindOverrides() {
    static const std::unordered_map<std::string, u8> m = [] {
        std::unordered_map<std::string, u8> out;
        if (const char* v = std::getenv("CF_WFX_KINDS")) {
            std::string s = v;
            size_t p = 0;
            while (p < s.size()) {
                size_t e = s.find_first_of(";,", p);
                if (e == std::string::npos) e = s.size();
                const std::string item = s.substr(p, e - p);
                const size_t eq = item.find('=');
                if (eq != std::string::npos) {
                    const std::string k = item.substr(eq + 1);
                    out[item.substr(0, eq)] = k == "water"    ? u8(AURORA_SURFACE_WATER)
                                              : k == "ground" ? u8(AURORA_SURFACE_GROUND)
                                                              : u8(AURORA_SURFACE_WORLD);
                }
                p = e + 1;
            }
        }
        return out;
    }();
    return m;
}

u8 kindOf(const std::string& n) {
    if (const auto& o = kindOverrides(); !o.empty())
        if (auto it = o.find(n); it != o.end()) return it->second;
    // Water: the sea, rivers and ponds (their pattern layer). Not: the river's foam edge, the sea's
    // white surf, wave shadows, the additive highlight layer, waterfalls and their ripples / spray.
    if (n == "m_ocean" || n == "m_river" || n == "m_river2_pattern" || n == "m_pond" || n == "m_water")
        return AURORA_SURFACE_WATER;
    // Walkable ground: grass (worn to soil by the grass wear), soil, sand, stone paths and plazas.
    if (n == "m_grass" || n == "m_soil" || n == "m_soil_sand" || n == "m_soil_stone" || n == "m_stone" ||
        n == "m_Es_G_soil" || n == "m_outgrass" || n == "m_buildsite" || n.rfind("m_plaza", 0) == 0 ||
        n.rfind("m_road", 0) == 0 || n.rfind("m_ground", 0) == 0)
        return AURORA_SURFACE_GROUND;
    // The city (d_bgt_drawNP, branch routes-2): pavements (hodo), the plaza's centre and the ring
    // round the fountain (surface mask checked with AURORA_WORLD_FX_SHOW=1: the plaza, the
    // streets and pavements). Not: flower beds (m_kadan), the bus stop, the fountain's water
    // m_water0-2 (tried as water with CF_WFX_KINDS: only the small top bowl is such a surface).
    if (n == "m_hodo" || n == "m_center" || n == "m_around_ft") return AURORA_SURFACE_GROUND;
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
    gx::endPendingDraw();  // (the game's last immediate-mode draw is closed lazily: not inside it)
    GXSetSurfaceAurora(k);
    sKind = k;
}

// ---- characters (branch reflection-face; Aurora 0570, docs/RENDERING.md "Planar reflections of
// characters") ----
// The player, villagers and the other NPCs are drawn a second time, mirrored through the water
// under them: their draws are AURORA_SURFACE_CHARACTER, one group per object (a transition into
// the kind starts a group, so two characters drawn back to back get a WORLD in between).
// Characters: the players' 'boy' / 'girl' models and their heads ('boy01a', ...), the villagers' and NPCs' three-letter species
// models ('kal', 'cbr', 'squ', ...), and the worn Mii mask (RFL head, its own draw: cf_tags.cpp).
bool isCharacterModel(const std::string& n) {
    // ('boy01a' etc.: the player's own head when no Mii mask is worn)
    if (n.compare(0, 3, "boy") == 0 || n.compare(0, 4, "girl") == 0) return true;
    if (n.size() != 3) return false;
    for (char ch : n)
        if (ch < 'a' || ch > 'z') return false;
    return true;
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
    float wetness = 0, rain = 0, sparkle = 0, haze = 0, moon = 0, clouds = 0;
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
        const u32 f = VA(0x8016D40C);
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
    a.clouds = st == 0 ? 0.3f : st == 1 ? 0.6f : 0.95f;
    // snow sparkle: snow cover (Dec 11 - Feb 24, as the game's terms 19-21) in daylight with the sun out
    const bool snowCover = a.md >= monthDay(11, 11) || a.md <= monthDay(1, 24);
    const float sun = sstep(6.5f, 8.0f, a.hour) * (1.0f - sstep(16.0f, 17.5f, a.hour));
    a.sparkle = snowCover ? sun * (st == 0 ? 1.0f : st == 1 ? 0.6f : st == 2 ? 0.2f : 0.0f) : 0.0f;
    // haze on grey days (and a trace at dawn / dusk)
    const float hz = st == 2 ? 0.18f : st == 3 ? 0.25f : st == 4 ? 0.38f : st == 5 ? 0.3f : st == 6 ? 0.42f : 0.0f;
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
    // Seasonal particles only in the town: its outdoor background module d_bgo_drawNP (108) is
    // linked there, indoors (d_bgi_drawNP 107) and in the city (d_bgt_drawNP 109, which has no
    // trees or blossoms) it is not. The city's pavements count as ground for the wet-ground
    // effects, so the surface test alone (outdoor) let leaves fall there (routes-2).
    const bool town = relBase(108, 1) != 0 && relBase(109, 1) == 0;
    if (!outdoor || !town) a.particles = 0;
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
        c.clouds = a.clouds;
        c.moonDir[0] = 0.0f, c.moonDir[1] = 0.45f, c.moonDir[2] = -0.89f;
        c.ambient[0] = a.ambient.r, c.ambient[1] = a.ambient.g, c.ambient[2] = a.ambient.b;
        c.tiltShift = s.tiltShift;
        c.tiltStrength = s.tiltStrength;
        c.tiltBand = s.tiltBand;
        c.grade = u32(s.grade);
        c.gradeStrength = s.gradeStrength;
        c.particles = s.particles ? u32(a.particles) : 0u;
        c.particleDensity = s.particleDensity;
        c.wind[0] = 0.35f, c.wind[1] = 0.12f;
        c.worldScale = kWorldScale;
        c.particleTop = kParticleTop;
        c.freezeTime = envFlag("CF_WFX_FREEZE");
    }
    aurora_world_fx_set_config(&c);
}

}  // namespace

// Startup (precache::init, before the first pipeline rebuild): the configuration the settings give
// outdoors, so the "Preparing shaders" hold already builds the surface-mask variants (Aurora 0451;
// the configuration was first sent with the first scene, after the hold, and the mask variants were
// then built while the game ran).
void applyStartupConfig() {
    const Settings s = current();
    if (s.enabled) applyConfig(s, true, true);
}

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
        sReflOn = sOn && s.reflections;
        sCharDepth = 0;
        if (!s.enabled && sApplied) {
            applyConfig(s, false, false);
            sApplied = false;
        }
        if (sOn) sApplied = true;
        if (sOn) cflight::wantGroundFit();  // the effects' centre (endWorld)
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

// The effects' centre on the town's rolling log (branch wfx-sky): the ground under the camera's
// look-at target, a little ahead of it. The game's camera orbits that target (about 64 units above
// the player's feet) at ~316 units: with D-pad Up ("look at the sky") it sinks from ~280 to ~180
// units above the ground and its pitch drops from ~52 to ~33 degrees below the horizontal, all
// about the same point (CF_WFX_FOCUS_LOG frames: every forward ray within 5 units of one point).
// The centre used to be where the view's forward ray met the log; looking up, that point raced off
// toward the horizon and then the ray missed the log altogether (the log falls away 38 degrees
// below the horizontal there), so the centre fell back to Aurora's depth focus (sky: 1500 units)
// and the frame to the snapped axis: the particle field jumped and zoomed ("the particles freak
// out"). The target is C + D forward for a fixed D, which does not move as the camera pitches
// about it; the ground point is its radial projection, moved kAhead along the ground in the
// camera's horizontal forward (so in the normal view the centre stays about where the forward ray
// met the ground before, and the look is unchanged). A D off by a few units moves the point by
// a few units over the whole look-up. Test aids: CF_WFX_CENTRE=ahead (the forward ray's ground
// point, as before), CF_WFX_TARGET=<D>,<ahead>.
bool effectsCentre(const float view[12], float out[3]) {
    static const int sAhead = [] {
        const char* v = std::getenv("CF_WFX_CENTRE");
        return v && std::strcmp(v, "ahead") == 0 ? 1 : 0;
    }();
    if (sAhead) return cflight::groundAhead(view, out);
    static const std::array<float, 2> sTarget = [] {
        std::array<float, 2> v{316.0f, 85.0f};
        if (const char* e = std::getenv("CF_WFX_TARGET")) std::sscanf(e, "%f,%f", &v[0], &v[1]);
        return v;
    }();
    float cy = 0.0f, cz = 0.0f, rad = 0.0f;
    if (!cflight::logAxis(cy, cz, rad) || !(rad > 100.0f)) return cflight::groundAhead(view, out);
    float C[3], f[3];
    for (int k = 0; k < 3; k++) {
        C[k] = -(view[k] * view[3] + view[4 + k] * view[7] + view[8 + k] * view[11]);
        f[k] = -view[8 + k];
    }
    float T[3];
    for (int k = 0; k < 3; k++) T[k] = C[k] + sTarget[0] * f[k];
    float ny = T[1] - cy, nz = T[2] - cz, nl = std::sqrt(ny * ny + nz * nz);
    if (!(nl > 1.0f)) return cflight::groundAhead(view, out);
    ny /= nl, nz /= nl;
    // the camera's forward along the ground at the target (the log's normal there removed)
    const float fn = f[1] * ny + f[2] * nz;
    float h[3] = {f[0], f[1] - fn * ny, f[2] - fn * nz};
    const float hl = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    if (hl > 0.05f)
        for (int k = 0; k < 3; k++) T[k] += sTarget[1] * h[k] / hl;
    ny = T[1] - cy, nz = T[2] - cz, nl = std::sqrt(ny * ny + nz * nz);
    if (!(nl > 1.0f)) return cflight::groundAhead(view, out);
    out[0] = T[0], out[1] = cy + rad * ny / nl, out[2] = cz + rad * nz / nl;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void endWorld() {
    gx::endPendingDraw();
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
    // The world's up axis: the town is z-up in g3d's world (the camera's up row leans to +z and its
    // forward to -z there); picked per frame among the six axes as the one the camera's up vector
    // points along and its forward away from (with some hysteresis), then rotated to +y.
    static int sAxis = 2;  // 0..5: +x -x +y -y +z -z
    {
        float best = -1e9f, cur = 0.0f;
        int bi = sAxis;
        for (int a = 0; a < 6; a++) {
            const int k = a / 2;
            const float sg = (a & 1) ? -1.0f : 1.0f;
            const float sc = sg * view[4 + k] + sg * view[8 + k];  // up . a - forward . a (forward = -row 2)
            if (a == sAxis) cur = sc;
            if (sc > best) best = sc, bi = a;
        }
        if (best > cur + 0.3f) sAxis = bi;
    }
    float e1[3] = {0, 0, 0};
    e1[sAxis / 2] = (sAxis & 1) ? -1.0f : 1.0f;
    float e0[3] = {sAxis / 2 == 0 ? 0.0f : 1.0f, sAxis / 2 == 0 ? 1.0f : 0.0f, 0.0f};
    float e2[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0]};
    float origin[3] = {0.0f, 0.0f, 0.0f};  // g3d world place of the effects frame's origin
    // The town is a "rolling log" (cf_light.cpp LogFit): walking north / south, the game turns the
    // camera about the log's axis (world X; the camera's translation stays put). The snapped axis
    // above then flipped by 90 degrees (+z <-> -y) as the camera turned, which turned the leaves'
    // volume and their fall with it, and the world frame never matched the ground under the player
    // anyway (branch interp-town: leaves jumped while walking, at any frame rate). With a fitted
    // log the effects frame is the log's own flat frame at the centre F (effectsCentre: the ground
    // under the camera's target, a little ahead; before wfx-sky the forward ray's ground point): x = the log's axis,
    // y = its normal at F, z = along the ground, with F at (F.x, 0, -R theta) (theta F's angle about
    // the axis, unwrapped): the ground near F keeps its effect coordinates as the log turns, so the
    // leaves, ripples and puddles stay on it; the frame turns continuously. CF_WFX_AXIS=snap: the old
    // snapped axis.
    static const bool sSnapAxis = [] {
        const char* v = std::getenv("CF_WFX_AXIS");
        return v && std::strcmp(v, "snap") == 0;
    }();
    float F[3], cy = 0.0f, cz = 0.0f, rad = 0.0f;
    bool logFrame = false;
    if (!sSnapAxis && effectsCentre(view, F) && cflight::logAxis(cy, cz, rad)) {
        const float ny = F[1] - cy, nz = F[2] - cz, nl = std::sqrt(ny * ny + nz * nz);
        if (nl > 1.0f) {
            static float sTheta = 0.0f;
            static bool sThetaValid = false;
            float th = std::atan2(ny, nz);
            if (sThetaValid)  // unwrapped: no step at +-180 degrees
                while (th - sTheta > 3.14159265f) th -= 6.2831853f;
            if (sThetaValid)
                while (th - sTheta < -3.14159265f) th += 6.2831853f;
            sTheta = th, sThetaValid = true;
            e0[0] = 1.0f, e0[1] = 0.0f, e0[2] = 0.0f;
            e1[0] = 0.0f, e1[1] = ny / nl, e1[2] = nz / nl;
            e2[0] = 0.0f, e2[1] = -e1[2], e2[2] = e1[1];  // e0 x e1
            const float q[3] = {F[0], 0.0f, -rad * th};
            for (int k = 0; k < 3; k++) origin[k] = F[k] - (e0[k] * q[0] + e1[k] * q[1] + e2[k] * q[2]);
            logFrame = true;
        }
    }
    float w[12];
    for (int r = 0; r < 3; r++) {
        const float* row = view + r * 4;
        w[r * 4 + 0] = row[0] * e0[0] + row[1] * e0[1] + row[2] * e0[2];
        w[r * 4 + 1] = row[0] * e1[0] + row[1] * e1[1] + row[2] * e1[2];
        w[r * 4 + 2] = row[0] * e2[0] + row[1] * e2[1] + row[2] * e2[2];
        w[r * 4 + 3] = row[0] * origin[0] + row[1] * origin[1] + row[2] * origin[2] + row[3];
    }
    if (sLog && game::guestFrames() % 120 < 2)
        log("[wfx] f%u effects frame %s up %.3f %.3f %.3f origin %.1f %.1f %.1f", game::guestFrames(), logFrame ? "log" : "snapped", e1[0],
            e1[1], e1[2], origin[0], origin[1], origin[2]);
    // Both matrices are noted as drawn from the scene's camera (branch wfx-sky): this runs at the
    // HUD's first draw, under its orthographic projection, so they were 2D values blended in view
    // space (the effects frame's origin is thousands of units away, its view-space translation moved
    // 70-100 units per game frame while the camera swung back from the sky view), and between game
    // frames the falling leaves lagged the scene and caught up at each game frame (120 fps: every
    // leaf stepping 4-6 px together). Factored by the camera they blend with the world.
    // CF_WFX_VIEW_NOTE=0: as before (A/B).
    static const bool sOwnView = [] {
        const char* v = std::getenv("CF_WFX_VIEW_NOTE");
        return !(v && *v == '0');
    }();
    struct ViewOverride {
        explicit ViewOverride(u32 src) { interp::setViewOverride(src); }
        ~ViewOverride() { interp::setViewOverride(0); }
    } viewOverride(sOwnView ? sMainRoot + 0xF8 + rd8(sMainRoot + 0xF4) * 0x10C : 0u);
    static const bool sDepthCentre = envFlag("CF_WFX_DEPTH_FOCUS");  // A/B: the old centre
    {
        interp::TagScope tag(interp::TagCat::Object, 0xCF57F000u, 0x57F0u, 0);
        const u32 at = interp::mark();
        GXLoadAppXfAurora(AURORA_WORLD_FX_XF_VIEW, w, 12);
        interp::note(at, interp::Kind::PosMtx, 0xCF57F000u, AURORA_WORLD_FX_XF_VIEW, 12);
    }
    {
        // The effects' centre (Aurora 0440): the ground under the camera's look-at target on the
        // rolling log (effectsCentre above; the CameraData target is not set by the game), as
        // the model matrix of a world-aligned object there, so the in-between frames blend it with
        // the world. Before, the centre was the scene depth under one pixel near the view's middle:
        // the player, a tree or the ground behind them as they passed under it, so the falling
        // leaves' volume and their scale jumped while walking, at any frame rate (branch
        // interp-town). No fit (indoors, the city, the first frames): zero = Aurora's depth centre.
        float fm[12] = {};
        float g[3] = {0.0f, 0.0f, 0.0f};
        if (!sDepthCentre && effectsCentre(view, g)) {
            std::memcpy(fm, w, sizeof fm);
            for (int r = 0; r < 3; r++) fm[r * 4 + 3] = view[r * 4] * g[0] + view[r * 4 + 1] * g[1] + view[r * 4 + 2] * g[2] + view[r * 4 + 3];
        }
        static const bool sFocusLog = envFlag("CF_WFX_FOCUS_LOG");  // test aid: the centre every frame
        if (sFocusLog) {
            float lcy = 0, lcz = 0, lr = 0;
            cflight::logAxis(lcy, lcz, lr);
            log("[wfx] f%u focus world %.1f %.1f %.1f view %.1f %.1f %.1f cam t %.1f %.1f %.1f r1 %.4f %.4f %.4f r2 %.4f %.4f %.4f "
                "log %.1f %.1f %.1f",
                game::guestFrames(), g[0], g[1], g[2], fm[3], fm[7], fm[11], view[3], view[7], view[11], view[4], view[5], view[6],
                view[8], view[9], view[10], lcy, lcz, lr);
        }
        interp::TagScope tag(interp::TagCat::Object, 0xCF57F001u, 0x57F1u, 0);
        const u32 at = interp::mark();
        GXLoadAppXfAurora(AURORA_WORLD_FX_XF_FOCUS, fm, 12);
        interp::note(at, interp::Kind::PosMtx, 0xCF57F001u, AURORA_WORLD_FX_XF_FOCUS, 12);
    }
    const float* vf = view;  // (log below)
    (void)target;
    GXWorldFxEndAurora();
    if (sLog && game::guestFrames() % 120 < 2) {
        const Amounts& a = sAmt;
        {
            const u32 data = sMainRoot + 0xF8 + rd8(sMainRoot + 0xF4) * 0x10C;
            std::string s;
            char b[32];
            for (u32 o = 0x70; o < 0xB8; o += 4) {
                std::snprintf(b, sizeof b, " %X:%.1f", o, rdf(data + o));
                s += b;
            }
            log("[wfx] camera data%s", s.c_str());
        }
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
        if (world::sCharDepth == 0) world::emitKind(k);  // (a character's materials stay CHARACTER)
    }
    callOriginal(c, "Draw1Mat1ShpDirectly__Q24nw4r3g3dFQ34nw4r3g3d6ResMatQ34nw4r3g3d6ResShpPCQ34nw4r4math5MTX34PCQ34nw4r4math5MTX34UlPQ34nw4r3g3d16Draw1Mat1ShpSwapPQ44nw4r3g3d8G3DState8IndMtxOp");
}

// cf_tags.cpp: a scene object (`obj`, 0 = the worn Mii mask's RFL head) begins / ends its draws.
bool world::characterBegin(u32 obj) {
    // Test aid CF_WFX_CHAR_LOG=1: the objects tested (every 300 frames).
    static const bool sCharLog = envFlag("CF_WFX_CHAR_LOG");
    if (sCharLog && game::guestFrames() % 300 == 0)
        log("[wfx] f%u character? obj %08X '%s' refl %d main %d", game::guestFrames(), obj, obj ? cflight::modelNameChecked(obj).c_str() : "(RFL head)",
            sReflOn ? 1 : 0, sMain ? 1 : 0);
    if (!sReflOn || !sMain) return false;
    const std::string name = obj ? cflight::modelNameChecked(obj) : std::string();
    if (obj && !isCharacterModel(name)) return false;
    if (sCharDepth++ == 0) {
        // A part of a character (branch reflection-head, Aurora 0580): the player's own head model
        // ('boy01a', ...: its own object, drawn after the body) and the worn Mii head are mirrored
        // through the plane of the character they belong to; with a plane of their own the head's
        // water lookup could fail while the body's found water (a reflection without its head).
        const bool part = obj == 0 || ((name.compare(0, 3, "boy") == 0 || name.compare(0, 4, "girl") == 0) && name != "boy" && name != "girl");
        const u8 kind = part ? AURORA_SURFACE_CHARACTER_PART : AURORA_SURFACE_CHARACTER;
        if (sKind == AURORA_SURFACE_CHARACTER || sKind == AURORA_SURFACE_CHARACTER_PART) emitKind(AURORA_SURFACE_WORLD);
        emitKind(kind);
    }
    return true;
}
void world::characterEnd() {
    if (sCharDepth > 0 && --sCharDepth == 0) emitKind(AURORA_SURFACE_WORLD);
}

// cf_hud.cpp: a layout's root pane is drawn (the 2D starts): the first one after the main pass ends
// the world (after enhanced lighting's scene end, which goes in at the same point).
void world::onLayoutDraw() {
    if (sOn && sPendingEnd) endWorld();
}

// dWeather_c every frame (USA / Europe / Japan / Korea).
HLE(fn_801C92F0) { world::weatherHook(c, "fn_801C92F0"); }
HLE(fn_801C8FE4) { world::weatherHook(c, "fn_801C8FE4"); }
HLE(fn_801C9090) { world::weatherHook(c, "fn_801C9090"); }
HLE(fn_801C90C8) { world::weatherHook(c, "fn_801C90C8"); }

}  // namespace recomp
