// SHA-1, AES-128-OFB and RSA-2048 PKCS#1 v1.5 (SHA-1) verification for the WiiConnect24
// download container (wc24_crypto.h). Written from the public specifications.
#include "recomp/wc24_crypto.h"

#include <cstring>

namespace wc24crypto {
namespace {

u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

// ---- SHA-1 (FIPS 180-4) ----------------------------------------------------------------------
struct Sha1 {
    u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    u8 buf[64];
    size_t n = 0;
    std::uint64_t total = 0;

    void block(const u8* p) {
        u32 w[80];
        for (int i = 0; i < 16; i++) w[i] = u32(p[4 * i]) << 24 | u32(p[4 * i + 1]) << 16 | u32(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            u32 f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else f = b ^ c ^ d, k = 0xCA62C1D6u;
            const u32 t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void update(const u8* p, size_t len) {
        total += len;
        while (len) {
            const size_t take = std::min(len, size_t(64) - n);
            std::memcpy(buf + n, p, take);
            n += take, p += take, len -= take;
            if (n == 64) block(buf), n = 0;
        }
    }
    void final(u8 out[20]) {
        const std::uint64_t bits = total * 8;
        const u8 one = 0x80, zero = 0;
        update(&one, 1);
        while (n != 56) update(&zero, 1);
        u8 lenb[8];
        for (int i = 0; i < 8; i++) lenb[i] = u8(bits >> (56 - 8 * i));
        update(lenb, 8);
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) out[4 * i + j] = u8(h[i] >> (24 - 8 * j));
    }
};

// ---- AES-128 (FIPS 197), encryption only (OFB needs no decryption) ----------------------------
struct Aes {
    u8 sbox[256];
    u8 rk[11][16];

    static u8 xt(u8 a) { return u8((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
    void makeSbox() {
        // Multiplicative inverse in GF(2^8) followed by the affine map.
        u8 p = 1, q = 1;
        do {
            p = u8(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= u8(q << 1);
            q ^= u8(q << 2);
            q ^= u8(q << 4);
            if (q & 0x80) q ^= 0x09;
            const u8 x = u8(q ^ u8((q << 1) | (q >> 7)) ^ u8((q << 2) | (q >> 6)) ^ u8((q << 3) | (q >> 5)) ^
                            u8((q << 4) | (q >> 4)));
            sbox[p] = u8(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
    }
    explicit Aes(const u8 key[16]) {
        makeSbox();
        std::memcpy(rk[0], key, 16);
        u8 rc = 1;
        for (int r = 1; r <= 10; r++) {
            u8 t[4] = {rk[r - 1][13], rk[r - 1][14], rk[r - 1][15], rk[r - 1][12]};
            for (u8& b : t) b = sbox[b];
            t[0] ^= rc;
            rc = xt(rc);
            for (int i = 0; i < 4; i++) rk[r][i] = u8(rk[r - 1][i] ^ t[i]);
            for (int i = 4; i < 16; i++) rk[r][i] = u8(rk[r - 1][i] ^ rk[r][i - 4]);
        }
    }
    void encrypt(u8 s[16]) const {
        for (int i = 0; i < 16; i++) s[i] ^= rk[0][i];
        for (int r = 1; r <= 10; r++) {
            u8 t[16];
            for (int i = 0; i < 16; i++) t[i] = sbox[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows
            if (r != 10) {
                for (int c = 0; c < 4; c++) {
                    const u8 a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                    s[4 * c] = u8(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                    s[4 * c + 1] = u8(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                    s[4 * c + 2] = u8(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                    s[4 * c + 3] = u8(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
                }
            } else {
                std::memcpy(s, t, 16);
            }
            for (int i = 0; i < 16; i++) s[i] ^= rk[r][i];
        }
    }
};

// ---- RSA public operation: Montgomery multiplication, 64 limbs of 32 bits ----------------------
constexpr int kLimbs = 64;
using Num = u32[kLimbs];  // little-endian limbs

void fromBytes(const u8* be, Num out) {
    for (int i = 0; i < kLimbs; i++) {
        const u8* p = be + 256 - 4 * (i + 1);
        out[i] = u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3];
    }
}
void toBytes(const Num in, u8* be) {
    for (int i = 0; i < kLimbs; i++) {
        u8* p = be + 256 - 4 * (i + 1);
        p[0] = u8(in[i] >> 24), p[1] = u8(in[i] >> 16), p[2] = u8(in[i] >> 8), p[3] = u8(in[i]);
    }
}
bool geq(const u32* a, const Num n) {  // a has kLimbs limbs
    for (int i = kLimbs - 1; i >= 0; i--)
        if (a[i] != n[i]) return a[i] > n[i];
    return true;
}
void sub(u32* a, const Num n) {
    std::uint64_t borrow = 0;
    for (int i = 0; i < kLimbs; i++) {
        const std::uint64_t d = std::uint64_t(a[i]) - n[i] - borrow;
        a[i] = u32(d);
        borrow = (d >> 63) & 1;
    }
}

struct Mont {
    Num n;
    u32 n0inv;  // -n^-1 mod 2^32

    explicit Mont(const Num mod) {
        std::memcpy(n, mod, sizeof(Num));
        u32 inv = 1;  // Newton iteration for n[0]^-1 mod 2^32 (n odd)
        for (int i = 0; i < 5; i++) inv *= 2 - n[0] * inv;
        n0inv = u32(0) - inv;
    }
    // out = a * b * R^-1 mod n (CIOS)
    void mul(const Num a, const Num b, Num out) const {
        u32 t[kLimbs + 2] = {};
        for (int i = 0; i < kLimbs; i++) {
            std::uint64_t carry = 0;
            for (int j = 0; j < kLimbs; j++) {
                const std::uint64_t v = std::uint64_t(t[j]) + std::uint64_t(a[j]) * b[i] + carry;
                t[j] = u32(v);
                carry = v >> 32;
            }
            std::uint64_t v = std::uint64_t(t[kLimbs]) + carry;
            t[kLimbs] = u32(v);
            t[kLimbs + 1] = u32(v >> 32);
            const u32 m = t[0] * n0inv;
            carry = (std::uint64_t(t[0]) + std::uint64_t(m) * n[0]) >> 32;
            for (int j = 1; j < kLimbs; j++) {
                const std::uint64_t w = std::uint64_t(t[j]) + std::uint64_t(m) * n[j] + carry;
                t[j - 1] = u32(w);
                carry = w >> 32;
            }
            v = std::uint64_t(t[kLimbs]) + carry;
            t[kLimbs - 1] = u32(v);
            t[kLimbs] = t[kLimbs + 1] + u32(v >> 32);
        }
        if (t[kLimbs] || geq(t, n)) sub(t, n);
        std::memcpy(out, t, sizeof(Num));
    }
};

// out = in^65537 mod modulus (all 256-byte big-endian). False for an even or too small modulus.
bool rsaPublic(const u8 modulus[256], const u8 in[256], u8 out[256]) {
    Num n, s;
    fromBytes(modulus, n);
    fromBytes(in, s);
    if (!(n[0] & 1) || geq(s, n)) return false;
    const Mont m(n);
    // R^2 mod n by doubling 1 4096 times (R = 2^2048).
    u32 r2[kLimbs + 1] = {1};
    for (int i = 0; i < 2 * 32 * kLimbs; i++) {
        u32 carry = 0;
        for (int j = 0; j <= kLimbs; j++) {
            const u32 nc = r2[j] >> 31;
            r2[j] = (r2[j] << 1) | carry;
            carry = nc;
        }
        if (r2[kLimbs] || geq(r2, n)) {
            sub(r2, n);
            r2[kLimbs] = 0;
        }
    }
    Num sm, acc, one = {1};
    m.mul(s, r2, sm);  // s * R mod n
    std::memcpy(acc, sm, sizeof(Num));
    for (int i = 0; i < 16; i++) m.mul(acc, acc, acc);
    m.mul(acc, sm, acc);
    m.mul(acc, one, acc);  // out of Montgomery form
    toBytes(acc, out);
    return true;
}

u8 hexNibble(char c) { return u8(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }
std::vector<u8> unhex(const char* h) {
    std::vector<u8> v;
    for (; h[0] && h[1]; h += 2) v.push_back(u8(hexNibble(h[0]) << 4 | hexNibble(h[1])));
    return v;
}

}  // namespace

void sha1(const void* data, size_t len, u8 out[20]) {
    Sha1 s;
    s.update(static_cast<const u8*>(data), len);
    s.final(out);
}

void aes128Ofb(const u8 key[16], const u8 iv[16], u8* data, size_t len) {
    const Aes aes(key);
    u8 fb[16];
    std::memcpy(fb, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        aes.encrypt(fb);
        const size_t n = std::min(size_t(16), len - off);
        for (size_t i = 0; i < n; i++) data[off + i] ^= fb[i];
    }
}

bool rsa2048VerifySha1(const u8 modulus[256], const u8 sig[256], const void* msg, size_t len) {
    u8 em[256];
    if (!rsaPublic(modulus, sig, em)) return false;
    static const u8 kDigestInfo[15] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E, 0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14};
    u8 want[256];
    want[0] = 0x00, want[1] = 0x01;
    const size_t padEnd = 256 - 20 - sizeof(kDigestInfo) - 1;
    std::memset(want + 2, 0xFF, padEnd - 2);
    want[padEnd] = 0x00;
    std::memcpy(want + padEnd + 1, kDigestInfo, sizeof(kDigestInfo));
    sha1(msg, len, want + 256 - 20);
    u8 diff = 0;
    for (int i = 0; i < 256; i++) diff |= u8(em[i] ^ want[i]);
    return diff == 0;
}

bool selfTest(std::string* failure) {
    auto fail = [&](const char* what) {
        if (failure) *failure = what;
        return false;
    };
    u8 h[20];
    sha1("abc", 3, h);
    if (std::memcmp(h, unhex("a9993e364706816aba3e25717850c26c9cd0d89d").data(), 20)) return fail("SHA-1 abc");
    {
        const std::string m(1000, 'a');
        sha1(m.data(), m.size(), h);  // multi-block
        if (std::memcmp(h, unhex("291e9a6c66994949b57ba5e650361e98fc36b1ba").data(), 20)) return fail("SHA-1 1000a");
    }
    {
        const auto key = unhex("2b7e151628aed2a6abf7158809cf4f3c");
        const auto iv = unhex("000102030405060708090a0b0c0d0e0f");
        auto data = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
        aes128Ofb(key.data(), iv.data(), data.data(), data.size());  // SP 800-38A F.4.1
        if (data != unhex("3b3fd92eb72dad20333449f8e83cfb4a7789508d16918f03f53c52dac54ed825")) return fail("AES-OFB");
    }
    {
        const auto n = unhex(
            "c81daf8515fdae90077119892b048315b0360444bdf88df302520322541847aad0cb910a610a995c8ea4f790aea5a47b9c56b4929c9ff6f92d0eb6a351d44ee558596e381936793351de4c3fe9fbe8845cca8dc2d5506216336f145f349e9145731d20a430d36c3c5127299212506518949c746d0ca6a1a4ba61cb36633b17b0e82a75e351a169f50e1efefa3bb85417bad75a4aea13d1082ac6758510aa4e4170dd707da8fa31f1de99b897767f9d3172e150465937747ce7f7e3c3407be9049c530c94e57c31a5e6309d8ac20a3c142d8ebd2f001dc3fc25faebb306518f97555fe7bbfc388dad9c203801f8766b7c47a152c3d27a0457b81f9e8bd324daa1");
        const auto s = unhex(
            "28a3d6d537e5d5090d33c042ad97db2081e47f9cc713e983ebb94121172153c8b191abbb01bc810e9c873fc9753047d7f910ab0513811fdbc06ede61aacf170b0105a3904ec8a03179d0ac37e167c66f513b69a99429cd8311826a5aba05a22039e4a44cc8d1d782abd0e1a26c44eef97eb88dbc4899c4572d3d43ed95255902a16acfba5d54a1f2edb21cabe3f51fbc5b61a01f265791c48affc9a9476d1601b1b4d8afe032c7b10f14d1609855f267125dc1c16ef5940b1562ed35a37a931b8dc8d3bceaf3a3e00ab75b6bc5773c43a3062b7882062033f319dc0063b8882e3ed1b654ea0faeeb64ca3eb7c100ae4705288dc7dd1f7d27754b6c835141116f");
        const auto want = unhex(
            "05946d26db6d7bcb8ed319ee7d6ef1c66fff720d1c3c0da76eed2d991e7e3ee82f4599b9fc42ab97ed7df222aede940a6f0ebd976172faf66582301d0fe857bbe6d29ebd53e20ce92765ae752def07ef85d4c1590bdb1916043ff71e8c0a84842ba67f0233a1eb9c072f12ffb5beebb6b56dc4f8f3dd2439ddc2018af396289780c5678e89c2945ed897b0bbff0e49801e8f93414d2f265f1b8bc29744824a9bd6cd5e811bb3183e9aa52ba9043cc98e71192b7456e76bdd9eb129e36fd4e80b3230d150e740aedcc539ef49f6f03911037ff736eea7de452927cb1bc181ae8f1ecda7a8aefd4f453bb098d34677b2304b03831548684e8e4d7b4f52fd0d8348");
        u8 out[256];
        if (!rsaPublic(n.data(), s.data(), out) || std::memcmp(out, want.data(), 256)) return fail("RSA modexp");
    }
    return true;
}

}  // namespace wc24crypto
