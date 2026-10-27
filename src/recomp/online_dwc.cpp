// HLE of the Nintendo Wi-Fi Connection library (DWC) at its public API, on top of the port's
// rooms (online_net.h). docs/ONLINE.md "The game's online flow" and "DWC on rooms".
//
// City Folk's network manager (thread fn_800E7C20, object at r13-0x3174) calls DWC_Init,
// DWC_InitFriendsMatch, DWC_LoginAsync, then every frame DWC_ProcessFriendsMatch; visits use
// server-client matching: the town's owner DWC_SetupGameServer (open gate), visitors
// DWC_ConnectToGameServerAsync(friend index) after checking DWC_GetFriendStatusDataSC (status
// 6 = server with room); town data goes over DWC_SendReliable / the user receive callback, voice
// (VCT) over DWC_SendUnreliable. Everything below DWC (GameSpy GP/QR2/SB/NATNEG/GT2, NAS login
// over NHTTP/SSL, SO sockets) never runs: these handlers answer as the library would, with the
// room members as the other consoles.
//
//   - Login succeeds only in a room; the profile id is this install's (online.cpp), written into
//     the game's user data as the authentic login id, so the game's own friend code (DWC's
//     checksum of pid + game code) is stable and shown in its menus and in F1.
//   - A friend-list entry (friend code or profile id) is a buddy (DWC_IsBuddyFriendData) when a
//     room member has that profile id and has ours in its own list: both registered each other
//     with the game's friend roster, as WFC required. Buddies see each other's status (online /
//     hosting with N of 4) and status data (DWC_SetOwnStatusData).
//   - Matches are an overlay on the room: the server gives AIDs (server 0), the room host
//     relays; reliable data in order, unreliable best effort.
// Every callback runs on the game's network thread inside DWC_ProcessFriendsMatch (or, as in
// DWC, inside DWC_CloseAllConnectionsHard / DWC_CloseConnectionHard).
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/online.h"
#include "recomp/online_net.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace recomp {
namespace {

namespace net = ::online::net;

constexpr u8 kStOffline = 0, kStOnline = 1, kStScCl = 5, kStScSv = 6;
constexpr int kErrNetwork = 6;           // DWC_ERROR_NETWORK (error type 3: shut down friends match)
constexpr int kCodeNotInRoom = -20100;   // shown as error 20100: login without a room
constexpr int kCodeRoomLost = -80430;    // shown as error 80430: the room connection was lost
constexpr int kMaxFriendPids = 64;

enum : u8 {
    M_STATUS = 1, M_JOIN_REQ, M_JOIN_OK, M_JOIN_NG, M_NODES, M_LEAVE, M_DATA, M_UDATA, M_PING, M_PONG,
    M_CLOSE, M_JOIN_CANCEL,
};

double now() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

struct Peer {  // a room member's DWC state (from its STATUS)
    u8 member = 0;
    u32 pid = 0;
    u64 fkey = 0;
    u8 status = kStOffline, maxEntry = 0, numEntry = 0;
    std::vector<u8> data;
    std::vector<u32> friends;
    bool known = false;
};

struct Node {
    u8 aid, member;
    u32 pid;
};

enum class MRole { None, Server, Joining, Client };

struct Dwc {
    bool inited = false;
    u32 userData = 0, friendList = 0;
    int friendCount = 0;
    bool loggedIn = false, loginPending = false;
    double loginAt = 0;
    u32 loginCb = 0, loginParam = 0;
    bool updPending = false, updDone = false;
    double updAt = 0;
    u32 updCb = 0, updParam = 0, statusCb = 0, statusParam = 0, delCb = 0, delParam = 0;
    u32 buddyCb = 0, buddyParam = 0;
    u8 ownStatus = kStOffline;
    std::vector<u8> ownData;
    std::vector<Peer> peers;
    std::vector<int> notified;  // per friend index: last status reported to statusCb (-1 none)
    double lastStatusSend = -100;
    std::vector<u32> lastSentFriends;
    // matching
    MRole role = MRole::None;
    int lastMatchType = -1;
    u8 maxEntry = 0;
    u32 matchedCb = 0, matchedParam = 0, newClientCb = 0, newClientParam = 0, attemptCb = 0, attemptParam = 0;
    std::vector<Node> nodes;
    u8 myAid = 0;
    u8 serverMember = 0;
    int serverIndex = -1;
    bool anybody = false;
    double joinAt = 0;
    bool suspended = false;
    u32 closedCb = 0, closedParam = 0;
    // transport
    u32 sendCb = 0, sendParam = 0, recvCb = 0, recvParam = 0, recvToCb = 0, recvToParam = 0, pingCb = 0, pingParam = 0;
    struct RecvBuf {
        u32 buf = 0, size = 0;
    } recvBuf[8];
    std::deque<std::function<void(Ctx&)>> later;  // callbacks for the next ProcessFriendsMatch
    // errors
    int lastError = 0, lastCode = 0;
    bool roomLostReported = false;
    // guest scratch
    u32 gStatusStr = 0, gUserData = 0;
};

Dwc d;
std::mutex sUiMutex;
std::string sUiStatus = "Not connected";

bool log() { return online::logOn(); }

void setUi(const std::string& s) {
    std::lock_guard<std::mutex> g(sUiMutex);
    sUiStatus = s;
}

// ---- guest helpers ----
u32 call(Ctx& c, u32 fn, std::initializer_list<u32> args) {
    if (!fn) return 0;
    return gu::call(c, fn, args);
}

u64 call64(Ctx& c, u32 fn, u32 a0) {
    const Ctx saved = c;
    c.r[3] = a0;
    c.call(fn);
    const u64 v = u64(c.r[3]) << 32 | c.r[4];
    c = saved;
    return v;
}

void ensureScratch() {
    if (!d.gStatusStr) d.gStatusStr = hleAlloc(256);
    if (!d.gUserData) d.gUserData = hleAlloc(32);
}

u32 entry(int i) { return d.friendList + 12u * u32(i); }
int entryType(int i) { return (rd32(entry(i)) >> 11) & 3; }
u32 entryPid(int i) {
    if (!d.friendList || i < 0 || i >= d.friendCount) return 0;
    const int t = entryType(i);
    return t == 2 || t == 3 ? rd32(entry(i) + 4) : 0;
}
bool entryBuddy(int i) { return entryType(i) == 3 && ((rd32(entry(i)) >> 13) & 1); }

std::vector<u32> myFriendPids() {
    std::vector<u32> v;
    for (int i = 0; i < d.friendCount && int(v.size()) < kMaxFriendPids; i++)
        if (const u32 p = entryPid(i)) v.push_back(p);
    return v;
}

Peer* peerByMember(u8 m) {
    for (auto& p : d.peers)
        if (p.member == m) return &p;
    return nullptr;
}

Peer* peerByPid(u32 pid) {
    for (auto& p : d.peers)
        if (p.known && p.pid == pid) return &p;
    return nullptr;
}

bool mutual(const Peer& p) {
    const u32 me = online::profileId();
    return std::find(p.friends.begin(), p.friends.end(), me) != p.friends.end();
}

// The buddy member for friend index i (registered both ways), or null.
Peer* buddyAt(int i) {
    const u32 pid = entryPid(i);
    if (!pid) return nullptr;
    Peer* p = peerByPid(pid);
    return p && mutual(*p) ? p : nullptr;
}

int friendIndexOf(u32 pid) {
    for (int i = 0; i < d.friendCount; i++)
        if (pid && entryPid(i) == pid) return i;
    return -1;
}

// ---- messages ----
struct W {
    std::vector<u8> b;
    W& u8_(u8 v) { b.push_back(v); return *this; }
    W& u16_(u16 v) { b.push_back(u8(v)); b.push_back(u8(v >> 8)); return *this; }
    W& u32_(u32 v) { for (int i = 0; i < 4; i++) b.push_back(u8(v >> (8 * i))); return *this; }
    W& u64_(u64 v) { for (int i = 0; i < 8; i++) b.push_back(u8(v >> (8 * i))); return *this; }
    W& bytes(const void* p, size_t n) { b.insert(b.end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n); return *this; }
};
struct R {
    const u8* p;
    size_t n, i = 0;
    bool ok = true;
    R(const std::vector<u8>& v) : p(v.data()), n(v.size()) {}
    u8 u8_() { if (i + 1 > n) { ok = false; return 0; } return p[i++]; }
    u16 u16_() { if (i + 2 > n) { ok = false; return 0; } u16 v = u16(p[i] | p[i + 1] << 8); i += 2; return v; }
    u32 u32_() { if (i + 4 > n) { ok = false; return 0; } u32 v = 0; for (int k = 0; k < 4; k++) v |= u32(p[i + k]) << (8 * k); i += 4; return v; }
    u64 u64_() { if (i + 8 > n) { ok = false; return 0; } u64 v = 0; for (int k = 0; k < 8; k++) v |= u64(p[i + k]) << (8 * k); i += 8; return v; }
    const u8* rest(size_t* len) { *len = ok && i <= n ? n - i : 0; return p + i; }
};

void send(u8 to, u8 type, const W& w, bool reliable = true) {
    net::session().sendApp(to, type, w.b.data(), w.b.size(), reliable);
}

void sendStatus(u8 to = net::kBroadcast) {
    W w;
    const u8 num = d.role == MRole::Server ? u8(d.nodes.size()) : 0;
    w.u32_(online::profileId()).u64_(online::friendKey()).u8_(d.ownStatus).u8_(d.maxEntry).u8_(num);
    w.u16_(u16(d.ownData.size())).bytes(d.ownData.data(), d.ownData.size());
    const auto fr = myFriendPids();
    w.u8_(u8(fr.size()));
    for (u32 p : fr) w.u32_(p);
    send(to, M_STATUS, w);
    d.lastStatusSend = now();
    d.lastSentFriends = fr;
}

void setOwnStatus(u8 s) {
    if (d.ownStatus == s) return;
    d.ownStatus = s;
    if (d.loggedIn) sendStatus();
}

void writeNodes(W& w) {
    w.u8_(u8(d.nodes.size()));
    for (auto& n : d.nodes) w.u8_(n.aid).u8_(n.member).u32_(n.pid);
}

std::vector<Node> readNodes(R& r) {
    std::vector<Node> v;
    const u8 k = r.u8_();
    for (int i = 0; i < k && r.ok && i < 8; i++) {
        Node n;
        n.aid = r.u8_();
        n.member = r.u8_();
        n.pid = r.u32_();
        if (r.ok) v.push_back(n);
    }
    return v;
}

Node* nodeByAid(u8 aid) {
    for (auto& n : d.nodes)
        if (n.aid == aid) return &n;
    return nullptr;
}
Node* nodeByMember(u8 m) {
    for (auto& n : d.nodes)
        if (n.member == m) return &n;
    return nullptr;
}

// ---- status notifications (statusCb) ----
void notifyStatuses(Ctx& c) {
    if (!d.updDone || !d.statusCb) return;
    d.notified.resize(size_t(std::max(d.friendCount, 0)), -1);
    for (int i = 0; i < d.friendCount; i++) {
        Peer* p = buddyAt(i);
        const int st = p ? p->status : kStOffline;
        if (d.notified[i] == st) continue;
        // Turn a friend-code entry into a buddy profile entry, as DWC does once GP confirms.
        if (p && !entryBuddy(i)) {
            const u32 e = entry(i);
            const u32 w0 = rd32(e);
            const u32 flags = ((w0 >> 11) & ~3u) | 3u | 4u;
            wr32(e, flags << 11 | (w0 & 0x7FF));
            wr32(e + 4, p->pid);
            wr32(e + 8, 0);
            if (log()) std::printf("[dwc] friend %d (pid %u) is a buddy now\n", i, p->pid);
            if (d.buddyCb) call(c, d.buddyCb, {u32(i), d.buddyParam});
        }
        d.notified[i] = st;
        wr8(d.gStatusStr, 0);
        if (log()) std::printf("[dwc] status callback: friend %d status %d\n", i, st);
        call(c, d.statusCb, {u32(i), u32(st), d.gStatusStr, d.statusParam});
    }
}

// ---- matching ----
void matched(Ctx& c, int error, bool cancel, bool self, bool isServer, int index) {
    if (log()) std::printf("[dwc] matched callback: error %d cancel %d self %d server %d index %d\n", error, cancel, self, isServer, index);
    call(c, d.matchedCb, {u32(error), cancel ? 1u : 0u, self ? 1u : 0u, isServer ? 1u : 0u, u32(index), d.matchedParam});
}

void closed(Ctx& c, int error, bool isLocal, bool isServer, u8 aid, int index) {
    if (log()) std::printf("[dwc] connection closed callback: aid %u local %d server %d index %d\n", aid, isLocal, isServer, index);
    call(c, d.closedCb, {u32(error), isLocal ? 1u : 0u, isServer ? 1u : 0u, aid, u32(index), d.closedParam});
}

void endMatchLocal() {
    d.nodes.clear();
    d.role = MRole::None;
    d.suspended = false;
    d.myAid = 0;
    setOwnStatus(d.loggedIn ? kStOnline : kStOffline);
}

// Removes node `aid`; tells the game. A departing server ends the match for everyone.
void dropNode(Ctx& c, u8 aid, bool isLocal) {
    Node* n = nodeByAid(aid);
    if (!n) return;
    const bool wasServer = aid == 0;
    const int index = friendIndexOf(n->pid);
    if (wasServer && d.role == MRole::Client) {
        std::vector<Node> others;
        for (auto& o : d.nodes)
            if (o.aid != d.myAid && o.aid != 0) others.push_back(o);
        d.nodes.clear();
        closed(c, 0, isLocal, true, 0, index);
        for (auto& o : others) closed(c, 0, isLocal, false, o.aid, friendIndexOf(o.pid));
        endMatchLocal();
        return;
    }
    d.nodes.erase(std::remove_if(d.nodes.begin(), d.nodes.end(), [&](const Node& x) { return x.aid == aid; }), d.nodes.end());
    if (d.role == MRole::Server) {
        W w;
        writeNodes(w);
        for (auto& o : d.nodes)
            if (o.aid != 0) send(o.member, M_NODES, w);
        sendStatus();
    }
    closed(c, 0, isLocal, wasServer, aid, index);
}

void onJoinRequest(Ctx& c, u8 from, R& r) {
    const u32 pid = r.u32_();
    u8 ud[4] = {};
    for (auto& b : ud) b = r.u8_();
    const bool anyb = r.u8_() != 0;
    auto ng = [&](u8 why) {
        W w;
        w.u8_(why);
        send(from, M_JOIN_NG, w);
        if (log()) std::printf("[dwc] refused join of member %u (reason %u)\n", from, why);
    };
    if (!r.ok) return;
    if (d.role != MRole::Server) return ng(1);
    if (d.suspended) return ng(2);
    if (int(d.nodes.size()) >= d.maxEntry) return ng(3);
    const int index = friendIndexOf(pid);
    Peer* p = peerByMember(from);
    if (!anyb && (index < 0 || !p || !mutual(*p))) return ng(4);  // friends only, as WFC
    if (nodeByMember(from)) return;
    // The game decides (fn_800DDF78 via its attempt callback: not while busy).
    std::memcpy(hostp(d.gUserData), ud, 4);
    if (d.attemptCb && !call(c, d.attemptCb, {d.gUserData, d.attemptParam})) return ng(5);
    u8 aid = 1;
    while (nodeByAid(aid)) aid++;
    d.nodes.push_back({aid, from, pid});
    W ok;
    ok.u8_(aid);
    writeNodes(ok);
    send(from, M_JOIN_OK, ok);
    W nl;
    writeNodes(nl);
    for (auto& o : d.nodes)
        if (o.aid != 0 && o.member != from) send(o.member, M_NODES, nl);
    if (log()) std::printf("[dwc] member %u (pid %u) joined our match as aid %u\n", from, pid, aid);
    if (d.newClientCb) call(c, d.newClientCb, {u32(index), d.newClientParam});
    matched(c, 0, false, false, false, index);
    sendStatus();
}

void onNodes(Ctx& c, const std::vector<Node>& list) {
    if (d.role != MRole::Client) return;
    std::vector<Node> before = d.nodes;
    d.nodes = list;
    for (auto& n : list) {
        bool had = false;
        for (auto& b : before) had |= b.aid == n.aid;
        if (!had && n.aid != d.myAid) {
            const int idx = friendIndexOf(n.pid);
            if (d.newClientCb) call(c, d.newClientCb, {u32(idx), d.newClientParam});
            matched(c, 0, false, false, false, idx);
        }
    }
    for (auto& b : before) {
        bool still = false;
        for (auto& n : list) still |= n.aid == b.aid;
        if (!still && b.aid != d.myAid) closed(c, 0, false, b.aid == 0, b.aid, friendIndexOf(b.pid));
    }
}

void tryAnybody() {
    for (auto& p : d.peers)
        if (p.known && p.status == kStScSv && p.numEntry < p.maxEntry) {
            d.serverMember = p.member;
            W w;
            w.u32_(online::profileId()).u8_(0).u8_(0).u8_(0).u8_(0).u8_(1);
            send(p.member, M_JOIN_REQ, w);
            d.joinAt = now();
            d.anybody = false;  // one request at a time; JOIN_NG restarts the search
            return;
        }
}

void handle(Ctx& c, const net::AppMsg& m) {
    R r(m.data);
    switch (m.type) {
    case M_STATUS: {
        Peer* p = peerByMember(m.from);
        if (!p) {
            d.peers.push_back({});
            p = &d.peers.back();
            p->member = m.from;
        }
        p->pid = r.u32_();
        p->fkey = r.u64_();
        p->status = r.u8_();
        p->maxEntry = r.u8_();
        p->numEntry = r.u8_();
        const u16 len = r.u16_();
        size_t rest = 0;
        const u8* dp = r.rest(&rest);
        if (len > rest || len > 512) return;
        p->data.assign(dp, dp + len);
        r.i += len;
        const u8 nf = r.u8_();
        p->friends.clear();
        for (int i = 0; i < nf && r.ok && i < kMaxFriendPids; i++) p->friends.push_back(r.u32_());
        p->known = r.ok;
        if (log()) std::printf("[dwc] status of member %u: pid %u status %u (%u/%u) data %u bytes, %zu friends%s\n", m.from, p->pid,
                               p->status, p->numEntry, p->maxEntry, len, p->friends.size(), mutual(*p) ? ", has us" : "");
        notifyStatuses(c);
        break;
    }
    case M_JOIN_REQ:
        onJoinRequest(c, m.from, r);
        break;
    case M_JOIN_OK:
        if (d.role == MRole::Joining && m.from == d.serverMember) {
            d.myAid = r.u8_();
            auto list = readNodes(r);
            if (!r.ok) return;
            d.nodes = list;
            d.role = MRole::Client;
            setOwnStatus(kStScCl);
            const Node* sv = nodeByAid(0);
            const int idx = d.serverIndex >= 0 ? d.serverIndex : friendIndexOf(sv ? sv->pid : 0);
            if (log()) std::printf("[dwc] joined the match of member %u as aid %u (%zu nodes)\n", m.from, d.myAid, list.size());
            if (d.newClientCb) call(c, d.newClientCb, {u32(idx), d.newClientParam});
            matched(c, 0, false, true, false, idx);
        }
        break;
    case M_JOIN_NG:
        if (d.role == MRole::Joining && m.from == d.serverMember) {
            const u8 why = r.u8_();
            if (log()) std::printf("[dwc] the server refused us (reason %u)\n", why);
            if (d.lastMatchType == 0) {  // anybody: keep looking
                d.anybody = true;
                break;
            }
            d.role = MRole::None;
            setOwnStatus(kStOnline);
            d.lastError = 13;  // a matching error
            d.lastCode = -81010;
            matched(c, 13, false, true, false, d.serverIndex);
        }
        break;
    case M_NODES: {
        auto list = readNodes(r);
        if (r.ok) onNodes(c, list);
        break;
    }
    case M_LEAVE: {
        const u8 aid = r.u8_();
        Node* n = nodeByAid(aid);
        if (r.ok && n && n->member == m.from) dropNode(c, aid, false);
        break;
    }
    case M_CLOSE:  // the server dropped us
        if (d.role == MRole::Client && m.from == d.serverMember) dropNode(c, 0, false);
        break;
    case M_JOIN_CANCEL:
        break;
    case M_DATA:
    case M_UDATA: {
        const u8 src = r.u8_();
        Node* n = nodeByAid(src);
        if (!r.ok || !n || n->member != m.from) return;
        size_t len = 0;
        const u8* p = r.rest(&len);
        if (src >= 8 || !d.recvCb) return;
        const auto& rb = d.recvBuf[src];
        if (!rb.buf) {
            if (log()) std::printf("[dwc] data from aid %u dropped: no receive buffer\n", src);
            return;
        }
        if (len > rb.size) {
            std::printf("[dwc] data from aid %u: %zu bytes > receive buffer %u, cut\n", src, len, rb.size);
            len = rb.size;
        }
        std::memcpy(hostp(rb.buf), p, len);
        call(c, d.recvCb, {src, rb.buf, u32(len), d.recvParam});
        break;
    }
    case M_PING: {
        const u8 src = r.u8_();
        const u32 t = r.u32_();
        W w;
        w.u32_(t).u8_(d.myAid);
        send(m.from, M_PONG, w, false);
        (void)src;
        break;
    }
    case M_PONG: {
        const u32 t = r.u32_();
        const u8 aid = r.u8_();
        if (r.ok && d.pingCb) call(c, d.pingCb, {u32(now() * 1000) - t, aid, d.pingParam});
        break;
    }
    }
}

void process(Ctx& c) {
    ensureScratch();
    auto& s = net::session();
    // Room lost while the game is online: a network error (the game shows its error and goes
    // offline, as when the Wii lost its connection).
    if (d.loggedIn && !s.active() && !d.roomLostReported) {
        d.roomLostReported = true;
        d.lastError = kErrNetwork;
        d.lastCode = kCodeRoomLost;
        std::printf("[dwc] the room is gone: network error %d for the game\n", -kCodeRoomLost);
        std::vector<Node> nodes = d.nodes;
        d.nodes.clear();
        for (auto& n : nodes)
            if (n.aid != d.myAid) closed(c, kErrNetwork, false, n.aid == 0, n.aid, friendIndexOf(n.pid));
        endMatchLocal();
        d.peers.clear();
    }
    if (d.loginPending && now() >= d.loginAt) {
        d.loginPending = false;
        if (!s.active()) {
            d.lastError = kErrNetwork;
            d.lastCode = kCodeNotInRoom;
            std::printf("[dwc] login refused: not in a room (F1 > Online)\n");
            setUi("Login refused: join or host a room first");
            call(c, d.loginCb, {u32(kErrNetwork), 0, d.loginParam});
        } else {
            const u32 pid = online::profileId();
            const u32 u = d.userData;
            if (u) {
                // Authentic login id (DWCUserData +0x10: type 1, user id; +0x18 player id) and
                // the GameSpy profile id (+0x1C), as the NAS/GP login would store them; dirty flag
                // set so the game saves it (it recomputes the CRC with DWC_ClearDirtyFlag).
                const u32 w0 = rd32(u + 0x10);
                const u32 flags = ((w0 >> 11) & ~3u) | 1u;
                wr32(u + 0x10, flags << 11);
                wr32(u + 0x14, pid ^ 0x5A5A0000u);
                wr32(u + 0x18, pid);
                if (rd32(u + 0x1C) != pid) {
                    if (rd32(u + 0x1C)) std::printf("[dwc] user data had profile id %u, now %u (this install)\n", rd32(u + 0x1C), pid);
                    wr32(u + 0x1C, pid);
                }
                wr32(u + 0x20, rd32(u + 0x20) | 1);
                static const u32 fnKey = guestSymbol("DWC_CreateFriendKey");
                if (fnKey) online::setFriendKey(call64(c, fnKey, u));
            }
            d.loggedIn = true;
            d.roomLostReported = false;
            d.ownStatus = kStOnline;
            char fc[32];
            const u64 k = online::friendKey();
            std::snprintf(fc, sizeof fc, "%04u-%04u-%04u", unsigned(k / 100000000ull % 10000), unsigned(k / 10000 % 10000), unsigned(k % 10000));
            std::printf("[dwc] logged in: profile id %u, friend code %s\n", pid, fc);
            setUi(std::string("Online (friend code ") + fc + ")");
            sendStatus();
            call(c, d.loginCb, {0, pid, d.loginParam});
        }
    }
    // Room membership.
    net::MemberEvent ev;
    while (s.pollMemberEvent(ev)) {
        if (ev.joined) {
            if (d.loggedIn) sendStatus(ev.id);
        } else {
            if (Node* n = nodeByMember(ev.id)) dropNode(c, n->aid, false);
            d.peers.erase(std::remove_if(d.peers.begin(), d.peers.end(), [&](const Peer& p) { return p.member == ev.id; }), d.peers.end());
            notifyStatuses(c);
        }
    }
    net::AppMsg m;
    int budget = 256;
    while (budget-- > 0 && s.pollApp(m)) handle(c, m);
    if (d.updPending && now() >= d.updAt) {
        d.updPending = false;
        d.updDone = true;
        d.notified.assign(size_t(std::max(d.friendCount, 0)), -1);
        if (log()) std::printf("[dwc] update servers done\n");
        notifyStatuses(c);
        call(c, d.updCb, {0, 1, d.updParam});
    }
    // Our friend list changed (the player registered someone): tell the room.
    if (d.loggedIn && now() - d.lastStatusSend > 2.0 && myFriendPids() != d.lastSentFriends) sendStatus();
    if (d.role == MRole::Joining) {
        if (d.anybody) {
            if (now() - d.joinAt > 2) tryAnybody();
        } else if (d.lastMatchType != 0 && now() - d.joinAt > 30) {
            d.role = MRole::None;
            setOwnStatus(kStOnline);
            d.lastError = 13;
            d.lastCode = -81020;
            matched(c, 13, false, true, false, d.serverIndex);
        }
    }
    while (!d.later.empty()) {
        auto f = std::move(d.later.front());
        d.later.pop_front();
        f(c);
    }
}

void resetAll() {
    const u32 s1 = d.gStatusStr, s2 = d.gUserData;  // hleAlloc memory is never freed: keep it
    d = Dwc{};
    d.gStatusStr = s1;
    d.gUserData = s2;
}

}  // namespace

std::string online::dwcStatusText() {
    std::lock_guard<std::mutex> g(sUiMutex);
    return sUiStatus;
}

// ---- network start-up below DWC (game: fn_800E80B0 / fn_800E8198 / fn_800E7FA8) ----
// Online only in a room; otherwise the original library runs (and finds no network).
HLE(SOInit) {
    if (!online::active()) return callOriginal(c, "SOInit");
    c.r[3] = 0;
}
HLE(SOStartup) {
    if (!online::active()) return callOriginal(c, "SOStartup");
    c.r[3] = 0;
}
HLE(SOCleanup) {
    if (!online::active() && !d.inited) return callOriginal(c, "SOCleanup");
    c.r[3] = 0;
}
HLE(SOFinish) {
    if (!online::active() && !d.inited) return callOriginal(c, "SOFinish");
    c.r[3] = 0;
}
// WiiConnect24's scheduler is paused while the game is online (no WC24 here: nothing to pause).
HLE(NWC24TrySuspendScheduler) {
    if (!online::active()) return callOriginal(c, "NWC24TrySuspendScheduler");
    c.r[3] = 0;
}
HLE(NWC24ResumeScheduler) {
    if (!online::active() && !d.inited) return callOriginal(c, "NWC24ResumeScheduler");
    c.r[3] = 0;
}

// ---- DWC main / friends match ----
HLE(DWC_InitFriendsMatch) {
    // (control, userdata, productID, gameName, secretKey, sendBufSize, recvBufSize, friendList, friendListLen)
    resetAll();
    ensureScratch();
    d.inited = true;
    d.userData = c.r[4];
    d.friendList = c.r[10];
    d.friendCount = int(stackArg(c, 0));
    if (d.friendCount < 0 || d.friendCount > 64) d.friendCount = 0;
    std::printf("[dwc] InitFriendsMatch: game %s, user data %08X, friend list %08X (%d)\n", gstr(c.r[6]), d.userData,
                d.friendList, d.friendCount);
    setUi("Starting");
}

HLE(DWC_ShutdownFriendsMatch) {
    if (d.role == MRole::Server || d.role == MRole::Client) {
        W w;
        w.u8_(d.myAid);
        for (auto& n : d.nodes)
            if (n.aid != d.myAid) send(n.member, M_LEAVE, w);
    }
    if (d.loggedIn) {
        d.ownStatus = kStOffline;
        d.ownData.clear();
        sendStatus();
    }
    std::printf("[dwc] ShutdownFriendsMatch\n");
    resetAll();
    setUi("Not connected");
}

HLE(DWC_LoginAsync) {
    // (ingamesn, reserved, callback(error, profileID, param), param)
    d.loginCb = c.r[5];
    d.loginParam = c.r[6];
    d.loginPending = true;
    d.loginAt = now() + 0.4;
    setUi("Logging in");
    if (log()) std::printf("[dwc] LoginAsync\n");
    c.r[3] = 1;
}

HLE(DWC_ProcessFriendsMatch) {
    if (d.inited) process(c);
}

HLE(DWC_UpdateServersAsync) {
    // (playerName, updateCb(error, isChanged, param), param, statusCb(index, status, string, param),
    //  param, deleteCb(deletedIndex, srcIndex, param), param)
    d.updCb = c.r[4];
    d.updParam = c.r[5];
    d.statusCb = c.r[6];
    d.statusParam = c.r[7];
    d.delCb = c.r[8];
    d.delParam = c.r[9];
    d.updPending = true;
    d.updAt = now() + 0.3;
    if (log()) std::printf("[dwc] UpdateServersAsync\n");
    c.r[3] = 1;
}

HLE(DWC_SetBuddyFriendCallback) {
    d.buddyCb = c.r[3];
    d.buddyParam = c.r[4];
    c.r[3] = 1;
}

HLE(DWC_CanChangeFriendList) { c.r[3] = d.updPending ? 0 : 1; }

HLE(DWC_DeleteBuddyFriendData) {
    const u32 e = c.r[3];
    for (u32 i = 0; i < 12; i += 4) wr32(e + i, 0);
    if (d.loggedIn) sendStatus();
}

HLE(DWC_SetOwnStatusData) {
    const u32 p = c.r[3], n = std::min<u32>(c.r[4], 256);
    d.ownData.assign(hostp(p), hostp(p) + n);
    if (d.loggedIn) sendStatus();
    c.r[3] = 1;
}

static u8 friendStatusSC(u32 friendData, u32 maxP, u32 numP) {
    u32 pid = 0;
    const u32 w0 = rd32(friendData);
    const int t = (w0 >> 11) & 3;
    if (t == 2 || t == 3) pid = rd32(friendData + 4);
    Peer* p = pid ? peerByPid(pid) : nullptr;
    const bool ok = p && mutual(*p);
    if (maxP) wr8(maxP, ok ? p->maxEntry : 0);
    if (numP) wr8(numP, ok ? p->numEntry : 0);
    return ok ? p->status : kStOffline;
}

HLE(DWC_GetFriendStatusSC) {
    // (friendData, maxEntry*, numEntry*, statusString*)
    const u8 st = friendStatusSC(c.r[3], c.r[4], c.r[5]);
    if (c.r[6]) wr8(c.r[6], 0);
    c.r[3] = st;
}

HLE(DWC_GetFriendStatusDataSC) {
    // (friendData, maxEntry*, numEntry*, statusData*, size*)
    const u32 fd = c.r[3], dataP = c.r[6], sizeP = c.r[7];
    const u8 st = friendStatusSC(fd, c.r[4], c.r[5]);
    Peer* p = st != kStOffline ? peerByPid(rd32(fd + 4)) : nullptr;
    const u32 len = p ? u32(p->data.size()) : 0;
    if (sizeP) wr32(sizeP, st == kStOffline ? 0xFFFFFFFFu : len);
    if (p && dataP && len) std::memcpy(hostp(dataP), p->data.data(), len);
    c.r[3] = st;
}

HLE(DWC_GetLastErrorEx) {
    if (c.r[3]) wr32(c.r[3], u32(d.lastCode));
    if (c.r[4]) {
        static const u8 types[25] = {0, 7, 6, 6, 6, 6, 3, 4, 6, 7, 1, 1, 1, 5, 6, 2, 6, 1, 6, 2, 2, 6, 8, 1, 2};
        wr32(c.r[4], d.lastError >= 0 && d.lastError < 25 ? types[d.lastError] : 0);
    }
    c.r[3] = u32(d.lastError);
}

HLE(DWC_ClearError) {
    d.lastError = 0;
    d.lastCode = 0;
    callOriginal(c, "DWC_ClearError");
}

// ---- matching ----
static void startJoin(Ctx& c, int serverIndex, bool anyb) {
    d.matchedCb = c.r[5];
    d.matchedParam = c.r[6];
    d.newClientCb = c.r[7];
    d.newClientParam = c.r[8];
    d.attemptCb = c.r[9];
    d.attemptParam = stackArg(c, 0);
    d.serverIndex = serverIndex;
    d.role = MRole::Joining;
    d.lastMatchType = anyb ? 0 : 3;
    d.joinAt = now();
    d.nodes.clear();
    setOwnStatus(kStScCl);
}

HLE(DWC_SetupGameServer) {
    // (topology, maxEntry, matchedCb, param, newClientCb, param, attemptCb, connectionUserData, attemptParam)
    d.maxEntry = u8(std::clamp<u32>(c.r[4], 2, net::kMaxMembers));
    d.matchedCb = c.r[5];
    d.matchedParam = c.r[6];
    d.newClientCb = c.r[7];
    d.newClientParam = c.r[8];
    d.attemptCb = c.r[9];
    d.attemptParam = stackArg(c, 0);
    d.role = MRole::Server;
    d.lastMatchType = 2;
    d.suspended = false;
    d.myAid = 0;
    d.nodes = {{0, net::session().selfId(), online::profileId()}};
    std::printf("[dwc] SetupGameServer: hosting the town for up to %u players\n", d.maxEntry);
    setUi("Hosting the town (gate open)");
    d.ownStatus = kStScSv;
    sendStatus();
    c.r[3] = 1;
}

HLE(DWC_ConnectToGameServerAsync) {
    // (topology, serverIndex, matchedCb, param, newClientCb, param, attemptCb, connectionUserData, attemptParam)
    const int index = int(c.r[4]);
    Peer* p = buddyAt(index);
    if (!p || p->status != kStScSv) {
        std::printf("[dwc] ConnectToGameServer: friend %d is not hosting\n", index);
        c.r[3] = 0;
        return;
    }
    const u32 udp = c.r[10];
    startJoin(c, index, false);
    d.serverMember = p->member;
    W w;
    w.u32_(online::profileId());
    for (int i = 0; i < 4; i++) w.u8_(udp ? u8(rd8(udp + u32(i))) : 0);
    w.u8_(0);
    send(p->member, M_JOIN_REQ, w);
    std::printf("[dwc] ConnectToGameServer: visiting friend %d (member %u)\n", index, p->member);
    setUi("Visiting a town");
    c.r[3] = 1;
}

HLE(DWC_ConnectToAnybodyAsync) {
    // (topology, numEntry, addFilter, matchedCb, param, newClientCb, param, evalCb, evalParam, attemptCb, ...)
    // Not reached in normal play (visits are friends only); joins any room member's open town.
    d.matchedCb = c.r[6];
    d.matchedParam = c.r[7];
    d.newClientCb = c.r[8];
    d.newClientParam = c.r[9];
    d.attemptCb = stackArg(c, 1);
    d.attemptParam = stackArg(c, 3);
    d.serverIndex = -1;
    d.role = MRole::Joining;
    d.lastMatchType = 0;
    d.anybody = true;
    d.joinAt = now();
    setOwnStatus(kStScCl);
    std::printf("[dwc] ConnectToAnybody (filter \"%s\")\n", c.r[5] ? gstr(c.r[5]) : "");
    c.r[3] = 1;
}

HLE(DWC_AddMatchKeyString) { c.r[3] = c.r[3] ? c.r[3] : 100; }

HLE(DWC_CancelMatch) {
    const bool server = d.role == MRole::Server;
    if (d.role == MRole::Joining) {
        W w;
        w.u32_(online::profileId());
        send(d.serverMember, M_JOIN_CANCEL, w);
    }
    if (d.role == MRole::Server && d.nodes.size() > 1) {
        // Clients already in: the match goes on, no new ones.
        d.suspended = true;
    } else if (d.role != MRole::None) {
        endMatchLocal();
        d.later.push_back([server](Ctx& cc) { matched(cc, 0, true, true, server, -1); });
    }
    if (log()) std::printf("[dwc] CancelMatch\n");
    c.r[3] = 1;
}

HLE(DWC_RequestSuspendMatchAsync) {
    // (suspend, callback(result, suspend, param), param)
    d.suspended = c.r[3] != 0;
    const u32 cb = c.r[4], param = c.r[5], s = c.r[3];
    d.later.push_back([cb, param, s](Ctx& cc) { call(cc, cb, {0, s, param}); });
    if (log()) std::printf("[dwc] suspend match: %u\n", s);
    c.r[3] = 1;
}

HLE(DWC_GetSuspendMatch) { c.r[3] = d.suspended ? 1 : 0; }
HLE(DWC_GetLastMatchingType) { c.r[3] = u32(d.lastMatchType); }
HLE(DWC_GetMyAID) { c.r[3] = d.myAid; }
HLE(DWC_GetNumConnectionHost) { c.r[3] = d.role == MRole::Server || d.role == MRole::Client ? u32(d.nodes.size()) : 0; }
HLE(DWC_IsValidAID) {
    const u8 aid = u8(c.r[3]);
    c.r[3] = aid != d.myAid && nodeByAid(aid) && (d.role == MRole::Server || d.role == MRole::Client) ? 1 : 0;
}
HLE(DWC_GetServerAID) { c.r[3] = (d.role == MRole::Server || d.role == MRole::Client) && nodeByAid(0) ? 0 : 0xFF; }
HLE(DWC_IsServerMyself) { c.r[3] = d.role == MRole::Server ? 1 : 0; }

HLE(DWC_SetConnectionClosedCallback) {
    d.closedCb = c.r[3];
    d.closedParam = c.r[4];
    c.r[3] = 1;
}

HLE(DWC_CloseAllConnectionsHard) {
    if (log()) std::printf("[dwc] CloseAllConnectionsHard (%zu nodes)\n", d.nodes.size());
    W w;
    w.u8_(d.myAid);
    std::vector<Node> nodes = d.nodes;
    for (auto& n : nodes)
        if (n.aid != d.myAid) send(n.member, M_LEAVE, w);
    const u8 me = d.myAid;
    endMatchLocal();
    for (auto& n : nodes)
        if (n.aid != me) closed(c, 0, true, n.aid == 0, n.aid, friendIndexOf(n.pid));
    c.r[3] = 0;
}

HLE(DWC_CloseConnectionHard) {
    const u8 aid = u8(c.r[3]);
    Node* n = nodeByAid(aid);
    if (!n || aid == d.myAid) {
        c.r[3] = 0;
        return;
    }
    if (log()) std::printf("[dwc] CloseConnectionHard aid %u\n", aid);
    W w;
    if (d.role == MRole::Server) {
        w.u8_(aid);
        send(n->member, M_CLOSE, w);
    } else {
        w.u8_(d.myAid);
        send(n->member, M_LEAVE, w);
    }
    dropNode(c, aid, true);
    c.r[3] = 0;
}

// ---- transport ----
HLE(DWC_SetUserSendCallback) { d.sendCb = c.r[3]; d.sendParam = c.r[4]; c.r[3] = 1; }
HLE(DWC_SetUserRecvCallback) { d.recvCb = c.r[3]; d.recvParam = c.r[4]; c.r[3] = 1; }
HLE(DWC_SetUserRecvTimeoutCallback) { d.recvToCb = c.r[3]; d.recvToParam = c.r[4]; c.r[3] = 1; }
HLE(DWC_SetUserPingCallback) { d.pingCb = c.r[3]; d.pingParam = c.r[4]; c.r[3] = 1; }
HLE(DWC_SetSendSplitMax) { c.r[3] = 1; }
HLE(DWC_SetTransportTimeout) { c.r[3] = c.r[3] ? 1 : 0; }

HLE(DWC_SetRecvBuffer) {
    const u8 aid = u8(c.r[3]);
    if (aid < 8) d.recvBuf[aid] = {c.r[4], c.r[5]};
    c.r[3] = 1;
}

HLE(DWC_IsSendableReliable) {
    const u8 aid = u8(c.r[3]);
    Node* n = nodeByAid(aid);
    c.r[3] = n && aid != d.myAid && net::session().queuedBytes(n->member) < 64 * 1024 ? 1 : 0;
}

static bool sendData(u8 aid, u32 buf, u32 size, bool reliable) {
    Node* n = nodeByAid(aid);
    if (!n || aid == d.myAid || size > (reliable ? 64u * 1024 : u32(net::kMaxUnreliable - 8))) return false;
    std::vector<u8> b;
    b.reserve(size + 1);
    b.push_back(d.myAid);
    b.insert(b.end(), hostp(buf), hostp(buf) + size);
    return net::session().sendApp(n->member, reliable ? M_DATA : M_UDATA, b.data(), b.size(), reliable);
}

HLE(DWC_SendReliable) {
    const u8 aid = u8(c.r[3]);
    const u32 buf = c.r[4], size = c.r[5];
    const bool ok = sendData(aid, buf, size, true);
    if (ok && d.sendCb) {
        const u32 cb = d.sendCb, param = d.sendParam;
        d.later.push_back([cb, param, size, aid](Ctx& cc) { call(cc, cb, {size, aid, param}); });
    }
    c.r[3] = ok ? 1 : 0;
}

HLE(DWC_SendUnreliable) {
    c.r[3] = sendData(u8(c.r[3]), c.r[4], c.r[5], false) ? 1 : 0;
}

HLE(DWC_Ping) {
    const u8 aid = u8(c.r[3]);
    Node* n = nodeByAid(aid);
    if (!n || aid == d.myAid) {
        c.r[3] = 0;
        return;
    }
    W w;
    w.u8_(d.myAid).u32_(u32(now() * 1000));
    send(n->member, M_PING, w, false);
    c.r[3] = 1;
}

}  // namespace recomp
