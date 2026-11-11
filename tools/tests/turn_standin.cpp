// TURN test server (turn_standin.h). Written from RFC 5389 (STUN) and RFC 5766 (TURN): Binding,
// Allocate, Refresh, CreatePermission, Send / Data indications over UDP, long-term credentials.
// Its message code is separate from the client's (src/recomp/online_turn.cpp) on purpose: only
// the hash functions are shared.
#include "turn_standin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/select.h>
#endif

#include "recomp/online_crypto.h"
#include "recomp/online_turn.h"

using namespace online::sock;

namespace {

constexpr uint32_t kCookie = 0x2112A442;
constexpr size_t kMaxAllocations = 48;  // select() on Windows takes 64 sockets

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

struct Attr {
    uint16_t type;
    const uint8_t* v;
    size_t len;
    size_t off;  // of the attribute header
};

// Splits a STUN message into its attributes; false when it is not one.
bool parse(const uint8_t* p, size_t n, std::vector<Attr>& attrs) {
    attrs.clear();
    if (n < 20 || (p[0] & 0xC0) || rd32(p + 4) != kCookie || size_t(rd16(p + 2)) + 20 != n || (n & 3)) return false;
    size_t i = 20;
    while (i < n) {
        if (n - i < 4) return false;
        const size_t len = rd16(p + i + 2);
        const size_t padded = (len + 3) & ~size_t(3);
        if (padded > n - i - 4) return false;
        attrs.push_back({rd16(p + i), p + i + 4, len, i});
        i += 4 + padded;
    }
    return true;
}

const Attr* find(const std::vector<Attr>& attrs, uint16_t type) {
    for (const Attr& a : attrs)
        if (a.type == type) return &a;
    return nullptr;
}

std::string text(const Attr* a) { return a ? std::string(reinterpret_cast<const char*>(a->v), a->len) : std::string(); }

struct Writer {
    std::vector<uint8_t> b;
    Writer(uint16_t type, const uint8_t* txid) {
        b = {uint8_t(type >> 8), uint8_t(type), 0, 0, 0x21, 0x12, 0xA4, 0x42};
        b.insert(b.end(), txid, txid + 12);
    }
    void put(uint16_t type, const void* v, size_t n) {
        b.push_back(uint8_t(type >> 8));
        b.push_back(uint8_t(type));
        b.push_back(uint8_t(n >> 8));
        b.push_back(uint8_t(n));
        const uint8_t* p = static_cast<const uint8_t*>(v);
        b.insert(b.end(), p, p + n);
        while (b.size() % 4) b.push_back(0);
        length();
    }
    void put32(uint16_t type, uint32_t v) {
        const uint8_t x[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        put(type, x, 4);
    }
    void putAddr(uint16_t type, const Addr& a) {
        const uint32_t ip = a.v4() ^ kCookie;
        const uint16_t port = uint16_t(a.port() ^ 0x2112);
        const uint8_t x[8] = {0, 1, uint8_t(port >> 8), uint8_t(port), uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip)};
        put(type, x, 8);
    }
    void error(int code, const char* reason) {
        std::vector<uint8_t> v = {0, 0, uint8_t(code / 100), uint8_t(code % 100)};
        v.insert(v.end(), reason, reason + std::strlen(reason));
        put(0x0009, v.data(), v.size());
    }
    void sign(const uint8_t key[16]) {
        const size_t off = b.size();
        const uint8_t zero[20] = {};
        put(0x0008, zero, 20);  // the length field now counts the attribute, as the HMAC wants
        uint8_t mac[20];
        online::turn::hmacSha1(key, 16, b.data(), off, mac);
        std::memcpy(b.data() + off + 4, mac, 20);
    }
    void length() {
        const size_t l = b.size() - 20;
        b[2] = uint8_t(l >> 8);
        b[3] = uint8_t(l);
    }
};

struct Allocation {
    Addr client;
    Socket relay = kInvalid;
    Addr relayAddr;
    double expiry = 0;
    bool dead = false;  // released; its socket is closed at the end of the loop turn
    uint8_t txid[12] = {};
    uint32_t lifetime = 0;
    std::map<uint32_t, double> perms;  // peer IPv4 -> expiry
};

}  // namespace

struct TurnTestServer::Impl {
    Socket mainSock = kInvalid;
    Addr listen;
    uint32_t bindIp = 0;
    std::string user, pass;
    const std::string realm = "cf-test";
    std::thread th;
    std::atomic<bool> quit{false};
    std::atomic<uint64_t> bytes{0}, reqCount{0}, refused{0};
    std::atomic<int> live{0}, lifeLimit{600}, permLife{300}, drop{0};
    std::mutex mu;  // nonce
    std::string nonce;
    uint8_t key[16] = {};
    std::vector<Allocation> allocs;  // server thread only
    uint32_t indCount = 0;

    void newNonce() {
        uint8_t r[12];
        onlinecrypto::randomBytes(r, sizeof r);
        nonce = onlinecrypto::hex(r, sizeof r);
    }
    std::string currentNonce() {
        std::lock_guard<std::mutex> lk(mu);
        return nonce;
    }

    Allocation* findAlloc(const Addr& client) {
        for (Allocation& a : allocs)
            if (!a.dead && a.client.v4() == client.v4() && a.client.port() == client.port()) return &a;
        return nullptr;
    }
    void count() {
        int n = 0;
        for (const Allocation& a : allocs) n += a.dead ? 0 : 1;
        live = n;
    }
    void send(const Addr& to, const std::vector<uint8_t>& b) {
        sendto(mainSock, reinterpret_cast<const char*>(b.data()), int(b.size()), 0, reinterpret_cast<const sockaddr*>(&to.ss), to.len);
    }
    void sendError(const Addr& to, uint16_t method, const uint8_t* txid, int code, const char* reason, bool challenge) {
        Writer w(method | 0x0110, txid);
        w.error(code, reason);
        if (challenge) {
            const std::string n = currentNonce();
            w.put(0x0014, realm.data(), realm.size());
            w.put(0x0015, n.data(), n.size());
        }
        send(to, w.b);
    }

    void allocateOk(const Addr& to, const Allocation& a, const uint8_t* txid) {
        Writer w(0x0103, txid);
        w.putAddr(0x0016, a.relayAddr);
        w.put32(0x000D, a.lifetime);
        w.putAddr(0x0020, to);
        w.sign(key);
        send(to, w.b);
    }

    void onMain(const Addr& from, const uint8_t* p, size_t n, double now) {
        std::vector<Attr> attrs;
        if (!parse(p, n, attrs)) return;
        const uint16_t type = rd16(p);
        const uint8_t* txid = p + 8;

        if (type == 0x0016) {  // Send indication
            Allocation* a = findAlloc(from);
            const Attr* peer = find(attrs, 0x0012);
            const Attr* data = find(attrs, 0x0013);
            if (!a || !peer || !data || peer->len < 8 || peer->v[1] != 1) return;
            const uint32_t ip = rd32(peer->v + 4) ^ kCookie;
            const uint16_t port = uint16_t(rd16(peer->v + 2) ^ 0x2112);
            const auto it = a->perms.find(ip);
            if (it == a->perms.end() || it->second <= now) {
                refused++;
                return;
            }
            const Addr to = makeV4(ip, port);
            sendto(a->relay, reinterpret_cast<const char*>(data->v), int(data->len), 0, reinterpret_cast<const sockaddr*>(&to.ss), to.len);
            bytes += data->len;
            return;
        }
        if (type & 0x0110) return;  // only requests from here on
        reqCount++;
        if (drop > 0) {
            drop--;
            return;
        }
        if (type == 0x0001) {  // Binding: no credentials needed
            Writer w(0x0101, txid);
            w.putAddr(0x0020, from);
            send(from, w.b);
            return;
        }

        // Long-term credentials (RFC 5389 section 10.2.2).
        const Attr* mi = find(attrs, 0x0008);
        if (!mi) return sendError(from, type, txid, 401, "Unauthorized", true);
        const Attr* un = find(attrs, 0x0006);
        const Attr* rl = find(attrs, 0x0014);
        const Attr* nc = find(attrs, 0x0015);
        if (!un || !rl || !nc) return sendError(from, type, txid, 400, "Bad Request", false);
        if (text(nc) != currentNonce()) return sendError(from, type, txid, 438, "Stale Nonce", true);
        bool ok = text(un) == user && text(rl) == realm && mi->len == 20;
        if (ok) {
            uint8_t hdr[20], mac[20];
            std::memcpy(hdr, p, 20);
            const size_t len = mi->off + 24 - 20;
            hdr[2] = uint8_t(len >> 8);
            hdr[3] = uint8_t(len);
            online::turn::hmacSha1(key, 16, hdr, 20, p + 20, mi->off - 20, mac);
            ok = std::memcmp(mac, mi->v, 20) == 0;
        }
        if (!ok) return sendError(from, type, txid, 401, "Unauthorized", true);

        Allocation* a = findAlloc(from);
        const Attr* lt = find(attrs, 0x000D);
        const uint32_t asked = (lt && lt->len >= 4) ? rd32(lt->v) : 600;
        const uint32_t limit = uint32_t(std::max(1, lifeLimit.load()));

        if (type == 0x0003) {  // Allocate
            if (a) {
                if (std::memcmp(a->txid, txid, 12) == 0) return allocateOk(from, *a, txid);  // retransmission
                return sendError(from, type, txid, 437, "Allocation Mismatch", false);
            }
            const Attr* rt = find(attrs, 0x0019);
            if (!rt || rt->len < 4) return sendError(from, type, txid, 400, "Bad Request", false);
            if (rt->v[0] != 17) return sendError(from, type, txid, 442, "Unsupported Transport Protocol", false);
            if (allocs.size() >= kMaxAllocations) return sendError(from, type, txid, 486, "Allocation Quota Reached", false);
            Allocation na;
            na.client = from;
            na.relay = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Addr want = makeV4(bindIp, 0);
            if (na.relay == kInvalid || bind(na.relay, reinterpret_cast<const sockaddr*>(&want.ss), want.len) != 0) {
                if (na.relay != kInvalid) closeSocket(na.relay);
                return sendError(from, type, txid, 508, "Insufficient Capacity", false);
            }
            setNonBlocking(na.relay);
            na.relayAddr.len = sizeof(sockaddr_in);
            socklen_t sl = sizeof(sockaddr_storage);
            getsockname(na.relay, reinterpret_cast<sockaddr*>(&na.relayAddr.ss), &sl);
            na.relayAddr.len = sl;
            na.lifetime = std::min(asked ? asked : 600u, limit);
            na.expiry = now + na.lifetime;
            std::memcpy(na.txid, txid, 12);
            allocs.push_back(na);
            count();
            return allocateOk(from, allocs.back(), txid);
        }
        if (type == 0x0004) {  // Refresh
            if (!a) return sendError(from, type, txid, 437, "Allocation Mismatch", false);
            uint32_t granted = 0;
            if (asked == 0) {
                a->dead = true;
                count();
            } else {
                granted = std::min(asked, limit);
                a->lifetime = granted;
                a->expiry = now + granted;
            }
            Writer w(0x0104, txid);
            w.put32(0x000D, granted);
            w.sign(key);
            return send(from, w.b);
        }
        if (type == 0x0008) {  // CreatePermission
            if (!a) return sendError(from, type, txid, 437, "Allocation Mismatch", false);
            int n4 = 0;
            for (const Attr& x : attrs) {
                if (x.type != 0x0012) continue;
                if (x.len < 8 || x.v[1] != 1) return sendError(from, type, txid, 443, "Peer Address Family Mismatch", false);
                n4++;
            }
            if (!n4) return sendError(from, type, txid, 400, "Bad Request", false);
            for (const Attr& x : attrs)
                if (x.type == 0x0012) a->perms[rd32(x.v + 4) ^ kCookie] = now + permLife.load();
            Writer w(0x0108, txid);
            w.sign(key);
            return send(from, w.b);
        }
        sendError(from, type, txid, 400, "Bad Request", false);
    }

    void onRelay(Allocation& a, const Addr& from, const uint8_t* p, size_t n, double now) {
        const auto it = a.perms.find(from.v4());
        if (from.family() != AF_INET || it == a.perms.end() || it->second <= now) {
            refused++;
            return;
        }
        uint8_t txid[12] = {'d', 'a', 't', 'a'};
        const uint32_t c = indCount++;
        std::memcpy(txid + 8, &c, 4);
        Writer w(0x0017, txid);
        w.putAddr(0x0012, from);
        w.put(0x0013, p, n);
        send(a.client, w.b);
        bytes += n;
    }

    void run() {
        std::vector<uint8_t> buf(65536);
        while (!quit) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(mainSock, &rd);
            Socket top = mainSock;
            for (const Allocation& a : allocs) {
                FD_SET(a.relay, &rd);
                if (a.relay > top) top = a.relay;
            }
            timeval tv{0, 20000};
            const int k = select(int(top) + 1, &rd, nullptr, nullptr, &tv);
            const double now = nowSeconds();
            if (k > 0) {
                // Relay sockets first: the main socket's requests may add allocations that are not
                // in the set.
                for (size_t i = 0; i < allocs.size(); i++) {
                    if (!FD_ISSET(allocs[i].relay, &rd)) continue;
                    for (int burst = 0; burst < 64; burst++) {
                        Addr from;
                        socklen_t sl = sizeof(sockaddr_storage);
                        const int n = int(recvfrom(allocs[i].relay, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                                   reinterpret_cast<sockaddr*>(&from.ss), &sl));
                        if (n < 0) break;  // nothing more (or an ICMP error for an earlier send: ignored)
                        from.len = sl;
                        if (!allocs[i].dead) onRelay(allocs[i], from, buf.data(), size_t(n), now);
                    }
                }
                if (FD_ISSET(mainSock, &rd)) {
                    for (int burst = 0; burst < 64; burst++) {
                        Addr from;
                        socklen_t sl = sizeof(sockaddr_storage);
                        const int n = int(recvfrom(mainSock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                                   reinterpret_cast<sockaddr*>(&from.ss), &sl));
                        if (n < 0) break;
                        from.len = sl;
                        onMain(from, buf.data(), size_t(n), now);
                    }
                }
            }
            bool changed = false;
            for (size_t i = 0; i < allocs.size();) {
                if (allocs[i].dead || allocs[i].expiry <= now) {
                    closeSocket(allocs[i].relay);
                    allocs.erase(allocs.begin() + ptrdiff_t(i));
                    changed = true;
                } else {
                    i++;
                }
            }
            if (changed) count();
        }
    }
};

TurnTestServer::TurnTestServer() : d(new Impl) {}
TurnTestServer::~TurnTestServer() { stop(); }

bool TurnTestServer::start(const std::string& bindIp, uint16_t port, const std::string& user, const std::string& pass, std::string* err) {
    stop();
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        if (d->mainSock != kInvalid) closeSocket(d->mainSock);
        d->mainSock = kInvalid;
        return false;
    };
    startup();
    in_addr ia{};
    if (inet_pton(AF_INET, bindIp.c_str(), &ia) != 1) return fail("not an IPv4 address: " + bindIp);
    d->bindIp = ntohl(ia.s_addr);
    d->mainSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (d->mainSock == kInvalid) return fail("socket() failed");
    const Addr want = makeV4(d->bindIp, port);
    if (bind(d->mainSock, reinterpret_cast<const sockaddr*>(&want.ss), want.len) != 0)
        return fail("cannot bind " + want.text() + " (error " + std::to_string(lastError()) + ")");
    setNonBlocking(d->mainSock);
    socklen_t sl = sizeof(sockaddr_storage);
    d->listen = Addr();
    getsockname(d->mainSock, reinterpret_cast<sockaddr*>(&d->listen.ss), &sl);
    d->listen.len = sl;
    d->user = user;
    d->pass = pass;
    online::turn::longTermKey(user, d->realm, pass, d->key);
    d->newNonce();
    d->quit = false;
    d->bytes = 0;
    d->reqCount = 0;
    d->refused = 0;
    d->drop = 0;
    d->live = 0;
    d->th = std::thread([this] { d->run(); });
    return true;
}

void TurnTestServer::stop() {
    if (d->th.joinable()) {
        d->quit = true;
        d->th.join();
    }
    for (Allocation& a : d->allocs) closeSocket(a.relay);
    d->allocs.clear();
    d->live = 0;
    if (d->mainSock != kInvalid) closeSocket(d->mainSock);
    d->mainSock = kInvalid;
}

Addr TurnTestServer::addr() const { return d->listen; }
uint64_t TurnTestServer::relayedBytes() const { return d->bytes; }
int TurnTestServer::allocations() const { return d->live; }
void TurnTestServer::setLifetimeLimit(int seconds) { d->lifeLimit = seconds; }
void TurnTestServer::setPermissionLifetime(int seconds) { d->permLife = seconds; }
void TurnTestServer::rotateNonce() {
    std::lock_guard<std::mutex> lk(d->mu);
    d->newNonce();
}
void TurnTestServer::dropRequests(int n) { d->drop = n; }
uint64_t TurnTestServer::requests() const { return d->reqCount; }
uint64_t TurnTestServer::droppedDatagrams() const { return d->refused; }
