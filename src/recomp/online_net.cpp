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
// master = HMAC(room secret, "cf-room" | PBKDF2(password, salt)); both sides add an ephemeral
// X25519 key to HELLO / CHALLENGE: k = HMAC(master, "cf-k2" | X25519 shared secret); proof and
// session key = HMAC(k, label | nonces | both ephemeral keys); one seal key per direction. Whoever
// carries the handshake (a lobby, a relay) cannot derive the key or test password guesses offline.
// The host signs the handshake with its Ed25519 key; a joiner signs it for "friends only" rooms.
//
// Lobby rooms (docs/ONLINE.md "The lobby"): the handshake travels through a Signaling object
// (addresses 100:2::<peer>), with each side's UDP candidates sealed inside AUTH / ACCEPT; then
// both sides send sealed pings to every candidate and use the first path whose pong came back
// (direct before relayed). The joiner allocates a TURN relay when nothing direct opens. A joiner
// that gives up before it is in the room (no path, cancelled) says so through the lobby (GONE),
// so the host frees its place at once.
#include "recomp/online_net.h"

#include "recomp/online_crypto.h"
#include "recomp/online_ed25519.h"
#include "recomp/online_nat.h"
#include "recomp/online_turn.h"

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

constexpr u8 kProto = 2;
constexpr u32 kPbkdfIterations = 100000;
constexpr size_t kMaxDatagram = 1400;
constexpr size_t kFragment = 1100;
constexpr size_t kWindow = 64;

enum : u8 {
    T_HELLO = 1, T_CHALLENGE = 2, T_AUTH = 3, T_REJECT = 4, T_WAIT = 5, T_ACCEPT = 6, T_CAND = 7, T_CANDOK = 8, T_GONE = 9,
    T_SEALED = 0x10, T_BEACON = 0x21, T_PROBE = 0x22, T_PUNCH = 0x23,
};
enum : u8 { B_PING = 1, B_PONG = 2, B_ACK = 3, B_REL = 4, B_UNREL = 5, B_BYE = 6 };
enum : u8 { R_WELCOME = 1, R_MEMBERS = 2, R_PROFILE = 3, R_KICK = 4, R_APP = 0x10 };
enum : u8 {
    REJ_VERSION = 1, REJ_FULL = 2, REJ_AUTH = 3, REJ_RATE = 4, REJ_KICKED = 5, REJ_DECLINED = 6,
    REJ_NEEDCODE = 7, REJ_CLOSED = 8, REJ_WRONGROOM = 9, REJ_FRIENDS = 10,
};

bool sTestMode = false;
std::atomic<bool> sForceRelay{false};  // (may be switched while sessions run)
std::string sStun[2];  // setStunServers

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

// Handshake keys: he / je = the host's and the joiner's ephemeral X25519 public keys.
void derived(const u8 k[32], const char* label, const u8 hn[16], const u8 cn[16], const u8 he[32], const u8 je[32], u8 out[32]) {
    std::vector<u8> m(label, label + std::strlen(label));
    m.insert(m.end(), hn, hn + 16);
    m.insert(m.end(), cn, cn + 16);
    m.insert(m.end(), he, he + 32);
    m.insert(m.end(), je, je + 32);
    oc::hmacSha256(k, 32, m.data(), m.size(), out);
}

void ephemeral(u8 priv[32], u8 pub[32]) {
    oc::randomBytes(priv, 32);
    priv[0] &= 248;
    priv[31] = u8((priv[31] & 127) | 64);
    oc::x25519(priv, nullptr, pub);
}

// k = HMAC(master, "cf-k2" | X25519(mine, theirs)).
void mixShared(const u8 master[32], const u8 myPriv[32], const u8 theirPub[32], u8 out[32]) {
    u8 m[5 + 32] = {'c', 'f', '-', 'k', '2'};
    oc::x25519(myPriv, theirPub, m + 5);
    oc::hmacSha256(master, 32, m, sizeof m, out);
}

std::vector<u8> transcript(const char* label, const u8 cn[16], const u8 hn[16], const u8 he[32], const u8 je[32]) {
    std::vector<u8> m(label, label + std::strlen(label));
    m.insert(m.end(), cn, cn + 16);
    m.insert(m.end(), hn, hn + 16);
    m.insert(m.end(), he, he + 32);
    m.insert(m.end(), je, je + 32);
    return m;
}

bool allZero(const u8* p, size_t n) {
    u8 v = 0;
    for (size_t i = 0; i < n; i++) v |= p[i];
    return v == 0;
}

// A UDP address the other side may be reached at (lobby rooms).
enum : u8 { C_LAN = 0, C_PUBLIC = 1, C_PREDICTED = 2, C_RELAY = 3 };
struct Cand {
    Addr a;
    u8 kind = C_PUBLIC;
};

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
    case REJ_FRIENDS: return "This room is for the host's friends only";
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
    // Lobby rooms: no address until a path is proved (a pong came back over it).
    bool viaSig = false;
    u32 sigPeer = 0;
    std::vector<Cand> cands;
    double probeStart = 0, lastProbe = -1, lastPathRx = 0;
    bool relayed = false;
    bool everPath = false;      // a path was proved once: silence then counts as on any other link
    std::vector<u8> acceptPkt;  // host: resent when the joiner repeats AUTH
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
    u8 kind = 0;  // 0 direct, 1 room code, 2 lobby
    u8 ePriv[32], ePub[32], je[32], jed[32];
    std::vector<u8> challenge;  // built once per joiner nonce
    std::vector<Cand> cands;
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
    enum class JoinStep { Hello, Auth, Wait, Punch, Done } step = JoinStep::Hello;
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
    // Optional self-hosted relay (docs/ONLINE.md "Relay", tools/relay/cf_relay.cpp): members reached
    // through it have virtual addresses (100::/64 discard prefix, peer id in the last 4 bytes).
    // Hole punching (docs/ONLINE.md "Hole punching"): a joiner that gets no answer shows its public
    // address as a reply code; the host enters it and sends packets there while the joiner keeps
    // sending HELLO, so both routers see outgoing traffic and let the other side in.
    Addr punchTarget;
    double punchUntil = -1, punchSent = -1;
    std::string replyCode;
    Addr relay;
    u8 relayRoom[8] = {};
    double relayRegAt = -100;
    bool relayOk = false;
    // Lobby rooms.
    std::shared_ptr<Signaling> sig;
    std::vector<std::array<u8, 32>> allowed;  // friends-only room: keys that may join
    Addr gateway;                              // tests: NAT simulator
    Addr stunAddr2, publicAddr2;
    u8 stunTx2[12] = {};
    bool stunLookup = false;  // findStun's thread has not delivered the servers' addresses yet
    double stunRound = -100;
    std::unique_ptr<turn::Client> turnc{new turn::Client};
    bool turnAsked = false, turnNone = false, relayCandSent = false;
    double punchStart = 0, challengeAt = 0;
    int helloTries = 0;
    bool authDeferred = false;
    u8 ePriv[32] = {}, ePub[32] = {}, hostEph[32] = {}, sessKey[32] = {};
    u8 expectKey[32] = {}, hostKey[32] = {};
    bool expectKeySet = false, hostKeyKnown = false;
    enum { kSigLobby = 2 };
    bool mapped = false;
    u8 probeTok[8] = {};
    double probeSent = -100;
    int probes = 0;
    bool probeOk = false;
    u16 mappedPort = 0;

    // ---- helpers (lock held) ----
    static bool isVirtual(const Addr& a, u32* peer = nullptr) {
        if (a.family() != AF_INET6) return false;
        const u8* ip = reinterpret_cast<const u8*>(&reinterpret_cast<const sockaddr_in6*>(&a.ss)->sin6_addr);
        if (ip[0] != 0x01 || ip[1] != 0x00) return false;
        for (int i = 2; i < 12; i++)
            if (ip[i]) return false;
        if (peer) *peer = u32(ip[12]) << 24 | u32(ip[13]) << 16 | u32(ip[14]) << 8 | ip[15];
        return true;
    }
    static Addr virtualAddr(u32 peer) {
        u8 ip[16] = {0x01, 0x00};
        ip[12] = u8(peer >> 24);
        ip[13] = u8(peer >> 16);
        ip[14] = u8(peer >> 8);
        ip[15] = u8(peer);
        return makeV6(ip, 1);
    }
    std::vector<u8> relayHead(u8 type) const {
        std::vector<u8> b = {'C', 'F', 'R', 'L', 1, type};
        b.insert(b.end(), relayRoom, relayRoom + 8);
        return b;
    }

    // Signaling peers 100:2::<peer>, peers behind our TURN relay 100:3::<IPv4> port.
    static bool isSig(const Addr& a, u32* peer = nullptr) {
        if (a.family() != AF_INET6) return false;
        const u8* ip = reinterpret_cast<const u8*>(&reinterpret_cast<const sockaddr_in6*>(&a.ss)->sin6_addr);
        if (ip[0] != 0x01 || ip[1] != 0x00 || ip[2] != 0x00 || ip[3] != 0x02) return false;
        if (peer) *peer = u32(ip[12]) << 24 | u32(ip[13]) << 16 | u32(ip[14]) << 8 | ip[15];
        return true;
    }
    static Addr sigAddr(u32 peer) {
        u8 ip[16] = {0x01, 0x00, 0x00, 0x02};
        ip[12] = u8(peer >> 24);
        ip[13] = u8(peer >> 16);
        ip[14] = u8(peer >> 8);
        ip[15] = u8(peer);
        return makeV6(ip, 1);
    }
    static bool isTurn(const Addr& a, Addr* real = nullptr) {
        if (a.family() != AF_INET6) return false;
        const u8* ip = reinterpret_cast<const u8*>(&reinterpret_cast<const sockaddr_in6*>(&a.ss)->sin6_addr);
        if (ip[0] != 0x01 || ip[1] != 0x00 || ip[2] != 0x00 || ip[3] != 0x03) return false;
        if (real) *real = makeV4(u32(ip[12]) << 24 | u32(ip[13]) << 16 | u32(ip[14]) << 8 | ip[15], a.port());
        return true;
    }
    static Addr turnAddr(const Addr& real) {
        u8 ip[16] = {0x01, 0x00, 0x00, 0x03};
        const u32 v = real.v4();
        ip[12] = u8(v >> 24);
        ip[13] = u8(v >> 16);
        ip[14] = u8(v >> 8);
        ip[15] = u8(v);
        return makeV6(ip, real.port());
    }

    // The socket itself (tests: through the NAT simulator, destination in a 6-byte header).
    void rawSend(const Addr& to, const u8* p, size_t n) {
        if (s == kInvalid) return;
        if (gateway.valid()) {
            const u32 v = to.normalized().v4();
            if (!v) return;
            std::vector<u8> w = {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v), u8(to.port() >> 8), u8(to.port())};
            w.insert(w.end(), p, p + n);
            const Addr g = gateway.forSocket(sfam);
            sendto(s, reinterpret_cast<const char*>(w.data()), int(w.size()), 0, reinterpret_cast<const sockaddr*>(&g.ss), g.len);
            return;
        }
        const Addr a = to.forSocket(sfam);
        sendto(s, reinterpret_cast<const char*>(p), int(n), 0, reinterpret_cast<const sockaddr*>(&a.ss), a.len);
    }

    void sendRaw(const Addr& to, const std::vector<u8>& b) {
        u32 peer = 0;
        if (isSig(to, &peer)) {
            if (sig) sig->send(peer, b);
            return;
        }
        if (Addr real; isTurn(to, &real)) {
            if (!turnc->ready()) return;
            const std::vector<u8> w = turnc->wrap(real, b.data(), b.size());
            if (!w.empty()) rawSend(turnc->server(), w.data(), w.size());
            return;
        }
        if (s == kInvalid) return;
        if (isVirtual(to, &peer)) {  // through the relay
            if (!relay.valid()) return;
            std::vector<u8> w = relayHead(role == Role::Host ? 5 : 3);
            if (role == Role::Host)
                for (int i = 3; i >= 0; i--) w.push_back(u8(peer >> (8 * i)));
            w.insert(w.end(), b.begin(), b.end());
            rawSend(relay, w.data(), w.size());
            return;
        }
        rawSend(to, b.data(), b.size());
    }

    void sendSealed(Link& l, u8 senderId, std::vector<u8> body) {
        if (!l.addr.valid()) return;  // lobby room: no path yet (reliable data is sent again later)
        sendSealedTo(l, l.addr, senderId, std::move(body));
    }

    void sendSealedTo(Link& l, const Addr& to, u8 senderId, std::vector<u8> body) {
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
        sendRaw(to, pkt);
    }

    int maxM() const { return std::clamp(opt.maxMembers, 2, kMaxMembers); }

    // ---- lobby rooms: candidates and paths ----
    std::string natText() const {
        if (!publicAddr.valid() || !publicAddr2.valid()) return {};
        if (publicAddr.v4() == publicAddr2.v4() && publicAddr.port() != publicAddr2.port())
            return "symmetric (a different port for every destination): direct connections may need the relay";
        return "ordinary (one public port)";
    }

    // Where the other side may reach this socket: LAN, what the STUN servers saw, predicted next
    // ports of a router that gives each destination a new one, the router mapping, global IPv6.
    std::vector<Cand> myCands() {
        std::vector<Cand> out;
        auto add = [&](const Addr& a, u8 kind) {
            if (!a.valid() || out.size() >= 16) return;
            const Addr n = a.normalized();
            for (auto& c : out)
                if (c.a == n) return;
            out.push_back({n, kind});
        };
        if (gateway.valid()) {
            // behind the NAT simulator the socket's own address must not be offered
        } else if (sTestMode) {
            add(makeV4(0x7F000001u, port), C_LAN);
        } else if (const u32 lanIp = nat::defaultLanV4()) {
            add(makeV4(lanIp, port), C_LAN);
        }
        add(publicAddr, C_PUBLIC);
        add(publicAddr2, C_PUBLIC);
        if (mapped && !pubEp.empty()) {
            Addr a;
            if (resolve(pubEp, port, &a)) add(a, C_PUBLIC);
        }
        if (publicAddr.valid() && publicAddr2.valid() && publicAddr.v4() == publicAddr2.v4()) {
            const int d = int(publicAddr2.port()) - int(publicAddr.port());
            if (d != 0 && d >= -64 && d <= 64)
                for (int k = 1; k <= 6; k++) {
                    const int pp = int(publicAddr2.port()) + d * k;
                    if (pp > 1023 && pp < 65536) add(makeV4(publicAddr2.v4(), u16(pp)), C_PREDICTED);
                }
        }
        if (sfam == AF_INET6 && !sTestMode)
            for (auto& la : nat::localAddresses())
                if (la.v6 && la.global) {
                    add(makeV6(la.v6bytes, port), C_PUBLIC);
                    break;
                }
        return out;
    }

    // Candidates travel sealed under the session key (the lobby sees only opaque bytes).
    static std::vector<u8> sealCands(const u8 sess[32], const char* label, u64 nonce, const std::vector<Cand>& cs) {
        W w;
        w.u8_(u8(cs.size()));
        for (auto& c : cs) {
            const Addr n = c.a.normalized();
            w.u8_(c.kind);
            if (n.family() == AF_INET) {
                w.u8_(4).u32_(n.v4());
            } else {
                w.u8_(6).bytes(&reinterpret_cast<const sockaddr_in6*>(&n.ss)->sin6_addr, 16);
            }
            w.u16_(n.port());
        }
        const oc::SealKey k = oc::deriveSealKey(sess, label);
        u8 tag[oc::kTagSize];
        oc::seal(k, nonce, nullptr, 0, w.b.data(), w.b.size(), tag);
        w.bytes(tag, oc::kTagSize);
        return w.b;
    }
    static bool openCands(const u8 sess[32], const char* label, u64 nonce, const u8* p, size_t n, std::vector<Cand>& out) {
        if (n < 1 + oc::kTagSize || n > 600) return false;
        std::vector<u8> b(p, p + n - oc::kTagSize);
        const oc::SealKey k = oc::deriveSealKey(sess, label);
        if (!oc::open(k, nonce, nullptr, 0, b.data(), b.size(), p + n - oc::kTagSize)) return false;
        R r(b.data(), b.size());
        const u8 cnt = r.u8_();
        for (int i = 0; i < cnt && r.ok && out.size() < 16; i++) {
            Cand c;
            c.kind = r.u8_();
            const u8 fam = r.u8_();
            if (fam == 4) {
                const u32 v = r.u32_();
                c.a = makeV4(v, r.u16_());
            } else if (fam == 6) {
                u8 ip[16];
                r.bytes(ip, 16);
                c.a = makeV6(ip, r.u16_());
            } else {
                return false;
            }
            // Never a virtual, zero or broadcast address from the other side.
            if (!r.ok || c.a.port() == 0 || isVirtual(c.a) || isSig(c.a) || isTurn(c.a)) continue;
            if (c.a.family() == AF_INET && (c.a.v4() == 0 || c.a.v4() == 0xFFFFFFFFu)) continue;
            out.push_back(c);
        }
        return r.ok;
    }

    static void addCand(Link& l, const Addr& a, u8 kind) {
        for (auto& c : l.cands)
            if (c.a == a) {
                if (kind == C_RELAY) c.kind = kind;
                return;
            }
        if (l.cands.size() < 24) l.cands.push_back({a, kind});
    }

    static bool relayPath(const Link& l, const Addr& a) {
        if (isTurn(a) || isVirtual(a)) return true;
        for (auto& c : l.cands)
            if (c.a == a) return c.kind == C_RELAY;
        return false;
    }

    // Sealed pings to every candidate until a path is proved; a relayed link keeps looking for a
    // direct one (rarely).
    void probeLink(Link& l, double t) {
        if (!l.viaSig) return;
        if (l.addr.valid() && t - l.lastPathRx > 4.0) {  // the path died: look again
            Addr real;
            std::printf("[online] path to member %u (%s%s) lost, looking for another\n", l.id,
                        isTurn(l.addr, &real) ? real.text().c_str() : l.addr.text().c_str(), l.relayed ? ", relayed" : "");
            l.addr = Addr{};
            l.relayed = false;
            l.probeStart = t;
        }
        if (l.cands.empty() || (l.addr.valid() && !l.relayed)) return;
        const double age = t - l.probeStart;
        const double every = l.addr.valid() ? 15.0 : age < 10 ? 0.25 : 1.0;
        if (t - l.lastProbe < every) return;
        l.lastProbe = t;
        for (auto& c : l.cands) {
            if (sForceRelay && c.kind != C_RELAY) continue;
            if (c.kind == C_PREDICTED && age < 0.5) continue;
            if (c.a == l.addr) continue;
            W w;
            w.u8_(B_PING).u32_(u32(t * 1000));
            sendSealedTo(l, c.a, myId(), w.b);
        }
    }

    void onPong(Link& l, const Addr& from, u32 sentMs) {
        const int rtt = int(u32(now() * 1000) - sentMs);
        if (!l.viaSig) {
            l.rtt = rtt;
            return;
        }
        const bool rel = relayPath(l, from);
        if (sForceRelay && !rel) return;
        if (!(from == l.addr) && (!l.addr.valid() || (l.relayed && !rel))) {
            const bool first = !l.addr.valid();
            l.addr = from;
            l.relayed = rel;
            l.everPath = true;
            Addr real;
            std::printf("[online] %s path %s member %u: %s%s\n", first ? "connected," : "switched,", role == Role::Host ? "to" : "to the host as",
                        role == Role::Host ? l.id : selfId, isTurn(from, &real) ? real.text().c_str() : from.text().c_str(),
                        rel ? " (relayed)" : " (direct)");
            for (auto& o : l.inflight) {  // waiting reliable data goes out now
                o.sent = -1;
                o.first = now();
            }
            pumpLink(l);
        }
        if (from == l.addr) {
            l.rtt = rtt;
            l.lastPathRx = now();
        }
    }

    Link* linkBySigPeer(u32 peer) {
        for (auto& l : links)
            if (l->viaSig && l->sigPeer == peer && !l->dead) return l.get();
        return nullptr;
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
        if (!l.addr.valid()) return;  // lobby room without a path yet
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
        w.u8_(0).str(ident.name).u32_(ident.pid).u64_(ident.friendKey).u16_(0).u8_(0);
        for (auto& l : links)
            w.u8_(l->id).str(l->name).u32_(l->pid).u64_(l->fkey).u16_(u16(std::max(l->rtt, 0))).u8_(l->relayed ? 1 : 0);
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
        if (isSig(from)) return;  // sealed data never travels through the lobby
        if (role == Role::Host) {
            for (auto& x : links)
                if (x->addr == from) l = x.get();
            if (!l)  // lobby room: a path not proved yet; the sender id picks the keys
                for (auto& x : links)
                    if (x->viaSig && x->id == p[4] && !x->dead) l = x.get();
        } else if (hostLink && (hostLink->addr == from || hostLink->viaSig)) {
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
        // A valid packet from an address nobody listed (a router that picked another port): it
        // becomes a candidate and is pinged with the rest.
        if (l->viaSig && !(from == l->addr)) addCand(*l, from, isTurn(from) ? C_RELAY : C_PUBLIC);
        onBody(*l, from, body.data(), body.size());
    }

    void onBody(Link& l, const Addr& from, const u8* p, size_t n) {
        R r(p, n);
        const u8 kind = r.u8_();
        switch (kind) {
        case B_PING: {
            const u32 t = r.u32_();
            if (l.viaSig && sForceRelay && !relayPath(l, from)) break;
            W w;
            w.u8_(B_PONG).u32_(t);
            sendSealedTo(l, l.viaSig ? from : l.addr, myId(), w.b);  // back the way it came
            break;
        }
        case B_PONG: {
            const u32 t = r.u32_();
            if (r.ok) onPong(l, from, t);
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
                    m.relayed = r.u8_() & 1;
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
        const std::string ip = isSig(from) ? std::string("lobby") : from.ipText();
        if (rateLimited(ip)) return reject(from, REJ_RATE);
        const std::string gameId = clean(r.str(), 8);
        const std::string portVer = clean(r.str(), 32);
        const std::string name = clean(r.str());
        u8 cnIn[16], room[4];
        r.bytes(cnIn, 16);
        const u8 kind = r.u8_();
        const bool direct = kind != 1;
        r.bytes(room, 4);
        const u32 pid = r.u32_();
        u8 je[32], jed[32];
        r.bytes(je, 32);
        r.bytes(jed, 32);
        if (!r.ok) return;
        u32 sigPeer = 0;
        if ((kind == kSigLobby) != isSig(from, &sigPeer)) return;  // lobby joins come through the lobby only
        if (kind == kSigLobby && linkBySigPeer(sigPeer)) return;
        if ((pid && kickedPids.count(pid)) || kickedAddrs.count(from.text())) return reject(from, REJ_KICKED);
        if (gameId != ident.gameId)
            return reject(from, REJ_VERSION, "The host plays " + ident.gameId + ", you play " + gameId +
                                                 ": towns can only visit within one region");
        if (portVer != ident.portVersion)
            return reject(from, REJ_VERSION, "The host runs City Folk Recompiled " + ident.portVersion + ", you run " + portVer +
                                                 ": both need the same version");
        if (kind == 0 && !opt.allowDirect) return reject(from, REJ_NEEDCODE);
        if (!direct) {
            u8 mine[4];
            roomIdOf(secret, mine);
            if (std::memcmp(mine, room, 4) != 0) return reject(from, REJ_WRONGROOM);
        }
        for (auto& l : links)
            if (l->addr == from) return;  // already a member (late duplicate)
        if (int(links.size()) + 1 >= maxM()) return reject(from, REJ_FULL);
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
        if (std::memcmp(pj->cn, cnIn, 16) != 0 || pj->challenge.empty()) {
            std::memcpy(pj->cn, cnIn, 16);
            pj->authed = false;
            std::memcpy(pj->je, je, 32);
            std::memcpy(pj->jed, jed, 32);
            ephemeral(pj->ePriv, pj->ePub);
            W w;
            w.b = head(T_CHALLENGE);
            w.bytes(pj->hn, 16).bytes(pj->cn, 16).u8_(hasPw ? 1 : 0).bytes(salt, 16).u32_(kPbkdfIterations).str(roomName);
            w.bytes(pj->ePub, 32).u8_(opt.friendsOnly ? 1 : 0);
            u8 sigv[64] = {};
            if (ident.haveKey) {
                const auto m = transcript("cf-hs2", pj->cn, pj->hn, pj->ePub, pj->je);
                oc::ed25519Sign(ident.edSeed, m.data(), m.size(), sigv);
                w.bytes(ident.edPub, 32);
            } else {
                const u8 zero[32] = {};
                w.bytes(zero, 32);
            }
            w.bytes(sigv, 64);
            pj->challenge = w.b;
        }
        pj->kind = kind;
        pj->name = name;
        pj->gameId = gameId;
        pj->portVersion = portVer;
        pj->direct = direct;
        pj->t = now();
        sendRaw(from, pj->challenge);
    }

    void onAuth(const Addr& from, R& r) {
        u32 sigPeer = 0;
        const bool viaSig = isSig(from, &sigPeer);
        const std::string ip = viaSig ? std::string("lobby") : from.ipText();
        if (rateLimited(ip)) return reject(from, REJ_RATE);
        PendingJoin* pj = nullptr;
        for (auto& x : pend)
            if (x.addr == from) pj = &x;
        if (!pj) {
            // The joiner did not get our ACCEPT (lost in the lobby): once more.
            if (Link* l = viaSig ? linkBySigPeer(sigPeer) : nullptr; l && !l->acceptPkt.empty()) sendRaw(from, l->acceptPkt);
            return;
        }
        u8 cnIn[16], proof[32], jsig[64];
        r.bytes(cnIn, 16);
        r.bytes(proof, 32);
        r.bytes(jsig, 64);
        if (!r.ok || std::memcmp(cnIn, pj->cn, 16) != 0) return;
        if (!pj->authed) {
            u8 zero[10] = {};
            u8 master[32], k[32], want[32];
            masterKey(pj->direct ? zero : secret, hasPw ? pwKey : nullptr, master);
            mixShared(master, pj->ePriv, pj->je, k);
            derived(k, "cf-auth", pj->hn, pj->cn, pj->ePub, pj->je, want);
            if (!oc::equal(want, proof, 32)) {
                auto& f = fails[ip];
                if (now() - f.second > 60) f = {0, now()};
                // Joins through the lobby all share one sender here, so wrong passwords there never
                // lock the room (anyone could lock others out; user decision). Only an address that
                // connects directly is held off after 5 wrong tries.
                if (!viaSig && ++f.first >= 5) {
                    blocked[ip] = now() + 60;
                    std::printf("[online] %s blocked for 60 s after %d failed joins\n", ip.c_str(), f.first);
                }
                // (a room from the list or by address has no code that could be wrong)
                const bool code = pj->kind == 1;
                std::printf("[online] %s: join refused (wrong password%s)\n", pj->name.c_str(), code ? " or room code" : "");
                pend.erase(pend.begin() + (pj - pend.data()));
                return reject(from, REJ_AUTH, code ? std::string() : std::string("Wrong password"));
            }
            if (opt.friendsOnly) {  // the joiner's key: one of the host's friends, and proved
                bool ok = false;
                for (auto& a : allowed) ok |= std::memcmp(a.data(), pj->jed, 32) == 0;
                const auto m = transcript("cf-hs2j", pj->cn, pj->hn, pj->ePub, pj->je);
                if (!ok || !oc::ed25519Verify(pj->jed, m.data(), m.size(), jsig)) {
                    pend.erase(pend.begin() + (pj - pend.data()));
                    return reject(from, REJ_FRIENDS);
                }
            }
            derived(k, "cf-sess", pj->hn, pj->cn, pj->ePub, pj->je, pj->sess);
            if (viaSig) {
                const u16 cl = r.u16_();
                size_t rest = 0;
                const u8* cp = r.rest(&rest);
                if (!r.ok || cl > rest || !openCands(pj->sess, "cand-j", 1, cp, cl, pj->cands)) return;
            }
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
        if (int(links.size()) + 1 >= maxM()) return reject(pj.addr, REJ_FULL);
        auto l = std::make_unique<Link>();
        u32 sigPeer = 0;
        if (isSig(pj.addr, &sigPeer)) {  // lobby: the UDP path comes from the candidates
            l->viaSig = true;
            l->sigPeer = sigPeer;
            l->cands = pj.cands;
            l->probeStart = now();
        } else {
            l->addr = pj.addr;
        }
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
        if (ref.viaSig) {
            W a;
            a.b = head(T_ACCEPT);
            a.bytes(pj.cn, 16).u8_(ref.id);
            const auto sc = sealCands(pj.sess, "cand-h", 1, myCands());
            a.u16_(u16(sc.size())).bytes(sc.data(), sc.size());
            ref.acceptPkt = a.b;
            sendRaw(pj.addr, ref.acceptPkt);
        }
        W w;
        w.u8_(ref.id).str(roomName).u8_(hasPw ? 1 : 0);
        queueRel(ref, R_WELCOME, w.b);
        events.push_back({true, ref.id});
        std::printf("[online] %s joined as member %u %s\n", ref.name.c_str(), ref.id,
                    ref.viaSig ? "through the lobby" : ("from " + ref.addr.text()).c_str());
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
        u8 he[32], hed[32], hsig[64];
        r.bytes(he, 32);
        const u8 hflags = r.u8_();
        r.bytes(hed, 32);
        r.bytes(hsig, 64);
        (void)hflags;
        if (!r.ok || std::memcmp(cnEcho, cn, 16) != 0) return;
        if (sig && !isSig(from)) return;
        auto fail = [&](const char* why) {
            st = State::Failed;
            msg = why;
            stopSockets();
        };
        hostKeyKnown = false;
        if (!allZero(hed, 32)) {
            const auto m = transcript("cf-hs2", cn, hnIn, he, ePub);
            if (!oc::ed25519Verify(hed, m.data(), m.size(), hsig)) return fail("The host's identity could not be checked (the handshake was changed on the way)");
            std::memcpy(hostKey, hed, 32);
            hostKeyKnown = true;
        }
        if (expectKeySet && (!hostKeyKnown || std::memcmp(hostKey, expectKey, 32) != 0))
            return fail("That room is not hosted by your friend (the host's key does not match)");
        if (needPw && joinPassword.empty()) {
            st = State::Failed;
            msg = "This room has a password: enter it and join again";
            stopSockets();
            return;
        }
        if (iters > 1000000) return;
        hostAddr = from;
        std::memcpy(hn, hnIn, 16);
        u8 pk[32], master[32], k[32], proof[32], sess[32];
        if (needPw) oc::pbkdf2Sha256(joinPassword.data(), joinPassword.size(), saltIn, 16, iters, pk, 32);
        u8 zero[10] = {};
        masterKey(joinDirect ? zero : secret, needPw ? pk : nullptr, master);
        mixShared(master, ePriv, he, k);
        std::memcpy(hostEph, he, 32);
        derived(k, "cf-auth", hn, cn, he, ePub, proof);
        derived(k, "cf-sess", hn, cn, he, ePub, sess);
        std::memcpy(sessKey, sess, 32);
        hostLink = std::make_unique<Link>();
        if (sig) {
            hostLink->viaSig = true;
        } else {
            hostLink->addr = from;
        }
        hostLink->id = 0;
        hostLink->tx = oc::deriveSealKey(sess, "c2h");
        hostLink->rx = oc::deriveSealKey(sess, "h2c");
        hostLink->lastRecv = now();
        W w;
        w.b = head(T_AUTH);
        w.bytes(cn, 16).bytes(proof, 32);
        u8 jsig[64] = {};
        if (ident.haveKey) {
            const auto m = transcript("cf-hs2j", cn, hn, he, ePub);
            oc::ed25519Sign(ident.edSeed, m.data(), m.size(), jsig);
        }
        w.bytes(jsig, 64);
        authPkt = w.b;
        step = JoinStep::Auth;
        challengeAt = now();
        if (sig) {
            authDeferred = true;  // the candidates go in once STUN answered (sigTick)
            return;
        }
        sendRaw(hostAddr, authPkt);
        lastSend = now();
    }

    // Lobby join, AUTH: our candidates sealed behind the proof.
    void sendSigAuth() {
        authDeferred = false;
        const auto sc = sealCands(sessKey, "cand-j", 1, myCands());
        W w;
        w.b = authPkt;
        w.u16_(u16(sc.size())).bytes(sc.data(), sc.size());
        authPkt = w.b;
        sendRaw(hostAddr, authPkt);
        lastSend = now();
    }

    void onAccept(const Addr& from, R& r) {
        if (role != Role::Member || !sig || !isSig(from) || !hostLink || (step != JoinStep::Auth && step != JoinStep::Wait)) return;
        u8 cnEcho[16];
        r.bytes(cnEcho, 16);
        const u8 id = r.u8_();
        const u16 cl = r.u16_();
        size_t rest = 0;
        const u8* cp = r.rest(&rest);
        std::vector<Cand> cs;
        if (!r.ok || std::memcmp(cnEcho, cn, 16) != 0 || cl > rest || !openCands(sessKey, "cand-h", 1, cp, cl, cs)) return;
        selfId = id;
        hostLink->cands = cs;
        hostLink->probeStart = hostLink->lastRecv = punchStart = now();
        step = JoinStep::Punch;
        st = State::Connecting;
        msg = "Opening a connection to the host...";
        std::printf("[online] the host let us in (member %u): opening a path (%zu addresses)\n", id, cs.size());
    }

    // Host: the joiner's relay address, sent when nothing direct opened.
    void onCand(const Addr& from, R& r) {
        u32 peer = 0;
        if (role != Role::Host || !isSig(from, &peer)) return;
        Link* l = linkBySigPeer(peer);
        if (!l) return;
        u8 cnEcho[16];
        r.bytes(cnEcho, 16);
        const u16 cl = r.u16_();
        size_t rest = 0;
        const u8* cp = r.rest(&rest);
        // The link's receive key proves the sender: the candidates are sealed with it.
        std::vector<Cand> cs;
        if (!r.ok || cl > rest || cl < 1 + oc::kTagSize) return;
        {
            std::vector<u8> b(cp, cp + cl - oc::kTagSize);
            if (!oc::open(l->rx, ~u64(0), nullptr, 0, b.data(), b.size(), cp + cl - oc::kTagSize)) return;
            R c(b.data(), b.size());
            const u32 v = c.u32_();
            const u16 pp = c.u16_();
            if (!c.ok || !v || !pp) return;
            addCand(*l, makeV4(v, pp), C_RELAY);
            if (l->addr == makeV4(v, pp)) l->relayed = true;
            std::printf("[online] member %u offers a relay address\n", l->id);
        }
        W w;
        w.b = head(T_CANDOK);
        w.bytes(cnEcho, 16);
        sendRaw(from, w.b);
    }

    // Lobby joiner that gives up before it is in the room (no way through, or cancelled): it has
    // no other way to tell the host, which would keep its place (and list it as a member) until
    // the 30 s for a first path are over. Sealed with the link's key, like the relay address.
    void sendSigGone() {
        if (role != Role::Member || !sig || !hostLink || !hostLink->viaSig || authDeferred) return;
        if (step != JoinStep::Auth && step != JoinStep::Wait && step != JoinStep::Punch) return;
        u8 one[1] = {1}, tag[oc::kTagSize];
        oc::seal(hostLink->tx, ~u64(0) - 1, nullptr, 0, one, 1, tag);
        W w;
        w.b = head(T_GONE);
        w.bytes(cn, 16).bytes(one, 1).bytes(tag, oc::kTagSize);
        sendRaw(sigAddr(0), w.b);
    }

    // Host: that joiner's link, or its request still waiting for the host's answer, goes at once.
    void onGone(const Addr& from, R& r) {
        u32 peer = 0;
        if (role != Role::Host || !isSig(from, &peer)) return;
        u8 cnEcho[16], one[1], tag[oc::kTagSize];
        r.bytes(cnEcho, 16);
        r.bytes(one, 1);
        r.bytes(tag, oc::kTagSize);
        if (!r.ok) return;
        bool known = false;
        if (Link* l = linkBySigPeer(peer)) {
            if (!oc::open(l->rx, ~u64(0) - 1, nullptr, 0, one, 1, tag)) return;
            if (l->leftWhy.empty()) l->leftWhy = "gave up before a connection opened";
            l->dead = true;  // dropped in tick()
            known = true;
        } else {
            for (size_t i = 0; i < pend.size(); i++) {
                PendingJoin& pj = pend[i];
                if (!(pj.addr == from) || !pj.authed || std::memcmp(pj.cn, cnEcho, 16) != 0) continue;
                const oc::SealKey k = oc::deriveSealKey(pj.sess, "c2h");
                if (!oc::open(k, ~u64(0) - 1, nullptr, 0, one, 1, tag)) return;
                std::printf("[online] %s no longer asks to join\n", pj.name.c_str());
                pend.erase(pend.begin() + long(i));
                known = true;
                break;
            }
        }
        if (!known) return;
        W w;
        w.b = head(T_CANDOK);
        w.bytes(cnEcho, 16);
        sendRaw(from, w.b);
    }

    // Member in a lobby join: one signal at a time (each is an HTTPS request with its answer).
    void sigTick(double t) {
        auto fail = [&](const std::string& why) {
            sendSigGone();  // the host frees our place
            st = State::Failed;
            msg = why;
            std::printf("[online] join failed: %s\n", why.c_str());
            std::vector<std::vector<u8>> out;
            if (turnc->started()) turnc->close(out);
            for (auto& o : out) rawSend(turnc->server(), o.data(), o.size());
            stopSockets();
        };
        if (step == JoinStep::Done) return;
        if (const std::string e = sig->error(); !e.empty()) return fail(e);
        // Our public address as two STUN servers see it (two answers tell a symmetric router).
        if ((!publicAddr.valid() || !publicAddr2.valid()) && stunTries < 6 && t - stunSent > 0.4) {
            if (stunAddr.valid() && !publicAddr.valid()) sendRaw(stunAddr, nat::stunRequest(stunTx));
            if (stunAddr2.valid() && !publicAddr2.valid()) sendRaw(stunAddr2, nat::stunRequest(stunTx2));
            stunSent = t;
            if (stunAddr.valid()) stunTries++;
        }
        if (step == JoinStep::Hello) {
            if (sig->busy() || t - lastSend < 1.0) return;
            if (helloTries++ >= 3) return fail("The room's host did not answer (it may just have closed)");
            W w;
            w.b = head(T_HELLO);
            w.str(ident.gameId).str(ident.portVersion).str(ident.name).bytes(cn, 16).u8_(kSigLobby);
            const u8 room[4] = {};
            w.bytes(room, 4).u32_(ident.pid).bytes(ePub, 32).bytes(ident.edPub, 32);
            sendRaw(sigAddr(0), w.b);
            lastSend = t;
        } else if (step == JoinStep::Auth || step == JoinStep::Wait) {
            if (authDeferred) {
                // (while the servers' names are still being looked up nothing was asked yet: wait
                // for that as well, or the host gets no public address of ours at all)
                const bool stunDone = !stunLookup && (publicAddr.valid() || !stunAddr.valid()) && (publicAddr2.valid() || !stunAddr2.valid());
                if (stunDone || t - challengeAt > 1.5) sendSigAuth();
            } else if (!sig->busy() && t - lastSend > 3.0) {
                if (t - challengeAt > 120) return fail("The host did not answer");
                sendRaw(hostAddr, authPkt);
                lastSend = t;
            }
        } else if (step == JoinStep::Punch && hostLink) {
            const double age = t - punchStart;
            // A router that showed two different ports to the two STUN servers will not be reached
            // directly unless prediction hits at once: no point waiting the full 3 s there.
            const bool sym = publicAddr.valid() && publicAddr2.valid() && publicAddr.v4() == publicAddr2.v4() && publicAddr.port() != publicAddr2.port();
            if (!hostLink->addr.valid() && !turnAsked && age > (sForceRelay ? 0.0 : sym ? 1.0 : 3.0)) {
                sig->requestTurn();
                turnAsked = true;
                msg = "No direct connection yet: asking the lobby for a relay...";
            }
            if (turnAsked && !turnc->started() && !turnNone) {
                std::string srv, user, pass;
                const int tr = sig->turnResult(&srv, &user, &pass);
                Addr ta;
                if (tr == 1 && resolve(srv, 3478, &ta)) {
                    turnc->start(ta.normalized(), {user, pass});
                    for (auto& c : hostLink->cands) turnc->permit(c.a);
                    std::printf("[online] relay: allocating on %s\n", ta.text().c_str());
                } else if (tr != 0) {
                    turnNone = true;
                    std::printf("[online] relay: this lobby has none\n");
                }
            }
            if (turnc->started() && turnc->failed() && !turnNone) {
                turnNone = true;
                std::printf("[online] relay: %s\n", turnc->error().c_str());
            }
            if (turnc->ready() && !relayCandSent && !sig->busy()) {
                // The relay's address for us goes to the host (sealed with the link's key); the
                // host's addresses are reached through the relay from here.
                relayCandSent = true;
                const Addr ra = turnc->relayed();
                W c;
                c.u32_(ra.v4()).u16_(ra.port());
                u8 tag[oc::kTagSize];
                oc::seal(hostLink->tx, ~u64(0), nullptr, 0, c.b.data(), c.b.size(), tag);
                c.bytes(tag, oc::kTagSize);
                W w;
                w.b = head(T_CAND);
                w.bytes(cn, 16).u16_(u16(c.b.size())).bytes(c.b.data(), c.b.size());
                sendRaw(sigAddr(0), w.b);
                std::vector<Cand> via;
                for (auto& x : hostLink->cands)
                    if (x.kind != C_RELAY && x.a.family() == AF_INET) via.push_back({turnAddr(x.a), C_RELAY});
                for (auto& x : via) addCand(*hostLink, x.a, C_RELAY);
                std::printf("[online] relay: ready at %s\n", ra.text().c_str());
                msg = "Connecting through the relay...";
            }
            if (!hostLink->addr.valid() && age > (turnNone ? 12.0 : 25.0))
                return fail(turnNone ? "Could not open a connection to the host: both routers refuse direct connections and this lobby has "
                                       "no relay. A port forward on the host's router (UDP " + std::to_string(kDefaultPort) +
                                           ") or a VPN (Tailscale, ZeroTier) works"
                                     : "Could not open a connection to the host, not even through the relay");
        }
    }

    // ---- loop ----
    void onDatagram(const Addr& fromRaw, const u8* p, size_t n) {
        if (n < 4 || n > kMaxDatagram) return;
        const Addr from = fromRaw.normalized();
        // Our TURN relay: its own answers, and data from the host behind it.
        if (turnc->started() && from == turnc->server()) {
            Addr peer;
            std::vector<u8> data;
            std::vector<std::vector<u8>> out;
            const bool took = turnc->onDatagram(p, n, now(), &peer, &data, out);
            for (auto& o : out) rawSend(turnc->server(), o.data(), o.size());
            if (took) {
                if (!data.empty() && peer.valid()) onDatagram(turnAddr(peer.normalized()), data.data(), data.size());
                return;
            }
        }
        // STUN answer (binding success response, magic cookie).
        if (n >= 20 && p[0] == 0x01 && p[1] == 0x01 && p[4] == 0x21 && p[5] == 0x12 && p[6] == 0xA4 && p[7] == 0x42) {
            Addr mapped;
            if (nat::stunParse(p, n, stunTx, &mapped)) {
                if (!(publicAddr == mapped.normalized())) std::printf("[online] STUN: public address %s\n", mapped.normalized().text().c_str());
                publicAddr = mapped.normalized();
                if (role == Role::Host) rebuildCode();
            } else if (nat::stunParse(p, n, stunTx2, &mapped)) {
                if (!(publicAddr2 == mapped.normalized())) {
                    publicAddr2 = mapped.normalized();
                    std::printf("[online] STUN (second server): %s; router: %s\n", publicAddr2.text().c_str(), natText().c_str());
                }
                publicAddr2 = mapped.normalized();
            }
            return;
        }
        if (n >= 14 && relay.valid() && from == relay.normalized() && std::memcmp(p, "CFRL", 4) == 0 && p[4] == 1 &&
            std::memcmp(p + 6, relayRoom, 8) == 0) {
            if (p[5] == 2) {  // REG_OK
                if (!relayOk) std::printf("[online] relay %s: room registered\n", relay.text().c_str());
                relayOk = true;
            } else if (p[5] == 4 && role == Role::Host && n >= 18) {  // FROM_PEER
                const u32 peer = u32(p[14]) << 24 | u32(p[15]) << 16 | u32(p[16]) << 8 | p[17];
                onDatagram(virtualAddr(peer), p + 18, n - 18);
            } else if (p[5] == 6 && role != Role::Host) {  // FROM_HOST
                onDatagram(virtualAddr(0), p + 14, n - 14);
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
            if (role == Role::Member && step == JoinStep::Auth && from == hostAddr && !authDeferred) {
                step = JoinStep::Wait;
                st = State::WaitingForHost;
                msg = "Waiting for the host to let you in...";
            }
            break;
        case T_REJECT:
            if (sig && !isSig(from)) break;  // a lobby join is refused through the lobby only, never by a datagram from somewhere
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
        case T_ACCEPT:
            onAccept(from, r);
            break;
        case T_CAND:
            onCand(from, r);
            break;
        case T_GONE:
            onGone(from, r);
            break;
        case T_CANDOK:
        case T_PUNCH:
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
        w.u8_(hasPw ? 1 : 0).u8_(u8(links.size() + 1)).u8_(u8(maxM())).u16_(port).bytes(secret, 10);
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
        if (relay.valid() && relay.normalized().family() == AF_INET) {  // the relay route last (tried in parallel)
            Endpoint e;
            e.family = kRelayFamily;
            const u32 v = relay.normalized().v4();
            e.addr[0] = u8(v >> 24);
            e.addr[1] = u8(v >> 16);
            e.addr[2] = u8(v >> 8);
            e.addr[3] = u8(v);
            std::memcpy(e.addr + 4, relayRoom, 8);
            e.port = relay.normalized().port();
            eps.push_back(e);
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
        if (turnc->started()) {
            std::vector<std::vector<u8>> out;
            turnc->tick(t, out);
            for (auto& o : out) rawSend(turnc->server(), o.data(), o.size());
        }
        if (role == Role::Member && st != State::Failed && sig) {
            sigTick(t);
            if (st == State::Failed) return;
        } else if (role == Role::Member && st != State::Failed) {
            if (step == JoinStep::Hello && stunAddr.valid() && !publicAddr.valid() && stunTries < 4 && t - stunSent > 1.5) {
                sendRaw(stunAddr, nat::stunRequest(stunTx));
                stunSent = t;
                stunTries++;
            }
            if (step == JoinStep::Hello && publicAddr.valid() && replyCode.empty() && t - joinStart > 3) {
                Endpoint e;
                e.family = 4;
                const u32 v = publicAddr.normalized().v4();
                e.addr[0] = u8(v >> 24);
                e.addr[1] = u8(v >> 16);
                e.addr[2] = u8(v >> 8);
                e.addr[3] = u8(v);
                e.port = publicAddr.normalized().port();
                const u8 zero[10] = {};
                if (v) replyCode = encodeCode(zero, {e});
                if (v) std::printf("[online] no answer yet: reply code for the host %s (%s)\n", replyCode.c_str(), publicAddr.text().c_str());
            }
            if (step == JoinStep::Hello && t - lastSend > 0.5) {
                if (t - joinStart > (sTestMode ? 4.0 : replyCode.empty() ? 10.0 : 90.0)) {
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
                w.bytes(room, 4).u32_(ident.pid).bytes(ePub, 32).bytes(ident.edPub, 32);
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
            probeLink(l, t);
            if (t - l.lastPing > 1.0) {
                W w;
                w.u8_(B_PING).u32_(u32(t * 1000));
                sendSealed(l, myId(), w.b);
                l.lastPing = t;
            }
            pumpLink(l);
            flushAcks(l);
            // (a lobby link gets 30 s to open its first path; one that had a path and lost it is
            // judged by its silence alone: a member that is still there keeps sending)
            if (t - l.lastRecv > (sTestMode ? 5.0 : 15.0) && (!l.viaSig || l.addr.valid() || l.everPath || t - l.probeStart > 30.0)) l.dead = true;
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
            if (punchTarget.valid() && t < punchUntil && t - punchSent > 0.2) {
                sendRaw(punchTarget, head(T_PUNCH));
                punchSent = t;
            }
            if (relay.valid() && t - relayRegAt > (relayOk ? 10.0 : 1.0)) {
                const std::vector<u8> w = relayHead(1);
                const Addr ra = relay.forSocket(sfam);
                sendto(s, reinterpret_cast<const char*>(w.data()), int(w.size()), 0, reinterpret_cast<const sockaddr*>(&ra.ss), ra.len);
                relayRegAt = t;
            }
            if (opt.lanVisible && t - lastBeacon > 2.0) {
                sendBeacon();
                lastBeacon = t;
            }
            if (t - lastMembers > 5.0 && !links.empty()) broadcastMembers();
            // STUN: the public address of this socket.
            if (!sig && opt.portMapping && !sTestMode && stunAddr.valid() && !publicAddr.valid() && stunTries < 4 && t - stunSent > 1.5) {
                const auto req = nat::stunRequest(stunTx);
                sendRaw(stunAddr, req);
                stunSent = t;
                stunTries++;
            }
            // Lobby room: both STUN servers, again every 20 s (the answers go to joiners as our
            // addresses, and the traffic keeps the router's mapping alive while nobody is here).
            if (sig && stunAddr.valid() && t - stunSent > ((publicAddr.valid() && (publicAddr2.valid() || !stunAddr2.valid())) ? 20.0 : 1.0)) {
                sendRaw(stunAddr, nat::stunRequest(stunTx));
                if (stunAddr2.valid()) sendRaw(stunAddr2, nat::stunRequest(stunTx2));
                stunSent = t;
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
                if (gateway.valid()) {  // tests: the NAT simulator names the real sender
                    if (!(a.normalized() == gateway) || n < 6) continue;
                    const u8* h = buf.data();
                    a = makeV4(u32(h[0]) << 24 | u32(h[1]) << 16 | u32(h[2]) << 8 | h[3], u16(h[4] << 8 | h[5]));
                    onDatagram(a, buf.data() + 6, size_t(n) - 6);
                    continue;
                }
                onDatagram(a, buf.data(), size_t(n));
            }
            if (sig && role != Role::None) {
                u32 peer = 0;
                std::vector<u8> d;
                for (int k = 0; k < 16 && sig && sig->recv(&peer, &d); k++) onDatagram(sigAddr(role == Role::Host ? peer : 0), d.data(), d.size());
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
        relay = Addr{};
        relayOk = false;
        relayRegAt = -100;
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
        sig.reset();
        turnc.reset(new turn::Client);
        turnAsked = turnNone = relayCandSent = authDeferred = false;
        helloTries = 0;
        stunAddr2 = publicAddr2 = Addr{};
        stunLookup = false;
        stunSent = -100;
        expectKeySet = hostKeyKnown = false;
    }

    // The two STUN servers (resolved on a thread; `g0` = the room generation it belongs to).
    void findStun(u32 g0) {
        if (!sStun[0].empty()) {
            resolve(sStun[0], 3478, &stunAddr);
            if (!sStun[1].empty()) resolve(sStun[1], 3478, &stunAddr2);
            return;
        }
        if (sTestMode) return;
        Impl* self = this;
        stunLookup = true;
        std::thread([self, g0] {
            Addr a, b;
            for (auto& srv : nat::stunServers()) {
                Addr x;
                if (!resolve(srv, 3478, &x)) continue;
                if (!a.valid()) a = x;
                else if (x.normalized().v4() != a.normalized().v4()) {
                    b = x;
                    break;
                }
            }
            std::lock_guard<std::recursive_mutex> g2(self->m);
            if (self->gen != g0 || self->role == Role::None) return;
            self->stunAddr = a;
            self->stunAddr2 = b;
            self->stunLookup = false;
        }).detach();
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
        ephemeral(ePriv, ePub);
        joinStart = now();
        lastSend = -10;
        replyCode.clear();
        oc::randomBytes(stunTx, 12);
        oc::randomBytes(stunTx2, 12);
        findStun(gen);
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
        } else if (role == Role::Member && st != State::Failed) {
            sendSigGone();  // a lobby join cancelled half way
        }
        if (turnc->started()) {  // give the relay allocation back
            std::vector<std::vector<u8>> out;
            turnc->close(out);
            for (auto& o : out) rawSend(turnc->server(), o.data(), o.size());
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
    oc::randomBytes(d.stunTx2, 12);
    d.hasPw = !o.password.empty();
    if (d.hasPw) oc::pbkdf2Sha256(o.password.data(), o.password.size(), d.salt, 16, kPbkdfIterations, d.pwKey, 32);
    if (!o.relay.empty() && !sTestMode) {
        if (resolve(o.relay, kRelayPort, &d.relay) && d.relay.normalized().family() == AF_INET) {
            oc::randomBytes(d.relayRoom, 8);
            std::printf("[online] relay %s for this room\n", d.relay.text().c_str());
        } else {
            d.relay = Addr{};
            std::printf("[online] relay %s: not found (only IPv4 relays)\n", o.relay.c_str());
        }
    } else if (!o.relay.empty() && sTestMode && resolve(o.relay, kRelayPort, &d.relay)) {
        oc::randomBytes(d.relayRoom, 8);
    }
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
    Addr relay;
    u8 relayRoom[8] = {};
    for (auto& e : eps) {
        if (e.family == kRelayFamily) {
            relay = makeV4(u32(e.addr[0]) << 24 | u32(e.addr[1]) << 16 | u32(e.addr[2]) << 8 | e.addr[3], e.port);
            std::memcpy(relayRoom, e.addr + 4, 8);
            addrs.push_back(Impl::virtualAddr(0));
            continue;
        }
        addrs.push_back(e.family == 4 ? makeV4(u32(e.addr[0]) << 24 | u32(e.addr[1]) << 16 | u32(e.addr[2]) << 8 | e.addr[3], e.port)
                                      : makeV6(e.addr, e.port));
    }
    std::lock_guard<std::recursive_mutex> g(d_->m);
    if (!d_->startJoin(addrs, false, password, err)) return false;
    d_->relay = relay;
    std::memcpy(d_->relayRoom, relayRoom, 8);
    std::memcpy(d_->secret, secret, 10);
    std::printf("[online] joining with a room code (%zu endpoints)\n", addrs.size());
    return true;
}

void Session::attachSignaling(std::shared_ptr<Signaling> sig) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    if (d_->role != Impl::Role::Host) return;
    d_->sig = std::move(sig);
    d_->stunSent = -100;
    // (both servers: the port-mapping thread of host() looks up only the first one)
    if (!d_->stunAddr.valid() || !d_->stunAddr2.valid()) d_->findStun(d_->gen);
}

bool Session::joinSignaled(std::shared_ptr<Signaling> sig, const u8* expectHostKey, const std::string& password, std::string* err) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    if (!d_->startJoin({Impl::sigAddr(0)}, true, password, err)) return false;
    std::memset(d_->secret, 0, 10);
    d_->sig = std::move(sig);
    d_->hostAddr = Impl::sigAddr(0);
    d_->expectKeySet = expectHostKey != nullptr;
    if (expectHostKey) std::memcpy(d_->expectKey, expectHostKey, 32);
    d_->msg = "Asking the room's host...";
    std::printf("[online] joining a lobby room\n");
    return true;
}

void Session::setAllowedKeys(const std::vector<std::array<u8, 32>>& keys) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    d_->allowed = keys;
}

void Session::setTestGateway(const std::string& gw) {
    std::lock_guard<std::recursive_mutex> g(d_->m);
    d_->gateway = Addr{};
    if (!gw.empty()) {
        resolve(gw, 0, &d_->gateway);
        d_->gateway = d_->gateway.normalized();
    }
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
            } else if (Impl::isSig(p.addr)) {
                // Lobby: an answer travels only as the reply to a request of the joiner, and none
                // is open now. The request stays, decided: the joiner's next AUTH (it asks again
                // every few seconds while it waits) gets the refusal (onAuth).
            } else {
                d_->reject(p.addr, REJ_DECLINED);
                d_->pend.erase(d_->pend.begin() + (&p - d_->pend.data()));
            }
            return;
        }
}

bool Session::punch(const std::string& reply, std::string* err) {
    u8 zero[10];
    std::vector<Endpoint> eps;
    if (!decodeCode(reply, zero, eps) || eps.size() != 1 || eps[0].family != 4) {
        if (err) *err = "That reply code is not valid";
        return false;
    }
    std::lock_guard<std::recursive_mutex> g(d_->m);
    if (d_->role != Impl::Role::Host) {
        if (err) *err = "Only the host lets players in";
        return false;
    }
    const auto& e = eps[0];
    d_->punchTarget = makeV4(u32(e.addr[0]) << 24 | u32(e.addr[1]) << 16 | u32(e.addr[2]) << 8 | e.addr[3], e.port);
    d_->punchUntil = now() + 30;
    d_->punchSent = -1;
    std::printf("[online] opening the way for %s (hole punching, 30 s)\n", d_->punchTarget.text().c_str());
    return true;
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
        if (p.authed && d.opt.askBeforeJoin && !p.decided) {
            std::string where = p.addr.text();
            if (Impl::isSig(p.addr)) {  // a lobby joiner: the public address it gave, never the signaling one
                where = p.cands.empty() ? std::string("through the lobby") : p.cands[0].a.text();
                for (auto& c : p.cands)
                    if (c.kind == C_PUBLIC) {
                        where = c.a.text();
                        break;
                    }
            }
            s.pending.push_back({p.ticket, p.name, where});
        }
    s.lanEndpoint = d.lanEp;
    s.publicEndpoint = d.pubEp.empty() && d.publicAddr.valid() ? d.publicAddr.text() : d.pubEp;
    s.reachability = d.reach;
    s.replyCode = d.st == State::Connecting ? d.replyCode : std::string();
    s.relayed = d.role == Impl::Role::Member && d.hostLink && d.hostLink->relayed;
    s.viaLobby = d.sig != nullptr;
    s.nat = d.natText();
    s.hostKeyKnown = d.role == Impl::Role::Member && d.hostKeyKnown;
    std::memcpy(s.hostKey, d.hostKey, 32);
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
            m.address = l->addr.valid() ? l->addr.text() : std::string("(connecting)");
            m.relayed = l->relayed;
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
void setStunServers(const std::string& a, const std::string& b) {
    sStun[0] = a;
    sStun[1] = b;
}
void setForceRelay(bool on) { sForceRelay = on; }

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
        b.insert(b.end(), e.addr, e.addr + (e.family == 4 ? 4 : e.family == kRelayFamily ? 12 : 16));
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
        const size_t al = e.family == 4 ? 4 : e.family == 6 ? 16 : e.family == kRelayFamily ? 12 : 0;
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
