// Wild World save reader for the move-in (src/game/ww_save.*; docs/MOVE_IN.md "Testing").
//   cf_move_in_test                 synthetic saves (every layout, .dsv, 512 KB, conversion, CRC)
//   CF_WW_SAVE=<file> cf_move_in_test   also prints what a real save holds (file only read)
#include "game/ww_save.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int sFail = 0;
#define CHECK(x)                                                     \
    do {                                                             \
        if (!(x)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            sFail++;                                                 \
        }                                                            \
    } while (0)

// Test character tables: Latin like the game's (1..26 'A'..'Z', 27..52 'a'..'z', 0x85 space),
// Japanese with a few kana and 'A'.
ww::CharTable latinTable() {
    ww::CharTable t{};
    for (int i = 0; i < 26; i++) {
        t[1 + i] = char16_t(u'A' + i);
        t[27 + i] = char16_t(u'a' + i);
    }
    t[0x85] = u' ';
    t[0x9B] = u'?';
    return t;
}
ww::CharTable jpTable() {
    ww::CharTable t{};
    t[1] = u'\x3042';  // a
    t[2] = u'\x3044';  // i
    t[0x60] = u'A';
    t[0x61] = u'?';
    return t;
}

void put16(std::vector<uint8_t>& v, uint32_t o, uint16_t x) {
    v[o] = uint8_t(x);
    v[o + 1] = uint8_t(x >> 8);
}

// A synthetic save of a layout: town "Ab", one resident "Bo" (girl, face 3, hair 5, colour 2).
std::vector<uint8_t> makeSave(ww::Layout l) {
    const auto& L = ww::layout(l);
    std::vector<uint8_t> img(ww::kBackupSize, 0xFF);
    std::fill(img.begin(), img.begin() + 2 * L.copySize, uint8_t(0));
    for (int c = 0; c < 2; c++) {
        const uint32_t b = c * L.copySize;
        auto putName = [&](uint32_t o, const char* s) {
            for (int i = 0; s[i]; i++) {
                if (l == ww::Layout::KR) put16(img, o + 2 * i, uint16_t(s[i]));
                else if (l == ww::Layout::JP) img[o + i] = uint8_t(s[i] == 'A' ? 0x60 : 1);
                else img[o + i] = uint8_t(s[i] >= 'a' ? 27 + s[i] - 'a' : 1 + s[i] - 'A');
            }
        };
        putName(b + L.townName, "Ab");
        const uint32_t p = b + L.players;
        put16(img, p + L.playerId, 0x1234);
        put16(img, p + L.townId, 0x5678);
        putName(p + L.name, "Bo");
        img[p + L.gender] = 1;
        img[p + L.face] = uint8_t(3 | 5 << 4);
        img[p + L.hairColor] = 2;
        img[p + L.catalog] = 0xA5;
        img[p + L.catalog + 0x122] = 0x5A;
        if (c == 1) {
            put16(img, L.townMarker[0], 0xBEEF);
            img[L.townMarker[1]] = 2;
            img[L.townMarker[2]] = 7;
        }
        put16(img, b, 0);
        put16(img, b, uint16_t(0x10000 - ww::sum16(img.data() + b, L.copySize)));
    }
    return img;
}

void testLayout(ww::Layout l) {
    const auto jp = jpTable(), latin = latinTable();
    std::vector<uint8_t> f = makeSave(l);
    ww::Save s;
    std::string err;
    CHECK(ww::load(f, s, err));
    CHECK(s.layout == l);
    CHECK(s.copyOk[0] && s.copyOk[1]);
    CHECK(s.townData);
    CHECK(s.residents() == 1);
    CHECK(s.players[0].exists && !s.players[1].exists);
    CHECK(s.players[0].gender == 1 && s.players[0].face == 3 && s.players[0].hair == 5 && s.players[0].hairColor == 2);
    if (l != ww::Layout::JP) {
        CHECK(ww::utf8(ww::decodeName(s.players[0].nameRaw.data(), ww::layout(l).nameLen, l, jp, latin)) == "Bo");
        CHECK(ww::utf8(ww::decodeName(s.townRaw.data(), ww::layout(l).nameLen, l, jp, latin)) == "Ab");
    }
    // Conversion into every other layout keeps the fields the game's import reads.
    for (ww::Layout to : {ww::Layout::JP, ww::Layout::USEU, ww::Layout::KR}) {
        if (to == l) continue;
        ww::Save c;
        CHECK(ww::load(ww::convert(s, to, jp, latin), c, err));
        CHECK(c.layout == to);
        CHECK(c.copyOk[0] && c.copyOk[1] && c.townData);
        CHECK(c.residents() == 1);
        CHECK(c.players[0].playerId == 0x1234 && c.players[0].townId == 0x5678);
        CHECK(c.players[0].gender == 1 && c.players[0].face == 3 && c.players[0].hair == 5 && c.players[0].hairColor == 2);
        const auto& D = ww::layout(to);
        CHECK(c.image[D.players + D.catalog] == 0xA5 && c.image[D.players + D.catalog + 0x122] == 0x5A);
        const std::string name = ww::utf8(ww::decodeName(c.players[0].nameRaw.data(), D.nameLen, to, jp, latin));
        if (l != ww::Layout::JP && to != ww::Layout::JP) CHECK(name == "Bo");
        if (l != ww::Layout::JP && to == ww::Layout::JP) CHECK(name == "??");  // the test JP table has no 'B'/'o'
    }
}

}  // namespace

int main() {
    for (ww::Layout l : {ww::Layout::JP, ww::Layout::USEU, ww::Layout::KR}) testLayout(l);

    // .dsv: footer removed.
    {
        std::vector<uint8_t> f = makeSave(ww::Layout::USEU);
        const char* foot = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
        f.insert(f.end(), foot, foot + std::strlen(foot));
        f.resize(f.size() + 0x40, 0x01);
        ww::Save s;
        std::string err;
        CHECK(ww::load(f, s, err));
        CHECK(s.format.find("DeSmuME") != std::string::npos);
        CHECK(s.residents() == 1);
    }
    // 512 KB with the save in the second half; trimmed file padded.
    {
        std::vector<uint8_t> f(ww::kBackupSize, 0xFF);
        const auto s1 = makeSave(ww::Layout::USEU);
        f.insert(f.end(), s1.begin(), s1.end());
        ww::Save s;
        std::string err;
        CHECK(ww::load(f, s, err));
        CHECK(s.format.find("second half") != std::string::npos);
        std::vector<uint8_t> t(s1.begin(), s1.begin() + 2 * ww::layout(ww::Layout::USEU).copySize);
        CHECK(ww::load(t, s, err) && s.image.size() == ww::kBackupSize);
    }
    // Damaged copy 1: copy 2 is read; both damaged: refused.
    {
        std::vector<uint8_t> f = makeSave(ww::Layout::USEU);
        f[0x100] ^= 1;
        ww::Save s;
        std::string err;
        CHECK(ww::load(f, s, err) && !s.copyOk[0] && s.copyOk[1] && s.copy == 1);
        f[ww::layout(ww::Layout::USEU).copySize + 0x100] ^= 1;
        CHECK(!ww::load(f, s, err));
        std::vector<uint8_t> blank(ww::kBackupSize, 0xFF);
        CHECK(!ww::load(blank, s, err));
    }
    // CRC-32 check value.
    CHECK(ww::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);
    // Region guess from the file name.
    {
        ww::Save s;
        std::string err;
        ww::load(makeSave(ww::Layout::USEU), s, err);
        CHECK(ww::guessRegion(s, "x/Animal Crossing - Wild World (USA) (Rev 1).sav") == ww::Region::US);
        CHECK(ww::guessRegion(s, "x/Animal Crossing - Wild World (Europe) (En,Fr,De,Es,It).sav") == ww::Region::EU);
    }

    if (const char* path = std::getenv("CF_WW_SAVE")) {
        ww::Save s;
        std::string err;
        if (!ww::loadFile(path, s, err)) {
            std::printf("%s: %s\n", path, err.c_str());
            sFail++;
        } else {
            const auto latin = latinTable(), jp = jpTable();
            std::printf("%s: %s, %s layout, copies %d/%d, town data %d, guessed region %s\n", path, s.format.c_str(),
                        ww::layoutName(s.layout), s.copyOk[0], s.copyOk[1], s.townData,
                        ww::regionName(ww::guessRegion(s, path)));
            for (int i = 0; i < 4; i++) {
                const auto& p = s.players[i];
                std::printf("  slot %d: %s pid %04X tid %04X gender %d face %d hair %d colour %d name bytes", i,
                            p.exists ? "resident" : "empty   ", p.playerId, p.townId, p.gender, p.face, p.hair, p.hairColor);
                for (uint8_t b : p.nameRaw) std::printf(" %02X", b);
                std::printf("\n");
            }
            (void)latin;
            (void)jp;
        }
    }
    std::printf(sFail ? "%d check(s) failed\n" : "all checks passed\n", sFail);
    return sFail ? 1 : 0;
}
