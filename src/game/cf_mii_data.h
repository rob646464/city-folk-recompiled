#pragma once
// Wii Mii data and the Mii database file (docs/MIIS.md). Host-only code, no guest access: the
// 74-byte Mii record RFL keeps for each Mii (RFLiCharData), the RFL store data (+ CRC), the
// database file /shared2/menu/FaceLib/RFL_DB.dat, and the single-Mii files players exchange.
//
// Every layout and limit here was read from City Folk's own RFL library (USA rev 1):
// convertRaw2InfoCore_ / convertInfo2RawCore_ (bit fields), RFLiCheckValidInfo (limits),
// RFLiCalculateCRC / RFLiCreateHeaderCRCAsync / bootloadCheckCRCCb_ (CRC), RFLiFormatAsync and
// RFLiClearDBBuffer (file layout), RFLiGetCharData / RFLiIsValidOnNAND (what counts as a Mii).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace recomp::mii {

constexpr size_t kRawSize = 0x4A;       // RFLiCharData
constexpr size_t kStoreSize = 0x4C;     // RFLStoreData: raw + CRC-16 (big endian)
constexpr int kMaxMiis = 100;           // official database slots
constexpr size_t kDbMiisOff = 0x4;      // after "RNOD"
constexpr size_t kDbMainSize = 0x1F1E0; // what RFL reads at boot: header, 100 Miis, parade table, CRC
constexpr size_t kDbCrcOff = 0x1F1DE;   // CRC-16 of [0, kDbCrcOff)
constexpr size_t kDbHiddenEntries = 10000, kDbHiddenEntry = 0x40;
constexpr size_t kDbFileSize = kDbMainSize + kDbHiddenEntries * kDbHiddenEntry;  // as RFLiFormatAsync writes it

// One editable attribute of the record: a bit field of a big-endian 16-bit word.
enum class F : int {
    Gender, BirthMonth, BirthDay, FavColor, Favorite, Height, Weight,
    FaceShape, SkinTone, FaceFeature, NoMingle, Downloaded,
    HairStyle, HairColor, HairFlip,
    BrowType, BrowRotation, BrowColor, BrowSize, BrowY, BrowX,
    EyeType, EyeRotation, EyeY, EyeColor, EyeSize, EyeX,
    NoseType, NoseSize, NoseY,
    MouthType, MouthColor, MouthSize, MouthY,
    GlassesType, GlassesColor, GlassesSize, GlassesY,
    MustacheType, BeardType, FacialHairColor, MustacheSize, MustacheY,
    MoleOn, MoleSize, MoleY, MoleX,
    Count
};

struct FieldInfo {
    uint8_t word;   // byte offset of the big-endian u16 in the record
    uint8_t shift;  // lowest bit of the field
    uint8_t bits;
    uint8_t min, max;  // RFLiCheckValidInfo's accepted range
};
const FieldInfo& fieldInfo(F f);

struct Mii {
    uint8_t raw[kRawSize] = {};

    int get(F f) const;
    void set(F f, int v);  // clamped to the field's range
    std::u16string name() const;
    std::u16string creator() const;
    void setName(const std::u16string& s);
    void setCreator(const std::u16string& s);
    uint32_t miiId() const;       // +0x18; 0 = empty slot
    uint32_t systemId() const;    // +0x1C
    void setIds(uint32_t miiId, uint32_t systemId);
    bool empty() const { return miiId() == 0 && systemId() == 0; }
    // RFLiGetCharData's tests: an ID, not a temporary Mii (ID bit 29), RFLiCheckValidInfo
    // (every field in range, a name, a real birthday or none, a special Mii never set to stay
    // home). Empty = valid; else the first problem in plain words.
    std::string problem() const;
    bool sameIdAs(const Mii& o) const { return miiId() == o.miiId() && systemId() == o.systemId(); }
};

// CRC-16 as RFL computes it (polynomial 0x1021, initial 0, the message followed by two zero
// bytes: CRC-16/XMODEM).
uint16_t crc16(const uint8_t* data, size_t len);

// RFLStoreData (the raw record + its CRC) and back. False: wrong size or CRC.
void toStoreData(const Mii& m, uint8_t out[kStoreSize]);

// A new Mii ID as the Mii Channel makes them: bit 31 set (a normal, not special, Mii), the
// creation time in 4-second steps since 2006-01-01 in the low 29 bits.
uint32_t newMiiId(int64_t unixSeconds);

// UTF-8 <-> the record's UTF-16 (basic multilingual plane).
std::string toUtf8(const std::u16string& s);
std::u16string fromUtf8(const std::string& s, size_t maxChars);

// The database file.
struct Database {
    std::vector<uint8_t> image;  // the whole file (at least kDbMainSize bytes)
    Mii slots[kMaxMiis];         // decoded from / encoded into the image

    void format();              // an empty database exactly as RFLiFormatAsync writes one
    // From a file's bytes: "RNOD", at least kDbMainSize bytes. crcOk tells whether the stored
    // CRC matched (RFL refuses a database with a bad one: the console would ask to repair it).
    bool parse(const std::vector<uint8_t>& bytes, bool& crcOk, std::string& error);
    // The image with the slots written in and a fresh CRC.
    const std::vector<uint8_t>& encode();
    int count() const;
    int firstFree() const;
    int findId(const Mii& m) const;  // slot with the same Mii ID + system ID, -1 if none
};

// Single-Mii files: .mii / .miigx (the 74-byte record), .rsd (store data, 76 bytes with CRC).
// Several records in one file (e.g. a .mii holding a whole parade) are not a thing; a whole
// database is Database::parse. Returns false with a reason when the bytes are no Wii Mii.
bool parseMiiFile(const std::vector<uint8_t>& bytes, Mii& out, std::string& error);

}  // namespace recomp::mii
