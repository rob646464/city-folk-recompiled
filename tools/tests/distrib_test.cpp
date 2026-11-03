// Nintendo events: known-answer tests of the WC24 container crypto and the VFF volume writer
// (docs/DISTRIBUTIONS.md "Testing"). `cf_distrib_test [file.enc wc24pubk.mod]` also opens a WC24
// file with a key file (prints whether the signature verifies; nothing is written).
#include "recomp/vff.h"
#include "recomp/wc24_crypto.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
