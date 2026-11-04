// Skin tone of the player being played (F1 > Enhancements > Gameplay "Skin tone"; docs/FEATURES.md
// "Skin tone").
//
// What the game does (USA rev 1; the other versions: same code, addresses through VA(), d_a_playerNP
// offsets identical in all four, fields checked with tools/recomp/gen_version_map.py diff/reldiff):
// - Player data ("pd") of save slot s: save + 0x20 + s * 0x86C0 (fn_8013B33C; save base from
//   fn_8010E308). fn_801017B8(i) returns the pd of player number i: the player manager at USA
//   0x805D26C0 keeps the save slot of each player number at +0x34 + 4i (fn_80100454; 7 = none,
//   4..7 = a visitor whose data lives elsewhere).
// - pd + 0x83F8 is the tan, 0..15; the tone shown is tan >> 1, 0..7 (d_a_playerNP fn_80_2080).
//   pd + 0x83EC is the face (0..7), pd + 0x7EEC the gender; pd + 0x7EC2 holds the town id (u16)
//   and name (8 UTF-16 characters), pd + 0x7ED8 the player id and name (the author record that
//   patterns copy).
// - d_a_playerNP fn_80_2080(player) returns the skin palette index = tan >> 1 (palette files
//   Plyr/Pltt/Palette.arc 0..7.brplt "skin"); fn_80_20D0 the face palette 8 + face * 8 + skin
//   (8..71: each face has its own eight-tone palette). The player actor keeps them at +0x221C /
//   +0x2218 (set when the actor is built, d_a_playerNP fn at 0x2C1F4) and fn_80_28BDC binds them
//   (fn_800B9A04 -> fn_800B9718 loads "%d.brplt" from the palette archive), called once from
//   fn_80_29E0C when the model is set up. So a new tone shows when the player model is next built
//   (scene change, entering/leaving a building, load) unless the port rebinds it (below).
// - fn_80137D34(pd, delta) is the daily tan update (seen at the 6 AM new day: 0x0E -> 0x0D in
//   October): it adjusts pd + 0x83F8 depending on delta and a "was in the sun" flag (0x50, test
//   fn_801386A4, cleared by fn_80138714), clamped to 0..15.
//
// What the port does: a fixed tone (1..8 in the menu = palette 0..7) for a player replaces
// fn_80_2080's result for that player's actor, so the skin and face palettes both follow; the
// tan update keeps that player's saved tan unchanged while fixed (the save keeps its own tan,
// which comes back with "Game"). Other residents are untouched. The choice is stored per player
// in settings.ini [skin_tone] as "<town id>-<player id>" (hex) = tone (0 = game), so it follows
// the resident whichever save slot or console the town is on. On a change in the menu the
// current player's actor is rebound at once (the game's fn_80_29E0C with the new indices).
#include "game/cf_features.h"

#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <ostream>
#include <string>

namespace recomp::cf {
namespace {

constexpr u32 kPlayerModule = 80;  // d_a_playerNP
// d_a_playerNP .text offsets (the same in all four versions: rels_us_map.txt "hash").
constexpr u32 kSkinIndexFn = 0x2080, kFaceIndexFn = 0x20D0, kSetupFn = 0x29E0C;
constexpr u32 kActorFace = 0x2218, kActorSkin = 0x221C, kActorPlayerNo = 0x2240;
constexpr u32 kPdTan = 0x83F8, kPdTown = 0x7EC2, kPdPlayer = 0x7ED8;

struct SkinAddrs {
    u32 playerMgr;   // slot of each player number at +0x34
    u32 playerData;  // fn_801017B8(player number) -> pd
    u32 pltt;        // fn_800B9718(&size, index) -> palette file
};
const SkinAddrs& addrs() {  // USA addresses, mapped to the running version (recomp/version.h)
    static const SkinAddrs a{VA(0x805D26C0), VA(0x801017B8), VA(0x800B9718)};
    return a;
}
constexpr const char* kTanFn = "fn_80137D34";  // hooks.txt "all"

bool logOn() {
    static const bool on = std::getenv("CF_SKIN_LOG") != nullptr;
    return on;
}

std::string hex4(u32 v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04X", v & 0xFFFF);
    return b;
}
// UTF-16BE name (up to 8 characters) -> UTF-8.
std::string name8(u32 a) {
    std::string s;
    for (u32 i = 0; i < 8; i++) {
        const u32 ch = rd16(a + 2 * i);
        if (!ch) break;
        if (ch < 0x80) s += char(ch);
        else if (ch < 0x800) {
            s += char(0xC0 | ch >> 6);
            s += char(0x80 | (ch & 0x3F));
        } else {
            s += char(0xE0 | ch >> 12);
            s += char(0x80 | (ch >> 6 & 0x3F));
            s += char(0x80 | (ch & 0x3F));
        }
    }
    return s;
}
std::string keyOf(u32 pd) { return hex4(rd16(pd + kPdTown)) + "-" + hex4(rd16(pd + kPdPlayer)); }

// Save slots live in MEM2; an online visitor's player data is a copy in MEM1 (the player manager's
// buffer, USA 0x805D2720 for player 1 while visiting/hosting).
bool validPd(u32 pd) {
    return (pd >= 0x90000000u && pd + 0x86C0u <= 0x94000000u) || (pd >= 0x80004000u && pd + 0x86C0u <= 0x81800000u);
}

// Per-player choices: key -> tone 1..8 (absent = game). Read on the guest thread, written by the
// menu (host thread).
std::mutex sMutex;
std::map<std::string, int>& choices() { return port::settings().gameplay.skinTone; }
std::map<std::string, int> sRemote;  // other players' tones during an online visit (not saved)
int fixedFor(const std::string& key) {
    std::lock_guard lk(sMutex);
    const auto it = choices().find(key);
    if (it != choices().end()) return it->second;
    const auto r = sRemote.find(key);
    return r == sRemote.end() ? 0 : r->second;
}

// What the menu shows: the player being played (player number 0).
struct Current {
    bool valid = false;
    std::string key, town, player;
    int tan = 0;     // saved tan 0..15
    int shown = -1;  // palette index the actor uses now (-1 unknown)
};
Current sCurrent;
std::atomic<bool> sRebind{false};
u32 sActor = 0, sActorSkin = 0;  // last local player actor seen by fn_80_2080, and its result
u32 sScratch = 0;
unsigned sSwatch[8] = {};
bool sSwatchesRead = false;

u32 playerModuleText() { return relBase(kPlayerModule, 1); }

}  // namespace

// d_a_playerNP fn_80_2080(player): skin palette index.
static void skinIndex(Ctx& c) {
    const u32 actor = c.r[3];
    const u32 no = rd32(actor + kActorPlayerNo);
    callOriginal(c, "d_a_playerNP::fn_80_2080");
    const u32 game = c.r[3];
    if (no >= 4) {
        if (logOn()) log("[skin] f%u actor %08X player number %u: not handled", game::guestFrames(), actor, no);
        return;
    }
    const u32 pd = guestCall(c, addrs().playerData, no);
    c.r[3] = game;
    if (!validPd(pd)) {
        if (logOn()) log("[skin] f%u actor %08X player %u pd %08X: not a player data address", game::guestFrames(), actor, no, pd);
        return;
    }
    const int fixed = fixedFor(keyOf(pd));
    const u32 result = fixed ? u32(fixed - 1) : game;
    c.r[3] = result;
    if (no == 0) {
        sActor = actor;
        sActorSkin = result;
    }
    if (logOn())
        log("[skin] f%u actor %08X player %u pd %08X (%s) tan %u: palette %u%s", game::guestFrames(), actor, no, pd,
            keyOf(pd).c_str(), rd8(pd + kPdTan), result, fixed ? " (fixed)" : "");
}
static HleReg sSkinReg("d_a_playerNP::fn_80_2080", &skinIndex);

// fn_80137D34(pd, delta): the daily tan update.
static void tanUpdate(Ctx& c) {
    const u32 pd = c.r[3];
    const bool fixed = validPd(pd) && fixedFor(keyOf(pd)) != 0;
    const u8 before = validPd(pd) ? u8(rd8(pd + kPdTan)) : 0;
    callOriginal(c, kTanFn);
    if (!validPd(pd)) return;
    if (fixed) wr8(pd + kPdTan, before);
    if (logOn())
        log("[skin] f%u tan update pd %08X (%s): %u -> %u%s", game::guestFrames(), pd, keyOf(pd).c_str(), before,
            rd8(pd + kPdTan), fixed ? " (fixed tone: kept)" : "");
}
static HleReg sTan("fn_80137D34", &tanUpdate);

// The menu's swatches: the main colour of each skin palette file (RGB5A3), read once through
// the game's palette loader.
static void readSwatches(Ctx& c) {
    if (sSwatchesRead) return;
    if (!sScratch) sScratch = hleAlloc(16);
    for (u32 i = 0; i < 8; i++) {
        wr32(sScratch, 0);
        const u32 file = guestCall(c, addrs().pltt, sScratch, i);
        const u32 size = rd32(sScratch);
        if (!file || size < 0x40 || size > 0x4000) return;  // archive not loaded yet
        u32 plt0 = 0;
        for (u32 o = 0; o + 4 <= size; o += 4)
            if (rd32(file + o) == 0x504C5430) {  // "PLT0"
                plt0 = file + o;
                break;
            }
        if (!plt0) return;
        // PLT0: +0x10 offset of the colour data (from the PLT0 header), +0x1C entry count.
        const u32 data = plt0 + rd32(plt0 + 0x10);
        const u32 n = rd16(plt0 + 0x1C);
        if (!n || n > 256 || data + 2 * n > file + size) return;
        if (logOn()) {
            char line[256];
            int k = 0;
            for (u32 e = 0; e < n && e < 16; e++) k += std::snprintf(line + k, sizeof line - k, " %04X", rd16(data + 2 * e));
            log("[skin] palette %u (%u entries, PLT0 at %08X):%s", i, n, plt0, line);
        }
        // Entry 6 is the main skin colour (0: FF34 light .. 7: E1C6 dark in the USA files).
        const u32 v = rd16(data + 2 * (n > 6 ? 6 : n - 1));
        unsigned r, g, b;
        if (v & 0x8000) {
            r = (v >> 10 & 31) * 255 / 31;
            g = (v >> 5 & 31) * 255 / 31;
            b = (v & 31) * 255 / 31;
        } else {
            r = (v >> 8 & 15) * 17;
            g = (v >> 4 & 15) * 17;
            b = (v & 15) * 17;
        }
        sSwatch[i] = r << 16 | g << 8 | b;
    }
    sSwatchesRead = true;
    if (logOn())
        log("[skin] swatches %06X %06X %06X %06X %06X %06X %06X %06X", sSwatch[0], sSwatch[1], sSwatch[2], sSwatch[3],
            sSwatch[4], sSwatch[5], sSwatch[6], sSwatch[7]);
}

void skinSetRemote(const std::map<std::string, int>& tones) {
    std::lock_guard lk(sMutex);
    sRemote.clear();
    for (const auto& [k, v] : tones)
        if (v >= 1 && v <= 8 && k.size() <= 16) sRemote[k] = v;
}

void skinOnFrame(Ctx& c) {
    // Registers of the HLE call this runs in are kept (guestCall uses r3..r6 and lr).
    u32 r[32];
    std::memcpy(r, c.r, sizeof r);
    const u32 lr = c.lr, ctr = c.ctr;
    u8 cr[8];
    std::memcpy(cr, c.cr, sizeof cr);

    Current cur;
    const u32 slot = rd32(addrs().playerMgr + 0x34);
    if (slot < 4 && playerModuleText()) {
        const u32 pd = guestCall(c, addrs().playerData, 0);
        if (validPd(pd) && rd16(pd + kPdPlayer)) {
            cur.valid = true;
            cur.key = keyOf(pd);
            cur.town = name8(pd + kPdTown + 2);
            cur.player = name8(pd + kPdPlayer + 2);
            cur.tan = rd8(pd + kPdTan);
        }
    }
    // Rebind the current player's palettes after a menu change, if its actor is still the one
    // fn_80_2080 last saw (same palette index stored, same player number, module linked).
    if (sRebind.exchange(false) && cur.valid && sActor && playerModuleText() &&
        rd32(sActor + kActorSkin) == sActorSkin && rd32(sActor + kActorPlayerNo) == 0) {
        const u32 text = playerModuleText();
        const u32 skin = guestCall(c, text + kSkinIndexFn, sActor);
        const u32 face = guestCall(c, text + kFaceIndexFn, sActor);
        wr32(sActor + kActorSkin, skin);
        wr32(sActor + kActorFace, face);
        sActorSkin = skin;
        // fn_80_29E0C: the game's own model palette setup (releases the previous bindings, then
        // fn_80_29330 + fn_80_28BDC bind the palettes the actor's +0x221C / +0x2218 name).
        guestCall(c, text + kSetupFn, sActor);
        log("[skin] f%u rebound player palettes: skin %u face %u", game::guestFrames(), skin, face);
    }
    if (sActor && rd32(sActor + kActorPlayerNo) == 0 && rd32(sActor + kActorSkin) == sActorSkin)
        cur.shown = int(sActorSkin);
    if (cur.valid) readSwatches(c);  // the palette archive is loaded once a player exists

    std::memcpy(c.r, r, sizeof r);
    c.lr = lr;
    c.ctr = ctr;
    std::memcpy(c.cr, cr, sizeof cr);

    // Test aid CF_SKIN_TONE_AT=<frame>:<tone 0-8>[,...]: sets the current player's choice at that
    // guest frame, as the menu does (0 = game).
    static const std::string sAt = std::getenv("CF_SKIN_TONE_AT") ? std::getenv("CF_SKIN_TONE_AT") : "";
    if (!sAt.empty() && cur.valid) {
        const u32 f = game::guestFrames();
        size_t p = 0;
        while (p < sAt.size()) {
            size_t q = sAt.find(',', p);
            if (q == std::string::npos) q = sAt.size();
            unsigned af = 0;
            int tone = 0;
            if (std::sscanf(sAt.substr(p, q - p).c_str(), "%u:%d", &af, &tone) == 2 && af == f) {
                {
                    std::lock_guard lk(sMutex);
                    if (tone > 0 && tone <= 8) choices()[cur.key] = tone;
                    else choices().erase(cur.key);
                }
                sRebind = true;
                log("[skin] f%u test: player %s tone %d", f, cur.key.c_str(), tone);
            }
            p = q + 1;
        }
    }
    std::lock_guard lk(sMutex);
    sCurrent = cur;
}

void skinMenu(bool& dirty) {
    ImGui::SeparatorText("Skin tone");
    Current cur;
    unsigned sw[8];
    bool haveSwatches;
    {
        std::lock_guard lk(sMutex);
        cur = sCurrent;
        std::memcpy(sw, sSwatch, sizeof sw);
        haveSwatches = sSwatchesRead;
    }
    if (!cur.valid) {
        ImGui::TextDisabled("Skin tone: no resident is being played right now.");
        return;
    }
    ImGui::Text("%s of %s", cur.player.empty() ? "(no name)" : cur.player.c_str(),
                cur.town.empty() ? "(no name)" : cur.town.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(game tan %d of 7)", cur.tan >> 1);
    int tone = fixedFor(cur.key);
    bool changed = false;
    if (ImGui::RadioButton("Game (tanning)", tone == 0)) {
        tone = 0;
        changed = true;
    }
    ImGui::SetItemTooltip("The game's own skin tone: tans in summer sun and fades back afterwards.");
    for (int i = 1; i <= 8; i++) {
        ImGui::SameLine();
        ImGui::PushID(i);
        if (haveSwatches) {
            const unsigned v = sw[i - 1];
            const ImVec4 col(float(v >> 16 & 255) / 255.0f, float(v >> 8 & 255) / 255.0f, float(v & 255) / 255.0f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
        }
        char label[16];
        std::snprintf(label, sizeof label, tone == i ? "[%d]" : " %d ", i);
        if (ImGui::Button(label)) {
            tone = i;
            changed = true;
        }
        if (haveSwatches) ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("Fixed tone %d of 8 (%s); tanning is off for this resident while it is set.", i,
                              i == 1 ? "lightest" : i == 8 ? "darkest" : "");
        ImGui::PopID();
    }
    if (changed) {
        {
            std::lock_guard lk(sMutex);
            if (tone) choices()[cur.key] = tone;
            else choices().erase(cur.key);
        }
        sRebind = true;
        dirty = true;
    }
    ImGui::TextDisabled("Only for %s: other residents of the town keep their own tone.",
                        cur.player.empty() ? "this resident" : cur.player.c_str());
}

// [skin_tone] in settings.ini: "<town id>-<player id> = <tone 1..8>" (absent = game tanning).
static void resetTones() { port::settings().gameplay.skinTone.clear(); }
static void loadTone(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "skin_tone") return;
    const int t = std::atoi(value.c_str());
    if (t >= 1 && t <= 8) port::settings().gameplay.skinTone[key] = t;
}
static void saveTones(std::ostream& out) {
    std::lock_guard lk(sMutex);
    if (port::settings().gameplay.skinTone.empty()) return;
    out << "\n[skin_tone]\n";
    for (const auto& [k, v] : port::settings().gameplay.skinTone) out << k << "=" << v << "\n";
}
static const bool sToneSettings = [] {
    port::addSettingsExtension({&resetTones, &loadTone, &saveTones});
    return true;
}();

}  // namespace recomp::cf
