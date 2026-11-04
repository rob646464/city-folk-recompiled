// Checks of the Mii data / database code (src/game/cf_mii_data.cpp, docs/MIIS.md): CRC, record
// fields, limits, the database file and the single-Mii files. Synthetic data only.
#include "game/cf_mii_data.h"

#include <cstdio>
#include <cstring>

using namespace recomp::mii;

static int sFail = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);   \
            sFail++;                                                    \
        }                                                               \
    } while (0)

static Mii sample() {
    Mii m;
    m.setIds(newMiiId(1700000000), 0x80123456u);
    m.setName(u"Robo");
    m.setCreator(u"rob");
    m.set(F::Gender, 1);
    m.set(F::BirthMonth, 2);
    m.set(F::BirthDay, 29);
    m.set(F::FavColor, 11);
    m.set(F::Height, 100);
    m.set(F::Weight, 20);
    m.set(F::HairStyle, 71);
    m.set(F::EyeType, 47);
    m.set(F::BrowY, 18);
    m.set(F::MoleOn, 1);
    m.set(F::MoleY, 30);
    m.set(F::MoleX, 16);
    return m;
}

int main() {
    // CRC-16/XMODEM check value.
    CHECK(crc16(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x31C3);

    // Fields round-trip and land in the documented bits.
    Mii m = sample();
    CHECK(m.get(F::Gender) == 1 && m.get(F::BirthMonth) == 2 && m.get(F::BirthDay) == 29);
    CHECK(m.get(F::HairStyle) == 71 && m.get(F::EyeType) == 47 && m.get(F::BrowY) == 18);
    CHECK(m.raw[0x16] == 100 && m.raw[0x17] == 20);
    CHECK((m.raw[0x22] >> 1) == 71);                       // hair: bits 15-9 of 0x22
    CHECK((m.raw[0x00] & 0x40) != 0);                      // gender: bit 14
    CHECK(m.name() == u"Robo" && m.creator() == u"rob");
    CHECK(m.raw[0x02] == 0 && m.raw[0x03] == 'R');          // UTF-16BE
    CHECK(m.problem().empty());
    m.set(F::BrowY, 0);                                    // clamped to RFL's minimum 3
    CHECK(m.get(F::BrowY) == 3);
    m.set(F::HairStyle, 200);
    CHECK(m.get(F::HairStyle) == 71);

    // Limits RFL checks.
    Mii bad = sample();
    bad.set(F::BirthDay, 0);
    CHECK(!bad.problem().empty());                         // month without day
    bad = sample();
    bad.set(F::BirthMonth, 4);
    bad.set(F::BirthDay, 31);
    CHECK(!bad.problem().empty());                         // April 31
    bad = sample();
    bad.setName(u"");
    CHECK(!bad.problem().empty());
    bad = sample();
    bad.setIds(0x20000001u, 1);                            // temporary + special
    CHECK(!bad.problem().empty());
    bad = sample();
    bad.setIds(0x00001234u, 1);                            // special Mii that may not travel
    bad.set(F::NoMingle, 1);
    CHECK(!bad.problem().empty());
    CHECK((newMiiId(1700000000) >> 31) == 1 && ((newMiiId(1700000000) >> 29) & 1) == 0);

    // Store data.
    uint8_t store[kStoreSize];
    toStoreData(sample(), store);
    CHECK(crc16(store, kStoreSize) == 0);

    // Database: format, add, encode, parse back.
    Database db;
    db.format();
    CHECK(db.image.size() == kDbFileSize && db.image.size() == 0xBB5E0);
    CHECK(std::memcmp(db.image.data(), "RNOD", 4) == 0 && std::memcmp(&db.image[0x1D00], "RNHD", 4) == 0);
    CHECK(db.count() == 0 && db.firstFree() == 0);
    db.slots[3] = sample();
    const auto& img = db.encode();
    CHECK(crc16(img.data(), kDbCrcOff) == uint16_t(img[kDbCrcOff] << 8 | img[kDbCrcOff + 1]));
    CHECK(crc16(img.data(), kDbMainSize) == 0);            // what bootloadCheckCRCCb_ tests
    Database back;
    bool crcOk = false;
    std::string err;
    CHECK(back.parse(img, crcOk, err) && crcOk);
    CHECK(back.count() == 1 && back.findId(sample()) == 3 && back.firstFree() == 0);
    std::vector<uint8_t> broken = img;
    broken[0x100] ^= 1;
    CHECK(back.parse(broken, crcOk, err) && !crcOk);
    std::vector<uint8_t> notDb(kDbMainSize, 0);
    CHECK(!back.parse(notDb, crcOk, err));

    // Single-Mii files.
    Mii out;
    const Mii s = sample();
    std::vector<uint8_t> mii(s.raw, s.raw + kRawSize);
    CHECK(parseMiiFile(mii, out, err) && out.sameIdAs(sample()));
    std::vector<uint8_t> rsd(store, store + kStoreSize);
    CHECK(parseMiiFile(rsd, out, err));
    rsd[5] ^= 0x10;
    CHECK(!parseMiiFile(rsd, out, err));
    CHECK(!parseMiiFile(std::vector<uint8_t>(10, 0), out, err));
    CHECK(!parseMiiFile(std::vector<uint8_t>(kRawSize, 0), out, err));  // empty slot

    // UTF-8 names.
    CHECK(fromUtf8("Ren\xC3\xA9 is long name", 10) == u"René is lo");
    CHECK(toUtf8(u"René") == "Ren\xC3\xA9");

    std::printf(sFail ? "%d FAILED\n" : "all Mii checks passed\n", sFail);
    return sFail ? 1 : 0;
}
