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
#include "game/cf_furniture_light.h"
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

// The rolling log itself (branch interp-town): every placed object stands on a cylinder about the
// world X axis, its Y axis exactly radial (checked with CF_SUN_LOG: house, post office, bulletin
// board, build sites, birds: up = (p - axis) / |p - axis| to 0.01 degrees, axis at y = z = 0 in the
// user's town; radius 670.7 on the main level, 614.7 on the lower terrace, more for flying birds).
// The mean Y axis above changed in a step whenever an object came into or left the view and lagged
// a frame, so the sun turned in jumps as the player walked up or down the log (shadows jumped). The
// axis is fitted from the objects (least squares: each object's up line passes through it), the
// ground radius is the most common object radius, and the local up is the mean of the log's normals
// under rays through the view (visibleGroundUp; shadow-fix): it turns continuously with the camera, like the
// ground under the player. Test aids CF_SUN_LOG=<n> (fit and both ups every n frames, samples every 60),
// CF_SUN_UP=mean (the old mean).
struct LogFit {
    double s11 = 0, s12 = 0, s22 = 0, b1 = 0, b2 = 0;  // normal equations of (cy, cz)
    u32 n = 0, rejected = 0;
    u32 place = ~0u;  // the kind of place the samples came from (town, city)
    bool valid = false;
    float cy = 0, cz = 0, r = 0;
    std::unordered_map<int, u32> radii;  // object radius (whole units) -> samples
};
LogFit sFit;
const u32 sSunLog = [] { const char* v = std::getenv("CF_SUN_LOG"); return v ? u32(std::max(1, std::atoi(v))) : 0u; }();  // every n frames (1 = every frame; samples every 60)
const bool sUpMean = [] {
    const char* v = std::getenv("CF_SUN_UP");
    return v && std::strcmp(v, "mean") == 0;
}();
u32 placeKey() { return (relBase(108, 1) != 0 ? 1u : 0u) | (relBase(109, 1) != 0 ? 2u : 0u) | (relBase(107, 1) != 0 ? 4u : 0u); }
void fitSample(float py, float pz, float uy, float uz) {
    const u32 key = placeKey();
    if (key != sFit.place) sFit = LogFit{}, sFit.place = key;
    const float ul = std::sqrt(uy * uy + uz * uz);
    if (!(ul > 0.5f) || !std::isfinite(py) || !std::isfinite(pz)) return;
    uy /= ul, uz /= ul;
    if (sFit.valid) {  // its up line misses the fitted axis: a prop that is not upright on the log
        const float off = (py - sFit.cy) * uz - (pz - sFit.cz) * uy;
        if (std::fabs(off) > 20.0f) {
            sFit.rejected++;
            return;
        }
    }
    // (p - c) x u = 0 in YZ: cy uz - cz uy = py uz - pz uy.
    const double a0 = uz, a1 = -uy, b = double(py) * uz - double(pz) * uy;
    sFit.s11 += a0 * a0, sFit.s12 += a0 * a1, sFit.s22 += a1 * a1, sFit.b1 += a0 * b, sFit.b2 += a1 * b;
    sFit.n++;
    const double det = sFit.s11 * sFit.s22 - sFit.s12 * sFit.s12;
    if (sFit.n < 16 || !(det > 1e-4 * double(sFit.n) * sFit.n)) return;  // up lines of several directions
    sFit.cy = float((sFit.b1 * sFit.s22 - sFit.b2 * sFit.s12) / det);
    sFit.cz = float((sFit.s11 * sFit.b2 - sFit.s12 * sFit.b1) / det);
    const float r = (py - sFit.cy) * uy + (pz - sFit.cz) * uz;
    if (r > 100.0f && r < 1e6f) {
        const int k = int(std::lround(r));
        const u32 cnt = ++sFit.radii[k];
        // (a clear majority to switch: the ground must not hop between terraces as views change)
        if (!sFit.valid || cnt > 2 * sFit.radii[int(sFit.r)] + 8) sFit.r = float(k);
        sFit.valid = true;
    }
}
// The log's up under the camera's target (world space); false = no fit for this place yet.
bool logUpAt(const float t[3], float up[3]) {
    if (!sFit.valid || sFit.place != placeKey() || sUpMean) return false;
    const float y = t[1] - sFit.cy, z = t[2] - sFit.cz, l = std::sqrt(y * y + z * z);
    if (!(l > 1.0f)) return false;
    up[0] = 0.0f, up[1] = y / l, up[2] = z / l;
    return true;
}
u32 sFitWanted = 0x80000000u;  // copyDispCount() of the last wantGroundFit() (Enhanced world)
}  // namespace

void wantGroundFit() { sFitWanted = copyDispCount(); }

bool logAxis(float& cy, float& cz, float& radius) {
    if (!sFit.valid || sFit.place != placeKey()) return false;
    cy = sFit.cy, cz = sFit.cz, radius = sFit.r;
    return true;
}

namespace {
// Where the view-space ray `vd` from the camera meets the log's ground (g3d world).
// horizon: a ray that misses the log gives the point where it passes closest to the axis (the
// horizon it looks over: continuous with the hits as a ray rises past the horizon).
bool rayHitLog(const float view[12], const float vd[3], float out[3], bool horizon = false) {
    if (!sFit.valid || sFit.place != placeKey()) return false;
    // Camera position C = -R^T t, the ray R^T vd (g3d world, the log's frame).
    float C[3], d[3];
    for (int k = 0; k < 3; k++) {
        C[k] = -(view[k] * view[3] + view[4 + k] * view[7] + view[8 + k] * view[11]);
        d[k] = view[k] * vd[0] + view[4 + k] * vd[1] + view[8 + k] * vd[2];
    }
    // |(C + s d)_yz - centre| = R, the first hit from outside.
    const float oy = C[1] - sFit.cy, oz = C[2] - sFit.cz;
    const float a = d[1] * d[1] + d[2] * d[2], b = 2.0f * (oy * d[1] + oz * d[2]), cc = oy * oy + oz * oz - sFit.r * sFit.r;
    const float disc = b * b - 4.0f * a * cc;
    if (horizon && a > 1e-6f && cc > 0.0f && disc < 0.0f) {
        const float s = std::max(-b / (2.0f * a), 0.0f);
        for (int k = 0; k < 3; k++) out[k] = C[k] + s * d[k];
        return true;
    }
    if (!(a > 1e-6f) || !(disc >= 0.0f) || !(cc > 0.0f)) return false;
    const float sq = std::sqrt(disc);
    float s = (-b - sq) / (2.0f * a);
    if (!(s > 0.0f)) s = (-b + sq) / (2.0f * a);
    if (!(s > 0.0f) || !std::isfinite(s)) return false;
    for (int k = 0; k < 3; k++) out[k] = C[k] + s * d[k];
    return true;
}
}  // namespace

bool groundAhead(const float view[12], float out[3]) {
    static const float kFwd[3] = {0.0f, 0.0f, -1.0f};
    return rayHitLog(view, kFwd, out);
}

namespace {
// The sun frame's up (shadow-fix): the mean of the log's normals where rays through five rows of
// the view's centre column meet the ground: the visible ground as a whole, like the old mean of the
// visible objects' axes (which the look was tuned with), but continuous. The up at the ground
// ahead alone (interp-town) leaned ~20-30 degrees further than the old mean, so a low sun (the
// 12-degree floor: late afternoon, the user's report at 16:57) grazed the ground there and the
// shadows were gone, flickering in only in frames without a fit (then the old mean was used).
bool visibleGroundUp(const float view[12], u32 cam, float up[3]) {
    float fovy = cam ? rdf32(cam + 0xAC) : 60.0f;  // CameraData fovy (degrees; 60 in the town)
    if (!(fovy > 10.0f && fovy < 120.0f)) fovy = 60.0f;
    const float t = std::tan(fovy * 0.5f * 3.14159265f / 180.0f);
    float acc[3] = {0.0f, 0.0f, 0.0f};
    int n = 0;
    for (const float row : {0.1f, 0.3f, 0.5f, 0.7f, 0.9f}) {  // 0 top .. 1 bottom
        const float vd[3] = {0.0f, (1.0f - 2.0f * row) * t, -1.0f};
        float p[3];
        if (!rayHitLog(view, vd, p, true)) continue;
        const float y = p[1] - sFit.cy, z = p[2] - sFit.cz, l = std::sqrt(y * y + z * z);
        if (!(l > 1.0f)) continue;
        acc[1] += y / l, acc[2] += z / l, n++;
    }
    const float l = std::sqrt(acc[1] * acc[1] + acc[2] * acc[2]);
    if (n < 2 || !(l > 1e-3f)) return false;
    up[0] = 0.0f, up[1] = acc[1] / l, up[2] = acc[2] / l;
    return true;
}

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
        // The cool night look leaves the light sources (lit windows, street lamps, signs) their
        // warm colour (Aurora 0421; branch enh-light-3).
        enh::setGradeKeepLights(0.85f * sNight);
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
        float fitUp[3] = {0.0f, 0.0f, 0.0f}, ground[3] = {0.0f, 0.0f, 0.0f};
        // (CameraData's position / target fields are not set by the game: the view matrix is.)
        // The visible ground's mean normal (visibleGroundUp); a frame without it holds the last
        // one for a while (shadow-fix: such frames fell back to the old mean, and the shadows
        // flickered in for a frame). CF_SUN_UP=ahead: the normal at the ground ahead (interp-town).
        static const bool sUpAhead = [] {
            const char* v = std::getenv("CF_SUN_UP");
            return v && std::strcmp(v, "ahead") == 0;
        }();
        static float sHeldUp[3] = {0.0f, 0.0f, 0.0f};
        static u32 sHeldFrame = 0, sHeldPlace = ~0u;
        bool fitted = false;
        if (!sUpMean) {
            const bool ahead = groundAhead(sView, ground) && logUpAt(ground, fitUp);
            if (!sUpAhead) fitted = visibleGroundUp(sView, sMainCam, fitUp);
            else fitted = ahead;
            if (fitted) {
                std::memcpy(sHeldUp, fitUp, sizeof sHeldUp);
                sHeldFrame = copyDispCount(), sHeldPlace = placeKey();
            } else if (sHeldPlace == placeKey() && copyDispCount() - sHeldFrame < 600) {
                std::memcpy(fitUp, sHeldUp, sizeof fitUp);
                fitted = true;
            }
        }
        if (fitted) U = fitUp;
        if (sSunLog && copyDispCount() % sSunLog == 0)
            log("[sun] f%u fit %s cy %.1f cz %.1f R %.1f n %u rejected %u | ground %.1f %.1f %.1f | up fit %.4f %.4f %.4f mean %.4f %.4f %.4f cam %.4f %.4f",
                copyDispCount(), fitted ? "used" : "none", sFit.cy, sFit.cz, sFit.r, sFit.n, sFit.rejected, fitted ? ground[0] : 0.0f,
                fitted ? ground[1] : 0.0f, fitted ? ground[2] : 0.0f, fitted ? fitUp[0] : 0.0f, fitted ? fitUp[1] : 0.0f,
                fitted ? fitUp[2] : 0.0f, sLocalUp[0], sLocalUp[1], sLocalUp[2], sView[5], sView[6]);
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
    {
        // Aurora 0441: the shadow cascades' texel grid fixed in the world (world X axis as the
        // reference, the world origin as the anchor), not moving with the camera (branch
        // interp-town: shadow edges crawled as the camera moved). CF_SUN_GRID=view: the old layout.
        static const bool sViewGrid = [] {
            const char* v = std::getenv("CF_SUN_GRID");
            return v && std::strcmp(v, "view") == 0;
        }();
        if (!sViewGrid) {
            const float ref[3] = {sView[0], sView[4], sView[8]};  // world X in view space
            const float origin[3] = {sView[3], sView[7], sView[11]};
            l.box[7] = ref[0], l.box[8] = ref[1], l.box[9] = ref[2];
            aurora::gfx::dir_light_anchor(l.pos, ref, origin, l.box + 10);
        }
    }
    if (!s.sunShadows) l.box[0] = l.box[1] = l.box[2] = 1.0f;
    submitExtraLights(&l, 1);
    // Shadows of objects outside the view (cf_shadow_casters.cpp): the light's direction in the g3d
    // world, how steep it is over the ground in view, how far its shadows are computed.
    if (s.sunShadows && !indoor && sky.strength > 0.002f) {
        float w[3];
        for (int k = 0; k < 3; k++) w[k] = sView[k] * l.pos[0] + sView[4 + k] * l.pos[1] + sView[8 + k] * l.pos[2];
        const float wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (wl > 1e-4f) {
            for (float& v : w) v /= wl;
            // Elevation over the local ground: the time-of-day light is held >= 12 degrees up in the
            // ground's frame (sky.dir[1]); the game's key light against the mean object up.
            // Its direction in the ground's own frame (east = world X, up, south = east x up) for the
            // game's flat-coordinate tests (structures).
            float local[3] = {sky.dir[0], sky.dir[1], sky.dir[2]};
            if (s.sunDirection == 1) {
                const float* U = sLocalUp;
                const float S[3] = {0.0f, -U[2], U[1]};
                local[0] = w[0], local[1] = w[0] * U[0] + w[1] * U[1] + w[2] * U[2], local[2] = w[0] * S[0] + w[1] * S[1] + w[2] * S[2];
                const float hl = std::sqrt(local[0] * local[0] + local[2] * local[2]);
                if (local[1] < 0.2079f && hl > 1e-4f)  // held >= 12 degrees up, like the time-of-day light
                    local[0] = local[0] / hl * 0.9781f, local[2] = local[2] / hl * 0.9781f, local[1] = 0.2079f;
            }
            const float sinEl = std::clamp(local[1], 0.2079f, 1.0f);  // sin 12 deg
            castersSetSun(w, local, sinEl, std::min(l.radius, l.box[6]), copyDispCount());
        }
    }
    if (logNow())
        log("[sky] f%u %02d:%02d yday %d weather %d dir %.2f %.2f %.2f view %.2f %.2f %.2f tint %.2f %.2f %.2f I %.2f soft %.3f",
            copyDispCount(), sClock.hour, sClock.min, sClock.yday, sWeather, sky.dir[0], sky.dir[1], sky.dir[2], l.pos[0], l.pos[1],
            l.pos[2], sky.tint[0], sky.tint[1], sky.tint[2], sky.intensity, sky.tanSize);
}

void submitNightLights();
void submitEffectLights();

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
            submitEffectLights();
            cffurn::submit(sView, sViewValid);  // furniture lights (cf_furniture_light.cpp)
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

bool mainView(float view[12]) {
    if (!sViewValid) return false;
    std::memcpy(view, sView, sizeof sView);
    return true;
}
std::string modelName(u32 scnObj) { return modelInfo(scnObj).name; }

namespace {

// ---- night lights ----
// Sources by model name: lit windows of houses and shops (a warm light in front of the building at
// window height), lamps. Collected from the draws of game frame N (their world matrix, g3d
// ScnObj + 0x3C), submitted with frame N+1's scene (the sources do not move); world -> view is the
// main camera (ScnObj view = camera x world, checked).
//
// Whether a window is lit is the game's own state: every structure with lit materials (m_light,
// m_window, m_spotlight, m_museum, ... : d_a_strNP's material table) has a daSTR_c light
// controller, and d_a_strNP fn_99_A40 sets those materials' TEV colour 1 from the controller's
// light amount (controller + 0, 0..1; ScnMdl at + 0x2C). The actors decide the amount (villager
// home and awake, shop open, someone in the player's house, ...); the hook records it per ScnMdl
// and the light follows it ("Follow the game's lights"). Kinds without a controller (bus stop,
// lamps) glow from dusk to dawn.
struct NightKind {
    const char* prefix;
    float offset[3];  // model space (y up, +z = the model's front)
    float color[3];
    float radius, intensity;
    bool shadow;
};
constexpr NightKind kNightKinds[] = {
    {"obj_myhome", {0.0f, 22.0f, 75.0f}, {1.0f, 0.70f, 0.38f}, 220.0f, 1.4f, true},   // players' houses
    {"obj_house", {0.0f, 22.0f, 70.0f}, {1.0f, 0.70f, 0.38f}, 200.0f, 1.2f, true},    // villagers' houses
    {"obj_nc", {0.0f, 30.0f, 55.0f}, {1.0f, 0.78f, 0.50f}, 230.0f, 1.3f, true},       // Nook's Cranny
    {"obj_nk", {0.0f, 34.0f, 65.0f}, {1.0f, 0.80f, 0.55f}, 260.0f, 1.3f, true},       // Nook 'n' Go
    {"obj_nw", {0.0f, 36.0f, 70.0f}, {1.0f, 0.80f, 0.55f}, 270.0f, 1.3f, true},       // Nookway
    {"obj_ng", {0.0f, 40.0f, 75.0f}, {1.0f, 0.80f, 0.55f}, 280.0f, 1.3f, true},       // Nookington's
    {"obj_tailor", {0.0f, 30.0f, 55.0f}, {1.0f, 0.76f, 0.52f}, 230.0f, 1.2f, true},   // Able Sisters
    {"obj_office", {0.0f, 40.0f, 70.0f}, {1.0f, 0.78f, 0.52f}, 260.0f, 1.1f, false},  // town hall
    {"obj_museum", {0.0f, 40.0f, 70.0f}, {1.0f, 0.78f, 0.52f}, 260.0f, 1.1f, false},
    {"obj_lighthouse", {0.0f, 120.0f, 0.0f}, {1.0f, 0.88f, 0.66f}, 300.0f, 1.0f, false},
    {"obj_windmill", {0.0f, 30.0f, 40.0f}, {1.0f, 0.76f, 0.48f}, 180.0f, 1.0f, false},
    {"obj_busstop", {0.0f, 45.0f, 5.0f}, {1.0f, 0.86f, 0.66f}, 120.0f, 0.9f, false},
    {"obj_lamp", {0.0f, 60.0f, 0.0f}, {1.0f, 0.84f, 0.60f}, 200.0f, 1.2f, true},
    {"obj_light", {0.0f, 60.0f, 0.0f}, {1.0f, 0.84f, 0.60f}, 200.0f, 1.2f, true},
};
struct NightSource {
    float world[3];
    float glow;  // the game's light amount (1 for kinds without a controller)
    u32 id;
    const NightKind* kind;
};
std::vector<NightSource> sNightCur, sNightNext;  // collected in the frame being drawn / to submit
constexpr size_t kMaxNightSources = 128;         // collected; the nearest 64 are submitted

void readMtx(u32 a, float* m) {
    for (int k = 0; k < 12; k++) m[k] = rdf32(a + 4 * k);
}

// The game's own light amount per structure model (daSTR_c light controller, fn_99_A40).
// Test aid CF_WINDOW_LOG=1: every change.
const bool sWindowLog = std::getenv("CF_WINDOW_LOG") != nullptr;
std::unordered_map<u32, float> sGlow;  // ScnMdl -> light amount 0..1
void noteGlow(u32 mdl, float amount) {
    if (!std::isfinite(amount)) return;
    amount = std::clamp(amount, 0.0f, 1.0f);
    auto [it, fresh] = sGlow.try_emplace(mdl, amount);
    if (sWindowLog && (fresh || std::fabs(it->second - amount) > 0.1f || (amount == 0.0f) != (it->second == 0.0f)))
        log("[window] f%u glow %08X '%s' %.2f -> %.2f at %.1f %.1f %.1f", copyDispCount(), mdl, modelInfo(mdl).name.c_str(),
            fresh ? -1.0f : it->second, amount, rdf32(mdl + 0x48), rdf32(mdl + 0x58), rdf32(mdl + 0x68));
    if (sWindowLog) {
        static u32 sCalls = 0, sFrame = 0;
        sCalls++;
        if (copyDispCount() / 600 != sFrame) {
            sFrame = copyDispCount() / 600;
            log("[window] f%u %u light-controller calls since the last report, %zu models", copyDispCount(), sCalls, sGlow.size());
            sCalls = 0;
        }
    }
    it->second = amount;
    if (sGlow.size() > 4096) sGlow.clear();
}

bool noteCityNightSource(u32 obj, const std::string& name);  // city section below

void noteNightSource(u32 obj, const std::string& name) {
    if (noteCityNightSource(obj, name)) return;
    const NightKind* kind = nullptr;
    for (const auto& k : kNightKinds)
        if (name.compare(0, std::strlen(k.prefix), k.prefix) == 0) kind = &k;
    if (!kind) return;
    NightSource s{};
    s.glow = 1.0f;
    if (port::settings().enhancedLighting.nightLightsGameState)
        if (auto it = sGlow.find(obj); it != sGlow.end()) s.glow = it->second;
    if (s.glow <= 0.01f) return;
    float w[12];
    readMtx(obj + 0x3C, w);
    for (int r = 0; r < 3; r++)
        s.world[r] = w[r * 4] * kind->offset[0] + w[r * 4 + 1] * kind->offset[1] + w[r * 4 + 2] * kind->offset[2] + w[r * 4 + 3];
    if (!std::isfinite(s.world[0] + s.world[1] + s.world[2])) return;
    s.id = 0x57000000u ^ (obj & 0x00FFFFFFu);  // interp keys world lights by "emitter" ids
    s.kind = kind;
    if (sNightCur.size() < kMaxNightSources) sNightCur.push_back(s);
}

// ---- city night lights (branch enh-light-3) ----------------------------------------------------
// The city's structures (d_a_str_townNP, models obj_tw_* from Str/Brres/town*StrBrres*.arc, the
// bus stop obj_busstop1, Kapp'n's bus obj_bus) have the same daSTR_c light controller as the
// town's: d_a_strNP fn_99_A40 lerps the TEV colour 1 of up to 17 materials by the controller's
// light amount (controller + 0), and the controller names those materials itself: controller
// + 0x14 + i (s8, -1 = none) is the model's material index for entry i of the material table
// (m_letter .. m_tw_light_07). So the lights are placed from the model's own data: for every lit
// material, the shapes drawn with it (the ResMdl's DrawOpa / DrawXlu byte code) and the bounding
// boxes of their position arrays give the glowing part in model space; flat parts (windows, signs)
// get a light just in front of them, compact ones (lamp heads) one just below. Parts covering most
// of the building (the material of a whole facade that the game also tints) give no light of their
// own. The light amount is the game's (shops close, lamps follow the evening).
struct LitPart {
    float pos[3];  // model space
    float size;    // largest extent
    bool lamp;     // compact part (a lamp head): light below it
};
struct LitModel {
    std::vector<LitPart> parts;
    bool done = false;
};
std::unordered_map<u32, std::array<s8, 17>> sLitMats;  // ScnMdl -> lit material indices (controller + 0x14)
std::unordered_map<u64, LitModel> sLitModels;          // (ResMdl, material set hash) -> parts
const int sCityLog = [] {
    const char* v = std::getenv("CF_CITY_LIGHT_LOG");  // test aid: lit parts per model, lights
    return v ? std::max(std::atoi(v), 1) : 0;
}();

void noteLitMaterials(u32 mdl, u32 ctl) {
    std::array<s8, 17> m{};
    for (u32 i = 0; i < 17; i++) m[i] = s8(rd8(ctl + 0x14 + i));
    sLitMats[mdl] = m;
    if (sLitMats.size() > 4096) sLitMats.clear();
}

// A ResDic (g3d index group) entry by index: the data pointer (group-relative offsets).
u32 resDicData(u32 group, u32 index) {
    if (!guestPtr(group)) return 0;
    const u32 n = rd32(group + 4);
    if (index >= n) return 0;
    const s32 off = s32(rd32(group + 8 + 16 * (index + 1) + 12));
    return off ? group + u32(off) : 0;
}
u32 resDicFind(u32 group, const char* name) {
    if (!guestPtr(group)) return 0;
    const u32 n = std::min(rd32(group + 4), 256u);
    for (u32 i = 1; i <= n; i++) {
        const u32 e = group + 8 + 16 * i;
        const s32 no = s32(rd32(e + 8));
        if (!no) continue;
        bool same = true;
        for (u32 k = 0; same; k++) {
            const char ch = char(rd8(group + u32(no) + k));
            same = ch == name[k];
            if (!ch || k > 32) break;
        }
        if (same) return group + u32(s32(rd32(e + 12)));
    }
    return 0;
}

const LitModel& litModel(u32 resMdl, const std::array<s8, 17>& mats) {
    u64 key = u64(resMdl) << 32;
    for (s8 m : mats) key = key * 31 + u8(m);
    LitModel& lm = sLitModels[key];
    if (lm.done) return lm;
    lm.done = true;
    if (sLitModels.size() > 1024) sLitModels.clear();
    const u32 rev = rd32(resMdl + 8);
    const u32 ns = rev >= 10 ? 14 : 11;
    auto sec = [&](u32 i) -> u32 {
        const s32 o = s32(rd32(resMdl + 0x10 + 4 * i));
        return o > 0 ? resMdl + u32(o) : 0;
    };
    const u32 defs = sec(0), boneSec = sec(1), posSec = sec(2), shpSec = sec(10);
    (void)ns;
    // Bounding box of a shape's positions in model space: a rigid shape's positions are in its
    // node's space, turned into the model by the node's bind matrix (ResNode + 0x70, MTX34; e.g. the
    // city lamp's glow sphere, a unit sphere scaled 25 at the lamp head).
    auto shapeBox = [&](u32 shp, u32 node, float lo[3], float hi[3]) {
        const u32 s = resDicData(shpSec, shp);
        if (!s) return false;
        const s16 pid = s16(rd16(s + 0x48));
        const u32 p = pid >= 0 ? resDicData(posSec, u32(pid)) : 0;
        if (!p) return false;
        float M[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (const u32 b = node != ~0u ? resDicData(boneSec, node) : 0) {
            float B[12];
            readMtx(b + 0x70, B);
            if (std::isfinite(B[0] + B[5] + B[10] + B[3] + B[7] + B[11])) std::memcpy(M, B, sizeof M);
        }
        float bl[3], bh[3];
        for (int k = 0; k < 3; k++) bl[k] = rdf32(p + 0x20 + 4 * k), bh[k] = rdf32(p + 0x2C + 4 * k);
        for (int corner = 0; corner < 8; corner++) {
            const float v[3] = {corner & 1 ? bh[0] : bl[0], corner & 2 ? bh[1] : bl[1], corner & 4 ? bh[2] : bl[2]};
            for (int r = 0; r < 3; r++) {
                const float w = M[r * 4] * v[0] + M[r * 4 + 1] * v[1] + M[r * 4 + 2] * v[2] + M[r * 4 + 3];
                lo[r] = std::min(lo[r], w), hi[r] = std::max(hi[r], w);
            }
        }
        return true;
    };
    // Whole-model box (every drawn shape), and per lit material the union of its shapes.
    float mlo[3] = {1e30f, 1e30f, 1e30f}, mhi[3] = {-1e30f, -1e30f, -1e30f};
    struct Acc {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        bool any = false;
    };
    std::array<Acc, 17> acc{};
    for (const char* list : {"DrawOpa", "DrawXlu"}) {
        u32 o = resDicFind(defs, list);
        for (int guard = 0; o && guard < 512 && rd8(o) == 4; guard++, o += 8) {
            const u32 mat = rd16(o + 1), shp = rd16(o + 3), node = rd16(o + 5);
            shapeBox(shp, node, mlo, mhi);
            for (u32 e = 0; e < 17; e++)
                if (mats[e] >= 0 && u32(mats[e]) == mat) acc[e].any |= shapeBox(shp, node, acc[e].lo, acc[e].hi);
        }
    }
    if (!(mhi[0] > mlo[0])) return lm;
    float mext[3], mc[3];
    for (int k = 0; k < 3; k++) mext[k] = mhi[k] - mlo[k], mc[k] = 0.5f * (mlo[k] + mhi[k]);
    for (u32 e = 0; e < 17; e++) {
        const Acc& a = acc[e];
        if (!a.any) continue;
        float ext[3], c[3];
        for (int k = 0; k < 3; k++) ext[k] = a.hi[k] - a.lo[k], c[k] = 0.5f * (a.lo[k] + a.hi[k]);
        // A part that spans most of the building in both horizontal directions is a facade tint.
        const bool whole = ext[0] > 0.6f * mext[0] && ext[2] > 0.6f * mext[2] && mext[0] > 40.0f;
        LitPart p{};
        p.size = std::max({ext[0], ext[1], ext[2]});
        // Flat part (a window pane, a sign): thin in x or z. The light goes a little in front of it,
        // on the side away from the model's centre.
        const int thin = ext[0] < ext[2] ? 0 : 2;
        const float other = thin == 0 ? ext[2] : ext[0];
        const bool flat = ext[thin] < 0.2f * std::max(other, 1.0f) && other > 8.0f;
        std::memcpy(p.pos, c, sizeof c);
        if (whole && !flat) {
            if (sCityLog) log("[citylight] mdl %08X entry %u: whole-building part (%.0f x %.0f x %.0f), no light", resMdl, e, ext[0], ext[1], ext[2]);
            continue;
        }
        if (flat) {
            float d = c[thin] - mc[thin];
            if (std::fabs(d) < 1.0f) d = 1.0f;
            p.pos[thin] += (d > 0 ? 1.0f : -1.0f) * (0.5f * ext[thin] + 14.0f);
        } else if (p.size < 40.0f) {
            p.lamp = true;  // lamp head: just below it (lights the street, not hidden in the glass)
            p.pos[1] = a.lo[1] - 4.0f;
        } else {
            // A lit volume (a shop's glowing front block): in front of its +z face, as the town's
            // houses (+z = the model's front).
            p.pos[2] = a.hi[2] + 14.0f;
        }
        // Merge with a part nearby (several materials of one window).
        bool merged = false;
        for (auto& q : lm.parts) {
            const float dx = q.pos[0] - p.pos[0], dy = q.pos[1] - p.pos[1], dz = q.pos[2] - p.pos[2];
            if (dx * dx + dy * dy + dz * dz < 40.0f * 40.0f) {
                for (int k = 0; k < 3; k++) q.pos[k] = 0.5f * (q.pos[k] + p.pos[k]);
                q.size = std::max(q.size, p.size);
                merged = true;
                break;
            }
        }
        if (!merged && lm.parts.size() < 3) lm.parts.push_back(p);
        if (sCityLog)
            log("[citylight] mdl %08X entry %u mat %d: part %.0f x %.0f x %.0f at %.1f %.1f %.1f -> light %.1f %.1f %.1f (%s)", resMdl, e,
                mats[e], ext[0], ext[1], ext[2], c[0], c[1], c[2], p.pos[0], p.pos[1], p.pos[2], flat ? "flat" : p.lamp ? "lamp" : "volume");
    }
    return lm;
}

// ---- lit materials are light sources, not shadow receivers (branch enh-light-3) ----------------
// At night a lit window / sign / lamp head is the game's glow (TEV colour 1 lerped to the light
// colour by the controller), but the enhanced world shaders darkened it like any surface: the
// moon's shadow and its terminator (a shop doorway facing away from the moon lost half its glow),
// AO. The lit materials are known from fn_99_A40 itself: the ResMatTevColor it writes (copied
// material data of that ScnMdl) is recorded with the light amount, and when g3d loads that
// material for drawing (G3DState::LoadResMatTevColor) the draw gets Aurora's emissive hint.
std::unordered_map<u32, float> sLitTev;  // ResMatTevColorData -> light amount
bool sInStrLight = false;
float sStrAmount = 0.0f;
void noteLitTev(u32 data) {
    if (!sInStrLight || !data) return;
    sLitTev[data] = sStrAmount;
    if (sLitTev.size() > 8192) sLitTev.clear();
}
}  // namespace
// Furniture glow parts (cf_furniture_light.cpp: lp_m / tv_m / ... at their game amount) are lit
// materials too.
void markLitMaterial(u32 tevColorData, float amount) {
    if (!tevColorData) return;
    sLitTev[tevColorData] = amount;
    if (sLitTev.size() > 8192) sLitTev.clear();
}
namespace {
void onLoadTevColor(u32 data) {
    if (!sInStage || sPass != 1 || sInPass2) return;
    const auto it = sLitTev.find(data);
    const bool lit = it != sLitTev.end() && it->second > 0.05f;
    enh::setEmissive(lit);
    if (sCityLog) {
        static u32 sFrame = 0, sLit = 0, sAll = 0;
        if (copyDispCount() != sFrame) {
            if (copyDispCount() % 600 == 0) log("[citylight] f%u lit materials drawn %u of %u (%zu known)", sFrame, sLit, sAll, sLitTev.size());
            sFrame = copyDispCount(), sLit = sAll = 0;
        }
        sAll++, sLit += lit;
    }
}

// Colour / reach by model (our numbers): warm shop windows, a cooler white for the street lamps.
constexpr NightKind kCityKinds[] = {
    {"obj_tw_lamp", {}, {1.0f, 0.86f, 0.62f}, 230.0f, 1.3f, true},
    {"obj_tw_", {}, {1.0f, 0.76f, 0.48f}, 210.0f, 1.2f, true},
    {"obj_busstop1", {}, {1.0f, 0.86f, 0.66f}, 140.0f, 0.9f, false},
};

bool noteCityNightSource(u32 obj, const std::string& name) {
    const NightKind* kind = nullptr;
    for (const auto& k : kCityKinds)
        if (!kind && name.compare(0, std::strlen(k.prefix), k.prefix) == 0) kind = &k;
    if (!kind) return false;
    const auto lit = sLitMats.find(obj);
    if (lit == sLitMats.end()) return true;  // no light controller: nothing of it glows
    float glow = 1.0f;
    if (auto it = sGlow.find(obj); it != sGlow.end()) glow = it->second;
    if (!port::settings().enhancedLighting.nightLightsGameState) glow = 1.0f;
    if (glow <= 0.01f) return true;
    const LitModel& lm = litModel(modelInfo(obj).mdl, lit->second);
    float w[12];
    readMtx(obj + 0x3C, w);
    u32 part = 0;
    for (const auto& p : lm.parts) {
        NightSource s{};
        for (int r = 0; r < 3; r++) s.world[r] = w[r * 4] * p.pos[0] + w[r * 4 + 1] * p.pos[1] + w[r * 4 + 2] * p.pos[2] + w[r * 4 + 3];
        if (!std::isfinite(s.world[0] + s.world[1] + s.world[2])) continue;
        s.glow = glow;
        s.id = 0x56000000u ^ ((obj & 0x000FFFFFu) << 2) ^ part++;
        s.kind = kind;
        if (sNightCur.size() < kMaxNightSources) sNightCur.push_back(s);
    }
    if (sCityLog && copyDispCount() % 600 == 0)
        log("[citylight] f%u %08X '%s' glow %.2f, %zu lights", copyDispCount(), obj, name.c_str(), glow, lm.parts.size());
    return true;
}

void toView(const float* world, float* view) {
    const float* v = sView;  // the camera data is the geometry's world -> view (ScnObj view = camera x world)
    for (int r = 0; r < 3; r++) view[r] = v[r * 4] * world[0] + v[r * 4 + 1] * world[1] + v[r * 4 + 2] * world[2] + v[r * 4 + 3];
}

// Submits last frame's night sources (guest thread, before enh::beginScene).
void submitNightLights() {
    sNightNext.swap(sNightCur);
    sNightCur.clear();
    if (sWindowLog && copyDispCount() % 1200 == 0)
        for (const auto& [m, a] : sGlow)
            log("[window] f%u   %08X '%s' light %.2f, world %.1f %.1f %.1f", copyDispCount(), m, modelInfo(m).name.c_str(), a, rdf32(m + 0x48),
                rdf32(m + 0x58), rdf32(m + 0x68));
    const auto& s = port::settings().enhancedLighting;
    if (!s.nightLights || sNight <= 0.01f || !sViewValid || sNightNext.empty()) return;
    std::array<aurora::gfx::ExtraLight, 64> out{};
    uint32_t n = 0;
    if (sNightNext.size() > out.size()) {
        // More sources than Aurora's lights (the city: shop windows + street lamps): the nearest.
        auto dist = [](const NightSource& a) {
            float v[3];
            toView(a.world, v);
            return v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        };
        std::sort(sNightNext.begin(), sNightNext.end(), [&](const NightSource& a, const NightSource& b) { return dist(a) < dist(b); });
        sNightNext.resize(out.size());
    }
    for (const auto& src : sNightNext) {
        aurora::gfx::ExtraLight l{};
        toView(src.world, l.pos);
        std::memcpy(l.color, src.kind->color, sizeof l.color);
        l.radius = src.kind->radius;
        l.intensity = src.kind->intensity * sNight * src.glow * std::clamp(s.nightLightIntensity, 0.0f, 3.0f);
        l.flags = aurora::gfx::ExtraLightTranslucent | (src.kind->shadow && s.nightLightShadows ? aurora::gfx::ExtraLightCastShadows : 0u);
        l.id = src.id;
        out[n++] = l;
    }
    submitExtraLights(out.data(), n);
    if (logNow() && n)
        log("[night] f%u %u lights (night %.2f), first at view %.1f %.1f %.1f I %.2f r %.0f", copyDispCount(), n, sNight, out[0].pos[0],
            out[0].pos[1], out[0].pos[2], out[0].intensity, out[0].radius);
}

// ---- effect lights: fireflies and fireworks ----
// nw4r::ef: DrawOrder::Draw(Effect*, DrawInfo&) draws an effect's ParticleManagers (ut::List at
// Effect + 0x90, link offset at list + 0xA). A ParticleManager: + 0x24 its EmitterResource (whose
// first word points at the emitter's name, e.g. af_ins_hotaru_glow), + 0x38 its particles (ut::List
// of 0xE4-byte Particles, link at + 0x14), + 0x8C the emitter's world matrix (MTX34). A Particle:
// + 0x20 its primary colour (RGBA: the colour animation and the alpha fade / blink are applied
// there every frame; + 0x24 the secondary colour, + 0x28 a modulation colour, usually white),
// + 0xA0 velocity, + 0xAC position (emitter space), + 0xDC age (u16). One source per
// ParticleManager at the brightness-weighted centre of its particles, in their mean colour;
// sources of one kind closer than a merge distance become one light (a firefly is two emitters,
// glow + glow_child, at one place; a burst several). Collected while frame N's effects draw,
// submitted with frame N + 1 (like the night lights).
const bool sEflLog = std::getenv("CF_EFL_LOG") != nullptr;  // test aid: emitters and effect lights
std::string readStr(u32 s, u32 max = 48) {
    std::string r;
    if (!guestPtr(s)) return r;
    for (u32 k = 0; k < max; k++) {
        const char ch = char(rd8(s + k));
        if (!ch) break;
        r += (ch >= 32 && ch < 127) ? ch : '?';
    }
    return r;
}
enum FxKind : u8 { FxNone = 0, FxFirefly, FxFirework, FxTest };
std::unordered_map<u32, u8> sResKind;  // EmitterResource -> FxKind
u8 fxKindOf(u32 pm) {
    const u32 res = rd32(pm + 0x24);
    if (!guestPtr(res) || (res & 3)) return FxNone;
    if (auto it = sResKind.find(res); it != sResKind.end()) return it->second;
    const std::string n = readStr(rd32(res), 40);
    u8 k = FxNone;
    if (n.rfind("af_ins_hotaru", 0) == 0 || n.rfind("af_hotaru", 0) == 0) k = FxFirefly;
    else if (n.rfind("af_fld_firework", 0) == 0 || n.rfind("af_itm_firework", 0) == 0) k = FxFirework;
    // Test aid CF_EFL_AS=<emitter name prefix>: that emitter's particles light like fireflies (to
    // check light placement against an effect that is easy to see, e.g. af_fsh_shadow).
    static const char* sAs = std::getenv("CF_EFL_AS");
    if (sAs && *sAs && n.rfind(sAs, 0) == 0) k = FxTest;
    if (sResKind.size() > 4096) sResKind.clear();
    sResKind[res] = k;
    if (sEflLog) log("[efl] f%u emitter %08X '%s' kind %u", copyDispCount(), res, n.c_str(), k);
    return k;
}
struct FxSource {
    float pos[3];    // world (world == true) or view space of the main camera (backdrop effects)
    float color[3];  // 0..1
    float amount;    // brightness: alpha x max channel, summed over the manager's particles
    float spread;    // half the extent of the particle cloud (units of its space)
    u32 id;
    u8 kind;
    bool world;
};
std::unordered_map<u32, FxSource> sFxCur;  // ParticleManager -> source (this frame's draws)
std::vector<FxSource> sFxNext;

// One colour layer of a particle (RGBA) -> weighted colour and brightness.
void addLayer(u32 rgba, float* col, float& bright) {
    const float c[3] = {float(rgba >> 24) / 255.0f, float((rgba >> 16) & 0xFF) / 255.0f, float((rgba >> 8) & 0xFF) / 255.0f};
    const float a = float(rgba & 0xFF) / 255.0f;
    for (int k = 0; k < 3; k++) col[k] += c[k] * a;
    bright = std::max(bright, a * std::max({c[0], c[1], c[2]}));
}

void noteEffectDraw(u32 effect, u32 drawInfo) {
    const auto& s = port::settings().enhancedLighting;
    if (!enh::masterOn() || !s.nightLights || !s.effectLights || !sInStage || sInPass2 || !sViewValid) return;
    // The view matrix the effect is drawn with (DrawInfo + 0, MTX34). Fireflies use the main
    // camera; the fireworks show is a sky backdrop drawn with its own fixed camera (identity
    // rotation, ~1300 units back): its bursts have no place in the town, so their light goes where
    // they appear on screen (the backdrop's view space taken as the main camera's: same projection).
    float D[12];
    readMtx(drawInfo, D);
    bool mainCam = true;
    for (int k = 0; k < 12 && mainCam; k++) mainCam = std::fabs(D[k] - sView[k]) < ((k & 3) == 3 ? 1.0f : 0.01f);
    const u32 list = effect + 0x90;
    const u32 off = rd16(list + 0xA);
    u32 pm = rd32(list);
    for (int guard = 0; guard < 256 && guestPtr(pm); guard++, pm = rd32(pm + off + 4)) {
        const u8 kind = fxKindOf(pm);
        if (kind == FxNone) continue;
        float M[12];
        readMtx(pm + 0x8C, M);
        if (!std::isfinite(M[3] + M[7] + M[11])) continue;
        const u32 plist = pm + 0x38;
        const u32 poff = rd16(plist + 0xA);
        float acc[3] = {}, col[3] = {}, wsum = 0.0f;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        u32 p = rd32(plist), n = 0;
        for (; guestPtr(p) && n < 512; n++, p = rd32(p + poff + 4)) {
            const float loc[3] = {rdf32(p + 0xAC), rdf32(p + 0xB0), rdf32(p + 0xB4)};
            if (!std::isfinite(loc[0] + loc[1] + loc[2])) continue;
            float w[3];
            for (int r = 0; r < 3; r++) w[r] = M[r * 4] * loc[0] + M[r * 4 + 1] * loc[1] + M[r * 4 + 2] * loc[2] + M[r * 4 + 3];
            if (!mainCam) {
                float v[3];
                for (int r = 0; r < 3; r++) v[r] = D[r * 4] * w[0] + D[r * 4 + 1] * w[1] + D[r * 4 + 2] * w[2] + D[r * 4 + 3];
                std::memcpy(w, v, sizeof v);
            }
            float c[3] = {}, b = 0.0f;
            if (kind == FxTest) c[0] = c[1] = c[2] = 1.0f, b = 1.0f;
            else addLayer(rd32(p + 0x20), c, b), addLayer(rd32(p + 0x24), c, b);
            const float cm = std::max({c[0], c[1], c[2]});
            if (b <= 0.01f || cm <= 0.0f) continue;
            for (int k = 0; k < 3; k++) {
                acc[k] += w[k] * b;
                lo[k] = std::min(lo[k], w[k]), hi[k] = std::max(hi[k], w[k]);
                col[k] += c[k] / cm * b;
            }
            wsum += b;
        }
        if (sEflLog) {
            static std::unordered_map<u32, u32> sSeen;
            if (sSeen[pm]++ % 30 == 0)
                log("[efl] f%u fx pm %08X kind %u %s particles %u bright %.2f centre %.1f %.1f %.1f", copyDispCount(), pm, kind,
                    mainCam ? "world" : "backdrop", n, wsum, wsum > 0 ? acc[0] / wsum : 0.0f, wsum > 0 ? acc[1] / wsum : 0.0f,
                    wsum > 0 ? acc[2] / wsum : 0.0f);
        }
        if (wsum <= 0.01f) continue;
        FxSource src{};
        for (int k = 0; k < 3; k++) src.pos[k] = acc[k] / wsum, src.color[k] = col[k] / wsum;
        src.amount = wsum;
        src.spread = 0.5f * std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
        src.id = 0x5A000000u ^ (pm & 0x00FFFFFFu);
        src.kind = kind;
        src.world = mainCam;
        if (sFxCur.size() < 128) sFxCur[pm] = src;
    }
}

// Light budget: at most this many firefly / firework lights (strongest first); together with the
// night lights they go through enhanced_lighting.cpp's ranking into Aurora's 64 extra lights.
constexpr u32 kMaxFireflyLights = 12, kMaxFireworkLights = 8;

void submitEffectLights() {
    sFxNext.clear();
    for (const auto& [pm, src] : sFxCur) sFxNext.push_back(src);
    sFxCur.clear();
    const auto& s = port::settings().enhancedLighting;
    if (!s.nightLights || !s.effectLights || !sViewValid || sFxNext.empty()) return;
    // Test aid CF_EFL_AB=1 (with CF_ENHANCED_AB=1): effect lights only in every other enhanced
    // frame (dump numbers 4k+1 with, 4k+3 without), so neighbouring shots compare them.
    static const bool sAb = std::getenv("CF_EFL_AB") != nullptr;
    if (sAb && ((copyDispCount() + 1) / 2) % 2 == 1) return;
    std::sort(sFxNext.begin(), sFxNext.end(), [](const FxSource& a, const FxSource& b) { return a.amount > b.amount; });
    // Merge sources of one kind that sit together (strongest first keeps its id: stable pairing).
    std::vector<FxSource> merged;
    for (const auto& src : sFxNext) {
        const float near = src.kind == FxFirefly ? 25.0f : 200.0f;
        FxSource* into = nullptr;
        for (auto& m : merged) {
            const float dx = m.pos[0] - src.pos[0], dy = m.pos[1] - src.pos[1], dz = m.pos[2] - src.pos[2];
            if (m.kind == src.kind && m.world == src.world && dx * dx + dy * dy + dz * dz < near * near) into = &m;
        }
        if (!into) {
            merged.push_back(src);
            continue;
        }
        const float t = src.amount / (into->amount + src.amount);
        for (int k = 0; k < 3; k++) into->pos[k] += (src.pos[k] - into->pos[k]) * t, into->color[k] += (src.color[k] - into->color[k]) * t;
        into->spread = std::max(into->spread, src.spread);
        into->amount += src.amount;
    }
    sFxNext.swap(merged);
    // Outdoors the effects light the scene in proportion to the darkness (a firework at dusk barely
    // shows); indoors (a firefly in its jar) at full strength.
    const bool indoor = relBase(107, 1) != 0 && relBase(108, 1) == 0;
    const float dark = indoor ? 1.0f : 0.25f + 0.75f * sNight;
    const float k = std::clamp(s.nightLightIntensity, 0.0f, 3.0f) * dark;
    std::array<aurora::gfx::ExtraLight, kMaxFireflyLights + kMaxFireworkLights> out{};
    u32 n = 0, flies = 0, works = 0;
    for (const auto& src : sFxNext) {
        aurora::gfx::ExtraLight l{};
        // Particle colours are often pastel: push toward the hue so the light reads as coloured.
        const float mx = std::max({src.color[0], src.color[1], src.color[2], 1e-3f});
        for (int c = 0; c < 3; c++) l.color[c] = std::pow(src.color[c] / mx, src.kind == FxFirework ? 2.5f : 1.5f);
        if (src.kind == FxTest) {
            l.radius = 400.0f;
            l.intensity = 3.0f;
        } else if (src.kind == FxFirefly) {
            if (flies >= kMaxFireflyLights) continue;
            flies++;
            // A firefly floats 30-60 units up: a pool of soft green light on the ground below it.
            l.radius = 110.0f;
            l.intensity = 1.6f * std::min(src.amount, 1.0f) * k;
        } else {
            if (works >= kMaxFireworkLights) continue;
            works++;
            // A burst high over the town (the show is a sky backdrop, see noteEffectDraw): a wide,
            // soft coloured flash over the visible scene, following the burst's brightness.
            l.radius = std::clamp(1100.0f + src.spread, 1100.0f, 1800.0f);
            l.intensity = 2.5f * std::min(src.amount / 8.0f, 1.0f) * k;
        }
        if (!(l.intensity > 0.01f)) continue;
        if (src.world) toView(src.pos, l.pos);
        else std::memcpy(l.pos, src.pos, sizeof l.pos);
        l.flags = aurora::gfx::ExtraLightTranslucent;
        l.id = src.id;
        out[n++] = l;
        if (n == out.size()) break;
    }
    submitExtraLights(out.data(), n);
    if (sEflLog && copyDispCount() % 60 == 0)
        for (u32 i = 0; i < n; i++)
            log("[efl] f%u light %u view %.1f %.1f %.1f colour %.2f %.2f %.2f I %.2f r %.0f", copyDispCount(), i, out[i].pos[0], out[i].pos[1],
                out[i].pos[2], out[i].color[0], out[i].color[1], out[i].color[2], out[i].intensity, out[i].radius);
    if ((logNow() || sEflLog) && n)
        log("[efl] f%u %u effect lights (%u fireflies, %u fireworks), first at view %.1f %.1f %.1f colour %.2f %.2f %.2f I %.2f r %.0f",
            copyDispCount(), n, flies, works, out[0].pos[0], out[0].pos[1], out[0].pos[2], out[0].color[0], out[0].color[1], out[0].color[2],
            out[0].intensity, out[0].radius);
}
}  // namespace

bool onModel(Ctx& c, u32 obj) {
    const bool fit = sFitWanted + 2 >= copyDispCount();  // Enhanced world's effects centre (groundAhead)
    if (!sModelLog && !enh::masterOn() && !fit) return true;
    const ModelInfo& mi = modelInfo(obj);
    const u32 task = c.r[4];
    if ((enh::masterOn() || fit) && sInStage && sPass == 1 && (task & 0xFFFF) == 6 &&
        (mi.name.compare(0, 4, "obj_") == 0 || mi.name.compare(0, 4, "sdw_") == 0)) {
        // The rolling log's fit (LogFit): reads only.
        const float y[3] = {rdf32(obj + 0x3C + 4), rdf32(obj + 0x3C + 20), rdf32(obj + 0x3C + 36)};
        const float yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        if (yl > 1e-3f && std::isfinite(yl)) {
            const float p[3] = {rdf32(obj + 0x3C + 12), rdf32(obj + 0x3C + 28), rdf32(obj + 0x3C + 44)};
            fitSample(p[1], p[2], y[1] / yl, y[2] / yl);
            if (sSunLog && copyDispCount() % 60 == 0)
                log("[sun] f%u sample %s p %.1f %.1f %.1f up %.4f %.4f %.4f", copyDispCount(), mi.name.c_str(), p[0], p[1], p[2], y[0] / yl,
                    y[1] / yl, y[2] / yl);
        }
    }
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
    enh::setEmissive(false);  // (set again by the model's lit materials)
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
    if (proc == 6 || proc == 7) cffurn::noteModel(obj, mi.mdl, mi.name);  // furniture lights
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

// d_a_strNP fn_99_A40: a structure's window-glow controller (r3) sets its lit materials' TEV
// colour 1 from the light amount at +0 (ScnMdl at +0x2C).
static void strLight(Ctx& c) {
    const u32 ctl = c.r[3];
    const u32 mdl = rd32(ctl + 0x2C);
    const float amount = rdf32(ctl);
    if (mdl) cflight::noteGlow(mdl, amount);
    if (mdl) cflight::noteLitMaterials(mdl, ctl);
    cflight::sInStrLight = true;
    cflight::sStrAmount = std::isfinite(amount) ? amount : 0.0f;
    callOriginal(c, "d_a_strNP::fn_99_A40");
    cflight::sInStrLight = false;
}
static HleReg sStrLightReg("d_a_strNP::fn_99_A40", &strLight);

// ResMatTevColor::GXSetTevColor (r3 = ResMatTevColor*, its data pointer first): fn_99_A40 sets a
// lit material's colour; G3DState::LoadResMatTevColor (r3 likewise): g3d loads a material to draw.
HLE(GXSetTevColor__Q34nw4r3g3d14ResMatTevColorF11_GXTevRegID8_GXColor) {
    if (cflight::sInStrLight) cflight::noteLitTev(rd32(c.r[3]));
    callOriginal(c, "GXSetTevColor__Q34nw4r3g3d14ResMatTevColorF11_GXTevRegID8_GXColor");
}
HLE(LoadResMatTevColor__Q34nw4r3g3d8G3DStateFQ34nw4r3g3d14ResMatTevColor) {
    if (enh::masterOn()) cflight::onLoadTevColor(rd32(c.r[3]));
    callOriginal(c, "LoadResMatTevColor__Q34nw4r3g3d8G3DStateFQ34nw4r3g3d14ResMatTevColor");
}

// nw4r::ef::DrawOrder::Draw(Effect*, const DrawInfo&): an effect's particle managers are drawn.
HLE(Draw__Q34nw4r2ef9DrawOrderFPQ34nw4r2ef6EffectRCQ34nw4r2ef8DrawInfo) {
    if (enh::masterOn()) cflight::noteEffectDraw(c.r[4], c.r[5]), enh::setEmissive(false);
    callOriginal(c, "Draw__Q34nw4r2ef9DrawOrderFPQ34nw4r2ef6EffectRCQ34nw4r2ef8DrawInfo");
}

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
