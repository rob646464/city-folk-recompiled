// City Folk enhanced lighting provider (cf_light.h; docs/RENDERING.md "Enhanced lighting (City
// Folk)"). Everything here only runs while F1 > Visuals > Enhanced lighting is on; with it off the
// hooks call the original and send nothing (the original look stays byte-identical).
//
// Scene structure (USA rev 1, every version by hash): the field / room scene is drawn by
// d_s_stageNP fn_125_C0, which draws the stage's ScnRoot twice (DrawOpa + DrawXlu + effects each):
// pass 1 with camera 0 is the picture, pass 2 with camera 1 a second layer (rotated light set,
// nothing of it visible in the town views checked: "Scene passes" in the docs). The 2D (lyt) comes
// after. So: the enhanced scene begins at pass 1's DrawOpa (draw class WORLD), pass 2 is drawn as
// class OTHER (its other camera would break the screen-space passes), the scene ends after
// fn_125_C0 or at the first root layout pane, whichever comes first.
#include "game/cf_light.h"
#include "game/cf_calendar.h"

#include "port/settings.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <aurora/lights.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp

namespace cflight {
namespace {

// Test aid CF_SCENE_LOG=<every n frames>: stage draw, root passes, cameras, layout start.
const u32 sSceneLog = [] {
    const char* v = std::getenv("CF_SCENE_LOG");
    return v ? u32(std::max(std::atoi(v), 1)) : 0u;
}();
bool logNow() { return sSceneLog && copyDispCount() % sSceneLog == 0; }
// Test aid CF_SCENE_SKIP=<n>: root pass n (1-based) of the stage draw is not drawn.
const int sSkipPass = [] {
    const char* v = std::getenv("CF_SCENE_SKIP");
    return v ? std::atoi(v) : 0;
}();

bool sInStage = false;     // inside fn_125_C0
bool sSceneOpen = false;   // enh::beginScene sent, endScene not yet
u32 sPass = 0;             // root DrawOpa calls so far in this stage draw
bool sInPass2 = false;
bool sBeginPending = false;  // pass 1 started: the scene begins at its first camera
u32 sMainCam = 0;
float sView[12] = {};      // main camera (pass 1), world -> view
bool sViewValid = false;
u32 sViewFrame = ~0u;

// ---- the sky light: sun by day, moon by night ----------------------------------------------
// World axes of the town (from the main camera, docs): +Y up, +X east (screen right), +Z south
// (toward the camera, which faces north).
struct Clock {
    int year = 2026, mon = 9, mday = 27, hour = 10, min = 0, sec = 0, yday = 300;
    bool valid = false;
};
Clock readClock() {
    Clock k;
    const u32 f = calendar::versionAddr(0x8016D40C);
    const u32 lis = rd32(f), addi = rd32(f + 4);
    if ((lis >> 26) != 15 || (addi >> 26) != 14) return k;  // lis / addi expected
    const u32 t = ((lis & 0xFFFF) << 16) + u32(int32_t(int16_t(addi & 0xFFFF)));
    k.sec = int(rd32(t)), k.min = int(rd32(t + 4)), k.hour = int(rd32(t + 8)), k.mday = int(rd32(t + 0xC));
    k.mon = int(rd32(t + 0x10)), k.year = int(rd32(t + 0x14)), k.yday = int(rd32(t + 0x1C));
    k.valid = k.hour >= 0 && k.hour < 24 && k.min >= 0 && k.min < 60 && k.mon >= 0 && k.mon < 12;
    return k;
}
// Test aid CF_SUN_HOUR=<h.hh>: the sky light as at that hour of the day (the game clock is untouched).
const float sHourOverride = [] {
    const char* v = std::getenv("CF_SUN_HOUR");
    return v ? float(std::atof(v)) : -1.0f;
}();

void smoothstepClamp(float& v) { v = std::clamp(v, 0.0f, 1.0f); v = v * v * (3.0f - 2.0f * v); }
float sstep(float a, float b, float x) {
    float t = (x - a) / (b - a);
    smoothstepClamp(t);
    return t;
}
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
void lerp3(const float* a, const float* b, float t, float* o) {
    for (int k = 0; k < 3; k++) o[k] = lerpf(a[k], b[k], t);
}

// Sun direction (world, toward the sun) for a day of the year and a local solar hour at latitude
// `lat` (degrees; the port uses 38 N, mirrored for the southern hemisphere option).
void sunWorld(float yday, float hour, float lat, float out[3]) {
    constexpr float kPi = 3.14159265f;
    const float decl = 23.44f * std::sin(2.0f * kPi * (yday - 80.0f) / 365.0f) * kPi / 180.0f;
    const float h = (hour - 12.0f) * 15.0f * kPi / 180.0f;
    const float phi = lat * kPi / 180.0f;
    const float east = -std::cos(decl) * std::sin(h);
    const float north = std::cos(phi) * std::sin(decl) - std::sin(phi) * std::cos(decl) * std::cos(h);
    const float up = std::sin(phi) * std::sin(decl) + std::cos(phi) * std::cos(decl) * std::cos(h);
    out[0] = east, out[1] = up, out[2] = -north;
}

struct SkyLight {
    float dir[3];      // world, toward the light
    float color[3];    // added light colour (x intensity)
    float intensity;
    float tint[3];     // full-shadow multiplier
    float away;        // turned-away surfaces count as shadowed
    float tanSize;     // angular radius (softness)
    bool on;
};
int sWeather = 0;      // the town's weather state this hour (cf_weather: 0 clear .. 6 heavy snow)
u32 sWeatherFrame = 0;

SkyLight skyLight(const Clock& k) {
    const auto& s = port::settings().enhancedLighting;
    float hour = float(k.hour) + float(k.min) / 60.0f + float(k.sec) / 3600.0f;
    if (sHourOverride >= 0.0f) hour = sHourOverride;
    const float lat = calendar::southern() ? -38.0f : 38.0f;
    float sun[3];
    sunWorld(float(k.yday), hour, lat, sun);
    float moon[3];
    sunWorld(float((k.yday + 182) % 365), hour + 12.0f, lat, moon);  // full-moon-like path opposite the sun
    const float sunEl = std::asin(std::clamp(sun[1], -1.0f, 1.0f)) * 57.2958f;
    const float moonEl = std::asin(std::clamp(moon[1], -1.0f, 1.0f)) * 57.2958f;
    SkyLight l{};
    // Day: the sun from 2 degrees above the horizon; night: the moon once the sun is 6 below.
    const float day = sstep(-1.0f, 4.0f, sunEl);
    const float night = sstep(-2.0f, -8.0f, sunEl) * sstep(2.0f, 10.0f, moonEl);
    const bool useSun = day >= night;
    const float* d = useSun ? sun : moon;
    float el = useSun ? sunEl : moonEl;
    // Keep the shadow-casting light at least 12 degrees up: a grazing light stretches shadows over
    // the whole town (and the game's ground is not flat: the "rolling log" curve).
    float dir[3] = {d[0], std::max(d[1], 0.0f), d[2]};
    const float minEl = 12.0f * 3.14159265f / 180.0f;
    {
        const float hl = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
        const float curEl = std::atan2(dir[1], std::max(hl, 1e-4f));
        if (curEl < minEl && hl > 1e-4f) {
            dir[0] = dir[0] / hl * std::cos(minEl), dir[2] = dir[2] / hl * std::cos(minEl), dir[1] = std::sin(minEl);
        }
    }
    std::memcpy(l.dir, dir, sizeof dir);
    // Weather: clouds soften and weaken the shadows; rain / snow almost remove them.
    float wx = 1.0f;
    if (s.timeOfDayWeather) {
        static constexpr float kWx[7] = {1.0f, 0.75f, 0.3f, 0.12f, 0.06f, 0.15f, 0.08f};
        wx = kWx[std::clamp(sWeather, 0, 6)];
    }
    const float strength = std::clamp(s.sunShadowStrength, 0.0f, 1.0f) * (useSun ? day : night * 0.6f) * wx;
    // Shadow tint: cool sky-blue shadows by day, deeper and bluer at the golden hour, grey-blue
    // under the moon.
    const float golden = useSun ? 1.0f - sstep(6.0f, 25.0f, el) : 0.0f;
    static const float kNoon[3] = {0.80f, 0.86f, 1.0f}, kGold[3] = {0.66f, 0.72f, 1.0f}, kMoon[3] = {0.82f, 0.86f, 1.0f};
    float base[3];
    lerp3(kNoon, kGold, golden, base);
    if (!useSun) std::memcpy(base, kMoon, sizeof base);
    for (int c = 0; c < 3; c++) l.tint[c] = 1.0f - strength * (1.0f - base[c] * 0.72f);
    static const float sAway = std::getenv("CF_SUN_AWAY") ? float(std::atof(std::getenv("CF_SUN_AWAY"))) : 0.75f;  // test aid
    l.away = sAway;
    l.tanSize = (useSun ? 0.012f : 0.02f) * std::clamp(s.sunShadowSoftness, 0.0f, 3.0f) * (1.0f + 2.0f * (1.0f - wx));
    // Added light: warm sunlight on sun-facing sides, golden near sunrise / sunset; faint cool moonlight.
    static const float kSunNoon[3] = {1.0f, 0.96f, 0.88f}, kSunGold[3] = {1.0f, 0.66f, 0.36f}, kMoonCol[3] = {0.55f, 0.66f, 1.0f};
    if (useSun) lerp3(kSunNoon, kSunGold, golden, l.color);
    else std::memcpy(l.color, kMoonCol, sizeof l.color);
    l.intensity = s.sunLight ? std::clamp(s.sunLightIntensity, 0.0f, 1.0f) * (useSun ? day * (0.6f + 0.4f * golden) : night * 0.35f) * wx : 0.0f;
    l.on = strength > 0.002f || l.intensity > 0.002f;
    return l;
}

void submitSky() {
    const auto& s = port::settings().enhancedLighting;
    if (!sViewValid || !(s.sunShadows || s.sunLight)) return;
    static Clock sClock;
    static u32 sClockFrame = ~0u;
    if (copyDispCount() / 30 != sClockFrame) {  // twice a second
        sClockFrame = copyDispCount() / 30;
        sClock = readClock();
    }
    if (!sClock.valid) return;
    const SkyLight sky = skyLight(sClock);
    if (!sky.on) return;
    aurora::gfx::ExtraLight l{};
    for (int r = 0; r < 3; r++) l.pos[r] = sView[r * 4] * sky.dir[0] + sView[r * 4 + 1] * sky.dir[1] + sView[r * 4 + 2] * sky.dir[2];
    std::memcpy(l.color, sky.color, sizeof l.color);
    l.intensity = sky.intensity;
    l.radius = std::clamp(s.sunShadowDistance, 500.0f, 20000.0f);
    l.flags = aurora::gfx::ExtraLightDirectional | (s.sunShadows ? aurora::gfx::ExtraLightCastShadows : 0u);
    l.id = 0x5E000001u;
    std::memcpy(l.box, sky.tint, sizeof sky.tint);
    l.box[3] = s.sunShadows ? sky.away : 0.0f;
    l.box[4] = 0.25f;
    l.box[5] = sky.tanSize;
    l.box[6] = 4000.0f;
    if (!s.sunShadows) l.box[0] = l.box[1] = l.box[2] = 1.0f;
    submitExtraLights(&l, 1);
    if (logNow())
        log("[sky] f%u %02d:%02d yday %d weather %d dir %.2f %.2f %.2f view %.2f %.2f %.2f tint %.2f %.2f %.2f I %.2f soft %.3f",
            copyDispCount(), sClock.hour, sClock.min, sClock.yday, sWeather, sky.dir[0], sky.dir[1], sky.dir[2], l.pos[0], l.pos[1],
            l.pos[2], sky.tint[0], sky.tint[1], sky.tint[2], sky.intensity, sky.tanSize);
}

void endScene() {
    if (!sSceneOpen) return;
    sSceneOpen = false;
    enh::setCasterPass(false);
    enh::endScene();
}

}  // namespace

void onCamera(Ctx& c, u32 data) {
    (void)c;
    if (!data) return;
    // The first camera pass 1 sets is the main camera (camera 0; the root then sets its other
    // cameras, identity views of other layers). The enhanced scene begins there (the lights are
    // in this frame's view space), and only draws under the main camera are class WORLD.
    if (sInStage && sPass == 1) {
        if (sBeginPending) {
            sBeginPending = false;
            sMainCam = data;
            for (int k = 0; k < 12; k++) sView[k] = rdf32(data + 4 * k);
            sViewValid = std::isfinite(sView[0]) && std::isfinite(sView[11]);
            sViewFrame = copyDispCount();
            sSceneOpen = true;
            submitSky();
            enh::beginScene();
        } else if (sSceneOpen) {
            // (ScnRoot::DrawOpa loads all 32 cameras up front: the last one set is not the current one)
        }
    }
    if (logNow())
        log("[scene] f%u camera %08X pass %u stage %d view %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f",
            copyDispCount(), data, sPass, sInStage, rdf32(data), rdf32(data + 4), rdf32(data + 8), rdf32(data + 12), rdf32(data + 16),
            rdf32(data + 20), rdf32(data + 24), rdf32(data + 28), rdf32(data + 32), rdf32(data + 36), rdf32(data + 40),
            rdf32(data + 44));
}

bool onRootDraw(Ctx& c, bool xlu, bool begin) {
    if (begin && !xlu && sInStage) sPass++;
    if (logNow() && begin) log("[scene] f%u root %08X %s pass %u stage %d", copyDispCount(), c.r[3], xlu ? "xlu" : "opa", sPass, sInStage);
    if (!sInStage) return true;
    if (sSkipPass && int(sPass) == sSkipPass) return false;
    if (begin && !xlu && sPass == 1 && enh::masterOn() && !sSceneOpen) sBeginPending = true;  // at its first camera
    if (begin && !xlu && sPass == 2 && sSceneOpen) {
        // The second layer: not part of the enhanced scene (its own camera).
        sInPass2 = true;
        enh::setCasterPass(true);
    }
    return true;
}

// ---- g3d models (night-light sources) ----
// A scene model's ResMdl: the first word in the object pointing at an 'MDL0' block; its name
// (MDL0 rev 9/10/11: name offset after the section offsets). Cached per object.
namespace {
struct ModelInfo {
    u32 mdl = 0;
    std::string name;
};
std::unordered_map<u32, ModelInfo> sModels;
bool guestPtr(u32 p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }
const ModelInfo& modelInfo(u32 obj) {
    auto [it, fresh] = sModels.try_emplace(obj);
    if (!fresh) return it->second;
    if (sModels.size() > 8192) sModels.clear(), it = sModels.try_emplace(obj).first;
    ModelInfo& mi = it->second;
    for (u32 off = 0xC; off < 0x180 && !mi.mdl; off += 4) {
        const u32 p = rd32(obj + off);
        if (guestPtr(p) && (p & 3) == 0 && rd32(p) == 0x4D444C30u) mi.mdl = p;  // 'MDL0'
    }
    if (mi.mdl) {
        const u32 rev = rd32(mi.mdl + 8);
        const u32 sections = rev >= 10 ? 14 : 11;
        const s32 nameOff = s32(rd32(mi.mdl + 0x10 + 4 * sections));
        if (nameOff > 0 && nameOff < 0x1000000) {
            const u32 s = mi.mdl + u32(nameOff);
            for (u32 k = 0; k < 48; k++) {
                const char ch = char(rd8(s + k));
                if (!ch) break;
                mi.name += (ch >= 32 && ch < 127) ? ch : '?';
            }
        }
    }
    return mi;
}
const bool sModelLog = std::getenv("CF_MODEL_LOG") != nullptr;
}  // namespace

void onModel(Ctx& c, u32 obj) {
    if (!sModelLog && !enh::masterOn()) return;
    const ModelInfo& mi = modelInfo(obj);
    if (sModelLog) {
        static std::unordered_map<std::string, u32> sSeen;
        if (sSeen[mi.name]++ < 2)
            log("[model] f%u obj %08X task %u mdl %08X '%s' m3c t %.1f %.1f %.1f  m0c t %.1f %.1f %.1f", copyDispCount(), obj, c.r[4],
                mi.mdl, mi.name.c_str(), rdf32(obj + 0x48), rdf32(obj + 0x58), rdf32(obj + 0x68), rdf32(obj + 0x18), rdf32(obj + 0x28),
                rdf32(obj + 0x38));
    }
}

void onLayoutDraw(Ctx& c) {
    (void)c;
    if (sSceneOpen) {
        if (logNow()) log("[scene] f%u layout starts inside the scene: scene ends", copyDispCount());
        endScene();
    }
}

}  // namespace cflight

// d_s_stageNP fn_125_C0: the stage's 3D (both root passes and their effects).
static void stageDraw(Ctx& c) {
    if (cflight::logNow()) log("[scene] f%u stage draw begins", copyDispCount());
    cflight::sInStage = true;
    cflight::sPass = 0;
    cflight::sInPass2 = false;
    cflight::sBeginPending = false;
    if (enh::masterOn() && copyDispCount() - cflight::sWeatherFrame >= 120) {
        // The town's weather this hour (fn_801C9E48(time*), cf_weather.cpp's hook included).
        cflight::sWeatherFrame = copyDispCount();
        const u32 f = calendar::versionAddr(0x8016D40C);
        const u32 lis = rd32(f), addi = rd32(f + 4);
        if ((lis >> 26) == 15 && (addi >> 26) == 14) {
            const u32 t = ((lis & 0xFFFF) << 16) + u32(int32_t(int16_t(addi & 0xFFFF)));
            const u32 st = guestCall(c, calendar::versionAddr(0x801C9E48), t);
            cflight::sWeather = st <= 6 ? int(st) : 0;
        }
    }
    callOriginal(c, "d_s_stageNP::fn_125_C0");
    cflight::sInStage = false;
    cflight::endScene();
    if (cflight::logNow()) log("[scene] f%u stage draw ends after %u passes", copyDispCount(), cflight::sPass);
}
static HleReg sStageReg("d_s_stageNP::fn_125_C0", &stageDraw);

namespace game {
bool enhViewMatrix(float m[12]) {
    if (!cflight::sViewValid) return false;
    std::memcpy(m, cflight::sView, sizeof(cflight::sView));
    return true;
}
bool enhPlayerPos(float*) { return false; }
void enhBeginScene(Ctx&) {}
bool enhRoomBounds(const float*, float*, float*) { return false; }
}  // namespace game

}  // namespace recomp
