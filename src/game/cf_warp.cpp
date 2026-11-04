// Warps (F1 > Enhancements > Debug "Warp", CF_WARP, route token WARP=; docs/WARPS.md).
//
// City Folk's places are "stages": one table of 68 (USA main.dol 0x804F0C98, 8 bytes each: name,
// stage info) from FIELD (the town, 0) through the rooms (RM_*), the city (TOWN, 0x2D) and the
// demo scenes (DM_*). Next to it per-stage tables (0x80479E60 flags: 0x10 room, 0x20 demo, 0x400
// player house, 0x800 villager house, 0x2000 museum, 0x4000 bus ride; 0x80479F70 the structure id
// of the building a room belongs to, fn_801628F0). The current stage is the byte r13-0x6384
// (fn_8016266C), the previous one r13-0x6383. Stage info +8 = the exit table {entries, count},
// entries of 0x1C bytes: u8 stage[4] (one per variant, e.g. which of the four player houses,
// fn_801627CC), pos x/y/z, s16 angle at +0x10, arrival type at +0x14. Stage codes above 0x43 are
// "outside": 0x44 the door of the room's own building (fn_801628F0 -> fn_801630FC), 0x45 the
// building entered last (mgr+0x1C), 0x46-0x49 player house 1-4, 0x4A the gate, 0x4B the door
// data saved when the town was left (0x805FB248: bus, gate).
//
// A stage change goes through the stage manager (static object, fn_80161DE8 returns it):
//   fn_80163064(mgr, stage, pos*, type, angle, fadeOut, fadeIn)  = "go to <stage>, arrive at
//       <pos> facing <angle>" (fn_80162EC8 requests the change: mgr+0 next stage, fade lengths,
//       request r13-0x6510 = 1, mgr+0x1F = 1; then mgr+4 pos, +0x10 angle, +0x14 arrival type).
//       Every door calls it: a building's door in the town or the city (d_a_strNP fn_99_41F4:
//       the building's room, pos 256,0,464, type 0x4A, angle 0x8000, fades 2/2), a room's exits
//       (fn_801635D0 -> the current stage's exit table), the continue from the title.
//   fn_801630FC(structure id 0xD0xx, &stage, &pos, &type, &angle, &a, &b) = the outside of a
//       building's door in the town or city (what a room's "outside" exit uses).
//   fn_80162C40(stage, fadeOut, fadeIn) = a stage change without an arrival spot (demo scenes:
//       the bus stop sign starts the bus ride with it, fades 4/0; title, save, load).
// A warp makes exactly these calls, on the game thread between two frames, with the arguments a
// door would pass (rooms: the game's own exit table entry that leads into the room, or the front
// door for rooms with a door outside; the town and the city: a building's door spot or a position),
// so the game loads the stage, its actors, music and state as it always does. Nothing is built by
// hand. Preconditions are listed per destination (docs/WARPS.md).
//
// Test aids: CF_WARP=<target>[,<frame>] (one warp; without a frame as soon as the game is in a
// playable stage and has been for CF_WARP_SETTLE frames, default 1500 = past the continue route and
// Resetti's greeting), route token WARP=<target> with '/' for ',' (fires at its item's time,
// follows `after link` anchors; waits until the stage is playable), CF_WARP_LOG=1 (every stage
// change the game or a warp makes, with its arguments, and every arrival), CF_WARP_DUMP=<frame>
// (stage table, exit tables, every building's door spot). <target> = a name from the table
// (`museum.cafe`), `stage:<number or NAME>`, either followed by `@x,y,z` (arrival position).
#include "game/cf_warp.h"

#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <aurora/aurora.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace recomp {
void setTimeBaseBias(u64 bias);  // runtime.cpp
}

namespace recomp::warp {
namespace {

// ---- The game's stage system (USA addresses; VA() maps them on the other versions) ----------
u32 stageMgr() { return VA(0x805FB218); }        // fn_80161DE8's object
u32 curStageAddr() { return VA(0x8074B5BC); }    // r13-0x6384 (fn_8016266C)
u32 stageTable() { return VA(0x804F0C98); }      // {char* name, StageInfo*} x 68
u32 stageFlagsTable() { return VA(0x80479E60); } // u32 x 68
u32 gameClock() { return VA(0x80600B18); }       // OSCalendarTime (fn_8016D40C)
constexpr u32 kStageCount = 0x44;
constexpr u32 kField = 0x00, kCity = 0x2D;

enum : u32 { kFlagRoom = 0x10, kFlagDemo = 0x20 };

u32 curStage() { return rd32(curStageAddr()) >> 24; }
bool changePending() { return (rd32(stageMgr() + 0x1C) & 0xFF) != 0; }  // mgr+0x1F
u32 stageFlags(u32 s) { return s < kStageCount ? rd32(stageFlagsTable() + 4 * s) : 0; }

std::string stageName(u32 s) {
    if (s >= kStageCount) return "?";
    const u32 p = rd32(stageTable() + 8 * s);
    return p ? std::string(gstr(p)) : "?";
}

// A stage the player walks around in (town, city, a room), not a demo scene / test stage.
bool playable(u32 s) {
    return s < kStageCount && s != 0x42 && s != 0x43 && !(stageFlags(s) & kFlagDemo);
}

const bool sLog = std::getenv("CF_WARP_LOG") != nullptr;

// ---- Destinations -------------------------------------------------------------------------------
enum class Kind {
    Room,       // a room: entered as the game enters it (front door or the exit leading into it)
    Door,       // the town / city outside a building's door (fn_801630FC of a structure id)
    Pos,        // the town / city at a fixed position
    Demo,       // a demo scene (fn_80162C40)
    Bus,        // stepping off Kapp'n's bus (fn_80169950: stage = 0 city, 1 town)
};

struct Dest {
    const char* name;
    const char* label;
    const char* group;
    Kind kind;
    u32 stage;        // Room / Pos / Demo
    u32 structId;     // Door (and Pos relative to a building's door when set)
    float x, y, z;    // Pos: position (or offset from the door when structId is set)
    const char* note; // preconditions / what to expect
};

// Every entry uses the game's own ids (stage numbers, structure ids D0xx) from main.dol.
const Dest kDests[] = {
    // The town (FIELD) at buildings' doors (fn_801630FC, the spot a room's exit puts the player).
    {"town", "Your house (player 1's house door)", "Town", Kind::Door, kField, 0xD001, 0, 0, 0, nullptr},
    {"town.house1", "Player house 1 door", "Town", Kind::Door, kField, 0xD001, 0, 0, 0, nullptr},
    {"town.house2", "Player house 2 door", "Town", Kind::Door, kField, 0xD002, 0, 0, 0, "the town's second house plot"},
    {"town.house3", "Player house 3 door", "Town", Kind::Door, kField, 0xD003, 0, 0, 0, "the town's third house plot"},
    {"town.house4", "Player house 4 door", "Town", Kind::Door, kField, 0xD004, 0, 0, 0, "the town's fourth house plot"},
    {"town.town_hall", "Town hall door", "Town", Kind::Door, kField, 0xD013, 0, 0, 0, nullptr},
    {"town.plaza", "Plaza (in front of the town hall)", "Town", Kind::Pos, kField, 0xD013, 0, 0, 160, nullptr},
    {"town.museum", "Museum door", "Town", Kind::Door, kField, 0xD017, 0, 0, 0, nullptr},
    {"town.nook", "Nook's shop door", "Town", Kind::Door, kField, 0xD015, 0, 0, 0, nullptr},
    {"town.able", "Able Sisters door", "Town", Kind::Door, kField, 0xD016, 0, 0, 0, nullptr},
    {"town.gate", "Gate (Copper's gatehouse door)", "Town", Kind::Door, kField, 0xD014, 0, 0, 0, nullptr},
    {"town.bus_stop", "Bus stop (off Kapp'n's bus from the city)", "Town", Kind::Bus, 1, 0, 0, 0, 0,
     "as the bus drops you off: the game's own arrival (fn_80169950)"},
    {"town.beach", "Beach (south shore)", "Town", Kind::Pos, kField, 0, 1300, 28, 2860, nullptr},
    {"town.river_mouth", "River mouth (where the river meets the sea)", "Town", Kind::Pos, kField, 0, 0, 0, 0,
     "found from the town's river (see docs/WARPS.md)"},
    {"town.villager1", "Villager house 1 door", "Town", Kind::Door, kField, 0xD009, 0, 0, 0, nullptr},
    {"town.villager2", "Villager house 2 door", "Town", Kind::Door, kField, 0xD00A, 0, 0, 0, nullptr},
    {"town.villager3", "Villager house 3 door", "Town", Kind::Door, kField, 0xD00B, 0, 0, 0, nullptr},
    {"town.villager4", "Villager house 4 door", "Town", Kind::Door, kField, 0xD00C, 0, 0, 0, nullptr},
    {"town.villager5", "Villager house 5 door", "Town", Kind::Door, kField, 0xD00D, 0, 0, 0, nullptr},
    {"town.villager6", "Villager house 6 door", "Town", Kind::Door, kField, 0xD00E, 0, 0, 0, nullptr},
    {"town.villager7", "Villager house 7 door", "Town", Kind::Door, kField, 0xD00F, 0, 0, 0, nullptr},
    {"town.villager8", "Villager house 8 door", "Town", Kind::Door, kField, 0xD010, 0, 0, 0, nullptr},
    {"town.villager9", "Villager house 9 door", "Town", Kind::Door, kField, 0xD011, 0, 0, 0, nullptr},
    {"town.villager10", "Villager house 10 door", "Town", Kind::Door, kField, 0xD012, 0, 0, 0, nullptr},

    // Player houses (stage 1 + 4 x house + floor: F1, F2, basement, loft = the upper floor's room).
    {"house1", "House 1: main room", "Player houses", Kind::Room, 0x01, 0, 0, 0, 0, nullptr},
    {"house1.upstairs", "House 1: upstairs", "Player houses", Kind::Room, 0x02, 0, 0, 0, 0, "the house must have it"},
    {"house1.basement", "House 1: basement", "Player houses", Kind::Room, 0x03, 0, 0, 0, 0, "the house must have it"},
    {"house1.loft", "House 1: back room / loft", "Player houses", Kind::Room, 0x04, 0, 0, 0, 0, "the house must have it"},
    {"house2", "House 2: main room", "Player houses", Kind::Room, 0x05, 0, 0, 0, 0, "a second player"},
    {"house2.upstairs", "House 2: upstairs", "Player houses", Kind::Room, 0x06, 0, 0, 0, 0, "a second player"},
    {"house2.basement", "House 2: basement", "Player houses", Kind::Room, 0x07, 0, 0, 0, 0, "a second player"},
    {"house2.loft", "House 2: back room / loft", "Player houses", Kind::Room, 0x08, 0, 0, 0, 0, "a second player"},
    {"house3", "House 3: main room", "Player houses", Kind::Room, 0x09, 0, 0, 0, 0, "a third player"},
    {"house3.upstairs", "House 3: upstairs", "Player houses", Kind::Room, 0x0A, 0, 0, 0, 0, "a third player"},
    {"house3.basement", "House 3: basement", "Player houses", Kind::Room, 0x0B, 0, 0, 0, 0, "a third player"},
    {"house3.loft", "House 3: back room / loft", "Player houses", Kind::Room, 0x0C, 0, 0, 0, 0, "a third player"},
    {"house4", "House 4: main room", "Player houses", Kind::Room, 0x0D, 0, 0, 0, 0, "a fourth player"},
    {"house4.upstairs", "House 4: upstairs", "Player houses", Kind::Room, 0x0E, 0, 0, 0, 0, "a fourth player"},
    {"house4.basement", "House 4: basement", "Player houses", Kind::Room, 0x0F, 0, 0, 0, 0, "a fourth player"},
    {"house4.loft", "House 4: back room / loft", "Player houses", Kind::Room, 0x10, 0, 0, 0, 0, "a fourth player"},

    // Villager houses by slot (RM_NPC0-9; the label gets the villager's name when the game knows it).
    {"villager1", "Villager house 1", "Villager houses", Kind::Room, 0x11, 0, 0, 0, 0, nullptr},
    {"villager2", "Villager house 2", "Villager houses", Kind::Room, 0x12, 0, 0, 0, 0, nullptr},
    {"villager3", "Villager house 3", "Villager houses", Kind::Room, 0x13, 0, 0, 0, 0, nullptr},
    {"villager4", "Villager house 4", "Villager houses", Kind::Room, 0x14, 0, 0, 0, 0, nullptr},
    {"villager5", "Villager house 5", "Villager houses", Kind::Room, 0x15, 0, 0, 0, 0, nullptr},
    {"villager6", "Villager house 6", "Villager houses", Kind::Room, 0x16, 0, 0, 0, 0, nullptr},
    {"villager7", "Villager house 7", "Villager houses", Kind::Room, 0x17, 0, 0, 0, 0, "a 7th villager"},
    {"villager8", "Villager house 8", "Villager houses", Kind::Room, 0x18, 0, 0, 0, 0, "an 8th villager"},
    {"villager9", "Villager house 9", "Villager houses", Kind::Room, 0x19, 0, 0, 0, 0, "a 9th villager"},
    {"villager10", "Villager house 10", "Villager houses", Kind::Room, 0x1A, 0, 0, 0, 0, "a 10th villager"},

    // Town buildings.
    {"town_hall", "Town hall (Pelly / Phyllis, Tortimer)", "Town buildings", Kind::Room, 0x1B, 0, 0, 0, 0, nullptr},
    {"gate", "Gatehouse (Copper, Booker)", "Town buildings", Kind::Room, 0x1C, 0, 0, 0, 0, nullptr},
    {"gate.online", "Gatehouse, gate open (online)", "Town buildings", Kind::Room, 0x1D, 0, 0, 0, 0,
     "the online variant of the gatehouse"},
    {"nook", "Nook's Cranny", "Town buildings", Kind::Room, 0x27, 0, 0, 0, 0, "the shop's first stage"},
    {"nook.n_go", "Nook 'n' Go", "Town buildings", Kind::Room, 0x28, 0, 0, 0, 0, "loads the room even if the shop is smaller"},
    {"nook.nookway", "Nookway", "Town buildings", Kind::Room, 0x29, 0, 0, 0, 0, "loads the room even if the shop is smaller"},
    {"nook.nookington", "Nookington's ground floor", "Town buildings", Kind::Room, 0x2A, 0, 0, 0, 0,
     "loads the room even if the shop is smaller"},
    {"nook.nookington2", "Nookington's upper floor", "Town buildings", Kind::Room, 0x2B, 0, 0, 0, 0,
     "loads the room even if the shop is smaller"},
    {"able", "Able Sisters", "Town buildings", Kind::Room, 0x2C, 0, 0, 0, 0, nullptr},

    // The museum.
    {"museum", "Museum lobby (Blathers)", "Museum", Kind::Room, 0x1E, 0, 0, 0, 0, nullptr},
    {"museum.fish", "Museum: fish (aquarium)", "Museum", Kind::Room, 0x1F, 0, 0, 0, 0, nullptr},
    {"museum.insects", "Museum: insects", "Museum", Kind::Room, 0x20, 0, 0, 0, 0, nullptr},
    {"museum.fossils", "Museum: fossils, first room", "Museum", Kind::Room, 0x21, 0, 0, 0, 0, nullptr},
    {"museum.fossils2", "Museum: fossils, second room", "Museum", Kind::Room, 0x22, 0, 0, 0, 0, nullptr},
    {"museum.art", "Museum: paintings, first room", "Museum", Kind::Room, 0x23, 0, 0, 0, 0, nullptr},
    {"museum.art2", "Museum: paintings, second room", "Museum", Kind::Room, 0x24, 0, 0, 0, 0, nullptr},
    {"museum.cafe", "Museum: The Roost (Brewster)", "Museum", Kind::Room, 0x25, 0, 0, 0, 0, nullptr},
    {"museum.observatory", "Museum: observatory (Celeste)", "Museum", Kind::Room, 0x26, 0, 0, 0, 0, nullptr},

    // The city (TOWN).
    {"city", "City bus stop (off Kapp'n's bus)", "City", Kind::Bus, 0, 0, 0, 0, 0,
     "as the bus drops you off: the game's own arrival (fn_80169950); Kapp'n's first-trip talk is skipped"},
    {"city.plaza", "City plaza (fountain)", "City", Kind::Pos, kCity, 0, 752, 56, 520, nullptr},
    {"city.gracie_door", "City: GracieGrace door", "City", Kind::Door, kCity, 0xD026, 0, 0, 0, nullptr},
    {"city.shampoodle_door", "City: Shampoodle door", "City", Kind::Door, kCity, 0xD022, 0, 0, 0, nullptr},
    {"city.katrina_door", "City: Katrina's door", "City", Kind::Door, kCity, 0xD025, 0, 0, 0, nullptr},
    {"city.redd_door", "City: Crazy Redd's door", "City", Kind::Door, kCity, 0xD024, 0, 0, 0, nullptr},
    {"city.hra_door", "City: Happy Room Academy door", "City", Kind::Door, kCity, 0xD023, 0, 0, 0, nullptr},
    {"city.auction_door", "City: Lyle's auction house door", "City", Kind::Door, kCity, 0xD027, 0, 0, 0, nullptr},
    {"city.theatre_door", "City: Club LOL door", "City", Kind::Door, kCity, 0xD028, 0, 0, 0, nullptr},
    {"city.gracie", "GracieGrace", "City rooms", Kind::Room, 0x31, 0, 0, 0, 0, nullptr},
    {"city.shampoodle", "Shampoodle (Harriet)", "City rooms", Kind::Room, 0x2F, 0, 0, 0, 0, nullptr},
    {"city.katrina", "Katrina's fortune shop", "City rooms", Kind::Room, 0x30, 0, 0, 0, 0, nullptr},
    {"city.redd", "Crazy Redd's", "City rooms", Kind::Room, 0x2E, 0, 0, 0, 0, "the game asks for a membership at the door"},
    {"city.hra", "Happy Room Academy (Lyle)", "City rooms", Kind::Room, 0x32, 0, 0, 0, 0, nullptr},
    {"city.hra_model", "Happy Room Academy model room", "City rooms", Kind::Room, 0x33, 0, 0, 0, 0, nullptr},
    {"city.auction", "Lyle's auction house", "City rooms", Kind::Room, 0x36, 0, 0, 0, 0, nullptr},
    {"city.theatre", "Club LOL (ticket hall)", "City rooms", Kind::Room, 0x34, 0, 0, 0, 0, nullptr},
    {"city.theatre_show", "Club LOL stage (Dr. Shrunk)", "City rooms", Kind::Room, 0x35, 0, 0, 0, 0, nullptr},
    {"city.resetti", "Reset Surveillance Center", "City rooms", Kind::Room, 0x37, 0, 0, 0, 0,
     "the room behind the city's back door"},

    // Demo scenes.
    {"bus.to_city", "Kapp'n's bus to the city", "Scenes", Kind::Demo, 0x3D, 0, 0, 0, 0, "A advances Kapp'n's talk"},
    {"bus.to_town", "Kapp'n's bus back to the town", "Scenes", Kind::Demo, 0x3E, 0, 0, 0, 0, "A advances Kapp'n's talk"},
    {"title", "Title screen", "Scenes", Kind::Demo, 0x38, 0, 0, 0, 0, "unsaved progress is lost, as in the game"},
};

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

const Dest* findDest(const std::string& name) {
    const std::string n = lower(name);
    for (const Dest& d : kDests)
        if (n == d.name) return &d;
    return nullptr;
}

// ---- Requests -------------------------------------------------------------------------------------
struct Request {
    const Dest* dest = nullptr;  // or:
    u32 stage = ~0u;             // stage:<n>
    bool hasPos = false;
    float pos[3] = {};
    u32 frame = 0;               // 0 = as soon as possible
    std::string text;
};

std::mutex sMu;
std::vector<Request> sPending;
std::string sLastResult;  // for the menu (under sMu)
bool sLastFailed = false;  // the last warp was refused (game thread)

bool parseTarget(std::string t, Request& r) {
    r.text = t;
    if (const auto at = t.find('@'); at != std::string::npos) {
        std::string p = t.substr(at + 1);
        std::replace(p.begin(), p.end(), '/', ',');
        if (std::sscanf(p.c_str(), "%f,%f,%f", &r.pos[0], &r.pos[1], &r.pos[2]) != 3) return false;
        r.hasPos = true;
        t.resize(at);
    }
    if (lower(t).rfind("stage:", 0) == 0) {
        const std::string s = t.substr(6);
        char* end = nullptr;
        const unsigned long n = std::strtoul(s.c_str(), &end, 0);
        if (end && *end == 0 && !s.empty()) {
            r.stage = u32(n);
        } else {
            for (u32 i = 0; i < kStageCount; i++)
                if (lower(stageName(i)) == lower(s)) r.stage = i;
        }
        return r.stage < kStageCount;
    }
    r.dest = findDest(t);
    return r.dest != nullptr;
}

void queue(const Request& r) {
    std::lock_guard lk(sMu);
    sPending.push_back(r);
}

// ---- The game's calls ------------------------------------------------------------------------------
u32 scratch() {
    static u32 a = hleAlloc(0x80);
    return a;
}

// fn_801630FC for one structure id: the town/city spot outside its door. False when the town has no
// such building (the game's own check).
bool structureDoor(Ctx& c, u32 id, u32& stage, float pos[3], u32& type, u32& angle) {
    const u32 s = scratch();
    for (u32 i = 0; i < 0x40; i += 4) wr32(s + i, 0);
    c.r[7] = s + 0x14;
    c.r[8] = s + 0x18;
    c.r[9] = s + 0x1C;
    if (!guestCall(c, VA(0x801630FC), id, s + 0x00, s + 0x04, s + 0x10)) return false;
    stage = rd32(s) >> 24;
    for (int i = 0; i < 3; i++) pos[i] = rdf32(s + 4 + 4 * i);
    type = rd32(s + 0x10);
    angle = rd32(s + 0x14) >> 16;
    return true;
}

struct ExitEntry {
    u32 from;
    float pos[3];
    u32 angle, type;
};

// Exit table of a stage (static data in main.dol, the info pointer is set at boot).
std::vector<u32> exits(u32 s) {
    std::vector<u32> out;
    if (s >= kStageCount) return out;
    const u32 info = rd32(stageTable() + 8 * s + 4);
    if (!info) return out;
    const u32 tab = rd32(info + 8);
    if (!tab) return out;
    const u32 ents = rd32(tab), n = rd32(tab + 4);
    for (u32 i = 0; i < n && i < 64 && ents; i++) out.push_back(ents + 0x1C * i);
    return out;
}

// How the game enters room <s>: through its building's front door (the room has an exit outside:
// stage code 0x44-0x49) as d_a_strNP does, or through the exit of another room that leads into it.
bool roomArrival(u32 s, float pos[3], u32& type, u32& angle) {
    for (u32 e : exits(s)) {
        const u32 b = rd32(e) >> 24;
        if (b >= 0x44 && b <= 0x49) {
            pos[0] = 256;
            pos[1] = 0;
            pos[2] = 464;
            type = 0x4A;
            angle = 0x8000;
            return true;
        }
    }
    for (u32 from = 0; from < kStageCount; from++) {
        if (from == s) continue;
        for (u32 e : exits(from)) {
            const u32 w = rd32(e);
            bool hit = false;
            for (int k = 0; k < 4; k++) hit |= ((w >> (24 - 8 * k)) & 0xFF) == s;
            if (!hit) continue;
            for (int i = 0; i < 3; i++) pos[i] = rdf32(e + 4 + 4 * i);
            angle = rd32(e + 0x10) >> 16;
            type = rd32(e + 0x14);
            return true;
        }
    }
    return false;
}

// River mouth: not stored as a spot anywhere; see riverMouth() below.
bool riverMouth(Ctx& c, float pos[3]);

bool execute(Ctx& c, const Request& r, std::string& why, u32& dst) {
    const u32 cur = curStage();
    u32 stage = r.stage, type = 0xB, angle = 0;
    float pos[3] = {0, 0, 0};
    bool demo = false;
    const Dest* d = r.dest;
    if (d && d->kind == Kind::Bus) {
        // Kapp'n's end of the ride (d_a_npc_sp_kappeiNP fn_54_824): fn_80169950(fn_80169AF0(),
        // 0 = city / 1 = town, 0, 0, 0) puts the player at that end's bus stop with the bus
        // arrival (its own fn_80163064 call). The ride's flag 0x26 (set before the ride home) is
        // left alone: the warp is not a ride.
        log("[warp] f%u warp %s: off the bus in the %s, from %u %s", game::guestFrames(), r.text.c_str(),
            d->stage ? "town" : "city", cur, stageName(cur).c_str());
        dst = d->stage ? kField : kCity;
        const u32 bus = guestCall(c, VA(0x80169AF0));
        c.r[7] = 0;
        if (!guestCall(c, VA(0x80169950), bus, d->stage, 0, 0)) {
            why = "the game refused the stage change";
            return false;
        }
        return true;
    }
    if (d) {
        stage = d->stage;
        switch (d->kind) {
        case Kind::Bus: break;
        case Kind::Room:
            if (!roomArrival(stage, pos, type, angle)) {
                why = "no way into " + stageName(stage);
                return false;
            }
            break;
        case Kind::Door: {
            u32 st = 0;
            if (!structureDoor(c, d->structId, st, pos, type, angle)) {
                why = "this town has no such building";
                return false;
            }
            stage = st;
            if (stage == kCity) {
                // In the city, arriving exactly on a door spot without having been inside leaves
                // the player hidden, waiting for the door to let them out; after 292 frames
                // d_a_playerNP (fn_80_1A5F4 -> fn_80_1B304) gives up and sends them home (type
                // 0x75). One step out of the door (48 units along the door's facing) is a normal
                // arrival.
                const float a = float(angle) * (6.28318531f / 65536.0f);
                pos[0] += 48.0f * std::sin(a);
                pos[2] += 48.0f * std::cos(a);
            }
            break;
        }
        case Kind::Pos:
            if (d->structId) {
                u32 st = 0;
                if (!structureDoor(c, d->structId, st, pos, type, angle)) {
                    why = "this town has no such building";
                    return false;
                }
                stage = st;
                pos[0] += d->x;
                pos[1] += d->y;
                pos[2] += d->z;
                angle = 0;
                type = 0xB;
            } else if (!std::strcmp(d->name, "town.river_mouth")) {
                if (!riverMouth(c, pos)) {
                    why = "the river mouth was not found";
                    return false;
                }
            } else {
                pos[0] = d->x;
                pos[1] = d->y;
                pos[2] = d->z;
            }
            break;
        case Kind::Demo: demo = true; break;
        }
    } else if (stage < kStageCount) {
        if (stageFlags(stage) & kFlagDemo) demo = true;
        else if (stageFlags(stage) & kFlagRoom) {
            if (!roomArrival(stage, pos, type, angle)) {
                why = "no way into " + stageName(stage);
                return false;
            }
        } else if (!r.hasPos) {
            why = "give a position: stage:" + std::to_string(stage) + "@x,y,z";
            return false;
        }
    }
    // A villager house slot without a villager: the game has no house for it (fn_801630FC fails
    // for its structure D009+n) and would show a placeholder room.
    if (stage >= 0x11 && stage <= 0x1A) {
        u32 st = 0, t2 = 0, a2 = 0;
        float p2[3];
        if (!structureDoor(c, 0xD009 + (stage - 0x11), st, p2, t2, a2)) {
            why = "no villager lives in house slot " + std::to_string(stage - 0x11 + 1);
            return false;
        }
    }
    if (r.hasPos) {
        for (int i = 0; i < 3; i++) pos[i] = r.pos[i];
        if (!(stageFlags(stage) & kFlagRoom)) type = 0xB;
    }
    dst = stage;
    log("[warp] f%u warp %s: stage %u %s from %u %s, pos %.1f %.1f %.1f type 0x%X angle 0x%04X", game::guestFrames(),
            r.text.c_str(), stage, stageName(stage).c_str(), cur, stageName(cur).c_str(), pos[0], pos[1], pos[2], type, angle);
    u32 ok;
    if (demo) {
        // As the bus stop sign starts the ride (fn_80162C40, fades 4/0); the title like the game's
        // own return to it.
        const bool bus = stageFlags(stage) & 0x4000;
        ok = guestCall(c, VA(0x80162C40), stage, bus ? 4 : 2, bus ? 0 : 2);
    } else {
        const u32 s = scratch() + 0x60;
        for (int i = 0; i < 3; i++) wrf32(s + 4 * i, pos[i]);
        c.r[7] = angle;
        c.r[8] = 2;
        c.r[9] = 2;
        ok = guestCall(c, VA(0x80163064), stageMgr(), stage, s, type);
    }
    if (!ok) {
        why = "the game refused the stage change";
        return false;
    }
    // The stage the game will load: fn_80162EC8 stores it at mgr+0 after its own mapping
    // (fn_80162E58: the online gatehouse RM_CHKP_NET becomes RM_CHKP while not online).
    dst = rd32(stageMgr()) >> 24;
    if (dst != stage) log("[warp] the game goes to stage %u %s instead", dst, stageName(dst).c_str());
    return true;
}

// ---- River mouth ------------------------------------------------------------------------------------
// The game keeps no spot for it: the river is part of the acre models. The town's 7 x 7 acres are
// loaded by block objects (static array at 0x80574A0C, fn_8006996C; 0x74 bytes each: +0x5C BgModel
// number = BgData/BgModel/<n>_<v>.brres, +0x60 column, +0x64 row; acres are 512 units square,
// row 5 = the beach, row 6 the sea). Beach blocks whose model has river water (material
// m_river2_pattern: 075-096 and 226 on the disc) hold the river's way to the sea; a town can have
// more than one (user-town: columns 1-3). The one with the river's bridge (m_bridgeA: 077, 078,
// 082-084, 087, 088, 091, 092, 095, 096) is taken first, else the westernmost; the player stands at
// the acre's centre, 2800 (in user-town: beside the bridge over the mouth, 1792,28,2800).
bool riverMouth(Ctx&, float pos[3]) {
    const u32 arr = VA(0x80574A0C);
    if (!arr) return false;
    auto isMouth = [](u32 m) { return (m >= 75 && m <= 96) || m == 226; };
    auto hasBridge = [](u32 m) {
        return m == 77 || m == 78 || (m >= 82 && m <= 84) || m == 87 || m == 88 || m == 91 || m == 92 || m == 95 || m == 96;
    };
    int best = -1, bestCol = 0;
    u32 bestModel = 0;
    for (u32 i = 0; i < 49; i++) {
        const u32 o = arr + 0x74 * i;
        const u32 model = rd32(o + 0x5C), col = rd32(o + 0x60), row = rd32(o + 0x64);
        if (row != 5 || col > 6 || !isMouth(model)) continue;
        const int score = hasBridge(model) ? 2 : 1;
        if (score > best) {
            best = score;
            bestCol = int(col);
            bestModel = model;
        }
    }
    if (best < 0) return false;
    pos[0] = float(bestCol) * 512 + 256;
    pos[1] = 28;
    pos[2] = 2800;
    log("[warp] river mouth: acre column %d (model %03u)", bestCol, bestModel);
    return true;
}

// ---- Time of day ----------------------------------------------------------------------------------
std::atomic<bool> sKeepTime{true};
std::atomic<int> sHour{22};

void jumpToHour(int hour) {
    const u32 t = gameClock();
    const int h = int(rd32(t + 8)), m = int(rd32(t + 4)), sec = int(rd32(t));
    long long secs = (long long)((hour - h + 24) % 24) * 3600 - m * 60 - sec;
    if (secs <= 0) secs += 86400;
    const u64 before = timeBase();
    setTimeBaseBias(0);
    const u64 elapsed = timeBase();
    setTimeBaseBias(before - elapsed + u64(s64(double(secs) * kTimerClock)));
    log("[warp] clock moved %+lld s to %02d:00", secs, hour);
}

// ---- Dump (CF_WARP_DUMP) ------------------------------------------------------------------------------
void dump(Ctx& c) {
    log("[warp] dump at stage %u %s", curStage(), stageName(curStage()).c_str());
    for (u32 s = 0; s < kStageCount; s++) {
        log("[warp] stage %2u 0x%02X %-16s flags %08X", s, s, stageName(s).c_str(), stageFlags(s));
        u32 i = 0;
        for (u32 e : exits(s)) {
            log("[warp]   exit %u: stages %02X %02X %02X %02X pos %.1f %.1f %.1f angle 0x%04X type 0x%X", i++,
                rd32(e) >> 24, (rd32(e) >> 16) & 0xFF, (rd32(e) >> 8) & 0xFF, rd32(e) & 0xFF, rdf32(e + 4), rdf32(e + 8),
                rdf32(e + 12), rd32(e + 0x10) >> 16, rd32(e + 0x14));
        }
    }
    for (u32 id = 0xD000; id < 0xD045; id++) {
        u32 st = 0, type = 0, angle = 0;
        float p[3];
        if (structureDoor(c, id, st, p, type, angle))
            log("[warp] structure %04X: stage %u %s pos %.1f %.1f %.1f type 0x%X angle 0x%04X", id, st,
                stageName(st).c_str(), p[0], p[1], p[2], type, angle);
    }
}

u32 sStageSince = 0;  // guest frame the current stage was entered
u32 sLastStage = ~0u;
u32 sIssuedAt = 0, sIssuedDst = 0, sIssuedFrom = 0;  // the last warp made (0 = none / failed)

bool canWarp(u32 frame) { return playable(curStage()) && !changePending() && frame >= sStageSince + 90; }

// The last warp has arrived: the stage it asked for is loaded (a warp within the same stage, e.g.
// town -> town, reloads it without changing the stage byte: 300 frames after the change ended).
bool arrived(u32 frame) {
    if (!sIssuedAt || changePending()) return false;
    if (curStage() != sIssuedDst) return false;
    if (sStageSince > sIssuedAt) return true;
    return sIssuedDst == sIssuedFrom && frame > sIssuedAt + 300;
}

// ---- Sweep (CF_WARP_SWEEP): every destination in turn, with a screenshot of each ---------------
//   CF_WARP_SWEEP=<shot dir>[,<dwell frames>[,<via>[,<filter>]]]
// For each destination whose name contains <filter> (default: all but the demo scenes), warp
// (first to <via> when given, e.g. `town` or `town_hall`, so every warp starts from the same kind
// of place), wait for the arrival plus <dwell> frames (default 400), write <dir>/<name>.ppm and
// log `[warp] sweep <name>: ...` (arrived stage vs expected, frames taken). A warp that does not
// arrive within 4000 frames is logged as TIMEOUT and the sweep goes on.
struct Sweep {
    bool on = false;
    std::string dir, via, filter;
    u32 dwell = 400;
    size_t next = 0;
    int phase = 0;  // 0 pick next, 1 via issued, 2 dest issued, 3 arrived (dwell), 4 done
    u32 t0 = 0, arrivedAt = 0;
    const Dest* cur = nullptr;
} sSweep = [] {
    Sweep s;
    const char* v = std::getenv("CF_WARP_SWEEP");
    if (!v) return s;
    std::string f[4];
    int i = 0;
    for (const char* p = v; *p && i < 4; p++) {
        if (*p == ',') i++;
        else f[i] += *p;
    }
    s.on = !f[0].empty();
    s.dir = f[0];
    if (!f[1].empty()) s.dwell = u32(std::strtoul(f[1].c_str(), nullptr, 10));
    s.via = f[2];
    s.filter = f[3];
    return s;
}();

void sweepIssue(const char* name) {
    Request r;
    parseTarget(name, r);
    r.frame = 0;
    queue(r);
}

void sweep(u32 frame, bool ready) {
    Sweep& w = sSweep;
    if (!w.on || !ready || w.phase == 4) return;
    {
        std::lock_guard lk(sMu);
        if (!sPending.empty()) return;  // a queued warp goes first
    }
    switch (w.phase) {
    case 0:
        while (w.next < std::size(kDests)) {
            const Dest& d = kDests[w.next++];
            const bool match = w.filter.empty() ? d.kind != Kind::Demo : std::strstr(d.name, w.filter.c_str()) != nullptr;
            if (!match) continue;
            w.cur = &d;
            break;
        }
        if (!w.cur) {
            log("[warp] sweep done");
            w.phase = 4;
            return;
        }
        w.t0 = frame;
        sIssuedAt = 0;
        if (!w.via.empty() && w.via != w.cur->name) {
            sweepIssue(w.via.c_str());
            w.phase = 1;
        } else {
            sweepIssue(w.cur->name);
            w.phase = 2;
        }
        return;
    case 1:  // via: wait for it, then a moment in it
        if ((arrived(frame) && frame >= sStageSince + 120) || frame > w.t0 + 4000 || (!sIssuedAt && frame > w.t0 + 30)) {
            sweepIssue(w.cur->name);
            sIssuedAt = 0;
            w.t0 = frame;
            w.phase = 2;
        }
        return;
    case 2:
        if (arrived(frame)) {
            w.arrivedAt = frame;
            w.phase = 3;
        } else if (frame > w.t0 + 4000 || (!sIssuedAt && sLastFailed)) {
            log("[warp] sweep %s: %s (stage %u %s)", w.cur->name, sIssuedAt ? "TIMEOUT" : "FAILED", curStage(),
                stageName(curStage()).c_str());
            w.cur = nullptr;
            w.phase = 0;
        }
        return;
    case 3:
        if (frame < w.arrivedAt + w.dwell) return;
        {
            const std::string path = w.dir + "/" + w.cur->name + ".ppm";
            aurora_request_screenshot(path.c_str());
            // Still there after the dwell (the game sends a player it cannot place back home).
            const bool stayed = curStage() == sIssuedDst && sStageSince <= w.arrivedAt;
            log("[warp] sweep %s: %s, stage %u %s, %u frames to arrive", w.cur->name, stayed ? "ok" : "LEFT AGAIN",
                curStage(), stageName(curStage()).c_str(), w.arrivedAt - w.t0);
        }
        w.cur = nullptr;
        w.phase = 0;
        return;
    }
}

}  // namespace

// ---- Hooks: log the game's own stage changes ------------------------------------------------------
// fn_80163064(mgr, stage, pos*, type, angle, fadeOut, fadeIn).
HLE(fn_80163064) {
    if (sLog) {
        const u32 p = c.r[5];
        log("[warp] go to stage %u %s pos %.1f %.1f %.1f type 0x%X angle 0x%04X fades %d/%d (from %u %s, lr %08X)",
            c.r[4] & 0xFF, stageName(c.r[4] & 0xFF).c_str(), rdf32(p), rdf32(p + 4), rdf32(p + 8), c.r[6],
            c.r[7] & 0xFFFF, s32(c.r[8]), s32(c.r[9]), curStage(), stageName(curStage()).c_str(), c.lr);
    }
    callOriginal(c, "fn_80163064");
}

// fn_80162EC8(mgr, stage, fadeOut, fadeIn, force): every stage change request.
HLE(fn_80162EC8) {
    const u32 st = c.r[4] & 0xFF, lr = c.lr, fo = c.r[5], fi = c.r[6], force = c.r[7];
    callOriginal(c, "fn_80162EC8");
    if (sLog)
        log("[warp] stage change request %u %s fades %d/%d force %u -> %u (from %u %s, lr %08X)", st,
            stageName(st).c_str(), s32(fo), s32(fi), force, c.r[3], curStage(), stageName(curStage()).c_str(), lr);
}

void requestFromScript(const std::string& target) {
    Request r;
    if (!parseTarget(target, r)) {
        log("[warp] unknown warp target \"%s\" (docs/WARPS.md)", target.c_str());
        return;
    }
    queue(r);
}

void onGuestFrame(Ctx& c, u32 frame) {
    static bool envDone = false;
    static const u32 settle = [] {
        const char* v = std::getenv("CF_WARP_SETTLE");
        return v ? u32(std::strtoul(v, nullptr, 10)) : 1500u;
    }();
    static u32 firstPlayable = 0;
    const u32 s = curStage();
    if (s != sLastStage) {
        if (sLog) {
            const u32 dd = VA(0x805FB248);  // saved door data (exit code 0x4B)
            log("[warp] f%u now in stage %u %s (saved door: stage %u pos %.1f %.1f %.1f type 0x%X angle 0x%04X %d %d)", frame,
                s, stageName(s).c_str(), rd32(dd + 0x10) >> 8 & 0xFF, rdf32(dd), rdf32(dd + 4), rdf32(dd + 8), rd32(dd + 0xC),
                rd32(dd + 0x10) >> 16, s32(int8_t(rd32(dd + 0x10) & 0xFF)), s32(int8_t(rd32(dd + 0x14) >> 24)));
        }
        sLastStage = s;
        sStageSince = frame;
    }
    if (!firstPlayable && playable(s)) firstPlayable = frame;
    if (!envDone) {
        envDone = true;
        if (const char* v = std::getenv("CF_WARP")) {
            // <target>[@x,y,z][,<frame>]: the frame is the comma field after the name or after z.
            std::string t = v;
            Request r;
            const auto at = t.find('@');
            const size_t fields = size_t(std::count(t.begin() + (at == std::string::npos ? 0 : at), t.end(), ','));
            if (fields == (at == std::string::npos ? 1u : 3u)) {
                const auto comma = t.rfind(',');
                r.frame = u32(std::strtoul(t.c_str() + comma + 1, nullptr, 10));
                t.resize(comma);
            }
            if (parseTarget(t, r)) {
                if (!r.frame) r.frame = ~0u;  // "when playable + settled"
                queue(r);
            } else {
                log("[warp] CF_WARP: unknown target \"%s\" (docs/WARPS.md)", v);
            }
        }
    }
    static const u32 dumpAt = [] {
        const char* v = std::getenv("CF_WARP_DUMP");
        return v ? u32(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (dumpAt && frame == dumpAt) dump(c);

    sweep(frame, firstPlayable && frame >= firstPlayable + settle);

    std::lock_guard lk(sMu);
    if (sPending.empty()) return;
    Request& r = sPending.front();
    if (r.frame == ~0u) {
        if (!firstPlayable || frame < firstPlayable + settle) return;
    } else if (r.frame && frame < r.frame) {
        return;
    }
    // Only between scenes' worth of normal play: a playable stage, no change in progress, and a
    // short while after the last arrival (the fade-in and the arrival walk).
    if (!canWarp(frame)) return;
    if (!sKeepTime && !(r.dest && r.dest->kind == Kind::Demo)) jumpToHour(sHour);
    std::string why;
    u32 dst = 0;
    if (execute(c, r, why, dst)) {
        sLastResult = "Warped to " + r.text;
        sIssuedAt = frame;
        sIssuedDst = dst;
        sIssuedFrom = s;
        sLastFailed = false;
    } else {
        sLastResult = "Warp to " + r.text + " failed: " + why;
        sIssuedAt = 0;
        sLastFailed = true;
        log("[warp] %s", sLastResult.c_str());
    }
    sPending.erase(sPending.begin());
}

// ---- F1 > Enhancements > Debug --------------------------------------------------------------------------
void debugMenu() {
    ImGui::SeparatorText("Warp");
    ImGui::TextWrapped(
        "Load straight into any place through the game's own door / stage change: the game loads the "
        "place, its characters and music as if you had walked there. Use it from the town, the city or a "
        "room (not during the title or a bus ride).");
    static char filter[64] = "";
    ImGui::SetNextItemWidth(260 * ImGui::GetFontSize() / 13.0f);
    ImGui::InputTextWithHint("##warpfilter", "Search places", filter, sizeof filter);
    bool keep = sKeepTime;
    if (recomp::menu::Checkbox("Keep the current time and weather", &keep)) sKeepTime = keep;
    ImGui::SetItemTooltip(
        "On: the warp is like walking through a door, the clock and weather go on as they are.\n"
        "Off: the clock is moved forward to the hour below first (the weather is the game's own for\n"
        "that hour), e.g. Crazy Redd's or the observatory at night.");
    if (!keep) {
        int h = sHour;
        if (recomp::menu::SliderInt("Hour", &h, 0, 23, "%02d:00")) sHour = h;
    }
    const u32 cur = curStage();
    ImGui::Text("Now in: %s (stage %u)", stageName(cur).c_str(), cur);
    std::string last;
    {
        std::lock_guard lk(sMu);
        last = sLastResult;
    }
    if (!last.empty()) ImGui::TextDisabled("%s", last.c_str());
    const std::string f = lower(filter);
    const char* group = nullptr;
    bool open = false;
    ImGui::BeginChild("##warplist", ImVec2(0, 360 * ImGui::GetFontSize() / 13.0f), ImGuiChildFlags_Borders);
    for (const Dest& d : kDests) {
        const std::string hay = lower(std::string(d.name) + " " + d.label + " " + d.group);
        if (!f.empty() && hay.find(f) == std::string::npos) continue;
        if (!group || std::strcmp(group, d.group) != 0) {
            if (group && open) ImGui::TreePop();
            group = d.group;
            recomp::menu::openTreesForCheck();
            open = ImGui::TreeNodeEx(d.group, ImGuiTreeNodeFlags_DefaultOpen);
        }
        if (!open) continue;
        ImGui::PushID(d.name);
        // "Warp##<name>": CF_MENU_CLICK can press one by its label.
        if (recomp::menu::SmallButton((std::string("Warp##") + d.name).c_str())) {
            Request r;
            r.dest = &d;
            r.text = d.name;
            queue(r);
            std::lock_guard lk(sMu);
            sLastResult = std::string("Warping to ") + d.name + "...";
        }
        ImGui::SameLine();
        ImGui::Text("%s", d.label);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", d.name);
        if (d.note) ImGui::SetItemTooltip("%s", d.note);
        ImGui::PopID();
    }
    if (group && open) ImGui::TreePop();
    ImGui::EndChild();
}

}  // namespace recomp::warp
