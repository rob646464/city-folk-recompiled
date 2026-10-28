#pragma once
// Online play transport (docs/ONLINE.md "Rooms and transport"): serverless rooms. The hosting
// player's game is the room host; members connect straight to it over UDP (room code = the
// host's reachable endpoints + a random room secret; LAN beacons; direct IP:port). Every packet
// after the handshake is encrypted and authenticated with a key only room members can derive
// (room secret + optional password, PBKDF2; online_crypto.h). The host relays packets between
// members (star), so members only ever talk to the host. Reliable ordered messages (with
// fragmentation) and unreliable ones; keepalive, timeouts, kick, "ask me before someone joins",
// failed-password rate limits.
//
// Game-agnostic: the DWC layer (online_dwc.cpp) sends its own application messages over it.
// Thread-safe; the I/O runs on its own host thread.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace online::net {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr int kMaxMembers = 4;        // City Folk: the town's owner + 3 visitors
constexpr u16 kDefaultPort = 29910;   // host UDP port (F1 > Online > Host)
constexpr u16 kLanPort = 29911;       // LAN beacons (broadcast)
constexpr u8 kBroadcast = 0xFF;       // sendApp: every other member
constexpr size_t kMaxAppMessage = 256 * 1024;
constexpr size_t kMaxUnreliable = 1000;

// Who this game is (sent in the handshake; the host refuses other games / port protocol versions).
struct Identity {
    std::string name;         // player name shown to the others (F1 > Online)
    std::string gameId;       // "RUUE01" ...: towns visit only within one region (docs/ONLINE.md)
    std::string portVersion;  // shown in mismatch messages
    u32 pid = 0;              // DWC profile id of this install (online_dwc.cpp), 0 = not yet known
    u64 friendKey = 0;        // the game's 12-digit friend code for pid
};
struct HostOptions {
    u16 port = kDefaultPort;
    std::string roomName;
    std::string password;      // empty = none
    bool askBeforeJoin = false;
    bool allowDirect = true;   // accept joiners that know only IP:port (no room code)
    bool lanVisible = true;    // LAN beacons
    bool portMapping = true;   // UPnP-IGD / NAT-PMP mapping + STUN public address
};

enum class State { Off, Hosting, Connecting, WaitingForHost, Joined, Failed };

struct Member {
    u8 id = 0;             // 0 = host
    std::string name;
    u32 pid = 0;
    u64 friendKey = 0;
    bool host = false;
    bool self = false;
    int rttMs = -1;
    std::string address;   // host's view only
};

struct JoinRequest {
    u32 ticket = 0;
    std::string name;
    std::string address;
};

struct Snapshot {
    State state = State::Off;
    std::string message;       // last status / error text for the UI
    std::string code;          // room code (host)
    std::string roomName;
    bool hasPassword = false;
    u8 selfId = 0;
    std::vector<Member> members;
    std::vector<JoinRequest> pending;  // host with "ask before join"
    // Host reachability (port mapping / STUN), human readable.
    std::string lanEndpoint, publicEndpoint, reachability;
};

// LAN room list entry (beacons heard in the last 6 s) while scanning.
struct LanRoom {
    std::string roomName, hostName, gameId, portVersion, address;  // address = ip:port
    bool hasPassword = false;
    int members = 0, maxMembers = kMaxMembers;
    bool compatible = false;
    u8 secret[10] = {};
};

// Application messages between members (routed through the host). Reliable ones arrive once,
// in order per sender; unreliable ones may be lost (<= kMaxUnreliable bytes).
struct AppMsg {
    u8 from = 0;
    u8 type = 0;
    std::vector<u8> data;
};

struct MemberEvent {
    bool joined = false;
    u8 id = 0;
};

// One room endpoint (this game's: session(); tests make several).
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void setIdentity(const Identity& id);
    bool host(const HostOptions& o, std::string* err);
    bool joinCode(const std::string& code, const std::string& password, std::string* err);
    bool joinAddress(const std::string& hostPort, const std::string& password, std::string* err);
    bool joinLan(const LanRoom& room, const std::string& password, std::string* err);
    void leave();
    void kick(u8 memberId);
    void answerJoinRequest(u32 ticket, bool accept);
    Snapshot snapshot();
    bool active();  // hosting or joined (the game may go online)
    u8 selfId();
    std::vector<Member> members();
    void lanScan(bool on);
    std::vector<LanRoom> lanRooms();
    bool sendApp(u8 to, u8 type, const void* data, size_t len, bool reliable);
    // The oldest message with lo <= type <= hi (DWC uses 0x01-0x3F, friends 0x40-0x5F).
    bool pollApp(AppMsg& out, u8 lo = 0, u8 hi = 0xFF);
    size_t queuedBytes(u8 to);  // reliable bytes not yet acknowledged toward `to` (flow control)
    bool pollMemberEvent(MemberEvent& out);

    struct Impl;

private:
    Impl* d_;
};

Session& session();

// Room codes: [version/flags][secret 10][endpoints...] in Crockford base32, dash-grouped.
struct Endpoint {
    u8 family = 4;   // 4 or 6
    u8 addr[16] = {};
    u16 port = 0;
};
std::string encodeCode(const u8 secret[10], const std::vector<Endpoint>& eps);
bool decodeCode(const std::string& code, u8 secret[10], std::vector<Endpoint>& eps);
std::string endpointText(const Endpoint& e);

// Tests (tools/tests/online_net_test.cpp): loopback endpoints only in codes, faster timers.
void setTestMode(bool on);

}  // namespace online::net
