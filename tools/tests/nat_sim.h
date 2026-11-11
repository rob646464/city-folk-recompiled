#pragma once
// NAT simulator for the online tests (tools/tests/online_lobby_test.cpp): one object is one home
// router on loopback. Session::setTestGateway("127.0.0.1:<port>") makes a session send every UDP
// datagram to the router's inside socket with a 6-byte header [destination IPv4][port, big endian]
// and take only datagrams from it, which carry [source IPv4][port] the same way. The router has a
// public IP of its own on loopback (127.x.y.z: Windows and Linux route all of 127/8 to loopback)
// and opens one external socket per mapping on it:
//   full cone         one mapping per inside address, anything may come in
//   port restricted   one mapping per inside address, inbound only from an ip:port the client
//                     already sent to through that mapping
//   symmetric         a new mapping (a new external port) per destination ip:port, inbound only
//                     from that destination; ports sequential (previous + 1) or random
// Mappings never expire. Header only; its I/O runs on a thread of its own.
#include "recomp/online_sock.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#endif

namespace natsim {

enum class Type { FullCone, PortRestricted, Symmetric };
enum class Ports { Sequential, Random };

class Router {
public:
    // publicIp in host byte order (e.g. 0x7F010203 = 127.1.2.3); firstPort: where the port numbers
    // of the mappings start (sequential: counted up from it; random: ignored).
    Router(Type type, Ports ports, uint32_t publicIp, uint16_t firstPort, std::string label)
        : type_(type), ports_(ports), publicIp_(publicIp), nextPort_(firstPort), label_(std::move(label)), rng_(std::random_device{}()) {}
    ~Router() { stop(); }
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    bool start(std::string* err) {
        using namespace online::sock;
        startup();
        inside_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const Addr want = makeV4(0x7F000001u, 0);
        if (inside_ == kInvalid || bind(inside_, reinterpret_cast<const sockaddr*>(&want.ss), want.len) != 0) {
            if (err) *err = "NAT simulator: no inside socket";
            return false;
        }
        prepare(inside_);
        sockaddr_in b{};
        socklen_t bl = sizeof b;
        getsockname(inside_, reinterpret_cast<sockaddr*>(&b), &bl);
        insidePort_ = ntohs(b.sin_port);
        // The public IP must be usable (a platform without 127/8 on loopback fails here).
        Socket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const Addr pub = makeV4(publicIp_, 0);
        const bool ok = probe != kInvalid && bind(probe, reinterpret_cast<const sockaddr*>(&pub.ss), pub.len) == 0;
        if (probe != kInvalid) closeSocket(probe);
        if (!ok) {
            if (err) *err = "NAT simulator: cannot bind " + makeV4(publicIp_, 0).ipText();
            return false;
        }
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        quit_ = true;
        if (th_.joinable()) th_.join();
        using namespace online::sock;
        if (inside_ != kInvalid) closeSocket(inside_);
        inside_ = kInvalid;
        for (auto& m : maps_) closeSocket(m->ext);
        maps_.clear();
    }

    std::string gateway() const { return "127.0.0.1:" + std::to_string(insidePort_); }
    std::string publicIpText() const { return online::sock::makeV4(publicIp_, 0).ipText(); }
    uint32_t publicIp() const { return publicIp_; }
    const std::string& label() const { return label_; }
    Type type() const { return type_; }

    uint64_t sentOut() const { return out_; }        // inside -> internet
    uint64_t passedIn() const { return in_; }        // internet -> inside
    uint64_t droppedIn() const { return droppedIn_; }  // refused by the filter
    int mappings() const { return mapCount_; }
    // The router "restarts": every mapping is forgotten, the next packet out gets a new port.
    void restart() { forget_ = true; }
    // The cable is pulled: nothing passes in either direction until it is plugged in again.
    void unplug(bool on) { unplugged_ = on; }
    // "full cone 127.1.2.3: 1 mapping, out 12, in 9, refused 3"
    std::string stats() const {
        char b[160];
        std::snprintf(b, sizeof b, "%s %s: %d mapping%s, out %llu, in %llu, refused %llu", label_.c_str(), publicIpText().c_str(), mappings(),
                      mappings() == 1 ? "" : "s", (unsigned long long)sentOut(), (unsigned long long)passedIn(), (unsigned long long)droppedIn());
        return b;
    }
    // The external ports in the order they were opened.
    std::vector<uint16_t> ports() {
        std::lock_guard<std::mutex> g(portsMutex_);
        return portLog_;
    }

private:
    using Socket = online::sock::Socket;
    using Addr = online::sock::Addr;
    using Key = std::pair<uint32_t, uint16_t>;  // ip (host order), port

    struct Map {
        Socket ext = online::sock::kInvalid;
        uint16_t port = 0;
        Addr client;        // the inside address it belongs to
        Key dest{0, 0};     // symmetric: the one destination
        std::set<Key> sentTo;
    };

    static void prepare(Socket s) {
        online::sock::setNonBlocking(s);
#if defined(_WIN32)
        // An ICMP port-unreachable must not make recvfrom fail on this socket (WSAECONNRESET).
        BOOL no = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &no, sizeof no, nullptr, 0, &ret, nullptr, nullptr);
#endif
        int big = 1 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof big);
    }

    Socket openExternal(uint16_t* portOut) {
        using namespace online::sock;
        for (int tries = 0; tries < 400; tries++) {
            uint16_t p;
            if (ports_ == Ports::Sequential) {
                p = nextPort_;
                nextPort_ = uint16_t(nextPort_ >= 60000 ? 20000 : nextPort_ + 1);
            } else {
                // Never near the last one: nothing about the next port can be guessed.
                do {
                    p = uint16_t(20000 + rng_() % 28000);
                } while (lastPort_ && (p > lastPort_ ? p - lastPort_ : lastPort_ - p) < 200);
            }
            const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == kInvalid) return kInvalid;
            const Addr a = makeV4(publicIp_, p);
            if (bind(s, reinterpret_cast<const sockaddr*>(&a.ss), a.len) != 0) {
                closeSocket(s);
                continue;  // taken: the next one
            }
            prepare(s);
            lastPort_ = p;
            *portOut = p;
            return s;
        }
        return kInvalid;
    }

    Map* mapFor(const Addr& client, const Key& dest) {
        for (auto& m : maps_)
            if (m->client == client && (type_ != Type::Symmetric || m->dest == dest)) return m.get();
        auto m = std::make_unique<Map>();
        m->ext = openExternal(&m->port);
        if (m->ext == online::sock::kInvalid) return nullptr;
        m->client = client;
        m->dest = dest;
        {
            std::lock_guard<std::mutex> g(portsMutex_);
            portLog_.push_back(m->port);
        }
        maps_.push_back(std::move(m));
        mapCount_ = int(maps_.size());
        return maps_.back().get();
    }

    void fromInside(const Addr& client, const uint8_t* p, size_t n) {
        if (n < 6 || unplugged_) return;
        const Key dest{uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3], uint16_t(p[4] << 8 | p[5])};
        if (!dest.first || !dest.second) return;
        Map* m = mapFor(client, dest);
        if (!m) return;
        m->sentTo.insert(dest);
        const Addr to = online::sock::makeV4(dest.first, dest.second);
        sendto(m->ext, reinterpret_cast<const char*>(p + 6), int(n - 6), 0, reinterpret_cast<const sockaddr*>(&to.ss), to.len);
        out_++;
    }

    void fromOutside(Map& m, const Addr& from, std::vector<uint8_t>& buf, size_t n) {
        const Key src{from.v4(), from.port()};
        bool pass = true;
        if (type_ == Type::PortRestricted) pass = m.sentTo.count(src) != 0;
        if (type_ == Type::Symmetric) pass = src == m.dest;
        if (unplugged_) return;
        if (!pass) {
            droppedIn_++;
            return;
        }
        uint8_t* h = buf.data();  // 6 bytes of room in front of the payload
        h[0] = uint8_t(src.first >> 24);
        h[1] = uint8_t(src.first >> 16);
        h[2] = uint8_t(src.first >> 8);
        h[3] = uint8_t(src.first);
        h[4] = uint8_t(src.second >> 8);
        h[5] = uint8_t(src.second);
        sendto(inside_, reinterpret_cast<const char*>(h), int(n + 6), 0, reinterpret_cast<const sockaddr*>(&m.client.ss), m.client.len);
        in_++;
    }

    void run() {
        using namespace online::sock;
        std::vector<uint8_t> buf(6 + 65536);
#if defined(_WIN32)
        std::vector<WSAPOLLFD> fds;
#else
        std::vector<pollfd> fds;
#endif
        while (!quit_) {
            if (forget_.exchange(false)) {
                for (auto& m : maps_) closeSocket(m->ext);
                maps_.clear();
                mapCount_ = 0;
            }
            fds.resize(maps_.size() + 1);
            fds[0].fd = inside_;
            fds[0].events = POLLRDNORM;
            fds[0].revents = 0;
            for (size_t i = 0; i < maps_.size(); i++) {
                fds[i + 1].fd = maps_[i]->ext;
                fds[i + 1].events = POLLRDNORM;
                fds[i + 1].revents = 0;
            }
#if defined(_WIN32)
            const int k = WSAPoll(fds.data(), ULONG(fds.size()), 10);
#else
            const int k = poll(fds.data(), nfds_t(fds.size()), 10);
#endif
            if (k <= 0) continue;
            const size_t known = maps_.size();  // fromInside may add mappings: not in fds yet
            for (size_t i = 0; i < known; i++) {
                if (!fds[i + 1].revents) continue;
                for (int burst = 0; burst < 64; burst++) {
                    sockaddr_storage from{};
                    socklen_t fl = sizeof from;
                    const int n = int(recvfrom(maps_[i]->ext, reinterpret_cast<char*>(buf.data() + 6), 65536, 0, reinterpret_cast<sockaddr*>(&from), &fl));
                    if (n < 0) break;
                    Addr a;
                    std::memcpy(&a.ss, &from, fl);
                    a.len = fl;
                    if (a.family() == AF_INET) fromOutside(*maps_[i], a, buf, size_t(n));
                }
            }
            if (fds[0].revents) {
                for (int burst = 0; burst < 256; burst++) {
                    sockaddr_storage from{};
                    socklen_t fl = sizeof from;
                    const int n = int(recvfrom(inside_, reinterpret_cast<char*>(buf.data()), 65536, 0, reinterpret_cast<sockaddr*>(&from), &fl));
                    if (n < 0) break;
                    Addr a;
                    std::memcpy(&a.ss, &from, fl);
                    a.len = fl;
                    fromInside(a, buf.data(), size_t(n));
                }
            }
        }
    }

    const Type type_;
    const Ports ports_;
    const uint32_t publicIp_;
    uint16_t nextPort_, lastPort_ = 0;
    const std::string label_;
    std::mt19937 rng_;
    Socket inside_ = online::sock::kInvalid;
    uint16_t insidePort_ = 0;
    std::vector<std::unique_ptr<Map>> maps_;  // the I/O thread's
    std::thread th_;
    std::atomic<bool> quit_{false}, forget_{false}, unplugged_{false};
    std::atomic<uint64_t> out_{0}, in_{0}, droppedIn_{0};
    std::atomic<int> mapCount_{0};
    std::mutex portsMutex_;
    std::vector<uint16_t> portLog_;
};

}  // namespace natsim
