// Save import data checks (src/game/cf_save_data.*, docs/NAND.md "Importing a Wii or Dolphin save"):
// AES-128 known answer, the save's checksums on a synthetic save, a damaged byte, a data.bin round
// trip with a made-up key (never a Nintendo key), a wrong key refused. With CF_SAVE_FILE=<rvforest.dat>
// also a real save (never committed): every checksum must match.
#include "game/cf_save_data.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace cfsave;

static int sFail = 0;
#define CHECK(x)                                                   \
    do {                                                           \
        if (!(x)) {                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            sFail++;                                               \
        }                                                          \
    } while (0)

static void putName(std::vector<u8>& d, u32 at, u16 id, const char* name) {
    d[at] = u8(id >> 8), d[at + 1] = u8(id);
    for (int i = 0; name[i]; i++) d[at + 2 + 2 * i] = 0, d[at + 3 + 2 * i] = u8(name[i]);
}

int main() {
    CHECK(aesSelfTest());
    CHECK(crc32(reinterpret_cast<const u8*>("123456789"), 9) == 0xCBF43926u);

    // A synthetic save: one resident "rob" in town "Aa", made in the USA (town byte 0x11).
    std::vector<u8> save(kSaveSize, 0);
    putName(save, kResidentBase + kTownStruct, 0x1234, "Aa");
    putName(save, kResidentBase + kPlayerStruct, 0x5678, "rob");
    save[kTownRegionByte] = 0x11;
    SaveInfo si = inspectSave(save);
    CHECK(!si.valid() && !si.badChecksums.empty());
    fixChecksums(save);
    si = inspectSave(save);
    CHECK(si.valid());
    CHECK(si.town == "Aa" && si.residents.size() == 1 && si.residents[0] == "rob");
    CHECK(si.townRegion == kUsa && si.townLanguage == 1);
    CHECK(checksums().size() == 14);
    // Every checksummed range detects a change.
    for (const auto& c : checksums()) {
        std::vector<u8> bad = save;
        bad[c.start + c.len / 2] ^= 0x40;
        const SaveInfo b = inspectSave(bad);
        CHECK(!b.valid() && b.badChecksums.size() == 1 && b.problem().find(c.what) != std::string::npos);
    }
    CHECK(!inspectSave(std::vector<u8>(save.begin(), save.end() - 1)).valid());

    // data.bin round trip with a made-up key and IV.
    u8 key[16], iv[16], wrong[16];
    CHECK(parseKey16("00112233445566778899aabbccddeeff", key));
    CHECK(parseKey16("0x0f 0e 0d 0c 0b 0a 09 08 07 06 05 04 03 02 01 00", iv));
    CHECK(!parseKey16("zz112233445566778899aabbccddeeff", wrong));
    std::memcpy(wrong, key, 16);
    wrong[3] ^= 1;
    std::vector<u8> banner(0xF0A0, 0x5A);
    std::memcpy(banner.data(), "WIBN", 4);
    std::vector<u8> other(1000, 0x77);
    const u64 tid = 0x0001000052555545ull;
    const auto bin = buildDataBin(tid, banner, {{"rvforest.dat", save}, {"wc24dl.vff", other}}, key, iv);
    DataBin info;
    std::string err;
    CHECK(parseDataBin(bin, info, err));
    CHECK(info.titleId == tid && info.files.size() == 2 && info.files[0].name == "rvforest.dat");
    DataBinContents out;
    CHECK(decryptDataBin(bin, info, key, iv, out, err));
    CHECK(out.banner == banner && out.files.size() == 2 && out.files[0].second == save && out.files[1].second == other);
    CHECK(!decryptDataBin(bin, info, wrong, iv, out, err) && err.find("not right") != std::string::npos);
    std::vector<u8> cut(bin.begin(), bin.begin() + 0x10000);
    CHECK(!parseDataBin(cut, info, err));
    CHECK(!parseDataBin(save, info, err));  // an rvforest.dat is not a data.bin

    if (const char* f = std::getenv("CF_SAVE_FILE")) {
        std::ifstream in(f, std::ios::binary);
        std::vector<u8> real((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const SaveInfo r = inspectSave(real);
        std::printf("%s: %s town %s, %zu residents, region %s: %s\n", f, r.valid() ? "valid" : "INVALID", r.town.c_str(),
                    r.residents.size(), regionName(r.townRegion), r.problem().c_str());
        CHECK(r.valid());
    }
    // Test aid: CF_SAVE_MAKE_DATABIN=<rvforest.dat>|<banner.bin>|<out data.bin>|<title hex> writes a data.bin
    // of a real save with the made-up key and IV above (for CF_SAVE_IMPORT runs with CF_SD_KEY /
    // CF_SD_IV = those values; a real data.bin needs the player's own key).
    if (const char* m = std::getenv("CF_SAVE_MAKE_DATABIN")) {
        std::string spec = m;
        std::vector<std::string> part;
        for (size_t a = 0, b; a <= spec.size(); a = b + 1) {
            b = spec.find('|', a);
            if (b == std::string::npos) b = spec.size();
            part.push_back(spec.substr(a, b - a));
        }
        auto rd = [](const std::string& p) {
            std::ifstream in(p, std::ios::binary);
            return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        if (part.size() == 4) {
            const u64 t = 0x0001000000000000ull | std::strtoul(part[3].c_str(), nullptr, 16);
            const auto out = buildDataBin(t, rd(part[1]), {{"rvforest.dat", rd(part[0])}}, key, iv);
            std::ofstream(part[2], std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
            std::printf("wrote %s (%zu bytes)\n", part[2].c_str(), out.size());
        }
    }
    std::printf(sFail ? "save import test: %d FAILED\n" : "save import test: all passed\n", sFail);
    return sFail ? 1 : 0;
}
