// Grass wear (F1 > Enhancements > Gameplay "Grass wear"; docs/FEATURES.md "Grass wear").
//
// What the game does (USA rev 1 addresses; the other versions through VA(), recomp/version.h; the
// field offsets below are the same in all four, checked with gen_version_map.py field):
// - The grass manager is a static object (fn_800A835C returns it: USA 0x80591C00). Its +0x58 is a
//   128 x 128 one-byte-per-tile "tiled array" (fn_802B6338 init, fn_802B637C get, fn_802B63CC set;
//   8 x 4 blocks like an I8 texture, buffer pointer at +0x7C = manager + 0x80) holding the health of
//   every tile of the town (acre (ax, ay) of the 8 x 8 grid covers x = ax*16.., y = ay*16..; a tile
//   is 32 x 32 world units), 0 = bare, 255 = full grass. +0x4080 is a 128 x 128 bit map of the tiles
//   already worn since it was last cleared (fn_800A77FC test, fn_800A7830 set, fn_800A773C clear).
// - The saved copy is in the town data: 25 acres x 16 x 16 bytes at town + 0x38A2 (fn_80110E7C,
//   acre stride 0x100, row of acres 0x500), each acre 8 x 4 tiled like the manager's array.
// - fn_800A7C84(mgr, x, y, value, flush) writes one tile: the value clamped to the tile's
//   minimum/maximum (fn_800A7DA0 / fn_800A7E10), into the save copy and the manager's array.
// - fn_800A7E80(mgr, x, y, delta, force, flush) adds `delta` to a tile; unless `force`, only if the
//   tile is not yet in the worn bit map, and then marks it: a tile wears once per load.
// - fn_8008D6B4(field, pos, amount): fn_800A7F14(mgr, pos, -amount) = tile under pos, then
//   fn_800A7E80. The player's step code (d_a_playerNP fn_80_2334, whenever the player enters a new
//   tile) calls it with amount 4 + rand(4), or 8 + rand(4) while player +0x2270 bit 0x00800000 is
//   set; the town generation calls it through fn_8008D658 with 255 (bare ground under buildings).
// - fn_800A7F9C(mgr, days, ?, flush) (from fn_8008D70C): the daily regrowth. Tiles with a plant on
//   them get the plant's bonus (item data +0x0E), then every tile gets 1 + (mean of its 8
//   neighbours) / 85 (+4 in healthy grass, less next to bare ground), both forced; the worn bit map
//   is cleared.
//
// Modes (port::settings().gameplay.grassWear):
//   0 Original   the game unchanged (the hooks only call the originals).
//   1 New Leaf   (our estimate, docs/FEATURES.md): a step wears a tile only while the player runs;
//                each tile wears at most once per game day (6 AM to 6 AM) as well as once per
//                load; the wear is halved (rounded up); the daily regrowth runs twice.
//   2 Off        steps never wear; regrowth unchanged, so existing bare patches heal.
// F1 also has "Regrow all grass now" (every worn tile, health 1..254, of the loaded town to 255
// through the game's own setter, saved with the town at the next save).
//
// Test aids: CF_GRASS_WEAR_X=<n> (steps wear n times as much), CF_GRASS_REGROW_AT=<frame> (the
// "Regrow all grass now" button at that guest frame), CF_GRASS_LOG=1: every player step wear (tile, amount, player speed, decision,
// old -> new health) and every regrowth (sum of all tiles and tiles under 64, before -> after).
#include "game/cf_features.h"

#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace recomp::cf {
namespace {

// USA addresses, mapped to the running version (recomp/version.h).
struct GrassAddrs {
    u32 mgr;      // grass manager singleton (fn_800A835C)
    u32 genRet;   // return address in fn_8008D658 (town generation) of its fn_8008D6B4 call
    u32 setTile;  // fn_800A7C84
};
const GrassAddrs& addrs() {
    static const GrassAddrs a{VA(0x80591C00), VA(0x8008D698), VA(0x800A7C84)};
    return a;
}
constexpr const char* kStepFn = "fn_8008D6B4";    // hooks.txt "all": the USA name on every version
constexpr const char* kRegrowFn = "fn_800A7F9C";

constexpr u32 kTexObj = 0x58, kTexBuf = 0x7C, kSide = 128;
constexpr float kTileSize = 32.0f;

bool logOn() {
    static const bool on = [] {
        const char* v = std::getenv("CF_GRASS_LOG");
        return v && *v && *v != '0';
    }();
    return on;
}

int mode() {
    const int m = port::settings().gameplay.grassWear;
    return m >= 0 && m <= 2 ? m : 0;
}

// The manager's 128 x 128 array, 8 x 4 blocks.
u32 tileAddr(u32 buf, u32 x, u32 y) { return buf + ((y / 4) * (kSide / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8; }
bool texReady(u32& buf) {
    const u32 m = addrs().mgr;
    buf = rd32(m + kTexBuf);
    return buf >= 0x80000000u && buf < 0x81800000u && rd32(m + kTexObj) == kSide && rd32(m + kTexObj + 4) == kSide;
}
int tile(u32 x, u32 y) {
    u32 buf;
    if (x >= kSide || y >= kSide || !texReady(buf)) return -1;
    return int(rd8(tileAddr(buf, x, y)));
}
long long grassSum(int& bare) {
    u32 buf;
    bare = 0;
    if (!texReady(buf)) return -1;
    long long s = 0;
    for (u32 y = 0; y < kSide; y++)
        for (u32 x = 0; x < kSide; x++) {
            const u8 v = u8(rd8(tileAddr(buf, x, y)));
            s += v;
            if (v < 0x40) bare++;
        }
    return s;
}

// Game day for "once per day": the console clock (seconds since 2000-01-01 local) with the day
// starting at 6 AM, as the game's new day does.
s64 gameDay() {
    const s64 secs = s64(double(timeBase()) / kTimerClock);
    return (secs - 6 * 3600) / 86400;
}
u8 sWornToday[kSide * kSide / 8];
s64 sWornDay = -1;
bool wornToday(u32 x, u32 y) {
    const s64 d = gameDay();
    if (d != sWornDay) {
        std::memset(sWornToday, 0, sizeof sWornToday);
        sWornDay = d;
    }
    const u32 i = y * kSide + x;
    return (sWornToday[i / 8] >> (i % 8)) & 1;
}
void markWornToday(u32 x, u32 y) {
    const u32 i = y * kSide + x;
    sWornToday[i / 8] |= u8(1u << (i % 8));
}

// The player actor (d_a_playerNP) passes &player->pos (+0x70) to the step wear; its speed is
// the float at +0x88 (the step code compares it with a constant right after the wear call).
constexpr u32 kPosInPlayer = 0x70, kSpeedInPlayer = 0x88;
// Between walking and running at full stick (measured with CF_GRASS_LOG, docs/FEATURES.md).
constexpr float kRunSpeed = 2.35f;  // walking 2.0, running 2.7 (3.0 at the first step)

std::atomic<bool> sRegrowAll{false};

void tileOf(u32 pos, u32& tx, u32& ty) {
    tx = u32(s32(std::floor(rdf32(pos) / kTileSize)));
    ty = u32(s32(std::floor(rdf32(pos + 8) / kTileSize)));
}

}  // namespace

// fn_8008D6B4(field, pos, amount): player steps and town generation (see the top of the file).
static void stepWear(Ctx& c) {
    const bool generation = c.lr == addrs().genRet;
    const int m = mode();
    if (generation || (m == 0 && !logOn() && !std::getenv("CF_GRASS_WEAR_X"))) {
        callOriginal(c, kStepFn);
        return;
    }
    const u32 pos = c.r[4];
    // Test aid CF_GRASS_WEAR_X=<n>: every player step wears n times as much (a path that would take
    // weeks of play shows in one run; screenshots of Original vs Off).
    static const int sWearX = std::getenv("CF_GRASS_WEAR_X") ? std::atoi(std::getenv("CF_GRASS_WEAR_X")) : 1;
    if (sWearX > 1) c.r[5] = u32(s32(c.r[5]) * sWearX);
    const s32 amount = s32(c.r[5]);
    u32 tx, ty;
    tileOf(pos, tx, ty);
    const float speed = rdf32(pos - kPosInPlayer + kSpeedInPlayer);
    const u32 f = game::guestFrames();
    const int before = logOn() ? tile(tx, ty) : 0;
    if (m == 0) {
        callOriginal(c, kStepFn);
        log("[grass] f%u step %u,%u amount %d speed %.2f: original (%d -> %d)", f, tx, ty, amount, speed, before,
            tile(tx, ty));
        return;
    }
    const char* what;
    if (m == 2) {
        what = "off: no wear";
    } else if (speed < kRunSpeed) {
        what = "new leaf: walking, no wear";
    } else if (tx < kSide && ty < kSide && wornToday(tx, ty)) {
        what = "new leaf: already worn today";
    } else {
        // Half the wear (rounded up); the game's once-per-load rule still applies inside.
        const s32 half = (amount + 1) / 2;
        c.r[5] = u32(half);
        if (tx < kSide && ty < kSide) markWornToday(tx, ty);
        callOriginal(c, kStepFn);
        if (logOn())
            log("[grass] f%u step %u,%u amount %d -> %d speed %.2f: new leaf: running (%d -> %d)", f, tx, ty, amount,
                half, speed, before, tile(tx, ty));
        return;
    }
    if (logOn()) log("[grass] f%u step %u,%u amount %d speed %.2f: %s (stays %d)", f, tx, ty, amount, speed, what, before);
}

// fn_800A7F9C(mgr, days, ?, flush): daily regrowth.
static void regrow(Ctx& c) {
    int bare0 = 0, bare1 = 0;
    const long long s0 = logOn() ? grassSum(bare0) : 0;
    const u32 r3 = c.r[3], r4 = c.r[4], r5 = c.r[5], r6 = c.r[6], lr = c.lr;
    callOriginal(c, kRegrowFn);
    if (mode() == 1) {
        // New Leaf style: a second regrowth pass the same day.
        c.r[3] = r3;
        c.r[4] = r4;
        c.r[5] = r5;
        c.r[6] = r6;
        c.lr = lr;
        callOriginal(c, kRegrowFn);
    }
    if (logOn()) {
        const long long s1 = grassSum(bare1);
        log("[grass] f%u regrowth (days %d, mode %d, lr %08X): sum %lld -> %lld, tiles under 64: %d -> %d",
            game::guestFrames(), s32(r4), mode(), lr, s0, s1, bare0, bare1);
    }
}

static HleReg sStep("fn_8008D6B4", &stepWear), sRegrow("fn_800A7F9C", &regrow);

// Test aid CF_GRASS_MAP=<dir>,<frame>[,<frame>...]: the town's 80 x 80 tile health (acres 1..5)
// as <dir>/grass_<frame>.pgm (one pixel per tile, 0 black = bare .. 255 white = full grass).
static void grassMapAid() {
    static const std::string sDir = [] {
        const char* v = std::getenv("CF_GRASS_MAP");
        if (!v) return std::string();
        std::string s = v;
        return s.substr(0, s.find(','));
    }();
    if (sDir.empty()) return;
    static const std::string sFrames = "," + std::string(std::getenv("CF_GRASS_MAP")).substr(sDir.size()) + ",";
    const u32 f = game::guestFrames();
    if (sFrames.find("," + std::to_string(f) + ",") == std::string::npos) return;
    u32 buf;
    if (!texReady(buf)) return;
    const std::string path = sDir + "/grass_" + std::to_string(f) + ".pgm";
    if (FILE* out = std::fopen(path.c_str(), "wb")) {
        std::fprintf(out, "P5\n80 80\n255\n");
        for (u32 y = 16; y < 96; y++)
            for (u32 x = 16; x < 96; x++) std::fputc(rd8(tileAddr(buf, x, y)), out);
        std::fclose(out);
        log("[grass] map written to %s", path.c_str());
    }
}

void grassOnFrame(Ctx& c) {
    grassMapAid();
    static const u32 sRegrowAt = std::getenv("CF_GRASS_REGROW_AT") ? u32(std::atoi(std::getenv("CF_GRASS_REGROW_AT"))) : 0;
    if (sRegrowAt && game::guestFrames() == sRegrowAt) sRegrowAll = true;
    if (!sRegrowAll.exchange(false)) return;
    u32 buf;
    if (!texReady(buf)) {
        log("[grass] regrow all: no town loaded");
        return;
    }
    int bare0 = 0, bare1 = 0;
    const long long s0 = grassSum(bare0);
    // Every worn tile (health 1..254) of the town's 5 x 5 acres (acres 1..5 of the 8 x 8 grid) to
    // 255 through the game's own setter (clamped to the tile's maximum, written to the saved copy
    // too). Tiles at 0 are left alone: the town generation sets the ground under buildings, the
    // plaza and other objects to 0, and the setter's clamp does not protect it (a first version
    // that raised every tile grew grass over them). This runs at the end of a frame inside an HLE
    // call: keep its registers.
    u32 r[32];
    std::memcpy(r, c.r, sizeof r);
    const u32 lr = c.lr, ctr = c.ctr;
    u8 cr[8];
    std::memcpy(cr, c.cr, sizeof cr);
    for (u32 y = 16; y < 16 * 6; y++)
        for (u32 x = 16; x < 16 * 6; x++) {
            const u8 v = u8(rd8(tileAddr(buf, x, y)));
            if (v == 0 || v == 255) continue;
            c.r[7] = 0;  // no per-tile data cache flush
            guestCall(c, addrs().setTile, addrs().mgr, x, y, 255);
        }
    std::memcpy(c.r, r, sizeof r);
    c.lr = lr;
    c.ctr = ctr;
    std::memcpy(c.cr, cr, sizeof cr);
    const long long s1 = grassSum(bare1);
    log("[grass] regrow all at guest frame %u: sum %lld -> %lld, tiles under 64: %d -> %d", game::guestFrames(), s0, s1,
        bare0, bare1);
}

void grassMenu(bool& dirty) {
    auto& g = port::settings().gameplay;
    ImGui::SeparatorText("Grass wear");
    static const char* kModes[] = {"Original", "New Leaf style", "Off"};
    int m = mode();
    if (ImGui::Combo("Grass wear", &m, kModes, 3)) {
        g.grassWear = m;
        dirty = true;
    }
    ImGui::SetItemTooltip(
        "How walking wears the town's grass down to dirt.\n"
        "Original: as the game does it (a tile loses some health the first time you step on it\n"
        "after each load; it grows back a little every day).\n"
        "New Leaf style (our estimate, not New Leaf's real numbers): only running wears grass,\n"
        "a tile wears at most once per day and half as much, and it grows back twice as fast.\n"
        "Off: grass never wears; existing bare patches grow back as usual.");
    if (ImGui::Button("Regrow all grass now")) sRegrowAll = true;
    ImGui::SetItemTooltip(
        "Gives every worn grass tile of the town full health right away. Completely bare ground\n"
        "(under buildings and objects, the plaza, tiles worn all the way down) is left as it is.\n"
        "Saved with the town the next time the game saves. Only while the town is loaded.");
}

}  // namespace recomp::cf
