// Unused items at Nook's (F1 > Enhancements > Unused content; docs/FEATURES.md "Unused items at
// Nook's"). One of the game's unused items (Wild World leftovers, the moving boxes, the shops'
// own walls and floors, the bottle and bulletin papers) is sold in place of one of the day's items
// of its category; the shop UI, prices, buying and the catalog are the game's own.
//
// What the game does (USA rev 1; the other versions run the same code, every function below is a
// hash match and "identical apart from addresses" in gen_version_map.py diff/reldiff):
// - The shop's stock lives in the save block (fn_8010DD60 / r13-0x2FE0) at + 0x630C0 ("S"):
//   S+0 the shop level (0 Nook's Cranny .. 3 Nookington's), then 36 slots of 4 bytes at S+8:
//   u16 item code (0xFFF1 = none), u8 sold flag (fn_80146524), u8 unused. fn_801456B4(S) fills
//   the whole stock for the day; it is called by fn_8014406C (the daily update, when the date at
//   save + 0x631FC is not today) and by d_shop_fgNP fn_119_A8 when the shop's level changed, just
//   before the shop room places the stock on its display points (fn_119_360).
// - Slot groups (fn_801456B4 and d_shop_fgNP fn_119_360): 0-16 clothes/tools/flowers/..., 17-24
//   furniture (count per level fn_801460E8: 2/3/5/8), 25-27 wallpaper (fn_80146118: 1/1/2/3),
//   28-30 flooring (fn_80146128: 1/1/2/3), 31-34 stationery (fn_80146138: 1/2/3/4), 35 one more.
//   The day's special furniture (seasonal / event items) is written into furniture positions 0/1
//   (levels 0-2) or 2/7 (level 3).
// - Item code = 0x9000 + item number * 4 (+ count bits: stationery sheets - 1). The item table in
//   memory: manager fn_800C24CC(), records at [mgr + 0x78] + number * 0x18C (fn_800C2704), the
//   first 0x18C bytes of each Item/item.arc item.bin record: +4 price, +8 number, +0x12 ten
//   0x22-byte UTF-16 names (JP, US en/es/fr, EU en/de/it/es/fr, KR), +0x166 kind (1 wall, 2 floor,
//   3 furniture, 9 stationery), +0x16A source (3/4/5 Nook's pools, 0x2C unused).
// - Furniture models: Item/Ftr/Ftr<furniture ordinal>.brres; the moving boxes (#E08-E0A) are
//   Ftr719-721 (int_itd_mov_box_L/M/S), so they have models.
// - Save writes: fn_800D256C .. fn_800D2830 each compute what they need (checksums over the save
//   block, e.g. fn_8010E1CC / fn_80115D0C) and start the save worker (fn_800D284C, thread
//   0x805A3000), which writes the block from memory and leaves its result in r13-0x6930 (0 while
//   running).
//
// What the port does (all only while an item is chosen; nothing is touched otherwise):
// - picks one slot of the item's category (deterministic: the last position that is not one of
//   the day's special furniture positions, sold or not, so a reload or a save gives the same slot
//   and an item bought once stays bought) and writes the unused item's code there (stationery
//   keeps the slot's sheet count); the original code is kept to put back.
// - The replacement never reaches the save file: each save starter puts the original back first
//   (before the checksum), and the replacement returns once the worker's result word is set. So
//   the save always has the day's retail stock (sold flags included), and turning the option off
//   - with the game running or not - leaves nothing behind.
// - A new day's stock (fn_801456B4) gets the replacement again at once, before the shop places it.
// - While the player is inside the shop (d_shop_fgNP linked) a change in the menu waits until they
//   leave, so the shelves and the stock agree. While in an online room the stock stays retail
//   (visitors get the town data).
// - Prices: walls, floors and moving boxes have price 0 in the game's table; while an item is
//   chosen their records get the prices below (also the Nintendo bench, pikmin and Blue Falcon:
//   2400, the user's price over the game's 120 / 1024 / 1024) (and the moving boxes and the bulletin paper names
//   in the languages that only have a placeholder); with nothing chosen the table is the disc's.
#include "game/cf_features.h"

#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/online.h"
#include "recomp/runtime.h"
#include "recomp/version.h"

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace recomp::cf {
namespace {

enum class Cat { Furniture, Wall, Floor, Stationery };

struct Item {
    u16 code;
    const char* name;  // the game's English name (US English slot)
    Cat cat;
    u32 price;            // 0: the game's own price is used (already non-zero)
    const char* group;    // menu group
    const char* tip;
    const char* names[10];  // names for placeholder slots ("not used" / "ブランク...") or null
};

// Prices (docs/FEATURES.md "The items"): Nintendo bench, pikmin and Blue Falcon 2400 (the user's
// price); the other furniture and the stationery keep the game's own table prices;
// walls 1440 = median of Nook's 54 regular wallpapers (sources 3/4/5: 800..2660, median 1435,
// rounded to 10 like every game price), floors 1540 = median of Nook's 51 regular floors
// (720..3200); moving boxes 100 Bells per box (the cardboard box, #C42, is 100) x 5 / 4 / 2.
const Item kItems[] = {
    // The user's prices (the game's 120 / 1024 / 1024 are far too cheap for these).
    {0xC0AC, "Nintendo bench", Cat::Furniture, 2400, "Furniture", "Wild World leftover.", {}},
    {0xC178, "pikmin", Cat::Furniture, 2400, "Furniture", "Wild World leftover.", {}},
    {0xC17C, "Blue Falcon", Cat::Furniture, 2400, "Furniture", "Wild World leftover.", {}},
    {0xC23C, "brown pot", Cat::Furniture, 0, "Furniture", "Wild World leftover.", {}},
    {0xC240, "white pot", Cat::Furniture, 0, "Furniture", "Wild World leftover.", {}},
    {0xC244, "moth orchid", Cat::Furniture, 0, "Furniture", "Wild World leftover.", {}},
    {0xC820, "moving boxes (5)", Cat::Furniture, 500, "Moving boxes",
     "A stack of five moving boxes (Japanese name: moving cardboard L; 'not used' in the English table,\n"
     "named here). Takes a furniture slot.",
     {nullptr, "5 moving boxes", "5 cajas mudanza", "5 cartons", "5 moving boxes", "5 Umzugskartons", "5 scatoloni",
      "5 cajas mudanza", "5 cartons", "5 moving boxes"}},
    {0xC824, "moving boxes (4)", Cat::Furniture, 400, "Moving boxes",
     "Four moving boxes (Japanese name: moving cardboard M). Takes a furniture slot.",
     {nullptr, "4 moving boxes", "4 cajas mudanza", "4 cartons", "4 moving boxes", "4 Umzugskartons", "4 scatoloni",
      "4 cajas mudanza", "4 cartons", "4 moving boxes"}},
    {0xC828, "moving boxes (2)", Cat::Furniture, 200, "Moving boxes",
     "Two moving boxes (Japanese name: moving cardboard S). Takes a furniture slot.",
     {nullptr, "2 moving boxes", "2 cajas mudanza", "2 cartons", "2 moving boxes", "2 Umzugskartons", "2 scatoloni",
      "2 cajas mudanza", "2 cartons", "2 moving boxes"}},
    {0xA0FC, "cranny wall", Cat::Wall, 1440, "Wallpaper", "The wall of Nook's Cranny.", {}},
    {0xA100, "convenient wall", Cat::Wall, 1440, "Wallpaper", "The wall of Nook 'n' Go.", {}},
    {0xA104, "supermarket wall", Cat::Wall, 1440, "Wallpaper", "The wall of Nookway.", {}},
    {0xA108, "department wall", Cat::Wall, 1440, "Wallpaper", "The wall of Nookington's.", {}},
    {0xA414, "cranny floor", Cat::Floor, 1540, "Flooring", "The floor of Nook's Cranny.", {}},
    {0xA418, "convenient floor", Cat::Floor, 1540, "Flooring", "The floor of Nook 'n' Go.", {}},
    {0xA41C, "market floor", Cat::Floor, 1540, "Flooring", "The floor of Nookway (supermarket floor).", {}},
    {0xA420, "department floor", Cat::Floor, 1540, "Flooring", "The floor of Nookington's.", {}},
    {0x9670, "weathered paper", Cat::Stationery, 0, "Stationery",
     "Wild World's bottle paper. Sold with the sheet count of the stationery it replaces (codes 9670-9673 = 1-4 sheets).",
     {}},
    {0x974C, "bulletin paper", Cat::Stationery, 0, "Stationery",
     "The town hall bulletin board's paper (codes 974C-974F = 1-4 sheets). Can only be used at the town hall's\n"
     "bulletin board, as in the game's own design.",
     {nullptr, nullptr, "papel de tablón", "papier babillard", nullptr, "Brettpapier", "carta bacheca", "papel de tablón",
      "papier babillard", nullptr}},
};
constexpr int kItemCount = int(sizeof(kItems) / sizeof(kItems[0]));

const Item* find(u32 code) {
    for (const Item& it : kItems)
        if (it.code == (code & ~3u) || (it.cat != Cat::Stationery && it.code == code)) return &it;
    return nullptr;
}
u32 numberOf(u32 code) { return ((code - 0x9000u) >> 2) & 0xFFFF; }

constexpr u32 kStockOff = 0x630C0;  // shop stock in the save block (d_shop_fgNP fn_119_360)
constexpr u32 kSlots = 36;
constexpr u16 kNone = 0xFFF1;
constexpr u32 kRecSize = 0x18C, kTableCount = 0xA15;
constexpr u32 kShopModule = 119;  // d_shop_fgNP (the same id in all four versions)

struct Addrs {
    u32 saveBlock;     // r13-0x2FE0: the save block pointer (fn_8010DD60)
    u32 playerMgr;     // player numbers' save slots at +0x34
    u32 saveResult;    // r13-0x6930: save worker result (0 = running)
    u32 itemMgr;       // fn_800C24CC
    u32 countFn[4];    // furniture / wall / floor / stationery counts per shop level
};
const Addrs& addrs() {
    static const Addrs a{VA(0x8074E960), VA(0x805D26C0), VA(0x8074B010), VA(0x800C24CC),
                         {VA(0x801460E8), VA(0x80146118), VA(0x80146128), VA(0x80146138)}};
    return a;
}
constexpr u32 kSlotBase[4] = {17, 25, 28, 31};

bool logOn() {
    static const bool on = std::getenv("CF_UNUSED_LOG") != nullptr;
    return on;
}

// Session state (guest thread; the menu reads sStatus under sMutex).
struct Applied {
    bool on = false;
    u32 stock = 0;  // S
    u32 slot = 0;
    u16 orig = 0, repl = 0;
};
Applied sApplied;
bool sSuspended = false;   // a save is being written: the stock is retail until it finishes
bool sTableTouched = false;
u32 sOrigPrice[kItemCount] = {};
u16 sOrigNames[kItemCount][10][17] = {};
bool sNamesTouched[kItemCount][10] = {};

struct Status {
    bool inGame = false, inShop = false, suspended = false, online = false;
    bool applied = false;
    u32 slot = 0;
    u16 orig = 0, repl = 0;
    std::string origName;
    std::string note;
};
std::mutex sMutex;
Status sStatus;

u32 stockAddr() {
    const u32 block = rd32(addrs().saveBlock);
    if (block < 0x80004000u || block >= 0x94000000u) return 0;
    return block + kStockOff;
}
u32 slotAddr(u32 stock, u32 slot) { return stock + 8 + 4 * slot; }

std::string utf16Name(u32 a) {
    std::string s;
    for (u32 i = 0; i < 16; i++) {
        const u32 ch = rd16(a + 2 * i);
        if (!ch) break;
        s += ch < 0x80 ? char(ch) : '?';
    }
    return s;
}

// The item's table record (null when the table is not loaded or the record is not the item).
u32 record(Ctx& c, u32 code) {
    const u32 mgr = guestCall(c, addrs().itemMgr);
    if (!mgr) return 0;
    const u32 base = rd32(mgr + 0x78);
    const u32 num = numberOf(code);
    if (!base || num >= kTableCount) return 0;
    const u32 rec = base + num * kRecSize;
    if (rd16(rec + 8) != num || rd16(rec + 0x10) != 0x1701) return 0;
    return rec;
}
std::string nameOf(Ctx& c, u32 code) {
    const u32 rec = record(c, code);
    return rec ? utf16Name(rec + 0x12 + 0x22) : std::string();
}

bool placeholder(u32 a) {
    // "not used" or "ブランク..." (blank + language) in the game's table
    static const char kNotUsed[] = "not used";
    bool notUsed = true;
    for (u32 i = 0; i < 9; i++)
        if (rd16(a + 2 * i) != u16(i < 8 ? kNotUsed[i] : 0)) notUsed = false;
    return notUsed || (rd16(a) == 0x30D6 && rd16(a + 2) == 0x30E9 && rd16(a + 4) == 0x30F3);
}
void writeName(u32 a, const char* utf8) {
    // Latin-1 / BMP UTF-8 -> UTF-16BE, at most 16 characters + terminator.
    u32 n = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p && n < 16;) {
        u32 ch = *p++;
        if (ch >= 0xE0 && p[0] && p[1]) {
            ch = (ch & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F);
            p += 2;
        } else if (ch >= 0xC0 && p[0]) {
            ch = (ch & 0x1F) << 6 | (p[0] & 0x3F);
            p += 1;
        }
        wr16(a + 2 * n++, u16(ch));
    }
    for (; n < 17; n++) wr16(a + 2 * n, 0);
}

// Prices and names in the item table: ours while an item is chosen, the disc's otherwise.
void updateTable(Ctx& c, bool on) {
    if (!on && !sTableTouched) return;
    for (int i = 0; i < kItemCount; i++) {
        const Item& it = kItems[i];
        const u32 rec = record(c, it.code);
        if (!rec) continue;
        if (it.price) {
            if (on) {
                const u32 cur = rd32(rec + 4);
                if (cur != it.price) {
                    if (!(sOrigPrice[i] & 0x80000000u)) sOrigPrice[i] = cur | 0x80000000u;  // high bit: saved
                    wr32(rec + 4, it.price);
                }
            } else if (sOrigPrice[i] & 0x80000000u) {
                wr32(rec + 4, sOrigPrice[i] & 0x7FFFFFFFu);
                sOrigPrice[i] = 0;
            }
        }
        for (int l = 0; l < 10; l++) {
            const char* nm = it.names[l];
            const u32 a = rec + 0x12 + 0x22 * l;
            if (on && nm && placeholder(a)) {  // (again after a reload of the table)
                if (!sNamesTouched[i][l])
                    for (u32 k = 0; k < 17; k++) sOrigNames[i][l][k] = u16(rd16(a + 2 * k));
                writeName(a, nm);
                sNamesTouched[i][l] = true;
            } else if (!on && sNamesTouched[i][l]) {
                for (u32 k = 0; k < 17; k++) wr16(a + 2 * k, sOrigNames[i][l][k]);
                sNamesTouched[i][l] = false;
            }
        }
    }
    sTableTouched = on;
}

void logStock(u32 stock, const char* why) {
    if (!logOn() || !stock) return;
    char line[1024];
    int k = std::snprintf(line, sizeof line, "[unused] f%u stock %08X (%s) level %u:", game::guestFrames(), stock, why,
                          rd32(stock));
    for (u32 s = 0; s < kSlots && k < int(sizeof line) - 16; s++)
        k += std::snprintf(line + k, sizeof line - k, " %u:%04X%s", s, rd16(slotAddr(stock, s)),
                           rd8(slotAddr(stock, s) + 2) ? "*" : "");
    log("%s", line);
}

// Puts the original back (if the slot still holds our item).
void restore(const char* why) {
    if (!sApplied.on) return;
    const u32 a = slotAddr(sApplied.stock, sApplied.slot);
    if (rd16(a) == sApplied.repl) wr16(a, sApplied.orig);
    if (logOn())
        log("[unused] f%u restored slot %u: %04X -> %04X (%s)", game::guestFrames(), sApplied.slot, sApplied.repl,
            sApplied.orig, why);
    sApplied = {};
}

// The slot of the category the item goes in (deterministic for the day's stock).
int chooseSlot(Ctx& c, u32 stock, Cat cat) {
    const u32 level = rd32(stock);
    if (level > 3) return -1;
    const int ci = int(cat);
    const int count = int(guestCall(c, addrs().countFn[ci], stock));
    if (count <= 0 || count > 8) return -1;
    auto special = [&](int i) {  // the day's seasonal / event furniture positions (fn_801456B4)
        if (cat != Cat::Furniture) return false;
        return level == 3 ? (i == 2 || i == 7) : (i == 0 || i == 1);
    };
    int pick = -1;
    for (int i = count - 1; i >= 0 && pick < 0; i--)
        if (!special(i) && rd16(slotAddr(stock, kSlotBase[ci] + i)) != kNone) pick = i;
    for (int i = count - 1; i >= 0 && pick < 0; i--)
        if (rd16(slotAddr(stock, kSlotBase[ci] + i)) != kNone) pick = i;
    return pick < 0 ? -1 : int(kSlotBase[ci]) + pick;
}

bool apply(Ctx& c, u32 stock, const Item& it, const char* why) {
    const int slot = chooseSlot(c, stock, it.cat);
    if (slot < 0) {
        if (logOn()) log("[unused] f%u no %s slot in the stock", game::guestFrames(), it.group);
        return false;
    }
    const u32 a = slotAddr(stock, u32(slot));
    const u16 orig = u16(rd16(a));
    const u16 repl = u16(it.cat == Cat::Stationery ? (it.code | (orig & 3)) : it.code);
    wr16(a, repl);
    sApplied = {true, stock, u32(slot), orig, repl};
    if (logOn())
        log("[unused] f%u slot %u: %04X -> %04X %s (%s)%s", game::guestFrames(), slot, orig, repl, it.name, why,
            rd8(a + 2) ? " (sold)" : "");
    logStock(stock, why);
    return true;
}

bool playerIn() { return rd32(addrs().playerMgr + 0x34) < 4; }
bool inShop() { return relBase(kShopModule, 1) != 0; }

const Item* wanted() { return find(u32(port::settings().unusedContent.nookItem)); }

// Brings the stock to what the setting asks for. `fromGenerator`: a new stock was just made
// (place the item at once, also inside the shop: the shelves are filled right after).
void reconcile(Ctx& c, bool fromGenerator) {
    const Item* want = playerIn() ? wanted() : nullptr;
    const bool online = online::active();
    if (sSuspended) return;
    const u32 stock = stockAddr();
    if (sApplied.on && (sApplied.stock != stock || rd16(slotAddr(sApplied.stock, sApplied.slot)) != sApplied.repl)) {
        if (logOn()) log("[unused] f%u slot %u no longer holds %04X: dropped", game::guestFrames(), sApplied.slot, sApplied.repl);
        sApplied = {};
    }
    if (!fromGenerator && inShop()) return;  // after the shop is left
    if (online || !want || !stock) {
        restore(online ? "online room" : "option off");
        return;
    }
    if (sApplied.on && (sApplied.repl & ~3u) == want->code) return;
    restore("another item chosen");
    apply(c, stock, *want, fromGenerator ? "new stock" : "chosen");
}

}  // namespace

// fn_801456B4(S): the day's stock.
HLE(fn_801456B4) {
    const u32 stock = c.r[3];
    callOriginal(c, "fn_801456B4");
    if (sApplied.on && sApplied.stock == stock) sApplied = {};  // the old stock is gone
    logStock(stock, "generated");
    if (stock == stockAddr()) {
        u32 r[32];
        std::memcpy(r, c.r, sizeof r);
        const u32 lr = c.lr;
        reconcile(c, true);
        std::memcpy(c.r, r, sizeof r);
        c.lr = lr;
    }
}

// The save-write starters: the stock goes back to retail before the save's checksum.
// Always suspends, also with nothing applied: no replacement may be written while the worker
// writes the block (e.g. an item ticked during "Saving...").
static void beforeSave(const char* which) {
    restore(which);
    sSuspended = true;
    if (logOn()) log("[unused] f%u save starter %s: stock retail until the save is written", game::guestFrames(), which);
}
#define CF_SAVE_HOOK(fn)          \
    HLE(fn) {                     \
        beforeSave(#fn);          \
        callOriginal(c, #fn);     \
    }
CF_SAVE_HOOK(fn_800D256C)
CF_SAVE_HOOK(fn_800D25D8)
CF_SAVE_HOOK(fn_800D2648)
CF_SAVE_HOOK(fn_800D26B0)
CF_SAVE_HOOK(fn_800D2720)
CF_SAVE_HOOK(fn_800D2780)
CF_SAVE_HOOK(fn_800D27E0)
CF_SAVE_HOOK(fn_800D2830)
#undef CF_SAVE_HOOK

void unusedItemsOnFrame(Ctx& c) {
    // Registers of the HLE call this runs in are kept (guestCall uses r3..r6 and lr).
    u32 r[32];
    std::memcpy(r, c.r, sizeof r);
    const u32 lr = c.lr, ctr = c.ctr;
    u8 cr[8];
    std::memcpy(cr, c.cr, sizeof cr);

    // The save worker leaves its result when the file is written (0 while it runs).
    if (sSuspended && rd32(addrs().saveResult) != 0) {
        sSuspended = false;
        if (logOn()) log("[unused] f%u save finished (result %d): resumed", game::guestFrames(), s32(rd32(addrs().saveResult)));
    }
    const Item* want = wanted();
    updateTable(c, want != nullptr);
    reconcile(c, false);

    // CF_UNUSED_LOG: once per shop visit, the room's item grid (16 x 16 tiles; fn_80190D68(0) =
    // the room's item layer, fn_8008C850(layer, x, z, 0) = the item at a tile, as d_shop_fgNP's
    // display-point search fn_8015F994 reads it), to find the replacement on the shelves.
    static bool sGridLogged = false;
    if (!inShop()) sGridLogged = false;
    else if (logOn() && !sGridLogged && game::guestFrames() % 30 == 0) {
        const u32 layer = guestCall(c, VA(0x80190D68), 0);
        if (layer) {
            std::string line;
            int n = 0;
            for (u32 z = 0; z < 16; z++)
                for (u32 x = 0; x < 16; x++) {
                    const u32 p = guestCall(c, VA(0x8008C850), layer, x, z, 0);
                    if (!p || rd16(p) == kNone) continue;
                    char b[40];
                    std::snprintf(b, sizeof b, " (%u,%u):%04X%s", x, z, rd16(p),
                                  sApplied.on && rd16(p) == sApplied.repl ? "<-" : "");
                    line += b;
                    n++;
                }
            if (n) {
                log("[unused] f%u shop room items:%s", game::guestFrames(), line.c_str());
                sGridLogged = true;
            }
        }
    }

    Status st;
    st.inGame = playerIn() && stockAddr();
    st.inShop = inShop();
    st.suspended = sSuspended;
    st.online = online::active();
    st.applied = sApplied.on;
    if (sApplied.on) {
        st.slot = sApplied.slot;
        st.orig = sApplied.orig;
        st.repl = sApplied.repl;
        st.origName = nameOf(c, sApplied.orig);
    }

    std::memcpy(c.r, r, sizeof r);
    c.lr = lr;
    c.ctr = ctr;
    std::memcpy(c.cr, cr, sizeof cr);
    std::lock_guard lk(sMutex);
    sStatus = st;
}

bool unusedItemsMenu() {
    auto& sel = port::settings().unusedContent.nookItem;
    bool dirty = false;
    ImGui::SeparatorText("Unused items at Nook's");
    ImGui::TextWrapped(
        "Nook's sells the ticked item in place of one of the day's items of the same kind (furniture, wallpaper, "
        "flooring, stationery; moving boxes take a furniture spot), at a normal price. One at a time. Unticking "
        "brings back what was for sale. The save file always keeps the day's own stock.");
    const char* group = nullptr;
    for (const Item& it : kItems) {
        if (!group || std::strcmp(group, it.group) != 0) {
            group = it.group;
            ImGui::TextDisabled("%s", group);
        }
        char label[96];
        if (it.price)
            std::snprintf(label, sizeof label, "%s (%04X, %u Bells)##unused%04X", it.name, it.code, it.price, it.code);
        else
            std::snprintf(label, sizeof label, "%s (%04X)##unused%04X", it.name, it.code, it.code);
        bool on = sel == it.code;
        if (recomp::menu::Checkbox(label, &on)) {
            sel = on ? it.code : 0;  // ticking one unticks the others
            dirty = true;
        }
        ImGui::SetItemTooltip("%s%s", it.tip,
                              it.price ? "" : "\nPrice: the game's own for this item.");
    }
    Status st;
    {
        std::lock_guard lk(sMutex);
        st = sStatus;
    }
    if (!sel) {
        ImGui::TextDisabled("Nothing chosen: Nook's stock is the game's.");
    } else if (!st.inGame) {
        ImGui::TextDisabled("Takes effect once a resident is in town.");
    } else if (st.online) {
        ImGui::TextDisabled("Paused while in an online room (the stock is the game's).");
    } else if (st.applied) {
        ImGui::TextDisabled("Today: shop spot %u, %s (%04X) replaced.%s", st.slot,
                            st.origName.empty() ? "?" : st.origName.c_str(), st.orig,
                            st.inShop ? " Changes apply when you leave the shop." : "");
    } else if (st.inShop) {
        ImGui::TextDisabled("Applies when you leave the shop (the shelves are already filled).");
    } else if (st.suspended) {
        ImGui::TextDisabled("Saving...");
    }
    return dirty;
}

}  // namespace recomp::cf
