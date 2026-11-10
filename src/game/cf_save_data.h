#pragma once
// City Folk's save file and the Wii's save containers, as data (no game or runtime state):
// rvforest.dat's own checksums and what it says (town, residents, region), banner.bin, and the
// SD-card export data.bin (docs/NAND.md "Importing a Wii or Dolphin save"). Used by the save import
// (cf_save_import.cpp) and cf_save_import_test.
//
// data.bin is encrypted with the Wii's SD-card key and IV. They are Nintendo's and are NOT part of
// this port: decryptDataBin() takes them from the caller (the player's own copy), and nothing here
// stores them.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cfsave {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kSaveSize = 0x40F340;  // rvforest.dat, every version (4,256,576 bytes)
constexpr u32 kResidentBase = 0x20, kResidentSize = 0x86C0, kResidents = 4;
constexpr u32 kTownStruct = 0x7EC2, kPlayerStruct = 0x7ED8;  // resident + : u16 id, 9 UTF-16BE units
constexpr u32 kTownRegionByte = 0x735C2;  // high nibble language index, low nibble region group
constexpr u32 kBannerMaxSize = 0xF0A0;    // WIBN header + banner + 8 icons (the game writes 61,600)

// The four City Folk titles: 00010000-5255554X.
enum Region : int { kJapan = 0, kUsa = 1, kEurope = 2, kKorea = 3, kUnknown = -1 };
constexpr u32 kGameCodes[4] = {0x5255554A /*RUUJ*/, 0x52555545 /*RUUE*/, 0x52555550 /*RUUP*/, 0x5255554B /*RUUK*/};
Region regionOfGameCode(u32 code);  // kUnknown for other games
const char* regionName(Region r);    // "Japan", "USA", "Europe", "Korea", "unknown"
std::string gameCodeText(u32 code);  // "RUUE"
std::string titleHex(u32 code);      // "52555545" (the NAND folder name, lower case)

// CRC-32 (reflected 0xEDB88320 table, the game's fn_802A9A20): crc32(data, n, init) ^ xorout.
u32 crc32(const u8* data, size_t n, u32 init = 0xFFFFFFFF, u32 xorout = 0xFFFFFFFF);

// One of the save's checksums (the save writers fn_8010E1CC / fn_80115E08 / fn_8011719C; the same
// offsets in all four versions, gen_version_map.py diff): big-endian CRC-32 at `at` over
// [start, start + len).
struct Checksum {
    const char* what;
    u32 at, start, len, init;
};
const std::vector<Checksum>& checksums();
// Recomputes every checksum (a test or a repaired file); the file must be kSaveSize long.
void fixChecksums(std::vector<u8>& save);

struct SaveInfo {
    bool sizeOk = false;
    std::vector<std::string> badChecksums;  // what each failing checksum covers
    std::string town;                       // UTF-8
    u16 townId = 0;
    std::vector<std::string> residents;     // UTF-8 names, in slot order
    Region townRegion = kUnknown;           // from the town byte (the region the town was made in)
    int townLanguage = -1;                  // its language index (0 JP, 1-3 USA, 4-8 Europe, 9 Korea)
    bool valid() const { return sizeOk && badChecksums.empty() && !residents.empty(); }
    std::string problem() const;            // why not valid ("" when valid)
};
SaveInfo inspectSave(const std::vector<u8>& save);

// banner.bin: "WIBN" magic, at most kBannerMaxSize bytes.
bool bannerLooksValid(const std::vector<u8>& banner);

// ---- data.bin (SD card: private/wii/title/<code>/data.bin) ----------------------------------------
// Layout (public Wii documentation, WiiBrew "Savegame Files"): 0xF0C0-byte header encrypted with
// AES-128-CBC (SD key, SD IV): title id, banner size, permissions, MD5, the banner (WIBN); then the
// "Bk" backup header in plain text (0x70 + 0x10 padding: NG id, file count, sizes, the title id
// again at +0x60, MAC address); then per file a plain 0x80-byte header (magic 0x03ADF17E, size,
// permissions, attributes, type 1 file / 2 folder, name from +0x0B, the data's IV at +0x50) and the
// data padded to 0x40, AES-128-CBC with the SD key and that IV; then the console's signature and
// certificates. Reading needs only the key and the IVs: the signature and certificates only let a
// Wii check where the file came from, and the header's MD5 needs a further Nintendo constant (the
// "MD5 blanker"), so the key is checked by the decrypted header's title id matching the plain one.
struct DataBinFile {
    std::string name;
    u32 size = 0;
    u8 perm = 0, attr = 0, type = 0;
    size_t dataOffset = 0;  // in data.bin
    std::array<u8, 16> iv{};
};
struct DataBin {
    u64 titleId = 0;  // from the plain Bk header
    u32 ngId = 0;
    std::vector<DataBinFile> files;
};
constexpr size_t kDataBinHeaderSize = 0xF0C0, kBkHeaderSize = 0x80;
// The plain parts (no key needed): false with `error` set when it is not a data.bin.
bool parseDataBin(const std::vector<u8>& bin, DataBin& out, std::string& error);
struct DataBinContents {
    std::vector<u8> banner;
    std::vector<std::pair<std::string, std::vector<u8>>> files;  // plain files of the data folder
};
// Decrypts with the player's SD key / IV. False with `error` ("the key is not right", ...).
bool decryptDataBin(const std::vector<u8>& bin, const DataBin& info, const u8 sdKey[16], const u8 sdIv[16],
                    DataBinContents& out, std::string& error);
// Builds a data.bin (tests only: synthetic files with a made-up key; signature/certificates zero).
std::vector<u8> buildDataBin(u64 titleId, const std::vector<u8>& banner,
                             const std::vector<std::pair<std::string, std::vector<u8>>>& files, const u8 key[16],
                             const u8 iv[16]);

// 16 bytes from 32 hex digits (spaces, ':' and "0x" ignored) or a 16-byte binary file's contents.
bool parseKey16(const std::string& textOrBinary, u8 out[16]);

// AES-128 (FIPS 197) in CBC mode; len a multiple of 16; iv is updated as CBC chains it.
void aes128CbcDecrypt(const u8 key[16], u8 iv[16], u8* data, size_t len);
void aes128CbcEncrypt(const u8 key[16], u8 iv[16], u8* data, size_t len);
bool aesSelfTest();  // FIPS 197 appendix C.1

}  // namespace cfsave
