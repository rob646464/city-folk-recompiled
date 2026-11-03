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
#include "recomp/version.h"

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
#include <array>
#include <vector>

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
    const u32 f = VA(0x8016D40C);
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
    // Time-of-day look (Aurora 0342 grade, 0343 shafts)
    float gain[3], lift[3], saturation, contrast, shafts;
    float sunEl, strength;
};

// The time-of-day grade by the sun's elevation (degrees): noon, golden hour, blue dusk, moonlit
// night, blended; overcast pulls toward a flat, slightly grey look. `k` scales the deviation
// from neutral (F1 strength).
void timeOfDayLook(float sunEl, float wx, float k, SkyLight& l) {
    struct Key {
        float gain[3], lift[3], sat, con;
    };
    static constexpr Key kNoon = {{1.00f, 1.00f, 0.99f}, {0.0f, 0.0f, 0.0f}, 1.05f, 1.03f};
    static constexpr Key kGold = {{1.10f, 0.97f, 0.82f}, {0.025f, 0.01f, 0.0f}, 1.14f, 1.06f};
    static constexpr Key kDusk = {{0.86f, 0.88f, 1.07f}, {0.0f, 0.008f, 0.035f}, 0.96f, 1.02f};
    static constexpr Key kNight = {{0.80f, 0.88f, 1.08f}, {0.0f, 0.004f, 0.025f}, 0.86f, 1.06f};
    static constexpr Key kGrey = {{0.97f, 0.98f, 1.00f}, {0.02f, 0.02f, 0.025f}, 0.88f, 0.95f};
    // weights: night below -10, dusk -10..-1, golden -1..15, noon above 25
    const float wNight = 1.0f - sstep(-12.0f, -6.0f, sunEl);
    const float wDusk = sstep(-12.0f, -6.0f, sunEl) * (1.0f - sstep(-2.0f, 2.0f, sunEl));
    const float wGold = sstep(-2.0f, 2.0f, sunEl) * (1.0f - sstep(10.0f, 25.0f, sunEl));
    const float wNoon = sstep(10.0f, 25.0f, sunEl);
    Key m{};
    const Key* keys[4] = {&kNight, &kDusk, &kGold, &kNoon};
    const float w[4] = {wNight, wDusk, wGold, wNoon};
    const float ws = std::max(w[0] + w[1] + w[2] + w[3], 1e-4f);
    for (int i = 0; i < 4; i++) {
        const float f = w[i] / ws;
        for (int c = 0; c < 3; c++) m.gain[c] += keys[i]->gain[c] * f, m.lift[c] += keys[i]->lift[c] * f;
        m.sat += keys[i]->sat * f, m.con += keys[i]->con * f;
    }
    // Clouds: toward the grey look by day (night stays night).
    const float grey = (1.0f - wx) * (1.0f - wNight) * 0.85f;
    for (int c = 0; c < 3; c++) m.gain[c] = lerpf(m.gain[c], kGrey.gain[c], grey), m.lift[c] = lerpf(m.lift[c], kGrey.lift[c], grey);
    m.sat = lerpf(m.sat, kGrey.sat, grey), m.con = lerpf(m.con, kGrey.con, grey);
    for (int c = 0; c < 3; c++) l.gain[c] = 1.0f + (m.gain[c] - 1.0f) * k, l.lift[c] = m.lift[c] * k;
    l.saturation = 1.0f + (m.sat - 1.0f) * k;
    l.contrast = 1.0f + (m.con - 1.0f) * k;
    // Shafts: strongest with a low sun, faint at noon, a little under the moon, none in rain.
    l.shafts = (0.35f * wNoon + 1.0f * wGold + 0.15f * wNight) * wx * wx;
}
int sWeather = 0;      // the town's weather state this hour (cf_weather: 0 clear .. 6 heavy snow)
u32 sWeatherFrame = 0;
float sNight = 0.0f;        // night amount 0..1 (sun below the horizon; night lights)
float sSunStrength = 0.0f;  // strength of the real shadows this frame (blob-shadow fade)
// Local "up" of the rolling-log ground (world space): the mean Y axis of the placed objects
// (obj_* / sdw_* models) drawn in the previous frame; world Y until the first one.
float sLocalUp[3] = {0.0f, 1.0f, 0.0f};
float sUpAcc[3] = {};
u32 sUpCount = 0, sUpFrame = ~0u;

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
        static constexpr float kWx[7] = {1.0f, 0.85f, 0.3f, 0.12f, 0.06f, 0.15f, 0.08f};
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
    for (int c = 0; c < 3; c++) l.tint[c] = 1.0f - strength * (1.0f - base[c] * 0.6f);
    static const float sAway = std::getenv("CF_SUN_AWAY") ? float(std::atof(std::getenv("CF_SUN_AWAY"))) : 0.75f;  // test aid
    l.away = sAway;
    l.tanSize = (useSun ? 0.012f : 0.02f) * std::clamp(s.sunShadowSoftness, 0.0f, 3.0f) * (1.0f + 2.0f * (1.0f - wx));
    // Added light: warm sunlight on sun-facing sides, golden near sunrise / sunset; faint cool moonlight.
    static const float kSunNoon[3] = {1.0f, 0.96f, 0.88f}, kSunGold[3] = {1.0f, 0.66f, 0.36f}, kMoonCol[3] = {0.55f, 0.66f, 1.0f};
    if (useSun) lerp3(kSunNoon, kSunGold, golden, l.color);
    else std::memcpy(l.color, kMoonCol, sizeof l.color);
    l.intensity = s.sunLight ? std::clamp(s.sunLightIntensity, 0.0f, 1.0f) * (useSun ? day * (0.6f + 0.4f * golden) : night * 0.35f) * wx : 0.0f;
    l.on = strength > 0.002f || l.intensity > 0.002f;
    timeOfDayLook(sunEl, wx, std::clamp(s.timeOfDayStrength, 0.0f, 2.0f), l);
    l.sunEl = sunEl;
    l.strength = strength;
    return l;
}

void submitSky() {
    const auto& s = port::settings().enhancedLighting;
    static Clock sClock;
    static u32 sClockFrame = ~0u;
    if (copyDispCount() / 30 != sClockFrame) {  // twice a second
        sClockFrame = copyDispCount() / 30;
        sClock = readClock();
    }
    if (!sClock.valid) return;
    SkyLight sky = skyLight(sClock);
    // Indoors (the room background module d_bgi_drawNP is linked, the outdoor one d_bgo_drawNP
    // is not): no sun, no time-of-day grade; with "Indoor shadows" the room's own key light casts
    // softer, lighter shadows.
    const bool indoor = relBase(107, 1) != 0 && relBase(108, 1) == 0;
    static bool sWasIndoor = false;
    if (indoor != sWasIndoor) log("[sky] f%u %s", copyDispCount(), indoor ? "indoors" : "outdoors");
    sWasIndoor = indoor;
    sNight = indoor ? 0.0f : 1.0f - sstep(-6.0f, 1.0f, sky.sunEl);
    sSunStrength = indoor ? (s.indoorShadows && s.sunShadows ? std::clamp(s.sunShadowStrength, 0.0f, 1.0f) * 0.6f : 0.0f)
                          : (s.sunShadows ? sky.strength : 0.0f);
    if (indoor) {
        static const float kOne[3] = {1, 1, 1}, kZero[3] = {0, 0, 0};
        enh::setGrade(kOne, kZero, 1.0f, 1.0f);
        enh::setShafts(0.0f);
        if (!s.indoorShadows) return;
        const float k = std::clamp(s.sunShadowStrength, 0.0f, 1.0f) * 0.6f;
        for (int c = 0; c < 3; c++) sky.tint[c] = 1.0f - k * (1.0f - (c == 2 ? 0.8f : 0.72f));
        sky.intensity = 0.0f;
        sky.tanSize = 0.03f * std::clamp(s.sunShadowSoftness, 0.0f, 3.0f);
        sky.on = true;
    } else {
        enh::setGrade(sky.gain, sky.lift, sky.saturation, sky.contrast);
        enh::setShafts(sky.shafts * std::clamp(s.sunShaftIntensity, 0.0f, 3.0f) * 0.5f);
    }
    if (!sViewValid || !(s.sunShadows || s.sunLight) || !sky.on) return;
    aurora::gfx::ExtraLight l{};
    if (indoor || s.sunDirection == 1) {
        // The game's own key light (GX_LIGHT0 of the first light set loaded, view space as
        // loaded: a GX directional light is a point ~10^6 units away).
        uint32_t n = 0;
        const enh::GameLight* gl = enh::gameLights(n);
        bool found = false;
        for (uint32_t i = 0; i < n && !found; i++) {
            const auto& g = gl[i];
            const float len = std::sqrt(g.pos[0] * g.pos[0] + g.pos[1] * g.pos[1] + g.pos[2] * g.pos[2]);
            if (g.idMask == 1 && (g.rgba[0] | g.rgba[1] | g.rgba[2]) != 0 && len > 1e4f) {
                for (int r = 0; r < 3; r++) l.pos[r] = g.pos[r] / len;
                found = true;
            }
        }
        if (!found) return;
    } else {
        // The town is a "rolling log": a cylinder about world X, every object placed on it turned
        // with the surface (its g3d world matrix: a house's Y axis was (0, 0.53, 0.85)). The sun's
        // east / up / south are therefore the local frame of the ground in view (sLocalUp, the
        // mean Y axis of the objects drawn last frame), not world X / Y / Z. The camera data is
        // the true world -> view (ScnObj view = camera x world, checked).
        const float* U = sLocalUp;
        const float E[3] = {1.0f, 0.0f, 0.0f};
        const float S[3] = {E[1] * U[2] - E[2] * U[1], E[2] * U[0] - E[0] * U[2], E[0] * U[1] - E[1] * U[0]};
        float w[3];
        for (int k = 0; k < 3; k++) w[k] = sky.dir[0] * E[k] + sky.dir[1] * U[k] + sky.dir[2] * S[k];
        for (int r = 0; r < 3; r++) l.pos[r] = sView[r * 4] * w[0] + sView[r * 4 + 1] * w[1] + sView[r * 4 + 2] * w[2];
    }
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

void submitNightLights();

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
        if (sBeginPending && c.r[5] != 0) {  // r5: the current camera (ScnRoot +0xF4), see cf_tags.cpp
            sBeginPending = false;
            sMainCam = data;
            for (int k = 0; k < 12; k++) sView[k] = rdf32(data + 4 * k);
            sViewValid = std::isfinite(sView[0]) && std::isfinite(sView[11]);
            sViewFrame = copyDispCount();
            sSceneOpen = true;
            submitSky();
            submitNightLights();
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

// ---- night lights ----
// Sources by model name: lit windows of houses and shops (a warm light in front of the house at
// window height), lamps. Collected from the draws of game frame N (their world matrix, g3d
// ScnObj + 0x3C), submitted with frame N+1's scene (the sources do not move). World -> view comes
// from the objects themselves: ScnObj + 0x6C is the view matrix g3d drew the object with, so
// V = view x world^-1 is exactly the transform of the geometry (the camera data's rotation is
// not, see submitSky), and V for the next frame = V x camera^-1(N) x camera(N+1).
struct NightKind {
    const char* prefix;
    float offset[3];  // model space (y up, +z = the model's front)
    float color[3];
    float radius, intensity;
    bool shadow;
};
constexpr NightKind kNightKinds[] = {
    {"obj_myhome", {0.0f, 22.0f, 75.0f}, {1.0f, 0.70f, 0.38f}, 220.0f, 1.4f, true},
    {"obj_house", {0.0f, 22.0f, 70.0f}, {1.0f, 0.70f, 0.38f}, 200.0f, 1.2f, true},
    {"obj_shop", {0.0f, 32.0f, 50.0f}, {1.0f, 0.78f, 0.50f}, 230.0f, 1.3f, true},
    {"obj_museum", {0.0f, 40.0f, 70.0f}, {1.0f, 0.78f, 0.52f}, 260.0f, 1.1f, false},
    {"obj_police", {0.0f, 35.0f, 55.0f}, {1.0f, 0.80f, 0.55f}, 220.0f, 1.1f, false},
    {"obj_townhall", {0.0f, 40.0f, 70.0f}, {1.0f, 0.78f, 0.52f}, 260.0f, 1.1f, false},
    {"obj_busstop", {0.0f, 45.0f, 5.0f}, {1.0f, 0.86f, 0.66f}, 120.0f, 0.9f, false},
    {"obj_lamp", {0.0f, 60.0f, 0.0f}, {1.0f, 0.84f, 0.60f}, 200.0f, 1.2f, true},
    {"obj_light", {0.0f, 60.0f, 0.0f}, {1.0f, 0.84f, 0.60f}, 200.0f, 1.2f, true},
    {"obj_cafe", {0.0f, 32.0f, 50.0f}, {1.0f, 0.74f, 0.44f}, 220.0f, 1.2f, false},
};
struct NightSource {
    float world[3];
    u32 id;
    const NightKind* kind;
};
std::vector<NightSource> sNightCur, sNightNext;  // collected in the frame being drawn / to submit
float sTrueView[12] = {};   // geometry world -> view of the frame the sources were seen in
float sTrueViewCam[12] = {};  // sView of that frame
bool sTrueViewValid = false;

void mul34(const float* a, const float* b, float* o) {  // o = a x b (3x4 affine)
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k];
        o[r * 4 + 3] += a[r * 4 + 3];
    }
}
bool inv34(const float* m, float* o) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float id = 1.0f / det;
    o[0] = (e * i - f * h) * id, o[1] = (c * h - b * i) * id, o[2] = (b * f - c * e) * id;
    o[4] = (f * g - d * i) * id, o[5] = (a * i - c * g) * id, o[6] = (c * d - a * f) * id;
    o[8] = (d * h - e * g) * id, o[9] = (b * g - a * h) * id, o[10] = (a * e - b * d) * id;
    for (int r = 0; r < 3; r++) o[r * 4 + 3] = -(o[r * 4] * m[3] + o[r * 4 + 1] * m[7] + o[r * 4 + 2] * m[11]);
    return true;
}
void readMtx(u32 a, float* m) {
    for (int k = 0; k < 12; k++) m[k] = rdf32(a + 4 * k);
}

void noteNightSource(u32 obj, const std::string& name) {
    const NightKind* kind = nullptr;
    for (const auto& k : kNightKinds)
        if (name.compare(0, std::strlen(k.prefix), k.prefix) == 0) kind = &k;
    if (!kind) return;
    float w[12], v[12];
    readMtx(obj + 0x3C, w);
    readMtx(obj + 0x6C, v);
    float wi[12], tv[12];
    if (!inv34(w, wi)) return;
    mul34(v, wi, tv);
    if (!std::isfinite(tv[3] + tv[7] + tv[11])) return;
    std::memcpy(sTrueView, tv, sizeof tv);
    std::memcpy(sTrueViewCam, sView, sizeof sView);
    sTrueViewValid = true;
    NightSource s{};
    for (int r = 0; r < 3; r++)
        s.world[r] = w[r * 4] * kind->offset[0] + w[r * 4 + 1] * kind->offset[1] + w[r * 4 + 2] * kind->offset[2] + w[r * 4 + 3];
    s.id = 0x57000000u ^ (obj & 0x00FFFFFFu);  // interp keys world lights by "emitter" ids
    s.kind = kind;
    if (sNightCur.size() < 64) sNightCur.push_back(s);
}

// Submits last frame's night sources (guest thread, before enh::beginScene).
void submitNightLights() {
    sNightNext.swap(sNightCur);
    sNightCur.clear();
    const auto& s = port::settings().enhancedLighting;
    if (!s.nightLights || sNight <= 0.01f || !sViewValid || sNightNext.empty()) return;
    const float* v = sView;  // the camera data is the geometry's world -> view (ScnObj view = camera x world)
    std::array<aurora::gfx::ExtraLight, 64> out{};
    uint32_t n = 0;
    for (const auto& src : sNightNext) {
        aurora::gfx::ExtraLight l{};
        for (int r = 0; r < 3; r++) l.pos[r] = v[r * 4] * src.world[0] + v[r * 4 + 1] * src.world[1] + v[r * 4 + 2] * src.world[2] + v[r * 4 + 3];
        std::memcpy(l.color, src.kind->color, sizeof l.color);
        l.radius = src.kind->radius;
        l.intensity = src.kind->intensity * sNight * std::clamp(s.nightLightIntensity, 0.0f, 3.0f);
        l.flags = aurora::gfx::ExtraLightTranslucent | (src.kind->shadow && s.nightLightShadows ? aurora::gfx::ExtraLightCastShadows : 0u);
        l.id = src.id;
        out[n++] = l;
    }
    submitExtraLights(out.data(), n);
    if (logNow() && n)
        log("[night] f%u %u lights (night %.2f), first at view %.1f %.1f %.1f I %.2f r %.0f", copyDispCount(), n, sNight, out[0].pos[0],
            out[0].pos[1], out[0].pos[2], out[0].intensity, out[0].radius);
}
}  // namespace

bool onModel(Ctx& c, u32 obj) {
    if (!sModelLog && !enh::masterOn()) return true;
    const ModelInfo& mi = modelInfo(obj);
    const u32 task = c.r[4];
    if (sModelLog) {
        static std::unordered_map<std::string, u32> sSeen;
        if (sSeen[mi.name]++ < 2)
            log("[model] f%u obj %08X task %u mdl %08X '%s' m3c t %.1f %.1f %.1f  m0c t %.1f %.1f %.1f", copyDispCount(), obj, task,
                mi.mdl, mi.name.c_str(), rdf32(obj + 0x48), rdf32(obj + 0x58), rdf32(obj + 0x68), rdf32(obj + 0x18), rdf32(obj + 0x28),
                rdf32(obj + 0x38));
        if (copyDispCount() % 600 == 0 && task == 0x10002 && mi.name == "obj_myhome0") {
            for (u32 base : {0x0Cu, 0x3Cu, 0x6Cu})
                log("[model] f%u %s +%02X: %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f", copyDispCount(), mi.name.c_str(),
                    base, rdf32(obj + base), rdf32(obj + base + 4), rdf32(obj + base + 8), rdf32(obj + base + 12), rdf32(obj + base + 16),
                    rdf32(obj + base + 20), rdf32(obj + base + 24), rdf32(obj + base + 28), rdf32(obj + base + 32),
                    rdf32(obj + base + 36), rdf32(obj + base + 40), rdf32(obj + base + 44));
            log("[model] f%u camera: %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f / %.3f %.3f %.3f %.1f", copyDispCount(), sView[0], sView[1],
                sView[2], sView[3], sView[4], sView[5], sView[6], sView[7], sView[8], sView[9], sView[10], sView[11]);
        }
    }
    if (!enh::masterOn() || !sInStage || sPass != 1) return true;
    // The game's own round / footprint shadows (sdw_* models, drawn in their own task): kept,
    // hidden while real shadows are strong (fade), or hidden.
    const u32 proc = task & 0xFFFF;  // G3DPROC: 6 draw opa, 7 draw xlu (bit 16 a flag)
    if ((proc == 6 || proc == 7) && mi.name.compare(0, 4, "sdw_") == 0) {
        const int mode = port::settings().enhancedLighting.blobShadows;
        if (mode == 2 || (mode == 1 && sSunStrength > 0.3f)) return false;
    }
    if (proc == 6 && (mi.name.compare(0, 4, "obj_") == 0 || mi.name.compare(0, 4, "sdw_") == 0)) {
        if (copyDispCount() != sUpFrame) {
            const float len = std::sqrt(sUpAcc[0] * sUpAcc[0] + sUpAcc[1] * sUpAcc[1] + sUpAcc[2] * sUpAcc[2]);
            if (sUpCount && len > 1e-3f)
                for (int k = 0; k < 3; k++) sLocalUp[k] = sUpAcc[k] / len;
            sUpAcc[0] = sUpAcc[1] = sUpAcc[2] = 0.0f;
            sUpCount = 0;
            sUpFrame = copyDispCount();
        }
        const float y[3] = {rdf32(obj + 0x3C + 4), rdf32(obj + 0x3C + 20), rdf32(obj + 0x3C + 36)};
        const float yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        if (yl > 1e-3f && std::isfinite(yl)) {
            for (int k = 0; k < 3; k++) sUpAcc[k] += y[k] / yl;
            sUpCount++;
        }
    }
    if (proc == 6 && sNight > 0.01f) noteNightSource(obj, mi.name);
    return true;
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
        const u32 f = VA(0x8016D40C);
        const u32 lis = rd32(f), addi = rd32(f + 4);
        if ((lis >> 26) == 15 && (addi >> 26) == 14) {
            const u32 t = ((lis & 0xFFFF) << 16) + u32(int32_t(int16_t(addi & 0xFFFF)));
            const u32 st = guestCall(c, VA(0x801C9E48), t);
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
