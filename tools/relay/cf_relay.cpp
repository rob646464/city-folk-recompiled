// cf_relay: the optional self-hosted relay for City Folk Recompiled online play (docs/ONLINE.md
// "Relay"). Anyone (a friend group) can run it on a machine the players can reach; nothing in the
// port needs it, and it never sees game data in the clear.
//
//   cf_relay [--port 29912] [--mail-dir <folder>] [--max-rooms 256]
//
// UDP (rooms): a host whose router cannot be reached registers its room under a random 8-byte
// room id (part of its room code); joiners reach it through the relay. Datagrams (sealed by the
// room's own encryption, end to end) are only forwarded between a room's host and its joiners.
//   "CFRL" ver(1) type
//   1 REG      room[8]                      host -> relay (every 10 s; answer 2 REG_OK room[8])
//   (answers and forwards carry room[8] after the type as well)
//   3 TO_HOST  room[8] payload              joiner -> relay -> host as 4 FROM_PEER peer(u32) payload
//   5 TO_PEER  room[8] peer(u32) payload    host -> relay -> joiner as 6 FROM_HOST payload
// TCP (letters, store-and-forward for friends who are not online at the same time): frames
// u32 length (big-endian) + body; body = type + fields.
//   10 PUT    to[32] from[32] id[8] blob     (blob sealed for the recipient by the sender)
//   11 AUTH   key[32]  -> 12 CHALLENGE nonce[32]; 13 PROVE sig[64] (Ed25519 over "cf-relay" nonce)
//   14 LIST   (after PROVE) -> 15 ITEMS count(u16) { from[32] id[8] size(u32) blob }
//   16 DEL    id[8]   (after PROVE)
//   17 OK / 18 ERR text
// Limits: 64 letters per recipient, 256 KB per letter, 30 days, 600 datagrams/s per address.
#include "recomp/online_crypto.h"
#include "recomp/online_ed25519.h"
#include "recomp/online_sock.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace online::sock;
namespace oc = ::onlinecrypto;
namespace fs = std::filesystem;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

namespace {

double now() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

std::string hex(const u8* p, size_t n) { return oc::hex(p, n); }

struct Room {
    Addr host;
    double seen = 0;
    std::map<u32, Addr> peers;   // peer id -> joiner address
    std::map<std::string, u32> byAddr;
    u32 next = 1;
};
std::map<u64, Room> sRooms;
std::map<std::string, std::pair<double, int>> sRate;
size_t sMaxRooms = 256;

u64 roomOf(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

bool rateOk(const Addr& a) {
    auto& r = sRate[a.ipText()];
    const double t = now();
    if (t - r.first > 1.0) r = {t, 0};
    return ++r.second <= 600;
}

void udpLoop(Socket s, int fam) {
    std::vector<u8> buf(2048);
    double lastSweep = 0;
    while (true) {
        sockaddr_storage from{};
        socklen_t fl = sizeof from;
        const int n = int(recvfrom(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl));
        const double t = now();
        if (t - lastSweep > 10) {
            lastSweep = t;
            for (auto it = sRooms.begin(); it != sRooms.end();) it = t - it->second.seen > 60 ? sRooms.erase(it) : std::next(it);
            sRate.clear();
        }
        if (n < 14 || std::memcmp(buf.data(), "CFRL", 4) != 0 || buf[4] != 1) continue;
        Addr a;
        std::memcpy(&a.ss, &from, fl);
        a.len = fl;
        if (!rateOk(a)) continue;
        const u8 type = buf[5];
        const u64 id = roomOf(buf.data() + 6);
        auto send = [&](const Addr& to, const std::vector<u8>& b) {
            const Addr x = to.forSocket(fam);
            sendto(s, reinterpret_cast<const char*>(b.data()), int(b.size()), 0, reinterpret_cast<const sockaddr*>(&x.ss), x.len);
        };
        std::vector<u8> out = {'C', 'F', 'R', 'L', 1, 0};
        if (type == 1) {  // REG
            auto it = sRooms.find(id);
            if (it == sRooms.end()) {
                if (sRooms.size() >= sMaxRooms) continue;
                it = sRooms.emplace(id, Room{}).first;
                std::printf("[relay] room %016llx registered by %s\n", (unsigned long long)id, a.text().c_str());
            }
            it->second.host = a;
            it->second.seen = t;
            out[5] = 2;
            out.insert(out.end(), buf.begin() + 6, buf.begin() + 14);
            send(a, out);
        } else if (type == 3) {  // TO_HOST
            auto it = sRooms.find(id);
            if (it == sRooms.end()) continue;
            Room& r = it->second;
            const std::string key = a.text();
            u32 peer = 0;
            if (auto p = r.byAddr.find(key); p != r.byAddr.end()) peer = p->second;
            else {
                if (r.peers.size() >= 16) continue;
                peer = r.next++;
                r.peers[peer] = a;
                r.byAddr[key] = peer;
            }
            out[5] = 4;
            out.insert(out.end(), buf.begin() + 6, buf.begin() + 14);  // room
            for (int i = 3; i >= 0; i--) out.push_back(u8(peer >> (8 * i)));
            out.insert(out.end(), buf.begin() + 14, buf.begin() + n);
            send(r.host, out);
        } else if (type == 5 && n >= 18) {  // TO_PEER
            auto it = sRooms.find(id);
            if (it == sRooms.end() || !(it->second.host == a)) continue;  // only the room's host
            const u32 peer = u32(buf[14]) << 24 | u32(buf[15]) << 16 | u32(buf[16]) << 8 | buf[17];
            auto p = it->second.peers.find(peer);
            if (p == it->second.peers.end()) continue;
            it->second.seen = t;
            out[5] = 6;
            out.insert(out.end(), buf.begin() + 6, buf.begin() + 14);  // room
            out.insert(out.end(), buf.begin() + 18, buf.begin() + n);
            send(p->second, out);
        }
    }
}

// ---- letters (TCP) ----
fs::path sMailDir;
std::mutex sMailMutex;

bool readAll(Socket c, u8* p, size_t n) {
    while (n) {
        const int k = int(recv(c, reinterpret_cast<char*>(p), int(n), 0));
        if (k <= 0) return false;
        p += k;
        n -= size_t(k);
    }
    return true;
}
bool readFrame(Socket c, std::vector<u8>& b) {
    u8 h[4];
    if (!readAll(c, h, 4)) return false;
    const u32 len = u32(h[0]) << 24 | u32(h[1]) << 16 | u32(h[2]) << 8 | h[3];
    if (len == 0 || len > 300 * 1024) return false;
    b.resize(len);
    return readAll(c, b.data(), len);
}
void writeFrame(Socket c, const std::vector<u8>& b) {
    std::vector<u8> f = {u8(b.size() >> 24), u8(b.size() >> 16), u8(b.size() >> 8), u8(b.size())};
    f.insert(f.end(), b.begin(), b.end());
    send(c, reinterpret_cast<const char*>(f.data()), int(f.size()), 0);
}
void err(Socket c, const char* t) {
    std::vector<u8> b = {18};
    b.insert(b.end(), t, t + std::strlen(t));
    writeFrame(c, b);
}

void client(Socket c) {
    std::vector<u8> b;
    u8 key[32] = {}, nonce[32] = {};
    bool proved = false, haveKey = false;
    while (readFrame(c, b)) {
        const u8 type = b[0];
        if (type == 10 && b.size() > 1 + 32 + 32 + 8) {  // PUT
            const std::string to = hex(b.data() + 1, 32);
            const fs::path d = sMailDir / to;
            std::lock_guard<std::mutex> g(sMailMutex);
            std::error_code ec;
            fs::create_directories(d, ec);
            size_t count = 0;
            for (auto& e : fs::directory_iterator(d, ec)) (void)e, count++;
            if (count >= 64 || b.size() > 256 * 1024 + 80) {
                err(c, "mailbox full");
                continue;
            }
            std::ofstream f(d / (hex(b.data() + 65, 8) + ".ltr"), std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(b.data() + 33), std::streamsize(b.size() - 33));  // from, id, blob
            writeFrame(c, {17});
        } else if (type == 11 && b.size() == 33) {  // AUTH
            std::memcpy(key, b.data() + 1, 32);
            haveKey = true;
            proved = false;
            oc::randomBytes(nonce, 32);
            std::vector<u8> ch = {12};
            ch.insert(ch.end(), nonce, nonce + 32);
            writeFrame(c, ch);
        } else if (type == 13 && b.size() == 65 && haveKey) {  // PROVE
            std::vector<u8> msg = {'c', 'f', '-', 'r', 'e', 'l', 'a', 'y'};
            msg.insert(msg.end(), nonce, nonce + 32);
            proved = oc::ed25519Verify(key, msg.data(), msg.size(), b.data() + 1);
            if (proved) writeFrame(c, {17});
            else err(c, "bad signature");
        } else if (type == 14 && proved) {  // LIST
            std::lock_guard<std::mutex> g(sMailMutex);
            std::vector<u8> out = {15, 0, 0};
            u32 n = 0;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(sMailDir / hex(key, 32), ec)) {
                std::ifstream f(e.path(), std::ios::binary);
                const std::vector<u8> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (d.size() < 40) continue;
                out.insert(out.end(), d.begin(), d.begin() + 40);  // from, id
                const u32 sz = u32(d.size() - 40);
                for (int i = 3; i >= 0; i--) out.push_back(u8(sz >> (8 * i)));
                out.insert(out.end(), d.begin() + 40, d.end());
                if (++n >= 16) break;
            }
            out[1] = u8(n >> 8);
            out[2] = u8(n);
            writeFrame(c, out);
        } else if (type == 16 && proved && b.size() == 9) {  // DEL
            std::lock_guard<std::mutex> g(sMailMutex);
            std::error_code ec;
            fs::remove(sMailDir / hex(key, 32) / (hex(b.data() + 1, 8) + ".ltr"), ec);
            writeFrame(c, {17});
        } else {
            err(c, "bad request");
        }
    }
    closeSocket(c);
}

void sweepMail() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(1));
        std::lock_guard<std::mutex> g(sMailMutex);
        std::error_code ec;
        const auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
        for (auto& d : fs::directory_iterator(sMailDir, ec))
            for (auto& e : fs::directory_iterator(d.path(), ec))
                if (fs::last_write_time(e.path(), ec) < limit) fs::remove(e.path(), ec);
    }
}

}  // namespace

#ifdef CF_RELAY_AS_LIB
int relayMain(int argc, char** argv) {  // the loopback test runs it in a thread (tools/tests)
#else
int main(int argc, char** argv) {
#endif
    int port = 29912;
    sMailDir = fs::path("cf_relay_mail");
    for (int i = 1; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--mail-dir")) sMailDir = argv[++i];
        else if (!std::strcmp(argv[i], "--max-rooms")) sMaxRooms = size_t(std::atoi(argv[++i]));
    }
    if (!startup()) return 1;
    std::error_code ec;
    fs::create_directories(sMailDir, ec);
    // UDP: one dual-stack socket.
    Socket u = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    int fam = AF_INET6;
    if (u != kInvalid) {
        int off = 0;
        setsockopt(u, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(u16(port));
        if (bind(u, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            closeSocket(u);
            u = kInvalid;
        }
    }
    if (u == kInvalid) {
        u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        fam = AF_INET;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(u16(port));
        if (bind(u, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            std::fprintf(stderr, "cf_relay: UDP port %d is in use\n", port);
            return 1;
        }
    }
    Socket t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    {
        int one = 1;
        setsockopt(t, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(u16(port));
        if (bind(t, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(t, 16) != 0) {
            std::fprintf(stderr, "cf_relay: TCP port %d is in use\n", port);
            return 1;
        }
    }
    std::printf("cf_relay: rooms on UDP %d, letters on TCP %d (%s)\n", port, port, sMailDir.string().c_str());
    std::fflush(stdout);
    std::thread(udpLoop, u, fam).detach();
    std::thread(sweepMail).detach();
    while (true) {
        Socket c = accept(t, nullptr, nullptr);
        if (c == kInvalid) continue;
        std::thread(client, c).detach();
    }
}
