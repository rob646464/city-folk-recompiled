// SHA-512, Ed25519 (RFC 8032) and X25519 (RFC 7748) for online friends (online_ed25519.h).
// Field elements mod 2^255-19 as 16 signed 64-bit limbs of 16 bits; constant-time ladders and
// selects. Constants derived from the curve definition (d = -121665/121666, sqrt(-1), base point
// y = 4/5) by a scratch script, checked by the RFC test vectors in edSelfTest().
#include "recomp/online_ed25519.h"

#include "recomp/online_crypto.h"

#include <cstring>

namespace onlinecrypto {
namespace {

using u8 = uint8_t;
using u64 = uint64_t;
using i64 = int64_t;

constexpr u64 K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

inline u64 rotr64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

struct Sha512 {
    u64 h[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    u8 buf[128];
    size_t n = 0;
    u64 len = 0;
    void block(const u8* p) {
        u64 w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int k = 0; k < 8; k++) w[i] = w[i] << 8 | p[8 * i + k];
        }
        for (int i = 16; i < 80; i++) {
            const u64 s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const u64 s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u64 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; i++) {
            const u64 S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            const u64 t1 = hh + S1 + ((e & f) ^ (~e & g)) + K512[i] + w[i];
            const u64 S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            const u64 t2 = S0 + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, size_t l) {
        const u8* p = static_cast<const u8*>(data);
        len += l;
        while (l) {
            const size_t t = l < 128 - n ? l : 128 - n;
            std::memcpy(buf + n, p, t);
            n += t;
            p += t;
            l -= t;
            if (n == 128) {
                block(buf);
                n = 0;
            }
        }
    }
    void final(u8 out[64]) {
        const u64 bits = len * 8;
        const u8 one = 0x80, zero = 0;
        update(&one, 1);
        while (n != 112) update(&zero, 1);
        u8 lb[16] = {};
        for (int i = 0; i < 8; i++) lb[15 - i] = u8(bits >> (8 * i));
        update(lb, 16);
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 8; k++) out[8 * i + k] = u8(h[i] >> (56 - 8 * k));
    }
};

// ---- field arithmetic ----
using gf = i64[16];
const gf gf0 = {0};
const gf gf1 = {1};
const gf k121665 = {0xDB41, 1};
const gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const gf X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const gf Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const gf I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

void set(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

void car(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (i64(1) << 16);
        const i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

void sel(gf p, gf q, int b) {
    const i64 c = ~(i64(b) - 1);
    for (int i = 0; i < 16; i++) {
        const i64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack(u8* o, const gf n) {
    gf m, t;
    set(t, n);
    car(t);
    car(t);
    car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = int((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = u8(t[i] & 0xff);
        o[2 * i + 1] = u8(t[i] >> 8);
    }
}

bool neq(const gf a, const gf b) {
    u8 c[32], d[32];
    pack(c, a);
    pack(d, b);
    return !equal(c, d, 32);
}

int par(const gf a) {
    u8 d[32];
    pack(d, a);
    return d[0] & 1;
}

void unpack(gf o, const u8* n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}
void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}
void M(gf o, const gf a, const gf b) {
    i64 t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car(o);
    car(o);
}
void S(gf o, const gf a) { M(o, a, a); }

void inv(gf o, const gf i) {
    gf c;
    set(c, i);
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    set(o, c);
}

void pow2523(gf o, const gf i) {
    gf c;
    set(c, i);
    for (int a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    set(o, c);
}

// ---- Edwards points (X, Y, Z, T) ----
void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

void cswap(gf p[4], gf q[4], u8 b) {
    for (int i = 0; i < 4; i++) sel(p[i], q[i], b);
}

void packPoint(u8* r, gf p[4]) {
    gf tx, ty, zi;
    inv(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack(r, ty);
    r[31] ^= u8(par(tx) << 7);
}

void scalarmult(gf p[4], gf q[4], const u8* s) {
    set(p[0], gf0);
    set(p[1], gf1);
    set(p[2], gf1);
    set(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        const u8 b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

void scalarbase(gf p[4], const u8* s) {
    gf q[4];
    set(q[0], X);
    set(q[1], Y);
    set(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

const u64 L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};

void modL(u8* r, i64 x[64]) {
    for (int i = 63; i >= 32; --i) {
        i64 carry = 0;
        int j;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * i64(L[j - (i - 32)]);
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    i64 carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * i64(L[j]);
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= carry * i64(L[j]);
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = u8(x[i] & 255);
    }
}

void reduce(u8* r) {
    i64 x[64];
    for (int i = 0; i < 64; i++) x[i] = u64(r[i]);
    for (int i = 0; i < 64; i++) r[i] = 0;
    modL(r, x);
}

bool unpackNeg(gf r[4], const u8 p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set(r[2], gf1);
    unpack(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);
    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq(chk, num)) M(r[0], r[0], I);
    S(chk, r[0]);
    M(chk, chk, den);
    if (neq(chk, num)) return false;
    if (par(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return true;
}

void expand(const u8 seed[32], u8 d[64]) {
    sha512(seed, 32, d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

}  // namespace

void sha512(const void* data, size_t len, uint8_t out[64]) {
    Sha512 s;
    s.update(data, len);
    s.final(out);
}

void ed25519PublicKey(const uint8_t seed[32], uint8_t pub[32]) {
    u8 d[64];
    gf p[4];
    expand(seed, d);
    scalarbase(p, d);
    packPoint(pub, p);
}

void ed25519Sign(const uint8_t seed[32], const void* msg, size_t len, uint8_t sig[64]) {
    u8 d[64], h[64], r[64], pub[32];
    gf p[4];
    expand(seed, d);
    scalarbase(p, d);
    packPoint(pub, p);
    Sha512 s1;
    s1.update(d + 32, 32);
    s1.update(msg, len);
    s1.final(r);
    reduce(r);
    scalarbase(p, r);
    packPoint(sig, p);
    Sha512 s2;
    s2.update(sig, 32);
    s2.update(pub, 32);
    s2.update(msg, len);
    s2.final(h);
    reduce(h);
    i64 x[64] = {0};
    for (int i = 0; i < 32; i++) x[i] = u64(r[i]);
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += h[i] * u64(d[j]);
    modL(sig + 32, x);
}

bool ed25519Verify(const uint8_t pub[32], const void* msg, size_t len, const uint8_t sig[64]) {
    gf p[4], q[4];
    if (sig[63] & 224) return false;
    if (!unpackNeg(q, pub)) return false;
    u8 h[64], t[32];
    Sha512 s;
    s.update(sig, 32);
    s.update(pub, 32);
    s.update(msg, len);
    s.final(h);
    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    add(p, q);
    packPoint(t, p);
    return equal(sig, t, 32);
}

void x25519(const uint8_t scalar[32], const uint8_t* point, uint8_t out[32]) {
    u8 z[32];
    i64 x[80];
    gf a, b, c, d, e, f;
    std::memcpy(z, scalar, 32);
    z[31] = u8((scalar[31] & 127) | 64);
    z[0] &= 248;
    u8 base[32] = {9};
    unpack(x, point ? point : base);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        const int r = (z[i >> 3] >> (i & 7)) & 1;
        sel(a, b, r);
        sel(c, d, r);
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, k121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel(a, b, r);
        sel(c, d, r);
    }
    gf cinv, res;
    inv(cinv, c);
    M(res, a, cinv);
    pack(out, res);
}

bool edSelfTest(std::string* failure) {
    auto fail = [&](const char* w) {
        if (failure) *failure = w;
        return false;
    };
    u8 h[64];
    sha512("abc", 3, h);
    if (hex(h, 64) != "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")
        return fail("sha512 abc");
    auto unhex = [](const char* s, u8* o) {
        for (size_t i = 0; s[2 * i]; i++) {
            auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            o[i] = u8(v(s[2 * i]) << 4 | v(s[2 * i + 1]));
        }
    };
    // RFC 8032 7.1 test 1 (empty message).
    u8 seed[32], pub[32], sig[64];
    unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
    ed25519PublicKey(seed, pub);
    if (hex(pub, 32) != "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a") return fail("ed25519 public key");
    ed25519Sign(seed, "", 0, sig);
    if (hex(sig, 64) != "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")
        return fail("ed25519 signature");
    if (!ed25519Verify(pub, "", 0, sig)) return fail("ed25519 verify");
    sig[5] ^= 1;
    if (ed25519Verify(pub, "", 0, sig)) return fail("ed25519 forged");
    // RFC 7748 5.2 first X25519 vector.
    u8 sc[32], u[32], out[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
    x25519(sc, u, out);
    if (hex(out, 32) != "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552") return fail("x25519");
    // Diffie-Hellman agreement.
    u8 a[32], b[32], pa[32], pb[32], s1[32], s2[32];
    randomBytes(a, 32);
    randomBytes(b, 32);
    x25519(a, nullptr, pa);
    x25519(b, nullptr, pb);
    x25519(a, pb, s1);
    x25519(b, pa, s2);
    if (!equal(s1, s2, 32)) return fail("x25519 agreement");
    return true;
}

}  // namespace onlinecrypto
