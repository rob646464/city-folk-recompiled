// Serverless room transport for online play (online_net.h, docs/ONLINE.md "Rooms and transport").
//
// Wire format (UDP, every datagram starts with 'C' 'F' <protocol version> <type>):
//   HELLO      joiner -> host   plain: game id, port version, name, joiner nonce, room id (from
//                               the code's secret; 0 = direct connect without a code)
//   CHALLENGE  host -> joiner   plain: host nonce, password flag, PBKDF2 salt + iterations
//   AUTH       joiner -> host   plain: HMAC(master, "cf-auth" | nonces) proves the secret and
//                               password without sending either
//   WAIT       host -> joiner   "ask before join": the host has not answered yet
//   REJECT     host -> joiner   reason + text (wrong game/version, full, wrong password, ...)
//   SEALED     both             ChaCha20 + HMAC-SHA256/128 under the session key; reliable
//                               (acked, retransmitted, in order, fragmented) and unreliable
//                               messages, keepalive pings, member list, kicks
//   BEACON     host -> LAN      broadcast room announcement (LAN list)
//   PROBE      host -> itself   reachability check through the router (hairpin)
// master = HMAC(room secret, "cf-room" | PBKDF2(password, salt)); session key =
// HMAC(master, "cf-sess" | host nonce | joiner nonce); one seal key per direction.
#include "recomp/online_net.h"

#include "recomp/online_crypto.h"
#include "recomp/online_nat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace online::sock {

bool startup() {
#if defined(_WIN32)
    static bool sOk = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return sOk;
#else
    return true;
#endif
}

uint32_t Addr::v4() const {
    if (ss.ss_family == AF_INET) return ntohl(reinterpret_cast<const sockaddr_in*>(&ss)->sin_addr.s_addr);
    if (ss.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&a->sin6_addr);
        static const uint8_t pre[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(b, pre, 12) == 0) return uint32_t(b[12]) << 24 | uint32_t(b[13]) << 16 | uint32_t(b[14]) << 8 | b[15];
    }
    return 0;
}

uint16_t Addr::port() const {
    if (ss.ss_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
    if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port);
    return 0;
}

Addr makeV4(uint32_t ip, uint16_t port) {
    Addr a;
    auto* s = reinterpret_cast<sockaddr_in*>(&a.ss);
    s->sin_family = AF_INET;
    s->sin_addr.s_addr = htonl(ip);
    s->sin_port = htons(port);
    a.len = sizeof(sockaddr_in);
    return a;
}

Addr makeV6(const uint8_t ip[16], uint16_t port) {
    Addr a;
    auto* s = reinterpret_cast<sockaddr_in6*>(&a.ss);
    s->sin6_family = AF_INET6;
    std::memcpy(&s->sin6_addr, ip, 16);
    s->sin6_port = htons(port);
    a.len = sizeof(sockaddr_in6);
    return a;
}

Addr Addr::normalized() const {
    if (ss.ss_family == AF_INET6) {
        const uint32_t v = v4();
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&a->sin6_addr);
        static const uint8_t pre[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(b, pre, 12) == 0) return makeV4(v, port());
        // Scope ids and flow info do not take part in comparisons.
        return makeV6(b, port());
    }
    return *this;
}

Addr Addr::forSocket(int sockFamily) const {
    if (sockFamily == AF_INET6 && ss.ss_family == AF_INET) {
        uint8_t b[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        const uint32_t v = v4();
        b[12] = uint8_t(v >> 24);
        b[13] = uint8_t(v >> 16);
        b[14] = uint8_t(v >> 8);
        b[15] = uint8_t(v);
        return makeV6(b, port());
    }
    return *this;
}

std::string Addr::ipText() const {
    const Addr n = normalized();
    char buf[64] = {};
    if (n.ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&n.ss)->sin_addr, buf, sizeof buf);
    } else if (n.ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&n.ss)->sin6_addr, buf, sizeof buf);
    }
    return buf;
}

std::string Addr::text() const {
    const Addr n = normalized();
    if (n.ss.ss_family == AF_INET6) return "[" + ipText() + "]:" + std::to_string(port());
    return ipText() + ":" + std::to_string(port());
}

bool resolve(const std::string& textIn, uint16_t defPort, Addr* out) {
    startup();
    std::string text = textIn;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    std::string host = text, port = std::to_string(defPort);
    if (!text.empty() && text[0] == '[') {
        const size_t e = text.find(']');
        if (e == std::string::npos) return false;
        host = text.substr(1, e - 1);
        if (e + 1 < text.size() && text[e + 1] == ':') port = text.substr(e + 2);
    } else {
        const size_t c = text.rfind(':');
        if (c != std::string::npos && text.find(':') == c) {
            host = text.substr(0, c);
            port = text.substr(c + 1);
        }
    }
    if (host.empty()) return false;
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    // Prefer IPv4 (most home routers map IPv4 ports).
    addrinfo* pick = res;
    for (addrinfo* r = res; r; r = r->ai_next)
        if (r->ai_family == AF_INET) {
            pick = r;
            break;
        }
    std::memcpy(&out->ss, pick->ai_addr, pick->ai_addrlen);
    out->len = socklen_t(pick->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

}  // namespace online::sock

namespace online::net {
namespace {

using namespace online::sock;
namespace oc = onlinecrypto;

constexpr u8 kProto = 1;
constexpr u32 kPbkdfIterations = 100000;
constexpr size_t kMaxDatagram = 1400;
constexpr size_t kFragment = 1100;
constexpr size_t kWindow = 64;

enum : u8 {
    T_HELLO = 1, T_CHALLENGE = 2, T_AUTH = 3, T_REJECT = 4, T_WAIT = 5,
    T_SEALED = 0x10, T_BEACON = 0x21, T_PROBE = 0x22,
};
enum : u8 { B_PING = 1, B_PONG = 2, B_ACK = 3, B_REL = 4, B_UNREL = 5, B_BYE = 6 };
enum : u8 { R_WELCOME = 1, R_MEMBERS = 2, R_PROFILE = 3, R_KICK = 4, R_APP = 0x10 };
enum : u8 {
    REJ_VERSION = 1, REJ_FULL = 2, REJ_AUTH = 3, REJ_RATE = 4, REJ_KICKED = 5, REJ_DECLINED = 6,
    REJ_NEEDCODE = 7, REJ_CLOSED = 8, REJ_WRONGROOM = 9,
};

bool sTestMode = false;

double now() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

// Byte writer / reader.
struct W {
    std::vector<u8> b;
    W& u8_(u8 v) { b.push_back(v); return *this; }
    W& u16_(u16 v) { b.push_back(u8(v)); b.push_back(u8(v >> 8)); return *this; }
    W& u32_(u32 v) { for (int i = 0; i < 4; i++) b.push_back(u8(v >> (8 * i))); return *this; }
    W& u64_(u64 v) { for (int i = 0; i < 8; i++) b.push_back(u8(v >> (8 * i))); return *this; }
    W& bytes(const void* p, size_t n) { b.insert(b.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n); return *this; }
    W& str(const std::string& s) {
        const size_t n = std::min<size_t>(s.size(), 255);
        u8_(u8(n));
        return bytes(s.data(), n);
    }
};
struct R {
    const u8* p;
    size_t n, i = 0;
    bool ok = true;
    R(const u8* p_, size_t n_) : p(p_), n(n_) {}
    bool need(size_t k) {
        if (i + k > n) ok = false;
        return ok;
    }
    u8 u8_() { return need(1) ? p[i++] : 0; }
    u16 u16_() { if (!need(2)) return 0; u16 v = u16(p[i] | p[i + 1] << 8); i += 2; return v; }
    u32 u32_() { if (!need(4)) return 0; u32 v = 0; for (int k = 0; k < 4; k++) v |= u32(p[i + k]) << (8 * k); i += 4; return v; }
    u64 u64_() { if (!need(8)) return 0; u64 v = 0; for (int k = 0; k < 8; k++) v |= u64(p[i + k]) << (8 * k); i += 8; return v; }
    bool bytes(void* out, size_t k) { if (!need(k)) return false; std::memcpy(out, p + i, k); i += k; return true; }
    std::string str() {
        const u8 k = u8_();
        if (!need(k)) return {};
        std::string s(reinterpret_cast<const char*>(p + i), k);
        i += k;
        return s;
    }
    const u8* rest(size_t* len) { *len = ok ? n - i : 0; return p + i; }
};

std::vector<u8> head(u8 type) { return {'C', 'F', kProto, type}; }

// Text from the network: printable, bounded.
std::string clean(std::string s, size_t max = 32) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) continue;
        o += char(c);
        if (o.size() >= max) break;
    }
    return o;
}

void roomIdOf(const u8 secret[10], u8 out[4]) {
    u8 h[32];
    std::vector<u8> in = {'c', 'f', '-', 'r', 'o', 'o', 'm', '-', 'i', 'd'};
    in.insert(in.end(), secret, secret + 10);
    oc::sha256(in.data(), in.size(), h);
    std::memcpy(out, h, 4);
}

void masterKey(const u8 secret[10], const u8* pwKey /*32 or null*/, u8 out[32]) {
    std::vector<u8> m = {'c', 'f', '-', 'r', 'o', 'o', 'm'};
    if (pwKey) m.insert(m.end(), pwKey, pwKey + 32);
    oc::hmacSha256(secret, 10, m.data(), m.size(), out);
}

void derived(const u8 master[32], const char* label, const u8 hn[16], const u8 cn[16], u8 out[32]) {
    std::vector<u8> m(label, label + std::strlen(label));
    m.insert(m.end(), hn, hn + 16);
    m.insert(m.end(), cn, cn + 16);
    oc::hmacSha256(master, 32, m.data(), m.size(), out);
}

const char* rejectText(u8 r) {
    switch (r) {
    case REJ_VERSION: return "The host plays a different game version or region";
    case REJ_FULL: return "The room is full";
    case REJ_AUTH: return "Wrong password or room code";
    case REJ_RATE: return "Too many failed attempts: try again in a minute";
    case REJ_KICKED: return "The host removed you from this room";
    case REJ_DECLINED: return "The host declined your request to join";
    case REJ_NEEDCODE: return "This room needs its room code (the host turned off direct connections)";
    case REJ_CLOSED: return "The room was closed";
    case REJ_WRONGROOM: return "That room is no longer open (old room code?)";
    }
    return "Refused by the host";
}

struct Link {
    Addr addr;  // normalized
    u8 id = 0;
    std::string name;
    u32 pid = 0;
    u64 fkey = 0;
    int rtt = -1;
    oc::SealKey tx{}, rx{};
    u64 txCtr = 0, rxMax = 0, rxWin = 0;
    bool anyRx = false;
    u32 nextSeq = 1, expect = 1;
    struct Out {
        u32 seq;
        std::vector<u8> body;
        double sent, rto, first;
    };
    std::deque<Out> inflight;
    std::deque<std::vector<u8>> queue;  // REL bodies not yet given a sequence number
    size_t queued = 0;
    std::map<u32, std::vector<u8>> early;
    std::vector<u32> acks;
    double lastRecv = 0, lastPing = 0;
    std::map<u8, std::vector<u8>> reasm;  // per origin member
    bool dead = false;
    std::string leftWhy;  // BYE text (host log)
};

struct PendingJoin {
    Addr addr;
    u8 cn[16], hn[16];
    std::string name, gameId, portVersion;
    bool direct = false;
    double t = 0;
    bool authed = false;      // proof checked, waiting for the host's answer
    u32 ticket = 0;
    u8 sess[32];
    bool decided = false, accept = false;
};

}  // namespace

struct Session::Impl {
    std::recursive_mutex m;
    std::thread th;
    std::atomic<bool> run{false};
    Identity ident;
    enum class Role { None, Host, Member } role = Role::None;
    State st = State::Off;
    std::string msg;
    Socket s = kInvalid;
    int sfam = AF_INET;
    u16 port = 0;
    Socket lan = kInvalid;      // beacons (host: send; scanner: receive)
    Socket lanScanSock = kInvalid;
    HostOptions opt;
    u8 secret[10] = {};
    u8 pwKey[32] = {};
    bool hasPw = false;
    u8 salt[16] = {};
    std::string roomName, code;
    // host
    std::vector<std::unique_ptr<Link>> links;
    std::vector<PendingJoin> pend;
    std::map<std::string, std::pair<int, double>> fails;  // ip -> (count, window start)
    std::map<std::string, double> blocked;                // ip -> until
    std::set<u32> kickedPids;  // this room only
    std::set<std::string> kickedAddrs;
    u32 nextTicket = 1;
    double lastBeacon = -10, lastMembers = 0;
    // member
    std::unique_ptr<Link> hostLink;
    std::vector<Addr> tryAddrs;
    Addr hostAddr;
    bool joinDirect = false;
    std::string joinPassword;
    u8 cn[16] = {}, hn[16] = {};
    enum class JoinStep { Hello, Auth, Wait, Done } step = JoinStep::Hello;
    double joinStart = 0, lastSend = 0;
    std::vector<u8> authPkt;
    u8 selfId = 0;
    std::vector<Member> roster;  // member view (from MEMBERS)
    // queues
    std::deque<AppMsg> inbox;
    std::deque<MemberEvent> events;
    // LAN scan
    bool scanning = false;
    struct Heard {
        LanRoom room;
        double t;
    };
    std::map<std::string, Heard> heard;
    // NAT
    u32 gen = 0;  // room generation (a late port-mapping result of an old room is ignored)
    std::string lanEp, pubEp, reach;
    std::vector<Endpoint> codeEps;
    Addr stunAddr;
    u8 stunTx[12] = {};
    double stunSent = -100;
    int stunTries = 0;
    Addr publicAddr;
    bool mapped = false;
    u8 probeTok[8] = {};
    double probeSent = -100;
    int probes = 0;
    bool probeOk = false;
    u16 mappedPort = 0;

    // ---- helpers (lock held) ----
    void sendRaw(const Addr& to, const std::vector<u8>& b) {
        if (s == kInvalid) return;
        const Addr a = to.forSocket(sfam);
        sendto(s, reinterpret_cast<const char*>(b.data()), int(b.size()), 0, reinterpret_cast<const sockaddr*>(&a.ss), a.len);
    }

    void sendSealed(Link& l, u8 senderId, std::vector<u8> body) {
        std::vector<u8> pkt = head(T_SEALED);
        pkt.push_back(senderId);
        pkt.push_back(0);
        pkt.push_back(0);
        pkt.push_back(0);
        const u64 ctr = ++l.txCtr;
        for (int i = 0; i < 8; i++) pkt.push_back(u8(ctr >> (8 * i)));
        u8 tag[oc::kTagSize];
        oc::seal(l.tx, ctr, pkt.data(), pkt.size(), body.data(), body.size(), tag);
        pkt.insert(pkt.end(), body.begin(), body.end());
        pkt.insert(pkt.end(), tag, tag + oc::kTagSize);
        sendRaw(l.addr, pkt);
    }

    u8 myId() const { return role == Role::Host ? 0 : selfId; }

    void queueRel(Link& l, u8 rk, const std::vector<u8>& payload) {
        std::vector<u8> b;
        b.reserve(payload.size() + 1);
        b.push_back(rk);
        b.insert(b.end(), payload.begin(), payload.end());
        l.queued += b.size();
        l.queue.push_back(std::move(b));
        pumpLink(l);
    }

    void sendUnrel(Link& l, u8 rk, const std::vector<u8>& payload) {
        W w;
        w.u8_(B_UNREL).u8_(rk).bytes(payload.data(), payload.size());
        sendSealed(l, myId(), w.b);
    }

    void pumpLink(Link& l) {
        const double t = now();
        while (!l.queue.empty() && l.inflight.size() < kWindow) {
            Link::Out o;
            o.seq = l.nextSeq++;
            o.body = std::move(l.queue.front());
            l.queue.pop_front();
            o.first = t;
            o.sent = -1;
            o.rto = l.rtt > 0 ? std::clamp(l.rtt * 2.5 / 1000.0, 0.1, 2.0) : 0.3;
            l.inflight.push_back(std::move(o));
        }
        for (auto& o : l.inflight) {
            if (o.sent >= 0 && t - o.sent < o.rto) continue;
            if (o.sent >= 0) o.rto = std::min(o.rto * 2, 2.0);
            o.sent = t;
            W w;
            w.u8_(B_REL).u32_(o.seq).bytes(o.body.data(), o.body.size());
            sendSealed(l, myId(), w.b);
        }
        if (!l.inflight.empty() && t - l.inflight.front().first > (sTestMode ? 8.0 : 20.0)) l.dead = true;
    }

    void flushAcks(Link& l) {
        while (!l.acks.empty()) {
            W w;
            const size_t n = std::min<size_t>(l.acks.size(), 64);
            w.u8_(B_ACK).u8_(u8(n));
            for (size_t i = 0; i < n; i++) w.u32_(l.acks[i]);
            l.acks.erase(l.acks.begin(), l.acks.begin() + long(n));
            sendSealed(l, myId(), w.b);
        }
    }

    Link* linkById(u8 id) {
        for (auto& l : links)
            if (l->id == id && !l->dead) return l.get();
        return nullptr;
    }

    // Messages nobody polls (DWC traffic while the game is not online) must not crowd out the
    // rest: the oldest goes when the inbox is full.
    void pushInbox(AppMsg&& m) {
        if (inbox.size() >= 4096) inbox.pop_front();
        inbox.push_back(std::move(m));
    }

    // App frame: from, to, type, frag, [total], data.
    void sendAppFrames(Link& l, u8 from, u8 to, u8 type, const u8* data, size_t len, bool reliable) {
        if (!reliable) {
            W w;
            w.u8_(from).u8_(to).u8_(type).u8_(0).bytes(data, len);
            sendUnrel(l, R_APP, w.b);
            return;
        }
        if (len <= kFragment) {
            W w;
            w.u8_(from).u8_(to).u8_(type).u8_(0).bytes(data, len);
            queueRel(l, R_APP, w.b);
            return;
        }
        size_t off = 0;
        while (off < len) {
            const size_t take = std::min(kFragment, len - off);
            W w;
            const u8 frag = off == 0 ? 1 : (off + take == len ? 3 : 2);
            w.u8_(from).u8_(to).u8_(type).u8_(frag);
            if (frag == 1) w.u32_(u32(len));
            w.bytes(data + off, take);
            queueRel(l, R_APP, w.b);
            off += take;
        }
    }

    // Delivers (or forwards, host) one app frame that came in on link `src` (null = local).
    void appFrame(Link* src, const u8* p, size_t n, bool reliable) {
        R r(p, n);
        u8 from = r.u8_();
        const u8 to = r.u8_();
        const u8 type = r.u8_();
        const u8 frag = r.u8_();
        if (!r.ok) return;
        if (role == Role::Host && src) {
            from = src->id;  // no spoofing
            std::vector<u8> fwd(p, p + n);
            fwd[0] = from;
            auto forward = [&](Link& t) {
                if (reliable) queueRel(t, R_APP, fwd);
                else sendUnrel(t, R_APP, fwd);
            };
            if (to == kBroadcast) {
                for (auto& l : links)
                    if (l.get() != src && !l->dead) forward(*l);
            } else if (to != 0) {
                if (Link* t = linkById(to)) forward(*t);
                return;
            }
        }
        size_t restLen = 0;
        if (frag == 1) {
            const u32 total = r.u32_();
            if (!r.ok || total > kMaxAppMessage) return;
            const u8* d = r.rest(&restLen);
            auto& buf = (src ? src->reasm : hostLink->reasm)[from];
            buf.assign(d, d + restLen);
            buf.reserve(total);
            return;
        }
        const u8* d = r.rest(&restLen);
        if (frag == 2 || frag == 3) {
            Link* owner = src ? src : hostLink.get();
            if (!owner) return;
            auto it = owner->reasm.find(from);
            if (it == owner->reasm.end()) return;
            if (it->second.size() + restLen > kMaxAppMessage) {
                owner->reasm.erase(it);
                return;
            }
            it->second.insert(it->second.end(), d, d + restLen);
            if (frag == 2) return;
            AppMsg m;
            m.from = from;
            m.type = type;
            m.data = std::move(it->second);
            owner->reasm.erase(it);
            pushInbox(std::move(m));
            return;
        }
        AppMsg m;
        m.from = from;
        m.type = type;
        m.data.assign(d, d + restLen);
        pushInbox(std::move(m));
    }

    std::vector<u8> membersBody() {
        W w;
        w.u8_(u8(links.size() + 1));
        w.u8_(0).str(ident.name).u32_(ident.pid).u64_(ident.friendKey).u16_(0);
        for (auto& l : links) w.u8_(l->id).str(l->name).u32_(l->pid).u64_(l->fkey).u16_(u16(std::max(l->rtt, 0)));
        return w.b;
    }

    void broadcastMembers() {
        const auto b = membersBody();
        for (auto& l : links)
            if (!l->dead) queueRel(*l, R_MEMBERS, b);
        lastMembers = now();
    }

    void dropLink(size_t i, const char* why) {
        Link& l = *links[i];
        if (why) {
            W w;
            w.u8_(B_BYE).str(why);
            sendSealed(l, 0, w.b);
        }
        events.push_back({false, l.id});
        std::printf("[online] member %u (%s) left: %s\n", l.id, l.name.c_str(),
                    why ? why : l.leftWhy.empty() ? "no answer (timeout)" : l.leftWhy.c_str());
        links.erase(links.begin() + long(i));
        broadcastMembers();
    }

    // ---- receive ----
    void onSealed(const Addr& from, const u8* p, size_t n) {
        if (n < 16 + oc::kTagSize) return;
        Link* l = nullptr;
        if (role == Role::Host) {
            for (auto& x : links)
                if (x->addr == from) l = x.get();
        } else if (hostLink && hostLink->addr == from) {
            l = hostLink.get();
        }
        if (!l) return;
        u64 ctr = 0;
        for (int i = 0; i < 8; i++) ctr |= u64(p[8 + i]) << (8 * i);
        // Replay window (64 packets).
        if (l->anyRx) {
            if (ctr + 64 <= l->rxMax) return;
            if (ctr <= l->rxMax && (l->rxWin >> (l->rxMax - ctr) & 1)) return;
        }
        std::vector<u8> body(p + 16, p + n - oc::kTagSize);
        if (!oc::open(l->rx, ctr, p, 16, body.data(), body.size(), p + n - oc::kTagSize)) return;
        if (!l->anyRx || ctr > l->rxMax) {
            const u64 shift = l->anyRx ? ctr - l->rxMax : 64;
            l->rxWin = shift >= 64 ? 1 : (l->rxWin << shift) | 1;
            l->rxMax = ctr;
            l->anyRx = true;
        } else {
            l->rxWin |= u64(1) << (l->rxMax - ctr);
        }
        l->lastRecv = now();
        onBody(*l, body.data(), body.size());
    }

    void onBody(Link& l, const u8* p, size_t n) {
        R r(p, n);
        const u8 kind = r.u8_();
        switch (kind) {
        case B_PING: {
            const u32 t = r.u32_();
            W w;
            w.u8_(B_PONG).u32_(t);
            sendSealed(l, myId(), w.b);
            break;
        }
        case B_PONG: {
            const u32 t = r.u32_();
            const u32 nowMs = u32(now() * 1000);
            l.rtt = int(nowMs - t);
            break;
        }
        case B_ACK: {
            const u8 k = r.u8_();
            for (int i = 0; i < k && r.ok; i++) {
                const u32 seq = r.u32_();
                for (auto it = l.inflight.begin(); it != l.inflight.end(); ++it)
                    if (it->seq == seq) {
                        l.queued -= std::min(l.queued, it->body.size());
                        l.inflight.erase(it);
                        break;
                    }
            }
            pumpLink(l);
            break;
        }
        case B_REL: {
            const u32 seq = r.u32_();
            if (!r.ok) return;
            l.acks.push_back(seq);
            if (seq < l.expect || l.early.count(seq)) return;
            size_t len = 0;
            const u8* d = r.rest(&len);
            l.early[seq].assign(d, d + len);
            while (true) {
                auto it = l.early.find(l.expect);
                if (it == l.early.end()) break;
                std::vector<u8> b = std::move(it->second);
                l.early.erase(it);
                l.expect++;
                if (!b.empty()) onRel(l, b[0], b.data() + 1, b.size() - 1);
                if (l.dead) return;
            }
            break;
        }
        case B_UNREL: {
            const u8 rk = r.u8_();
            size_t len = 0;
            const u8* d = r.rest(&len);
            if (rk == R_APP) appFrame(role == Role::Host ? &l : nullptr, d, len, false);
            break;
        }
        case B_BYE: {
            const std::string why = clean(r.str(), 96);
            l.dead = true;
            l.leftWhy = why.empty() ? "left" : why;
            if (role == Role::Member) {
                st = State::Failed;
                msg = why.empty() ? "The host closed the room" : why;
            }
            break;
        }
        }
    }

    void onRel(Link& l, u8 rk, const u8* p, size_t n) {
        R r(p, n);
        switch (rk) {
        case R_WELCOME:
            if (role == Role::Member) {
                selfId = r.u8_();
                roomName = clean(r.str());
                hasPw = r.u8_() != 0;
                st = State::Joined;
                step = JoinStep::Done;
                msg = "Joined " + (roomName.empty() ? std::string("the room") : "\"" + roomName + "\"");
                std::printf("[online] joined room as member %u\n", selfId);
                // Our profile (pid / friend code) to the host.
                sendProfile();
            }
            break;
        case R_MEMBERS:
            if (role == Role::Member) {
                const u8 k = r.u8_();
                std::vector<Member> ms;
                for (int i = 0; i < k && r.ok && i < kMaxMembers + 1; i++) {
                    Member m;
                    m.id = r.u8_();
                    m.name = clean(r.str());
                    m.pid = r.u32_();
                    m.friendKey = r.u64_();
                    m.rttMs = r.u16_();
                    m.host = m.id == 0;
                    m.self = m.id == selfId;
                    if (r.ok) ms.push_back(m);
                }
                // Joined / left events for the DWC layer.
                for (auto& m : ms) {
                    bool known = false;
                    for (auto& o : roster) known |= o.id == m.id;
                    if (!known && !m.self) events.push_back({true, m.id});
                }
                for (auto& o : roster) {
                    bool still = false;
                    for (auto& m : ms) still |= o.id == m.id;
                    if (!still && !o.self) events.push_back({false, o.id});
                }
                roster = std::move(ms);
            }
            break;
        case R_PROFILE:
            if (role == Role::Host) {
                l.name = clean(r.str());
                l.pid = r.u32_();
                l.fkey = r.u64_();
                broadcastMembers();
            }
            break;
        case R_KICK:
            if (role == Role::Member) {
                st = State::Failed;
                msg = rejectText(REJ_KICKED);
                l.dead = true;
            }
            break;
        case R_APP:
            appFrame(role == Role::Host ? &l : nullptr, p, n, true);
            break;
        }
    }

    void sendProfile() {
        if (role != Role::Member || !hostLink || st != State::Joined) return;
        W w;
        w.str(ident.name).u32_(ident.pid).u64_(ident.friendKey);
        queueRel(*hostLink, R_PROFILE, w.b);
    }

    // Host: handshake packets.
    void reject(const Addr& to, u8 reason, const std::string& text = {}) {
        W w;
        w.b = head(T_REJECT);
        w.u8_(reason).str(text.empty() ? rejectText(reason) : text);
        sendRaw(to, w.b);
    }

    bool rateLimited(const std::string& ip) {
        auto it = blocked.find(ip);
        return it != blocked.end() && now() < it->second;
    }

    void onHello(const Addr& from, R& r) {
        const std::string ip = from.ipText();
        if (rateLimited(ip)) return reject(from, REJ_RATE);
        const std::string gameId = clean(r.str(), 8);
        const std::string portVer = clean(r.str(), 32);
        const std::string name = clean(r.str());
        u8 cnIn[16], room[4];
        r.bytes(cnIn, 16);
        const bool direct = r.u8_() == 0;
        r.bytes(room, 4);
        const u32 pid = r.u32_();
        if (!r.ok) return;
        if ((pid && kickedPids.count(pid)) || kickedAddrs.count(from.text())) return reject(from, REJ_KICKED);
        if (gameId != ident.gameId)
            return reject(from, REJ_VERSION, "The host plays " + ident.gameId + ", you play " + gameId +
                                                 ": towns can only visit within one region");
        if (portVer != ident.portVersion)
            return reject(from, REJ_VERSION, "The host runs City Folk Recompiled " + ident.portVersion + ", you run " + portVer +
                                                 ": both need the same version");
        if (direct && !opt.allowDirect) return reject(from, REJ_NEEDCODE);
        if (!direct) {
            u8 mine[4];
            roomIdOf(secret, mine);
            if (std::memcmp(mine, room, 4) != 0) return reject(from, REJ_WRONGROOM);
        }
        for (auto& l : links)
            if (l->addr == from) return;  // already a member (late duplicate)
        if (int(links.size()) + 1 >= kMaxMembers) return reject(from, REJ_FULL);
        PendingJoin* pj = nullptr;
        for (auto& x : pend)
            if (x.addr == from) pj = &x;
        if (!pj) {
            if (pend.size() >= 16) return;  // handshake flood
            pend.push_back({});
            pj = &pend.back();
            pj->addr = from;
            oc::randomBytes(pj->hn, 16);
        }
        if (std::memcmp(pj->cn, cnIn, 16) != 0) {
            std::memcpy(pj->cn, cnIn, 16);
            pj->authed = false;
        }
        pj->name = name;
        pj->gameId = gameId;
        pj->portVersion = portVer;
        pj->direct = direct;
        pj->t = now();
        W w;
        w.b = head(T_CHALLENGE);
        w.bytes(pj->hn, 16).bytes(pj->cn, 16).u8_(hasPw ? 1 : 0).bytes(salt, 16).u32_(kPbkdfIterations).str(roomName);
        sendRaw(from, w.b);
    }

    void onAuth(const Addr& from, R& r) {
        const std::string ip = from.ipText();
        if (rateLimited(ip)) return reject(from, REJ_RATE);
        PendingJoin* pj = nullptr;
        for (auto& x : pend)
            if (x.addr == from) pj = &x;
        if (!pj) return;
        u8 cnIn[16], proof[32];
        r.bytes(cnIn, 16);
        r.bytes(proof, 32);
        if (!r.ok || std::memcmp(cnIn, pj->cn, 16) != 0) return;
        if (!pj->authed) {
            u8 zero[10] = {};
            u8 master[32], want[32];
            masterKey(pj->direct ? zero : secret, hasPw ? pwKey : nullptr, master);
            derived(master, "cf-auth", pj->hn, pj->cn, want);
            if (!oc::equal(want, proof, 32)) {
                auto& f = fails[ip];
                if (now() - f.second > 60) f = {0, now()};
                if (++f.first >= 5) {
                    blocked[ip] = now() + 60;
                    std::printf("[online] %s blocked for 60 s after %d failed joins\n", ip.c_str(), f.first);
                }
                pend.erase(pend.begin() + (pj - pend.data()));
                return reject(from, REJ_AUTH);
            }
            derived(master, "cf-sess", pj->hn, pj->cn, pj->sess);
            pj->authed = true;
            if (opt.askBeforeJoin) {
                pj->ticket = nextTicket++;
                std::printf("[online] %s (%s) asks to join\n", pj->name.c_str(), ip.c_str());
            }
        }
        if (opt.askBeforeJoin && !pj->decided) {
            W w;
            w.b = head(T_WAIT);
            sendRaw(from, w.b);
            return;
        }
        if (opt.askBeforeJoin && !pj->accept) {
            pend.erase(pend.begin() + (pj - pend.data()));
            return reject(from, REJ_DECLINED);
        }
        admit(*pj);
        pend.erase(pend.begin() + (pj - pend.data()));
    }

    void admit(PendingJoin& pj) {
        if (int(links.size()) + 1 >= kMaxMembers) return reject(pj.addr, REJ_FULL);
        auto l = std::make_unique<Link>();
        l->addr = pj.addr;
        for (u8 id = 1; id < kMaxMembers + 3; id++) {
            if (!linkById(id)) {
                l->id = id;
                break;
            }
        }
        l->name = pj.name;
        l->tx = oc::deriveSealKey(pj.sess, "h2c");
        l->rx = oc::deriveSealKey(pj.sess, "c2h");
        l->lastRecv = now();
        Link& ref = *l;
        links.push_back(std::move(l));
        W w;
        w.u8_(ref.id).str(roomName).u8_(hasPw ? 1 : 0);
        queueRel(ref, R_WELCOME, w.b);
        events.push_back({true, ref.id});
        std::printf("[online] %s joined as member %u from %s\n", ref.name.c_str(), ref.id, ref.addr.text().c_str());
        broadcastMembers();
    }

    // Member: handshake replies.
    void onChallenge(const Addr& from, R& r) {
        if (role != Role::Member || step != JoinStep::Hello) return;
        u8 hnIn[16], cnEcho[16], saltIn[16];
        r.bytes(hnIn, 16);
        r.bytes(cnEcho, 16);
        const bool needPw = r.u8_() != 0;
        r.bytes(saltIn, 16);
        const u32 iters = r.u32_();
        roomName = clean(r.str());
        if (!r.ok || std::memcmp(cnEcho, cn, 16) != 0) return;
        if (needPw && joinPassword.empty()) {
            st = State::Failed;
            msg = "This room has a password: enter it and join again";
            stopSockets();
            return;
        }
        if (iters > 1000000) return;
        hostAddr = from;
        std::memcpy(hn, hnIn, 16);
        u8 pk[32], master[32], proof[32], sess[32];
        if (needPw) oc::pbkdf2Sha256(joinPassword.data(), joinPassword.size(), saltIn, 16, iters, pk, 32);
        u8 zero[10] = {};
        masterKey(joinDirect ? zero : secret, needPw ? pk : nullptr, master);
        derived(master, "cf-auth", hn, cn, proof);
        derived(master, "cf-sess", hn, cn, sess);
        hostLink = std::make_unique<Link>();
        hostLink->addr = from;
        hostLink->id = 0;
        hostLink->tx = oc::deriveSealKey(sess, "c2h");
        hostLink->rx = oc::deriveSealKey(sess, "h2c");
        hostLink->lastRecv = now();
        W w;
        w.b = head(T_AUTH);
        w.bytes(cn, 16).bytes(proof, 32);
        authPkt = w.b;
        step = JoinStep::Auth;
        sendRaw(hostAddr, authPkt);
        lastSend = now();
    }

    // ---- loop ----
    void onDatagram(const Addr& fromRaw, const u8* p, size_t n) {
        if (n < 4 || n > kMaxDatagram) return;
        const Addr from = fromRaw.normalized();
        // STUN answer (binding success response, magic cookie).
        if (n >= 20 && p[0] == 0x01 && p[1] == 0x01 && p[4] == 0x21 && p[5] == 0x12 && p[6] == 0xA4 && p[7] == 0x42) {
            Addr mapped;
            if (nat::stunParse(p, n, stunTx, &mapped)) {
                publicAddr = mapped.normalized();
                std::printf("[online] STUN: public address %s\n", publicAddr.text().c_str());
                rebuildCode();
            }
            return;
        }
        if (p[0] != 'C' || p[1] != 'F' || p[2] != kProto) return;
        R r(p + 4, n - 4);
        switch (p[3]) {
        case T_HELLO:
            if (role == Role::Host) onHello(from, r);
            break;
        case T_AUTH:
            if (role == Role::Host) onAuth(from, r);
            break;
        case T_CHALLENGE:
            onChallenge(from, r);
            break;
        case T_WAIT:
            if (role == Role::Member && step == JoinStep::Auth && from == hostAddr) {
                step = JoinStep::Wait;
                st = State::WaitingForHost;
                msg = "Waiting for the host to let you in...";
            }
            break;
        case T_REJECT:
            if (role == Role::Member && step != JoinStep::Done) {
                const u8 why = r.u8_();
                const std::string text = clean(r.str(), 160);
                st = State::Failed;
                msg = text.empty() ? rejectText(why) : text;
                std::printf("[online] join refused: %s\n", msg.c_str());
                stopSockets();
            }
            break;
        case T_SEALED:
            onSealed(from, p, n);
            break;
        case T_PROBE:
            if (role == Role::Host && n >= 12 && std::memcmp(p + 4, probeTok, 8) == 0 && !probeOk) {
                probeOk = true;
                reach = "Reachable from the internet at " + pubEp + " (checked through the router)";
                std::printf("[online] reachability probe came back through %s\n", pubEp.c_str());
            }
            break;
        }
    }

    void onLanDatagram(const Addr& from, const u8* p, size_t n) {
        if (n < 4 || p[0] != 'C' || p[1] != 'F' || p[2] != kProto || p[3] != T_BEACON) return;
        R r(p + 4, n - 4);
        LanRoom lr;
        lr.gameId = clean(r.str(), 8);
        lr.portVersion = clean(r.str());
        lr.roomName = clean(r.str());
        lr.hostName = clean(r.str());
        const u8 flags = r.u8_();
        lr.members = r.u8_();
        lr.maxMembers = r.u8_();
        const u16 port = r.u16_();
        r.bytes(lr.secret, 10);
        if (!r.ok) return;
        lr.hasPassword = flags & 1;
        lr.compatible = lr.gameId == ident.gameId;
        lr.address = from.ipText() + ":" + std::to_string(port);
        heard[lr.address] = {lr, now()};
    }

    void sendBeacon() {
        if (lan == kInvalid) return;
        W w;
        w.b = head(T_BEACON);
        w.str(ident.gameId).str(ident.portVersion).str(roomName).str(ident.name);
        w.u8_(hasPw ? 1 : 0).u8_(u8(links.size() + 1)).u8_(kMaxMembers).u16_(port).bytes(secret, 10);
        const Addr bc = makeV4(0xFFFFFFFFu, kLanPort);
        sendto(lan, reinterpret_cast<const char*>(w.b.data()), int(w.b.size()), 0, reinterpret_cast<const sockaddr*>(&bc.ss), bc.len);
        if (sTestMode) {
            const Addr lo = makeV4(0x7F000001u, kLanPort);
            sendto(lan, reinterpret_cast<const char*>(w.b.data()), int(w.b.size()), 0, reinterpret_cast<const sockaddr*>(&lo.ss), lo.len);
        }
    }

    void rebuildCode() {
        std::vector<Endpoint> eps;
        auto addEp = [&](const Addr& a) {
            if (!a.valid()) return;
            Endpoint e;
            const Addr nrm = a.normalized();
            if (nrm.family() == AF_INET) {
                e.family = 4;
                const u32 v = nrm.v4();
                e.addr[0] = u8(v >> 24);
                e.addr[1] = u8(v >> 16);
                e.addr[2] = u8(v >> 8);
                e.addr[3] = u8(v);
            } else {
                e.family = 6;
                std::memcpy(e.addr, &reinterpret_cast<const sockaddr_in6*>(&nrm.ss)->sin6_addr, 16);
            }
            e.port = nrm.port();
            for (auto& o : eps)
                if (o.family == e.family && std::memcmp(o.addr, e.addr, 16) == 0 && o.port == e.port) return;
            eps.push_back(e);
        };
        if (sTestMode) {
            addEp(makeV4(0x7F000001u, port));
        } else {
            if (mapped && !pubEp.empty()) {
                Addr a;
                if (resolve(pubEp, port, &a)) addEp(a);
            }
            if (publicAddr.valid()) addEp(mapped ? makeV4(publicAddr.v4(), mappedPort ? mappedPort : port) : publicAddr);
            if (const u32 lanIp = nat::defaultLanV4()) addEp(makeV4(lanIp, port));
            if (sfam == AF_INET6)
                for (auto& la : nat::localAddresses())
                    if (la.v6 && la.global) {
                        addEp(makeV6(la.v6bytes, port));
                        break;
                    }
        }
        codeEps = eps;
        code = encodeCode(secret, eps);
    }

    void stopSockets() {
        if (s != kInvalid) closeSocket(s);
        s = kInvalid;
        if (lan != kInvalid) closeSocket(lan);
        lan = kInvalid;
    }

    bool openSocket(u16 bindPort, std::string* err) {
        startup();
        s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        sfam = AF_INET6;
        if (s != kInvalid) {
            int off = 0;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
            sockaddr_in6 a{};
            a.sin6_family = AF_INET6;
            a.sin6_addr = in6addr_any;
            a.sin6_port = htons(bindPort);
            if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
                closeSocket(s);
                s = kInvalid;
            }
        }
        if (s == kInvalid) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sfam = AF_INET;
            if (s == kInvalid) {
                if (err) *err = "Could not create a network socket";
                return false;
            }
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(bindPort);
            if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
                if (err) *err = "UDP port " + std::to_string(bindPort) + " is in use (another program or game instance?)";
                closeSocket(s);
                s = kInvalid;
                return false;
            }
        }
        setNonBlocking(s);
#if defined(_WIN32)
        // An ICMP port-unreachable must not make recvfrom fail on this socket (WSAECONNRESET).
        BOOL no = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &no, sizeof no, nullptr, 0, &ret, nullptr, nullptr);
#endif
        sockaddr_storage b{};
        socklen_t bl = sizeof b;
        getsockname(s, reinterpret_cast<sockaddr*>(&b), &bl);
        Addr ba;
        std::memcpy(&ba.ss, &b, bl);
        ba.len = bl;
        port = ba.port();
        return true;
    }

    void openLanSender() {
        lan = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (lan == kInvalid) return;
        int one = 1;
        setsockopt(lan, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
        setNonBlocking(lan);
    }

    void openLanScanner() {
        lanScanSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (lanScanSock == kInvalid) return;
        int one = 1;
        setsockopt(lanScanSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(lanScanSock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof one);
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(kLanPort);
        if (bind(lanScanSock, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            closeSocket(lanScanSock);
            lanScanSock = kInvalid;
            return;
        }
        setNonBlocking(lanScanSock);
    }

    void tick() {
        const double t = now();
        if (role == Role::Member && st != State::Failed) {
            if (step == JoinStep::Hello && t - lastSend > 0.5) {
                if (t - joinStart > (sTestMode ? 4.0 : 10.0)) {
                    st = State::Failed;
                    msg = "No answer from the host. Check the code; if the host is behind a router without UPnP, "
                          "they need a port forward (UDP " + std::to_string(kDefaultPort) + ") or a VPN such as Tailscale/ZeroTier";
                    stopSockets();
                    return;
                }
                W w;
                w.b = head(T_HELLO);
                w.str(ident.gameId).str(ident.portVersion).str(ident.name).bytes(cn, 16).u8_(joinDirect ? 0 : 1);
                u8 room[4] = {};
                if (!joinDirect) roomIdOf(secret, room);
                w.bytes(room, 4).u32_(ident.pid);
                for (auto& a : tryAddrs) sendRaw(a, w.b);
                lastSend = t;
            } else if ((step == JoinStep::Auth && t - lastSend > 1.0) || (step == JoinStep::Wait && t - lastSend > 2.0)) {
                sendRaw(hostAddr, authPkt);
                lastSend = t;
                if (step == JoinStep::Auth && t - joinStart > 20) {
                    st = State::Failed;
                    msg = "The host stopped answering";
                    stopSockets();
                }
            }
        }
        auto linkTimers = [&](Link& l) {
            if (t - l.lastPing > 1.0) {
                W w;
                w.u8_(B_PING).u32_(u32(t * 1000));
                sendSealed(l, myId(), w.b);
                l.lastPing = t;
            }
            pumpLink(l);
            flushAcks(l);
            if (t - l.lastRecv > (sTestMode ? 5.0 : 15.0)) l.dead = true;
        };
        if (role == Role::Host) {
            for (size_t i = 0; i < links.size();) {
                linkTimers(*links[i]);
                if (links[i]->dead) {
                    dropLink(i, nullptr);
                    continue;
                }
                i++;
            }
            pend.erase(std::remove_if(pend.begin(), pend.end(), [&](const PendingJoin& p) { return t - p.t > 30; }), pend.end());
            if (opt.lanVisible && t - lastBeacon > 2.0) {
                sendBeacon();
                lastBeacon = t;
            }
            if (t - lastMembers > 5.0 && !links.empty()) broadcastMembers();
            // STUN: the public address of this socket.
            if (opt.portMapping && !sTestMode && stunAddr.valid() && !publicAddr.valid() && stunTries < 4 && t - stunSent > 1.5) {
                const auto req = nat::stunRequest(stunTx);
                sendRaw(stunAddr, req);
                stunSent = t;
                stunTries++;
            }
            // Reachability probe through the router's public address (hairpin).
            if (mapped && !probeOk && probes < 5 && t - probeSent > 1.0 && !pubEp.empty()) {
                Addr pa;
                if (resolve(pubEp, port, &pa)) {
                    W w;
                    w.b = head(T_PROBE);
                    w.bytes(probeTok, 8);
                    sendRaw(pa, w.b);
                }
                probeSent = t;
                if (++probes == 5)
                    reach = "Port " + std::to_string(mappedPort) + " mapped on the router (" + pubEp +
                            "); the router did not loop a test packet back, so this could not be checked from here";
            }
        } else if (hostLink) {
            linkTimers(*hostLink);
            if (hostLink->dead && st == State::Joined) {
                st = State::Failed;
                if (msg.rfind("Joined", 0) == 0 || msg.empty()) msg = "Lost the connection to the host";
                for (auto& o : roster)
                    if (!o.self) events.push_back({false, o.id});
                roster.clear();
                stopSockets();
            }
        }
        for (auto it = heard.begin(); it != heard.end();)
            it = t - it->second.t > 6 ? heard.erase(it) : std::next(it);
    }

    void loop() {
        std::vector<u8> buf(2048);
        while (run) {
            fd_set fds;
            FD_ZERO(&fds);
            Socket maxS = 0;
            {
                std::lock_guard<std::recursive_mutex> g(m);
                if (s != kInvalid) {
                    FD_SET(s, &fds);
                    maxS = std::max(maxS, s);
                }
                if (lanScanSock != kInvalid) {
                    FD_SET(lanScanSock, &fds);
                    maxS = std::max(maxS, lanScanSock);
                }
            }
            timeval tv{0, 10000};
            select(int(maxS + 1), &fds, nullptr, nullptr, &tv);
            std::lock_guard<std::recursive_mutex> g(m);
            for (int k = 0; k < 64 && s != kInvalid; k++) {
                sockaddr_storage from{};
                socklen_t fl = sizeof from;
                const int n = int(recvfrom(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl));
                if (n <= 0) break;
                Addr a;
                std::memcpy(&a.ss, &from, fl);
                a.len = fl;
                onDatagram(a, buf.data(), size_t(n));
            }
            for (int k = 0; k < 16 && lanScanSock != kInvalid; k++) {
                sockaddr_storage from{};
                socklen_t fl = sizeof from;
                const int n = int(recvfrom(lanScanSock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl));
                if (n <= 0) break;
                Addr a;
                std::memcpy(&a.ss, &from, fl);
                a.len = fl;
                onLanDatagram(a.normalized(), buf.data(), size_t(n));
            }
            tick();
        }
    }

    void ensureThread() {
        if (run) return;
        run = true;
        th = std::thread([this] { loop(); });
    }

    void stopThread() {
        run = false;
        if (th.joinable()) th.join();
    }

    void resetRoom() {
        gen++;
        kickedPids.clear();
        kickedAddrs.clear();
        links.clear();
        pend.clear();
        hostLink.reset();
        roster.clear();
        inbox.clear();
        code.clear();
        roomName.clear();
        codeEps.clear();
        lanEp.clear();
        pubEp.clear();
        reach.clear();
        publicAddr = {};
        stunAddr = {};
        stunTries = 0;
        mapped = false;
        probeOk = false;
        probes = 0;
        mappedPort = 0;
        hasPw = false;
        selfId = 0;
        step = JoinStep::Hello;
    }

    bool startJoin(const std::vector<Addr>& addrs, bool direct, const std::string& password, std::string* err) {
        leaveLocked();
        resetRoom();
        if (!openSocket(0, err)) return false;
        role = Role::Member;
        st = State::Connecting;
        msg = "Connecting...";
        tryAddrs = addrs;
        joinDirect = direct;
        joinPassword = password;
        oc::randomBytes(cn, 16);
        joinStart = now();
        lastSend = -10;
        ensureThread();
        return true;
    }

    void leaveLocked() {
        if (role == Role::Host) {
            for (auto& l : links) {
                W w;
                w.u8_(B_BYE).str("The host closed the room");
                for (int i = 0; i < 2; i++) sendSealed(*l, 0, w.b);
                events.push_back({false, l->id});
            }
            if (mappedPort) {
                const u16 mp = mappedPort;
                std::thread([mp] { nat::unmapPort(mp); }).detach();
            }
        } else if (role == Role::Member && hostLink && st == State::Joined) {
            W w;
            w.u8_(B_BYE).str("left");
            sendSealed(*hostLink, selfId, w.b);
            for (auto& o : roster)
                if (!o.self) events.push_back({false, o.id});
        }
        stopSockets();
        role = Role::None;
        st = State::Off;
        resetRoom();
    }
};

Session::Session() : d_(new Impl) {}
Session::~Session() {
    {
        std::lock_guard<std::recursive_mutex> g(d_->m);
        d_->leaveLocked();
        if (d_->lanScanSock != kInvalid) closeSocket(d_->lanScanSock);
        d_->lanScanSock = kInvalid;
    }
    d_->stopThread();
    delete d_;
}

void Session::setIdentity(const Identity& id) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    const bool changed = id.pid != d_->ident.pid || id.friendKey != d_->ident.friendKey || id.name != d_->ident.name;
    d_->ident = id;
    if (!changed) return;
    if (d_->role == Impl::Role::Member) d_->sendProfile();
    if (d_->role == Impl::Role::Host) d_->broadcastMembers();
}

bool Session::host(const HostOptions& o, std::string* err) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    Impl& d = *d_;
    d.leaveLocked();
    d.resetRoom();
    d.opt = o;
    if (!d.openSocket(o.port, err)) return false;
    d.role = Impl::Role::Host;
    d.st = State::Hosting;
    d.roomName = clean(o.roomName.empty() ? d.ident.name + "'s town" : o.roomName);
    oc::randomBytes(d.secret, 10);
    oc::randomBytes(d.salt, 16);
    oc::randomBytes(d.probeTok, 8);
    oc::randomBytes(d.stunTx, 12);
    d.hasPw = !o.password.empty();
    if (d.hasPw) oc::pbkdf2Sha256(o.password.data(), o.password.size(), d.salt, 16, kPbkdfIterations, d.pwKey, 32);
    if (o.lanVisible) d.openLanSender();
    if (const u32 lanIp = sTestMode ? 0x7F000001u : nat::defaultLanV4()) d.lanEp = makeV4(lanIp, d.port).text();
    d.rebuildCode();
    d.msg = "Hosting on UDP port " + std::to_string(d.port);
    d.reach = o.portMapping && !sTestMode ? "Checking the router (UPnP / NAT-PMP)..." : "LAN only (port mapping off)";
    std::printf("[online] hosting room \"%s\" on UDP %u%s\n", d.roomName.c_str(), d.port, d.hasPw ? " (password)" : "");
    if (o.portMapping && !sTestMode) {
        const u16 p = d.port;
        const u32 lanIp = nat::defaultLanV4();
        Impl* self = d_;
        const u32 gen = d.gen;
        std::thread([self, p, lanIp, gen] {
            Addr stun;
            for (auto& srv : nat::stunServers())
                if (resolve(srv, 3478, &stun)) break;
            const nat::MapResult mr = nat::mapPort(p, lanIp);
            std::lock_guard<std::recursive_mutex> g2(self->m);
            if (self->gen != gen || self->role != Impl::Role::Host || self->port != p) return;
            self->stunAddr = stun;
            if (mr.ok) {
                self->mapped = true;
                self->mappedPort = mr.externalPort ? mr.externalPort : p;
                self->pubEp = mr.externalIp + ":" + std::to_string(self->mappedPort);
                self->reach = "Port mapped by " + mr.method + " (" + self->pubEp + "), checking...";
                std::printf("[online] %s mapped UDP %u -> %s\n", mr.method.c_str(), p, self->pubEp.c_str());
            } else {
                self->reach = "Not reachable from the internet: " + mr.error +
                              ". Friends on your LAN can join; others need a port forward (UDP " + std::to_string(p) +
                              ") or a VPN (Tailscale, ZeroTier, Hamachi) and Direct connect";
                std::printf("[online] port mapping failed: %s\n", mr.error.c_str());
            }
            self->rebuildCode();
        }).detach();
    }
    d.ensureThread();
    return true;
}

bool Session::joinCode(const std::string& code, const std::string& password, std::string* err) {
    u8 secret[10];
    std::vector<Endpoint> eps;
    if (!decodeCode(code, secret, eps) || eps.empty()) {
        if (err) *err = "That room code is not valid (typo?)";
        return false;
    }
    std::vector<Addr> addrs;
    for (auto& e : eps) addrs.push_back(e.family == 4 ? makeV4(u32(e.addr[0]) << 24 | u32(e.addr[1]) << 16 | u32(e.addr[2]) << 8 | e.addr[3], e.port)
                                                      : makeV6(e.addr, e.port));
    std::lock_guard<std::recursive_mutex> g(d_->m);
    if (!d_->startJoin(addrs, false, password, err)) return false;
    std::memcpy(d_->secret, secret, 10);
    std::printf("[online] joining with a room code (%zu endpoints)\n", addrs.size());
    return true;
}

bool Session::joinAddress(const std::string& hostPort, const std::string& password, std::string* err) {
    Addr a;
    if (!resolve(hostPort, kDefaultPort, &a)) {
        if (err) *err = "Could not find " + hostPort;
        return false;
    }
    std::lock_guard<std::recursive_mutex> g(d_->m);
    if (!d_->startJoin({a.normalized()}, true, password, err)) return false;
    std::memset(d_->secret, 0, 10);
    std::printf("[online] direct connect to %s\n", a.text().c_str());
    return true;
}

bool Session::joinLan(const LanRoom& room, const std::string& password, std::string* err) {
    Addr a;
    if (!resolve(room.address, kDefaultPort, &a)) return false;
    std::lock_guard<std::recursive_mutex> g(d_->m);
    if (!d_->startJoin({a.normalized()}, false, password, err)) return false;
    std::memcpy(d_->secret, room.secret, 10);
    return true;
}

void Session::leave() {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    d_->leaveLocked();
}

void Session::kick(u8 id) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    Impl& d = *d_;
    if (d.role != Impl::Role::Host) return;
    for (size_t i = 0; i < d.links.size(); i++)
        if (d.links[i]->id == id) {
            if (d.links[i]->pid) d.kickedPids.insert(d.links[i]->pid);
            d.kickedAddrs.insert(d.links[i]->addr.text());
            d.queueRel(*d.links[i], R_KICK, {});
            d.flushAcks(*d.links[i]);
            d.dropLink(i, rejectText(REJ_KICKED));
            return;
        }
}

void Session::answerJoinRequest(u32 ticket, bool accept) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    for (auto& p : d_->pend)
        if (p.authed && p.ticket == ticket) {
            p.decided = true;
            p.accept = accept;
            p.t = now();
            if (accept) {
                PendingJoin copy = p;
                d_->pend.erase(d_->pend.begin() + (&p - d_->pend.data()));
                d_->admit(copy);
            } else {
                d_->reject(p.addr, REJ_DECLINED);
                d_->pend.erase(d_->pend.begin() + (&p - d_->pend.data()));
            }
            return;
        }
}

Snapshot Session::snapshot() {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    Impl& d = *d_;
    Snapshot s;
    s.state = d.st;
    s.message = d.msg;
    s.code = d.code;
    s.roomName = d.roomName;
    s.hasPassword = d.hasPw;
    s.selfId = d.myId();
    s.members = members();
    for (auto& p : d.pend)
        if (p.authed && d.opt.askBeforeJoin && !p.decided) s.pending.push_back({p.ticket, p.name, p.addr.text()});
    s.lanEndpoint = d.lanEp;
    s.publicEndpoint = d.pubEp.empty() && d.publicAddr.valid() ? d.publicAddr.text() : d.pubEp;
    s.reachability = d.reach;
    return s;
}

bool Session::active() {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    return d_->st == State::Hosting || d_->st == State::Joined;
}

u8 Session::selfId() {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    return d_->myId();
}

std::vector<Member> Session::members() {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    Impl& d = *d_;
    std::vector<Member> out;
    if (d.role == Impl::Role::Host) {
        Member me;
        me.id = 0;
        me.name = d.ident.name;
        me.pid = d.ident.pid;
        me.friendKey = d.ident.friendKey;
        me.host = me.self = true;
        out.push_back(me);
        for (auto& l : d.links) {
            Member m;
            m.id = l->id;
            m.name = l->name;
            m.pid = l->pid;
            m.friendKey = l->fkey;
            m.rttMs = l->rtt;
            m.address = l->addr.text();
            out.push_back(m);
        }
    } else if (d.st == State::Joined) {
        out = d.roster;
    }
    return out;
}

void Session::lanScan(bool on) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    Impl& d = *d_;
    if (on == d.scanning) return;
    d.scanning = on;
    if (on) {
        startup();
        d.openLanScanner();
        d.ensureThread();
    } else {
        if (d.lanScanSock != kInvalid) closeSocket(d.lanScanSock);
        d.lanScanSock = kInvalid;
        d.heard.clear();
    }
}

std::vector<LanRoom> Session::lanRooms() {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    std::vector<LanRoom> out;
    for (auto& [k, h] : d_->heard) {
        // Our own room is not in the list.
        if (d_->role == Impl::Role::Host && std::memcmp(h.room.secret, d_->secret, 10) == 0) continue;
        out.push_back(h.room);
    }
    return out;
}

bool Session::sendApp(u8 to, u8 type, const void* data, size_t len, bool reliable) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    Impl& d = *d_;
    if (len > (reliable ? kMaxAppMessage : kMaxUnreliable)) return false;
    const u8* p = static_cast<const u8*>(data);
    if (d.role == Impl::Role::Host) {
        if (to == kBroadcast) {
            for (auto& l : d.links)
                if (!l->dead) d.sendAppFrames(*l, 0, to, type, p, len, reliable);
            return true;
        }
        Link* l = d.linkById(to);
        if (!l) return false;
        d.sendAppFrames(*l, 0, to, type, p, len, reliable);
        return true;
    }
    if (d.role == Impl::Role::Member && d.hostLink && d.st == State::Joined) {
        d.sendAppFrames(*d.hostLink, d.selfId, to, type, p, len, reliable);
        return true;
    }
    return false;
}

bool Session::pollApp(AppMsg& out, u8 lo, u8 hi) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    for (auto it = d_->inbox.begin(); it != d_->inbox.end(); ++it)
        if (it->type >= lo && it->type <= hi) {
            out = std::move(*it);
            d_->inbox.erase(it);
            return true;
        }
    return false;
}

size_t Session::queuedBytes(u8 to) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    Impl& d = *d_;
    if (d.role == Impl::Role::Host) {
        Link* l = d.linkById(to);
        return l ? l->queued : 0;
    }
    return d.hostLink ? d.hostLink->queued : 0;
}

bool Session::pollMemberEvent(MemberEvent& out) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    if (d_->events.empty()) return false;
    out = d_->events.front();
    d_->events.pop_front();
    return true;
}

Session& session() {
    static Session* s = new Session;  // never destroyed: the game may still be online at exit
    return *s;
}

void setTestMode(bool on) { sTestMode = on; }

// ---- room codes ----
namespace {
const char* kB32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
int b32val(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; i++)
        if (kB32[i] == c) return i;
    return -1;
}
}  // namespace

std::string encodeCode(const u8 secret[10], const std::vector<Endpoint>& eps) {
    std::vector<u8> b;
    b.push_back(u8(0x10 | std::min<size_t>(eps.size(), 4)));
    b.insert(b.end(), secret, secret + 10);
    for (size_t i = 0; i < eps.size() && i < 4; i++) {
        const auto& e = eps[i];
        b.push_back(e.family);
        b.insert(b.end(), e.addr, e.addr + (e.family == 4 ? 4 : 16));
        b.push_back(u8(e.port >> 8));
        b.push_back(u8(e.port));
    }
    u8 h[32];
    oc::sha256(b.data(), b.size(), h);
    b.push_back(h[0]);
    std::string out;
    u32 acc = 0;
    int bits = 0;
    for (u8 v : b) {
        acc = acc << 8 | v;
        bits += 8;
        while (bits >= 5) {
            out += kB32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits) out += kB32[(acc << (5 - bits)) & 31];
    std::string grouped;
    for (size_t i = 0; i < out.size(); i++) {
        if (i && i % 5 == 0) grouped += '-';
        grouped += out[i];
    }
    return grouped;
}

bool decodeCode(const std::string& code, u8 secret[10], std::vector<Endpoint>& eps) {
    std::vector<u8> b;
    u32 acc = 0;
    int bits = 0;
    for (char c : code) {
        if (c == '-' || c == ' ') continue;
        const int v = b32val(c);
        if (v < 0) return false;
        acc = acc << 5 | u32(v);
        bits += 5;
        if (bits >= 8) {
            b.push_back(u8(acc >> (bits - 8)));
            bits -= 8;
        }
    }
    if (b.size() < 12) return false;
    const u8 check = b.back();
    b.pop_back();
    u8 h[32];
    // Trailing bits may have produced one padding byte too many: try both lengths.
    oc::sha256(b.data(), b.size(), h);
    if (h[0] != check) {
        if (b.size() < 13) return false;
        const u8 c2 = b.back();
        b.pop_back();
        oc::sha256(b.data(), b.size(), h);
        if (h[0] != c2) return false;
    }
    if ((b[0] >> 4) != 1) return false;
    const int n = b[0] & 15;
    std::memcpy(secret, b.data() + 1, 10);
    size_t i = 11;
    eps.clear();
    for (int k = 0; k < n; k++) {
        if (i >= b.size()) return false;
        Endpoint e;
        e.family = b[i++];
        const size_t al = e.family == 4 ? 4 : e.family == 6 ? 16 : 0;
        if (!al || i + al + 2 > b.size()) return false;
        std::memcpy(e.addr, b.data() + i, al);
        i += al;
        e.port = u16(b[i] << 8 | b[i + 1]);
        i += 2;
        eps.push_back(e);
    }
    return true;
}

std::string endpointText(const Endpoint& e) {
    if (e.family == 4) return makeV4(u32(e.addr[0]) << 24 | u32(e.addr[1]) << 16 | u32(e.addr[2]) << 8 | e.addr[3], e.port).text();
    return makeV6(e.addr, e.port).text();
}

}  // namespace online::net
