// Letters between F1 friends (online_mail.h, docs/ONLINE.md "Letters").
//
// NWC24 message object (the library's, 0x100 bytes; offsets from its accessors): +0x00 message id
// (NWC24GetMsgAppId; what NWC24DeleteMsg takes), +0x04 flags (bit 0: has a sender id; bits 2/3:
// type, NWC24GetMsgType; 0x200: attachments readable), +0x0C tag (the game: 'RUU' + region),
// +0x20 sender id (u64), +0x68 attachment data (NWC24Data {ptr, size} x 2), +0x78 sizes [2],
// +0x80 MIME types [2], +0x88 recipients (u64 x 8), +0xC8 recipient count, +0xC9 attachment count.
// The game (fn_800E9AC8) attaches the letter as type 0x30000 and commits it; its WiiConnect24
// task (fn_8017B6F4 -> fn_800EA0DC, on the save path) searches the receive box, keeps 'RUU'
// messages with a 0x30000 attachment, reads it (fn_8040B21C = NWC24ReadMsgAttached here) and
// deletes the message.
//
// Files: <data>/online/mail/out/<id>.ltr and in/<id>.ltr:
//   "CFLT" u32 version 1, u64 id, peer key[32], u64 sender Wii number, u32 tag, u8 count,
//   count x (u32 type, u32 size, data). Big-endian.
// Room messages: MAIL 0x46 = the same record without "CFLT"/version and with the sender's key
// implied by the proved member; MAIL_ACK 0x47 = u64 id.
#include "recomp/online_mail.h"

#include "port/install.h"
#include "recomp/hle.h"
#include "recomp/online.h"
#include "recomp/online_crypto.h"
#include "recomp/online_friends.h"
#include "recomp/online_net.h"
#include "recomp/online_ed25519.h"
#include "recomp/online_sock.h"
#include "port/settings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace recomp::online::mail {
namespace {

namespace net = ::online::net;
namespace oc = ::onlinecrypto;
namespace fs = std::filesystem;

constexpr u8 M_MAIL = 0x46, M_ACK = 0x47;
constexpr u32 kMaxAttach = 0x40000;   // the game reads up to 0x25800
constexpr size_t kMaxBox = 200;       // letters kept per box
constexpr u32 kOurIdBase = 0x7C000000;  // message ids handed to the game for inbox letters

struct Attachment {
    u32 type = 0;
    std::vector<u8> data;
};
struct Letter {
    u64 id = 0;
    u8 peer[32] = {};  // out: recipient; in: sender
    u64 senderWii = 0;
    u32 tag = 0;
    std::vector<Attachment> att;
    double lastSent = -1e9;
};

std::mutex sMutex;
bool sLoaded = false;
std::map<u64, Letter> sOut, sIn;
std::map<u32, u64> sGameIds;  // game message id -> inbox letter id
int sSearchBox = -1;

double secs() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

fs::path dir(bool out) { return port::dataDir() / "online" / "mail" / (out ? "out" : "in"); }

void put32(std::vector<u8>& b, u32 v) {
    for (int i = 3; i >= 0; i--) b.push_back(u8(v >> (8 * i)));
}
void put64(std::vector<u8>& b, u64 v) {
    for (int i = 7; i >= 0; i--) b.push_back(u8(v >> (8 * i)));
}
struct Rd {
    const std::vector<u8>& b;
    size_t i = 0;
    bool ok = true;
    u32 r32() {
        if (i + 4 > b.size()) return ok = false, 0;
        u32 v = 0;
        for (int k = 0; k < 4; k++) v = v << 8 | b[i++];
        return v;
    }
    u64 r64() {
        const u64 hi = r32();
        return hi << 32 | r32();
    }
    u8 r8() { return i < b.size() ? b[i++] : (ok = false, 0); }
    bool raw(void* p, size_t n) {
        if (i + n > b.size()) return ok = false;
        std::memcpy(p, b.data() + i, n);
        i += n;
        return true;
    }
};

std::vector<u8> encode(const Letter& l, bool withPeer) {
    std::vector<u8> b;
    put64(b, l.id);
    if (withPeer) b.insert(b.end(), l.peer, l.peer + 32);
    put64(b, l.senderWii);
    put32(b, l.tag);
    b.push_back(u8(l.att.size()));
    for (auto& a : l.att) {
        put32(b, a.type);
        put32(b, u32(a.data.size()));
        b.insert(b.end(), a.data.begin(), a.data.end());
    }
    return b;
}
bool decode(const std::vector<u8>& b, size_t at, bool withPeer, Letter& l) {
    Rd r{b, at};
    l.id = r.r64();
    if (withPeer) r.raw(l.peer, 32);
    l.senderWii = r.r64();
    l.tag = r.r32();
    const u8 n = r.r8();
    if (!r.ok || n > 2) return false;
    for (u8 k = 0; k < n; k++) {
        Attachment a;
        a.type = r.r32();
        const u32 size = r.r32();
        if (!r.ok || size > kMaxAttach || r.i + size > b.size()) return false;
        a.data.assign(b.begin() + long(r.i), b.begin() + long(r.i + size));
        r.i += size;
        l.att.push_back(std::move(a));
    }
    return r.ok;
}

void store(bool out, const Letter& l) {
    std::error_code ec;
    fs::create_directories(dir(out), ec);
    std::vector<u8> b = {'C', 'F', 'L', 'T'};
    put32(b, 1);
    const auto body = encode(l, true);
    b.insert(b.end(), body.begin(), body.end());
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.ltr", (unsigned long long)l.id);
    std::ofstream f(dir(out) / name, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
void unstore(bool out, u64 id) {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.ltr", (unsigned long long)id);
    std::error_code ec;
    fs::remove(dir(out) / name, ec);
}

void load() {
    if (sLoaded) return;
    sLoaded = true;
    for (bool out : {true, false}) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir(out), ec)) {
            std::ifstream f(e.path(), std::ios::binary);
            const std::vector<u8> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            Letter l;
            if (b.size() < 8 || std::memcmp(b.data(), "CFLT", 4) != 0 || !decode(b, 8, true, l)) continue;
            (out ? sOut : sIn)[l.id] = std::move(l);
        }
    }
}

u64 newId() {
    u8 r[8];
    oc::randomBytes(r, 8);
    u64 v = 0;
    for (u8 x : r) v = v << 8 | x;
    return v | 1;
}

bool logOn() { return online::logOn(); }

// Game message id for an inbox letter (stable while it is in the inbox).
u32 gameIdOf(u64 letterId) {
    for (auto& [g, l] : sGameIds)
        if (l == letterId) return g;
    const u32 g = kOurIdBase + u32(sGameIds.size() + 1);
    sGameIds[g] = letterId;
    return g;
}

// ---- sealed letters for paths outside a room (email: online_email.cpp) ----
// Sealed for the friend pair (friends::letterKey: X25519 of the two letters keys), signed with our
// Ed25519 key; only a friend's signed letters open.
namespace sk = ::online::sock;
std::set<u64> sSeen;  // letter ids already taken in (direct and relay copies of one letter)
bool sWake = false;

std::string relayName() {
    if (const char* v = std::getenv("CF_ONLINE_RELAY")) return v;
    return port::settings().online.relay;
}

struct Tcp {
    sk::Socket s = sk::kInvalid;
    ~Tcp() {
        if (s != sk::kInvalid) sk::closeSocket(s);
    }
    bool connect(const std::string& name) {
        sk::Addr a;
        if (!sk::resolve(name, net::kRelayPort, &a)) return false;
        a = a.normalized();
        s = socket(a.family(), SOCK_STREAM, IPPROTO_TCP);
        if (s == sk::kInvalid) return false;
#if defined(_WIN32)
        DWORD tmo = 5000;
#else
        timeval tmo{5, 0};
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
        return ::connect(s, reinterpret_cast<const sockaddr*>(&a.ss), a.len) == 0;
    }
    bool write(const std::vector<u8>& b) {
        std::vector<u8> f;
        put32(f, u32(b.size()));
        f.insert(f.end(), b.begin(), b.end());
        size_t off = 0;
        while (off < f.size()) {
            const int k = int(send(s, reinterpret_cast<const char*>(f.data() + off), int(f.size() - off), 0));
            if (k <= 0) return false;
            off += size_t(k);
        }
        return true;
    }
    bool readN(u8* p, size_t n) {
        while (n) {
            const int k = int(recv(s, reinterpret_cast<char*>(p), int(n), 0));
            if (k <= 0) return false;
            p += k;
            n -= size_t(k);
        }
        return true;
    }
    bool read(std::vector<u8>& b) {
        u8 h[4];
        if (!readN(h, 4)) return false;
        const u32 len = u32(h[0]) << 24 | u32(h[1]) << 16 | u32(h[2]) << 8 | h[3];
        if (!len || len > 4u * 1024 * 1024) return false;
        b.resize(len);
        return readN(b.data(), len);
    }
};

std::vector<u8> sealFor(const Letter& l, const u8 to[32], const u8 theirX[32]) {
    u8 key[32];
    friends::letterKey(theirX, key);
    const oc::SealKey k = oc::deriveSealKey(key, "letter");
    std::vector<u8> body = encode(l, false);
    u8 nonce[8];
    oc::randomBytes(nonce, 8);
    u64 n = 0;
    for (u8 x : nonce) n = n << 8 | x;
    u8 hdr[64];
    std::memcpy(hdr, friends::myPublicKey(), 32);
    std::memcpy(hdr + 32, to, 32);
    u8 tag[oc::kTagSize];
    oc::seal(k, n, hdr, 64, body.data(), body.size(), tag);
    std::vector<u8> blob(nonce, nonce + 8);
    blob.insert(blob.end(), body.begin(), body.end());
    blob.insert(blob.end(), tag, tag + oc::kTagSize);
    u8 sig[64];
    oc::ed25519Sign(online::identitySeed(), blob.data(), blob.size(), sig);
    blob.insert(blob.end(), sig, sig + 64);
    return blob;
}

bool openFrom(const u8 from[32], const std::vector<u8>& blob, Letter& l) {
    if (blob.size() < 8 + oc::kTagSize + 64) return false;
    const size_t sealedLen = blob.size() - 64;
    if (!oc::ed25519Verify(from, blob.data(), sealedLen, blob.data() + sealedLen)) return false;
    u8 theirX[32];
    if (!friends::friendXPub(from, theirX)) return false;
    u8 key[32];
    friends::letterKey(theirX, key);
    const oc::SealKey k = oc::deriveSealKey(key, "letter");
    u64 n = 0;
    for (int i = 0; i < 8; i++) n = n << 8 | blob[size_t(i)];
    std::vector<u8> body(blob.begin() + 8, blob.begin() + long(sealedLen - oc::kTagSize));
    u8 hdr[64];
    std::memcpy(hdr, from, 32);
    std::memcpy(hdr + 32, friends::myPublicKey(), 32);
    if (!oc::open(k, n, hdr, 64, body.data(), body.size(), blob.data() + sealedLen - oc::kTagSize)) return false;
    return decode(body, 0, false, l);
}

void takeIn(Letter&& l, const u8 from[32]);

}  // namespace

namespace {
void takeInLocked(Letter&& l) {
    if (sIn.count(l.id) || sSeen.count(l.id)) return;  // a repeat (a lost ack, or both routes)
    if (sIn.size() >= kMaxBox) {
        std::printf("[mail] inbox full: a letter was dropped\n");
        return;
    }
    std::printf("[mail] letter %016llx received (%zu attachments)\n", (unsigned long long)l.id, l.att.size());
    sSeen.insert(l.id);
    store(false, l);
    sIn[l.id] = std::move(l);
    online::postNotice("A letter from a friend has arrived");
}
void takeIn(Letter&& l, const u8 from[32]) {
    std::lock_guard<std::mutex> g(sMutex);
    load();
    std::memcpy(l.peer, from, 32);
    l.senderWii = friends::wiiNumberOf(from);
    takeInLocked(std::move(l));
}
}  // namespace

void onRoomMessage(uint8_t from, uint8_t type, const std::vector<uint8_t>& data, const uint8_t* pub) {
    std::lock_guard<std::mutex> g(sMutex);
    load();
    if (!pub) return;
    if (type == M_ACK) {
        Rd r{data};
        const u64 id = r.r64();
        auto it = sOut.find(id);
        if (!r.ok || it == sOut.end() || std::memcmp(it->second.peer, pub, 32) != 0) return;
        std::printf("[mail] letter %016llx delivered\n", (unsigned long long)id);
        unstore(true, id);
        sOut.erase(it);
        return;
    }
    if (type != M_MAIL || !friends::isFriend(pub)) return;  // only friends can mail us
    Letter l;
    if (!decode(data, 0, false, l)) return;
    std::memcpy(l.peer, pub, 32);
    l.senderWii = friends::wiiNumberOf(pub);  // the sender's number from its key
    std::vector<u8> ack;
    put64(ack, l.id);
    net::session().sendApp(from, M_ACK, ack.data(), ack.size(), true);
    takeInLocked(std::move(l));
}

void tick() {
    std::lock_guard<std::mutex> g(sMutex);
    load();
    auto& s = net::session();
    if (!s.active()) return;
    for (auto& [id, l] : sOut) {
        const u8 m = friends::memberOf(l.peer);
        if (m == 0xFF || secs() - l.lastSent < 30) continue;  // resend every 30 s until acknowledged
        const auto b = encode(l, false);
        s.sendApp(m, M_MAIL, b.data(), b.size(), true);
        l.lastSent = secs();
        if (logOn()) std::printf("[mail] letter %016llx sent to member %u\n", (unsigned long long)id, m);
    }
}

int queueTestLetter() {
    int n = 0;
    for (auto& f : friends::list()) {
        if (f.state != friends::State::Friend) continue;
        Letter l;
        l.id = newId();
        std::memcpy(l.peer, f.pub, 32);
        l.senderWii = friends::myWiiNumber();
        l.tag = 0x52555545;  // 'RUUE'
        Attachment a;
        a.type = 0x30000;
        a.data.assign(64, 0x41);
        l.att.push_back(a);
        std::lock_guard<std::mutex> g(sMutex);
        load();
        store(true, l);
        sOut[l.id] = std::move(l);
        sWake = true;
        n++;
    }
    return n;
}

Counts counts() {
    std::lock_guard<std::mutex> g(sMutex);
    load();
    return {int(sOut.size()), int(sIn.size())};
}

}  // namespace recomp::online::mail

// ---- NWC24 (the game's WiiConnect24 library) ----
namespace recomp {
namespace {
namespace om = recomp::online::mail;
namespace fr = recomp::online::friends;

u64 rd64(u32 a) { return u64(rd32(a)) << 32 | rd32(a + 4); }
}  // namespace

// Wii numbers of F1 friends are valid recipients (their check digits are not the NWC24 ones).
HLE(NWC24CheckUserId) {
    const u64 id = u64(c.r[3]) << 32 | c.r[4];
    u8 pub[32];
    if (id == fr::myWiiNumber()) {
        c.r[3] = u32(-0x23);  // as the library: the own number
        return;
    }
    if (fr::friendByWiiNumber(id, pub)) {
        c.r[3] = 0;
        return;
    }
    callOriginal(c, "NWC24CheckUserId");
}

// A letter to F1 friends goes to the outbox; others to the library as before.
HLE(NWC24CommitMsg) {
    const u32 obj = c.r[3];
    const u32 n = rd8(obj + 0xC8), na = rd8(obj + 0xC9);
    std::vector<std::array<u8, 32>> to;
    bool other = false;
    for (u32 i = 0; i < n && i < 8; i++) {
        const u64 id = rd64(obj + 0x88 + 8 * i);
        std::array<u8, 32> pub{};
        if (fr::friendByWiiNumber(id, pub.data())) to.push_back(pub);
        else other = true;
    }
    if (to.empty()) return callOriginal(c, "NWC24CommitMsg");
    const u64 myWii = fr::myWiiNumber();  // before the mail lock (lock order: friends -> mail)
    {
        std::lock_guard<std::mutex> g(om::sMutex);
        om::load();
        for (auto& pub : to) {
            om::Letter l;
            l.id = om::newId();
            std::memcpy(l.peer, pub.data(), 32);
            l.senderWii = myWii;
            l.tag = rd32(obj + 0x0C);
            for (u32 k = 0; k < na && k < 2; k++) {
                om::Attachment a;
                a.type = rd32(obj + 0x80 + 4 * k);
                const u32 ptr = rd32(obj + 0x68 + 8 * k), size = std::min<u32>(rd32(obj + 0x6C + 8 * k), om::kMaxAttach);
                if (ptr && size) a.data.assign(hostp(ptr), hostp(ptr) + size);
                l.att.push_back(std::move(a));
            }
            if (om::sOut.size() >= om::kMaxBox) {
                std::printf("[mail] outbox full: letter not kept\n");
                continue;
            }
            std::printf("[mail] letter %016llx to a friend queued (%zu attachments, tag %08X)\n", (unsigned long long)l.id,
                        l.att.size(), l.tag);
            om::store(true, l);
            om::sOut[l.id] = std::move(l);
            om::sWake = true;  // the relay (if any) takes it when the friend is away
        }
    }
    if (other) return callOriginal(c, "NWC24CommitMsg");
    c.r[3] = 0;
}

HLE(NWC24SetSearchCondMsgBox) {
    om::sSearchBox = int(c.r[3]);
    callOriginal(c, "NWC24SetSearchCondMsgBox");
}

// (objects, max, &found, &remaining): inbox letters first, as received WiiConnect24 messages.
HLE(NWC24SearchMsgs) {
    const u32 objs = c.r[3], max = c.r[4], foundP = c.r[5], restP = c.r[6];
    std::unique_lock<std::mutex> g(om::sMutex);
    om::load();
    if (om::sSearchBox != 1 || om::sIn.empty() || !objs || max == 0) {
        g.unlock();
        return callOriginal(c, "NWC24SearchMsgs");
    }
    const om::Letter& l = om::sIn.begin()->second;
    std::memset(hostp(objs), 0, 0x100);
    wr32(objs + 0x00, om::gameIdOf(l.id));
    wr32(objs + 0x04, 0x1u | 0x200u | 0x10000u);  // sender id, attachments readable; type 3
    wr32(objs + 0x0C, l.tag ? l.tag : 0x52555500u);
    wr32(objs + 0x20, u32(l.senderWii >> 32));
    wr32(objs + 0x24, u32(l.senderWii));
    wr8(objs + 0xC9, u8(std::min<size_t>(l.att.size(), 2)));
    for (size_t k = 0; k < l.att.size() && k < 2; k++) {
        wr32(objs + 0x78 + 4 * u32(k), u32(l.att[k].data.size()));
        wr32(objs + 0x80 + 4 * u32(k), l.att[k].type);
    }
    if (foundP) wr32(foundP, 1);
    if (restP) wr32(restP, u32(om::sIn.size() - 1));
    std::printf("[mail] letter %016llx handed to the game\n", (unsigned long long)l.id);
    c.r[3] = 0;
}

// fn_8040B21C (obj, index, buf, bufSize): attachment data.
HLE(NWC24ReadMsgAttached) {
    const u32 obj = c.r[3], index = c.r[4], buf = c.r[5], size = c.r[6];
    std::unique_lock<std::mutex> g(om::sMutex);
    const auto it = om::sGameIds.find(rd32(obj));
    const auto lt = it == om::sGameIds.end() ? om::sIn.end() : om::sIn.find(it->second);
    if (lt == om::sIn.end()) {
        g.unlock();
        return callOriginal(c, "NWC24ReadMsgAttached");
    }
    if (index >= lt->second.att.size()) {
        c.r[3] = u32(-0xD);
        return;
    }
    const auto& d = lt->second.att[index].data;
    if (d.size() > size) {
        c.r[3] = u32(-8);
        return;
    }
    if (!d.empty()) std::memcpy(hostp(buf), d.data(), d.size());
    c.r[3] = 0;
}

HLE(NWC24DeleteMsg) {
    const u32 box = c.r[3], id = c.r[4];
    std::unique_lock<std::mutex> g(om::sMutex);
    const auto it = om::sGameIds.find(id);
    if (box != 1 || it == om::sGameIds.end()) {
        g.unlock();
        return callOriginal(c, "NWC24DeleteMsg");
    }
    std::printf("[mail] letter %016llx taken by the game\n", (unsigned long long)it->second);
    om::unstore(false, it->second);
    om::sIn.erase(it->second);
    om::sGameIds.erase(it);
    c.r[3] = 0;
}

}  // namespace recomp
