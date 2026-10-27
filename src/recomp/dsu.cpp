// DSU ("cemuhook") controller client: docs/CONTROLS.md "DSU controllers".
//
// Protocol (version 1001, little endian): every packet starts with a 16-byte header
// ("DSUC" from the client / "DSUS" from the server, u16 version, u16 length of what follows
// the header, u32 CRC32 of the whole packet with this field zero, u32 sender id), then a u32
// message type. The client asks for the slots' info (0x100001) and subscribes to pad data
// (0x100002, all slots); a server stops sending after about 5 s without a request, so both
// are repeated every second. Pad data: slot, state (2 = connected), model, connection,
// MAC, battery, connected flag, packet number, the DS4's buttons and analog values, touch
// and motion. Motion and touch are not used (the game has nothing to drive with them).
//
// Each connected (server, slot) is an SDL virtual gamepad with a DualShock 4's ids, named
// "DSU slot <n>" (or "DSU <server> slot <n>"), so controls.cpp gives it a port, a binding
// profile with the PlayStation layout, and the input test. The network thread only stores
// the latest packet per slot; the virtual pads are attached, updated and detached by
// update() under controls.cpp's lock.
#include "recomp/dsu.h"

#include "recomp/menu.h"
#include "port/settings.h"

#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::dsu {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kProtocol = 1001;
constexpr uint32_t kMsgVersion = 0x100000, kMsgPorts = 0x100001, kMsgPadData = 0x100002;
constexpr int kSlots = 4;
constexpr int kMaxServers = 4;
constexpr const char* kDefaultServers = "127.0.0.1:26760";
constexpr double kSlotTimeout = 2.0;   // no pad data for this long: the pad is unplugged
constexpr double kServerTimeout = 3.0;

// ---- Settings ([controls.dsu]) --------------------------------------------------------

// What the server's controllers are: decides how the DS4-shaped DSU buttons become the
// virtual pad's buttons and which default bindings its profile gets.
enum class Layout : int {
    GameCube,     // a GameCube controller behind the server (NSO GC Driver: A/B/X/Y = cross/
                  // circle/square/triangle, Z = R3, L/R = analog L2/R2 with L1/R1 at the click)
    PlayStation,  // a DS4-shaped pad (DS4Windows, BetterJoy, phone apps): buttons by position
};
struct Config {
    bool enabled = false;
    std::string servers = kDefaultServers;  // "host:port" entries separated by ';' or ','
    Layout layout = Layout::GameCube;
    bool operator==(const Config&) const = default;
};
std::mutex sCfgM;
Config sCfg;
std::atomic<unsigned> sCfgGen{1};

void resetConfig() {
    std::lock_guard lk(sCfgM);
    sCfg = Config{};
    sCfgGen++;
}
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "controls.dsu") return;
    std::lock_guard lk(sCfgM);
    if (key == "enabled") sCfg.enabled = value == "true" || value == "1";
    else if (key == "servers") sCfg.servers = value;
    else if (key == "layout") sCfg.layout = value == "playstation" ? Layout::PlayStation : Layout::GameCube;
    sCfgGen++;
}
void saveConfig(std::ostream& out) {
    std::lock_guard lk(sCfgM);
    out << "[controls.dsu]\n"
        << "enabled=" << (sCfg.enabled ? "true" : "false") << '\n'
        << "servers=" << sCfg.servers << '\n'
        << "layout=" << (sCfg.layout == Layout::PlayStation ? "playstation" : "gamecube") << '\n';
}
const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

std::vector<std::string> splitServers(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= s.size(); i++) {
        const char c = i < s.size() ? s[i] : ';';
        if (c == ';' || c == ',') {
            const auto b = cur.find_first_not_of(" \t"), e = cur.find_last_not_of(" \t");
            if (b != std::string::npos && out.size() < size_t(kMaxServers)) out.push_back(cur.substr(b, e - b + 1));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// ---- Packets ---------------------------------------------------------------------------

uint32_t crc32(const uint8_t* p, size_t n) {
    static const auto kTable = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, uint16_t(v)), put16(p + 2, uint16_t(v >> 16)); }
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) | uint32_t(get16(p + 2)) << 16; }

// A client packet: header + message type + payload.
std::vector<uint8_t> makePacket(uint32_t clientId, uint32_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> p(20 + payload.size());
    std::memcpy(p.data(), "DSUC", 4);
    put16(&p[4], kProtocol);
    put16(&p[6], uint16_t(p.size() - 16));
    put32(&p[12], clientId);
    put32(&p[16], type);
    std::copy(payload.begin(), payload.end(), p.begin() + 20);
    put32(&p[8], crc32(p.data(), p.size()));
    return p;
}

// Latest pad data of one slot (offsets into the 100-byte pad data packet).
struct PadData {
    bool valid = false;
    Clock::time_point when;
    uint8_t model = 0, connection = 0, battery = 0;
    uint8_t buttons1 = 0, buttons2 = 0, home = 0, touch = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
    unsigned packetsThisSecond = 0, packetsLastSecond = 0;
    Clock::time_point secondStart;
};

struct Server {
    std::string address, host, port;
    std::string error;
    SocketHandle sock = kNoSocket;
    sockaddr_storage addr{};
    socklen_t addrLen = 0;
    Clock::time_point lastValid{}, lastRequest{};
    uint32_t serverId = 0;
    uint16_t version = 0;
    unsigned droppedSenders = 0, rejected = 0;  // datagrams from other addresses / failing the checks
    std::array<PadData, kSlots> pads{};
};

std::mutex sM;  // sServers' status and pad data (network thread vs update()/drawTab())
std::vector<Server> sServers;
std::atomic<bool> sThreadStarted{false};

void closeSocket(SocketHandle& s) {
    if (s == kNoSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    s = kNoSocket;
}

// CF_DSU_LOG=1 (test aid, docs/CONTROLS.md "DSU controllers"): rejected datagrams and every
// change of a slot's buttons, plus each slot's packet rate once a second.
const bool sLog = [] {
    const char* v = std::getenv("CF_DSU_LOG");
    return v && *v && *v != '0';
}();

std::string addressText(const sockaddr_storage& a) {
    char host[64] = "?";
    unsigned port = 0;
    if (a.ss_family == AF_INET) {
        const auto& x = reinterpret_cast<const sockaddr_in&>(a);
        inet_ntop(AF_INET, &x.sin_addr, host, sizeof(host));
        port = ntohs(x.sin_port);
    } else if (a.ss_family == AF_INET6) {
        const auto& x = reinterpret_cast<const sockaddr_in6&>(a);
        inet_ntop(AF_INET6, &x.sin6_addr, host, sizeof(host));
        port = ntohs(x.sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

// A datagram from an address other than the server's (sM held).
void dropped(Server& s, const sockaddr_storage& from) {
    s.droppedSenders++;
    if (sLog) log("[dsu] %s: dropped a datagram from %s (not the server)", s.address.c_str(), addressText(from).c_str());
}

// Same address and port (IPv4 / IPv6).
bool sameAddress(const sockaddr_storage& a, const sockaddr_storage& b) {
    if (a.ss_family != b.ss_family) return false;
    if (a.ss_family == AF_INET) {
        const auto& x = reinterpret_cast<const sockaddr_in&>(a);
        const auto& y = reinterpret_cast<const sockaddr_in&>(b);
        return x.sin_port == y.sin_port && std::memcmp(&x.sin_addr, &y.sin_addr, sizeof(x.sin_addr)) == 0;
    }
    if (a.ss_family == AF_INET6) {
        const auto& x = reinterpret_cast<const sockaddr_in6&>(a);
        const auto& y = reinterpret_cast<const sockaddr_in6&>(b);
        return x.sin6_port == y.sin6_port && std::memcmp(&x.sin6_addr, &y.sin6_addr, sizeof(x.sin6_addr)) == 0;
    }
    return false;
}

// Resolves "host[:port]" and opens a UDP socket connected to it. connect() makes the system
// drop datagrams from any other address (anyone on the network could otherwise inject pad
// input by sending DSUS packets to our port), and for a loopback server the socket is bound to
// the loopback interface, so nothing outside this computer can reach it at all.
bool openServer(Server& s) {
    const std::string& a = s.address;
    const auto colon = a.rfind(':');
    s.host = colon == std::string::npos ? a : a.substr(0, colon);
    s.port = colon == std::string::npos ? "26760" : a.substr(colon + 1);
    if (s.host.size() > 2 && s.host.front() == '[' && s.host.back() == ']') s.host = s.host.substr(1, s.host.size() - 2);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (const int rc = getaddrinfo(s.host.c_str(), s.port.c_str(), &hints, &res); rc != 0 || !res) {
        s.error = "cannot resolve " + s.address + ": " + gai_strerror(rc);
        return false;
    }
    std::memcpy(&s.addr, res->ai_addr, res->ai_addrlen);
    s.addrLen = socklen_t(res->ai_addrlen);
    s.sock = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    freeaddrinfo(res);
    if (s.sock != kNoSocket) {
#ifdef _WIN32
        // A connected UDP socket reports an ICMP "port unreachable" (no server running yet) as
        // WSAECONNRESET on the next receive; that is not an error here.
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s.sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof(off), nullptr, 0, &ret,
                 nullptr, nullptr);
#endif
    }
    if (s.sock == kNoSocket) {
        s.error = "cannot open a UDP socket";
        return false;
    }
    if (connect(s.sock, reinterpret_cast<const sockaddr*>(&s.addr), s.addrLen) != 0) {
        s.error = "cannot connect a UDP socket to " + s.address;
        closeSocket(s.sock);
        return false;
    }
    s.error.clear();
    return true;
}

void sendRequests(Server& s, uint32_t clientId) {
    const std::vector<uint8_t> ports = makePacket(clientId, kMsgPorts, {kSlots, 0, 0, 0, 0, 1, 2, 3});
    // Registration flags 0: every slot (slot and MAC are ignored).
    const std::vector<uint8_t> data = makePacket(clientId, kMsgPadData, {0, 0, 0, 0, 0, 0, 0, 0});
    for (const auto* p : {&ports, &data})  // connected socket: send(), not sendto() (EISCONN on macOS)
        send(s.sock, reinterpret_cast<const char*>(p->data()), int(p->size()), 0);
}

// One received datagram from server s (sM held).
void handlePacket(Server& s, const uint8_t* p, size_t n) {
    auto reject = [&](const char* why) {
        s.rejected++;
        if (sLog) log("[dsu] %s: rejected a %zu-byte datagram (%s)", s.address.c_str(), n, why);
    };
    if (n < 20 || std::memcmp(p, "DSUS", 4) != 0) return reject("not DSUS");
    const uint16_t version = get16(p + 4), len = get16(p + 6);
    if (version > kProtocol || size_t(len) + 16 > n) return reject("version or length");
    n = size_t(len) + 16;
    std::vector<uint8_t> copy(p, p + n);
    put32(&copy[8], 0);
    if (crc32(copy.data(), n) != get32(p + 8)) return reject("CRC");
    s.version = version;
    s.serverId = get32(p + 12);
    s.lastValid = Clock::now();
    if (get32(p + 16) != kMsgPadData || n < 56) return;
    const int slot = p[20];
    if (slot < 0 || slot >= kSlots) return;
    PadData& d = s.pads[size_t(slot)];
    const auto now = Clock::now();
    if (p[21] != 2 || !p[31]) {  // state "connected" and the connected flag
        d.valid = false;
        return;
    }
    if (sLog && (!d.valid || d.buttons1 != p[36] || d.buttons2 != p[37] || d.home != p[38]))
        log("[dsu] %s slot %d: buttons %02X %02X home %u, sticks %u,%u %u,%u, L2 %u R2 %u", s.address.c_str(), slot + 1,
            p[36], p[37], p[38], p[40], p[41], p[42], p[43], p[55], p[54]);
    d.valid = true;
    d.when = now;
    d.model = p[22];
    d.connection = p[23];
    d.battery = p[30];
    d.buttons1 = p[36];
    d.buttons2 = p[37];
    d.home = p[38];
    d.touch = p[39];
    d.lx = p[40], d.ly = p[41], d.rx = p[42], d.ry = p[43];
    d.l2 = p[55], d.r2 = p[54];
    if (std::chrono::duration<double>(now - d.secondStart).count() >= 1.0) {
        if (sLog)
            log("[dsu] %s slot %d: %u packets/s (rejected %u, other senders %u)", s.address.c_str(), slot + 1,
                d.packetsThisSecond, s.rejected, s.droppedSenders);
        d.packetsLastSecond = d.packetsThisSecond;
        d.packetsThisSecond = 0;
        d.secondStart = now;
    }
    d.packetsThisSecond++;
}

// Network thread: (re)opens the configured servers, repeats the requests every second and
// stores what comes back. Runs for the whole session once DSU was first enabled.
void netThread() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const uint32_t clientId = std::random_device{}();
    unsigned gen = 0;
    std::vector<uint8_t> buf(2048);
    for (;;) {
        if (const unsigned g = sCfgGen.load(); g != gen) {
            gen = g;
            Config cfg;
            {
                std::lock_guard lk(sCfgM);
                cfg = sCfg;
            }
            // Resolved and opened without sM: getaddrinfo can block for seconds on a host name,
            // and update() takes sM before every PADRead (the game would stall). Only this
            // thread changes sServers, so the new list is swapped in under the lock afterwards.
            std::vector<Server> fresh;
            if (cfg.enabled)
                for (const auto& a : splitServers(cfg.servers)) {
                    Server s;
                    s.address = a;
                    if (openServer(s)) log("[dsu] client for %s (%s port %s)", a.c_str(), s.host.c_str(), s.port.c_str());
                    else log("[dsu] %s", s.error.c_str());
                    fresh.push_back(std::move(s));
                }
            {
                std::lock_guard lk(sM);
                sServers.swap(fresh);
            }
            for (auto& s : fresh) closeSocket(s.sock);  // the old list
            continue;  // the configuration may have changed again meanwhile
        }
        fd_set fds;
        FD_ZERO(&fds);
        SocketHandle maxFd = 0;
        {
            std::lock_guard lk(sM);
            const auto now = Clock::now();
            for (auto& s : sServers) {
                if (s.sock == kNoSocket) continue;
                if (std::chrono::duration<double>(now - s.lastRequest).count() >= 1.0) {
                    sendRequests(s, clientId);
                    s.lastRequest = now;
                }
                FD_SET(s.sock, &fds);
                maxFd = std::max(maxFd, s.sock);
            }
        }
        if (maxFd == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        timeval tv{0, 100000};
        if (select(int(maxFd + 1), &fds, nullptr, nullptr, &tv) <= 0) continue;
        std::lock_guard lk(sM);
        for (auto& s : sServers) {
            if (s.sock == kNoSocket || !FD_ISSET(s.sock, &fds)) continue;
            for (;;) {  // every datagram that is waiting
                sockaddr_storage from{};
                socklen_t fromLen = sizeof(from);
                const auto n = recvfrom(s.sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                        reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n <= 0) break;
                // The connected socket only delivers the server's datagrams; checked again in
                // case a system does not filter (never take input from another sender).
                if (sameAddress(from, s.addr)) handlePacket(s, buf.data(), size_t(n));
                else dropped(s, from);
                fd_set more;
                FD_ZERO(&more);
                FD_SET(s.sock, &more);
                timeval zero{0, 0};
                if (select(int(s.sock + 1), &more, nullptr, nullptr, &zero) <= 0) break;
            }
        }
    }
}

// ---- Virtual gamepads ------------------------------------------------------------------

struct Virtual {
    SDL_JoystickID id = 0;
    SDL_Joystick* joy = nullptr;
    std::string key;  // pad name
    Layout layout = Layout::GameCube;
};
std::vector<Virtual> sVirtual;

std::string padName(int server, int slot, size_t servers) {
    char n[48];
    if (servers > 1) std::snprintf(n, sizeof(n), "DSU %d slot %d", server + 1, slot + 1);
    else std::snprintf(n, sizeof(n), "DSU slot %d", slot + 1);
    return n;
}

// Gives a virtual pad a gamepad mapping of its own GUID. The pads differ only in the GUID's name
// CRC, and SDL looks a mapping up by the exact GUID first, then ignoring the CRC: the second pad
// found the automatic mapping SDL had made for the first one, so SDL (and Aurora's log, the
// Ports tab, ...) called it by the first pad's name ("DSU slot 1" for slot 2). Its own mapping
// (same buttons, its name, exact GUID) is found first. Done before Aurora opens the gamepad
// (it does that when it handles the attach event, after update()).
void ownMapping(SDL_JoystickID id, const std::string& name) {
    char* m = SDL_GetGamepadMappingForID(id);
    if (!m) return;
    const std::string map = m;
    SDL_free(m);
    const size_t c1 = map.find(','), c2 = c1 == std::string::npos ? c1 : map.find(',', c1 + 1);
    if (c2 == std::string::npos) return;
    std::string body = map.substr(c2);  // ",a:b0,..."
    // A crc: field would set the GUID's CRC to the other pad's again.
    if (const size_t crc = body.find(",crc:"); crc != std::string::npos) {
        const size_t end = body.find(',', crc + 1);
        body.erase(crc, end == std::string::npos ? std::string::npos : end - crc);
    }
    char guid[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    std::string safe = name;
    std::replace(safe.begin(), safe.end(), ',', ' ');
    const std::string own = std::string(guid) + "," + safe + body;
    if (own == map) return;  // already its own (the first pad)
    if (SDL_AddGamepadMapping(own.c_str()) < 0) log("[dsu] %s: no own gamepad mapping (%s)", name.c_str(), SDL_GetError());
    else if (sLog) log("[dsu] %s: mapping %s", name.c_str(), own.c_str());
}

Virtual attach(const std::string& name, const std::string& key, Layout layout) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    // The ids decide SDL's gamepad type, so the profile gets that type's defaults (and its own
    // GUID per layout): Nintendo's GameCube adapter (GameCube defaults: labels, Z, L/R clicks on
    // MISC3/MISC4, no extra dead zone), or a DualShock 4 (PlayStation defaults).
    const bool gc = layout == Layout::GameCube;
    desc.vendor_id = gc ? 0x057E : 0x054C;
    desc.product_id = gc ? 0x0337 : 0x09CC;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = name.c_str();
    Virtual v;
    v.key = key;
    v.layout = layout;
    v.id = SDL_AttachVirtualJoystick(&desc);
    if (v.id) ownMapping(v.id, name);
    v.joy = v.id ? SDL_OpenJoystick(v.id) : nullptr;
    if (v.joy)
        for (int t : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
            SDL_SetJoystickVirtualAxis(v.joy, t, SDL_JOYSTICK_AXIS_MIN);  // released
    log("[dsu] %s connected (virtual gamepad %u)%s", name.c_str(), unsigned(v.id), v.joy ? "" : SDL_GetError());
    return v;
}

void detach(Virtual& v) {
    if (v.joy) SDL_CloseJoystick(v.joy);
    if (v.id) SDL_DetachVirtualJoystick(v.id);
    log("[dsu] %s disconnected", v.key.c_str());
    v = Virtual{};
}

// DSU stick byte (128 = centre, larger = right / up) to an SDL axis (negative = left / up).
Sint16 stickAxis(uint8_t v, bool flipY) {
    const int c = (int(v) - 128) * 256;
    return Sint16(std::clamp(flipY ? -c : c, -32768, 32767));
}
// DSU trigger byte (0..255) to a virtual trigger axis (-32768 released .. 32767 pulled).
Sint16 triggerAxis(uint8_t v) { return Sint16(-32768 + int(v) * 257); }

void apply(SDL_Joystick* j, const PadData& d, Layout layout) {
    const uint8_t b1 = d.buttons1, b2 = d.buttons2;
    // The L2/R2 digital bits (0x01/0x02) of a server that sends no analog trigger value: then the
    // bit is the whole press (full pull below, and the GameCube click). A server with analog
    // triggers may set the bit as soon as the trigger moves (DS4 semantics), so there it is ignored.
    const bool l2Digital = (b2 & 0x01) && d.l2 == 0, r2Digital = (b2 & 0x02) && d.r2 == 0;
    if (layout == Layout::GameCube) {
        // SDL's GameCube buttons: A south, B west, X east, Y north, Z right shoulder, L/R clicks
        // MISC3/MISC4. The server sends B as circle, X as square, Z as R3 and the clicks as L1/R1
        // (NSO GC Driver); the L2/R2 bit only counts as the click for a digital-only server, so a
        // half pull on a server that sets that bit with the analog value does not click.
        const std::pair<int, bool> buttons[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, (b2 & 0x20) != 0},          {SDL_GAMEPAD_BUTTON_WEST, (b2 & 0x40) != 0},
            {SDL_GAMEPAD_BUTTON_EAST, (b2 & 0x10) != 0},           {SDL_GAMEPAD_BUTTON_NORTH, (b2 & 0x80) != 0},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, (b1 & 0x04) != 0}, {SDL_GAMEPAD_BUTTON_MISC3, (b2 & 0x04) || l2Digital},
            {SDL_GAMEPAD_BUTTON_MISC4, (b2 & 0x08) || r2Digital},  {SDL_GAMEPAD_BUTTON_START, (b1 & 0x08) != 0},
            {SDL_GAMEPAD_BUTTON_BACK, (b1 & 0x01) != 0},           {SDL_GAMEPAD_BUTTON_LEFT_STICK, (b1 & 0x02) != 0},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, (b1 & 0x10) != 0},        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, (b1 & 0x20) != 0},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, (b1 & 0x40) != 0},      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, (b1 & 0x80) != 0},
            {SDL_GAMEPAD_BUTTON_GUIDE, d.home != 0},
        };
        for (const auto& [b, down] : buttons) SDL_SetJoystickVirtualButton(j, b, down);
    } else {
    const std::pair<int, bool> buttons[] = {
        {SDL_GAMEPAD_BUTTON_BACK, (b1 & 0x01) != 0},          {SDL_GAMEPAD_BUTTON_LEFT_STICK, (b1 & 0x02) != 0},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, (b1 & 0x04) != 0},   {SDL_GAMEPAD_BUTTON_START, (b1 & 0x08) != 0},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, (b1 & 0x10) != 0},       {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, (b1 & 0x20) != 0},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, (b1 & 0x40) != 0},     {SDL_GAMEPAD_BUTTON_DPAD_LEFT, (b1 & 0x80) != 0},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, (b2 & 0x04) != 0}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, (b2 & 0x08) != 0},
        {SDL_GAMEPAD_BUTTON_WEST, (b2 & 0x10) != 0},          {SDL_GAMEPAD_BUTTON_SOUTH, (b2 & 0x20) != 0},
        {SDL_GAMEPAD_BUTTON_EAST, (b2 & 0x40) != 0},          {SDL_GAMEPAD_BUTTON_NORTH, (b2 & 0x80) != 0},
        {SDL_GAMEPAD_BUTTON_GUIDE, d.home != 0},              {SDL_GAMEPAD_BUTTON_TOUCHPAD, d.touch != 0},
    };
    for (const auto& [b, down] : buttons) SDL_SetJoystickVirtualButton(j, b, down);
    }
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, stickAxis(d.lx, false));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, stickAxis(d.ly, true));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTX, stickAxis(d.rx, false));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTY, stickAxis(d.ry, true));
    // A server that only sets the L2/R2 bits still gives a full pull; one with analog values keeps
    // them (before, any set bit forced 255, so such a server's half pulls became full ones).
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, triggerAxis(l2Digital ? 255 : d.l2));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, triggerAxis(r2Digital ? 255 : d.r2));
}

const char* modelName(uint8_t m) {
    switch (m) {
    case 1: return "partial gyro";
    case 2: return "full gyro";
    default: return "no gyro";
    }
}
const char* connectionName(uint8_t c) {
    switch (c) {
    case 1: return "USB";
    case 2: return "Bluetooth";
    default: return "network";
    }
}
std::string batteryName(uint8_t b) {
    switch (b) {
    case 0x01: return "dying";
    case 0x02: return "low";
    case 0x03: return "medium";
    case 0x04: return "high";
    case 0x05: return "full";
    case 0xEE: return "charging";
    case 0xEF: return "charged";
    default: return "";
    }
}

}  // namespace

void update() {
    bool enabled;
    Layout layout;
    {
        std::lock_guard lk(sCfgM);
        enabled = sCfg.enabled;
        layout = sCfg.layout;
    }
    if (enabled && !sThreadStarted.exchange(true)) std::thread(netThread).detach();
    std::lock_guard lk(sM);
    const auto now = Clock::now();
    std::vector<std::string> live;
    for (size_t s = 0; s < sServers.size(); s++)
        for (int slot = 0; slot < kSlots; slot++) {
            const PadData& d = sServers[s].pads[size_t(slot)];
            if (!d.valid || std::chrono::duration<double>(now - d.when).count() > kSlotTimeout) continue;
            const std::string key = padName(int(s), slot, sServers.size());
            live.push_back(key);
            auto it = std::find_if(sVirtual.begin(), sVirtual.end(), [&](const Virtual& v) { return v.key == key; });
            if (it != sVirtual.end() && it->layout != layout) {  // layout changed: plug in again as the other type
                detach(*it);
                sVirtual.erase(it);
                it = sVirtual.end();
            }
            if (it == sVirtual.end()) {
                sVirtual.push_back(attach(key, key, layout));
                it = sVirtual.end() - 1;
            }
            if (it->joy) apply(it->joy, d, layout);
        }
    for (auto it = sVirtual.begin(); it != sVirtual.end();) {
        if (std::find(live.begin(), live.end(), it->key) == live.end()) {
            detach(*it);
            it = sVirtual.erase(it);
        } else {
            ++it;
        }
    }
}

bool drawTab() {
    bool changed = false;
    Config cfg;
    {
        std::lock_guard lk(sCfgM);
        cfg = sCfg;
    }
    ImGui::TextWrapped("DSU (\"cemuhook\") controllers: pads that another program serves over the network, such as "
                       "DS4Windows, BetterJoy, Steam Deck or phone apps (the protocol Dolphin's DSU client uses). "
                       "Each connected DSU slot shows up as a controller: assign it on the Ports tab and change its "
                       "buttons on the Bindings tab. Motion is not used.");
    ImGui::Spacing();
    changed |= recomp::menu::Checkbox("Enable DSU client", &cfg.enabled);
    ImGui::SetItemTooltip("Connects to the DSU servers below and adds their controllers.");
    static char sBuf[256];
    static unsigned sBufGen = 0;
    static bool sEditing = false;
    if (sBufGen != sCfgGen.load() && !sEditing) {
        std::snprintf(sBuf, sizeof(sBuf), "%s", cfg.servers.c_str());
        sBufGen = sCfgGen.load();
    }
    ImGui::BeginDisabled(!cfg.enabled);
    ImGui::SetNextItemWidth(320.0f * ImGui::GetFontSize() / 13.0f);
    if (ImGui::InputText("Servers", sBuf, sizeof(sBuf), ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        cfg.servers = sBuf;
        changed = true;
    }
    sEditing = ImGui::IsItemActive();
    ImGui::SetItemTooltip("host:port of each DSU server, separated by ';' (up to %d). The usual one is\n"
                          "127.0.0.1:26760 (a server on this computer); a phone or another PC is given by its\n"
                          "IP address, e.g. 192.168.1.20:26760.",
                          kMaxServers);
    ImGui::SameLine();
    if (recomp::menu::Button("Default")) {
        cfg.servers = kDefaultServers;
        changed = true;
    }
    {
        int l = int(cfg.layout);
        ImGui::SetNextItemWidth(320.0f * ImGui::GetFontSize() / 13.0f);
        if (ImGui::Combo("Controller layout", &l, "GameCube controller\0PlayStation (DS4) or other pad\0")) {
            cfg.layout = Layout(l);
            changed = true;
        }
        ImGui::SetItemTooltip("What the DSU server's controllers are. GameCube controller: a GameCube pad behind the\n"
                              "server (e.g. NSO GC Driver: A/B/X/Y as cross/circle/square/triangle, Z as R3, L/R as\n"
                              "the analog triggers with L1/R1 at the click); the pad gets the GameCube layout.\n"
                              "PlayStation: DS4Windows, BetterJoy, phone apps; buttons by position.\n"
                              "Each layout has its own binding profile.");
    }
    ImGui::EndDisabled();
    if (changed) {
        std::lock_guard lk(sCfgM);
        if (!(cfg == sCfg)) {
            sCfg = cfg;
            sCfgGen++;
        } else {
            changed = false;
        }
    }
    if (!cfg.enabled) return changed;
    ImGui::Separator();
    std::lock_guard lk(sM);
    const auto now = Clock::now();
    if (sServers.empty()) ImGui::TextDisabled("No servers.");
    for (size_t s = 0; s < sServers.size(); s++) {
        const Server& sv = sServers[s];
        const bool answering = sv.lastValid != Clock::time_point{} &&
                               std::chrono::duration<double>(now - sv.lastValid).count() < kServerTimeout;
        ImGui::PushID(int(s));
        if (!sv.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s: %s", sv.address.c_str(), sv.error.c_str());
        else if (!answering) ImGui::Text("%s: waiting for the server...", sv.address.c_str());
        else ImGui::Text("%s: connected (protocol %u, server id %08X)", sv.address.c_str(), sv.version, sv.serverId);
        int shown = 0;
        for (int slot = 0; slot < kSlots; slot++) {
            const PadData& d = sv.pads[size_t(slot)];
            if (!d.valid || std::chrono::duration<double>(now - d.when).count() > kSlotTimeout) continue;
            const std::string bat = batteryName(d.battery);
            ImGui::BulletText("%s: %s, %s%s%s, %u packets/s", padName(int(s), slot, sServers.size()).c_str(),
                              connectionName(d.connection), modelName(d.model), bat.empty() ? "" : ", battery ",
                              bat.c_str(), d.packetsLastSecond);
            shown++;
        }
        if (answering && !shown) ImGui::BulletText("No controller connected to this server.");
        ImGui::PopID();
    }
    return changed;
}

}  // namespace recomp::dsu
