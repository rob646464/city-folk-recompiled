// cf_relay: the optional self-hosted relay for City Folk Recompiled online play (docs/ONLINE.md
// "Relay"). Anyone (a friend group) can run it on a machine the players can reach; nothing in the
// port needs it, and it never sees game data in the clear. Rooms only.
//
//   cf_relay [--port 29912] [--max-rooms 256]
//
// UDP (rooms): a host whose router cannot be reached registers its room under a random 8-byte
// room id (part of its room code); joiners reach it through the relay. Datagrams (sealed by the
// room's own encryption, end to end) are only forwarded between a room's host and its joiners.
//   "CFRL" ver(1) type
//   1 REG      room[8]                      host -> relay (every 10 s; answer 2 REG_OK room[8])
//   (answers and forwards carry room[8] after the type as well)
//   3 TO_HOST  room[8] payload              joiner -> relay -> host as 4 FROM_PEER peer(u32) payload
//   5 TO_PEER  room[8] peer(u32) payload    host -> relay -> joiner as 6 FROM_HOST payload
// It stores nothing (user decision: no letters on the relay). Limits: 256 rooms, 16 joiners per
// room, 600 datagrams/s per address.
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

}  // namespace

#ifdef CF_RELAY_AS_LIB
int relayMain(int argc, char** argv) {  // the loopback test runs it in a thread (tools/tests)
#else
int main(int argc, char** argv) {
#endif
    int port = 29912;
    for (int i = 1; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--max-rooms")) sMaxRooms = size_t(std::atoi(argv[++i]));
    }
    if (!startup()) return 1;
    std::error_code ec;
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
    std::printf("cf_relay: rooms on UDP %d\n", port);
    std::fflush(stdout);
    udpLoop(u, fam);
    return 0;
}
