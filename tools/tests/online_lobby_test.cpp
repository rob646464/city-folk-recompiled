// Lobby rooms end to end (src/recomp/online_net.* joinSignaled / attachSignaling, online_lobby.*):
// hosts and joiners of this process meet through the self-hosted lobby (tools/lobby/cf_lobby.cpp in
// a thread) and open their UDP path through simulated home routers (tools/tests/nat_sim.h): no
// NAT, full cone, port restricted, symmetric with sequential or random ports. Two in-process
// STUN / TURN servers (tools/tests/turn_standin.*) on loopback addresses of their own. Loopback
// only, no internet; every port is chosen by the system. About 100 s.
//
// Checked: every pair of routers ends direct or relayed as the design says (cases 1-9, one summary
// line each with the seconds to Joined), a relayed pair moves to a direct path, a router restart,
// members and hosts that vanish, lost answers, three joiners at once with messages between them,
// room rules (password, size, friends only, ask before join, kick, close), the room list, and bad
// input (garbage and replayed signals, forged refusals, random datagrams at the sockets).
//   cf_online_lobby_test [seed] [--observe]
// --observe adds reports on known weak spots (never failures).
#include "nat_sim.h"
#include "turn_standin.h"

#include "port/http.h"
#include "recomp/online_crypto.h"
#include "recomp/online_ed25519.h"
#include "recomp/online_lobby.h"
#include "recomp/online_net.h"
#include "recomp/online_sock.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#endif

using namespace online::net;
namespace lobby = online::lobby;
namespace http = port::http;
using natsim::Router;

int lobbyMain(int argc, char** argv);  // tools/lobby/cf_lobby.cpp (CF_LOBBY_AS_LIB)
extern std::atomic<int> gLobbyPort;
extern std::atomic<bool> gLobbyStop;

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

using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static bool waitFor(const std::function<bool()>& f, double seconds) {
    const auto t0 = Clock::now();
    while (since(t0) < seconds) {
        if (f()) return true;
        sleepMs(5);
    }
    return f();
}

static std::mt19937 gRng;
static TurnTestServer gTurnA, gTurnB;  // A: STUN + TURN (the lobby's relay), B: the second STUN server
static const char* kTurnUser = "cf-lobby-test";
static const char* kTurnPass = "relay-secret";
static const char* kGame = "RUUE01";
static std::vector<std::string> gSummary;

static Identity ident(const std::string& name, u32 pid) {
    Identity i;
    i.name = name;
    i.gameId = kGame;
    i.portVersion = "test";
    i.pid = pid;
    i.friendKey = pid;
    return i;
}

static Identity identKey(const std::string& name, u32 pid, const u8 seed[32]) {
    Identity i = ident(name, pid);
    i.haveKey = true;
    std::memcpy(i.edSeed, seed, 32);
    onlinecrypto::ed25519PublicKey(seed, i.edPub);
    return i;
}

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- the lobby (cf_lobby in a thread) -------------------------------------------------------------
struct LobbyProc {
    std::thread th;
    std::string url;

    bool start(const std::vector<std::string>& extra) {
        gLobbyPort = 0;  // an earlier lobby of this process keeps running on its own port
        std::vector<std::string> args = {"cf_lobby", "--bind", "127.0.0.1", "--port", "0", "--signals-per-min", "1000000",
                                         "--turns-per-hour", "1000000", "--rooms-per-ip", "64", "--signal-timeout-ms", "2000"};
        args.insert(args.end(), extra.begin(), extra.end());
        th = std::thread([args] {
            std::vector<char*> argv;
            for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            lobbyMain(int(argv.size()), argv.data());
        });
        if (!waitFor([] { return gLobbyPort.load() != 0; }, 5)) return false;
        url = "http://127.0.0.1:" + std::to_string(gLobbyPort.load());
        return true;
    }
    void join() {
        if (th.joinable()) th.join();
    }
};

// ---- routers --------------------------------------------------------------------------------------
struct Nat {
    const char* name;
    bool on;
    natsim::Type type;
    natsim::Ports ports;
};
static const Nat kNoNat{"no NAT", false, natsim::Type::FullCone, natsim::Ports::Sequential};
static const Nat kFull{"full cone", true, natsim::Type::FullCone, natsim::Ports::Sequential};
static const Nat kRestricted{"port-restricted", true, natsim::Type::PortRestricted, natsim::Ports::Sequential};
static const Nat kSymSeq{"symmetric (sequential)", true, natsim::Type::Symmetric, natsim::Ports::Sequential};
static const Nat kSymRand{"symmetric (random)", true, natsim::Type::Symmetric, natsim::Ports::Random};

static std::unique_ptr<Router> makeRouter(const Nat& n) {
    if (!n.on) return nullptr;
    static int index = 0;
    index++;
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = int(getpid());
#endif
    // A public address of its own per router; the process id keeps parallel runs apart.
    const uint32_t ip = 0x7F000000u | uint32_t(64 + pid % 128) << 16 | uint32_t(1 + index / 250) << 8 | uint32_t(1 + index % 250);
    const uint16_t first = uint16_t(20000 + gRng() % 28000);
    auto r = std::make_unique<Router>(n.type, n.ports, ip, first, n.name);
    std::string err;
    if (!r->start(&err)) {
        CHECK(false, "%s", err.c_str());
        return nullptr;
    }
    return r;
}

static u16 freeUdpPort() {
    using namespace online::sock;
    startup();
    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    u16 port = 0;
    if (s != kInvalid && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        socklen_t l = sizeof a;
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
    }
    if (s != kInvalid) closeSocket(s);
    return port;
}

// ---- the room list --------------------------------------------------------------------------------
static std::vector<lobby::Room> fetchRooms(const std::string& url) {
    waitFor([] { return !lobby::state().loading; }, 12);
    lobby::refresh(url, kGame);
    waitFor([] { return !lobby::state().loading; }, 12);
    const lobby::ListState st = lobby::state();
    CHECK(st.error.empty() && st.everLoaded && st.url == url, "room list: %s", st.error.c_str());
    return st.error.empty() ? st.rooms : std::vector<lobby::Room>();
}

static bool findRoom(const std::string& url, const std::function<bool(const lobby::Room&)>& match, lobby::Room* out, double seconds) {
    const auto t0 = Clock::now();
    do {
        for (auto& r : fetchRooms(url))
            if (match(r)) {
                if (out) *out = r;
                return true;
            }
        sleepMs(50);
    } while (since(t0) < seconds);
    return false;
}

// ---- a room: the host's session behind its router, listed on a lobby ------------------------------
struct Room {
    std::unique_ptr<Router> nat;  // (destroyed after the session)
    std::unique_ptr<Session> s;
    std::shared_ptr<lobby::HostChannel> chan;
    std::string url, name, id;
    lobby::Room listed;
    u16 port = 0;
    void close() {
        if (chan) chan->stop();
        if (s) s->leave();
    }
    ~Room() { close(); }
};

static bool openRoom(Room& r, const Nat& nat, const std::string& url, HostOptions o, const Identity& id) {
    static int number = 0;
    r.url = url;
    r.nat = makeRouter(nat);
    r.s = std::make_unique<Session>();
    r.s->setIdentity(id);
    if (r.nat) r.s->setTestGateway(r.nat->gateway());
    o.lanVisible = false;
    o.portMapping = false;
    if (o.roomName.empty()) o.roomName = "Test room " + std::to_string(++number);
    r.name = o.roomName;
    std::string err;
    bool ok = false;
    for (int tries = 0; tries < 5 && !ok; tries++) {
        o.port = freeUdpPort();
        ok = o.port && r.s->host(o, &err);
    }
    CHECK(ok, "host: %s", err.c_str());
    if (!ok) return false;
    r.port = o.port;
    lobby::RoomInfo info;
    info.name = o.roomName;
    info.host = id.name;
    info.town = "Testville";
    info.game = id.gameId;
    info.ver = id.portVersion;
    info.players = 1;
    info.max = o.maxMembers;
    info.lock = !o.password.empty();
    info.friends = o.friendsOnly;
    info.tag = id.haveKey ? lobby::makeTag(id.edPub) : std::string("0000000000000000");
    r.chan = lobby::HostChannel::start(url, info);
    r.s->attachSignaling(r.chan);
    CHECK(waitFor([&] { return r.chan->listed(); }, 6), "room listed: %s", r.chan->status().c_str());
    // Both STUN answers are in before anyone joins (they are the host's addresses for joiners).
    CHECK(waitFor([&] { return !r.s->snapshot().nat.empty(); }, 4), "the host's STUN answers (%s)", nat.name);
    const std::string natText = r.s->snapshot().nat;
    CHECK(contains(natText, nat.type == natsim::Type::Symmetric && nat.on ? "symmetric" : "ordinary"), "host router seen as: %s (is %s)", natText.c_str(), nat.name);
    const std::string tag = info.tag;
    const bool friends = o.friendsOnly;
    const std::string name = r.name;
    const bool found = findRoom(url, [&](const lobby::Room& x) { return friends ? x.tag == tag : x.name == name; }, &r.listed, 5);
    CHECK(found, "room \"%s\" in the lobby's list", r.name.c_str());
    r.id = r.listed.id;
    return found;
}

// ---- a joiner behind its router -------------------------------------------------------------------
struct Peer {
    std::unique_ptr<Router> nat;
    std::unique_ptr<Session> s;
    const Nat* kind = nullptr;
    std::string name;
};

static std::unique_ptr<Peer> makePeer(const Nat& n, const Identity& id) {
    auto p = std::make_unique<Peer>();
    p->kind = &n;
    p->name = id.name;
    p->nat = makeRouter(n);
    p->s = std::make_unique<Session>();
    p->s->setIdentity(id);
    if (p->nat) p->s->setTestGateway(p->nat->gateway());
    return p;
}

struct JoinOut {
    State state = State::Off;
    double secs = 0;
    std::string message;
    bool joined() const { return state == State::Joined; }
};

static bool joinBegin(Session& j, const std::string& url, const std::string& roomId, const u8* expectKey, const std::string& password,
                      std::shared_ptr<Signaling> sig = nullptr) {
    std::string err;
    if (!sig) sig = lobby::joinChannel(url, roomId);
    const bool started = j.joinSignaled(sig, expectKey, password, &err);
    CHECK(started, "joinSignaled: %s", err.c_str());
    return started;
}

static bool settled(Session& j) {
    const State st = j.snapshot().state;
    return st == State::Joined || st == State::Failed;
}

static JoinOut joinRoom(Session& j, const std::string& url, const std::string& roomId, const u8* expectKey, const std::string& password,
                        double timeout, std::shared_ptr<Signaling> sig = nullptr) {
    JoinOut o;
    const auto t0 = Clock::now();
    if (!joinBegin(j, url, roomId, expectKey, password, std::move(sig))) return o;
    waitFor([&] { return settled(j); }, timeout);
    o.secs = since(t0);
    const Snapshot s = j.snapshot();
    o.state = s.state;
    o.message = s.message;
    return o;
}

static bool hostMember(Session& h, const std::string& name, Member* out = nullptr) {
    for (auto& m : h.members())
        if (!m.host && m.name == name) {
            if (out) *out = m;
            return true;
        }
    return false;
}

static std::vector<u8> pattern(size_t n, u32 seed) {
    std::vector<u8> v(n);
    u32 x = seed * 2654435761u + 12345;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        v[i] = u8(x >> 24);
    }
    return v;
}

// 50,000 bytes reliable, host -> member and member -> host, compared byte for byte.
static bool exchange(Session& h, Session& j, u8 jid, const char* what) {
    static u32 round = 0;
    const std::vector<u8> a = pattern(50000, ++round), b = pattern(50000, ++round);
    CHECK(h.sendApp(jid, 0x21, a.data(), a.size(), true), "%s: host sendApp", what);
    CHECK(j.sendApp(0, 0x22, b.data(), b.size(), true), "%s: member sendApp", what);
    bool gotA = false, gotB = false;
    waitFor([&] {
        AppMsg m;
        while (j.pollApp(m, 0x21, 0x21)) gotA = m.from == 0 && m.data == a;
        while (h.pollApp(m, 0x22, 0x22)) gotB = m.from == jid && m.data == b;
        return gotA && gotB;
    }, 20);
    CHECK(gotA, "%s: 50,000 bytes host -> member", what);
    CHECK(gotB, "%s: 50,000 bytes member -> host", what);
    return gotA && gotB;
}

// What must hold for every connected pair: state, member lists, the path's kind on both sides, a
// big reliable message each way.
static void checkConnected(Room& r, Peer& p, bool relayed, size_t members, const char* what) {
    const Snapshot js = p.s->snapshot();
    CHECK(js.state == State::Joined, "%s: joiner state (%s)", what, js.message.c_str());
    if (js.state != State::Joined) return;
    CHECK(js.viaLobby && js.selfId != 0, "%s: joiner snapshot (lobby %d, id %u)", what, js.viaLobby, js.selfId);
    CHECK(js.roomName == r.name, "%s: room name \"%s\"", what, js.roomName.c_str());
    const bool lists = waitFor([&] {
        Member hm;
        if (r.s->members().size() != members || p.s->members().size() != members) return false;
        return hostMember(*r.s, p.name, &hm) && hm.id == js.selfId;
    }, 4);
    CHECK(lists, "%s: member lists (host sees %zu, joiner %zu, want %zu)", what, r.s->members().size(), p.s->members().size(), members);
    bool sawHost = false, sawSelf = false;
    for (auto& m : p.s->members()) {
        sawHost |= m.host && m.id == 0 && m.name == r.s->members()[0].name;
        sawSelf |= m.self && m.id == js.selfId && m.name == p.name;
    }
    CHECK(sawHost && sawSelf, "%s: the joiner's list names the host and itself", what);
    exchange(*r.s, *p.s, js.selfId, what);
    // After the traffic both sides have proved their path.
    Member hm;
    const bool kinds = waitFor([&] {
        return p.s->snapshot().relayed == relayed && hostMember(*r.s, p.name, &hm) && hm.relayed == relayed && hm.address != "(connecting)";
    }, 4);
    hostMember(*r.s, p.name, &hm);
    CHECK(kinds, "%s: path kind: joiner says %s, host says %s (%s), expected %s", what, p.s->snapshot().relayed ? "relayed" : "direct",
          hm.relayed ? "relayed" : "direct", hm.address.c_str(), relayed ? "relayed" : "direct");
    const std::string natText = p.s->snapshot().nat;
    CHECK(contains(natText, p.kind->on && p.kind->type == natsim::Type::Symmetric ? "symmetric" : "ordinary"), "%s: joiner's router seen as: %s (is %s)",
          what, natText.c_str(), p.kind->name);
}

static void leaveAndCheck(Room& r, Peer& p, const char* what) {
    p.s->leave();
    CHECK(waitFor([&] { return !hostMember(*r.s, p.name); }, 8), "%s: the host sees the joiner leave", what);
}

static void routerStats(const Room& r, const Peer& p) {
    if (r.nat) std::printf("    host router:   %s\n", r.nat->stats().c_str());
    if (p.nat) std::printf("    joiner router: %s\n", p.nat->stats().c_str());
}

static void summary(const std::string& label, const std::string& result, const char* path, double secs) {
    char b[256];
    if (secs >= 0) std::snprintf(b, sizeof b, "%-66s %-8s %-8s %5.2f s", label.c_str(), result.c_str(), path, secs);
    else std::snprintf(b, sizeof b, "%-66s %-8s %-8s", label.c_str(), result.c_str(), path);
    gSummary.push_back(b);
    std::printf("== %s\n", b);
}

// One join of a fresh room under a pair of routers. relayed: 1 expected relayed, 0 direct, -1 either
// (reported only).
static void natCase(const std::string& label, const Nat& hostNat, const Nat& joinNat, int relayed, const std::string& url) {
    std::printf("\n---- %s\n", label.c_str());
    static u32 pid = 100;
    Room room;
    if (!openRoom(room, hostNat, url, HostOptions{}, ident("Host", ++pid))) return summary(label, "NO ROOM", "-", -1);
    auto p = makePeer(joinNat, ident("Joiner", ++pid));
    const uint64_t bytes0 = gTurnA.relayedBytes();
    const JoinOut o = joinRoom(*p->s, url, room.id, nullptr, "", 30);
    CHECK(o.joined(), "%s: %s", label.c_str(), o.message.c_str());
    if (!o.joined()) {
        routerStats(room, *p);
        return summary(label, "FAILED", "-", o.secs);
    }
    sleepMs(150);  // the joiner's own pong (its path) may come a moment after the welcome
    const bool isRelayed = relayed >= 0 ? relayed == 1 : p->s->snapshot().relayed;
    checkConnected(room, *p, isRelayed, 2, label.c_str());
    const uint64_t moved = gTurnA.relayedBytes() - bytes0;
    if (isRelayed) {
        CHECK(moved >= 100000, "%s: bytes through the relay: %llu", label.c_str(), (unsigned long long)moved);
        CHECK(gTurnA.allocations() == 1, "%s: relay allocations %d", label.c_str(), gTurnA.allocations());
    } else {
        CHECK(moved == 0, "%s: a direct path, but %llu bytes went through the relay", label.c_str(), (unsigned long long)moved);
    }
    // Timing: reported; a failure only when far off.
    const double want = isRelayed ? 8.0 : 2.0;
    if (o.secs > want) std::printf("    NOTE: slower than expected (%.2f s, expected within ~%.0f s)\n", o.secs, want);
    CHECK(o.secs < want * 2.5, "%s: %.2f s to Joined", label.c_str(), o.secs);
    leaveAndCheck(room, *p, label.c_str());
    if (isRelayed) CHECK(waitFor([] { return gTurnA.allocations() == 0; }, 4), "%s: the relay allocation is given back", label.c_str());
    routerStats(room, *p);
    summary(label, "Joined", isRelayed ? "relayed" : "direct", o.secs);
}

// ---- raw signaling (robustness) -------------------------------------------------------------------
static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64(const std::vector<u8>& d) {
    std::string o;
    for (size_t i = 0; i < d.size(); i += 3) {
        const u32 n = u32(d[i]) << 16 | (i + 1 < d.size() ? u32(d[i + 1]) << 8 : 0) | (i + 2 < d.size() ? u32(d[i + 2]) : 0);
        o += kB64[n >> 18 & 63];
        o += kB64[n >> 12 & 63];
        o += i + 1 < d.size() ? kB64[n >> 6 & 63] : '=';
        o += i + 2 < d.size() ? kB64[n & 63] : '=';
    }
    return o;
}
static std::vector<u8> unb64(const std::string& s) {
    std::vector<u8> out;
    u32 acc = 0;
    int bits = 0;
    for (char c : s) {
        const char* p = c ? std::strchr(kB64, c) : nullptr;
        if (!p) break;
        acc = acc << 6 | u32(p - kB64);
        bits += 6;
        if (bits >= 8) {
            out.push_back(u8(acc >> (bits - 8)));
            bits -= 8;
        }
    }
    return out;
}

struct SigAnswer {
    int status = 0;
    std::vector<u8> data;
    std::string error;
};

static SigAnswer postSignalText(const std::string& url, const std::string& room, const std::string& peer, const std::string& dataB64, int timeoutMs = 7000) {
    const std::string body = "{\"v\":1,\"room\":\"" + room + "\",\"peer\":\"" + peer + "\",\"data\":\"" + dataB64 + "\"}";
    const http::Result r = http::request("POST", url + "/v1/signal", body, "application/json", timeoutMs);
    SigAnswer a;
    a.status = r.status;
    a.error = r.error;
    http::Json j;
    if (r.ok() && http::parseJson(r.body, j)) a.data = unb64(j["data"].str());
    return a;
}
static SigAnswer postSignal(const std::string& url, const std::string& room, const std::string& peer, const std::vector<u8>& d) {
    return postSignalText(url, room, peer, b64(d));
}

static std::string randomPeer() {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", unsigned(gRng()));
    return b;
}

static std::vector<u8> randomBytes(size_t n) {
    std::vector<u8> v(n);
    for (auto& x : v) x = u8(gRng());
    return v;
}

// A joiner's signaling whose peer id the test knows: one POST /v1/signal per datagram.
struct RawSignaling final : Signaling {
    struct St {
        std::mutex m;
        std::deque<std::vector<u8>> in;
        int working = 0;
        std::string error;
    };
    std::shared_ptr<St> st = std::make_shared<St>();
    std::string url, room, peer;
    void send(u32, const std::vector<u8>& d) override {
        {
            std::lock_guard<std::mutex> g(st->m);
            st->working++;
        }
        std::thread([s = st, url = url, room = room, peer = peer, d] {
            const SigAnswer a = postSignal(url, room, peer, d);
            std::lock_guard<std::mutex> g(s->m);
            s->working--;
            if (a.status == 200 && !a.data.empty()) s->in.push_back(a.data);
            else if (a.status != 504) s->error = "signal failed (HTTP " + std::to_string(a.status) + ")";
        }).detach();
    }
    bool recv(u32* p, std::vector<u8>* d) override {
        std::lock_guard<std::mutex> g(st->m);
        if (st->in.empty()) return false;
        *p = 0;
        *d = std::move(st->in.front());
        st->in.pop_front();
        return true;
    }
    bool busy() override {
        std::lock_guard<std::mutex> g(st->m);
        return st->working > 0;
    }
    std::string error() override {
        std::lock_guard<std::mutex> g(st->m);
        return st->error;
    }
    void requestTurn() override {}
    int turnResult(std::string*, std::string*, std::string*) override { return -1; }
};

// Records the handshake datagrams that pass through a joiner's signaling.
struct Recorder final : Signaling {
    std::shared_ptr<Signaling> inner;
    std::mutex m;
    std::vector<std::vector<u8>> sent, got;
    explicit Recorder(std::shared_ptr<Signaling> s) : inner(std::move(s)) {}
    void send(u32 peer, const std::vector<u8>& d) override {
        {
            std::lock_guard<std::mutex> g(m);
            sent.push_back(d);
        }
        inner->send(peer, d);
    }
    bool recv(u32* peer, std::vector<u8>* d) override {
        if (!inner->recv(peer, d)) return false;
        std::lock_guard<std::mutex> g(m);
        got.push_back(*d);
        return true;
    }
    bool busy() override { return inner->busy(); }
    std::string error() override { return inner->error(); }
    void requestTurn() override { inner->requestTurn(); }
    int turnResult(std::string* a, std::string* b, std::string* c) override { return inner->turnResult(a, b, c); }
    std::vector<u8> first(bool ofSent, u8 type) {
        std::lock_guard<std::mutex> g(m);
        for (auto& d : ofSent ? sent : got)
            if (d.size() >= 4 && d[3] == type) return d;
        return {};
    }
};

// Loses the first answer of each given type on its way to the joiner (a lobby hiccup).
struct Lossy final : Signaling {
    std::shared_ptr<Signaling> inner;
    std::vector<u8> lose;  // wire types, each dropped once
    int lost = 0;
    Lossy(std::shared_ptr<Signaling> s, std::vector<u8> types) : inner(std::move(s)), lose(std::move(types)) {}
    void send(u32 peer, const std::vector<u8>& d) override { inner->send(peer, d); }
    bool recv(u32* peer, std::vector<u8>* d) override {
        if (!inner->recv(peer, d)) return false;
        for (size_t i = 0; i < lose.size(); i++)
            if (d->size() >= 4 && (*d)[3] == lose[i]) {
                lose.erase(lose.begin() + long(i));
                lost++;
                return false;
            }
        return true;
    }
    bool busy() override { return inner->busy(); }
    std::string error() override { return inner->error(); }
    void requestTurn() override { inner->requestTurn(); }
    int turnResult(std::string* a, std::string* b, std::string* c) override { return inner->turnResult(a, b, c); }
};

// Wire types of online_net.cpp (the test only looks at the type byte).
enum : u8 { T_HELLO = 1, T_CHALLENGE = 2, T_AUTH = 3, T_REJECT = 4, T_ACCEPT = 6, T_CAND = 7, T_SEALED = 0x10 };

static std::vector<u8> typed(u8 type, size_t bodyLen) {
    std::vector<u8> d = {'C', 'F', 2, type};
    const std::vector<u8> r = randomBytes(bodyLen);
    d.insert(d.end(), r.begin(), r.end());
    return d;
}

// Random datagrams straight at a UDP port: noise, well-formed headers with random bodies, sealed
// look-alikes with the member's id, STUN and relay look-alikes, over-long ones.
static void fuzzUdp(const std::string& target, int count, u8 memberId) {
    using namespace online::sock;
    Addr to;
    if (!resolve(target, 0, &to) || to.normalized().family() != AF_INET) {
        CHECK(false, "fuzz target %s", target.c_str());
        return;
    }
    to = to.normalized();
    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    static const u8 types[] = {1, 2, 3, 4, 5, 6, 7, 8, 0x10, 0x21, 0x22, 0x23, 0x7F};
    for (int i = 0; i < count; i++) {
        std::vector<u8> d;
        switch (i % 6) {
        case 0:
            d = randomBytes(gRng() % 1400);
            break;
        case 1:
            d = typed(types[gRng() % sizeof types], gRng() % 700);
            break;
        case 2: {  // sealed: sender id, 3 zero bytes, counter, body, tag
            d = typed(T_SEALED, 12 + 16 + gRng() % 300);
            d[4] = (gRng() & 1) ? memberId : u8(gRng());
            if (gRng() & 1) {
                for (int k = 9; k < 16; k++) d[size_t(k)] = 0;  // a small counter (inside the replay window)
            }
            break;
        }
        case 3: {  // STUN binding success look-alike
            d = randomBytes(20 + gRng() % 80);
            const u8 h[8] = {0x01, 0x01, 0, u8(d.size() - 20), 0x21, 0x12, 0xA4, 0x42};
            std::memcpy(d.data(), h, 8);
            break;
        }
        case 4: {  // cf_relay look-alike
            d = randomBytes(6 + gRng() % 60);
            std::memcpy(d.data(), "CFRL\x01", 5);
            d[5] = u8(gRng() % 8);
            break;
        }
        default:
            d = (gRng() & 1) ? typed(types[gRng() % sizeof types], 1400 + gRng() % 600) : randomBytes(gRng() % 4);
            break;
        }
        sendto(s, reinterpret_cast<const char*>(d.data()), int(d.size()), 0, reinterpret_cast<const sockaddr*>(&to.ss), to.len);
        if (i % 20 == 19) sleepMs(2);
    }
    closeSocket(s);
}

// ---- the parts of the test ------------------------------------------------------------------------
static void threeJoiners(const std::string& url) {
    const char* what = "3 joiners (full cone, symmetric sequential, symmetric random) into a port-restricted host";
    std::printf("\n---- %s\n", what);
    Room room;
    if (!openRoom(room, kRestricted, url, HostOptions{}, ident("Host", 500))) return;
    auto a = makePeer(kFull, ident("Ann", 501));
    auto b = makePeer(kSymSeq, ident("Ben", 502));
    auto c = makePeer(kSymRand, ident("Cat", 503));
    // All three ask at the same moment.
    Peer* ps[3] = {a.get(), b.get(), c.get()};
    JoinOut os[3];
    const auto t0 = Clock::now();
    for (Peer* p : ps) joinBegin(*p->s, url, room.id, nullptr, "");
    waitFor([&] {
        bool all = true;
        for (int i = 0; i < 3; i++) {
            if (os[i].secs == 0 && settled(*ps[i]->s)) os[i].secs = since(t0);
            all &= os[i].secs != 0;
        }
        return all;
    }, 30);
    for (int i = 0; i < 3; i++) {
        const Snapshot s = ps[i]->s->snapshot();
        os[i].state = s.state;
        os[i].message = s.message;
        CHECK(os[i].joined(), "joiner %s (%s): %s", ps[i]->name.c_str(), ps[i]->kind->name, s.message.c_str());
    }
    const JoinOut &oa = os[0], &ob = os[1], &oc = os[2];
    if (!oa.joined() || !ob.joined() || !oc.joined()) return summary(what, "FAILED", "-", -1);
    sleepMs(150);
    checkConnected(room, *a, false, 4, "3 joiners: full cone");
    checkConnected(room, *b, false, 4, "3 joiners: symmetric sequential");
    checkConnected(room, *c, true, 4, "3 joiners: symmetric random");
    // The host's list tells every member who is relayed.
    CHECK(waitFor([&] {
        for (auto& m : a->s->members())
            if (m.name == "Cat") return m.relayed;
        return false;
    }, 7), "a member's list marks the relayed member");
    // A member's broadcast reaches the host and both other members (through the host); a message
    // to one member reaches only that one.
    const u8 ida = a->s->selfId(), idb = b->s->selfId(), idc = c->s->selfId();
    const std::vector<u8> bc = pattern(5000, 77), one = pattern(3000, 78);
    CHECK(b->s->sendApp(kBroadcast, 0x31, bc.data(), bc.size(), true), "broadcast");
    CHECK(a->s->sendApp(idc, 0x32, one.data(), one.size(), true), "member to member");
    bool gotH = false, gotA = false, gotC = false, gotOne = false, stray = false;
    waitFor([&] {
        AppMsg m;
        while (room.s->pollApp(m, 0x31, 0x32)) {
            if (m.type == 0x31) gotH = m.from == idb && m.data == bc;
            else stray = true;
        }
        while (a->s->pollApp(m, 0x31, 0x32)) {
            if (m.type == 0x31) gotA = m.from == idb && m.data == bc;
            else stray = true;
        }
        while (b->s->pollApp(m, 0x31, 0x32)) stray = true;
        while (c->s->pollApp(m, 0x31, 0x32)) {
            if (m.type == 0x31) gotC = m.from == idb && m.data == bc;
            else gotOne = m.from == ida && m.data == one;
        }
        return gotH && gotA && gotC && gotOne;
    }, 10);
    CHECK(gotH && gotA && gotC, "a member's broadcast: host %d, full cone %d, relayed %d", gotH, gotA, gotC);
    CHECK(gotOne, "member -> member through the host (direct member to relayed member)");
    CHECK(!stray, "a message reached someone it was not for");
    // The room is full now (host + 3).
    auto d = makePeer(kFull, ident("Dan", 504));
    const JoinOut od = joinRoom(*d->s, url, room.id, nullptr, "", 10);
    CHECK(od.state == State::Failed && contains(od.message, "full"), "fifth player: %s", od.message.c_str());
    std::printf("    fifth player: %s\n", od.message.c_str());
    leaveAndCheck(room, *b, "3 joiners: symmetric sequential");
    CHECK(waitFor([&] { return a->s->members().size() == 3 && c->s->members().size() == 3; }, 4), "the others see a member leave");
    leaveAndCheck(room, *c, "3 joiners: symmetric random");
    leaveAndCheck(room, *a, "3 joiners: full cone");
    summary("3 at once into a port-restricted host: full cone", "Joined", "direct", oa.secs);
    summary("3 at once into a port-restricted host: symmetric (sequential)", "Joined", "direct", ob.secs);
    summary("3 at once into a port-restricted host: symmetric (random)", "Joined", "relayed", oc.secs);
}

static void roomRules(const std::string& url) {
    std::printf("\n---- rooms: password, size, friends only, the list\n");
    // Password.
    {
        Room room;
        HostOptions o;
        o.password = "bells123";
        if (openRoom(room, kFull, url, o, ident("PwHost", 600))) {
            CHECK(room.listed.lock && !room.listed.friends && room.listed.max == 4 && room.listed.players == 1 && room.listed.game == kGame,
                  "the listing of a password room (lock %d, max %d)", room.listed.lock, room.listed.max);
            auto p = makePeer(kFull, ident("PwJoiner", 601));
            JoinOut r = joinRoom(*p->s, url, room.id, nullptr, "", 10);
            CHECK(r.state == State::Failed && contains(r.message, "has a password"), "no password: %s", r.message.c_str());
            std::printf("    no password given (%.2f s): %s\n", r.secs, r.message.c_str());
            r = joinRoom(*p->s, url, room.id, nullptr, "bells124", 10);
            CHECK(r.state == State::Failed && contains(r.message, "Wrong password"), "wrong password: %s", r.message.c_str());
            CHECK(r.secs < 3.0, "wrong password took %.2f s", r.secs);
            std::printf("    wrong password (%.2f s): %s\n", r.secs, r.message.c_str());
            summary("password room: wrong password", "Failed", "-", r.secs);
            r = joinRoom(*p->s, url, room.id, nullptr, "bells123", 15);
            CHECK(r.joined(), "right password: %s", r.message.c_str());
            if (r.joined()) {
                CHECK(p->s->snapshot().hasPassword, "the joiner knows the room has a password");
                checkConnected(room, *p, false, 2, "password room");
                leaveAndCheck(room, *p, "password room");
                summary("password room: right password (full cone / full cone)", "Joined", "direct", r.secs);
            }
        }
    }
    // Two players at most.
    {
        Room room;
        HostOptions o;
        o.maxMembers = 2;
        if (openRoom(room, kNoNat, url, o, ident("SmallHost", 610))) {
            CHECK(room.listed.max == 2, "listed max %d", room.listed.max);
            auto p1 = makePeer(kFull, ident("First", 611));
            auto p2 = makePeer(kRestricted, ident("Second", 612));
            JoinOut r = joinRoom(*p1->s, url, room.id, nullptr, "", 15);
            CHECK(r.joined(), "first of two: %s", r.message.c_str());
            r = joinRoom(*p2->s, url, room.id, nullptr, "", 10);
            CHECK(r.state == State::Failed && r.message == "The room is full", "second joiner of a 2-player room: %s", r.message.c_str());
            std::printf("    room for two, second joiner (%.2f s): %s\n", r.secs, r.message.c_str());
            summary("2-player room: second joiner", "Failed", "-", r.secs);
            if (p1->s->snapshot().state == State::Joined) {
                checkConnected(room, *p1, false, 2, "2-player room");
                leaveAndCheck(room, *p1, "2-player room");
                // A place is free again.
                r = joinRoom(*p2->s, url, room.id, nullptr, "", 15);
                CHECK(r.joined(), "after the first left: %s", r.message.c_str());
            }
        }
    }
    // Friends only.
    {
        u8 hostSeed[32], friendSeed[32], strangerSeed[32], hostPub[32], friendPub[32], strangerPub[32];
        onlinecrypto::randomBytes(hostSeed, 32);
        onlinecrypto::randomBytes(friendSeed, 32);
        onlinecrypto::randomBytes(strangerSeed, 32);
        onlinecrypto::ed25519PublicKey(hostSeed, hostPub);
        onlinecrypto::ed25519PublicKey(friendSeed, friendPub);
        onlinecrypto::ed25519PublicKey(strangerSeed, strangerPub);
        Room room;
        HostOptions o;
        o.friendsOnly = true;
        o.roomName = "Secret garden";
        if (openRoom(room, kRestricted, url, o, identKey("FriendHost", 620, hostSeed))) {
            std::array<u8, 32> fk;
            std::memcpy(fk.data(), friendPub, 32);
            room.s->setAllowedKeys({fk});
            const lobby::Room& l = room.listed;
            CHECK(l.friends && l.name.empty() && l.host.empty() && l.town.empty(), "a friends-only room is listed without names (\"%s\", \"%s\", \"%s\")",
                  l.name.c_str(), l.host.c_str(), l.town.c_str());
            CHECK(lobby::tagMatches(l.tag, hostPub), "the tag matches the host's key");
            CHECK(!lobby::tagMatches(l.tag, strangerPub) && !lobby::tagMatches(l.tag, friendPub), "the tag matches no other key");
            auto fr = makePeer(kRestricted, identKey("Friend", 621, friendSeed));
            JoinOut r = joinRoom(*fr->s, url, room.id, hostPub, "", 15);
            CHECK(r.joined(), "the friend (expecting the host's key): %s", r.message.c_str());
            if (r.joined()) {
                const Snapshot s = fr->s->snapshot();
                CHECK(s.hostKeyKnown && std::memcmp(s.hostKey, hostPub, 32) == 0, "the joiner knows the host's proved key");
                checkConnected(room, *fr, false, 2, "friends-only room");
                leaveAndCheck(room, *fr, "friends-only room");
                summary("friends-only room: the friend (port-restricted / port-restricted)", "Joined", "direct", r.secs);
            }
            auto stranger = makePeer(kFull, identKey("Stranger", 622, strangerSeed));
            r = joinRoom(*stranger->s, url, room.id, nullptr, "", 10);
            CHECK(r.state == State::Failed && contains(r.message, "friends only"), "a stranger: %s", r.message.c_str());
            std::printf("    stranger with another key (%.2f s): %s\n", r.secs, r.message.c_str());
            summary("friends-only room: a stranger's key", "Failed", "-", r.secs);
            auto nokey = makePeer(kFull, ident("NoKey", 623));
            r = joinRoom(*nokey->s, url, room.id, nullptr, "", 10);
            CHECK(r.state == State::Failed && contains(r.message, "friends only"), "a joiner without a key: %s", r.message.c_str());
            // The stranger shows the friend's key but cannot sign with it.
            Identity liar = identKey("Liar", 624, strangerSeed);
            std::memcpy(liar.edPub, friendPub, 32);
            auto lp = makePeer(kFull, liar);
            r = joinRoom(*lp->s, url, room.id, nullptr, "", 10);
            CHECK(r.state == State::Failed && contains(r.message, "friends only"), "a borrowed public key: %s", r.message.c_str());
            // The friend expects another host.
            r = joinRoom(*fr->s, url, room.id, strangerPub, "", 10);
            CHECK(r.state == State::Failed && contains(r.message, "key does not match"), "wrong expected host key: %s", r.message.c_str());
            std::printf("    expecting another host's key (%.2f s): %s\n", r.secs, r.message.c_str());
            summary("friends-only room: wrong expected host key", "Failed", "-", r.secs);
            r = joinRoom(*fr->s, url, room.id, hostPub, "", 15);
            CHECK(r.joined(), "the friend again: %s", r.message.c_str());
            CHECK(room.s->members().size() == 2, "only the friend is in the room (%zu)", room.s->members().size());
        }
    }
    // "Ask me before someone joins": the host's answer reaches the joiner through the lobby.
    {
        Room room;
        HostOptions o;
        o.askBeforeJoin = true;
        if (openRoom(room, kFull, url, o, ident("AskHost", 640))) {
            auto yes = makePeer(kRestricted, ident("Welcome", 641));
            auto no = makePeer(kFull, ident("Unwelcome", 642));
            auto t0 = Clock::now();
            joinBegin(*yes->s, url, room.id, nullptr, "");
            CHECK(waitFor([&] { return yes->s->snapshot().state == State::WaitingForHost; }, 4), "waiting for the host: %s", yes->s->snapshot().message.c_str());
            CHECK(waitFor([&] { return room.s->snapshot().pending.size() == 1; }, 3), "the host sees the request");
            CHECK(room.s->members().size() == 1, "nobody is in before the host said yes");
            sleepMs(1200);
            if (!room.s->snapshot().pending.empty()) room.s->answerJoinRequest(room.s->snapshot().pending[0].ticket, true);
            t0 = Clock::now();
            CHECK(waitFor([&] { return settled(*yes->s); }, 12) && yes->s->snapshot().state == State::Joined, "let in by the host: %s",
                  yes->s->snapshot().message.c_str());
            const double tYes = since(t0);
            if (yes->s->snapshot().state == State::Joined) {
                checkConnected(room, *yes, false, 2, "ask before join");
                summary("ask-before-join room: after the host's yes (port-restricted / full cone)", "Joined", "direct", tYes);
            }
            joinBegin(*no->s, url, room.id, nullptr, "");
            CHECK(waitFor([&] { return no->s->snapshot().state == State::WaitingForHost; }, 4), "second request waiting");
            CHECK(waitFor([&] { return room.s->snapshot().pending.size() == 1; }, 3), "the host sees the second request");
            if (!room.s->snapshot().pending.empty()) room.s->answerJoinRequest(room.s->snapshot().pending[0].ticket, false);
            t0 = Clock::now();
            const bool told = waitFor([&] { return settled(*no->s); }, 12);
            const Snapshot s = no->s->snapshot();
            CHECK(told && s.state == State::Failed && contains(s.message, "declined"), "declined by the host: state %d, %s", int(s.state), s.message.c_str());
            std::printf("    declined by the host (%.2f s after the answer): %s\n", since(t0), s.message.c_str());
            summary("ask-before-join room: after the host's no", told ? "Failed" : "NO ANSWER", "-", since(t0));
            CHECK(room.s->members().size() == 2 && room.s->snapshot().pending.empty(), "the room after the two answers");
            // A joiner that stops waiting takes its request back.
            auto gone = makePeer(kNoNat, ident("Impatient", 643));
            joinBegin(*gone->s, url, room.id, nullptr, "");
            CHECK(waitFor([&] { return room.s->snapshot().pending.size() == 1 && gone->s->snapshot().state == State::WaitingForHost; }, 4), "third request waiting");
            if (!room.s->snapshot().pending.empty()) {
                const JoinRequest q = room.s->snapshot().pending[0];
                CHECK(q.name == "Impatient" && q.address.rfind("127.0.0.1:", 0) == 0, "the request as the host sees it: %s from %s", q.name.c_str(), q.address.c_str());
                // While it waits, strangers send it forged refusals and noise (no router here: its
                // socket takes them): only the lobby's word counts.
                if (q.address.rfind("127.0.0.1:", 0) == 0) {
                    using namespace online::sock;
                    Addr to;
                    resolve(q.address, 0, &to);
                    const Socket u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    for (int i = 0; i < 20; i++) {
                        std::vector<u8> d = {'C', 'F', 2, 4 /*REJECT*/, u8(1 + i % 10), 5, 'p', 'w', 'n', 'e', 'd'};
                        sendto(u, reinterpret_cast<const char*>(d.data()), int(d.size()), 0, reinterpret_cast<const sockaddr*>(&to.ss), to.len);
                    }
                    closeSocket(u);
                    fuzzUdp(q.address, 200, 0);
                    sleepMs(300);
                    const Snapshot gs = gone->s->snapshot();
                    CHECK(gs.state == State::WaitingForHost, "a waiting joiner after forged refusals and noise: state %d, %s", int(gs.state), gs.message.c_str());
                }
            }
            gone->s->leave();
            CHECK(waitFor([&] { return room.s->snapshot().pending.empty(); }, 3), "a joiner that stopped waiting is no longer asked about");
        }
    }
    // The host's actions: the listing follows the player count; a kicked member cannot come
    // back; closing the room tells the members.
    {
        Room room;
        if (openRoom(room, kNoNat, url, HostOptions{}, ident("ActHost", 650))) {
            auto p = makePeer(kFull, ident("Kicked", 651));
            auto q = makePeer(kSymRand, ident("Stays", 652));
            JoinOut r = joinRoom(*p->s, url, room.id, nullptr, "", 15);
            CHECK(r.joined(), "first member: %s", r.message.c_str());
            r = joinRoom(*q->s, url, room.id, nullptr, "", 15);
            CHECK(r.joined(), "second member: %s", r.message.c_str());
            lobby::RoomInfo info;
            info.name = room.name;
            info.host = "ActHost";
            info.town = "Testville";
            info.game = kGame;
            info.ver = "test";
            info.players = 3;
            info.max = 4;
            info.tag = "0000000000000000";
            room.chan->update(info);
            const std::string id = room.id;
            CHECK(findRoom(url, [&](const lobby::Room& x) { return x.id == id && x.players == 3; }, nullptr, 4), "the listing shows 3 players after update()");
            Member km;
            CHECK(waitFor([&] { return hostMember(*room.s, "Kicked", &km) && km.pid == 651; }, 4), "the host knows the member's profile id");
            if (hostMember(*room.s, "Kicked", &km)) room.s->kick(km.id);
            CHECK(waitFor([&] { return p->s->snapshot().state == State::Failed; }, 5) && contains(p->s->snapshot().message, "removed"), "kicked: %s",
                  p->s->snapshot().message.c_str());
            CHECK(waitFor([&] { return q->s->members().size() == 2; }, 5), "the other member sees the kick");
            r = joinRoom(*p->s, url, room.id, nullptr, "", 10);
            CHECK(r.state == State::Failed && contains(r.message, "removed"), "a kicked player coming back: %s", r.message.c_str());
            std::printf("    kicked player joining again (%.2f s): %s\n", r.secs, r.message.c_str());
            room.chan->stop();
            room.s->leave();
            CHECK(waitFor([&] { return q->s->snapshot().state == State::Failed; }, 6) && contains(q->s->snapshot().message, "closed"),
                  "the member of a closed room: %s", q->s->snapshot().message.c_str());
            std::printf("    member of a room the host closed: %s\n", q->s->snapshot().message.c_str());
        }
    }
    // The list: a closed room and a host that dropped are gone.
    {
        Room a, b;
        const bool okA = openRoom(a, kNoNat, url, HostOptions{}, ident("CloseHost", 630));
        const bool okB = openRoom(b, kNoNat, url, HostOptions{}, ident("DropHost", 631));
        if (okA && okB) {
            const std::string idA = a.id, idB = b.id;
            a.chan->stop();
            a.s->leave();
            auto gone = [&](const std::string& id) {
                return waitFor([&] {
                    for (auto& r : fetchRooms(url))
                        if (r.id == id) return false;
                    return true;
                }, 4);
            };
            CHECK(gone(idA), "a closed room leaves the list");
            bool stillB = false;
            for (auto& r : fetchRooms(url)) stillB |= r.id == idB;
            CHECK(stillB, "the other room stays listed");
            b.chan->stop();  // the lobby connection drops; the session still hosts
            CHECK(gone(idB), "a host that dropped leaves the list");
            CHECK(b.s->snapshot().state == State::Hosting, "the session itself still hosts");
            auto p = makePeer(kNoNat, ident("Late", 632));
            JoinOut r = joinRoom(*p->s, url, idA, nullptr, "", 10);
            CHECK(r.state == State::Failed && contains(r.message, "no longer open"), "joining a closed room: %s", r.message.c_str());
            std::printf("    closed room (%.2f s): %s\n", r.secs, r.message.c_str());
            // The dropped host lists its room again (a new lobby connection): a new id, and joins work.
            lobby::RoomInfo info;
            info.name = b.name;
            info.host = "DropHost";
            info.game = kGame;
            info.ver = "test";
            info.tag = "0000000000000000";
            b.chan = lobby::HostChannel::start(url, info);
            b.s->attachSignaling(b.chan);
            const std::string name = b.name;
            lobby::Room again;
            CHECK(findRoom(url, [&](const lobby::Room& x) { return x.name == name; }, &again, 6), "listed again");
            CHECK(!again.id.empty() && again.id != idB, "a new listing has a new id");
            r = joinRoom(*p->s, url, idB, nullptr, "", 10);
            CHECK(r.state == State::Failed && contains(r.message, "no longer open"), "the old id: %s", r.message.c_str());
            r = joinRoom(*p->s, url, again.id, nullptr, "", 15);
            CHECK(r.joined(), "joining the room that was listed again: %s", r.message.c_str());
        }
    }
}

static void noRelay(const std::string& urlNoTurn) {
    const std::string label = "9  symmetric (random) / symmetric (random), lobby without a relay";
    std::printf("\n---- %s\n", label.c_str());
    Room room;
    if (!openRoom(room, kSymRand, urlNoTurn, HostOptions{}, ident("Host", 900))) return summary(label, "NO ROOM", "-", -1);
    auto p = makePeer(kSymRand, ident("Joiner", 901));
    const uint64_t bytes0 = gTurnA.relayedBytes();
    JoinOut o = joinRoom(*p->s, urlNoTurn, room.id, nullptr, "", 30);
    CHECK(o.state == State::Failed, "no path and no relay must fail (state %d)", int(o.state));
    CHECK(contains(o.message, "no relay"), "the message names the missing relay: %s", o.message.c_str());
    CHECK(o.secs < 16.0, "failed after %.2f s", o.secs);
    CHECK(gTurnA.relayedBytes() == bytes0 && gTurnA.allocations() == 0, "nothing went through a relay");
    std::printf("    failed after %.2f s: %s\n", o.secs, o.message.c_str());
    routerStats(room, *p);
    summary(label, "Failed", "-", o.secs);
    // The joiner told the host through the lobby that it gave up: its place is free at once.
    CHECK(waitFor([&] { return room.s->members().size() == 1; }, 3), "the host drops the joiner that gave up (%zu members listed)",
          room.s->members().size());
    // The same for a joiner that cancels while the path is being opened.
    {
        auto c = makePeer(kSymRand, ident("Canceller", 903));
        joinBegin(*c->s, urlNoTurn, room.id, nullptr, "");
        CHECK(waitFor([&] { return hostMember(*room.s, "Canceller"); }, 5), "the host let the second joiner in");
        sleepMs(700);
        CHECK(c->s->snapshot().state == State::Connecting, "still opening a path (state %d)", int(c->s->snapshot().state));
        c->s->leave();
        CHECK(waitFor([&] { return !hostMember(*room.s, "Canceller"); }, 3), "the host drops a joiner that cancelled");
    }
    // The host is still usable: someone behind a full-cone router gets in (directly: the host's
    // packets open the way).
    CHECK(room.s->snapshot().state == State::Hosting && room.chan->listed(), "the host still hosts and is listed");
    auto q = makePeer(kFull, ident("NextJoiner", 902));
    o = joinRoom(*q->s, urlNoTurn, room.id, nullptr, "", 20);
    CHECK(o.joined(), "a full-cone joiner after the failed one: %s", o.message.c_str());
    if (o.joined()) {
        sleepMs(150);
        checkConnected(room, *q, false, room.s->members().size(), "full cone into the symmetric host after the failed join");
        leaveAndCheck(room, *q, "after the failed join");
        summary("9b full cone joiner / the same symmetric (random) host, no relay", "Joined", "direct", o.secs);
    }
    room.close();
    natCase("9c full cone / full cone on the lobby without a relay", kFull, kFull, 0, urlNoTurn);
}

// A relayed pair moves to the direct path once one opens (here: the forced-relay switch goes off).
static void relayThenDirect(const std::string& url) {
    const std::string label = "8b the same pair once the forced relay is switched off";
    std::printf("\n---- %s\n", label.c_str());
    setForceRelay(true);
    Room room;
    auto p = makePeer(kFull, ident("Joiner", 811));
    if (!openRoom(room, kFull, url, HostOptions{}, ident("Host", 810))) {
        setForceRelay(false);
        return summary(label, "NO ROOM", "-", -1);
    }
    const JoinOut o = joinRoom(*p->s, url, room.id, nullptr, "", 30);
    CHECK(o.joined(), "%s", o.message.c_str());
    if (o.joined()) {
        sleepMs(150);
        checkConnected(room, *p, true, 2, "forced relay");
        setForceRelay(false);
        const auto t0 = Clock::now();
        Member hm;
        const bool moved = waitFor([&] { return !p->s->snapshot().relayed && hostMember(*room.s, p->name, &hm) && !hm.relayed; }, 25);
        const double secs = since(t0);
        CHECK(moved, "both sides move to the direct path (joiner %s, host %s)", p->s->snapshot().relayed ? "relayed" : "direct", hm.relayed ? "relayed" : "direct");
        const uint64_t bytes0 = gTurnA.relayedBytes();
        exchange(*room.s, *p->s, p->s->selfId(), "after the move to the direct path");
        const uint64_t through = gTurnA.relayedBytes() - bytes0;
        CHECK(through < 2000, "after the move %llu bytes still went through the relay", (unsigned long long)through);
        std::printf("    moved to the direct path %.1f s after the switch; the host now sends to %s\n", secs, hm.address.c_str());
        leaveAndCheck(room, *p, label.c_str());
        summary(label, moved ? "Joined" : "FAILED", moved ? "direct" : "relayed", secs);
    }
    setForceRelay(false);
}

// The joiner's router forgets its mappings (a restart): the joiner's packets now come from another
// port. Both sides must find the path again without the lobby.
static void routerRestart(const std::string& url) {
    const std::string label = "10 full cone / full cone, the joiner's router restarts";
    std::printf("\n---- %s\n", label.c_str());
    Room room;
    auto p = makePeer(kFull, ident("Joiner", 821));
    if (!openRoom(room, kFull, url, HostOptions{}, ident("Host", 820))) return summary(label, "NO ROOM", "-", -1);
    const JoinOut o = joinRoom(*p->s, url, room.id, nullptr, "", 30);
    CHECK(o.joined(), "%s", o.message.c_str());
    if (!o.joined()) return summary(label, "FAILED", "-", o.secs);
    sleepMs(150);
    checkConnected(room, *p, false, 2, "before the restart");
    Member before, after;
    hostMember(*room.s, p->name, &before);
    p->nat->restart();
    const auto t0 = Clock::now();
    const bool ok = exchange(*room.s, *p->s, p->s->selfId(), "after the router restart");
    const double secs = since(t0);
    hostMember(*room.s, p->name, &after);
    CHECK(ok && p->s->snapshot().state == State::Joined && room.s->members().size() == 2, "the pair survives the restart");
    CHECK(after.address != before.address, "the host follows the joiner to its new address (%s -> %s)", before.address.c_str(), after.address.c_str());
    std::printf("    the joiner's address moved %s -> %s; 50,000 bytes each way were through %.1f s after the restart\n", before.address.c_str(),
                after.address.c_str(), secs);
    CHECK(secs < 12, "recovery took %.1f s", secs);
    leaveAndCheck(room, *p, label.c_str());
    summary(label, ok ? "Joined" : "FAILED", "direct", secs);
}

// The host's answers get lost once on their way through the lobby: the joiner asks again and the
// path still opens (the host has been sending to the joiner's addresses for seconds by then).
static void lostAnswers(const std::string& url) {
    const std::string label = "12 joiner symmetric (sequential) / host port-restricted, CHALLENGE and ACCEPT lost once";
    std::printf("\n---- %s\n", label.c_str());
    Room room;
    auto p = makePeer(kSymSeq, ident("Unlucky", 841));
    if (!openRoom(room, kRestricted, url, HostOptions{}, ident("Host", 840))) return summary(label, "NO ROOM", "-", -1);
    auto lossy = std::make_shared<Lossy>(lobby::joinChannel(url, room.id), std::vector<u8>{2 /*CHALLENGE*/, 6 /*ACCEPT*/});
    const JoinOut o = joinRoom(*p->s, url, room.id, nullptr, "", 30, lossy);
    CHECK(o.joined(), "%s", o.message.c_str());
    CHECK(lossy->lost == 2, "answers lost: %d", lossy->lost);
    if (o.joined()) {
        sleepMs(150);
        const bool relayed = p->s->snapshot().relayed;
        checkConnected(room, *p, relayed, 2, label.c_str());
        leaveAndCheck(room, *p, label.c_str());
        summary(label, "Joined", relayed ? "relayed" : "direct", o.secs);
    } else {
        summary(label, "FAILED", "-", o.secs);
    }
    routerStats(room, *p);
}

// Connections that end without a goodbye (a cable pulled): the others must notice within the
// transport's ordinary silence limit (5 s with the test timers), relayed members as well.
static void vanish(const std::string& url) {
    const std::string label = "11 a member and then the host vanish (no goodbye)";
    std::printf("\n---- %s\n", label.c_str());
    Room room;
    auto a = makePeer(kFull, ident("Stays", 831));
    auto b = makePeer(kFull, ident("Vanishes", 832));
    auto c = makePeer(kSymRand, ident("Relayed", 833));
    if (!openRoom(room, kRestricted, url, HostOptions{}, ident("Host", 830))) return summary(label, "NO ROOM", "-", -1);
    const JoinOut oa = joinRoom(*a->s, url, room.id, nullptr, "", 30);
    const JoinOut ob = joinRoom(*b->s, url, room.id, nullptr, "", 30);
    const JoinOut oc = joinRoom(*c->s, url, room.id, nullptr, "", 30);
    CHECK(oa.joined() && ob.joined() && oc.joined(), "three members: %s / %s / %s", oa.message.c_str(), ob.message.c_str(), oc.message.c_str());
    if (!oa.joined() || !ob.joined() || !oc.joined()) return summary(label, "FAILED", "-", -1);
    CHECK(waitFor([&] { return a->s->members().size() == 4 && c->s->snapshot().relayed; }, 5), "everyone listed, the third member relayed");
    // One member's line goes dead.
    b->nat->unplug(true);
    auto t0 = Clock::now();
    const bool hostSaw = waitFor([&] { return !hostMember(*room.s, "Vanishes"); }, 45);
    const double tHost = since(t0);
    const bool memberSaw = waitFor([&] { return b->s->snapshot().state == State::Failed; }, 45);
    const double tMember = since(t0);
    CHECK(hostSaw && tHost < 12, "the host drops the silent member after %.1f s", tHost);
    CHECK(memberSaw && tMember < 12, "the cut-off member gives up after %.1f s: %s", tMember, b->s->snapshot().message.c_str());
    CHECK(waitFor([&] { return a->s->members().size() == 3 && c->s->members().size() == 3; }, 6), "the others see it go");
    std::printf("    silent member: dropped by the host after %.1f s; it reports \"%s\" after %.1f s\n", tHost, b->s->snapshot().message.c_str(), tMember);
    summary("11 a member vanishes: until the host drops it", hostSaw ? "dropped" : "KEPT", "-", tHost);
    exchange(*room.s, *a->s, a->s->selfId(), "the room after a member vanished");
    // The host's line goes dead: the direct and the relayed member both notice.
    room.nat->unplug(true);
    t0 = Clock::now();
    const bool aSaw = waitFor([&] { return a->s->snapshot().state == State::Failed; }, 45);
    const double tA = since(t0);
    const bool cSaw = waitFor([&] { return c->s->snapshot().state == State::Failed; }, 45);
    const double tC = since(t0);
    CHECK(aSaw && tA < 12, "the direct member notices the dead host after %.1f s", tA);
    CHECK(cSaw && tC < 12, "the relayed member notices the dead host after %.1f s", tC);
    std::printf("    dead host: the direct member reports \"%s\" after %.1f s, the relayed one after %.1f s\n", a->s->snapshot().message.c_str(), tA, tC);
    summary("11 the host vanishes: until a member reports it", aSaw && cSaw ? "Failed" : "HANGS", "-", std::max(tA, tC));
}

// Not pass / fail: how the weak spots of the design behave (cf_online_lobby_test --observe).
static void observations(const std::string& url) {
    std::printf("\n---- observations (reported, not checked)\n");
    // A second joiner into a host behind a symmetric router with sequential ports: the host's
    // STUN answers (and so its predicted ports) are from before the first joiner.
    {
        Room room;
        if (openRoom(room, kSymSeq, url, HostOptions{}, ident("Host", 950))) {
            std::string seen;
            for (int i = 1; i <= 9; i++) {
                auto p = makePeer(kRestricted, ident("Visitor" + std::to_string(i), u32(950 + i)));
                const JoinOut a = joinRoom(*p->s, url, room.id, nullptr, "", 30);
                sleepMs(300);
                const bool relayed = p->s->snapshot().relayed;
                seen += a.joined() ? (relayed ? " relayed" : " direct") : " FAILED";
                char label[120];
                std::snprintf(label, sizeof label, "obs: port-restricted joiner no. %d / one symmetric (sequential) host", i);
                summary(label, a.joined() ? "Joined" : "Failed", relayed ? "relayed" : "direct", a.secs);
                p->s->leave();
                waitFor([&] { return room.s->members().size() == 1; }, 6);
            }
            std::printf("    one after the other into the same host:%s\n    host router: %s\n", seen.c_str(), room.nat->stats().c_str());
        }
    }
    {
        Room room;
        if (openRoom(room, kSymSeq, url, HostOptions{}, ident("Host", 960))) {
            auto p = makePeer(kSymSeq, ident("Joiner", 961));
            const JoinOut a = joinRoom(*p->s, url, room.id, nullptr, "", 30);
            sleepMs(300);
            summary("obs: symmetric (sequential) / symmetric (sequential)", a.joined() ? "Joined" : "Failed", p->s->snapshot().relayed ? "relayed" : "direct", a.secs);
        }
    }
    // Wrong passwords through the lobby are counted for all lobby joiners together.
    {
        Room room;
        HostOptions o;
        o.password = "right";
        if (openRoom(room, kNoNat, url, o, ident("Host", 970))) {
            auto bad = makePeer(kNoNat, ident("Guesser", 971));
            auto good = makePeer(kNoNat, ident("Friend", 972));
            int tries = 0;
            JoinOut r;
            for (; tries < 12; tries++) {
                r = joinRoom(*bad->s, url, room.id, nullptr, "wrong" + std::to_string(tries), 10);
                if (!contains(r.message, "Wrong password")) break;
            }
            std::printf("    after %d wrong passwords the guesser gets: %s\n", tries, r.message.c_str());
            r = joinRoom(*good->s, url, room.id, nullptr, "right", 10);
            std::printf("    someone else with the right password then gets: %s (%s)\n", r.joined() ? "Joined" : "Failed", r.message.c_str());
            summary("obs: right password after another player's 8 wrong ones", r.joined() ? "Joined" : "Failed", "-", r.secs);
        }
    }
}

static void robustness(const std::string& url) {
    std::printf("\n---- robustness: bad signals, replays, random datagrams\n");
    Room room;
    if (!openRoom(room, kNoNat, url, HostOptions{}, ident("Host", 700))) return;
    // Two live members whose handshakes are recorded: one through the game's own lobby channel, one
    // through a channel whose peer id the test knows.
    auto p1 = makePeer(kNoNat, ident("Recorded", 701));
    auto rec1 = std::make_shared<Recorder>(lobby::joinChannel(url, room.id));
    JoinOut o = joinRoom(*p1->s, url, room.id, nullptr, "", 15, rec1);
    CHECK(o.joined(), "recorded join: %s", o.message.c_str());
    auto raw = std::make_shared<RawSignaling>();
    raw->url = url;
    raw->room = room.id;
    raw->peer = randomPeer();
    auto rec2 = std::make_shared<Recorder>(raw);
    auto p2 = makePeer(kNoNat, ident("Known", 702));
    o = joinRoom(*p2->s, url, room.id, nullptr, "", 15, rec2);
    CHECK(o.joined(), "join with a known peer id: %s", o.message.c_str());
    const std::vector<u8> hello = rec1->first(true, T_HELLO), auth = rec1->first(true, T_AUTH);
    const std::vector<u8> challenge = rec1->first(false, T_CHALLENGE), accept = rec1->first(false, T_ACCEPT);
    const std::vector<u8> hello2 = rec2->first(true, T_HELLO), auth2 = rec2->first(true, T_AUTH), accept2 = rec2->first(false, T_ACCEPT);
    CHECK(!hello.empty() && !auth.empty() && !challenge.empty() && !accept.empty() && !hello2.empty() && !auth2.empty() && !accept2.empty(),
          "handshake recorded (%zu %zu %zu %zu)", hello.size(), auth.size(), challenge.size(), accept.size());
    if (hello.empty() || auth.empty() || challenge.empty() || accept.empty() || hello2.empty() || auth2.empty() || accept2.empty()) return;
    std::printf("    recorded: HELLO %zu bytes, CHALLENGE %zu, AUTH %zu, ACCEPT %zu\n", hello.size(), challenge.size(), auth.size(), accept.size());

    // What each raw request must come back as.
    enum Want { Nothing, Challenge, Reject, SameAccept, LobbyRefuses };
    struct Shot {
        const char* what;
        std::string peer;
        std::vector<std::vector<u8>> datagrams;  // sent one after the other; the last one's answer counts
        Want want;
        std::string text;  // instead of datagrams: the "data" string as it is
        SigAnswer answer;
    };
    auto fire = [&](std::vector<Shot>& shots) {
        std::vector<std::thread> ths;
        for (auto& s : shots)
            ths.emplace_back([&s, &url, &room] {
                if (s.datagrams.empty()) s.answer = postSignalText(url, room.id, s.peer, s.text);
                for (auto& d : s.datagrams) s.answer = postSignal(url, room.id, s.peer, d);
            });
        for (auto& t : ths) t.join();
        for (auto& s : shots) {
            const SigAnswer& a = s.answer;
            const u8 type = a.data.size() >= 4 ? a.data[3] : 0;
            // Never an ACCEPT to anyone but the member it was made for.
            CHECK(type != T_ACCEPT || s.want == SameAccept, "%s: the host answered with an ACCEPT", s.what);
            switch (s.want) {
            case Nothing:
                CHECK(a.status == 504 && a.data.empty(), "%s: expected no answer, got HTTP %d (%zu bytes, type %u)", s.what, a.status, a.data.size(), type);
                break;
            case Challenge:
                CHECK(a.status == 200 && type == T_CHALLENGE && a.data != challenge, "%s: expected a fresh CHALLENGE, got HTTP %d type %u", s.what, a.status, type);
                break;
            case Reject:
                CHECK(a.status == 200 && type == T_REJECT, "%s: expected a REJECT, got HTTP %d type %u", s.what, a.status, type);
                break;
            case SameAccept:
                CHECK(a.status == 504 || (a.status == 200 && a.data == accept2), "%s: expected the old ACCEPT again or nothing, got HTTP %d type %u", s.what, a.status, type);
                break;
            case LobbyRefuses:
                CHECK(a.status == 400 || a.status == 413, "%s: expected the lobby to refuse, got HTTP %d", s.what, a.status);
                break;
            }
        }
    };
    auto cut = [](const std::vector<u8>& d, size_t n) { return std::vector<u8>(d.begin(), d.begin() + long(std::min(n, d.size()))); };
    auto proto = [](std::vector<u8> d, u8 v) {
        d[2] = v;
        return d;
    };
    {
        std::vector<Shot> s;
        s.push_back({"one random byte", randomPeer(), {randomBytes(1)}, Nothing});
        s.push_back({"header only", randomPeer(), {{'C', 'F', 2}}, Nothing});
        s.push_back({"64 random bytes", randomPeer(), {randomBytes(64)}, Nothing});
        s.push_back({"1400 random bytes", randomPeer(), {randomBytes(1400)}, Nothing});
        s.push_back({"a 2000-byte datagram (over the transport's size)", randomPeer(), {typed(T_HELLO, 1996)}, Nothing});
        s.push_back({"sealed data through the lobby", randomPeer(), {typed(T_SEALED, 100)}, Nothing});
        s.push_back({"random AUTH", randomPeer(), {typed(T_AUTH, 200)}, Nothing});
        s.push_back({"random CAND", randomPeer(), {typed(T_CAND, 100)}, Nothing});
        fire(s);
    }
    {
        std::vector<Shot> s;
        s.push_back({"HELLO cut to 4 bytes", randomPeer(), {cut(hello, 4)}, Nothing});
        s.push_back({"HELLO cut to 12 bytes", randomPeer(), {cut(hello, 12)}, Nothing});
        s.push_back({"HELLO cut in half", randomPeer(), {cut(hello, hello.size() / 2)}, Nothing});
        s.push_back({"HELLO one byte short", randomPeer(), {cut(hello, hello.size() - 1)}, Nothing});
        s.push_back({"HELLO with protocol byte 1", randomPeer(), {proto(hello, 1)}, Nothing});
        s.push_back({"HELLO with protocol byte 3", randomPeer(), {proto(hello, 3)}, Nothing});
        s.push_back({"AUTH replayed without a HELLO", randomPeer(), {auth}, Nothing});
        s.push_back({"AUTH cut short without a HELLO", randomPeer(), {cut(auth, 40)}, Nothing});
        fire(s);
    }
    {
        std::vector<Shot> s;
        s.push_back({"HELLO replayed by another peer", randomPeer(), {hello}, Challenge});
        s.push_back({"HELLO + AUTH replayed by another peer", randomPeer(), {hello, auth}, Reject});
        s.push_back({"HELLO, then AUTH cut inside its signature", randomPeer(), {hello, cut(auth, 60)}, Nothing});
        s.push_back({"HELLO, then AUTH without its last bytes", randomPeer(), {hello, cut(auth, auth.size() - 5)}, Reject});
        s.push_back({"the host's own CHALLENGE sent back", randomPeer(), {challenge}, Nothing});
        s.push_back({"the host's own ACCEPT sent back", randomPeer(), {accept}, Nothing});
        s.push_back({"HELLO replayed with a live member's peer id", raw->peer, {hello2}, Nothing});
        Shot big{"data over the lobby's size limit", randomPeer(), {}, LobbyRefuses};
        big.text = b64(randomBytes(2400));
        s.push_back(big);
        Shot empty{"empty data", randomPeer(), {}, Nothing};
        s.push_back(empty);
        fire(s);
    }
    {
        std::vector<Shot> s;
        s.push_back({"AUTH replayed with a live member's peer id", raw->peer, {auth2}, SameAccept});
        Shot bad{"data that is not base64", randomPeer(), {}, LobbyRefuses};
        bad.text = "****";
        s.push_back(bad);
        s.push_back({"HELLO with a wrong protocol byte, then the real one", randomPeer(), {proto(hello, 9), hello}, Challenge});
        fire(s);
    }
    {
        // "This joiner gave up" (type 9) and "my relay address" need the member's key.
        std::vector<u8> cand = typed(T_CAND, 16 + 2 + 22);
        cand[20] = 22;  // the sealed part's length, as a real one has it
        cand[21] = 0;
        std::vector<Shot> s;
        s.push_back({"forged GONE and CAND with a live member's peer id", raw->peer, {typed(9, 16 + 1 + 16), cand}, Nothing});
        s.push_back({"forged GONE from an unknown peer", randomPeer(), {typed(9, 16 + 1 + 16)}, Nothing});
        fire(s);
    }
    // The room is as before.
    CHECK(room.s->snapshot().state == State::Hosting && room.chan->listed(), "the host after the bad signals");
    CHECK(room.s->members().size() == 3, "members after the bad signals: %zu", room.s->members().size());
    CHECK(p1->s->snapshot().state == State::Joined && p2->s->snapshot().state == State::Joined, "both members still joined");
    exchange(*room.s, *p1->s, p1->s->selfId(), "after the bad signals");

    // Random datagrams straight at the sockets (no routers here: everything reaches the sessions).
    Member m1, m2;
    CHECK(hostMember(*room.s, "Recorded", &m1) && hostMember(*room.s, "Known", &m2), "members' addresses");
    const std::string hostAddr = "127.0.0.1:" + std::to_string(room.port);
    std::printf("    200 random datagrams each to the host %s and the members %s, %s\n", hostAddr.c_str(), m1.address.c_str(), m2.address.c_str());
    fuzzUdp(hostAddr, 200, m1.id);
    fuzzUdp(m1.address, 200, 0);
    fuzzUdp(m2.address, 200, m2.id);
    sleepMs(300);
    CHECK(room.s->snapshot().state == State::Hosting && room.s->members().size() == 3, "the host after the random datagrams (%zu members)",
          room.s->members().size());
    CHECK(p1->s->snapshot().state == State::Joined && p2->s->snapshot().state == State::Joined, "both members after the random datagrams: %s / %s",
          p1->s->snapshot().message.c_str(), p2->s->snapshot().message.c_str());
    exchange(*room.s, *p1->s, p1->s->selfId(), "after the random datagrams (first member)");
    exchange(*room.s, *p2->s, p2->s->selfId(), "after the random datagrams (second member)");
    // And a normal join still works.
    auto p3 = makePeer(kFull, ident("Afterwards", 703));
    o = joinRoom(*p3->s, url, room.id, nullptr, "", 15);
    CHECK(o.joined(), "a normal join after all that: %s", o.message.c_str());
    if (o.joined()) {
        checkConnected(room, *p3, false, 4, "join after the robustness part");
        leaveAndCheck(room, *p3, "join after the robustness part");
    }
    summary("robustness: join after bad signals, replays and 600 random datagrams", o.joined() ? "Joined" : "FAILED", "direct", o.secs);
    // A room listed on the lobby still takes a room code and a direct connection.
    {
        Session byCode, byAddress;
        byCode.setIdentity(ident("ByCode", 705));
        byAddress.setIdentity(ident("ByAddress", 706));
        std::string err;
        CHECK(byCode.joinCode(room.s->snapshot().code, "", &err), "joinCode: %s", err.c_str());
        CHECK(waitFor([&] { return byCode.snapshot().state == State::Joined; }, 6), "room code into a lobby room: %s", byCode.snapshot().message.c_str());
        CHECK(!byCode.snapshot().viaLobby && !byCode.snapshot().relayed, "a room-code member is not a lobby member");
        CHECK(waitFor([&] { return room.s->members().size() == 4; }, 3), "members with the room-code one: %zu", room.s->members().size());
        exchange(*room.s, byCode, byCode.selfId(), "room-code member of a lobby room");
        byCode.leave();
        CHECK(waitFor([&] { return room.s->members().size() == 3; }, 6), "the room-code member leaves (%zu)", room.s->members().size());
        CHECK(byAddress.joinAddress("127.0.0.1:" + std::to_string(room.port), "", &err), "joinAddress: %s", err.c_str());
        CHECK(waitFor([&] { return byAddress.snapshot().state == State::Joined; }, 6), "direct connection into a lobby room: %s", byAddress.snapshot().message.c_str());
        byAddress.leave();
        CHECK(waitFor([&] { return room.s->members().size() == 3; }, 6), "the direct member leaves (%zu)", room.s->members().size());
    }
    leaveAndCheck(room, *p2, "known-peer member");
    leaveAndCheck(room, *p1, "recorded member");
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned seed = std::random_device{}();
    bool observe = false;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--observe") observe = true;
        else seed = unsigned(std::strtoul(argv[i], nullptr, 0));
    }
    gRng.seed(seed);
    std::printf("seed %u\n", seed);
    // The whole run takes about a minute; a hang must not block whoever runs the tests.
    std::thread([] {
        sleepMs(300000);
        std::printf("FAIL: the test did not finish within 300 s\n");
        std::_Exit(3);
    }).detach();

    std::string why;
    CHECK(onlinecrypto::selfTest(&why), "%s", why.c_str());
    CHECK(onlinecrypto::edSelfTest(&why), "%s", why.c_str());
    online::sock::startup();
    setTestMode(true);

    std::string err;
    if (!gTurnA.start("127.0.0.9", 0, kTurnUser, kTurnPass, &err) || !gTurnB.start("127.0.0.10", 0, "other", "other", &err)) {
        std::printf("FAIL: STUN / TURN test servers: %s\n", err.c_str());
        return 1;
    }
    setStunServers(gTurnA.addr().text(), gTurnB.addr().text());
    std::printf("STUN %s and %s, TURN %s\n", gTurnA.addr().text().c_str(), gTurnB.addr().text().c_str(), gTurnA.addr().text().c_str());

    LobbyProc withTurn, noTurn;
    if (!withTurn.start({"--turn", gTurnA.addr().text(), "--turn-user", kTurnUser, "--turn-pass", kTurnPass}) || !noTurn.start({})) {
        std::printf("FAIL: the lobby did not start\n");
        return 1;
    }
    std::printf("lobby %s (relay), lobby %s (no relay)\n", withTurn.url.c_str(), noTurn.url.c_str());
    {
        fetchRooms(withTurn.url);
        CHECK(lobby::state().relay, "the lobby with --turn says it has a relay");
        fetchRooms(noTurn.url);
        CHECK(!lobby::state().relay, "the lobby without --turn says it has none");
    }

    const std::string& url = withTurn.url;
    natCase("1  no NAT / no NAT", kNoNat, kNoNat, 0, url);
    natCase("2  full cone / full cone", kFull, kFull, 0, url);
    natCase("3  port-restricted / port-restricted", kRestricted, kRestricted, 0, url);
    natCase("4  joiner symmetric (sequential) / host port-restricted", kRestricted, kSymSeq, 0, url);
    natCase("5  joiner port-restricted / host symmetric (sequential)", kSymSeq, kRestricted, 0, url);
    natCase("6  joiner symmetric (random) / host port-restricted", kRestricted, kSymRand, 1, url);
    natCase("7  symmetric (random) / symmetric (random)", kSymRand, kSymRand, 1, url);
    setForceRelay(true);
    natCase("8  full cone / full cone, forced relay", kFull, kFull, 1, url);
    setForceRelay(false);
    relayThenDirect(url);
    noRelay(noTurn.url);
    routerRestart(url);
    vanish(url);
    lostAnswers(url);
    threeJoiners(url);
    roomRules(url);
    robustness(url);
    if (observe) observations(url);

    waitFor([] { return !lobby::state().loading; }, 12);
    gLobbyStop = true;
    withTurn.join();
    noTurn.join();
    gTurnA.stop();
    gTurnB.stop();

    std::printf("\n%-66s %-8s %-8s %s\n", "case (joiner / host)", "result", "path", "to Joined / Failed");
    for (auto& l : gSummary) std::printf("%s\n", l.c_str());
    std::printf(sFails ? "%d FAILURES\n" : "all online lobby checks passed\n", sFails);
    return sFails ? 1 : 0;
}
