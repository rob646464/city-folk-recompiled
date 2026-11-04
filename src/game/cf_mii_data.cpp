// Wii Mii data and the Mii database file (cf_mii_data.h, docs/MIIS.md).
#include "game/cf_mii_data.h"

#include <algorithm>
#include <cstring>

namespace recomp::mii {
namespace {

// Bit fields of the record (big-endian u16 words), from convertRaw2InfoCore_ and the limits of
// RFLiCheckValidInfo (the Info struct's fields mapped back to the record).
const FieldInfo kFields[int(F::Count)] = {
    {0x00, 14, 1, 0, 1},   // Gender (0 male, 1 female)
    {0x00, 10, 4, 0, 12},  // BirthMonth (0 = not set)
    {0x00, 5, 5, 0, 31},   // BirthDay
    {0x00, 1, 4, 0, 11},   // FavColor
    {0x00, 0, 1, 0, 1},    // Favorite
    {0x16, 8, 8, 0, 127},  // Height (u8 at 0x16; RFL accepts up to 128, the Mii Channel makes 0..127)
    {0x16, 0, 8, 0, 127},  // Weight (u8 at 0x17)
    {0x20, 13, 3, 0, 7},   // FaceShape
    {0x20, 10, 3, 0, 5},   // SkinTone
    {0x20, 6, 4, 0, 11},   // FaceFeature
    {0x20, 2, 1, 0, 1},    // NoMingle (Info flag 0: a special Mii must have it clear)
    {0x20, 0, 1, 0, 1},    // Downloaded
    {0x22, 9, 7, 0, 71},   // HairStyle
    {0x22, 6, 3, 0, 7},    // HairColor
    {0x22, 5, 1, 0, 1},    // HairFlip
    {0x24, 11, 5, 0, 23},  // BrowType
    {0x24, 6, 5, 0, 11},   // BrowRotation
    {0x26, 13, 3, 0, 7},   // BrowColor
    {0x26, 9, 4, 0, 8},    // BrowSize
    {0x26, 4, 5, 3, 18},   // BrowY
    {0x26, 0, 4, 0, 12},   // BrowX
    {0x28, 10, 6, 0, 47},  // EyeType
    {0x28, 5, 5, 0, 7},    // EyeRotation
    {0x28, 0, 5, 0, 18},   // EyeY
    {0x2A, 13, 3, 0, 5},   // EyeColor
    {0x2A, 9, 4, 0, 7},    // EyeSize
    {0x2A, 5, 4, 0, 12},   // EyeX
    {0x2C, 12, 4, 0, 11},  // NoseType
    {0x2C, 8, 4, 0, 8},    // NoseSize
    {0x2C, 3, 5, 0, 18},   // NoseY
    {0x2E, 11, 5, 0, 23},  // MouthType
    {0x2E, 9, 2, 0, 2},    // MouthColor
    {0x2E, 5, 4, 0, 8},    // MouthSize
    {0x2E, 0, 5, 0, 18},   // MouthY
    {0x30, 12, 4, 0, 8},   // GlassesType
    {0x30, 9, 3, 0, 5},    // GlassesColor
    {0x30, 5, 4, 0, 7},    // GlassesSize
    {0x30, 0, 5, 0, 20},   // GlassesY
    {0x32, 14, 2, 0, 3},   // MustacheType
    {0x32, 12, 2, 0, 3},   // BeardType
    {0x32, 9, 3, 0, 7},    // FacialHairColor
    {0x32, 5, 4, 0, 8},    // MustacheSize
    {0x32, 0, 5, 0, 16},   // MustacheY
    {0x34, 15, 1, 0, 1},   // MoleOn
    {0x34, 11, 4, 0, 8},   // MoleSize
    {0x34, 6, 5, 0, 30},   // MoleY
    {0x34, 1, 5, 0, 16},   // MoleX
};

// RFLiCheckValidInfo's days per month (USA 0x8048A4B8).
constexpr uint8_t kDays[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void wr32(uint8_t* p, uint32_t v) {
    wr16(p, uint16_t(v >> 16));
    wr16(p + 2, uint16_t(v));
}

std::u16string readName(const uint8_t* p) {
    std::u16string s;
    for (int i = 0; i < 10; i++) {
        const char16_t ch = char16_t(rd16(p + 2 * i));
        if (!ch) break;
        s += ch;
    }
    return s;
}
void writeName(uint8_t* p, const std::u16string& s) {
    for (int i = 0; i < 10; i++) wr16(p + 2 * i, i < int(s.size()) ? uint16_t(s[i]) : 0);
}

}  // namespace

const FieldInfo& fieldInfo(F f) { return kFields[int(f)]; }

int Mii::get(F f) const {
    const FieldInfo& fi = kFields[int(f)];
    return (rd16(raw + fi.word) >> fi.shift) & ((1 << fi.bits) - 1);
}

void Mii::set(F f, int v) {
    const FieldInfo& fi = kFields[int(f)];
    v = std::clamp(v, int(fi.min), int(fi.max));
    const uint16_t mask = uint16_t(((1 << fi.bits) - 1) << fi.shift);
    wr16(raw + fi.word, uint16_t((rd16(raw + fi.word) & ~mask) | (uint16_t(v << fi.shift) & mask)));
}

std::u16string Mii::name() const { return readName(raw + 0x02); }
std::u16string Mii::creator() const { return readName(raw + 0x36); }
void Mii::setName(const std::u16string& s) { writeName(raw + 0x02, s); }
void Mii::setCreator(const std::u16string& s) { writeName(raw + 0x36, s); }
uint32_t Mii::miiId() const { return rd32(raw + 0x18); }
uint32_t Mii::systemId() const { return rd32(raw + 0x1C); }
void Mii::setIds(uint32_t miiId, uint32_t systemId) {
    wr32(raw + 0x18, miiId);
    wr32(raw + 0x1C, systemId);
}

std::string Mii::problem() const {
    if (miiId() == 0 && systemId() == 0) return "no Mii ID";
    if (miiId() >> 29 & 1) return "a temporary Mii (not stored on a console)";
    // Unused bits of the words are not checked by RFL; only the fields' ranges are.
    for (int i = 0; i < int(F::Count); i++) {
        if (F(i) == F::Height || F(i) == F::Weight) continue;  // RFL takes up to 128 (below)
        const FieldInfo& fi = kFields[i];
        const int v = get(F(i));
        if (v < fi.min || v > fi.max) return "a value out of range";
    }
    if (raw[0x16] > 128 || raw[0x17] > 128) return "height or weight out of range";
    if (name().empty()) return "no name";
    const int m = get(F::BirthMonth), d = get(F::BirthDay);
    if ((m == 0) != (d == 0)) return "a birthday with only a month or only a day";
    if (m && d > kDays[m]) return "a birthday that does not exist";
    // Special Miis (Mii ID bit 31 clear) may not have the "don't mingle" flag.
    if (!(miiId() >> 31) && get(F::NoMingle)) return "a special Mii that may not travel";
    return {};
}

uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= uint16_t(data[i]) << 8;
        for (int b = 0; b < 8; b++) crc = uint16_t(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

void toStoreData(const Mii& m, uint8_t out[kStoreSize]) {
    std::memcpy(out, m.raw, kRawSize);
    wr16(out + kRawSize, crc16(out, kRawSize));
}

uint32_t newMiiId(int64_t unixSeconds) {
    constexpr int64_t k2006 = 1136073600;  // 2006-01-01 00:00 UTC
    const int64_t t = std::max<int64_t>(unixSeconds - k2006, 0) / 4;
    return 0x80000000u | (uint32_t(t) & 0x1FFFFFFFu);
}

std::string toUtf8(const std::u16string& s) {
    std::string o;
    for (char16_t ch : s) {
        const uint32_t c = ch;
        if (c < 0x80) o += char(c);
        else if (c < 0x800) {
            o += char(0xC0 | c >> 6);
            o += char(0x80 | (c & 0x3F));
        } else {
            o += char(0xE0 | c >> 12);
            o += char(0x80 | (c >> 6 & 0x3F));
            o += char(0x80 | (c & 0x3F));
        }
    }
    return o;
}

std::u16string fromUtf8(const std::string& s, size_t maxChars) {
    std::u16string o;
    for (size_t i = 0; i < s.size() && o.size() < maxChars;) {
        const uint8_t b = uint8_t(s[i]);
        uint32_t c = '?';
        size_t n = 1;
        if (b < 0x80) c = b;
        else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) c = (b & 0x1F) << 6 | (s[i + 1] & 0x3F), n = 2;
        else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) c = (b & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F), n = 3;
        else if ((b & 0xF8) == 0xF0) n = 4;  // outside the BMP: not storable
        i += n;
        if (c) o += char16_t(c);
    }
    return o;
}

void Database::format() {
    // RFLiFormatAsync / RFLiClearDBBuffer: zeroed, "RNOD", the word at 0x1CEC with bit 31 set,
    // the parade table "RNHD" with both list heads -1 and 10000 entries whose links are 0x7FFF,
    // then the hidden-Mii area of 10000 zeroed 0x40-byte entries.
    image.assign(kDbFileSize, 0);
    std::memcpy(image.data(), "RNOD", 4);
    wr32(&image[0x1CEC], 0x80000000u);
    std::memcpy(&image[0x1D00], "RNHD", 4);
    wr16(&image[0x1D04], 0xFFFF);
    wr16(&image[0x1D06], 0xFFFF);
    for (size_t i = 0; i < kDbHiddenEntries; i++) {
        uint8_t* e = &image[0x1D08 + i * 12];
        wr16(e + 8, 0x7FFF);
        wr16(e + 10, 0x7FFF);
    }
    for (Mii& m : slots) m = Mii{};
    encode();
}

bool Database::parse(const std::vector<uint8_t>& bytes, bool& crcOk, std::string& error) {
    if (bytes.size() < kDbMainSize || std::memcmp(bytes.data(), "RNOD", 4) != 0) {
        error = "not a Wii Mii database (RFL_DB.dat starts with \"RNOD\" and is at least 127 KB)";
        return false;
    }
    image = bytes;
    crcOk = crc16(image.data(), kDbCrcOff) == rd16(&image[kDbCrcOff]);
    for (int i = 0; i < kMaxMiis; i++) std::memcpy(slots[i].raw, &image[kDbMiisOff + i * kRawSize], kRawSize);
    return true;
}

const std::vector<uint8_t>& Database::encode() {
    if (image.size() < kDbMainSize) format();
    for (int i = 0; i < kMaxMiis; i++) std::memcpy(&image[kDbMiisOff + i * kRawSize], slots[i].raw, kRawSize);
    wr16(&image[kDbCrcOff], crc16(image.data(), kDbCrcOff));
    return image;
}

int Database::count() const {
    int n = 0;
    for (const Mii& m : slots) n += !m.empty();
    return n;
}

int Database::firstFree() const {
    for (int i = 0; i < kMaxMiis; i++)
        if (slots[i].empty()) return i;
    return -1;
}

int Database::findId(const Mii& m) const {
    for (int i = 0; i < kMaxMiis; i++)
        if (!slots[i].empty() && slots[i].sameIdAs(m)) return i;
    return -1;
}

bool parseMiiFile(const std::vector<uint8_t>& bytes, Mii& out, std::string& error) {
    if (bytes.size() == kStoreSize) {
        // .rsd: the record + its CRC (RFLStoreData).
        if (crc16(bytes.data(), kStoreSize) != 0) {
            error = "the file's checksum does not match (damaged store data)";
            return false;
        }
    } else if (bytes.size() != kRawSize) {
        error = "not a Wii Mii (.mii / .miigx files are 74 bytes, .rsd files 76)";
        return false;
    }
    std::memcpy(out.raw, bytes.data(), kRawSize);
    if (out.empty()) {
        error = "the file holds an empty Mii slot";
        return false;
    }
    if (const std::string p = out.problem(); !p.empty()) {
        error = "the Mii has " + p + " (RFL would reject it)";
        return false;
    }
    return true;
}

}  // namespace recomp::mii
