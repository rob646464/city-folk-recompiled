// TURN client test (src/recomp/online_turn.*): the client on a real loopback UDP socket against
// the in-process test server (turn_standin.*), plus plain UDP sockets as peers. No game, no
// internet; about 25 s (refresh and time-out cases wait in real time).
#include "recomp/online_crypto.h"
#include "recomp/online_sock.h"
#include "recomp/online_turn.h"
#include "turn_standin.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace online;
using sock::Addr;
using Bytes = std::vector<uint8_t>;

static int sFails = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
            sFails++;                                          \
        }                                                      \
    } while (0)

static double nowS() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static bool same(const Addr& a, const Addr& b) {
    return a.valid() && b.valid() && a.normalized().v4() == b.normalized().v4() && a.port() == b.port();
}

static Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    for (auto& x : b) x = uint8_t((seed = seed * 1664525u + 1013904223u) >> 24);
    return b;
}

// A UDP socket on a loopback address.
struct Udp {
    sock::Socket s = sock::kInvalid;
    Addr local;
    ~Udp() { close(); }
    bool open(const char* ip) {
        in_addr ia{};
        if (inet_pton(AF_INET, ip, &ia) != 1) return false;
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == sock::kInvalid) return false;
        const Addr want = sock::makeV4(ntohl(ia.s_addr), 0);
        if (bind(s, reinterpret_cast<const sockaddr*>(&want.ss), want.len) != 0) {
            close();
            return false;
        }
        sock::setNonBlocking(s);
        socklen_t sl = sizeof(sockaddr_storage);
        getsockname(s, reinterpret_cast<sockaddr*>(&local.ss), &sl);
        local.len = sl;
        return true;
    }
    void close() {
        if (s != sock::kInvalid) sock::closeSocket(s);
        s = sock::kInvalid;
    }
    void send(const Addr& to, const Bytes& b) {
        sendto(s, reinterpret_cast<const char*>(b.data()), int(b.size()), 0, reinterpret_cast<const sockaddr*>(&to.ss), to.len);
    }
    bool recv(Addr* from, Bytes* d) {
        static std::vector<uint8_t> buf(65536);
        for (int tries = 0; tries < 8; tries++) {
            socklen_t sl = sizeof(sockaddr_storage);
            *from = Addr();
            const int n = int(recvfrom(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from->ss), &sl));
            if (n >= 0) {
                from->len = sl;
                d->assign(buf.begin(), buf.begin() + n);
                return true;
            }
#if defined(_WIN32)
            if (sock::lastError() == WSAECONNRESET) continue;  // ICMP answer to an earlier send; more may wait
#endif
            return false;
        }
        return false;
    }
};

// The client with its socket: what online_net.cpp will do with the game's socket.
struct Driver {
    Udp ep;
    turn::Client c;
    std::vector<std::pair<Addr, Bytes>> got;  // (peer, payload) from Data indications
    std::vector<Bytes> raw;                   // datagrams from the server as received
    int notConsumed = 0;                      // datagrams from the server the client did not take

    void pump() {
        std::vector<Bytes> out;
        c.tick(nowS(), out);
        Addr from;
        Bytes d;
        while (ep.s != sock::kInvalid && ep.recv(&from, &d)) {
            if (!same(from, c.server())) continue;
            Addr peer;
            Bytes data;
            if (c.onDatagram(d.data(), d.size(), nowS(), &peer, &data, out)) {
                if (!data.empty()) got.push_back({peer, data});
            } else {
                notConsumed++;
            }
            raw.push_back(d);
        }
        for (const Bytes& o : out) ep.send(c.server(), o);
    }
};

static std::vector<Driver*> sActive;

static bool waitFor(const std::function<bool()>& f, double seconds) {
    const double t0 = nowS();
    for (;;) {
        for (Driver* d : sActive) d->pump();
        if (f()) return true;
        if (nowS() - t0 >= seconds) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
static void idle(double seconds) {
    waitFor([] { return false; }, seconds);
}

// What `peer` receives within the time: true and the datagram, or false.
static bool peerGets(Udp& peer, Addr* from, Bytes* d, double seconds) {
    return waitFor([&] { return peer.recv(from, d); }, seconds);
}

static void deactivate(Driver* d) {
    for (size_t i = 0; i < sActive.size(); i++)
        if (sActive[i] == d) sActive.erase(sActive.begin() + ptrdiff_t(i));
}

// ---------------------------------------------------------------------------------------------
// A scripted server without sockets: hand-built answers and a made-up clock. Checks the requests
// the client writes (credentials; MESSAGE-INTEGRITY computed here on its own) and the paths the
// test server never takes (forged answers, refused permissions, errors in the middle of a
// session).
// ---------------------------------------------------------------------------------------------
struct Msg {
    Bytes b;
    Msg(uint16_t type, const uint8_t* txid) {
        b = {uint8_t(type >> 8), uint8_t(type), 0, 0, 0x21, 0x12, 0xA4, 0x42};
        b.insert(b.end(), txid, txid + 12);
    }
    Msg& attr(uint16_t t, const void* v, size_t n) {
        const uint8_t h[4] = {uint8_t(t >> 8), uint8_t(t), uint8_t(n >> 8), uint8_t(n)};
        b.insert(b.end(), h, h + 4);
        b.insert(b.end(), static_cast<const uint8_t*>(v), static_cast<const uint8_t*>(v) + n);
        while (b.size() & 3) b.push_back(0);
        b[2] = uint8_t((b.size() - 20) >> 8);
        b[3] = uint8_t(b.size() - 20);
        return *this;
    }
    Msg& str(uint16_t t, const char* s) { return attr(t, s, std::strlen(s)); }
    Msg& u32(uint16_t t, uint32_t v) {
        const uint8_t x[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        return attr(t, x, 4);
    }
    Msg& addr(uint16_t t, uint32_t ip, uint16_t port) {
        ip ^= 0x2112A442u;
        port ^= 0x2112;
        const uint8_t x[8] = {0, 1, uint8_t(port >> 8), uint8_t(port), uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip)};
        return attr(t, x, 8);
    }
    Msg& err(int code) {
        const uint8_t x[8] = {0, 0, uint8_t(code / 100), uint8_t(code % 100), 'o', 'o', 'p', 's'};
        return attr(0x0009, x, 8);
    }
    Msg& sign(const uint8_t key[16], bool wrong = false) {
        const size_t off = b.size();
        const uint8_t zero[20] = {};
        attr(0x0008, zero, 20);  // the length field now counts it, as the HMAC wants
        uint8_t mac[20];
        turn::hmacSha1(key, 16, b.data(), off, mac);
        if (wrong) mac[7] ^= 0x10;
        std::memcpy(b.data() + off + 4, mac, 20);
        return *this;
    }
};

static const uint8_t* findAttr(const Bytes& m, uint16_t type, size_t* len, size_t* off = nullptr) {
    size_t i = 20;
    while (i + 4 <= m.size()) {
        const uint16_t t = uint16_t(m[i] << 8 | m[i + 1]);
        const size_t n = size_t(m[i + 2] << 8 | m[i + 3]);
        if (i + 4 + n > m.size()) return nullptr;
        if (t == type) {
            *len = n;
            if (off) *off = i;
            return m.data() + i + 4;
        }
        i += 4 + ((n + 3) & ~size_t(3));
    }
    return nullptr;
}
static std::string attrText(const Bytes& m, uint16_t type) {
    size_t n = 0;
    const uint8_t* v = findAttr(m, type, &n);
    return v ? std::string(reinterpret_cast<const char*>(v), n) : std::string("<none>");
}
// A request of this type with these credentials, its MESSAGE-INTEGRITY right for the password.
static bool signedRequest(const Bytes& m, uint16_t type, const char* user, const char* realm, const char* nonce, const char* pass) {
    if (m.size() < 20 || (m[0] << 8 | m[1]) != type || size_t(m[2] << 8 | m[3]) + 20 != m.size()) return false;
    if (attrText(m, 0x0006) != user || attrText(m, 0x0014) != realm || attrText(m, 0x0015) != nonce) return false;
    size_t n = 0, off = 0;
    const uint8_t* mi = findAttr(m, 0x0008, &n, &off);
    if (!mi || n != 20 || off + 24 != m.size()) return false;  // the last attribute
    uint8_t key[16], mac[20];
    turn::longTermKey(user, realm, pass, key);
    turn::hmacSha1(key, 16, m.data(), off, mac);
    return std::memcmp(mac, mi, 20) == 0;
}

static void scripted() {
    const Addr S = sock::makeV4(0xC6336401, 3478);     // 198.51.100.1
    const Addr peer = sock::makeV4(0x0A000002, 7000);  // 10.0.0.2
    uint8_t key[16];
    turn::longTermKey("u", "r", "p", key);
    std::vector<Bytes> out;
    Addr from;
    Bytes data;
    double t = 50;
    auto feed = [&](turn::Client& c, const Bytes& m) { return c.onDatagram(m.data(), m.size(), t, &from, &data, out); };
    auto challenge = [](const Bytes& req) { return Msg(0x0113, req.data() + 8).err(401).str(0x0014, "r").str(0x0015, "n1").b; };

    turn::Client c;
    c.permit(peer);
    c.start(S, {"u", "p"});
    c.tick(t, out);
    CHECK(out.size() == 1 && out[0].size() == 36, "first Allocate");
    if (out.size() != 1) return;
    Bytes req = out[0];
    out.clear();
    CHECK(feed(c, challenge(req)), "401 taken");
    CHECK(out.size() == 1 && signedRequest(out[0], 0x0003, "u", "r", "n1", "p"), "Allocate with credentials");
    if (out.size() != 1) return;
    size_t n = 0;
    CHECK(findAttr(out[0], 0x0019, &n) && findAttr(out[0], 0x000D, &n), "REQUESTED-TRANSPORT and LIFETIME in the second Allocate");
    req = out[0];
    out.clear();

    // A forged success (wrong MESSAGE-INTEGRITY) is dropped; the real one is taken afterwards.
    auto okAlloc = [&](bool wrong) {
        return Msg(0x0103, req.data() + 8).addr(0x0016, 0xC6336407, 40000).u32(0x000D, 600).addr(0x0020, 0xCB007109, 1234).sign(key, wrong).b;
    };
    const Bytes forged = okAlloc(true), real = okAlloc(false);
    CHECK(feed(c, forged) && !c.ready() && !c.failed() && out.empty(), "forged Allocate success ignored");
    CHECK(feed(c, real) && c.ready(), "Allocate success");
    CHECK(c.relayed().v4() == 0xC6336407 && c.relayed().port() == 40000 && c.mapped().v4() == 0xCB007109 && c.mapped().port() == 1234,
          "relayed %s mapped %s", c.relayed().text().c_str(), c.mapped().text().c_str());
    CHECK(feed(c, real) && out.size() == 1, "a second copy of the answer is taken and changes nothing");

    // The permission asked for before start() goes out with the answer; 403 leaves the allocation alone.
    CHECK(out.size() == 1 && signedRequest(out[0], 0x0008, "u", "r", "n1", "p"), "CreatePermission");
    if (out.size() != 1) return;
    const uint8_t wantPeer[8] = {0, 1, (7000 ^ 0x2112) >> 8, (7000 ^ 0x2112) & 0xFF, 0x0A ^ 0x21, 0x00 ^ 0x12, 0x00 ^ 0xA4, 0x02 ^ 0x42};
    const uint8_t* xp = findAttr(out[0], 0x0012, &n);
    CHECK(xp && n == 8 && std::memcmp(xp, wantPeer, 8) == 0, "XOR-PEER-ADDRESS");
    req = out[0];
    out.clear();
    CHECK(feed(c, Msg(0x0118, req.data() + 8).err(403).b) && c.ready() && !c.permitted(peer) && out.empty(), "refused permission");
    t += 29;
    c.tick(t, out);
    CHECK(out.empty(), "no retry of a refused permission before 30 s");
    t += 2;
    c.tick(t, out);
    CHECK(out.size() == 1 && signedRequest(out[0], 0x0008, "u", "r", "n1", "p"), "permission asked again after 30 s");
    if (out.size() != 1) return;
    req = out[0];
    out.clear();
    CHECK(feed(c, Msg(0x0108, req.data() + 8).sign(key).b) && c.permitted(peer), "permission");

    // Data from the peer, IPv4 and IPv6; wrap() the other way.
    const uint8_t id[12] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2};
    CHECK(feed(c, Msg(0x0017, id).addr(0x0012, 0x0A000002, 7000).attr(0x0013, "abcde", 5).b) && same(from, peer) &&
              data == Bytes({'a', 'b', 'c', 'd', 'e'}),
          "Data indication");
    uint8_t x6[20] = {0, 2, (7001 ^ 0x2112) >> 8, (7001 ^ 0x2112) & 0xFF, 0x20 ^ 0x21, 0x01 ^ 0x12, 0x0d ^ 0xA4, 0xb8 ^ 0x42};
    for (int i = 0; i < 12; i++) x6[8 + i] = id[i];
    x6[19] ^= 1;  // 2001:db8::1
    CHECK(feed(c, Msg(0x0017, id).attr(0x0012, x6, 20).attr(0x0013, "v6", 2).b) && from.family() == AF_INET6 && from.port() == 7001 &&
              from.ipText() == "2001:db8::1" && data.size() == 2,
          "Data indication from an IPv6 peer: %s", from.text().c_str());
    CHECK(feed(c, Msg(0x0017, id).addr(0x0012, 0x0A000002, 7000).attr(0x0013, "", 0).b) && data.empty(), "empty Data indication");
    const uint8_t xyz[3] = {'x', 'y', 'z'};
    const Bytes w = c.wrap(peer, xyz, 3);
    const uint8_t wantHead[8] = {0x00, 0x16, 0x00, 0x14, 0x21, 0x12, 0xA4, 0x42};
    const uint8_t wantPeerHead[4] = {0x00, 0x12, 0x00, 0x08};
    const uint8_t wantData[8] = {0x00, 0x13, 0x00, 0x03, 'x', 'y', 'z', 0};
    CHECK(w.size() == 40 && std::memcmp(w.data(), wantHead, 8) == 0 && std::memcmp(w.data() + 20, wantPeerHead, 4) == 0 &&
              std::memcmp(w.data() + 24, wantPeer, 8) == 0 && std::memcmp(w.data() + 32, wantData, 8) == 0,
          "Send indication bytes");
    const Bytes w2 = c.wrap(peer, xyz, 3);
    CHECK(w2.size() == 40 && std::memcmp(w.data() + 8, w2.data() + 8, 12) != 0, "indications have different transaction ids");
    CHECK(c.wrap(peer, nullptr, 0).size() == 36 && c.wrap(peer, w.data(), turn::kMaxPayload + 1).empty(), "wrap() sizes");

    // The permission is refreshed after 4 minutes, the allocation at half its lifetime.
    out.clear();
    t = 50 + 31 + 239;
    c.tick(t, out);
    CHECK(out.empty(), "nothing due yet (%zu)", out.size());
    t += 2;
    c.tick(t, out);
    CHECK(out.size() == 1 && signedRequest(out[0], 0x0008, "u", "r", "n1", "p"), "permission refresh after 4 minutes");
    if (out.size() != 1) return;
    req = out[0];
    out.clear();
    CHECK(feed(c, Msg(0x0108, req.data() + 8).sign(key).b) && out.empty(), "permission refreshed");
    t = 50 + 299;
    c.tick(t, out);
    CHECK(out.empty(), "no Refresh before half the lifetime");
    t += 2;
    c.tick(t, out);
    CHECK(out.size() == 1 && signedRequest(out[0], 0x0004, "u", "r", "n1", "p"), "Refresh at half the lifetime");
    if (out.size() != 1) return;
    const uint8_t* lt = findAttr(out[0], 0x000D, &n);
    CHECK(lt && n == 4 && lt[0] == 0 && lt[1] == 0 && lt[2] == 0x02 && lt[3] == 0x58, "Refresh asks for 600 s");
    // 438 with a new nonce: the same request again, new transaction, new nonce.
    req = out[0];
    out.clear();
    CHECK(feed(c, Msg(0x0114, req.data() + 8).err(438).str(0x0014, "r").str(0x0015, "n2").b) && c.ready(), "438 taken");
    CHECK(out.size() == 1 && signedRequest(out[0], 0x0004, "u", "r", "n2", "p") && std::memcmp(out[0].data() + 8, req.data() + 8, 12) != 0,
          "Refresh with the new nonce");
    if (out.size() != 1) return;
    req = out[0];
    out.clear();
    CHECK(feed(c, Msg(0x0104, req.data() + 8).u32(0x000D, 100).sign(key).b) && c.ready(), "Refresh success");
    t += 49;
    c.tick(t, out);
    CHECK(out.empty(), "next Refresh not before half of the new lifetime");
    t += 2;
    c.tick(t, out);
    CHECK(out.size() == 1 && signedRequest(out[0], 0x0004, "u", "r", "n2", "p"), "next Refresh after 50 s");
    if (out.size() != 1) return;
    // The server no longer accepts the credentials (they ran out): failure with a clear text.
    req = out[0];
    out.clear();
    CHECK(feed(c, Msg(0x0114, req.data() + 8).err(401).str(0x0014, "r").str(0x0015, "n3").b) && c.failed() && !c.ready(), "401 in the session");
    CHECK(c.error().find("user name or password") != std::string::npos, "error text: %s", c.error().c_str());
    CHECK(!c.permitted(peer) && c.wrap(peer, xyz, 3).empty(), "nothing relayed after a failure");
    // close() after a failure still tells the server (it may hold the allocation).
    c.close(out);
    CHECK(out.size() == 1 && signedRequest(out[0], 0x0004, "u", "r", "n2", "p") && !c.started(), "close() after a failure");
    if (out.size() == 1) {
        lt = findAttr(out[0], 0x000D, &n);
        CHECK(lt && n == 4 && (lt[0] | lt[1] | lt[2] | lt[3]) == 0, "close() sends lifetime 0");
        CHECK(feed(c, Msg(0x0104, out[0].data() + 8).u32(0x000D, 0).sign(key).b) && !c.started(), "the answer to close() is taken");
    }

    // An allocation whose Refresh gets no answer: failure when the lifetime is over.
    {
        turn::Client e;
        out.clear();
        t = 1000;
        e.start(S, {"u", "p"});
        e.tick(t, out);
        if (out.size() != 1) return;
        req = out[0];
        out.clear();
        feed(e, challenge(req));
        if (out.size() != 1) return;
        req = out[0];
        // No MESSAGE-INTEGRITY in the answer: accepted (servers may leave it out).
        CHECK(feed(e, Msg(0x0103, req.data() + 8).addr(0x0016, 0xC6336407, 40001).u32(0x000D, 10).b) && e.ready(), "Allocate success without integrity");
        CHECK(!e.mapped().valid(), "no XOR-MAPPED-ADDRESS in the answer");
        int sent = 0;
        double failedAt = -1;
        for (int i = 0; i < 2000 && failedAt < 0; i++) {
            t = 1000 + i * 0.02;
            out.clear();
            e.tick(t, out);
            sent += int(out.size());
            if (e.failed()) failedAt = t - 1000;
        }
        CHECK(sent == 4 && failedAt > 9.9 && failedAt < 13, "unanswered Refresh: %d sent, failed at %.2f s: %s", sent, failedAt, e.error().c_str());
    }

    // 437 Allocation Mismatch: one release + second Allocate, then an error text.
    {
        turn::Client m;
        out.clear();
        t = 2000;
        m.start(S, {"u", "p"});
        m.tick(t, out);
        if (out.size() != 1) return;
        req = out[0];
        out.clear();
        feed(m, challenge(req));
        if (out.size() != 1) return;
        req = out[0];
        out.clear();
        CHECK(feed(m, Msg(0x0113, req.data() + 8).err(437).b) && !m.failed(), "first 437");
        CHECK(out.size() == 1 && signedRequest(out[0], 0x0004, "u", "r", "n1", "p"), "Refresh that releases the old allocation");
        if (out.size() != 1) return;
        req = out[0];
        out.clear();
        CHECK(feed(m, Msg(0x0104, req.data() + 8).u32(0x000D, 0).sign(key).b) && !m.failed() && !m.ready(), "release answered");
        CHECK(out.size() == 1 && signedRequest(out[0], 0x0003, "u", "r", "n1", "p"), "second Allocate");
        if (out.size() != 1) return;
        req = out[0];
        out.clear();
        CHECK(feed(m, Msg(0x0113, req.data() + 8).err(437).b) && m.failed() && out.empty(), "second 437 is final");
        CHECK(m.error().find("older session") != std::string::npos, "error text: %s", m.error().c_str());
        std::printf("allocation mismatch twice: \"%s\"\n", m.error().c_str());
    }

    // Other errors come out in words, unknown ones with their number.
    {
        turn::Client q;
        out.clear();
        t = 3000;
        q.start(S, {"u", "p"});
        q.tick(t, out);
        if (out.size() != 1) return;
        req = out[0];
        CHECK(feed(q, Msg(0x0113, req.data() + 8).err(486).b) && q.failed() && q.error().find("limit") != std::string::npos, "486: %s", q.error().c_str());
        out.clear();
        q.start(S, {"u", "p"});
        q.tick(t, out);
        if (out.size() != 1) return;
        req = out[0];
        CHECK(feed(q, Msg(0x0113, req.data() + 8).err(500).b) && q.failed() && q.error().find("500 (oops)") != std::string::npos, "500: %s",
              q.error().c_str());
    }
    std::printf("scripted answers (credentials, forged answer, refused permission, refresh times, stale nonce, errors): done\n");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    sock::startup();
    std::string why;
    CHECK(turn::selfTest(&why), "%s", why.c_str());
    std::printf("self-test (MD5, SHA-1, HMAC-SHA1, CRC-32, RFC 5769 samples): %s\n", why.empty() ? "ok" : why.c_str());

    // ---- Timing without any network: 4 transmissions (0, 0.5, 1.5, 3.5 s), given up at 7.5 s.
    {
        turn::Client c;
        std::vector<Bytes> out;
        const uint8_t junk[64] = {};
        CHECK(!c.started() && !c.ready() && !c.failed(), "a new client is idle");
        CHECK(!c.onDatagram(junk, sizeof junk, 0, nullptr, nullptr, out), "idle client takes nothing");
        c.tick(0, out);
        CHECK(out.empty(), "idle client sends nothing");
        c.start(sock::makeV4(0x7F000001, 9), {"user", "pw"});
        CHECK(c.started() && out.empty(), "start() sends nothing itself");
        std::vector<double> times;
        Bytes first;
        double failedAt = -1;
        for (int i = 0; i <= 1000 && failedAt < 0; i++) {
            const double t = 100.0 + i * 0.01;
            out.clear();
            c.tick(t, out);
            for (const Bytes& o : out) {
                times.push_back(t - 100.0);
                if (first.empty()) first = o;
                CHECK(o == first, "a retransmission is the same datagram");
            }
            if (c.failed()) failedAt = t - 100.0;
        }
        auto about = [](double a, double b) { return a > b - 0.005 && a < b + 0.03; };
        CHECK(times.size() == 4 && about(times[0], 0) && about(times[1], 0.5) && about(times[2], 1.5) && about(times[3], 3.5),
              "%zu transmissions", times.size());
        CHECK(failedAt > 7.45 && failedAt < 7.6, "gave up at %.2f s", failedAt);
        CHECK(c.error().find("No answer") != std::string::npos, "error text: %s", c.error().c_str());
        // Allocate request: header + REQUESTED-TRANSPORT (UDP) + LIFETIME 600, no credentials yet.
        const uint8_t want[] = {0x00, 0x03, 0x00, 0x10, 0x21, 0x12, 0xA4, 0x42};
        const uint8_t attrs[] = {0x00, 0x19, 0x00, 0x04, 17, 0, 0, 0, 0x00, 0x0D, 0x00, 0x04, 0, 0, 0x02, 0x58};
        CHECK(first.size() == 36 && std::memcmp(first.data(), want, 8) == 0 && std::memcmp(first.data() + 20, attrs, 16) == 0, "first Allocate");
        CHECK(c.wrap(sock::makeV4(0x7F000001, 5), junk, 8).empty(), "wrap() before ready");
        CHECK(!c.ready() && c.started(), "failed client");
        c.start(sock::makeV4(0x7F000001, 9), {"user", "pw"});
        CHECK(!c.failed() && c.started() && c.error().empty(), "start() after a failure");
        out.clear();
        c.close(out);
        CHECK(out.empty() && !c.started(), "close() before any credentials: nothing to release");
        std::printf("timing: transmissions at %.2f %.2f %.2f %.2f s, failed at %.2f s\n", times.size() > 0 ? times[0] : -1,
                    times.size() > 1 ? times[1] : -1, times.size() > 2 ? times[2] : -1, times.size() > 3 ? times[3] : -1, failedAt);
    }

    scripted();

    // ---- The test server. 127.0.0.9 where the system has all of 127/8, else 127.0.0.1.
    TurnTestServer srv;
    std::string err;
    if (!srv.start("127.0.0.9", 0, "player", "s3cret", &err) && !srv.start("127.0.0.1", 0, "player", "s3cret", &err)) {
        std::printf("FAIL: test server: %s\n", err.c_str());
        return 1;
    }
    const Addr server = srv.addr();
    const turn::Creds creds{"player", "s3cret"};
    std::printf("test server on %s\n", server.text().c_str());

    // ---- Allocation with the right password.
    Driver a;
    CHECK(a.ep.open("127.0.0.1"), "client socket");
    sActive.push_back(&a);
    a.c.start(server, creds);
    CHECK(waitFor([&] { return a.c.ready() || a.c.failed(); }, 3) && a.c.ready(), "allocation: %s", a.c.error().c_str());
    CHECK(a.c.relayed().v4() == server.v4() && a.c.relayed().port() != 0 && a.c.relayed().port() != server.port(), "relayed %s",
          a.c.relayed().text().c_str());
    CHECK(same(a.c.mapped(), a.ep.local), "mapped %s, socket %s", a.c.mapped().text().c_str(), a.ep.local.text().c_str());
    CHECK(srv.allocations() == 1, "%d allocations", srv.allocations());
    std::printf("allocation: relayed %s, mapped %s\n", a.c.relayed().text().c_str(), a.c.mapped().text().c_str());

    // ---- A peer: dropped without a permission, relayed with one, in both directions.
    Udp p1;
    CHECK(p1.open("127.0.0.1"), "peer socket");
    const Bytes hello = {'h', 'e', 'l', 'l', 'o'};
    const Bytes big1 = pattern(1200, 1), big2 = pattern(1200, 2);
    Addr from;
    Bytes d;
    {
        p1.send(a.c.relayed(), hello);
        idle(0.3);
        CHECK(a.got.empty() && srv.droppedDatagrams() == 1, "peer data without a permission: %zu delivered, %llu dropped", a.got.size(),
              static_cast<unsigned long long>(srv.droppedDatagrams()));
        a.ep.send(server, a.c.wrap(p1.local, hello.data(), hello.size()));
        CHECK(!peerGets(p1, &from, &d, 0.3) && srv.droppedDatagrams() == 2, "Send indication without a permission");
        CHECK(!a.c.permitted(p1.local), "permitted() before permit()");

        const uint64_t reqs = srv.requests();
        a.c.permit(p1.local);
        a.c.permit(p1.local);
        CHECK(waitFor([&] { return a.c.permitted(p1.local); }, 2), "permission");
        a.c.permit(p1.local);
        p1.send(a.c.relayed(), hello);
        CHECK(waitFor([&] { return a.got.size() == 1; }, 2), "peer -> client");
        CHECK(a.got.size() == 1 && same(a.got[0].first, p1.local) && a.got[0].second == hello, "peer -> client content");

        const Bytes w = a.c.wrap(p1.local, hello.data(), hello.size());
        CHECK(w.size() == turn::kWrapOverhead + 8, "Send indication of 5 bytes: %zu", w.size());
        a.ep.send(server, w);
        CHECK(peerGets(p1, &from, &d, 2) && same(from, a.c.relayed()) && d == hello, "client -> peer (from %s)", from.text().c_str());

        // 1200-byte payloads.
        p1.send(a.c.relayed(), big1);
        CHECK(waitFor([&] { return a.got.size() == 2; }, 2) && a.got.size() == 2 && a.got[1].second == big1 && same(a.got[1].first, p1.local),
              "1200 bytes peer -> client");
        const Bytes wb = a.c.wrap(p1.local, big2.data(), big2.size());
        CHECK(wb.size() == turn::kWrapOverhead + 1200, "Send indication of 1200 bytes: %zu", wb.size());
        a.ep.send(server, wb);
        CHECK(peerGets(p1, &from, &d, 2) && same(from, a.c.relayed()) && d == big2, "1200 bytes client -> peer");
        CHECK(srv.relayedBytes() == 5 + 5 + 1200 + 1200, "relayed bytes %llu", static_cast<unsigned long long>(srv.relayedBytes()));
        CHECK(srv.requests() == reqs + 1, "permit() three times asked once (%llu requests)", static_cast<unsigned long long>(srv.requests() - reqs));

        // The permission is for the IP address: another port of it passes, another address not.
        Udp p2, p3;
        CHECK(p2.open("127.0.0.1"), "second peer");
        p2.send(a.c.relayed(), hello);
        CHECK(waitFor([&] { return a.got.size() == 3; }, 2) && a.got.size() == 3 && same(a.got[2].first, p2.local), "same IP, other port");
        if (p3.open("127.0.0.3")) {
            const uint64_t dropped = srv.droppedDatagrams();
            p3.send(a.c.relayed(), hello);
            idle(0.3);
            CHECK(a.got.size() == 3 && srv.droppedDatagrams() == dropped + 1, "peer on another address without a permission");
            // A new nonce on the server in between: 438 Stale Nonce, then the same request again.
            srv.rotateNonce();
            a.c.permit(p3.local);
            CHECK(waitFor([&] { return a.c.permitted(p3.local); }, 2) && a.c.ready(), "permission after a stale nonce: %s", a.c.error().c_str());
            p3.send(a.c.relayed(), big1);
            CHECK(waitFor([&] { return a.got.size() == 4; }, 2) && a.got.size() == 4 && same(a.got[3].first, p3.local) && a.got[3].second == big1,
                  "peer on the second address");
            a.ep.send(server, a.c.wrap(p3.local, big2.data(), big2.size()));
            CHECK(peerGets(p3, &from, &d, 2) && same(from, a.c.relayed()) && d == big2, "client -> second address");
        } else {
            std::printf("(no 127.0.0.3 on this system: second-address checks skipped)\n");
            srv.rotateNonce();
            const Addr other = sock::makeV4(0x0A010203, 7);
            a.c.permit(other);
            CHECK(waitFor([&] { return a.c.permitted(other); }, 2) && a.c.ready(), "permission after a stale nonce: %s", a.c.error().c_str());
        }
        // IPv6 peers are not relayed.
        const uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        a.c.permit(sock::makeV6(v6, 5));
        CHECK(a.c.wrap(sock::makeV6(v6, 5), hello.data(), hello.size()).empty() && !a.c.permitted(sock::makeV6(v6, 5)), "IPv6 peer");
        std::printf("relay: %llu payload bytes through the server, %llu datagrams refused\n", static_cast<unsigned long long>(srv.relayedBytes()),
                    static_cast<unsigned long long>(srv.droppedDatagrams()));
    }

    // ---- Garbage, truncated and damaged datagrams: never a crash, never taken.
    {
        std::vector<Bytes> out;
        Addr peer;
        Bytes data;
        const size_t gotBefore = a.got.size();
        CHECK(!a.c.onDatagram(nullptr, 0, nowS(), &peer, &data, out), "empty datagram");
        uint32_t seed = 12345;
        auto rnd = [&] { return uint8_t((seed = seed * 1664525u + 1013904223u) >> 24); };
        int taken = 0;
        for (int i = 0; i < 4000; i++) {
            Bytes g(size_t(i % 300));
            for (auto& x : g) x = rnd();
            if ((i & 1) && g.size() >= 20) {  // looks like STUN: right cookie and length, random rest
                g[0] &= 0x3F;
                g[2] = uint8_t((g.size() - 20) >> 8);
                g[3] = uint8_t(g.size() - 20);
                g[4] = 0x21, g[5] = 0x12, g[6] = 0xA4, g[7] = 0x42;
                if (i & 2) g[0] = 0x00, g[1] = 0x17;        // Data indication with random attributes
                if ((i & 6) == 4) g[0] = 0x01, g[1] = 0x03;  // Allocate success of nobody's transaction
            }
            taken += a.c.onDatagram(g.data(), g.size(), nowS(), &peer, &data, out) ? 1 : 0;
            taken += a.c.onDatagram(g.data(), g.size(), nowS(), nullptr, nullptr, out) ? 1 : 0;
        }
        CHECK(taken == 0, "%d random datagrams taken", taken);
        CHECK(a.raw.size() >= 6, "%zu datagrams from the server recorded", a.raw.size());
        int prefixTaken = 0, longerTaken = 0;
        for (const Bytes& r : a.raw) {
            for (size_t n = 0; n < r.size(); n++) {
                const Bytes pre(r.begin(), r.begin() + ptrdiff_t(n));  // own heap block: reading past n shows under a sanitizer
                prefixTaken += a.c.onDatagram(pre.data(), pre.size(), nowS(), &peer, &data, out) ? 1 : 0;
            }
            Bytes longer = r;
            longer.push_back(0);
            longerTaken += a.c.onDatagram(longer.data(), longer.size(), nowS(), &peer, &data, out) ? 1 : 0;
            longer.resize(r.size() + 4);
            longerTaken += a.c.onDatagram(longer.data(), longer.size(), nowS(), &peer, &data, out) ? 1 : 0;
            // Damaged copies: any answer is fine, crashing or reading past the end is not.
            for (int k = 0; k < 400; k++) {
                Bytes bad = r;
                bad[(size_t(rnd()) << 8 | rnd()) % bad.size()] ^= uint8_t(1u << (rnd() & 7));
                if (k & 1) bad[2 + (rnd() & 1)] = rnd();  // length field
                Bytes exact(bad.begin(), bad.end());      // own heap block: an overrun shows under a sanitizer
                a.c.onDatagram(exact.data(), exact.size(), nowS(), &peer, &data, out);
            }
        }
        CHECK(prefixTaken == 0 && longerTaken == 0, "%d truncated, %d lengthened datagrams taken", prefixTaken, longerTaken);
        // Hand-made bad ones: a Data indication whose XOR-PEER-ADDRESS is too short, one without
        // DATA, one whose attribute runs past the end, and a success answer nobody asked for.
        const uint8_t shortPeer[] = {0x00, 0x17, 0x00, 0x10, 0x21, 0x12, 0xA4, 0x42, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                     0x00, 0x12, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x13, 0x00, 0x04, 'd', 'a', 't', 'a'};
        const uint8_t noData[] = {0x00, 0x17, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                  0x00, 0x12, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x5E, 0x12, 0xA4, 0x43};
        const uint8_t overrun[] = {0x00, 0x17, 0x00, 0x10, 0x21, 0x12, 0xA4, 0x42, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                   0x00, 0x12, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x5E, 0x12, 0xA4, 0x43, 0x00, 0x13, 0xFF, 0xFF};
        const uint8_t stranger[] = {0x01, 0x03, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        const uint8_t zeroId[] = {0x01, 0x03, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(!a.c.onDatagram(shortPeer, sizeof shortPeer, nowS(), &peer, &data, out), "short XOR-PEER-ADDRESS");
        CHECK(!a.c.onDatagram(noData, sizeof noData, nowS(), &peer, &data, out), "Data indication without DATA");
        CHECK(!a.c.onDatagram(overrun, sizeof overrun, nowS(), &peer, &data, out), "attribute past the end");
        CHECK(!a.c.onDatagram(stranger, sizeof stranger, nowS(), &peer, &data, out), "answer to an unknown transaction");
        CHECK(!a.c.onDatagram(zeroId, sizeof zeroId, nowS(), &peer, &data, out), "answer with a zero transaction id");
        CHECK(a.c.ready() && !a.c.failed(), "still ready after the garbage: %s", a.c.error().c_str());
        CHECK(a.got.size() == gotBefore, "garbage did not reach the game");
        p1.send(a.c.relayed(), hello);
        CHECK(waitFor([&] { return a.got.size() == gotBefore + 1; }, 2), "relay still works after the garbage");
        std::printf("garbage: 8000 random, every prefix of %zu server datagrams, %zu damaged copies: none taken, no crash\n", a.raw.size(),
                    a.raw.size() * 400);
    }

    // ---- permit() before start(): installed as soon as the allocation exists.
    Driver b;
    CHECK(b.ep.open("127.0.0.1"), "client socket");
    b.c.permit(p1.local);
    CHECK(!b.c.permitted(p1.local) && b.c.wrap(p1.local, hello.data(), hello.size()).empty(), "nothing before start()");
    sActive.push_back(&b);
    b.c.start(server, creds);
    CHECK(waitFor([&] { return b.c.permitted(p1.local); }, 3), "early permit(): %s", b.c.error().c_str());
    p1.send(b.c.relayed(), big1);
    CHECK(waitFor([&] { return b.got.size() == 1; }, 2) && b.got.size() == 1 && b.got[0].second == big1, "data after an early permit()");
    CHECK(srv.allocations() == 2 && !same(a.c.relayed(), b.c.relayed()), "two allocations");

    // ---- Lost requests are sent again (0.5 s, then 1 s later).
    Driver c;
    CHECK(c.ep.open("127.0.0.1"), "client socket");
    sActive.push_back(&c);
    {
        srv.dropRequests(2);
        const double t0 = nowS();
        c.c.start(server, creds);
        CHECK(waitFor([&] { return c.c.ready() || c.c.failed(); }, 6) && c.c.ready(), "allocation after two lost requests: %s", c.c.error().c_str());
        const double dt = nowS() - t0;
        CHECK(dt > 1.4 && dt < 3.0, "took %.2f s", dt);
        std::printf("retransmission: ready %.2f s after two lost requests\n", dt);
    }

    // ---- Wrong password / wrong user name: a clear error, no allocation.
    {
        Driver w, u;
        CHECK(w.ep.open("127.0.0.1") && u.ep.open("127.0.0.1"), "client sockets");
        sActive.push_back(&w);
        sActive.push_back(&u);
        w.c.start(server, {"player", "wrong"});
        u.c.start(server, {"nobody", "s3cret"});
        CHECK(waitFor([&] { return (w.c.failed() || w.c.ready()) && (u.c.failed() || u.c.ready()); }, 3), "an answer to wrong credentials");
        CHECK(w.c.failed() && !w.c.ready() && w.c.error().find("password") != std::string::npos, "wrong password: '%s'", w.c.error().c_str());
        CHECK(u.c.failed() && u.c.error().find("user name") != std::string::npos, "wrong user: '%s'", u.c.error().c_str());
        CHECK(w.c.wrap(p1.local, hello.data(), 5).empty() && !w.c.relayed().valid(), "no relay after a failure");
        CHECK(srv.allocations() == 3, "%d allocations", srv.allocations());
        CHECK(w.notConsumed == 0 && u.notConsumed == 0, "answers taken");
        std::printf("wrong password: \"%s\"\n", w.c.error().c_str());
        // The same client with the right password afterwards.
        w.c.start(server, creds);
        CHECK(waitFor([&] { return w.c.ready() || w.c.failed(); }, 3) && w.c.ready(), "right password after a wrong one: %s", w.c.error().c_str());
        std::vector<Bytes> out;
        w.c.close(out);
        for (const Bytes& o : out) w.ep.send(server, o);
        CHECK(waitFor([&] { return srv.allocations() == 3; }, 2), "closed again");
        idle(0.1);
        deactivate(&w);
        deactivate(&u);
    }

    // ---- start() again on the same socket while the server still holds the allocation (a restart
    // without close(), a crashed run): 437 Allocation Mismatch -> release -> allocate.
    {
        const Addr old = a.c.relayed();
        a.c.start(server, creds);
        CHECK(!a.c.ready(), "restart is not ready at once");
        CHECK(waitFor([&] { return a.c.ready() || a.c.failed(); }, 3) && a.c.ready(), "restart over a live allocation: %s", a.c.error().c_str());
        CHECK(srv.allocations() == 3, "%d allocations after the restart", srv.allocations());
        CHECK(waitFor([&] { return a.c.permitted(p1.local); }, 2), "permissions come back after a restart");
        const size_t n = a.got.size();
        p1.send(a.c.relayed(), hello);
        CHECK(waitFor([&] { return a.got.size() == n + 1; }, 2), "data after the restart");
        std::printf("restart over a live allocation: relayed %s -> %s\n", old.text().c_str(), a.c.relayed().text().c_str());
    }

    // ---- close(): the allocation is gone at once.
    {
        std::vector<Bytes> out;
        const Addr wasRelayed = b.c.relayed();
        b.c.close(out);
        CHECK(out.size() == 1 && !b.c.started() && !b.c.ready() && !b.c.failed(), "close()");
        for (const Bytes& o : out) b.ep.send(server, o);
        CHECK(waitFor([&] { return srv.allocations() == 2; }, 2), "%d allocations after close()", srv.allocations());
        CHECK(b.c.wrap(p1.local, hello.data(), 5).empty(), "wrap() after close()");
        const size_t n = b.got.size();
        p1.send(wasRelayed, hello);
        idle(0.2);
        CHECK(b.got.size() == n, "nothing for a closed client");
        out.clear();
        a.c.close(out);
        c.c.close(out);
        CHECK(out.size() == 2, "two more closes");
        if (out.size() == 2) {
            a.ep.send(server, out[0]);
            c.ep.send(server, out[1]);
        }
        CHECK(waitFor([&] { return srv.allocations() == 0; }, 2), "%d allocations after closing all", srv.allocations());
        idle(0.2);
        CHECK(a.notConsumed == 0 && b.notConsumed == 0 && c.notConsumed == 0, "every server datagram was taken (%d %d %d)", a.notConsumed,
              b.notConsumed, c.notConsumed);
        deactivate(&a);
        deactivate(&b);
        deactivate(&c);
        std::printf("close: 0 allocations left\n");
    }

    // ---- Refresh: the server grants 4 s; a client that is ticked stays alive for 12 s, one that
    // is not loses its allocation. At the same time: a server that never answers.
    {
        srv.setLifetimeLimit(4);
        Driver r, x, dead;
        CHECK(r.ep.open("127.0.0.1") && x.ep.open("127.0.0.1") && dead.ep.open("127.0.0.1"), "client sockets");
        sActive.push_back(&r);
        sActive.push_back(&x);
        r.c.start(server, creds);
        x.c.start(server, creds);
        r.c.permit(p1.local);
        CHECK(waitFor([&] { return r.c.permitted(p1.local) && x.c.ready(); }, 3), "allocations with a 4 s lifetime");
        CHECK(srv.allocations() == 2, "%d allocations", srv.allocations());
        deactivate(&x);  // abandoned: never ticked again

        Udp gone;
        CHECK(gone.open("127.0.0.1"), "socket");
        const Addr nowhere = gone.local;
        gone.close();
        dead.c.start(nowhere, creds);
        sActive.push_back(&dead);

        const double t0 = nowS();
        const uint64_t reqs0 = srv.requests();
        double deadAt = -1, lostAt = -1;
        while (nowS() - t0 < 12) {
            for (Driver* dr : sActive) dr->pump();
            if (lostAt < 0 && !r.c.ready()) lostAt = nowS() - t0;
            if (deadAt < 0 && dead.c.failed()) deadAt = nowS() - t0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const uint64_t refreshes = srv.requests() - reqs0;
        CHECK(lostAt < 0 && r.c.ready(), "allocation lost after %.1f s: %s", lostAt, r.c.error().c_str());
        CHECK(srv.allocations() == 1, "%d allocations after 12 s (the abandoned one must be gone)", srv.allocations());
        CHECK(refreshes >= 5 && refreshes <= 8, "%llu refreshes in 12 s", static_cast<unsigned long long>(refreshes));
        CHECK(deadAt > 7.0 && deadAt < 10.0, "silent server: failed after %.2f s", deadAt);
        CHECK(dead.c.error().find("No answer") != std::string::npos, "silent server: '%s'", dead.c.error().c_str());
        p1.send(r.c.relayed(), big1);
        CHECK(waitFor([&] { return r.got.size() == 1; }, 2) && r.got.size() == 1 && r.got[0].second == big1, "relay after 12 s of refreshes");
        std::printf("refresh: alive after 12 s with a 4 s lifetime (%llu refreshes); silent server: \"%s\" after %.2f s\n",
                    static_cast<unsigned long long>(refreshes), dead.c.error().c_str(), deadAt);

        // ---- The server goes away: the client notices.
        srv.stop();
        CHECK(srv.allocations() == 0, "stopped server");
        const double t1 = nowS();
        CHECK(waitFor([&] { return r.c.failed(); }, 10), "server stopped: client still thinks it is ready");
        CHECK(!r.c.ready() && !r.c.error().empty(), "server stopped: error text");
        std::printf("server stopped: \"%s\" after %.2f s\n", r.c.error().c_str(), nowS() - t1);
        deactivate(&r);
        deactivate(&dead);
    }

    std::printf("%s (%d failures)\n", sFails ? "FAILED" : "all TURN tests passed", sFails);
    return sFails;
}
