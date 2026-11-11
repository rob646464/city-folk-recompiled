// cf_lobby: the self-hosted lobby of City Folk Recompiled (docs/ONLINE.md "The lobby"), and the local
// stand-in for the Cloudflare Worker in tests (tools/tests/lobby_api_test.cpp). It implements the lobby
// HTTP API v1: a room directory (GET /v1/rooms), the host's WebSocket (GET /v1/host), a signaling pipe
// from a joining player to a room's host (POST /v1/signal) and static relay credentials (POST /v1/turn).
// It never carries game data and keeps nothing on disk: a room lives exactly as long as its host's socket.
//
// One self-contained C++17 file, no dependencies:
//   Linux / macOS:  g++ -O2 -std=c++17 -pthread cf_lobby.cpp -o cf_lobby
//   Windows:        clang-cl /std:c++17 /EHsc /O2 cf_lobby.cpp ws2_32.lib
// Plain HTTP/1.1 only (every answer closes its connection). For https put a reverse proxy that passes
// WebSocket upgrades in front (Caddy, nginx) and start cf_lobby with --trust-proxy.
//
//   cf_lobby [--port 29920] [--bind <ip>] [--turn <host:port> --turn-user <u> --turn-pass <p>]
//            [--max-rooms 500] [--rooms-per-ip 4] [--trust-proxy] [--verbose]
//
// Privacy: client addresses are kept in memory only (for the per-address limits) and never printed.
// --verbose prints one line per room registered / removed (the room's name, no address); otherwise one
// statistics line every 10 minutes.
//
// One thread, one poll() loop: a POST /v1/signal that waits for the host's answer is just a connection
// in a waiting state, so every other request is served meanwhile.
//
// Test build: -DCF_LOBBY_AS_LIB turns main into lobbyMain(argc, argv); gLobbyPort is the bound port once
// it listens, setting gLobbyStop makes lobbyMain return. Test knobs: --signal-timeout-ms, --stale-ms,
// --http-timeout-ms, --signals-per-min, --turns-per-hour, --stats-s.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

std::atomic<int> gLobbyPort{0};      // the bound port once listening, 0 before / after
std::atomic<bool> gLobbyStop{false}; // set true: lobbyMain closes everything and returns

namespace {

// ---- sockets ------------------------------------------------------------------------------------
#ifdef _WIN32
using sock_t = SOCKET;
const sock_t kNoSock = INVALID_SOCKET;
using pollfd_t = WSAPOLLFD;
int pollFds(pollfd_t* f, size_t n, int ms) { return WSAPoll(f, ULONG(n), ms); }
void closeSock(sock_t s) { closesocket(s); }
bool wouldBlock() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
}
void setNonBlocking(sock_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
void shutdownSend(sock_t s) { shutdown(s, SD_SEND); }
const int kSendFlags = 0;
#else
using sock_t = int;
const sock_t kNoSock = -1;
using pollfd_t = struct pollfd;
int pollFds(pollfd_t* f, size_t n, int ms) { return poll(f, nfds_t(n), ms); }
void closeSock(sock_t s) { close(s); }
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
void setNonBlocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
void shutdownSend(sock_t s) { shutdown(s, SHUT_WR); }
#ifdef MSG_NOSIGNAL
const int kSendFlags = MSG_NOSIGNAL;
#else
const int kSendFlags = 0;
#endif
#endif

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

// ---- limits of the API (LOBBY_API.md) -----------------------------------------------------------
constexpr size_t kMaxHeader = 8192;   // request line + headers
constexpr size_t kMaxBody = 8192;     // "request bodies over 8 KB -> 400"
constexpr size_t kMaxWsMessage = 4096;
constexpr size_t kMaxData = 2800;     // base64 signal blob
constexpr size_t kMaxListed = 200;
constexpr int kMaxWaitingPerRoom = 8;
constexpr int kMessagesPer10s = 30;
constexpr size_t kMaxOutBuffer = 512 * 1024;  // a peer that does not read is dropped
constexpr const char* kVersionMessage = "This lobby needs a newer City Folk Recompiled";

// ---- SHA-1 and base64 (the WebSocket accept key) ------------------------------------------------
void sha1(const std::string& in, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string m = in;
    const uint64_t bits = uint64_t(in.size()) * 8;
    m += char(-128);  // 0x80
    while (m.size() % 64 != 56) m += char(0);
    for (int i = 7; i >= 0; i--) m += char(uint8_t(bits >> (8 * i)));
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(uint8_t(m[off + 4 * i])) << 24 | uint32_t(uint8_t(m[off + 4 * i + 1])) << 16 |
                   uint32_t(uint8_t(m[off + 4 * i + 2])) << 8 | uint32_t(uint8_t(m[off + 4 * i + 3]));
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* kTab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += kTab[v >> 18 & 63];
        out += kTab[v >> 12 & 63];
        out += i + 1 < n ? kTab[v >> 6 & 63] : '=';
        out += i + 2 < n ? kTab[v & 63] : '=';
    }
    return out;
}

// ---- text helpers --------------------------------------------------------------------------------
// One UTF-8 code point at s[i] (strict: no overlong forms, no surrogates). Advances i.
bool nextCodePoint(const std::string& s, size_t& i, uint32_t& cp) {
    const size_t n = s.size();
    const uint8_t c = uint8_t(s[i]);
    int extra;
    uint32_t min;
    if (c < 0x80) {
        cp = c;
        i++;
        return true;
    } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F;
        extra = 1;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F;
        extra = 2;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07;
        extra = 3;
        min = 0x10000;
    } else {
        return false;
    }
    if (i + size_t(extra) >= n) return false;
    for (int k = 1; k <= extra; k++) {
        const uint8_t d = uint8_t(s[i + size_t(k)]);
        if ((d & 0xC0) != 0x80) return false;
        cp = cp << 6 | (d & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return false;
    i += size_t(extra) + 1;
    return true;
}

bool validUtf8(const std::string& s) {
    size_t i = 0;
    uint32_t cp;
    while (i < s.size())
        if (!nextCodePoint(s, i, cp)) return false;
    return true;
}

void putUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | cp >> 6);
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | cp >> 12);
        out += char(0x80 | (cp >> 6 & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | cp >> 18);
        out += char(0x80 | (cp >> 12 & 0x3F));
        out += char(0x80 | (cp >> 6 & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// A name as the lobby lists it: control characters (< 0x20, 0x7F) removed, spaces trimmed, cut to
// maxCodePoints code points (and trimmed again). The input is valid UTF-8.
std::string sanitizeName(const std::string& s, size_t maxCodePoints) {
    std::vector<uint32_t> cps;
    size_t i = 0;
    uint32_t cp;
    while (i < s.size()) {
        if (!nextCodePoint(s, i, cp)) break;
        if (cp < 0x20 || cp == 0x7F) continue;
        cps.push_back(cp);
    }
    size_t a = 0, b = cps.size();
    while (a < b && cps[a] == ' ') a++;
    while (b > a && cps[b - 1] == ' ') b--;
    if (b - a > maxCodePoints) b = a + maxCodePoints;
    while (b > a && cps[b - 1] == ' ') b--;
    std::string out;
    for (size_t k = a; k < b; k++) putUtf8(out, cps[k]);
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool isHex(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

bool isGameId(const std::string& s) {
    if (s.size() != 6) return false;
    for (const char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

bool isVersionText(const std::string& s) {
    if (s.size() > 24) return false;
    for (const char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '+' || c == '-'))
            return false;
    return true;
}

// A signal blob: base64 (standard or URL-safe alphabet), at most 2800 characters. Opaque otherwise.
bool isSignalData(const std::string& s) {
    if (s.size() > kMaxData) return false;
    for (const char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
              c == '=' || c == '-' || c == '_'))
            return false;
    return true;
}

// ---- JSON ---------------------------------------------------------------------------------------
struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::string> ok;  // object keys / values (two vectors: a pair of an incomplete type
    std::vector<JVal> ov;         // does not compile as C++20 with clang + libstdc++)

    const JVal* get(const char* key) const {  // the last one wins, as in JavaScript
        if (type != Obj) return nullptr;
        const JVal* found = nullptr;
        for (size_t i = 0; i < ok.size(); i++)
            if (ok[i] == key) found = &ov[i];
        return found;
    }
    bool str(const char* key, std::string& out) const {
        const JVal* v = get(key);
        if (!v || v->type != Str) return false;
        out = v->s;
        return true;
    }
    // An integer value (1, 1.0, 1e0), like Number.isInteger.
    bool integer(const char* key, long long& out) const {
        const JVal* v = get(key);
        if (!v || v->type != Num) return false;
        if (!(v->n >= -9e15 && v->n <= 9e15)) return false;
        const long long i = static_cast<long long>(v->n);
        if (double(i) != v->n) return false;
        out = i;
        return true;
    }
};

struct JParser {
    const char* p;
    const char* end;
    int depth = 0;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool lit(const char* t) {
        const size_t n = std::strlen(t);
        if (size_t(end - p) < n || std::memcmp(p, t, n) != 0) return ok = false;
        p += n;
        return true;
    }
    bool hex4(uint32_t& v) {
        if (end - p < 4) return ok = false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return ok = false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (p >= end || *p != '"') return ok = false;
        p++;
        while (p < end && *p != '"') {
            const uint8_t c = uint8_t(*p);
            if (c < 0x20) return ok = false;
            if (c != '\\') {
                out += *p++;
                continue;
            }
            if (++p >= end) return ok = false;
            const char e = *p++;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    const char* save = p;
                    p += 2;
                    uint32_t lo = 0;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = 0xFFFD;  // a lone high surrogate; the next escape is read again
                        p = save;
                    }
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                putUtf8(out, cp);
                break;
            }
            default: return ok = false;
            }
        }
        if (p >= end) return ok = false;
        p++;
        return true;
    }
    bool number(double& out) {
        const char* s = p;
        if (p < end && *p == '-') p++;
        if (p >= end) return ok = false;
        if (*p == '0') {
            p++;
        } else if (*p >= '1' && *p <= '9') {
            while (p < end && *p >= '0' && *p <= '9') p++;
        } else {
            return ok = false;
        }
        if (p < end && *p == '.') {
            p++;
            if (p >= end || *p < '0' || *p > '9') return ok = false;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            p++;
            if (p < end && (*p == '+' || *p == '-')) p++;
            if (p >= end || *p < '0' || *p > '9') return ok = false;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p - s > 64) return ok = false;
        out = std::strtod(std::string(s, p).c_str(), nullptr);
        return true;
    }
    JVal value() {
        JVal v;
        ws();
        if (p >= end || ++depth > 16) {
            ok = false;
            return v;
        }
        const char c = *p;
        if (c == '{') {
            v.type = JVal::Obj;
            p++;
            ws();
            if (p < end && *p == '}') {
                p++;
            } else {
                while (ok) {
                    ws();
                    std::string k;
                    if (!string(k)) break;
                    ws();
                    if (p >= end || *p != ':') {
                        ok = false;
                        break;
                    }
                    p++;
                    JVal item = value();
                    v.ok.push_back(std::move(k));
                    v.ov.push_back(std::move(item));
                    ws();
                    if (p < end && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < end && *p == '}') {
                        p++;
                        break;
                    }
                    ok = false;
                }
            }
        } else if (c == '[') {
            v.type = JVal::Arr;
            p++;
            ws();
            if (p < end && *p == ']') {
                p++;
            } else {
                while (ok) {
                    v.a.push_back(value());
                    ws();
                    if (p < end && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < end && *p == ']') {
                        p++;
                        break;
                    }
                    ok = false;
                }
            }
        } else if (c == '"') {
            v.type = JVal::Str;
            string(v.s);
        } else if (c == 't') {
            v.type = JVal::Bool;
            v.b = lit("true");
        } else if (c == 'f') {
            v.type = JVal::Bool;
            lit("false");
        } else if (c == 'n') {
            lit("null");
        } else {
            v.type = JVal::Num;
            number(v.n);
        }
        depth--;
        return v;
    }
};

bool parseJson(const std::string& text, JVal& out) {
    if (!validUtf8(text)) return false;
    JParser jp{text.data(), text.data() + text.size()};
    out = jp.value();
    jp.ws();
    return jp.ok && jp.p == jp.end;
}

std::string jq(const std::string& s) {  // a quoted JSON string
    static const char* kHex = "0123456789abcdef";
    std::string out = "\"";
    for (const char ch : s) {
        const uint8_t c = uint8_t(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) {
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else out += ch;
    }
    out += '"';
    return out;
}

std::string errorJson(const char* code, const std::string& message) {
    return std::string("{\"error\":\"") + code + "\",\"message\":" + jq(message) + "}";
}

// ---- the server ---------------------------------------------------------------------------------
struct Options {
    int port = 29920;
    std::string bind;
    std::string turn, turnUser, turnPass;
    int maxRooms = 500;
    int roomsPerIp = 4;
    bool trustProxy = false;
    bool verbose = false;
    // test knobs
    int signalTimeoutMs = 8000;
    int staleMs = 120000;
    int httpTimeoutMs = 10000;
    int signalsPerMin = 20;
    int turnsPerHour = 10;
    int statsS = 600;
};

struct Room {
    std::string name, host, town, game, ver, tag;
    int players = 1, max = 4;
    bool lock = false, friends = false;
};

enum class St {
    Http,   // reading a request
    Wait,   // a POST /v1/signal waiting for the host's answer
    Ws,     // a host's WebSocket
    Drain,  // the answer is queued: send it, half-close, wait for the peer to close
};

struct Request {
    std::string method, path, query, wsKey, wsVersion, upgrade, forwardedFor;
    size_t headerEnd = 0;  // offset of the body
    size_t contentLength = 0;
    bool parsed = false;
};

struct Conn {
    uint64_t seq = 0;
    sock_t fd = kNoSock;
    std::string ip;      // the limits' key (TCP peer, or the proxy's X-Forwarded-For)
    std::string peerIp;  // the TCP peer (per-address connection count)
    St st = St::Http;
    std::string in, out;
    Clock::time_point born;
    bool dead = false;
    Request req;
    // Wait
    std::string waitKey;  // "<room id>:<peer>"
    std::string waitRoom;
    Clock::time_point waitDeadline;
    // Ws
    bool hasRoom = false;
    std::string id;
    Room room;
    Clock::time_point registered;
    uint64_t regSeq = 0;
    Clock::time_point lastMsg;
    std::deque<Clock::time_point> msgTimes;
    int waiting = 0;  // signals waiting for this host
    // Drain
    bool shutSent = false;
    bool peerEof = false;  // the peer finished sending while its answer was still going out
    Clock::time_point drainStart, drainDeadline;
};

struct Rate {
    Clock::time_point start;
    int count = 0;
};

const char* statusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Error";
    }
}

class Server {
public:
    explicit Server(const Options& o) : o_(o), rng_(seed()) {}
    int run();

private:
    static uint64_t seed() {
        std::random_device rd;
        return (uint64_t(rd()) << 32 | rd()) ^ uint64_t(Clock::now().time_since_epoch().count());
    }

    // connections
    void acceptAll();
    void onReadable(Conn& c);
    void flush(Conn& c);
    void timers();
    void remove(uint64_t seq);
    void startDrain(Conn& c);

    // HTTP
    void processHttp(Conn& c);
    bool parseRequest(Conn& c, size_t headerEnd);
    void dispatch(Conn& c, const std::string& body);
    void respond(Conn& c, int status, const std::string& body);
    void fail(Conn& c, int status, const char* code, const std::string& message) {
        respond(c, status, errorJson(code, message));
    }
    void badRequest(Conn& c, const std::string& message) { fail(c, 400, "bad_request", message); }
    void handleInfo(Conn& c);
    void handleRooms(Conn& c, const std::string& query);
    void handleHostUpgrade(Conn& c);
    void handleSignal(Conn& c, const std::string& body);
    void handleTurn(Conn& c, const std::string& body);

    // WebSocket
    void processWs(Conn& c);
    void wsSend(Conn& c, int opcode, const std::string& payload);
    void wsText(Conn& c, const std::string& text) { wsSend(c, 1, text); }
    void wsClose(Conn& c, int code);
    void wsError(Conn& c, const char* code, const std::string& message) {
        wsText(c, std::string("{\"t\":\"error\",\"code\":\"") + code + "\",\"message\":" + jq(message) + "}");
    }
    void onMessage(Conn& c, const std::string& text);
    void onReg(Conn& c, const JVal& msg);
    void onSig(Conn& c, const JVal& msg);
    void removeRoom(Conn& c);
    void endWait(Conn& c);

    bool allow(std::unordered_map<std::string, Rate>& m, const std::string& ip, int limit, Ms window);
    std::string newRoomId();
    int roomCount() const { return int(rooms_.size()); }
    bool turnConfigured() const { return !o_.turn.empty(); }
    std::string roomJson(const Conn& c, Clock::time_point now) const;

    Options o_;
    std::mt19937_64 rng_;
    sock_t listener_ = kNoSock;
    uint64_t nextSeq_ = 1;
    std::map<uint64_t, std::unique_ptr<Conn>> conns_;
    std::unordered_map<std::string, uint64_t> rooms_;   // room id -> host connection
    std::map<std::string, uint64_t> waiters_;           // "<room id>:<peer>" -> waiting connection
    std::unordered_map<std::string, int> connsPerIp_;   // TCP peers
    std::unordered_map<std::string, int> socketsPerIp_; // host sockets per client
    std::unordered_map<std::string, Rate> signalRate_, turnRate_;
    int wsCount_ = 0;
    // statistics
    uint64_t statRequests_ = 0, statSignals_ = 0, statAnswered_ = 0, statRegistered_ = 0;
};

bool Server::allow(std::unordered_map<std::string, Rate>& m, const std::string& ip, int limit, Ms window) {
    const auto now = Clock::now();
    Rate& r = m[ip];
    if (r.count == 0 || now - r.start >= window) {
        r.start = now;
        r.count = 0;
    }
    if (r.count >= limit) return false;
    r.count++;
    return true;
}

std::string Server::newRoomId() {
    static const char* kHex = "0123456789abcdef";
    for (;;) {
        const uint64_t v = rng_();
        std::string id(16, '0');
        for (int i = 0; i < 16; i++) id[size_t(i)] = kHex[v >> (60 - 4 * i) & 15];
        if (!rooms_.count(id)) return id;
    }
}

// ---- connections ---------------------------------------------------------------------------------
void Server::acceptAll() {
    for (int i = 0; i < 64; i++) {
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
        const sock_t fd = accept(listener_, reinterpret_cast<sockaddr*>(&ss), &len);
        if (fd == kNoSock) return;
        char host[NI_MAXHOST] = "";
        getnameinfo(reinterpret_cast<sockaddr*>(&ss), len, host, sizeof host, nullptr, 0, NI_NUMERICHOST);
        std::string ip = host;
        if (ip.rfind("::ffff:", 0) == 0 && ip.find('.') != std::string::npos) ip = ip.substr(7);
        // Too many connections in all, or from one address (not behind a proxy: there every peer is the proxy).
        const size_t maxConns = size_t(o_.maxRooms) + 1024;
        if (conns_.size() >= maxConns || (!o_.trustProxy && connsPerIp_[ip] >= 64)) {
            closeSock(fd);
            continue;
        }
        setNonBlocking(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof one);
#endif
        auto c = std::make_unique<Conn>();
        c->seq = nextSeq_++;
        c->fd = fd;
        c->ip = c->peerIp = ip;
        c->born = Clock::now();
        connsPerIp_[ip]++;
        conns_[c->seq] = std::move(c);
    }
}

void Server::onReadable(Conn& c) {
    char buf[16384];
    for (int i = 0; i < 4 && !c.dead; i++) {
        const int n = int(recv(c.fd, buf, sizeof buf, 0));
        if (n > 0) {
            if (c.st == St::Http) {
                c.in.append(buf, size_t(n));
                processHttp(c);
            } else if (c.st == St::Ws) {
                c.in.append(buf, size_t(n));
                processWs(c);
            }  // Wait / Drain: whatever else the peer sends is dropped
            if (size_t(n) < sizeof buf) return;
        } else if (n == 0) {
            // The peer closed: a waiting joiner gave up, a host went away, a drain finished. A peer that only
            // finished sending still gets the rest of its answer.
            if (c.st == St::Drain && !c.out.empty()) c.peerEof = true;
            else c.dead = true;
            return;
        } else {
            if (!wouldBlock()) c.dead = true;
            return;
        }
    }
}

void Server::flush(Conn& c) {
    while (!c.out.empty() && !c.dead) {
        const int n = int(send(c.fd, c.out.data(), int(std::min<size_t>(c.out.size(), 65536)), kSendFlags));
        if (n > 0) {
            c.out.erase(0, size_t(n));
        } else {
            if (!wouldBlock()) c.dead = true;
            break;
        }
    }
    if (c.out.size() > kMaxOutBuffer) c.dead = true;
    if (c.peerEof && c.out.empty()) c.dead = true;
    if (c.st == St::Drain && c.out.empty() && !c.shutSent && !c.dead) {
        shutdownSend(c.fd);
        c.shutSent = true;
        c.drainDeadline = Clock::now() + Ms(2000);
    }
}

void Server::startDrain(Conn& c) {
    if (c.st == St::Ws) {
        removeRoom(c);
        wsCount_--;
        if (--socketsPerIp_[c.ip] <= 0) socketsPerIp_.erase(c.ip);
    } else if (c.st == St::Wait) {
        endWait(c);
    }
    c.st = St::Drain;
    c.in.clear();
    c.drainStart = Clock::now();
}

void Server::remove(uint64_t seq) {
    auto it = conns_.find(seq);
    if (it == conns_.end()) return;
    Conn& c = *it->second;
    if (c.st == St::Ws || c.st == St::Wait) startDrain(c);  // releases the room / the waiter
    closeSock(c.fd);
    if (--connsPerIp_[c.peerIp] <= 0) connsPerIp_.erase(c.peerIp);
    conns_.erase(it);
}

void Server::timers() {
    const auto now = Clock::now();
    for (auto& kv : conns_) {
        Conn& c = *kv.second;
        if (c.dead) continue;
        switch (c.st) {
        case St::Http:
            if (now - c.born > Ms(o_.httpTimeoutMs)) c.dead = true;  // a slow or half-open request
            break;
        case St::Wait:
            if (now >= c.waitDeadline) fail(c, 504, "timeout", "The room's host did not answer");
            break;
        case St::Ws:
            if (now - c.lastMsg > Ms(o_.staleMs)) wsClose(c, 1001);  // sent nothing, not even "ping"
            break;
        case St::Drain:
            if ((c.shutSent && now >= c.drainDeadline) || now - c.drainStart > Ms(10000)) c.dead = true;
            break;
        }
    }
}

// ---- HTTP ------------------------------------------------------------------------------------------
void Server::respond(Conn& c, int status, const std::string& body) {
    if (c.st == St::Drain) return;
    char head[256];
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                  status, statusText(status), body.size());
    startDrain(c);
    c.out += head;
    c.out += body;
}

bool Server::parseRequest(Conn& c, size_t headerEnd) {
    Request& r = c.req;
    const std::string head = c.in.substr(0, headerEnd);
    if (head.find('\0') != std::string::npos) return false;
    size_t lineEnd = head.find("\r\n");
    const std::string line = head.substr(0, lineEnd);
    const size_t s1 = line.find(' ');
    const size_t s2 = s1 == std::string::npos ? std::string::npos : line.find(' ', s1 + 1);
    if (s2 == std::string::npos) return false;
    r.method = line.substr(0, s1);
    const std::string target = line.substr(s1 + 1, s2 - s1 - 1);
    const std::string proto = line.substr(s2 + 1);
    if (r.method.empty() || r.method.size() > 16) return false;
    for (const char ch : r.method)
        if (ch < 'A' || ch > 'Z') return false;
    if (target.empty() || target[0] != '/' || proto.rfind("HTTP/1.", 0) != 0) return false;
    const size_t q = target.find('?');
    r.path = target.substr(0, q);
    r.query = q == std::string::npos ? std::string() : target.substr(q + 1);
    bool haveLength = false;
    while (lineEnd != std::string::npos && lineEnd + 2 < head.size()) {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string h = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
        if (h.empty()) continue;
        const size_t colon = h.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        const std::string name = lower(h.substr(0, colon));
        size_t a = colon + 1, b = h.size();
        while (a < b && (h[a] == ' ' || h[a] == '\t')) a++;
        while (b > a && (h[b - 1] == ' ' || h[b - 1] == '\t')) b--;
        const std::string value = h.substr(a, b - a);
        if (name == "content-length") {
            if (value.empty() || value.size() > 9 || haveLength) return false;
            for (const char ch : value)
                if (ch < '0' || ch > '9') return false;
            r.contentLength = size_t(std::atol(value.c_str()));
            haveLength = true;
        } else if (name == "transfer-encoding") {
            return false;  // chunked bodies are not supported (no client of this API sends them)
        } else if (name == "upgrade") {
            r.upgrade = lower(value);
        } else if (name == "sec-websocket-key") {
            r.wsKey = value;
        } else if (name == "sec-websocket-version") {
            r.wsVersion = value;
        } else if (name == "x-forwarded-for") {
            r.forwardedFor = value;
        }
    }
    r.headerEnd = headerEnd + 4;
    r.parsed = true;
    return true;
}

void Server::processHttp(Conn& c) {
    if (!c.req.parsed) {
        const size_t he = c.in.find("\r\n\r\n");
        if (he == std::string::npos ? c.in.size() > kMaxHeader : he > kMaxHeader)
            return badRequest(c, "The request header is too large");
        if (he == std::string::npos) return;
        if (!parseRequest(c, he)) return badRequest(c, "Not an HTTP request this lobby understands");
        if (o_.trustProxy && !c.req.forwardedFor.empty()) {
            // The last entry is the one our own proxy added.
            std::string v = c.req.forwardedFor;
            const size_t comma = v.rfind(',');
            if (comma != std::string::npos) v = v.substr(comma + 1);
            size_t a = 0, b = v.size();
            while (a < b && v[a] == ' ') a++;
            while (b > a && v[b - 1] == ' ') b--;
            v = lower(v.substr(a, b - a));
            bool good = !v.empty() && v.size() <= 45;
            for (const char ch : v)
                if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || ch == ':' || ch == '.')) good = false;
            if (good) c.ip = v;
        }
        if (c.req.contentLength > kMaxBody) return badRequest(c, "The request body is too large");
    }
    if (c.in.size() < c.req.headerEnd + c.req.contentLength) return;
    const std::string body = c.in.substr(c.req.headerEnd, c.req.contentLength);
    statRequests_++;
    dispatch(c, body);
}

// The value of a query parameter ("" when absent). Values of this API need no percent-decoding.
std::string queryParam(const std::string& query, const char* key, bool* present = nullptr) {
    if (present) *present = false;
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string item = query.substr(pos, amp - pos);
        const size_t eq = item.find('=');
        if (item.substr(0, eq) == key) {
            if (present) *present = true;
            return eq == std::string::npos ? std::string() : item.substr(eq + 1);
        }
        pos = amp + 1;
    }
    return {};
}

void Server::dispatch(Conn& c, const std::string& body) {
    const Request& r = c.req;
    const bool get = r.method == "GET", post = r.method == "POST";
    const bool isGetPath = r.path == "/v1/info" || r.path == "/v1/rooms" || r.path == "/v1/host";
    const bool isPostPath = r.path == "/v1/signal" || r.path == "/v1/turn";
    if (!isGetPath && !isPostPath) return fail(c, 404, "not_found", "No such path");
    if (isGetPath && !get) return badRequest(c, "Use GET for this path");
    if (isPostPath && !post) return badRequest(c, "Use POST for this path");
    if (isGetPath) {
        if (queryParam(r.query, "v") != "1") return fail(c, 426, "version", kVersionMessage);
        if (r.path == "/v1/info") return handleInfo(c);
        if (r.path == "/v1/rooms") return handleRooms(c, r.query);
        return handleHostUpgrade(c);
    }
    if (r.path == "/v1/signal") return handleSignal(c, body);
    return handleTurn(c, body);
}

void Server::handleInfo(Conn& c) {
    respond(c, 200, "{\"v\":1,\"name\":\"cf-lobby\",\"rooms\":" + std::to_string(roomCount()) + ",\"turn\":" +
                        (turnConfigured() ? "true" : "false") + "}");
}

std::string Server::roomJson(const Conn& c, Clock::time_point now) const {
    const Room& r = c.room;
    const long long age = std::chrono::duration_cast<std::chrono::seconds>(now - c.registered).count();
    std::string s = "{\"id\":\"" + c.id + "\"";
    // Friends-only rooms are listed without their names: friends recognise them by the tag.
    s += ",\"name\":" + jq(r.friends ? std::string() : r.name);
    s += ",\"host\":" + jq(r.friends ? std::string() : r.host);
    s += ",\"town\":" + jq(r.friends ? std::string() : r.town);
    s += ",\"players\":" + std::to_string(r.players) + ",\"max\":" + std::to_string(r.max);
    s += std::string(",\"lock\":") + (r.lock ? "true" : "false") + ",\"friends\":" + (r.friends ? "true" : "false");
    s += ",\"game\":" + jq(r.game) + ",\"ver\":" + jq(r.ver) + ",\"tag\":" + jq(r.tag);
    s += ",\"age\":" + std::to_string(age < 0 ? 0 : age) + "}";
    return s;
}

void Server::handleRooms(Conn& c, const std::string& query) {
    bool hasGame = false;
    const std::string game = queryParam(query, "game", &hasGame);
    if (hasGame && !isGameId(game)) return badRequest(c, "Bad game id");
    std::vector<const Conn*> list;
    for (const auto& kv : rooms_) {
        const auto it = conns_.find(kv.second);
        if (it == conns_.end() || !it->second->hasRoom) continue;
        if (hasGame && it->second->room.game != game) continue;
        list.push_back(it->second.get());
    }
    std::sort(list.begin(), list.end(), [](const Conn* a, const Conn* b) { return a->regSeq > b->regSeq; });  // newest first
    if (list.size() > kMaxListed) list.resize(kMaxListed);
    const auto now = Clock::now();
    std::string s = std::string("{\"v\":1,\"turn\":") + (turnConfigured() ? "true" : "false") + ",\"rooms\":[";
    for (size_t i = 0; i < list.size(); i++) {
        if (i) s += ',';
        s += roomJson(*list[i], now);
    }
    s += "]}";
    respond(c, 200, s);
}

void Server::handleHostUpgrade(Conn& c) {
    const Request& r = c.req;
    if (r.upgrade.find("websocket") == std::string::npos || r.wsKey.empty() || r.wsKey.size() > 64 || r.wsVersion != "13")
        return badRequest(c, "This path is a WebSocket (version 13)");
    // Sockets are limited a little above the room limits, so a host whose old socket is not gone yet can reconnect.
    if (wsCount_ >= o_.maxRooms + 100) return fail(c, 503, "full", "The lobby is full, try again later");
    if (socketsPerIp_[c.ip] >= std::max(8, o_.roomsPerIp * 2))
        return fail(c, 429, "too_many", "Too many connections from this address");
    uint8_t digest[20];
    sha1(r.wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", digest);
    c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
             base64(digest, 20) + "\r\n\r\n";
    c.in.erase(0, r.headerEnd);
    c.st = St::Ws;
    c.lastMsg = Clock::now();
    wsCount_++;
    socketsPerIp_[c.ip]++;
    if (!c.in.empty()) processWs(c);
}

void Server::handleSignal(Conn& c, const std::string& body) {
    JVal j;
    if (!parseJson(body, j) || j.type != JVal::Obj) return badRequest(c, "The body is not a JSON object");
    long long v = 0;
    if (!j.integer("v", v) || v != 1) return fail(c, 426, "version", kVersionMessage);
    std::string room, peer, data;
    if (!j.str("room", room) || !isHex(room, 16)) return badRequest(c, "Bad room id");
    if (!j.str("peer", peer) || !isHex(peer, 8)) return badRequest(c, "Bad peer id");
    if (!j.str("data", data) || !isSignalData(data)) return badRequest(c, "Bad signal data");
    if (!allow(signalRate_, c.ip, o_.signalsPerMin, Ms(60000)))
        return fail(c, 429, "too_many", "Too many join attempts, wait a minute");
    room = lower(room);
    const auto rit = rooms_.find(room);
    const auto hit = rit == rooms_.end() ? conns_.end() : conns_.find(rit->second);
    if (hit == conns_.end() || hit->second->st != St::Ws) return fail(c, 404, "no_room", "This room is no longer open");
    Conn& host = *hit->second;
    const std::string key = room + ":" + lower(peer);
    // The same peer asking again replaces its earlier request (it gave up on that one).
    if (const auto w = waiters_.find(key); w != waiters_.end()) {
        const auto old = conns_.find(w->second);
        if (old != conns_.end()) fail(*old->second, 504, "timeout", "The room's host did not answer");
        else waiters_.erase(w);
    }
    if (host.waiting >= kMaxWaitingPerRoom) return fail(c, 429, "too_many", "This room is busy, try again in a moment");
    statSignals_++;
    wsText(host, "{\"t\":\"sig\",\"peer\":" + jq(peer) + ",\"data\":" + jq(data) + "}");
    c.st = St::Wait;
    c.in.clear();
    c.waitKey = key;
    c.waitRoom = room;
    c.waitDeadline = Clock::now() + Ms(o_.signalTimeoutMs);
    waiters_[key] = c.seq;
    host.waiting++;
}

void Server::endWait(Conn& c) {
    const auto w = waiters_.find(c.waitKey);
    if (w != waiters_.end() && w->second == c.seq) waiters_.erase(w);
    const auto rit = rooms_.find(c.waitRoom);
    if (rit != rooms_.end()) {
        const auto hit = conns_.find(rit->second);
        if (hit != conns_.end() && hit->second->waiting > 0) hit->second->waiting--;
    }
}

void Server::handleTurn(Conn& c, const std::string& body) {
    JVal j;
    if (!parseJson(body, j) || j.type != JVal::Obj) return badRequest(c, "The body is not a JSON object");
    long long v = 0;
    if (!j.integer("v", v) || v != 1) return fail(c, 426, "version", kVersionMessage);
    std::string room;
    if (!j.str("room", room) || !isHex(room, 16)) return badRequest(c, "Bad room id");
    if (!turnConfigured()) return fail(c, 404, "no_turn", "This lobby has no relay");
    if (!allow(turnRate_, c.ip, o_.turnsPerHour, Ms(3600000)))
        return fail(c, 429, "too_many", "Too many relay requests, try again later");
    if (!rooms_.count(lower(room))) return fail(c, 404, "no_room", "This room is no longer open");
    respond(c, 200, "{\"servers\":[" + jq(o_.turn) + "],\"username\":" + jq(o_.turnUser) + ",\"credential\":" +
                        jq(o_.turnPass) + ",\"ttl\":86400}");
}

// ---- WebSocket ---------------------------------------------------------------------------------------
void Server::wsSend(Conn& c, int opcode, const std::string& payload) {
    if (c.st != St::Ws) return;
    std::string f;
    f += char(0x80 | opcode);
    if (payload.size() < 126) {
        f += char(payload.size());
    } else {
        f += char(126);
        f += char(payload.size() >> 8);
        f += char(payload.size() & 0xFF);
    }
    c.out += f;
    c.out += payload;
}

void Server::wsClose(Conn& c, int code) {
    if (c.st != St::Ws) return;
    std::string p;
    p += char(code >> 8);
    p += char(code & 0xFF);
    wsSend(c, 8, p);
    startDrain(c);
}

void Server::removeRoom(Conn& c) {
    if (!c.hasRoom) return;
    c.hasRoom = false;
    rooms_.erase(c.id);
    // Joiners waiting for this host learn at once that the room is gone.
    const std::string prefix = c.id + ":";
    std::vector<uint64_t> waiting;
    for (auto it = waiters_.lower_bound(prefix); it != waiters_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
        waiting.push_back(it->second);
    for (const uint64_t seq : waiting) {
        const auto w = conns_.find(seq);
        if (w != conns_.end()) fail(*w->second, 404, "no_room", "This room is no longer open");
    }
    if (o_.verbose) {
        std::printf("cf_lobby: room closed: %s (%d open)\n", c.room.friends ? "(friends only)" : c.room.name.c_str(), roomCount());
        std::fflush(stdout);
    }
}

void Server::processWs(Conn& c) {
    size_t pos = 0;
    while (c.st == St::Ws) {
        const size_t avail = c.in.size() - pos;
        if (avail < 2) break;
        const uint8_t b0 = uint8_t(c.in[pos]), b1 = uint8_t(c.in[pos + 1]);
        const int op = b0 & 15;
        const bool fin = (b0 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        size_t hdr = 2;
        if ((b0 & 0x70) || !(b1 & 0x80)) {  // extensions were not negotiated; clients must mask
            wsClose(c, 1002);
            break;
        }
        if (op >= 8) {
            if (!fin || len > 125) {
                wsClose(c, 1002);
                break;
            }
        } else if (op == 2) {  // binary
            wsClose(c, 1003);
            break;
        } else if (op == 0 || (op == 1 && !fin)) {  // fragmented messages are not accepted
            wsClose(c, 1003);
            break;
        } else if (op != 1) {
            wsClose(c, 1002);
            break;
        }
        if (len == 126) {
            if (avail < 4) break;
            len = uint64_t(uint8_t(c.in[pos + 2])) << 8 | uint8_t(c.in[pos + 3]);
            hdr = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | uint8_t(c.in[pos + 2 + size_t(i)]);
            hdr = 10;
        }
        if (len > kMaxWsMessage) {  // refused before its payload is read
            wsClose(c, 1009);
            break;
        }
        const size_t total = hdr + 4 + size_t(len);
        if (avail < total) break;
        std::string payload(size_t(len), '\0');
        const char* mask = c.in.data() + pos + hdr;
        const char* src = mask + 4;
        for (size_t i = 0; i < size_t(len); i++) payload[i] = char(src[i] ^ mask[i & 3]);
        pos += total;
        if (op == 1) {
            onMessage(c, payload);
        } else if (op == 8) {
            int code = 1000;
            if (payload.size() >= 2) code = int(uint8_t(payload[0])) << 8 | uint8_t(payload[1]);
            if (code < 1000 || code > 4999 || code == 1005 || code == 1006) code = 1000;
            wsClose(c, code);
        } else if (op == 9) {
            wsSend(c, 10, payload);
        } else if (op != 10) {
            wsClose(c, 1002);
        }
    }
    if (c.st == St::Ws) c.in.erase(0, pos);
}

void Server::onMessage(Conn& c, const std::string& text) {
    const auto now = Clock::now();
    if (text == "ping") {  // the keep-alive: answered, not counted as a message (the Worker never sees it)
        c.lastMsg = now;
        wsText(c, "pong");
        return;
    }
    while (!c.msgTimes.empty() && now - c.msgTimes.front() >= Ms(10000)) c.msgTimes.pop_front();
    if (int(c.msgTimes.size()) >= kMessagesPer10s) return wsClose(c, 1008);
    c.msgTimes.push_back(now);
    c.lastMsg = now;
    if (!validUtf8(text)) return wsClose(c, 1007);
    JVal j;
    if (!parseJson(text, j) || j.type != JVal::Obj) return wsError(c, "bad_request", "Not a JSON object");
    std::string t;
    j.str("t", t);
    if (t == "reg") return onReg(c, j);
    if (t == "sig") return onSig(c, j);
    wsError(c, "bad_request", "Unknown message type");
}

void Server::onReg(Conn& c, const JVal& msg) {
    const JVal* r = msg.get("room");
    if (!r || r->type != JVal::Obj) return wsError(c, "bad_request", "No room");
    Room room;
    // Names: optional strings, cleaned up. Everything else must be exactly right.
    for (const auto& f : {std::make_pair("name", &room.name), std::make_pair("host", &room.host), std::make_pair("town", &room.town)}) {
        const JVal* v = r->get(f.first);
        if (!v) continue;
        if (v->type != JVal::Str) return wsError(c, "bad_request", std::string("Bad ") + f.first);
        *f.second = v->s;
    }
    room.name = sanitizeName(room.name, 32);
    room.host = sanitizeName(room.host, 24);
    room.town = sanitizeName(room.town, 16);
    if (room.name.empty()) room.name = "Room";
    for (const auto& f : {std::make_pair("lock", &room.lock), std::make_pair("friends", &room.friends)}) {
        const JVal* v = r->get(f.first);
        if (!v) continue;
        if (v->type != JVal::Bool) return wsError(c, "bad_request", std::string("Bad ") + f.first);
        *f.second = v->b;
    }
    long long players = 0, max = 0;
    if (!r->str("game", room.game) || !isGameId(room.game)) return wsError(c, "bad_request", "Bad game");
    if (!r->str("ver", room.ver) || !isVersionText(room.ver)) return wsError(c, "bad_request", "Bad ver");
    if (!r->str("tag", room.tag) || !isHex(room.tag, 16)) return wsError(c, "bad_request", "Bad tag");
    if (!r->integer("max", max) || max < 2 || max > 4) return wsError(c, "bad_request", "Bad max");
    if (!r->integer("players", players) || players < 1 || players > max) return wsError(c, "bad_request", "Bad players");
    room.players = int(players);
    room.max = int(max);
    if (!c.hasRoom) {
        int mine = 0;
        for (const auto& kv : rooms_) {
            const auto it = conns_.find(kv.second);
            if (it != conns_.end() && it->second->ip == c.ip) mine++;
        }
        if (mine >= o_.roomsPerIp) return wsError(c, "too_many", "Too many rooms from this address");
        if (roomCount() >= o_.maxRooms) return wsError(c, "full", "The lobby is full, try again later");
        c.id = newRoomId();
        c.hasRoom = true;
        c.registered = Clock::now();
        c.regSeq = nextSeq_++;
        rooms_[c.id] = c.seq;
        statRegistered_++;
        if (o_.verbose) {
            std::printf("cf_lobby: room opened: %s (%s, %d open)\n", room.friends ? "(friends only)" : room.name.c_str(),
                        room.game.c_str(), roomCount());
            std::fflush(stdout);
        }
    }
    c.room = room;
    wsText(c, "{\"t\":\"reg_ok\",\"id\":\"" + c.id + "\"}");
}

void Server::onSig(Conn& c, const JVal& msg) {
    std::string peer, data;
    if (!msg.str("peer", peer) || !isHex(peer, 8)) return wsError(c, "bad_request", "Bad peer id");
    if (!msg.str("data", data) || !isSignalData(data)) return wsError(c, "bad_request", "Bad signal data");
    if (!c.hasRoom) return wsError(c, "bad_request", "No room registered");
    const auto w = waiters_.find(c.id + ":" + lower(peer));
    if (w == waiters_.end()) return;  // the joiner gave up or timed out: a late answer is dropped
    const auto it = conns_.find(w->second);
    if (it == conns_.end()) return;
    statAnswered_++;
    respond(*it->second, 200, "{\"data\":" + jq(data) + "}");
}

// ---- main loop ---------------------------------------------------------------------------------------
int Server::run() {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    hints.ai_family = AF_UNSPEC;
    addrinfo* ai = nullptr;
    const std::string bindTo = o_.bind.empty() ? "0.0.0.0" : o_.bind;
    if (getaddrinfo(bindTo.c_str(), std::to_string(o_.port).c_str(), &hints, &ai) != 0 || !ai) {
        std::fprintf(stderr, "cf_lobby: bad --bind address %s\n", bindTo.c_str());
        return 1;
    }
    listener_ = socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
    bool ok = listener_ != kNoSock;
    if (ok) {
        int one = 1, zero = 0;
#ifndef _WIN32
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#endif
        if (ai->ai_family == AF_INET6)  // --bind :: also takes IPv4
            setsockopt(listener_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero), sizeof zero);
        (void)one;
        ok = bind(listener_, ai->ai_addr, socklen_t(ai->ai_addrlen)) == 0 && listen(listener_, 128) == 0;
    }
    freeaddrinfo(ai);
    if (!ok) {
        std::fprintf(stderr, "cf_lobby: cannot listen on %s port %d (in use?)\n", bindTo.c_str(), o_.port);
        if (listener_ != kNoSock) closeSock(listener_);
        return 1;
    }
    setNonBlocking(listener_);
    sockaddr_storage ss{};
    socklen_t slen = sizeof ss;
    getsockname(listener_, reinterpret_cast<sockaddr*>(&ss), &slen);
    const int port = ss.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port)
                                             : ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    std::printf("cf_lobby: listening on %s port %d%s\n", bindTo.c_str(), port, turnConfigured() ? " (relay configured)" : "");
    std::fflush(stdout);
    gLobbyPort = port;

    auto lastStats = Clock::now(), lastPrune = lastStats;
    std::vector<pollfd_t> fds;
    std::vector<uint64_t> seqs;
    while (!gLobbyStop.load()) {
        fds.clear();
        seqs.clear();
        pollfd_t l{};
        l.fd = listener_;
        l.events = POLLRDNORM;
        fds.push_back(l);
        seqs.push_back(0);
        for (auto& kv : conns_) {
            pollfd_t p{};
            p.fd = kv.second->fd;
            p.events = kv.second->peerEof ? 0 : POLLRDNORM;
            if (!kv.second->out.empty()) p.events |= POLLWRNORM;
            fds.push_back(p);
            seqs.push_back(kv.first);
        }
        const int n = pollFds(fds.data(), fds.size(), 50);
        if (n > 0) {
            if (fds[0].revents) acceptAll();
            for (size_t i = 1; i < fds.size(); i++) {
                if (!fds[i].revents) continue;
                const auto it = conns_.find(seqs[i]);
                if (it == conns_.end()) continue;
                Conn& c = *it->second;
                if (fds[i].revents & POLLNVAL) c.dead = true;
                else if (fds[i].revents & (POLLRDNORM | POLLIN | POLLHUP | POLLERR)) onReadable(c);
            }
        }
        timers();
        // Send what this round queued (a host's socket gets its signals from other connections), drop the dead.
        std::vector<uint64_t> dead;
        for (auto& kv : conns_) {
            if (!kv.second->dead && (!kv.second->out.empty() || kv.second->st == St::Drain)) flush(*kv.second);
            if (kv.second->dead) dead.push_back(kv.first);
        }
        for (const uint64_t seq : dead) remove(seq);
        // remove() can answer waiting joiners (their room went away): those answers go out next round.

        const auto now = Clock::now();
        if (now - lastPrune > Ms(60000)) {
            lastPrune = now;
            for (auto it = signalRate_.begin(); it != signalRate_.end();)
                it = now - it->second.start >= Ms(60000) ? signalRate_.erase(it) : std::next(it);
            for (auto it = turnRate_.begin(); it != turnRate_.end();)
                it = now - it->second.start >= Ms(3600000) ? turnRate_.erase(it) : std::next(it);
        }
        if (o_.statsS > 0 && now - lastStats > std::chrono::seconds(o_.statsS)) {
            lastStats = now;
            std::printf("cf_lobby: %d rooms, %d host sockets, %zu connections; since start: %llu rooms registered, "
                        "%llu requests, %llu signals (%llu answered)\n",
                        roomCount(), wsCount_, conns_.size(), (unsigned long long)statRegistered_,
                        (unsigned long long)statRequests_, (unsigned long long)statSignals_, (unsigned long long)statAnswered_);
            std::fflush(stdout);
        }
    }
    gLobbyPort = 0;
    for (auto& kv : conns_) closeSock(kv.second->fd);
    conns_.clear();
    closeSock(listener_);
    return 0;
}

bool parseOptions(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        auto number = [&](int& out) {
            std::string s;
            if (!value(s)) return false;
            out = std::atoi(s.c_str());
            return true;
        };
        bool ok = true;
        if (a == "--port") ok = number(o.port);
        else if (a == "--bind") ok = value(o.bind);
        else if (a == "--turn") ok = value(o.turn);
        else if (a == "--turn-user") ok = value(o.turnUser);
        else if (a == "--turn-pass") ok = value(o.turnPass);
        else if (a == "--max-rooms") ok = number(o.maxRooms);
        else if (a == "--rooms-per-ip") ok = number(o.roomsPerIp);
        else if (a == "--trust-proxy") o.trustProxy = true;
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--signal-timeout-ms") ok = number(o.signalTimeoutMs);
        else if (a == "--stale-ms") ok = number(o.staleMs);
        else if (a == "--http-timeout-ms") ok = number(o.httpTimeoutMs);
        else if (a == "--signals-per-min") ok = number(o.signalsPerMin);
        else if (a == "--turns-per-hour") ok = number(o.turnsPerHour);
        else if (a == "--stats-s") ok = number(o.statsS);
        else ok = false;
        if (!ok) return false;
    }
    return o.port >= 0 && o.port < 65536 && o.maxRooms > 0 && o.roomsPerIp > 0;
}

}  // namespace

#ifdef CF_LOBBY_AS_LIB
int lobbyMain(int argc, char** argv) {  // tools/tests/lobby_api_test.cpp runs it in a thread
#else
static void onSignal(int) { gLobbyStop = true; }
int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#endif
    Options o;
    if (!parseOptions(argc, argv, o)) {
        std::fprintf(stderr,
                     "usage: cf_lobby [--port 29920] [--bind <ip>] [--turn <host:port> --turn-user <u> --turn-pass <p>]\n"
                     "                [--max-rooms 500] [--rooms-per-ip 4] [--trust-proxy] [--verbose]\n");
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    int rc;
    {
        Server server(o);
        rc = server.run();
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
