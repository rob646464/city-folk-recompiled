// Online room transport test (src/recomp/online_net.*): one host and several members in this
// process over loopback UDP. No game, no internet. Run: build/windows-clang/cf_online_test.exe
#include "recomp/online_crypto.h"
#include "recomp/online_ed25519.h"
#include "recomp/online_net.h"
#include "recomp/online_sock.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace online::net;
int relayMain(int argc, char** argv);  // tools/relay/cf_relay.cpp (CF_RELAY_AS_LIB)

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

static bool waitFor(const std::function<bool()>& f, double seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(seconds)) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

static Identity ident(const char* name, u32 pid, const char* game = "RUUE01") {
    Identity i;
    i.name = name;
    i.gameId = game;
    i.portVersion = "test";
    i.pid = pid;
    i.friendKey = pid;
    return i;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string why;
    CHECK(onlinecrypto::selfTest(&why), "%s", why.c_str());
    CHECK(onlinecrypto::edSelfTest(&why), "%s", why.c_str());
    setTestMode(true);

    // Room codes round trip.
    {
        u8 sec[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        Endpoint a;
        a.family = 4;
        a.addr[0] = 203; a.addr[1] = 0; a.addr[2] = 113; a.addr[3] = 7;
        a.port = 29910;
        Endpoint b;
        b.family = 6;
        for (int i = 0; i < 16; i++) b.addr[i] = u8(0x20 + i);
        b.port = 1234;
        const std::string code = encodeCode(sec, {a, b});
        u8 sec2[10];
        std::vector<Endpoint> eps;
        CHECK(decodeCode(code, sec2, eps), "decode %s", code.c_str());
        CHECK(eps.size() == 2 && std::memcmp(sec, sec2, 10) == 0 && eps[0].port == 29910 && eps[1].port == 1234 &&
                  std::memcmp(eps[1].addr, b.addr, 16) == 0,
              "round trip");
        std::string lower = code;
        for (auto& c : lower) c = char(std::tolower(c));
        CHECK(decodeCode(lower, sec2, eps), "lower case code");
        std::string typo = code;
        typo[3] = typo[3] == 'A' ? 'B' : 'A';
        CHECK(!decodeCode(typo, sec2, eps), "typo detected");
        std::printf("code example (IPv4 + IPv6): %s\n", code.c_str());
        std::printf("code example (IPv4 only):   %s\n", encodeCode(sec, {a}).c_str());
    }

    Session host, m1, m2, m3;
    host.setIdentity(ident("Host", 1001));
    m1.setIdentity(ident("Alice", 2002));
    m2.setIdentity(ident("Bob", 3003));
    m3.setIdentity(ident("Eve", 4004));

    HostOptions ho;
    ho.port = 39910;
    ho.roomName = "Test town";
    ho.password = "bells123";
    ho.portMapping = false;
    std::string err;
    CHECK(host.host(ho, &err), "%s", err.c_str());
    const std::string code = host.snapshot().code;
    std::printf("room code: %s\n", code.c_str());

    // No password -> asked for one; wrong password -> refused; right one -> joined.
    CHECK(m1.joinCode(code, "", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return m1.snapshot().state == State::Failed; }, 3), "no-password should fail");
    std::printf("no password: %s\n", m1.snapshot().message.c_str());
    CHECK(m1.joinCode(code, "wrong", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return m1.snapshot().state == State::Failed; }, 3), "wrong password should fail");
    std::printf("wrong password: %s\n", m1.snapshot().message.c_str());
    CHECK(m1.joinCode(code, "bells123", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return m1.snapshot().state == State::Joined; }, 3), "join: %s", m1.snapshot().message.c_str());

    // Direct connect (IP:port, no code) with the password.
    CHECK(m2.joinAddress("127.0.0.1:39910", "bells123", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return m2.snapshot().state == State::Joined; }, 3), "direct: %s", m2.snapshot().message.c_str());
    CHECK(waitFor([&] { return m1.members().size() == 3 && m2.members().size() == 3; }, 3), "member lists");
    for (auto& m : m2.members()) std::printf("  member %u %s pid %u%s%s\n", m.id, m.name.c_str(), m.pid, m.host ? " (host)" : "", m.self ? " (you)" : "");

    // Wrong game region refused.
    Session jp;
    jp.setIdentity(ident("Taro", 5005, "RUUJ01"));
    CHECK(jp.joinCode(code, "bells123", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return jp.snapshot().state == State::Failed; }, 3), "region mismatch");
    std::printf("other region: %s\n", jp.snapshot().message.c_str());

    // Messages: member -> member (relayed), big reliable (fragmented), broadcast, unreliable.
    MemberEvent ev;
    while (m1.pollMemberEvent(ev)) {}
    const u8 id1 = m1.selfId(), id2 = m2.selfId();
    std::vector<u8> big(200000);
    for (size_t i = 0; i < big.size(); i++) big[i] = u8(i * 7 + (i >> 9));
    CHECK(m1.sendApp(id2, 7, big.data(), big.size(), true), "send big");
    const char hello[] = "hello host";
    CHECK(m2.sendApp(0, 8, hello, sizeof hello, true), "send to host");
    CHECK(host.sendApp(kBroadcast, 9, "bc", 2, true), "broadcast");
    for (int i = 0; i < 20; i++) m1.sendApp(id2, 10, &i, sizeof i, false);
    bool gotBig = false, gotHello = false, gotBc1 = false, gotBc2 = false;
    int unrel = 0;
    waitFor([&] {
        AppMsg msg;
        while (m2.pollApp(msg)) {
            if (msg.type == 7) gotBig = msg.from == id1 && msg.data == big;
            if (msg.type == 9) gotBc2 = msg.from == 0;
            if (msg.type == 10) unrel++;
        }
        while (host.pollApp(msg))
            if (msg.type == 8) gotHello = msg.from == id2 && std::string(reinterpret_cast<char*>(msg.data.data())) == hello;
        while (m1.pollApp(msg))
            if (msg.type == 9) gotBc1 = true;
        return gotBig && gotHello && gotBc1 && gotBc2 && unrel >= 15;
    }, 10);
    CHECK(gotBig, "200 KB reliable relayed member->member");
    CHECK(gotHello, "member->host");
    CHECK(gotBc1 && gotBc2, "host broadcast");
    CHECK(unrel >= 15, "unreliable on loopback: %d of 20", unrel);

    // Room full (host + 3).
    CHECK(m3.joinCode(code, "bells123", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return m3.snapshot().state == State::Joined; }, 3), "third member: %s", m3.snapshot().message.c_str());
    Session m4;
    m4.setIdentity(ident("Late", 6006));
    CHECK(m4.joinCode(code, "bells123", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return m4.snapshot().state == State::Failed; }, 3), "full room");
    std::printf("full: %s\n", m4.snapshot().message.c_str());

    // Kick.
    host.kick(m3.selfId());
    CHECK(waitFor([&] { return m3.snapshot().state == State::Failed; }, 3), "kicked");
    std::printf("kicked: %s\n", m3.snapshot().message.c_str());
    CHECK(waitFor([&] { return m1.members().size() == 3; }, 3), "kicked member gone from lists");

    // Leave -> others see it.
    m2.leave();
    CHECK(waitFor([&] { return host.members().size() == 2 && m1.members().size() == 2; }, 3), "leave");

    // Ask before join.
    host.leave();
    CHECK(waitFor([&] { return m1.snapshot().state == State::Failed; }, 3), "room closed");
    std::printf("closed: %s\n", m1.snapshot().message.c_str());
    ho.password.clear();
    ho.askBeforeJoin = true;
    CHECK(host.host(ho, &err), "%s", err.c_str());
    const std::string code2 = host.snapshot().code;
    CHECK(m1.joinCode(code2, "", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return m1.snapshot().state == State::WaitingForHost; }, 3), "waiting");
    CHECK(waitFor([&] { return host.snapshot().pending.size() == 1; }, 3), "pending request");
    if (!host.snapshot().pending.empty()) host.answerJoinRequest(host.snapshot().pending[0].ticket, true);
    CHECK(waitFor([&] { return m1.snapshot().state == State::Joined; }, 3), "accepted");
    CHECK(m2.joinCode(code2, "", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return host.snapshot().pending.size() == 1; }, 3), "pending request 2");
    if (!host.snapshot().pending.empty()) host.answerJoinRequest(host.snapshot().pending[0].ticket, false);
    CHECK(waitFor([&] { return m2.snapshot().state == State::Failed; }, 3), "declined");
    std::printf("declined: %s\n", m2.snapshot().message.c_str());

    // Old code after the room was re-hosted.
    CHECK(m3.joinCode(code, "", &err), "%s", err.c_str());
    CHECK(waitFor([&] { return m3.snapshot().state == State::Failed; }, 4), "old code");
    std::printf("old code: %s\n", m3.snapshot().message.c_str());

    // LAN list.
    Session scanner;
    scanner.setIdentity(ident("Scanner", 7007));
    scanner.lanScan(true);
    CHECK(waitFor([&] { return !scanner.lanRooms().empty(); }, 5), "LAN beacon heard");
    for (auto& r : scanner.lanRooms())
        std::printf("LAN room: \"%s\" by %s at %s, %d/%d%s\n", r.roomName.c_str(), r.hostName.c_str(), r.address.c_str(),
                    r.members, r.maxMembers, r.hasPassword ? " (password)" : "");

    // Through the relay (tools/relay/cf_relay.cpp, run in a thread): a code whose only route is the
    // relay; the joiner reaches the host and a big message crosses.
    {
        static char a0[] = "cf_relay", a1[] = "--port", a2[] = "39912";
        static char* av[] = {a0, a1, a2, nullptr};
        std::thread([] { relayMain(3, av); }).detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        Session rh, rj;
        rh.setIdentity(ident("RelayHost", 8008));
        rj.setIdentity(ident("RelayJoiner", 9009));
        HostOptions o;
        o.port = 39930;
        o.portMapping = false;
        o.relay = "127.0.0.1:39912";
        CHECK(rh.host(o, &err), "%s", err.c_str());
        u8 sec[10];
        std::vector<Endpoint> eps, only;
        CHECK(decodeCode(rh.snapshot().code, sec, eps), "relay code decodes");
        for (auto& e : eps)
            if (e.family == kRelayFamily) only.push_back(e);
        CHECK(only.size() == 1, "relay endpoint in the code (%zu endpoints)", eps.size());
        const std::string rcode = encodeCode(sec, only);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // the host registers
        CHECK(rj.joinCode(rcode, "", &err), "%s", err.c_str());
        CHECK(waitFor([&] { return rj.snapshot().state == State::Joined; }, 6), "join through the relay: %s", rj.snapshot().message.c_str());
        std::vector<u8> msg(50000, 0x5A);
        CHECK(rj.sendApp(0, 0x40, msg.data(), msg.size(), true), "send via relay");
        AppMsg got;
        CHECK(waitFor([&] { return rh.pollApp(got, 0x40, 0x40) && got.data.size() == msg.size(); }, 6), "big message via relay");
        std::printf("relay: joined and a %zu-byte message crossed\n", msg.size());
    }

    // Hole punching: the host sends packets to a joiner's reply code (its public address).
    {
        Session ph;
        ph.setIdentity(ident("PunchHost", 1111));
        HostOptions o;
        o.port = 39960;
        o.portMapping = false;
        CHECK(ph.host(o, &err), "%s", err.c_str());
        online::sock::Socket u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(39961);
        a.sin_addr.s_addr = htonl(0x7F000001u);
        CHECK(bind(u, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0, "bind");
        online::sock::setNonBlocking(u);
        Endpoint e;
        e.family = 4;
        e.addr[0] = 127; e.addr[3] = 1;
        e.port = 39961;
        const u8 zero[10] = {};
        CHECK(ph.punch(encodeCode(zero, {e}), &err), "%s", err.c_str());
        bool got = waitFor([&] {
            char b[64];
            const int n = int(recv(u, b, sizeof b, 0));
            return n >= 4 && b[0] == 'C' && b[1] == 'F' && u8(b[3]) == 0x23;
        }, 3);
        CHECK(got, "punch packets arrive at the reply code's address");
        std::printf("hole punching: packets to the reply code: %s\n", got ? "yes" : "no");
        online::sock::closeSocket(u);
    }

    std::printf(sFails ? "%d FAILURES\n" : "all online transport checks passed\n", sFails);
    return sFails ? 1 : 0;
}
