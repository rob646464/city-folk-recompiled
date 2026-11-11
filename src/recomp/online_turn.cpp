// TURN client (online_turn.h). Written from RFC 8489 / 5389 (STUN), RFC 8656 / 5766 (TURN),
// RFC 1321 (MD5), RFC 3174 (SHA-1), RFC 2104 (HMAC) and RFC 5769 (test vectors); no third-party
// code.
#include "recomp/online_turn.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "recomp/online_crypto.h"

namespace online::turn {
namespace {

inline uint32_t rotl(uint32_t x, int n) { return x << n | x >> (32 - n); }

// ---------------------------------------------------------------------------------------------
// MD5 (RFC 1321)
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                           14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                           4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

struct Md5 {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint8_t buf[64];
    size_t n = 0;
    uint64_t len = 0;

    void block(const uint8_t* p) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++) m[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) & 15;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) & 15;
            }
            f += a + kMd5K[i] + m[g];
            a = d;
            d = c;
            c = b;
            b += rotl(f, kMd5S[i]);
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    void update(const void* data, size_t l) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len += l;
        while (l) {
            const size_t k = std::min(l, sizeof buf - n);
            std::memcpy(buf + n, p, k);
            n += k;
            p += k;
            l -= k;
            if (n == sizeof buf) {
                block(buf);
                n = 0;
            }
        }
    }
    void final(uint8_t out[16]) {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80, zero = 0;
        update(&pad, 1);
        while (n != 56) update(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (8 * i));
        update(l, 8);
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) out[i * 4 + k] = uint8_t(h[i] >> (8 * k));
    }
};

// ---------------------------------------------------------------------------------------------
// SHA-1 (RFC 3174)
// ---------------------------------------------------------------------------------------------
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    uint8_t buf[64];
    size_t n = 0;
    uint64_t len = 0;

    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    void update(const void* data, size_t l) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len += l;
        while (l) {
            const size_t k = std::min(l, sizeof buf - n);
            std::memcpy(buf + n, p, k);
            n += k;
            p += k;
            l -= k;
            if (n == sizeof buf) {
                block(buf);
                n = 0;
            }
        }
    }
    void final(uint8_t out[20]) {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80, zero = 0;
        update(&pad, 1);
        while (n != 56) update(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
        update(l, 8);
        for (int i = 0; i < 5; i++)
            for (int k = 0; k < 4; k++) out[i * 4 + k] = uint8_t(h[i] >> (24 - 8 * k));
    }
};

// ---------------------------------------------------------------------------------------------
// STUN messages
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kCookie = 0x2112A442;
constexpr uint32_t kFingerprintXor = 0x5354554e;

// Methods (the request type; + kSuccess / kError for the answers, + kIndication).
constexpr uint16_t kAllocate = 0x003, kRefresh = 0x004, kSend = 0x006, kData = 0x007, kCreatePermission = 0x008;
constexpr uint16_t kIndication = 0x0010, kSuccess = 0x0100, kError = 0x0110, kClassMask = 0x0110;

constexpr uint16_t aUsername = 0x0006, aIntegrity = 0x0008, aErrorCode = 0x0009, aLifetime = 0x000D, aXorPeer = 0x0012,
                   aData = 0x0013, aRealm = 0x0014, aNonce = 0x0015, aXorRelayed = 0x0016, aRequestedTransport = 0x0019,
                   aXorMapped = 0x0020, aFingerprint = 0x8028;

inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A STUN message whose header and attribute lengths were checked (check()).
struct View {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint16_t type() const { return be16(p); }
    const uint8_t* txid() const { return p + 8; }
    // First attribute of this type: its value and length, nullptr when absent. *off = offset of
    // the attribute's header in the message.
    const uint8_t* find(uint16_t type, size_t* len, size_t* off = nullptr) const {
        size_t i = 20;
        while (i + 4 <= n) {
            const uint16_t t = be16(p + i);
            const size_t alen = be16(p + i + 2);
            if (t == type) {
                *len = alen;
                if (off) *off = i;
                return p + i + 4;
            }
            i += 4 + ((alen + 3) & ~size_t(3));
        }
        return nullptr;
    }
};

// True for a well-formed STUN message that fills the datagram exactly: magic cookie, length field,
// every attribute inside the message, and a right FINGERPRINT when there is one.
bool check(const uint8_t* p, size_t n, View* v) {
    if (!p || n < 20 || n > 65535 + 20 || (p[0] & 0xC0) != 0 || be32(p + 4) != kCookie) return false;
    const size_t len = be16(p + 2);
    if (len + 20 != n || (len & 3) != 0) return false;
    size_t i = 20;
    while (i < n) {
        if (i + 4 > n) return false;
        const uint16_t t = be16(p + i);
        const size_t alen = be16(p + i + 2);
        const size_t padded = (alen + 3) & ~size_t(3);
        if (padded > n - i - 4) return false;
        if (t == aFingerprint) {
            if (alen != 4 || i + 8 != n) return false;  // always the last attribute
            if ((crc32(p, i) ^ kFingerprintXor) != be32(p + i + 4)) return false;
        }
        i += 4 + padded;
    }
    v->p = p;
    v->n = n;
    return true;
}

// MESSAGE-INTEGRITY at `off` (attribute header): HMAC-SHA1 over the message before it, with the
// header's length field counting up to the end of the attribute.
void integrityOf(const uint8_t* msg, size_t off, const void* key, size_t keyLen, uint8_t mac[20]) {
    uint8_t hdr[20];
    std::memcpy(hdr, msg, 20);
    const size_t len = off + 24 - 20;
    hdr[2] = uint8_t(len >> 8);
    hdr[3] = uint8_t(len);
    hmacSha1(key, keyLen, hdr, 20, msg + 20, off - 20, mac);
}

// 0 = no MESSAGE-INTEGRITY, 1 = right, -1 = wrong.
int integrityState(const View& m, const void* key, size_t keyLen) {
    size_t len = 0, off = 0;
    const uint8_t* mi = m.find(aIntegrity, &len, &off);
    if (!mi) return 0;
    if (len != 20) return -1;
    uint8_t mac[20];
    integrityOf(m.p, off, key, keyLen, mac);
    return onlinecrypto::equal(mac, mi, 20) ? 1 : -1;
}

// XOR-MAPPED-ADDRESS style value (also XOR-PEER-ADDRESS, XOR-RELAYED-ADDRESS).
bool parseAddr(const uint8_t* v, size_t n, const uint8_t* txid, sock::Addr* out) {
    if (n < 8) return false;
    const uint16_t port = uint16_t(be16(v + 2) ^ (kCookie >> 16));
    if (v[1] == 1) {
        *out = sock::makeV4(be32(v + 4) ^ kCookie, port);
        return true;
    }
    if (v[1] == 2 && n >= 20) {
        uint8_t ip[16];
        const uint8_t mask[16] = {0x21, 0x12, 0xA4, 0x42, txid[0], txid[1], txid[2], txid[3],
                                  txid[4], txid[5], txid[6], txid[7], txid[8], txid[9], txid[10], txid[11]};
        for (int k = 0; k < 16; k++) ip[k] = v[4 + k] ^ mask[k];
        *out = sock::makeV6(ip, port);
        return true;
    }
    return false;
}

struct Builder {
    std::vector<uint8_t> b;
    Builder(uint16_t type, const uint8_t txid[12], size_t reserve = 128) {
        b.reserve(reserve);
        b = {uint8_t(type >> 8), uint8_t(type), 0, 0, 0x21, 0x12, 0xA4, 0x42};
        b.insert(b.end(), txid, txid + 12);
    }
    void setLen() {
        const size_t l = b.size() - 20;
        b[2] = uint8_t(l >> 8);
        b[3] = uint8_t(l);
    }
    void attr(uint16_t t, const void* v, size_t n) {
        const uint8_t h[4] = {uint8_t(t >> 8), uint8_t(t), uint8_t(n >> 8), uint8_t(n)};
        b.insert(b.end(), h, h + 4);
        const uint8_t* p = static_cast<const uint8_t*>(v);
        b.insert(b.end(), p, p + n);
        while (b.size() & 3) b.push_back(0);
        setLen();
    }
    void str(uint16_t t, const std::string& s) { attr(t, s.data(), s.size()); }
    void u32(uint16_t t, uint32_t v) {
        const uint8_t x[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        attr(t, x, 4);
    }
    void addr4(uint16_t t, uint32_t ip, uint16_t port) {
        const uint32_t x = ip ^ kCookie;
        const uint16_t xp = uint16_t(port ^ (kCookie >> 16));
        const uint8_t v[8] = {0, 1, uint8_t(xp >> 8), uint8_t(xp), uint8_t(x >> 24), uint8_t(x >> 16), uint8_t(x >> 8), uint8_t(x)};
        attr(t, v, 8);
    }
    void integrity(const void* key, size_t keyLen) {
        const size_t off = b.size();
        b.resize(off + 24);
        b[off] = uint8_t(aIntegrity >> 8);
        b[off + 1] = uint8_t(aIntegrity);
        b[off + 2] = 0;
        b[off + 3] = 20;
        setLen();
        uint8_t mac[20];
        hmacSha1(key, keyLen, b.data(), off, mac);
        std::memcpy(b.data() + off + 4, mac, 20);
    }
    void fingerprint() {
        const size_t off = b.size();
        b.resize(off + 8);
        b[off] = uint8_t(aFingerprint >> 8);
        b[off + 1] = uint8_t(aFingerprint & 0xFF);
        b[off + 2] = 0;
        b[off + 3] = 4;
        setLen();
        const uint32_t c = crc32(b.data(), off) ^ kFingerprintXor;
        b[off + 4] = uint8_t(c >> 24);
        b[off + 5] = uint8_t(c >> 16);
        b[off + 6] = uint8_t(c >> 8);
        b[off + 7] = uint8_t(c);
    }
};

// ---------------------------------------------------------------------------------------------
// Client state
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kLifetime = 600;        // seconds asked for
constexpr double kRto = 0.5;               // first retransmission after this, then doubled
constexpr int kSends = 4;                  // 0, 0.5, 1.5, 3.5 s; given up 4 s after the last
constexpr double kPermissionRefresh = 240; // permissions last 5 minutes on the server
constexpr double kPermissionLife = 300;
constexpr double kPermissionRetry = 30;    // after a refused CreatePermission
constexpr size_t kMaxPermissions = 64;
constexpr int kRecent = 16;

struct Req {
    uint8_t txid[12] = {};
    uint16_t method = 0;
    bool auth = false;
    bool release = false;   // Refresh with lifetime 0 that clears an older allocation before a new Allocate
    uint32_t lifetime = 0;  // Allocate / Refresh
    uint32_t peerIp = 0;    // CreatePermission
    uint16_t peerPort = 0;
    int stale = 0;          // times it was sent again for a new nonce
    std::vector<uint8_t> bytes;
    double first = 0, next = 0, rto = kRto;
    int sends = 0;
    bool grace = false;     // timed out; one more tick for an answer that is already in the socket
};

struct Perm {
    uint16_t port = 0;
    bool pending = false;
    bool installed = false;
    double nextAt = 0;      // when the next CreatePermission is due
    double validUntil = 0;
};

std::string errorText(uint16_t method, int code, const std::string& reason) {
    switch (code) {
    case 401:
    case 431:
    case 441: return "The relay server did not accept the user name or password.";
    case 403: return "The relay server refused the request.";
    case 437:
        return method == kAllocate ? "The relay server still holds an older session for this connection. Try again in a few minutes."
                                   : "The relay session was lost on the relay server.";
    case 442: return "The relay server does not relay UDP.";
    case 486: return "The relay account has reached its limit of sessions.";
    case 508: return "The relay server is full.";
    case 300: return "The relay server pointed to another server, which is not supported.";
    default: break;
    }
    std::string s = "The relay server answered with error " + std::to_string(code);
    if (!reason.empty()) s += " (" + reason + ")";
    return s + ".";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public primitives
// ---------------------------------------------------------------------------------------------
void md5(const void* data, size_t len, uint8_t out[16]) {
    Md5 m;
    m.update(data, len);
    m.final(out);
}

void sha1(const void* data, size_t len, uint8_t out[20]) {
    Sha1 s;
    s.update(data, len);
    s.final(out);
}

void hmacSha1(const void* key, size_t keyLen, const void* a, size_t aLen, const void* b, size_t bLen, uint8_t out[20]) {
    uint8_t k[64] = {};
    if (keyLen > 64) {
        sha1(key, keyLen, k);
    } else if (keyLen) {
        std::memcpy(k, key, keyLen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[20];
    Sha1 in;
    in.update(ipad, 64);
    if (aLen) in.update(a, aLen);
    if (bLen) in.update(b, bLen);
    in.final(inner);
    Sha1 o;
    o.update(opad, 64);
    o.update(inner, 20);
    o.final(out);
}

void hmacSha1(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[20]) {
    hmacSha1(key, keyLen, data, len, nullptr, 0, out);
}

uint32_t crc32(const void* data, size_t len) {
    static const struct Table {
        uint32_t t[256];
        Table() {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
        }
    } table;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = table.t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void longTermKey(const std::string& username, const std::string& realm, const std::string& password, uint8_t out[16]) {
    const std::string s = username + ":" + realm + ":" + password;
    md5(s.data(), s.size(), out);
}

// ---------------------------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------------------------
struct Client::Impl {
    enum class State { Idle, Allocating, Ready, Failed };
    State state = State::Idle;
    sock::Addr server, relayed, mapped;
    Creds creds;
    std::string error, realm, nonce;
    uint8_t key[16] = {};
    bool haveKey = false;
    bool triedRelease = false;  // one Refresh(0) + Allocate after 437 Allocation Mismatch
    double expiry = 0, refreshAt = 0;
    double lastNow = 0;  // the latest time tick() or onDatagram() was given
    bool refreshing = false;
    std::vector<Req> reqs;
    std::map<uint32_t, Perm> perms;  // by peer IPv4 (host order)
    uint8_t recent[kRecent][12] = {};
    int recentN = 0, recentPos = 0;
    uint8_t indId[8] = {};
    mutable uint32_t indCount = 0;

    void remember(const uint8_t txid[12]) {
        std::memcpy(recent[recentPos], txid, 12);
        recentPos = (recentPos + 1) % kRecent;
        recentN = std::min(recentN + 1, kRecent);
    }
    bool isRecent(const uint8_t* txid) const {
        for (int i = 0; i < recentN; i++)
            if (std::memcmp(recent[i], txid, 12) == 0) return true;
        return false;
    }

    void fail(const std::string& why) {
        state = State::Failed;
        error = why;
        for (const Req& r : reqs) remember(r.txid);
        reqs.clear();
        refreshing = false;
        for (auto& [ip, pm] : perms) pm.pending = pm.installed = false;
    }

    // (Re)builds the datagram of a request with a new transaction id and the current credentials.
    void build(Req& r) {
        if (!onlinecrypto::randomBytes(r.txid, 12)) {
            // No OS random source: ids only have to be unpredictable off-path, which this is not;
            // still unique per request.
            static uint32_t sCounter = 0x6b8b4567;
            for (int i = 0; i < 12; i++) r.txid[i] = uint8_t((sCounter = sCounter * 1103515245u + 12345u) >> 16);
        }
        Builder b(r.method, r.txid, 256);
        if (r.method == kAllocate) {
            const uint8_t udp[4] = {17, 0, 0, 0};
            b.attr(aRequestedTransport, udp, 4);
            b.u32(aLifetime, r.lifetime);
        } else if (r.method == kRefresh) {
            b.u32(aLifetime, r.lifetime);
        } else if (r.method == kCreatePermission) {
            b.addr4(aXorPeer, r.peerIp, r.peerPort);
        }
        r.auth = haveKey;
        if (haveKey) {
            b.str(aUsername, creds.username);
            b.str(aRealm, realm);
            b.str(aNonce, nonce);
            b.integrity(key, sizeof key);
        }
        r.bytes = std::move(b.b);
        r.sends = 0;
        r.next = 0;
        r.rto = kRto;
        r.grace = false;
    }
    void submit(Req r) {
        build(r);
        reqs.push_back(std::move(r));
    }
    void allocate() {
        Req r;
        r.method = kAllocate;
        r.lifetime = kLifetime;
        submit(std::move(r));
    }

    // Sends what is due: new refreshes and permissions, first transmissions, retransmissions.
    void pump(double now, std::vector<std::vector<uint8_t>>& out) {
        lastNow = now;
        if (state == State::Ready) {
            if (now >= expiry) {
                fail("The relay session ran out.");
                return;
            }
            if (!refreshing && now >= refreshAt) {
                refreshing = true;
                Req r;
                r.method = kRefresh;
                r.lifetime = kLifetime;
                submit(std::move(r));
            }
            for (auto& [ip, pm] : perms) {
                if (pm.pending || now < pm.nextAt) continue;
                pm.pending = true;
                Req r;
                r.method = kCreatePermission;
                r.peerIp = ip;
                r.peerPort = pm.port;
                submit(std::move(r));
            }
        }
        if (state != State::Ready && state != State::Allocating) return;
        for (size_t i = 0; i < reqs.size(); i++) {
            Req& r = reqs[i];
            if (now < r.next) continue;
            if (r.sends >= kSends) {
                if (!r.grace) {
                    r.grace = true;
                    continue;
                }
                fail("No answer from the relay server.");
                return;
            }
            if (r.sends == 0) r.first = now;
            out.push_back(r.bytes);
            r.sends++;
            r.next = now + r.rto;
            r.rto *= 2;
        }
    }

    // LIFETIME of an Allocate / Refresh success.
    void granted(const Req& r, const View& m) {
        size_t n = 0;
        const uint8_t* v = m.find(aLifetime, &n);
        uint32_t lt = (v && n >= 4) ? be32(v) : r.lifetime;
        if (lt == 0) {
            fail("The relay session was lost on the relay server.");
            return;
        }
        lt = std::min<uint32_t>(lt, 24 * 3600);
        expiry = r.first + lt;
        refreshAt = r.first + std::max(lt * 0.5, 0.25);
    }

    void success(const Req& r, const View& m) {
        size_t n = 0;
        switch (r.method) {
        case kAllocate: {
            const uint8_t* a = m.find(aXorRelayed, &n);
            sock::Addr rel;
            if (!a || !parseAddr(a, n, m.txid(), &rel) || rel.family() != AF_INET) {
                fail("The relay server gave no usable relay address.");
                return;
            }
            relayed = rel;
            mapped = sock::Addr();
            if ((a = m.find(aXorMapped, &n)) != nullptr) parseAddr(a, n, m.txid(), &mapped);
            state = State::Ready;
            refreshing = false;
            for (auto& [ip, pm] : perms) {
                pm.pending = pm.installed = false;
                pm.nextAt = 0;
            }
            granted(r, m);
            break;
        }
        case kRefresh:
            if (r.release) {
                allocate();
                break;
            }
            refreshing = false;
            granted(r, m);
            break;
        case kCreatePermission: {
            const auto it = perms.find(r.peerIp);
            if (it != perms.end()) {
                it->second.pending = false;
                it->second.installed = true;
                it->second.nextAt = r.first + kPermissionRefresh;
                it->second.validUntil = r.first + kPermissionLife;
            }
            break;
        }
        default: break;
        }
    }

    void failure(Req& r, const View& m, double now) {
        size_t n = 0;
        const uint8_t* e = m.find(aErrorCode, &n);
        const int code = (e && n >= 4) ? (e[2] & 7) * 100 + e[3] : 0;
        std::string reason;
        for (size_t i = 4; e && i < n && reason.size() < 80; i++)
            if (e[i] >= 0x20 && e[i] < 0x7F) reason.push_back(char(e[i]));

        // 401 to a request without credentials: the realm and nonce to authenticate with.
        // 438 Stale Nonce: the same request again with the new nonce.
        if (code == 438 || (code == 401 && !r.auth)) {
            size_t nn = 0, rn = 0;
            const uint8_t* nv = m.find(aNonce, &nn);
            const uint8_t* rv = m.find(aRealm, &rn);
            if (!nv || nn == 0 || nn > 1024 || (rv ? rn > 1024 : realm.empty()) || ++r.stale > 3) {
                fail(code == 438 ? "The relay server keeps refusing the request (stale nonce)."
                                 : "The relay server asked for a kind of sign-in that is not supported.");
                return;
            }
            nonce.assign(reinterpret_cast<const char*>(nv), nn);
            if (rv) realm.assign(reinterpret_cast<const char*>(rv), rn);
            longTermKey(creds.username, realm, creds.password, key);
            haveKey = true;
            build(r);
            reqs.push_back(std::move(r));
            return;
        }
        if (r.release) {  // nothing to release after all: the Allocate decides
            allocate();
            return;
        }
        if (r.method == kAllocate && code == 437 && haveKey && !triedRelease) {
            // The server still has an allocation for this socket (a run that ended without
            // close(), or a restart): release it, then allocate once more.
            triedRelease = true;
            Req q;
            q.method = kRefresh;
            q.lifetime = 0;
            q.release = true;
            submit(std::move(q));
            return;
        }
        if (r.method == kCreatePermission && code != 401 && code != 431 && code != 437 && code != 441) {
            // This peer is refused (403 for an address the server does not relay to, 508...): the
            // allocation itself is fine. Asked again later, in case it was temporary.
            const auto it = perms.find(r.peerIp);
            if (it != perms.end()) {
                it->second.pending = false;
                it->second.installed = false;
                it->second.nextAt = now + kPermissionRetry;
            }
            return;
        }
        fail(errorText(r.method, code, reason));
    }

    void onResponse(size_t idx, const View& m, bool ok, double now) {
        if (ok && reqs[idx].auth && haveKey && integrityState(m, key, sizeof key) < 0) return;  // damaged or forged: keep waiting
        Req r = std::move(reqs[idx]);
        reqs.erase(reqs.begin() + ptrdiff_t(idx));
        remember(r.txid);
        if (ok) {
            success(r, m);
        } else {
            failure(r, m, now);
        }
    }
};

Client::Client() : d_(new Impl) {
    onlinecrypto::randomBytes(d_->indId, sizeof d_->indId);
}
Client::~Client() = default;

void Client::start(const sock::Addr& server, const Creds& creds) {
    Impl& d = *d_;
    for (const Req& r : d.reqs) d.remember(r.txid);
    d.reqs.clear();
    d.server = server;
    d.creds = creds;
    d.state = Impl::State::Allocating;
    d.error.clear();
    d.realm.clear();
    d.nonce.clear();
    d.haveKey = false;
    d.triedRelease = false;
    d.refreshing = false;
    d.expiry = d.refreshAt = 0;
    d.relayed = d.mapped = sock::Addr();
    for (auto& [ip, pm] : d.perms) {
        pm.pending = pm.installed = false;
        pm.nextAt = 0;
    }
    d.allocate();
}

bool Client::started() const { return d_->state != Impl::State::Idle; }
const sock::Addr& Client::server() const { return d_->server; }
bool Client::ready() const { return d_->state == Impl::State::Ready; }
bool Client::failed() const { return d_->state == Impl::State::Failed; }
std::string Client::error() const { return d_->state == Impl::State::Failed ? d_->error : std::string(); }
sock::Addr Client::relayed() const { return d_->relayed; }
sock::Addr Client::mapped() const { return d_->mapped; }

void Client::tick(double now, std::vector<std::vector<uint8_t>>& out) { d_->pump(now, out); }

bool Client::onDatagram(const uint8_t* p, size_t n, double now, sock::Addr* peer, std::vector<uint8_t>* data,
                        std::vector<std::vector<uint8_t>>& out) {
    if (data) data->clear();
    Impl& d = *d_;
    View m;
    if (!d.server.valid() || !check(p, n, &m)) return false;
    const uint16_t type = m.type();
    if (type == (kData | kIndication)) {
        size_t an = 0, dn = 0;
        const uint8_t* a = m.find(aXorPeer, &an);
        const uint8_t* v = m.find(aData, &dn);
        sock::Addr from;
        if (!a || !v || !parseAddr(a, an, m.txid(), &from)) return false;
        if (d.state != Impl::State::Ready) return true;  // late data of an allocation that is gone
        if (peer) *peer = from;
        if (data) data->assign(v, v + dn);
        return true;
    }
    const uint16_t cls = type & kClassMask;
    if (cls != kSuccess && cls != kError) return false;
    const uint16_t method = type & uint16_t(~kClassMask);
    for (size_t i = 0; i < d.reqs.size(); i++) {
        if (d.reqs[i].method != method || std::memcmp(d.reqs[i].txid, m.txid(), 12) != 0) continue;
        d.onResponse(i, m, cls == kSuccess, now);
        d.pump(now, out);
        return true;
    }
    return d.isRecent(m.txid());  // a second answer to a retransmitted request
}

void Client::permit(const sock::Addr& peer) {
    Impl& d = *d_;
    const sock::Addr a = peer.normalized();
    if (a.family() != AF_INET) return;
    const uint32_t ip = a.v4();
    if (d.perms.count(ip) || d.perms.size() >= kMaxPermissions) return;
    Perm pm;
    pm.port = a.port();
    d.perms[ip] = pm;
}

bool Client::permitted(const sock::Addr& peer) const {
    const Impl& d = *d_;
    const sock::Addr a = peer.normalized();
    if (d.state != Impl::State::Ready || a.family() != AF_INET) return false;
    const auto it = d.perms.find(a.v4());
    return it != d.perms.end() && it->second.installed && d.lastNow < it->second.validUntil;
}

std::vector<uint8_t> Client::wrap(const sock::Addr& peer, const uint8_t* data, size_t n) const {
    const Impl& d = *d_;
    const sock::Addr a = peer.normalized();
    if (d.state != Impl::State::Ready || a.family() != AF_INET || n > kMaxPayload || (n && !data)) return {};
    // Indications get no answer: the id only has to differ from one to the next.
    uint8_t txid[12];
    std::memcpy(txid, d.indId, 8);
    const uint32_t c = d.indCount++;
    txid[8] = uint8_t(c >> 24);
    txid[9] = uint8_t(c >> 16);
    txid[10] = uint8_t(c >> 8);
    txid[11] = uint8_t(c);
    Builder b(kSend | kIndication, txid, kWrapOverhead + n + 4);
    b.addr4(aXorPeer, a.v4(), a.port());
    b.attr(aData, data, n);
    return std::move(b.b);
}

void Client::close(std::vector<std::vector<uint8_t>>& out) {
    Impl& d = *d_;
    for (const Req& r : d.reqs) d.remember(r.txid);
    d.reqs.clear();
    // Also after a failure: the server may still hold the allocation (it answers 437 if not).
    if (d.state != Impl::State::Idle && d.server.valid() && d.haveKey) {
        Req r;
        r.method = kRefresh;
        r.lifetime = 0;
        d.build(r);
        d.remember(r.txid);  // its answer is consumed, nothing waits for it
        out.push_back(std::move(r.bytes));
    }
    d.state = Impl::State::Idle;
    d.error.clear();
    d.haveKey = false;
    d.refreshing = false;
    d.perms.clear();
    d.relayed = d.mapped = sock::Addr();
}

// ---------------------------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------------------------
namespace {

bool hexIs(const uint8_t* p, size_t n, const char* want) { return onlinecrypto::hex(p, n) == want; }

// RFC 5769 section 2.1: sample request, short-term credential (password below), with FINGERPRINT.
const uint8_t kSampleRequest[] = {
    0x00, 0x01, 0x00, 0x58, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
    0x80, 0x22, 0x00, 0x10, 0x53, 0x54, 0x55, 0x4e, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74,
    0x00, 0x24, 0x00, 0x04, 0x6e, 0x00, 0x01, 0xff, 0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36,
    0x00, 0x06, 0x00, 0x09, 0x65, 0x76, 0x74, 0x6a, 0x3a, 0x68, 0x36, 0x76, 0x59, 0x20, 0x20, 0x20, 0x00, 0x08, 0x00, 0x14,
    0x9a, 0xea, 0xa7, 0x0c, 0xbf, 0xd8, 0xcb, 0x56, 0x78, 0x1e, 0xf2, 0xb5, 0xb2, 0xd3, 0xf2, 0x49, 0xc1, 0xb5, 0x71, 0xa2,
    0x80, 0x28, 0x00, 0x04, 0xe5, 0x7a, 0x3b, 0xcf};
// Section 2.2: sample IPv4 response (192.0.2.1:32853), same password.
const uint8_t kSampleResponse[] = {
    0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
    0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20, 0x00, 0x20, 0x00, 0x08,
    0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43, 0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3,
    0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};
const char kSamplePassword[] = "VOkJxbRl1RmTxUk/WvJxBt";
// Section 2.4: sample request with long-term authentication (user name in katakana, realm
// "example.org", password "TheMatrIX").
const uint8_t kSampleLongTerm[] = {
    0x00, 0x01, 0x00, 0x60, 0x21, 0x12, 0xa4, 0x42, 0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0, 0x29, 0xda, 0x41, 0x2e,
    0x00, 0x06, 0x00, 0x12, 0xe3, 0x83, 0x9e, 0xe3, 0x83, 0x88, 0xe3, 0x83, 0xaa, 0xe3, 0x83, 0x83, 0xe3, 0x82, 0xaf, 0xe3,
    0x82, 0xb9, 0x00, 0x00, 0x00, 0x15, 0x00, 0x1c, 0x66, 0x2f, 0x2f, 0x34, 0x39, 0x39, 0x6b, 0x39, 0x35, 0x34, 0x64, 0x36,
    0x4f, 0x4c, 0x33, 0x34, 0x6f, 0x4c, 0x39, 0x46, 0x53, 0x54, 0x76, 0x79, 0x36, 0x34, 0x73, 0x41, 0x00, 0x14, 0x00, 0x0b,
    0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x6f, 0x72, 0x67, 0x00, 0x00, 0x08, 0x00, 0x14, 0xf6, 0x70, 0x24, 0x65,
    0x6d, 0xd6, 0x4a, 0x3e, 0x02, 0xb8, 0xe0, 0x71, 0x2e, 0x85, 0xc9, 0xa2, 0x8c, 0xa8, 0x96, 0x66};

}  // namespace

bool selfTest(std::string* failure) {
    auto fail = [&](const char* what) {
        if (failure) *failure = std::string("online_turn self-test: ") + what;
        return false;
    };
    uint8_t d16[16], d20[20];

    // MD5 (RFC 1321 A.5).
    md5("", 0, d16);
    if (!hexIs(d16, 16, "d41d8cd98f00b204e9800998ecf8427e")) return fail("MD5 of the empty string");
    md5("abc", 3, d16);
    if (!hexIs(d16, 16, "900150983cd24fb0d6963f7d28e17f72")) return fail("MD5 abc");
    md5("message digest", 14, d16);
    if (!hexIs(d16, 16, "f96b697d7cb7938d525a2f31aaf161d0")) return fail("MD5 message digest");
    {
        const std::string s = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
        md5(s.data(), s.size(), d16);
        if (!hexIs(d16, 16, "57edf4a22be3c955ac49da2e2107b67a")) return fail("MD5 of 80 digits");
    }
    // RFC 5389 section 15.4's example key.
    longTermKey("user", "realm", "pass", d16);
    if (!hexIs(d16, 16, "8493fbc53ba582fb4c044c456bdc40eb")) return fail("long-term key");

    // SHA-1 (RFC 3174 section 7.3).
    sha1("", 0, d20);
    if (!hexIs(d20, 20, "da39a3ee5e6b4b0d3255bfef95601890afd80709")) return fail("SHA-1 of the empty string");
    sha1("abc", 3, d20);
    if (!hexIs(d20, 20, "a9993e364706816aba3e25717850c26c9cd0d89d")) return fail("SHA-1 abc");
    {
        const std::string s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha1(s.data(), s.size(), d20);
        if (!hexIs(d20, 20, "84983e441c3bd26ebaae4aa1f95129e5e54670f1")) return fail("SHA-1 of two blocks");
        const std::string a(1000, 'a');
        Sha1 h;
        for (int i = 0; i < 1000; i++) h.update(a.data(), a.size());
        h.final(d20);
        if (!hexIs(d20, 20, "34aa973cd4c4daa4f61eeb2bdbad27316534016f")) return fail("SHA-1 of a million a");
    }

    // HMAC-SHA1 (RFC 2202 cases 1, 2 and 6).
    {
        uint8_t k[80];
        std::memset(k, 0x0b, 20);
        hmacSha1(k, 20, "Hi There", 8, d20);
        if (!hexIs(d20, 20, "b617318655057264e28bc0b6fb378c8ef146be00")) return fail("HMAC-SHA1 case 1");
        hmacSha1("Jefe", 4, "what do ya want ", 16, "for nothing?", 12, d20);
        if (!hexIs(d20, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79")) return fail("HMAC-SHA1 case 2");
        std::memset(k, 0xaa, 80);
        const std::string s = "Test Using Larger Than Block-Size Key - Hash Key First";
        hmacSha1(k, 80, s.data(), s.size(), d20);
        if (!hexIs(d20, 20, "aa4ae5e15272d00e95705637ce8a3b55ed402112")) return fail("HMAC-SHA1 case 6");
    }

    // CRC-32 check value.
    if (crc32("123456789", 9) != 0xCBF43926u) return fail("CRC-32");

    // RFC 5769 samples: FINGERPRINT (checked by check()), MESSAGE-INTEGRITY, XOR-MAPPED-ADDRESS.
    View m;
    if (!check(kSampleRequest, sizeof kSampleRequest, &m)) return fail("RFC 5769 request: fingerprint");
    if (integrityState(m, kSamplePassword, sizeof kSamplePassword - 1) != 1) return fail("RFC 5769 request: message integrity");
    if (!check(kSampleResponse, sizeof kSampleResponse, &m)) return fail("RFC 5769 response: fingerprint");
    if (integrityState(m, kSamplePassword, sizeof kSamplePassword - 1) != 1) return fail("RFC 5769 response: message integrity");
    {
        size_t n = 0;
        const uint8_t* a = m.find(aXorMapped, &n);
        sock::Addr addr;
        if (!a || !parseAddr(a, n, m.txid(), &addr) || addr.v4() != 0xC0000201u || addr.port() != 32853) return fail("RFC 5769 response: mapped address");
    }
    {
        uint8_t bad[sizeof kSampleResponse];
        std::memcpy(bad, kSampleResponse, sizeof bad);
        bad[30] ^= 1;
        if (check(bad, sizeof bad, &m)) return fail("a damaged message passed the fingerprint");
    }
    if (!check(kSampleLongTerm, sizeof kSampleLongTerm, &m)) return fail("RFC 5769 long-term request");
    {
        const std::string user = "\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9";
        longTermKey(user, "example.org", "TheMatrIX", d16);
        if (integrityState(m, d16, 16) != 1) return fail("RFC 5769 long-term request: message integrity");
        d16[0] ^= 1;
        if (integrityState(m, d16, 16) != -1) return fail("a wrong key passed the message integrity");
        d16[0] ^= 1;

        // The builder writes the same bytes.
        Builder b(0x0001, kSampleLongTerm + 8);
        b.str(aUsername, user);
        b.str(aNonce, "f//499k954d6OL34oL9FSTvy64sA");
        b.str(aRealm, "example.org");
        b.integrity(d16, 16);
        if (b.b.size() != sizeof kSampleLongTerm || std::memcmp(b.b.data(), kSampleLongTerm, sizeof kSampleLongTerm) != 0)
            return fail("built message differs from the RFC 5769 long-term sample");
        b.fingerprint();
        if (!check(b.b.data(), b.b.size(), &m) || integrityState(m, d16, 16) != 1) return fail("built fingerprint");
    }
    return true;
}

}  // namespace online::turn
