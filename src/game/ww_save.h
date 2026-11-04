#pragma once
// Animal Crossing: Wild World (Nintendo DS) save files, for the Move In from Wild World feature
// (docs/MOVE_IN.md). Host-only code, no guest access (unit-tested by cf_move_in_test).
//
// What City Folk receives from its Moving Van DS program is the Wild World cartridge's backup
// memory (256 KB). Its reader (USA main.dol fn_80085388 .. fn_80085D3C, d_a_npc_sp_player_selectNP)
// interprets it with per-region tables that this module mirrors (layout() below; the numbers are
// the game's own tables at USA 0x8046DC70 / 0x8046DCDC / 0x8046DD18 / 0x8046DD28 / 0x8046DD38):
// two copies of the save, each checked by "the 16-bit little-endian sum of the copy is 0", four
// player slots, a resident exists when its player id and town id are both nonzero.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ww {

// Cartridge / disc regions, numbered like City Folk's region getter (USA fn_801069D0).
enum class Region : int { JP = 0, US = 1, EU = 2, KR = 3 };
const char* regionName(Region r);

// Save layouts: Japan, North America + Europe (identical), Korea.
enum class Layout : int { JP = 0, USEU = 1, KR = 2 };
Layout layoutOf(Region r);
const char* layoutName(Layout l);

struct LayoutInfo {
    uint32_t copySize;       // one save copy (checksummed)
    uint32_t playerStride;   // one player slot
    uint32_t townName;       // copy-relative: town name
    uint32_t players;        // copy-relative: first player slot
    uint32_t townMarker[3];  // copy-0-relative: u16 + 2 bytes the game checks (town data present)
    // player-relative fields, in the game's table order
    uint32_t flags;          // [0] bit 0
    uint32_t face;           // [1] low nibble (face), high nibble (hair style) at the same byte
    uint32_t hairColor;      // [3] low 3 bits
    uint32_t playerId;       // [4] u16 LE
    uint32_t townId;         // [5] u16 LE
    uint32_t name;           // [6] player name
    uint32_t gender;         // [7] byte (0 boy, 1 girl)
    uint32_t catalog;        // [8] catalog block (converted by the game's fn_800C51D8)
    int nameLen;             // characters (JP/KR 6, USA/EU 8)
    int charBytes;           // 1, or 2 for Korea (u16 LE)
};
const LayoutInfo& layout(Layout l);

constexpr uint32_t kBackupSize = 0x40000;  // the cartridge's flash (and City Folk's receive buffer)

struct Player {
    bool exists = false;
    uint16_t playerId = 0, townId = 0;
    uint8_t gender = 0, face = 0, hair = 0, hairColor = 0;
    std::vector<uint8_t> nameRaw;  // nameLen characters as stored
};

struct Save {
    std::vector<uint8_t> image;  // kBackupSize bytes, as the DS program would read the flash
    std::string format;          // "Raw backup (256 KB)", "DeSmuME .dsv (footer removed)", ...
    Layout layout = Layout::USEU;
    bool copyOk[2] = {false, false};
    int copy = -1;               // the copy the game would read (0/1), -1 none valid
    bool townData = false;       // the town marker the game tests (USA fn_80085C88) says a town exists
    std::vector<uint8_t> townRaw;
    Player players[4];
    int residents() const;
};

// Parses a save file (raw 256/512 KB flash dump, DeSmuME .dsv, No$GBA uncompressed). `error`
// gets a reason when it returns false.
bool load(const std::vector<uint8_t>& file, Save& out, std::string& error);
bool loadFile(const std::string& path, Save& out, std::string& error);

// The cartridge region a save most likely came from: the layout decides Japan / Korea; for the
// shared North America / Europe layout the file name ("(Europe)", "(EUR)", "(PAL)", ...) decides,
// else North America.
Region guessRegion(const Save& s, const std::string& path);

// Character tables: Wild World code -> Unicode, read from the running game (USA 0x8046D870:
// Japanese at +0, North America / Europe at +0x200). Korean names are UTF-16 LE.
using CharTable = std::array<char16_t, 256>;
std::u16string decodeName(const uint8_t* p, int n, Layout l, const CharTable& jp, const CharTable& latin);
std::string utf8(const std::u16string& s);

// 16-bit little-endian sum (the save checksum: 0 for a valid copy).
uint16_t sum16(const uint8_t* p, uint32_t n);

// Re-lays a save out for another region's game (F1 "Allow Wild World saves of other regions"):
// both copies get the town name, the town marker and, per player slot, every field the game's
// import reads (face, hair, colour, ids, name, gender, catalog block), names re-encoded through
// the character tables (characters the target set lacks become '?'), each copy's checksum fixed
// up in its first halfword. The result is kBackupSize bytes.
std::vector<uint8_t> convert(const Save& s, Layout to, const CharTable& jp, const CharTable& latin);

// CRC-32 (reflected 0xEDB88320, init and final xor ~0), the Moving Van protocol's checksum
// (City Folk USA fn_802A9A20).
uint32_t crc32(const uint8_t* p, size_t n);

}  // namespace ww
