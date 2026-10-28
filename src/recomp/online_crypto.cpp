// Cryptographic primitives for online play (online_crypto.h). Written from FIPS 180-4 (SHA-256),
// RFC 2104 (HMAC), RFC 8018 (PBKDF2) and RFC 8439 (ChaCha20).
#include "recomp/online_crypto.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <cstdio>
#else
#include <cstdio>
#include <cstdlib>
#endif

namespace onlinecrypto {
namespace {

constexpr u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
inline u32 rotl(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
inline u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
inline u32 le32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
inline void putLe32(u8* p, u32 v) {
    p[0] = u8(v);
    p[1] = u8(v >> 8);
    p[2] = u8(v >> 16);
    p[3] = u8(v >> 24);
}

}  // namespace

Sha256::Sha256() {
    static constexpr u32 init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof h_);
}

void Sha256::block(const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        const u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 t1 = h + S1 + ch + K[i] + w[i];
        const u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const u8* p = static_cast<const u8*>(data);
    len_ += len;
    while (len) {
        const size_t take = std::min(len, size_t(64) - n_);
        std::memcpy(buf_ + n_, p, take);
        n_ += take;
        p += take;
        len -= take;
        if (n_ == 64) {
            block(buf_);
            n_ = 0;
        }
    }
}

void Sha256::final(u8 out[32]) {
    const u64 bits = len_ * 8;
    const u8 pad = 0x80;
    update(&pad, 1);
    const u8 zero = 0;
    while (n_ != 56) update(&zero, 1);
    u8 lenBytes[8];
    for (int i = 0; i < 8; i++) lenBytes[i] = u8(bits >> (56 - 8 * i));
    update(lenBytes, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = u8(h_[i] >> 24);
        out[4 * i + 1] = u8(h_[i] >> 16);
        out[4 * i + 2] = u8(h_[i] >> 8);
        out[4 * i + 3] = u8(h_[i]);
    }
}

void sha256(const void* data, size_t len, u8 out[32]) {
    Sha256 s;
    s.update(data, len);
    s.final(out);
}

void hmacSha256(const void* key, size_t keyLen, const void* a, size_t aLen, const void* b, size_t bLen, u8 out[32]) {
    u8 k[64] = {};
    if (keyLen > 64) {
        sha256(key, keyLen, k);
    } else if (keyLen) {
        std::memcpy(k, key, keyLen);
    }
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    u8 inner[32];
    Sha256 s;
    s.update(ipad, 64);
    if (aLen) s.update(a, aLen);
    if (bLen) s.update(b, bLen);
    s.final(inner);
    Sha256 o;
    o.update(opad, 64);
    o.update(inner, 32);
    o.final(out);
}

void hmacSha256(const void* key, size_t keyLen, const void* data, size_t len, u8 out[32]) {
    hmacSha256(key, keyLen, data, len, nullptr, 0, out);
}

void pbkdf2Sha256(const void* pw, size_t pwLen, const void* salt, size_t saltLen, u32 iterations, u8* out, size_t outLen) {
    for (u32 blockNo = 1; outLen; blockNo++) {
        u8 be[4] = {u8(blockNo >> 24), u8(blockNo >> 16), u8(blockNo >> 8), u8(blockNo)};
        u8 u[32], t[32];
        hmacSha256(pw, pwLen, salt, saltLen, be, 4, u);
        std::memcpy(t, u, 32);
        for (u32 i = 1; i < iterations; i++) {
            hmacSha256(pw, pwLen, u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        const size_t take = std::min(outLen, size_t(32));
        std::memcpy(out, t, take);
        out += take;
        outLen -= take;
    }
}

namespace {
inline void qr(u32& a, u32& b, u32& c, u32& d) {
    a += b; d ^= a; d = rotl(d, 16);
    c += d; b ^= c; b = rotl(b, 12);
    a += b; d ^= a; d = rotl(d, 8);
    c += d; b ^= c; b = rotl(b, 7);
}
void chachaBlock(const u8 key[32], const u8 nonce[12], u32 counter, u8 out[64]) {
    u32 s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
    u32 x[16];
    std::memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        qr(x[0], x[4], x[8], x[12]);
        qr(x[1], x[5], x[9], x[13]);
        qr(x[2], x[6], x[10], x[14]);
        qr(x[3], x[7], x[11], x[15]);
        qr(x[0], x[5], x[10], x[15]);
        qr(x[1], x[6], x[11], x[12]);
        qr(x[2], x[7], x[8], x[13]);
        qr(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) putLe32(out + 4 * i, x[i] + s[i]);
}
}  // namespace

void chacha20(const u8 key[32], const u8 nonce[12], u32 counter, u8* data, size_t len) {
    u8 ks[64];
    while (len) {
        chachaBlock(key, nonce, counter++, ks);
        const size_t take = std::min(len, size_t(64));
        for (size_t i = 0; i < take; i++) data[i] ^= ks[i];
        data += take;
        len -= take;
    }
}

bool randomBytes(void* out, size_t len) {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
    u8* p = static_cast<u8*>(out);
    while (len) {
        const ssize_t n = getrandom(p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
#else
    arc4random_buf(out, len);
    return true;
#endif
}

bool equal(const void* a, const void* b, size_t len) {
    const u8* x = static_cast<const u8*>(a);
    const u8* y = static_cast<const u8*>(b);
    u8 d = 0;
    for (size_t i = 0; i < len; i++) d |= x[i] ^ y[i];
    return d == 0;
}

SealKey deriveSealKey(const u8 session[32], const char* label) {
    SealKey k;
    std::string l = std::string("cf-seal-enc-") + label;
    hmacSha256(session, 32, l.data(), l.size(), k.enc);
    l = std::string("cf-seal-mac-") + label;
    hmacSha256(session, 32, l.data(), l.size(), k.mac);
    return k;
}

namespace {
void nonceBytes(u64 n, u8 out[12]) {
    std::memset(out, 0, 12);
    for (int i = 0; i < 8; i++) out[4 + i] = u8(n >> (8 * i));
}
void tagOf(const SealKey& k, u64 nonce, const u8* hdr, size_t hdrLen, const u8* body, size_t bodyLen, u8 tag[kTagSize]) {
    // MAC over nonce || header || ciphertext.
    u8 nb[8];
    for (int i = 0; i < 8; i++) nb[i] = u8(nonce >> (8 * i));
    u8 k2[64] = {};
    std::memcpy(k2, k.mac, 32);
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k2[i] ^ 0x36;
        opad[i] = k2[i] ^ 0x5c;
    }
    u8 inner[32], full[32];
    Sha256 s;
    s.update(ipad, 64);
    s.update(nb, 8);
    if (hdrLen) s.update(hdr, hdrLen);
    if (bodyLen) s.update(body, bodyLen);
    s.final(inner);
    Sha256 o;
    o.update(opad, 64);
    o.update(inner, 32);
    o.final(full);
    std::memcpy(tag, full, kTagSize);
}
}  // namespace

void seal(const SealKey& k, u64 nonce, const u8* hdr, size_t hdrLen, u8* body, size_t bodyLen, u8 tag[kTagSize]) {
    u8 n[12];
    nonceBytes(nonce, n);
    chacha20(k.enc, n, 1, body, bodyLen);
    tagOf(k, nonce, hdr, hdrLen, body, bodyLen, tag);
}

bool open(const SealKey& k, u64 nonce, const u8* hdr, size_t hdrLen, u8* body, size_t bodyLen, const u8 tag[kTagSize]) {
    u8 t[kTagSize];
    tagOf(k, nonce, hdr, hdrLen, body, bodyLen, t);
    if (!equal(t, tag, kTagSize)) return false;
    u8 n[12];
    nonceBytes(nonce, n);
    chacha20(k.enc, n, 1, body, bodyLen);
    return true;
}

std::string hex(const void* data, size_t len) {
    static const char* d = "0123456789abcdef";
    std::string s;
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < len; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

bool selfTest(std::string* failure) {
    auto fail = [&](const char* what) {
        if (failure) *failure = what;
        return false;
    };
    u8 h[32];
    sha256("abc", 3, h);
    if (hex(h, 32) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") return fail("sha256 abc");
    sha256("", 0, h);
    if (hex(h, 32) != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") return fail("sha256 empty");
    std::string million(1000000, 'a');
    sha256(million.data(), million.size(), h);
    if (hex(h, 32) != "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") return fail("sha256 million");
    const char* jefe = "what do ya want for nothing?";
    hmacSha256("Jefe", 4, jefe, std::strlen(jefe), h);
    if (hex(h, 32) != "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") return fail("hmac jefe");
    pbkdf2Sha256("password", 8, "salt", 4, 1, h, 32);
    if (hex(h, 32) != "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b") return fail("pbkdf2 c=1");
    pbkdf2Sha256("password", 8, "salt", 4, 4096, h, 32);
    if (hex(h, 32) != "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a") return fail("pbkdf2 c=4096");
    u8 key[32];
    for (int i = 0; i < 32; i++) key[i] = u8(i);
    const u8 nonce[12] = {0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0};
    const char* pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    std::string buf(pt);
    chacha20(key, nonce, 1, reinterpret_cast<u8*>(buf.data()), buf.size());
    if (hex(buf.data(), 16) != "6e2e359a2568f98041ba0728dd0d6981") return fail("chacha20");
    // Seal round trip and tamper detection.
    u8 sess[32];
    for (int i = 0; i < 32; i++) sess[i] = u8(0xA0 + i);
    const SealKey sk = deriveSealKey(sess, "test");
    u8 hdr[4] = {1, 2, 3, 4};
    std::string msg = "hello town";
    u8 tag[kTagSize];
    seal(sk, 7, hdr, 4, reinterpret_cast<u8*>(msg.data()), msg.size(), tag);
    std::string tampered = msg;
    tampered[0] ^= 1;
    if (open(sk, 7, hdr, 4, reinterpret_cast<u8*>(tampered.data()), tampered.size(), tag)) return fail("seal tamper");
    if (open(sk, 8, hdr, 4, reinterpret_cast<u8*>(msg.data()), msg.size(), tag)) return fail("seal nonce");
    if (!open(sk, 7, hdr, 4, reinterpret_cast<u8*>(msg.data()), msg.size(), tag) || msg != "hello town") return fail("seal open");
    return true;
}

}  // namespace onlinecrypto
