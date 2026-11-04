// Nintendo events: known-answer tests of the WC24 container crypto and the VFF volume writer
// (docs/DISTRIBUTIONS.md "Testing"). `cf_distrib_test [file.enc wc24pubk.mod]` also opens a WC24
// file with a key file (prints whether the signature verifies; nothing is written).
#include "recomp/distrib_record.h"
#include "recomp/vff.h"
#include "recomp/wc24_crypto.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

// The per-town record (distrib_record.h): town keys, an older state.ini upgraded, the legacy
// record adopted by the first town, a second town on its own, write + parse round trip.
static bool recordTest(std::string* why) {
    namespace r = recomp::distrib::record;
    auto fail = [&](const char* w) {
        *why = w;
        return false;
    };
    // Town struct as in the user-town fixture's save (pd + 0x7EC2): id F177, "Frunch".
    const unsigned char frunch[0x14] = {0xF1, 0x77, 0, 'F', 0, 'r', 0, 'u', 0, 'n', 0, 'c', 0, 'h', 0, 0, 0, 0, 0, 0};
    unsigned char other[0x14];
    std::memcpy(other, frunch, sizeof other);
    other[1] = 0x78;  // same name, another town id
    const unsigned char none[0x14] = {0xFF, 0xFF};
    std::string readable;
    const std::string kA = r::townKey(frunch, &readable), kB = r::townKey(other);
    if (kA != "F177-004600720075006E00630068" || readable != "Frunch (F177)") return fail("town key");
    if (kB.empty() || kB == kA || !r::townKey(none).empty()) return fail("town key: other / none");

    // A state.ini written before the change: one record "town", a Receive now letter (4D000001)
    // and a loop letter (4C000577) served.
    const char* oldFile =
        "[state]\ntown=town\nmanual_counter=1\nlast_stored=4C000577\n"
        "[town.town]\nlast_delivery=847000000\ncreepy_clock=846990000,26\nkappn_model=847000000,26\n"
        "[served]\n4D000001=5\n4C000577=7\n";
    r::Book b;
    {
        std::istringstream in(oldFile);
        r::parse(b, in);
    }
    auto keyOf = [](int i) -> const char* { return i == 5 ? "creepy_clock" : i == 7 ? "kappn_model" : nullptr; };
    if (!r::upgrade(b, keyOf) || b.format != r::kFormat) return fail("upgrade");
    if (b.towns["town"].now.size() != 1 || b.towns["town"].now["creepy_clock"] != 846990000) return fail("upgrade: Receive now");
    if (r::upgrade(b, keyOf)) return fail("upgrade twice");
    // The first town identified takes the legacy record.
    if (!r::identify(b, kA, readable) || b.town != kA || b.towns.count("town")) return fail("adopt");
    if (!b.towns[kA].now.count("creepy_clock") || b.towns[kA].received.size() != 2 || b.towns[kA].cycle["kappn_model"] != 26)
        return fail("adopt: contents");
    // Another town: its own (empty) record; back to the first: still there.
    r::identify(b, kB, "");
    if (b.town != kB || !b.towns[kB].now.empty()) return fail("second town");
    b.towns[kB].now["red_pikmin"] = 900000000;
    r::identify(b, kA, readable);
    // Round trip.
    std::ostringstream out;
    r::write(b, out);
    r::Book c;
    {
        std::istringstream in(out.str());
        r::parse(c, in);
    }
    if (c.format != r::kFormat || c.town != kA || c.names[kA] != "Frunch (F177)" || c.towns[kA].now != b.towns[kA].now ||
        c.towns[kB].now != b.towns[kB].now || c.towns[kA].received != b.towns[kA].received || c.served != b.served ||
        c.manualCounter != 1 || c.lastStored != 0x4C000577)
        return fail("round trip");
    if (r::upgrade(c, keyOf)) return fail("upgrade of a new file");
    if (!r::isReceiveNowId(0x4D000002) || r::isReceiveNowId(0x4C000577) || r::isReceiveNowId(0x4F0157D9)) return fail("ids");
    return true;
}

static std::vector<unsigned char> load(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    int fails = 0;
    std::string why;
    if (!wc24crypto::selfTest(&why)) std::printf("FAIL crypto: %s\n", why.c_str()), fails++;
    else std::printf("ok   crypto (SHA-1, AES-128-OFB, RSA-2048 modexp)\n");
    if (!recomp::vff::selfTest(&why)) std::printf("FAIL vff: %s\n", why.c_str()), fails++;
    else std::printf("ok   vff (FAT12 + FAT16 write / rewrite / read / remove)\n");
    if (!recordTest(&why)) std::printf("FAIL record: %s\n", why.c_str()), fails++;
    else std::printf("ok   record (town keys, older state.ini upgraded and adopted, two towns, round trip)\n");
    if (argc >= 3) {
        auto enc = load(argv[1]), key = load(argv[2]);
        if (enc.size() < 0x140 || key.size() < 0x210) {
            std::printf("bad input sizes\n");
            return 2;
        }
        std::vector<unsigned char> pay(enc.begin() + 0x140, enc.end());
        wc24crypto::aes128Ofb(key.data() + 0x200, enc.data() + 0x30, pay.data(), pay.size());
        const bool ok = wc24crypto::rsa2048VerifySha1(key.data(), enc.data() + 0x40, pay.data(), pay.size());
        std::printf("%s: payload %02x%02x%02x%02x, signature %s\n", argv[1], pay[0], pay[1], pay[2], pay[3],
                    ok ? "verifies" : "does NOT verify");
    }
    return fails ? 1 : 0;
}
