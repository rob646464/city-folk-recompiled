// F1 friends: keys, friend codes, requests over rooms (online_friends.h, docs/ONLINE.md).
//
// Room messages (app types 0x40-0x5F, after DWC's 0x01-0x3F):
//   IDENT   public key, a fresh nonce, name       (to every member, on joining / when one joins)
//   PROVE   Ed25519 signature over "cf-prove" | the peer's nonce | our key: the key is ours
//   REQ     friend request (name)                  ACCEPT / DECLINE   the answer
//   INFO    our roster entry as the game builds it (0x78 bytes: player and town name); kept for
//           friends only, so their roster shows the right names before the first visit
// A member's room profile id must be pidOf(its key) (the DWC layer uses that id as the game's
// friend code), so a proved key also proves the friend code shown in the game.
#include "recomp/online_friends.h"

#include "port/install.h"
#include "recomp/online.h"
#include "recomp/online_crypto.h"
#include "recomp/online_ed25519.h"
#include "recomp/online_mail.h"
#include "recomp/online_net.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace recomp::online::friends {
namespace {

namespace net = ::online::net;
namespace oc = ::onlinecrypto;
using u8 = uint8_t;
using u32 = uint32_t;

enum : u8 { F_IDENT = 0x40, F_PROVE = 0x41, F_REQ = 0x42, F_ACCEPT = 0x43, F_DECLINE = 0x44, F_INFO = 0x45, F_ENDPOINTS = 0x48, F_EMAIL = 0x49 };

std::recursive_mutex sMutex;
bool sLoaded = false;
u8 sPub[32];
u8 sXPriv[32], sXPub[32];  // X25519 key for letters through a relay (sealed per friend pair)
std::vector<Friend> sFriends;
std::vector<u8> sMyInfo;
std::string sMyEndpoints;
std::string sMyEmail;  // own address while "Always online mail" is on (sent to friends in rooms)  // "ip:port;ip:port" of this game's letters link (online_mail.cpp)
std::vector<u32> sRemoved;  // profile ids to take out of the game's roster (removed / blocked in F1)
std::vector<u32> sSession;  // room members put into the game's roster for the session (not F1 friends)

struct MemberState {
    u8 pub[32] = {};
    bool havePub = false, verified = false;
    u8 myNonce[16] = {};   // we sent; they sign it
    std::string name;
    u8 xpub[32] = {};
    bool haveX = false;
    bool identSent = false;
    bool infoSent = false;
    std::string epSent, emailSent;
    std::vector<u8> info;  // their INFO (applied once proved and a friend)
    double lastReq = -100;
    int reqCount = 0;
};
std::map<u8, MemberState> sMembers;

double secs() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

std::filesystem::path listPath() { return port::dataDir() / "online" / "friends.txt"; }

const char* stateName(State s) {
    switch (s) {
    case State::Friend: return "friend";
    case State::RequestSent: return "sent";
    case State::RequestReceived: return "received";
    case State::Blocked: return "blocked";
    }
    return "friend";
}

void save() {
    std::error_code ec;
    std::filesystem::create_directories(listPath().parent_path(), ec);
    std::ofstream out(listPath());
    out << "# City Folk Recompiled friends: <state> <public key> <name>\n";
    for (auto& f : sFriends) out << stateName(f.state) << " " << oc::hex(f.pub, 32) << " " << f.name << "\n";
    for (auto& f : sFriends)
        if (f.gameInfo.size() == kGameInfoSize) out << "info " << oc::hex(f.pub, 32) << " " << oc::hex(f.gameInfo.data(), f.gameInfo.size()) << "\n";
    for (u32 pid : sRemoved) out << "removed " << pid << "\n";
    for (u32 pid : sSession) out << "session " << pid << "\n";
    for (auto& f : sFriends)
        if (!f.email.empty()) out << "email " << oc::hex(f.pub, 32) << " " << f.email << "\n";
    for (auto& f : sFriends)
        if (!f.endpoints.empty()) out << "ep " << oc::hex(f.pub, 32) << " " << f.endpoints << "\n";
    for (auto& f : sFriends)
        if (f.haveX) out << "xkey " << oc::hex(f.pub, 32) << " " << oc::hex(f.xpub, 32) << "\n";
}

std::vector<u8> unhex(const std::string& hx) {
    std::vector<u8> v;
    for (size_t i = 0; i + 1 < hx.size(); i += 2) v.push_back(u8(std::stoi(hx.substr(i, 2), nullptr, 16)));
    return v;
}

void load() {
    if (sLoaded) return;
    sLoaded = true;
    oc::ed25519PublicKey(online::identitySeed(), sPub);
    oc::hmacSha256(online::identitySeed(), 32, "cf-x25519", 9, sXPriv);
    sXPriv[0] &= 248;
    sXPriv[31] = u8((sXPriv[31] & 127) | 64);
    static const u8 kBase[32] = {9};
    oc::x25519(sXPriv, kBase, sXPub);
    std::ifstream in(listPath());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string st, hx;
        ls >> st >> hx;
        std::string name;
        std::getline(ls, name);
        while (!name.empty() && name[0] == ' ') name.erase(name.begin());
        if (st == "removed" || st == "session") {
            const u32 pid = u32(std::strtoul(hx.c_str(), nullptr, 10));
            if (pid) (st == "removed" ? sRemoved : sSession).push_back(pid);
            continue;
        }
        if (hx.size() != 64) continue;
        if (st == "email") {
            const auto key = unhex(hx);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            for (auto& f : sFriends)
                if (std::memcmp(f.pub, key.data(), 32) == 0) f.email = name.substr(0, 120);
            continue;
        }
        if (st == "ep") {
            const auto key = unhex(hx);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            for (auto& f : sFriends)
                if (std::memcmp(f.pub, key.data(), 32) == 0) f.endpoints = name.substr(0, 200);
            continue;
        }
        if (st == "xkey") {
            const auto key = unhex(hx);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            const auto x = unhex(name);
            for (auto& f : sFriends)
                if (x.size() == 32 && std::memcmp(f.pub, key.data(), 32) == 0) {
                    std::memcpy(f.xpub, x.data(), 32);
                    f.haveX = true;
                }
            continue;
        }
        if (st == "info") {
            const auto key = unhex(hx);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            for (auto& f : sFriends)
                if (std::memcmp(f.pub, key.data(), 32) == 0 && name.size() == 2 * kGameInfoSize) f.gameInfo = unhex(name);
            continue;
        }
        Friend f;
        for (int i = 0; i < 32; i++) f.pub[i] = u8(std::stoi(hx.substr(2 * i, 2), nullptr, 16));
        f.name = name.substr(0, 32);
        f.state = st == "sent" ? State::RequestSent : st == "received" ? State::RequestReceived : st == "blocked" ? State::Blocked : State::Friend;
        f.code = codeOf(f.pub);
        f.pid = pidOf(f.pub);
        sFriends.push_back(f);
    }
}

Friend* byPub(const u8 pub[32]) {
    for (auto& f : sFriends)
        if (std::memcmp(f.pub, pub, 32) == 0) return &f;
    return nullptr;
}
Friend* byCode(const std::string& code) {
    for (auto& f : sFriends)
        if (f.code == code) return &f;
    return nullptr;
}

void notice(const std::string& t) { online::postNotice(t); }

void send(u8 to, u8 type, const std::vector<u8>& b) { net::session().sendApp(to, type, b.data(), b.size(), true); }

void sendIdent(u8 to, MemberState& m) {
    oc::randomBytes(m.myNonce, 16);
    std::vector<u8> b(sPub, sPub + 32);
    b.insert(b.end(), m.myNonce, m.myNonce + 16);
    const std::string n = online::playerName();
    b.push_back(u8(std::min<size_t>(n.size(), 32)));
    b.insert(b.end(), n.begin(), n.begin() + std::min<size_t>(n.size(), 32));
    b.insert(b.end(), sXPub, sXPub + 32);  // letters key (covered by the proof)
    send(to, F_IDENT, b);
    m.identSent = true;
}

std::string cleanName(const u8* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && s.size() < 32; i++)
        if (p[i] >= 0x20 && p[i] != 0x7F) s += char(p[i]);
    return s;
}

u32 memberPid(u8 id) {
    for (auto& m : net::session().members())
        if (m.id == id) return m.pid;
    return 0;
}

// A friend's INFO into its friend entry (proved key, friend state).
void applyInfo(MemberState& ms) {
    if (!ms.verified || ms.info.size() != kGameInfoSize) return;
    Friend* f = byPub(ms.pub);
    if (f && ms.haveX && !f->haveX) {
        std::memcpy(f->xpub, ms.xpub, 32);
        f->haveX = true;
        save();
    }
    if (!f || f->state != State::Friend || f->gameInfo == ms.info) return;
    f->gameInfo = ms.info;
    save();
    if (online::logOn()) std::printf("[friends] roster entry of %s received\n", f->name.c_str());
}

void sendInfo(u8 to, MemberState& m) {
    if (sMyInfo.size() != kGameInfoSize) return;
    send(to, F_INFO, sMyInfo);
    m.infoSent = true;
}

void onMessage(const net::AppMsg& m) {
    MemberState& ms = sMembers[m.from];
    const auto& d = m.data;
    switch (m.type) {
    case F_IDENT: {
        if (d.size() < 49) return;
        std::memcpy(ms.pub, d.data(), 32);
        ms.havePub = true;
        ms.verified = false;
        const size_t nl = std::min<size_t>(d[48], d.size() - 49);
        ms.name = cleanName(d.data() + 49, nl);
        ms.haveX = d.size() >= 49 + nl + 32;
        if (ms.haveX) std::memcpy(ms.xpub, d.data() + 49 + nl, 32);
        // Prove our key (and our letters key): sign their nonce.
        std::vector<u8> msg = {'c', 'f', '-', 'p', 'r', 'o', 'v', 'e'};
        msg.insert(msg.end(), d.data() + 32, d.data() + 48);
        msg.insert(msg.end(), sPub, sPub + 32);
        msg.insert(msg.end(), sXPub, sXPub + 32);
        u8 sig[64];
        oc::ed25519Sign(online::identitySeed(), msg.data(), msg.size(), sig);
        send(m.from, F_PROVE, std::vector<u8>(sig, sig + 64));
        if (!ms.identSent) sendIdent(m.from, ms);
        break;
    }
    case F_PROVE: {
        if (d.size() != 64 || !ms.havePub) return;
        std::vector<u8> msg = {'c', 'f', '-', 'p', 'r', 'o', 'v', 'e'};
        msg.insert(msg.end(), ms.myNonce, ms.myNonce + 16);
        msg.insert(msg.end(), ms.pub, ms.pub + 32);
        if (ms.haveX) msg.insert(msg.end(), ms.xpub, ms.xpub + 32);
        ms.verified = oc::ed25519Verify(ms.pub, msg.data(), msg.size(), d.data());
        const u32 claimed = memberPid(m.from);
        if (ms.verified && claimed && claimed != pidOf(ms.pub)) {
            std::printf("[friends] member %u: profile id %u does not belong to its key\n", m.from, claimed);
            ms.verified = false;
        }
        if (ms.verified) {
            if (Friend* f = byPub(ms.pub)) {
                bool changed = false;
                if (f->name != ms.name && !ms.name.empty()) {
                    f->name = ms.name;
                    changed = true;
                }
                if (ms.haveX && (!f->haveX || std::memcmp(f->xpub, ms.xpub, 32) != 0)) {
                    std::memcpy(f->xpub, ms.xpub, 32);
                    f->haveX = true;
                    changed = true;
                }
                if (changed) save();
            }
        }
        if (online::logOn()) std::printf("[friends] member %u (%s) key %s\n", m.from, ms.name.c_str(), ms.verified ? "proved" : "NOT proved");
        break;
    }
    case F_REQ: {
        if (!ms.verified) return;
        Friend* f = byPub(ms.pub);
        if (f && f->state == State::Blocked) return;
        if (secs() - ms.lastReq < 20 || ++ms.reqCount > 5) return;  // rate limit
        ms.lastReq = secs();
        if (f && f->state == State::RequestSent) {  // both asked: friends
            f->state = State::Friend;
            send(m.from, F_ACCEPT, {});
            notice("You and " + ms.name + " are friends now");
            applyInfo(ms);
        } else if (!f) {
            Friend nf;
            std::memcpy(nf.pub, ms.pub, 32);
            nf.name = ms.name;
            nf.code = codeOf(nf.pub);
            nf.pid = pidOf(nf.pub);
            nf.state = State::RequestReceived;
            if (ms.haveX) std::memcpy(nf.xpub, ms.xpub, 32), nf.haveX = true;
            sFriends.push_back(nf);
            notice(ms.name + " sent you a friend request (F1 > Online > Friends)");
        }
        save();
        break;
    }
    case F_ACCEPT: {
        if (!ms.verified) return;
        Friend* f = byPub(ms.pub);
        if (f && f->state == State::RequestSent) {
            f->state = State::Friend;
            save();
            notice(ms.name + " accepted your friend request");
            applyInfo(ms);
        }
        break;
    }
    case 0x46:  // MAIL / MAIL_ACK (online_mail.cpp)
    case 0x47:
        mail::onRoomMessage(m.from, m.type, d, ms.verified ? ms.pub : nullptr);
        break;
    case F_EMAIL: {  // a friend's own mail address (only kept for friends; empty = it turned mail off)
        Friend* f = ms.verified ? byPub(ms.pub) : nullptr;
        if (!f || f->state != State::Friend || d.size() > 120) return;
        std::string e;
        for (u8 ch : d)
            if (ch > 0x20 && ch < 0x7F && ch != '<' && ch != '>' && ch != '"') e += char(ch);
        if (!e.empty() && (e.find('@') == std::string::npos || e.find('@') == 0)) return;
        if (e != f->email) {
            f->email = e;
            save();
            if (online::logOn()) std::printf("[friends] %s's mail address %s\n", f->name.c_str(), e.empty() ? "(none)" : "known");
        }
        break;
    }
    case F_ENDPOINTS: {  // a friend's letters link addresses (only kept for friends)
        Friend* f = ms.verified ? byPub(ms.pub) : nullptr;
        if (!f || f->state != State::Friend || d.size() > 200) return;
        std::string e;
        for (u8 ch : d)
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == ':' || ch == ';' || ch == '[' || ch == ']' || (ch >= 'a' && ch <= 'f')) e += char(ch);
        if (e != f->endpoints) {
            f->endpoints = e;
            save();
            if (online::logOn()) std::printf("[friends] %s's letters link: %s\n", f->name.c_str(), e.c_str());
        }
        break;
    }
    case F_INFO: {
        if (d.size() != kGameInfoSize) return;
        ms.info = d;
        applyInfo(ms);
        break;
    }
    case F_DECLINE: {
        Friend* f = ms.verified ? byPub(ms.pub) : nullptr;
        if (f && f->state == State::RequestSent) {
            sFriends.erase(sFriends.begin() + (f - sFriends.data()));
            save();
            notice(ms.name + " declined your friend request");
        }
        break;
    }
    }
}

// Session roster entries of players who are no longer in a room with us go to the removal list
// (also after a crash: the list is in friends.txt).
void sweepSession() {
    if (sSession.empty()) return;
    bool changed = false;
    for (auto it = sSession.begin(); it != sSession.end();) {
        bool here = false, isFriend = false;
        for (auto& [id, ms] : sMembers) here |= ms.verified && pidOf(ms.pub) == *it;
        for (auto& f : sFriends) isFriend |= f.pid == *it && f.state == State::Friend;
        if (here && !isFriend) {
            ++it;
            continue;
        }
        if (!isFriend) sRemoved.push_back(*it);
        it = sSession.erase(it);
        changed = true;
    }
    if (changed) save();
}

u8 memberOfPub(const u8 pub[32]) {
    for (auto& [id, ms] : sMembers)
        if (ms.verified && std::memcmp(ms.pub, pub, 32) == 0) return id;
    return 0xFF;
}

}  // namespace

const uint8_t* myPublicKey() {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    return sPub;
}

uint32_t pidOf(const uint8_t pub[32]) {
    u8 h[32];
    oc::hmacSha256("cf-dwc-profile-id", 17, pub, 32, h);
    u32 pid = (u32(h[0]) << 24 | u32(h[1]) << 16 | u32(h[2]) << 8 | h[3]) & 0x7FFFFFFFu;
    if (pid < 0x10000) pid |= 0x10000;
    return pid;
}

std::string codeOf(const uint8_t pub[32]) {
    static const char* b32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    u8 h[32];
    oc::sha256(pub, 32, h);
    std::string s;
    u32 acc = 0;
    int bits = 0;
    for (int i = 0; i < 10; i++) {
        acc = acc << 8 | h[i];
        bits += 8;
        while (bits >= 5) {
            s += b32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    return s.substr(0, 4) + "-" + s.substr(4, 4) + "-" + s.substr(8, 4) + "-" + s.substr(12, 4);
}

std::string myCode() { return codeOf(myPublicKey()); }

void setMyGameInfo(const std::vector<uint8_t>& info) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    if (info.size() != kGameInfoSize || info == sMyInfo) return;
    sMyInfo = info;
    for (auto& [id, ms] : sMembers) ms.infoSent = false;  // tick() sends it again
}

void tick() {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    auto& s = net::session();
    if (!s.active()) {
        mail::tick();  // starts the relay worker (letters while not in a room)
        sMembers.clear();
        sweepSession();
        return;
    }
    const u8 self = s.selfId();
    const auto ms = s.members();
    // Forget members that left; introduce ourselves to new ones.
    for (auto it = sMembers.begin(); it != sMembers.end();) {
        bool present = false;
        for (auto& m : ms) present |= m.id == it->first;
        it = present ? std::next(it) : sMembers.erase(it);
    }
    for (auto& m : ms) {
        if (m.id == self || m.self) continue;
        auto& st = sMembers[m.id];
        if (!st.identSent) sendIdent(m.id, st);
        if (!st.infoSent) sendInfo(m.id, st);
        if (st.verified && st.emailSent != sMyEmail) {
            const Friend* f = byPub(st.pub);
            if (f && f->state == State::Friend) {
                send(m.id, F_EMAIL, std::vector<u8>(sMyEmail.begin(), sMyEmail.end()));
                st.emailSent = sMyEmail;
            }
        }
        if (st.verified && !sMyEndpoints.empty() && st.epSent != sMyEndpoints) {
            const Friend* f = byPub(st.pub);
            if (f && f->state == State::Friend) {
                send(m.id, F_ENDPOINTS, std::vector<u8>(sMyEndpoints.begin(), sMyEndpoints.end()));
                st.epSent = sMyEndpoints;
            }
        }
    }
    net::AppMsg msg;
    int budget = 64;
    while (budget-- > 0 && s.pollApp(msg, 0x40, 0x5F)) onMessage(msg);
    sweepSession();
    mail::tick();
}

Rel relation(uint8_t memberId) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    auto it = sMembers.find(memberId);
    if (it == sMembers.end() || !it->second.verified) return Rel::Unknown;
    const Friend* f = byPub(it->second.pub);
    if (!f) return Rel::Verified;
    switch (f->state) {
    case State::Friend: return Rel::Friend;
    case State::RequestSent: return Rel::RequestSent;
    case State::RequestReceived: return Rel::RequestReceived;
    case State::Blocked: return Rel::Blocked;
    }
    return Rel::Verified;
}

std::string memberCode(uint8_t memberId) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    auto it = sMembers.find(memberId);
    return it != sMembers.end() && it->second.verified ? codeOf(it->second.pub) : std::string();
}

void requestFriend(uint8_t memberId) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    auto it = sMembers.find(memberId);
    if (it == sMembers.end() || !it->second.verified) return;
    auto& ms = it->second;
    Friend* f = byPub(ms.pub);
    if (f && f->state == State::RequestReceived) {  // they asked first: this accepts
        f->state = State::Friend;
        send(memberId, F_ACCEPT, {});
        notice("You and " + ms.name + " are friends now");
        save();
        applyInfo(ms);
        return;
    } else if (!f) {
        Friend nf;
        std::memcpy(nf.pub, ms.pub, 32);
        nf.name = ms.name;
        nf.code = codeOf(nf.pub);
        nf.pid = pidOf(nf.pub);
        nf.state = State::RequestSent;
        if (ms.haveX) std::memcpy(nf.xpub, ms.xpub, 32), nf.haveX = true;
        sFriends.push_back(nf);
        const std::string n = online::playerName();
        send(memberId, F_REQ, std::vector<u8>(n.begin(), n.end()));
    } else {
        return;
    }
    save();
}

std::vector<Friend> list() {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    return sFriends;
}

std::vector<Friend> sessionMembers() {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    std::vector<Friend> v;
    for (auto& [id, ms] : sMembers) {
        if (!ms.verified || ms.info.size() != kGameInfoSize) continue;
        if (byPub(ms.pub) && (byPub(ms.pub)->state == State::Friend || byPub(ms.pub)->state == State::Blocked)) continue;
        Friend f;
        std::memcpy(f.pub, ms.pub, 32);
        f.name = ms.name;
        f.code = codeOf(ms.pub);
        f.pid = pidOf(ms.pub);
        f.state = State::Friend;
        f.gameInfo = ms.info;
        v.push_back(std::move(f));
    }
    return v;
}

void noteSessionRoster(uint32_t pid) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    for (u32 p : sSession)
        if (p == pid) return;
    sSession.push_back(pid);
    // Queued for removal earlier (it left, then came back before the game dropped it): it stays.
    for (auto it = sRemoved.begin(); it != sRemoved.end();) it = *it == pid ? sRemoved.erase(it) : std::next(it);
    save();
}

uint8_t memberOf(const uint8_t pub[32]) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    return memberOfPub(pub);
}

bool isFriend(const uint8_t pub[32]) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    const Friend* f = byPub(pub);
    return f && f->state == State::Friend;
}

bool friendByWiiNumber(uint64_t number, uint8_t pubOut[32]) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    for (auto& f : sFriends)
        if (f.state == State::Friend && wiiNumberOf(f.pub) == number) {
            std::memcpy(pubOut, f.pub, 32);
            return true;
        }
    return false;
}

uint64_t wiiNumberOf(const uint8_t pub[32]) {
    u8 h[32];
    oc::hmacSha256("cf-wii-number", 13, pub, 32, h);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | h[i];
    return 1000000000000000ull + v % 9000000000000000ull;  // 16 digits, as a Wii number
}

uint64_t myWiiNumber() { return wiiNumberOf(myPublicKey()); }

void setMyEmail(const std::string& address) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    sMyEmail = address;
}

std::string friendEmail(const uint8_t pub[32]) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    const Friend* f = byPub(pub);
    return f && f->state == State::Friend ? f->email : std::string();
}

void setMyEndpoints(const std::string& eps) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    sMyEndpoints = eps;
}

std::string myEndpointsText() {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    return sMyEndpoints;
}

std::string friendEndpoints(const uint8_t pub[32]) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    const Friend* f = byPub(pub);
    return f && f->state == State::Friend ? f->endpoints : std::string();
}

void setFriendEndpoints(const uint8_t pub[32], const std::string& eps) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    Friend* f = byPub(pub);
    if (!f || f->state != State::Friend || eps.size() > 200) return;
    std::string e;
    for (char ch : eps)
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == ':' || ch == ';' || ch == '[' || ch == ']' || (ch >= 'a' && ch <= 'f')) e += ch;
    if (e.empty() || e == f->endpoints) return;
    f->endpoints = e;
    save();
    if (online::logOn()) std::printf("[friends] %s's letters link (from the link): %s\n", f->name.c_str(), e.c_str());
}

bool friendXPub(const uint8_t pub[32], uint8_t out[32]) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    const Friend* f = byPub(pub);
    if (!f || !f->haveX || f->state != State::Friend) return false;
    std::memcpy(out, f->xpub, 32);
    return true;
}

const uint8_t* myXPub() {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    return sXPub;
}

void letterKey(const uint8_t theirX[32], uint8_t out[32]) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    u8 shared[32];
    oc::x25519(sXPriv, theirX, shared);
    oc::hmacSha256(shared, 32, "cf-letter", 9, out);
}

bool inRoom(const std::string& code) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    const Friend* f = byCode(code);
    return f && memberOfPub(f->pub) != 0xFF;
}

std::vector<Friend> incoming() {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    std::vector<Friend> v;
    for (auto& f : sFriends)
        if (f.state == State::RequestReceived) v.push_back(f);
    return v;
}

void accept(const std::string& code) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    Friend* f = byCode(code);
    if (!f || f->state != State::RequestReceived) return;
    f->state = State::Friend;
    const u8 m = memberOfPub(f->pub);
    if (m != 0xFF) send(m, F_ACCEPT, {});
    save();
    if (m != 0xFF) applyInfo(sMembers[m]);
}

void decline(const std::string& code) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    Friend* f = byCode(code);
    if (!f || f->state != State::RequestReceived) return;
    const u8 m = memberOfPub(f->pub);
    if (m != 0xFF) send(m, F_DECLINE, {});
    sFriends.erase(sFriends.begin() + (f - sFriends.data()));
    save();
}

void block(const std::string& code) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    if (Friend* f = byCode(code)) {
        if (f->state == State::Friend) sRemoved.push_back(f->pid);
        f->state = State::Blocked;
        save();
    }
}

void remove(const std::string& code) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    if (Friend* f = byCode(code)) {
        if (f->state == State::Friend) sRemoved.push_back(f->pid);
        sFriends.erase(sFriends.begin() + (f - sFriends.data()));
        save();
    }
}

std::vector<uint32_t> pendingRosterRemovals() {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    load();
    return sRemoved;
}

void rosterRemovalDone(uint32_t pid) {
    std::lock_guard<std::recursive_mutex> g(sMutex);
    for (auto it = sRemoved.begin(); it != sRemoved.end();)
        it = *it == pid ? sRemoved.erase(it) : std::next(it);
    // A friend again (re-added before the game got to remove it) keeps its entry.
    save();
}

}  // namespace recomp::online::friends
