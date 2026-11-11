// Lobby API test (docs/ONLINE.md "The lobby"): the self-hosted lobby (tools/lobby/cf_lobby.cpp, built in
// here with CF_LOBBY_AS_LIB) on a loopback port of the OS's choice, driven through the game's own client
// code (port::http::request / WebSocket) and, for malformed input, plain sockets. No internet.
// Run: cf_lobby_api_test   (CF_LOBBY_TEST_IDLE=<seconds> adds an idle host socket check, e.g. 45)
//
// External mode: CF_LOBBY_TEST_URL=http://127.0.0.1:8787 runs the checks against a lobby that is already
// running there instead (the Cloudflare Worker under `wrangler dev`: tools/lobby_worker, `npm run dev`).
// Not run then, because they need the stand-in's own options or test its own HTTP parser: malformed HTTP
// and most malformed-frame checks, the shortened stale time, --max-rooms, --trust-proxy, static relay
// credentials, stopping the lobby. Adapted: lists are read after the Worker's 5 s list cache, the signal
// wait is the real 8 s, the signal and relay rate limits are checked last (they stay used up for a while).
//   CF_LOBBY_TEST_TURN_FAKE=http://127.0.0.1:8788  the lobby was started with relay secrets that point at
//       tools/lobby_worker/test/fake_turn.mjs there: checks POST /v1/turn's conversion (else: no_turn)
//   CF_LOBBY_TEST_SLOW=1  adds the real 120 s stale check: a silent host is closed, one that only sends
//       "ping" (answered without waking the Durable Object, which hibernates meanwhile) keeps its room
#define _CRT_SECURE_NO_WARNINGS  // getenv
#include "port/http.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
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
#include <csignal>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using port::http::Json;
using port::http::jsonQuote;
using port::http::parseJson;
using port::http::Result;
using port::http::WebSocket;

int lobbyMain(int argc, char** argv);  // tools/lobby/cf_lobby.cpp (CF_LOBBY_AS_LIB)
extern std::atomic<int> gLobbyPort;
extern std::atomic<bool> gLobbyStop;

static int sFails = 0;
#define CHECK(cond, ...)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                   \
            std::printf("\n");                                          \
            sFails++;                                                   \
        }                                                               \
    } while (0)

using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static bool waitFor(const std::function<bool()>& f, double seconds) {
    const auto t0 = Clock::now();
    while (since(t0) < seconds) {
        if (f()) return true;
        sleepMs(20);
    }
    return f();
}

// ---- the lobby under test -------------------------------------------------------------------------
struct Lobby {
    std::thread th;
    std::string base;
    int port = 0;
    int rc = -1;

    bool start(const std::vector<std::string>& extra) {
        gLobbyStop = false;
        std::vector<std::string> args = {"cf_lobby", "--bind", "127.0.0.1", "--port", "0"};
        args.insert(args.end(), extra.begin(), extra.end());
        th = std::thread([this, args] {
            std::vector<char*> argv;
            for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            rc = lobbyMain(int(argv.size()), argv.data());
        });
        if (!waitFor([] { return gLobbyPort.load() != 0; }, 5)) return false;
        port = gLobbyPort.load();
        base = "http://127.0.0.1:" + std::to_string(port);
        return true;
    }
    double stop() {  // seconds until lobbyMain returned
        const auto t0 = Clock::now();
        gLobbyStop = true;
        if (th.joinable()) th.join();
        return since(t0);
    }
};
static Lobby sLobby;
static bool sExternal = false;      // CF_LOBBY_TEST_URL: a lobby somebody else started
static double sSignalWait = 1.5;    // how long a signal waits for the host (shortened in the stand-in)
static bool sRelay = false;         // the lobby under test has relay credentials (external mode with the fake TURN API)
static std::string sTurnFake;       // CF_LOBBY_TEST_TURN_FAKE
// External mode: the Worker's list is up to 5 s old. Called before a list or room count is read.
static void syncList() {
    if (sExternal) std::this_thread::sleep_for(std::chrono::milliseconds(5400));
}

static Result GET(const std::string& path, int timeoutMs = 5000) { return port::http::request("GET", sLobby.base + path, {}, {}, timeoutMs); }
static Result POST(const std::string& path, const std::string& body, int timeoutMs = 10000) {
    return port::http::request("POST", sLobby.base + path, body, "application/json", timeoutMs);
}
static Json J(const Result& r) {
    Json j;
    if (!parseJson(r.body, j)) j = Json();
    return j;
}
// The answer is this error: status, code and a message in plain words.
static bool isError(const Result& r, int status, const char* code) {
    const Json j = J(r);
    return r.error.empty() && r.status == status && j["error"].str() == code && !j["message"].str().empty();
}

static bool wsOpen(WebSocket& ws, const std::string& path = "/v1/host?v=1", int* status = nullptr, std::string* err = nullptr) {
    std::string e;
    const bool ok = ws.connect(sLobby.base + path, 5000, &e, status);  // http:// is ws:// to the WebSocket class
    if (err) *err = e;
    return ok;
}
static Json wsJson(WebSocket& ws, int timeoutMs = 3000) {
    std::string text;
    Json j;
    if (ws.recv(text, timeoutMs) != WebSocket::Recv::Message || !parseJson(text, j)) return Json();
    return j;
}

struct RoomSpec {
    std::string name = "Maple Room", host = "Rob", town = "Sunny";
    int players = 1, max = 4;
    bool lock = false, friends = false;
    std::string game = "RUUE01", ver = "1.2.0", tag = "0123456789abcdef";
};
static std::string regMessage(const RoomSpec& r) {
    return "{\"t\":\"reg\",\"room\":{\"name\":" + jsonQuote(r.name) + ",\"host\":" + jsonQuote(r.host) + ",\"town\":" + jsonQuote(r.town) +
           ",\"players\":" + std::to_string(r.players) + ",\"max\":" + std::to_string(r.max) + ",\"lock\":" + (r.lock ? "true" : "false") +
           ",\"friends\":" + (r.friends ? "true" : "false") + ",\"game\":" + jsonQuote(r.game) + ",\"ver\":" + jsonQuote(r.ver) +
           ",\"tag\":" + jsonQuote(r.tag) + "}}";
}
// Sends a registration; the room id from reg_ok, or "" (lastError = the error code).
static std::string sLastError;
static std::string reg(WebSocket& ws, const std::string& message) {
    sLastError.clear();
    if (!ws.sendText(message)) {
        sLastError = "send failed";
        return {};
    }
    const Json j = wsJson(ws);
    if (j["t"].str() == "reg_ok") return j["id"].str();
    sLastError = j["t"].str() == "error" ? j["code"].str() : "no answer";
    return {};
}
static std::string reg(WebSocket& ws, const RoomSpec& r) { return reg(ws, regMessage(r)); }
// A registration with this text as the room object's inside: the error code it gets ("" = accepted).
static std::string regError(WebSocket& ws, const std::string& roomFields) {
    reg(ws, "{\"t\":\"reg\",\"room\":{" + roomFields + "}}");
    return sLastError;
}

static bool isHex(const std::string& s, size_t n) {
    return s.size() == n && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
// The listed room with this id (Null when not listed).
static Json listed(const std::string& id, const std::string& query = "") {
    const Json j = J(GET("/v1/rooms?v=1" + query));
    for (const Json& r : j["rooms"].a)
        if (r["id"].str() == id) return r;
    return Json();
}
static std::string signalBody(const std::string& room, const std::string& peer, const std::string& data) {
    return "{\"v\":1,\"room\":" + jsonQuote(room) + ",\"peer\":" + jsonQuote(peer) + ",\"data\":" + jsonQuote(data) + "}";
}

// ---- plain sockets (malformed input) ---------------------------------------------------------------
#ifdef _WIN32
using sock_t = SOCKET;
static const sock_t kNoSock = INVALID_SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
static const sock_t kNoSock = -1;
static void closeSock(sock_t s) { close(s); }
#endif

struct Raw {
    sock_t s = kNoSock;
    bool eof = false;

    bool open() {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kNoSock) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(sLobby.port));
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            closeSock(s);
            s = kNoSock;
            return false;
        }
        return true;
    }
    void write(const std::string& data) {
        size_t done = 0;
        while (done < data.size()) {
#ifdef MSG_NOSIGNAL
            const int flags = MSG_NOSIGNAL;
#else
            const int flags = 0;
#endif
            const int n = int(send(s, data.data() + done, int(data.size() - done), flags));
            if (n <= 0) return;  // the server may have answered and closed already
            done += size_t(n);
        }
    }
    // Reads until `until` returns true for what came so far, the peer closes, or the time is up.
    std::string read(double seconds, const std::function<bool(const std::string&)>& until = nullptr) {
        std::string got;
        const auto t0 = Clock::now();
        while (since(t0) < seconds) {
            if (until && until(got)) break;
#ifdef _WIN32
            WSAPOLLFD p{};
            p.fd = s;
            p.events = POLLRDNORM;
            const int n = WSAPoll(&p, 1, 50);
#else
            struct pollfd p {};
            p.fd = s;
            p.events = POLLIN;
            const int n = poll(&p, 1, 50);
#endif
            if (n <= 0) continue;
            char buf[4096];
            const int r = int(recv(s, buf, sizeof buf, 0));
            if (r <= 0) {
                eof = true;
                break;
            }
            got.append(buf, size_t(r));
        }
        return got;
    }
    void close() {
        if (s != kNoSock) closeSock(s);
        s = kNoSock;
    }
    ~Raw() { close(); }
};

// A client WebSocket frame.
static std::string frame(int opcode, const std::string& payload, bool masked = true, bool fin = true) {
    std::string f;
    f += char((fin ? 0x80 : 0) | opcode);
    const size_t n = payload.size();
    const int m = masked ? 0x80 : 0;
    if (n < 126) {
        f += char(m | int(n));
    } else {
        f += char(m | 126);
        f += char(n >> 8);
        f += char(n & 0xFF);
    }
    if (!masked) return f + payload;
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    f.append(mask, 4);
    for (size_t i = 0; i < n; i++) f += char(payload[i] ^ mask[i & 3]);
    return f;
}
// The WebSocket handshake on a plain socket, with RFC 6455's example key and its known answer.
static bool rawUpgrade(Raw& r) {
    if (!r.open()) return false;
    r.write("GET /v1/host?v=1 HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    const std::string head = r.read(3, [](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; });
    return head.rfind("HTTP/1.1 101", 0) == 0 && head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos;
}
// The close status the server sends after `bytes` on a fresh WebSocket (0: none came).
static int rawCloseCode(const std::string& bytes) {
    Raw r;
    if (!rawUpgrade(r)) return -1;
    r.write(bytes);
    const std::string got = r.read(3, [](const std::string& s) { return s.size() >= 4; });
    // (the status, and for the Worker a few words of reason after it)
    if (got.size() >= 4 && uint8_t(got[0]) == 0x88 && got[1] >= 2 && got[1] <= 125) return int(uint8_t(got[2])) << 8 | uint8_t(got[3]);
    return 0;
}

// ---- the tests -----------------------------------------------------------------------------------------
static void testBasics() {
    std::printf("basics: info, version, paths\n");
    syncList();  // (rooms of an earlier run are gone from the list by now)
    Result r = GET("/v1/info?v=1");
    Json j = J(r);
    CHECK(r.ok(), "status %d %s", r.status, r.error.c_str());
    CHECK(j["v"].num(-1) == 1 && j["name"].str() == "cf-lobby" && j["rooms"].num(-1) == 0, "%s", r.body.c_str());
    CHECK(j["turn"].type == Json::Bool && j["turn"].b == sRelay, "%s", r.body.c_str());
    r = GET("/v1/rooms?v=1");
    j = J(r);
    CHECK(r.ok() && j["v"].num(-1) == 1 && j["rooms"].type == Json::Array && j["rooms"].a.empty() && j["turn"].type == Json::Bool,
          "%d %s", r.status, r.body.c_str());

    // Any other protocol version: 426 with the message the client shows.
    for (const char* path : {"/v1/info", "/v1/info?v=2", "/v1/info?v=1x", "/v1/rooms?v=0", "/v1/rooms?game=RUUE01", "/v1/host?v=2"}) {
        r = GET(path);
        CHECK(isError(r, 426, "version"), "%s: %d %s", path, r.status, r.body.c_str());
        CHECK(J(r)["message"].str() == "This lobby needs a newer City Folk Recompiled", "%s", r.body.c_str());
    }
    r = POST("/v1/signal", "{\"v\":2,\"room\":\"0123456789abcdef\",\"peer\":\"01234567\",\"data\":\"QUJD\"}");
    CHECK(isError(r, 426, "version"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/signal", "{\"room\":\"0123456789abcdef\",\"peer\":\"01234567\",\"data\":\"QUJD\"}");
    CHECK(isError(r, 426, "version"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/turn", "{\"v\":\"1\",\"room\":\"0123456789abcdef\"}");
    CHECK(isError(r, 426, "version"), "%d %s", r.status, r.body.c_str());
    {
        WebSocket ws;
        int status = 0;
        std::string err;
        CHECK(!wsOpen(ws, "/v1/host?v=3", &status, &err), "connected to a wrong version");
        CHECK(status == 426 && err.find("newer City Folk Recompiled") != std::string::npos, "status %d: %s", status, err.c_str());
        CHECK(!ws.isOpen(), "open after a refused connect");
    }

    // Paths and methods.
    for (const char* path : {"/", "/v1", "/v1/nope?v=1", "/v1/rooms/x?v=1", "/v2/info?v=1"}) {
        r = GET(path);
        CHECK(isError(r, 404, "not_found"), "%s: %d %s", path, r.status, r.body.c_str());
    }
    r = POST("/v1/nope", "{}");
    CHECK(isError(r, 404, "not_found"), "%d %s", r.status, r.body.c_str());
    r = GET("/v1/signal?v=1");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/rooms?v=1", "{}");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = port::http::request("DELETE", sLobby.base + "/v1/info?v=1");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = GET("/v1/host?v=1");  // not a WebSocket upgrade
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = GET("/v1/rooms?v=1&game=ruue01");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    // No relay configured in this run (with one: testRelayExternal).
    if (!sRelay) {
        r = POST("/v1/turn", "{\"v\":1,\"room\":\"0123456789abcdef\"}");
        CHECK(isError(r, 404, "no_turn"), "%d %s", r.status, r.body.c_str());
    }
}

static void testBadRequests() {
    std::printf("bad requests\n");
    const std::string room = "0123456789abcdef", peer = "89abcdef";
    const char* bad[] = {
        "",
        "not json",
        "[1,2]",
        "\"text\"",
        "{\"v\":1",
        "{\"v\":1,}",
        "{\"v\":1,\"room\":\"0123456789abcdef\",\"peer\":\"89abcdef\"}",                      // no data
        "{\"v\":1,\"room\":\"0123456789abcdef\",\"data\":\"QUJD\"}",                          // no peer
        "{\"v\":1,\"peer\":\"89abcdef\",\"data\":\"QUJD\"}",                                  // no room
        "{\"v\":1,\"room\":\"0123456789abcde\",\"peer\":\"89abcdef\",\"data\":\"QUJD\"}",     // 15 hex
        "{\"v\":1,\"room\":\"0123456789abcdeg\",\"peer\":\"89abcdef\",\"data\":\"QUJD\"}",    // not hex
        "{\"v\":1,\"room\":1234567890123456,\"peer\":\"89abcdef\",\"data\":\"QUJD\"}",        // not a string
        "{\"v\":1,\"room\":\"0123456789abcdef\",\"peer\":\"89abcde\",\"data\":\"QUJD\"}",     // 7 hex
        "{\"v\":1,\"room\":\"0123456789abcdef\",\"peer\":\"89abcdex\",\"data\":\"QUJD\"}",
        "{\"v\":1,\"room\":\"0123456789abcdef\",\"peer\":\"89abcdef\",\"data\":\"a b\"}",     // not base64
        "{\"v\":1,\"room\":\"0123456789abcdef\",\"peer\":\"89abcdef\",\"data\":\"<script>\"}",
        "{\"v\":1,\"room\":\"0123456789abcdef\",\"peer\":\"89abcdef\",\"data\":7}",
    };
    for (const char* body : bad) {
        const Result r = POST("/v1/signal", body);
        CHECK(isError(r, 400, "bad_request"), "body %s: %d %s", body, r.status, r.body.c_str());
    }
    // The data limit: 2800 characters pass (the room does not exist: 404), 2801 do not.
    Result r = POST("/v1/signal", signalBody(room, peer, std::string(2800, 'A')));
    CHECK(isError(r, 404, "no_room"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/signal", signalBody(room, peer, std::string(2801, 'A')));
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    // The body limit: 8192 bytes pass, more do not (whatever is in them).
    std::string body = signalBody(room, peer, "QUJD");
    body += std::string(8192 - body.size(), ' ');
    r = POST("/v1/signal", body);
    CHECK(isError(r, 404, "no_room"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/signal", body + " ");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/signal", std::string(200000, 'x'));
    CHECK(isError(r, 400, "bad_request"), "%d %s %s", r.status, r.body.c_str(), r.error.c_str());
    r = POST("/v1/turn", "{\"v\":1}");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/turn", "{\"v\":1,\"room\":\"xyz\"}");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/turn", "nope");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    // A signal to a room nobody hosts.
    r = POST("/v1/signal", signalBody("ffffffffffffffff", peer, "QUJD"));
    CHECK(isError(r, 404, "no_room"), "%d %s", r.status, r.body.c_str());
}

static void testRawHttp() {
    std::printf("malformed HTTP on plain sockets\n");
#define RAW_OPEN(r) Raw r; CHECK(r.open(), "connect")
    {  // garbage
        RAW_OPEN(r);
        r.write(std::string("\x01\x02\xff\xfe garbage \x00 more\r\n\r\n", 23));
        const std::string got = r.read(3);
        CHECK(got.rfind("HTTP/1.1 400", 0) == 0 && r.eof, "%s", got.c_str());
    }
    {  // a header that never ends
        RAW_OPEN(r);
        r.write("GET /v1/info?v=1 HTTP/1.1\r\nX-Big: " + std::string(100000, 'a'));
        const std::string got = r.read(4);
        CHECK(got.rfind("HTTP/1.1 400", 0) == 0 && got.find("bad_request") != std::string::npos, "%s", got.c_str());
        CHECK(r.eof, "the connection stayed open");
    }
    {  // a complete but oversized header
        RAW_OPEN(r);
        r.write("GET /v1/info?v=1 HTTP/1.1\r\nX-Big: " + std::string(9000, 'a') + "\r\n\r\n");
        const std::string got = r.read(3);
        CHECK(got.rfind("HTTP/1.1 400", 0) == 0, "%s", got.c_str());
    }
    {  // a body announced larger than allowed: refused before it is sent
        RAW_OPEN(r);
        r.write("POST /v1/signal HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 500000\r\n\r\n{");
        const std::string got = r.read(3);
        CHECK(got.rfind("HTTP/1.1 400", 0) == 0, "%s", got.c_str());
    }
    for (const char* req : {"GET /v1/info?v=1\r\n\r\n", "GET\r\n\r\n", "get /v1/info?v=1 HTTP/1.1\r\n\r\n", "GET v1/info HTTP/1.1\r\n\r\n",
                            "GET /v1/info?v=1 HTTP/1.1\r\nNoColonHere\r\n\r\n", "POST /v1/signal HTTP/1.1\r\nContent-Length: -5\r\n\r\n",
                            "POST /v1/signal HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n",
                            "POST /v1/signal HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"}) {
        RAW_OPEN(r);
        r.write(req);
        const std::string got = r.read(3);
        CHECK(got.rfind("HTTP/1.1 400", 0) == 0 && r.eof, "%s -> %s", req, got.c_str());
    }
    {  // a well-formed request on a plain socket, for comparison (and the response headers)
        RAW_OPEN(r);
        r.write("GET /v1/info?v=1 HTTP/1.1\r\nHost: x\r\n\r\n");
        const std::string got = r.read(3);
        CHECK(got.rfind("HTTP/1.1 200", 0) == 0 && got.find("Content-Type: application/json\r\n") != std::string::npos &&
                  got.find("\"name\":\"cf-lobby\"") != std::string::npos && r.eof,
              "%s", got.c_str());
    }
    {  // connections that open and say nothing, half a request, half a body: closed after the request timeout,
       // and everything else is served meanwhile
        Raw silent, half, body, gone;
        CHECK(silent.open() && half.open() && body.open() && gone.open(), "connect");
        half.write("GET /v1/in");
        body.write("POST /v1/signal HTTP/1.1\r\nContent-Length: 100\r\n\r\n{\"v\":1,");
        gone.write("GET /v1/rooms?v=1 HTT");
        gone.close();
        const auto t0 = Clock::now();
        const Result r = GET("/v1/info?v=1");
        CHECK(r.ok() && since(t0) < 1.0, "%d after %.2f s", r.status, since(t0));
        const std::string a = silent.read(4), b = half.read(4), c = body.read(4);
        CHECK(silent.eof && half.eof && body.eof, "half-open connections were kept (%d %d %d)", silent.eof, half.eof, body.eof);
        CHECK(a.empty() && b.empty() && c.empty(), "unexpected answers");
    }
#undef RAW_OPEN

    std::printf("malformed WebSocket frames on plain sockets\n");
    CHECK(rawCloseCode(frame(1, "{\"t\":\"x\"}", false)) == 1002, "unmasked frame");
    CHECK(rawCloseCode(frame(1, "{\"t\":", true, false)) == 1003, "fragmented message");
    CHECK(rawCloseCode(frame(0, "tail")) == 1003, "continuation frame");
    CHECK(rawCloseCode(frame(2, "binary")) == 1003, "binary frame");
    CHECK(rawCloseCode(frame(3, "reserved")) == 1002, "reserved opcode");
    CHECK(rawCloseCode(std::string("\xC1\x80", 2)) == 1002, "reserved bit");
    CHECK(rawCloseCode(frame(9, std::string(126, 'p'))) == 1002, "long ping");
    CHECK(rawCloseCode(frame(1, std::string(4097, 'x'))) == 1009, "4097 bytes");
    CHECK(rawCloseCode(std::string("\x81\xFF\x7F\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 10)) == 1009, "a frame of 2^63 bytes");
    CHECK(rawCloseCode(frame(1, "\xC3\x28 not UTF-8")) == 1007, "invalid UTF-8");
    CHECK(rawCloseCode(frame(8, std::string("\x03\xE9", 2))) == 1001, "close 1001 is echoed");
    CHECK(rawCloseCode(frame(8, "")) == 1000, "close without a status");
    {  // protocol ping -> pong with the same payload; a 4096-byte message is still fine; the text ping
        Raw r;
        CHECK(rawUpgrade(r), "upgrade");
        r.write(frame(9, "abc"));
        std::string got = r.read(3, [](const std::string& s) { return s.size() >= 5; });
        CHECK(got == std::string("\x8A\x03", 2) + "abc", "pong: %zu bytes", got.size());
        r.write(frame(1, "{\"t\":\"x\",\"pad\":\"" + std::string(4096 - 18, 'p') + "\"}"));
        got = r.read(3, [](const std::string& s) { return s.find("}") != std::string::npos; });
        CHECK(got.find("\"code\":\"bad_request\"") != std::string::npos, "%s", got.c_str());
        r.write(frame(1, "ping"));
        got = r.read(3, [](const std::string& s) { return s.size() >= 6; });
        CHECK(got == std::string("\x81\x04", 2) + "pong", "text pong: %s", got.c_str());
        // A host that vanishes without a close frame: its room goes at once.
        RoomSpec spec;
        spec.name = "Vanishing";
        r.write(frame(1, regMessage(spec)));
        got = r.read(3, [](const std::string& s) { return s.find("}") != std::string::npos; });
        const size_t at = got.find("\"id\":\"");
        const std::string id = at == std::string::npos ? "" : got.substr(at + 6, 16);
        CHECK(isHex(id, 16) && listed(id)["name"].str() == "Vanishing", "%s", got.c_str());
        r.close();
        CHECK(waitFor([&] { return listed(id).isNull(); }, 2), "the room of a dropped connection stayed");
    }
    const Result r = GET("/v1/info?v=1");
    CHECK(r.ok() && J(r)["rooms"].num(-1) == 0, "%d %s", r.status, r.body.c_str());
}

// External mode: the plain-socket checks that are about the lobby, not about cf_lobby's own HTTP and frame
// parser (the Worker's runtime does that part itself).
static void testRawExternal() {
    std::printf("plain sockets: upgrade, keep-alive, close codes, a host that vanishes, list headers\n");
    {
        Raw r;
        CHECK(r.open(), "connect");
        r.write("GET /v1/rooms?v=1 HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(sLobby.port) + "\r\nConnection: close\r\n\r\n");
        std::string got = r.read(4, [](const std::string& s) { return s.find("\"rooms\"") != std::string::npos; });
        for (auto& ch : got) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        CHECK(got.rfind("http/1.1 200", 0) == 0 && got.find("content-type: application/json\r\n") != std::string::npos &&
                  got.find("cache-control: public, max-age=5\r\n") != std::string::npos,
              "%s", got.c_str());
    }
    CHECK(rawCloseCode(frame(2, "binary")) == 1003, "binary frame");
    CHECK(rawCloseCode(frame(1, std::string(4097, 'x'))) == 1009, "4097 bytes");
    Raw r;
    CHECK(rawUpgrade(r), "upgrade (the accept key of RFC 6455's example)");
    r.write(frame(1, "{\"t\":\"x\",\"pad\":\"" + std::string(4096 - 18, 'p') + "\"}"));  // 4096 bytes are fine
    std::string got = r.read(4, [](const std::string& s) { return s.find("}") != std::string::npos; });
    CHECK(got.find("\"code\":\"bad_request\"") != std::string::npos, "%s", got.c_str());
    r.write(frame(1, "ping"));
    got = r.read(4, [](const std::string& s) { return s.size() >= 6; });
    CHECK(got == std::string("\x81\x04", 2) + "pong", "text pong: %s", got.c_str());
    // A host that vanishes without a close frame: its room goes (from the list: within its 5 s).
    RoomSpec spec;
    spec.name = "Vanishing";
    r.write(frame(1, regMessage(spec)));
    got = r.read(4, [](const std::string& s) { return s.find("}") != std::string::npos; });
    const size_t at = got.find("\"id\":\"");
    const std::string id = at == std::string::npos ? "" : got.substr(at + 6, 16);
    CHECK(isHex(id, 16), "%s", got.c_str());
    syncList();
    CHECK(listed(id)["name"].str() == "Vanishing", "the room is not listed");
    r.close();
    sleepMs(500);
    // At once for a joiner (the Durable Object knows), within the list's 5 s for the list.
    CHECK(waitFor([&] { return isError(POST("/v1/signal", signalBody(id, "00000001", "QUJD")), 404, "no_room"); }, 3),
          "a signal still reaches the room of a dropped connection");
    syncList();
    CHECK(listed(id).isNull(), "the room of a dropped connection stayed");
}

static void testRooms() {
    std::printf("rooms: register, list, update, friends, filter, limits\n");
    WebSocket a, f, p, s;
    CHECK(wsOpen(a), "connect");
    CHECK(a.isOpen(), "not open");
    RoomSpec specA;
    const std::string idA = reg(a, specA);
    CHECK(isHex(idA, 16), "reg: %s (%s)", idA.c_str(), sLastError.c_str());
    syncList();
    {
        const Result r = GET("/v1/rooms?v=1");
        const Json j = J(r);
        CHECK(r.ok() && j["rooms"].a.size() == 1, "%d %s", r.status, r.body.c_str());
        const Json room = listed(idA);
        CHECK(room["id"].str() == idA && room["name"].str() == "Maple Room" && room["host"].str() == "Rob" && room["town"].str() == "Sunny",
              "%s", r.body.c_str());
        CHECK(room["players"].num(-1) == 1 && room["max"].num(-1) == 4, "%s", r.body.c_str());
        CHECK(room["lock"].type == Json::Bool && !room["lock"].b && room["friends"].type == Json::Bool && !room["friends"].b, "%s",
              r.body.c_str());
        CHECK(room["game"].str() == "RUUE01" && room["ver"].str() == "1.2.0" && room["tag"].str() == "0123456789abcdef", "%s", r.body.c_str());
        CHECK(room["age"].type == Json::Number && room["age"].num(-1) >= 0 && room["age"].num(-1) <= (sExternal ? 12 : 5), "%s", r.body.c_str());
        if (!sExternal) {
            CHECK(room["ping"].isNull(), "cf_lobby has no ping estimate: %s", r.body.c_str());
        } else {
            // The Worker: an estimate in steps of 10 ms when both places are known (locally both are the same
            // placeholder place: 20), else no field.
            const double ping = room["ping"].num(-1);
            CHECK(room["ping"].isNull() || (ping >= 20 && ping == double(int(ping / 10) * 10)), "ping: %s", r.body.c_str());
            std::printf("  ping estimate: %s\n", room["ping"].isNull() ? "none" : std::to_string(int(ping)).c_str());
        }
        CHECK(J(GET("/v1/info?v=1"))["rooms"].num(-1) == 1, "info count");
    }
    // An update: same id, the new values.
    specA.players = 2;
    specA.lock = true;
    specA.name = "Maple Room II";
    specA.max = 3;
    CHECK(reg(a, specA) == idA, "the id changed on an update (%s)", sLastError.c_str());
    if (!sExternal) {
        const Json room = listed(idA);
        CHECK(room["players"].num(-1) == 2 && room["max"].num(-1) == 3 && room["lock"].b && room["name"].str() == "Maple Room II", "update");
    }
    // A refused update keeps the room as it was.
    specA.players = 4;  // > max 3
    CHECK(reg(a, specA).empty() && sLastError == "bad_request", "%s", sLastError.c_str());
    syncList();
    {
        const Json room = listed(idA);
        CHECK(room["players"].num(-1) == 2 && room["max"].num(-1) == 3 && room["lock"].b && room["name"].str() == "Maple Room II",
              "the update, then a refused update");
        CHECK(J(GET("/v1/rooms?v=1"))["rooms"].a.size() == 1, "an update made a second room");
    }

    // Friends only: listed without names, with its tag.
    CHECK(wsOpen(f), "connect");
    RoomSpec specF;
    specF.name = "Secret";
    specF.host = "Friend";
    specF.town = "Hidden";
    specF.friends = true;
    specF.tag = "FEDCBA9876543210";
    const std::string idF = reg(f, specF);
    CHECK(isHex(idF, 16) && idF != idA, "reg: %s", sLastError.c_str());
    // Another game (registered a moment later: the list is ordered by registration time).
    sleepMs(20);
    CHECK(wsOpen(p), "connect");
    RoomSpec specP;
    specP.name = "Europe";
    specP.game = "RUUP01";
    const std::string idP = reg(p, specP);
    CHECK(isHex(idP, 16), "reg: %s", sLastError.c_str());
    syncList();
    {
        const Json room = listed(idF);
        CHECK(room["id"].str() == idF && room["friends"].b, "friends room not listed");
        CHECK(room["name"].type == Json::String && room["name"].s.empty() && room["host"].type == Json::String && room["host"].s.empty() &&
                  room["town"].type == Json::String && room["town"].s.empty(),
              "friends-only names are listed");
        CHECK(room["tag"].str() == "FEDCBA9876543210" && room["game"].str() == "RUUE01" && room["players"].num(-1) == 1, "friends fields");
        const Result r = GET("/v1/rooms?v=1");
        CHECK(r.body.find("Secret") == std::string::npos && r.body.find("Hidden") == std::string::npos, "%s", r.body.c_str());
    }
    // The filter; newest first.
    {
        const Json all = J(GET("/v1/rooms?v=1"));
        CHECK(all["rooms"].a.size() == 3, "3 rooms expected");
        if (all["rooms"].a.size() == 3)
            CHECK(all["rooms"].a[0]["id"].str() == idP && all["rooms"].a[1]["id"].str() == idF && all["rooms"].a[2]["id"].str() == idA,
                  "not newest first");
        const Json pal = J(GET("/v1/rooms?v=1&game=RUUP01"));
        CHECK(pal["rooms"].a.size() == 1 && pal["rooms"].a[0]["id"].str() == idP, "game filter");
        const Json usa = J(GET("/v1/rooms?game=RUUE01&v=1"));
        CHECK(usa["rooms"].a.size() == 2 && listed(idP, "&game=RUUE01").isNull(), "game filter");
        CHECK(J(GET("/v1/rooms?v=1&game=RUUJ01"))["rooms"].a.empty(), "game filter");
    }

    // Sanitised names: control characters out, trimmed, cut by code points; an empty name is "Room".
    CHECK(wsOpen(s), "connect");
    RoomSpec specS;
    specS.name = std::string("  \tCo\x01ol\x7f R\xC3\xB6", 13) + "om\r\n  ";
    specS.host.clear();
    for (int i = 0; i < 30; i++) specS.host += "\xF0\x9F\x98\x80";  // 30 emoji, 4 bytes each
    specS.town = "abcdefghijklmno pqrs";                            // cut after the space: trimmed again
    const std::string idS = reg(s, specS);
    CHECK(isHex(idS, 16), "reg: %s", sLastError.c_str());
    syncList();
    {
        const Json room = listed(idS);
        CHECK(room["name"].str() == "Cool R\xC3\xB6om", "name '%s'", room["name"].str().c_str());
        CHECK(room["host"].str().size() == 24 * 4 && room["host"].str().compare(0, 4, "\xF0\x9F\x98\x80") == 0, "host: %zu bytes",
              room["host"].str().size());
        CHECK(room["town"].str() == "abcdefghijklmno", "town '%s'", room["town"].str().c_str());
    }
    specS.name.clear();
    for (int i = 0; i < 40; i++) specS.name += "\xC3\xA9";
    specS.host = "\x01\x02 \x7f";
    specS.town = "0123456789abcdefXYZ";
    CHECK(reg(s, specS) == idS, "%s", sLastError.c_str());
    syncList();
    {
        const Json room = listed(idS);
        CHECK(room["name"].str().size() == 64, "name: %zu bytes", room["name"].str().size());
        CHECK(room["host"].type == Json::String && room["host"].s.empty(), "host '%s'", room["host"].str().c_str());
        CHECK(room["town"].str() == "0123456789abcdef", "town '%s'", room["town"].str().c_str());
    }
    specS.name = " \x1f ";
    CHECK(reg(s, specS) == idS, "%s", sLastError.c_str());
    syncList();
    CHECK(listed(idS)["name"].str() == "Room", "empty name: '%s'", listed(idS)["name"].str().c_str());
    // Names given as \u escapes (a pair of surrogates, a control character).
    CHECK(regError(s, "\"name\":\"\\ud83d\\ude00\\u0007 ok\",\"players\":1,\"max\":2,\"game\":\"RUUE01\",\"ver\":\"\",\"tag\":\"0123456789abcdef\"").empty(),
          "%s", sLastError.c_str());
    syncList();
    CHECK(listed(idS)["name"].str() == "\xF0\x9F\x98\x80 ok" && listed(idS)["max"].num() == 2 && listed(idS)["host"].str().empty(), "'%s'",
          listed(idS)["name"].str().c_str());

    // The fifth room from one address.
    {
        WebSocket fifth;
        CHECK(wsOpen(fifth), "connect");
        CHECK(reg(fifth, RoomSpec()).empty() && sLastError == "too_many", "fifth room: %s", sLastError.c_str());
        CHECK(J(GET("/v1/info?v=1"))["rooms"].num(-1) == 4, "room count");  // (the list is fresh: read just above)
        // The socket stays usable: once another room closes, it can register.
        p.close();
        CHECK(!p.isOpen(), "open after close");
        if (!sExternal) CHECK(listed(idP).isNull(), "a closed room is still listed");
        const std::string id5 = reg(fifth, RoomSpec());
        CHECK(isHex(id5, 16), "after a room closed: %s", sLastError.c_str());
        // More sockets than rooms are allowed, up to 8 from one address.
        WebSocket extra[4];
        for (auto& e : extra) CHECK(wsOpen(e), "connect");
        WebSocket ninth;
        int status = 0;
        std::string err;
        CHECK(!wsOpen(ninth, "/v1/host?v=1", &status, &err) && status == 429, "ninth socket: status %d %s", status, err.c_str());
        for (auto& e : extra) e.close();
        fifth.close();
        syncList();
        CHECK(listed(id5).isNull() && listed(idP).isNull(), "a closed room is still listed");
    }
    CHECK(J(GET("/v1/info?v=1"))["rooms"].num(-1) == 3, "room count");

    // Closing the host socket removes the room at once (from the Worker's list: within its 5 s).
    f.close();
    syncList();
    CHECK(listed(idF).isNull() && !listed(idA).isNull() && !listed(idS).isNull(), "after closing the friends room");
    s.close();
    a.close();
    syncList();
    CHECK(J(GET("/v1/rooms?v=1"))["rooms"].a.empty(), "rooms left");
}

static void testMessages() {
    std::printf("host socket messages\n");
    WebSocket v;
    CHECK(wsOpen(v), "connect");
    std::string text;
    // The keep-alive.
    CHECK(v.sendText("ping") && v.recv(text, 3000) == WebSocket::Recv::Message && text == "pong", "ping: '%s'", text.c_str());
    // Nonsense gets an error message and the socket stays.
    for (const char* m : {"hello", "[]", "{\"t\":1}", "{\"t\":\"nope\"}", "{}", "{\"t\":\"reg\"}", "{\"t\":\"reg\",\"room\":[]}",
                          "{\"t\":\"sig\",\"peer\":\"01234567\",\"data\":\"QUJD\"}" /* no room yet */, "{\"t\":\"sig\",\"peer\":\"xyz\",\"data\":\"QUJD\"}",
                          "{\"t\":\"reg\",\"room\":{\"name\":\"raw control \x01 character\",\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1\",\"tag\":\"0123456789abcdef\"}}"}) {
        CHECK(v.sendText(m), "send");
        const Json j = wsJson(v);
        CHECK(j["t"].str() == "error" && j["code"].str() == "bad_request" && !j["message"].str().empty(), "%s", m);
    }
    const std::string good = "\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"";
    const char* badRooms[] = {
        "\"players\":1,\"max\":4,\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",                              // no game
        "\"players\":1,\"max\":4,\"game\":\"ruue01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",          // lower case
        "\"players\":1,\"max\":4,\"game\":\"RUUE012\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
        "\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"tag\":\"0123456789abcdef\"",                            // no ver
        "\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2 beta\",\"tag\":\"0123456789abcdef\"",
        "\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1234567890123456789012345\",\"tag\":\"0123456789abcdef\"",
        "\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\"",                                       // no tag
        "\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcde\"",
        "\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdeg\"",
        "\"players\":1,\"max\":5,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
        "\"players\":1,\"max\":1,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
        "\"players\":0,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
        "\"players\":4,\"max\":3,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
        "\"players\":1.5,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
        "\"players\":\"1\",\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
        "\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",                        // no players
        "\"name\":7,\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
        "\"lock\":\"no\",\"players\":1,\"max\":4,\"game\":\"RUUE01\",\"ver\":\"1.2.0\",\"tag\":\"0123456789abcdef\"",
    };
    WebSocket w;  // a second socket: 30 messages per 10 s each
    CHECK(wsOpen(w), "connect");
    for (const char* fields : badRooms) CHECK(regError(w, fields) == "bad_request", "%s -> %s", fields, sLastError.c_str());
    syncList();
    CHECK(J(GET("/v1/info?v=1"))["rooms"].num(-1) == 0, "a refused registration made a room");
    CHECK(regError(w, good).empty(), "%s", sLastError.c_str());
    syncList();
    CHECK(J(GET("/v1/rooms?v=1"))["rooms"].a.size() == 1 && J(GET("/v1/rooms?v=1"))["rooms"].a[0]["name"].str() == "Room", "defaults");
    w.close();
    v.close();

    // A message over 4096 bytes closes the socket with 1009.
    {
        WebSocket big;
        CHECK(wsOpen(big), "connect");
        CHECK(isHex(reg(big, RoomSpec()), 16), "%s", sLastError.c_str());
        CHECK(big.sendText("{\"t\":\"reg\",\"pad\":\"" + std::string(5000, 'x') + "\"}"), "send");
        CHECK(big.recv(text, 3000) == WebSocket::Recv::Closed, "an oversized message was accepted");
        CHECK(big.closeCode() == 1009, "close code %d", big.closeCode());
        CHECK(!big.isOpen() && !big.sendText("ping"), "open after the close");
        syncList();
        CHECK(J(GET("/v1/info?v=1"))["rooms"].num(-1) == 0, "the room stayed");
    }
    // More than 30 messages in 10 s: closed with 1008. Keep-alive pings do not count.
    {
        WebSocket many;
        CHECK(wsOpen(many), "connect");
        for (int i = 0; i < 10; i++) CHECK(many.sendText("ping") && many.recv(text, 3000) == WebSocket::Recv::Message && text == "pong", "ping %d", i);
        for (int i = 0; i < 30; i++) {
            CHECK(many.sendText("{\"t\":\"nope\"}"), "send %d", i);
            CHECK(wsJson(many)["code"].str() == "bad_request", "message %d", i);
        }
        many.sendText("{\"t\":\"nope\"}");
        CHECK(many.recv(text, 3000) == WebSocket::Recv::Closed && many.closeCode() == 1008, "31st message: code %d", many.closeCode());
    }
}

static void testSignals() {
    std::printf("signals\n");
    WebSocket host;
    CHECK(wsOpen(host), "connect");
    const std::string room = reg(host, RoomSpec());
    CHECK(isHex(room, 16), "%s", sLastError.c_str());
    const std::string offer(2800, 'Q');

    // A round trip: the joiner's POST is answered by the host over its socket.
    {
        Result res;
        const auto t0 = Clock::now();
        std::thread joiner([&] { res = POST("/v1/signal", signalBody(room, "0A1b2C3d", offer)); });
        const Json m = wsJson(host);
        CHECK(m["t"].str() == "sig" && m["peer"].str() == "0A1b2C3d" && m["data"].str() == offer, "the host got: t=%s peer=%s", m["t"].str().c_str(),
              m["peer"].str().c_str());
        CHECK(host.sendText("{\"t\":\"sig\",\"peer\":\"0A1b2C3d\",\"data\":\"YW5zd2Vy+/=_-\"}"), "send");
        joiner.join();
        CHECK(res.ok() && J(res)["data"].str() == "YW5zd2Vy+/=_-", "%d %s %s", res.status, res.body.c_str(), res.error.c_str());
        CHECK(since(t0) < 1.5, "a round trip took %.2f s", since(t0));
    }
    // The room id is accepted in upper case too.
    {
        std::string upper = room;
        for (auto& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        Result res;
        std::thread joiner([&] { res = POST("/v1/signal", signalBody(upper, "00000001", "QQ==")); });
        const Json m = wsJson(host);
        CHECK(m["peer"].str() == "00000001", "upper-case room id");
        host.sendText("{\"t\":\"sig\",\"peer\":\"00000001\",\"data\":\"\"}");
        joiner.join();
        CHECK(res.ok() && J(res)["data"].type == Json::String && J(res)["data"].s.empty(), "%d %s", res.status, res.body.c_str());
    }
    // No answer: 504 after the wait (8 s; shortened in the stand-in), and other requests are served meanwhile.
    {
        Result res;
        const auto t0 = Clock::now();
        std::thread joiner([&] { res = POST("/v1/signal", signalBody(room, "000000aa", "QUJD")); });
        CHECK(wsJson(host)["peer"].str() == "000000aa", "the host did not get the signal");
        const auto t1 = Clock::now();
        const Result info = GET("/v1/info?v=1");
        const Result list = GET("/v1/rooms?v=1");
        CHECK(info.ok() && list.ok() && since(t1) < 1.0 && since(t0) < sSignalWait - 0.1, "requests during a waiting signal: %d %d after %.2f s",
              info.status, list.status, since(t1));
        joiner.join();
        CHECK(isError(res, 504, "timeout"), "%d %s", res.status, res.body.c_str());
        CHECK(since(t0) > sSignalWait - 0.2 && since(t0) < sSignalWait + 2.0, "the wait took %.2f s (%.1f s set)", since(t0), sSignalWait);
        // The late answer is dropped without an error; bad answers get one.
        std::string text;
        host.sendText("{\"t\":\"sig\",\"peer\":\"000000aa\",\"data\":\"bGF0ZQ==\"}");
        host.sendText("ping");
        CHECK(host.recv(text, 3000) == WebSocket::Recv::Message && text == "pong", "after a late answer: %s", text.c_str());
        host.sendText("{\"t\":\"sig\",\"peer\":\"000000aa\",\"data\":\"not base64!\"}");
        CHECK(wsJson(host)["code"].str() == "bad_request", "bad answer data");
        host.sendText("{\"t\":\"sig\",\"peer\":\"000000aa\",\"data\":\"" + std::string(2801, 'A') + "\"}");
        CHECK(wsJson(host)["code"].str() == "bad_request", "answer data over 2800");
    }
    // The same peer asking again replaces its first request.
    {
        Result first, second;
        std::thread j1([&] { first = POST("/v1/signal", signalBody(room, "000000bb", "Zmlyc3Q=")); });
        CHECK(wsJson(host)["data"].str() == "Zmlyc3Q=", "first");
        const auto t0 = Clock::now();
        std::thread j2([&] { second = POST("/v1/signal", signalBody(room, "000000BB", "c2Vjb25k")); });
        CHECK(wsJson(host)["data"].str() == "c2Vjb25k", "second");
        j1.join();
        CHECK(isError(first, 504, "timeout") && since(t0) < 1.0, "the replaced request: %d after %.2f s", first.status, since(t0));
        host.sendText("{\"t\":\"sig\",\"peer\":\"000000bb\",\"data\":\"b2s=\"}");
        j2.join();
        CHECK(second.ok() && J(second)["data"].str() == "b2s=", "%d %s", second.status, second.body.c_str());
    }
    // More than 8 signals waiting on one room.
    {
        Result res[9];
        std::vector<std::thread> joiners;
        for (int i = 0; i < 9; i++)
            joiners.emplace_back([&, i] { res[i] = POST("/v1/signal", signalBody(room, "0000010" + std::to_string(i), "QUJD")); });
        for (auto& t : joiners) t.join();
        int timeouts = 0, tooMany = 0;
        for (const Result& r : res) {
            if (isError(r, 504, "timeout")) timeouts++;
            if (isError(r, 429, "too_many")) tooMany++;
        }
        CHECK(timeouts == 8 && tooMany == 1, "%d timeouts, %d too_many", timeouts, tooMany);
        int got = 0;
        while (wsJson(host, 200)["t"].str() == "sig") got++;
        CHECK(got == 8, "the host got %d signals", got);
        // The waiting slots are free again.
        Result again;
        std::thread joiner([&] { again = POST("/v1/signal", signalBody(room, "00000200", "QUJD")); });
        CHECK(wsJson(host)["peer"].str() == "00000200", "after the burst");
        host.sendText("{\"t\":\"sig\",\"peer\":\"00000200\",\"data\":\"QUJD\"}");
        joiner.join();
        CHECK(again.ok(), "%d %s", again.status, again.body.c_str());
    }
    // A joiner that gives up (its request times out on its side) frees its slot; the host can still be reached.
    {
        const Result gaveUp = POST("/v1/signal", signalBody(room, "00000300", "QUJD"), 300);
        CHECK(!gaveUp.ok(), "no answer expected");
        CHECK(wsJson(host)["peer"].str() == "00000300", "the host did not get the signal");
    }
    // The host leaves while a joiner waits: no_room at once.
    {
        Result res;
        const auto t0 = Clock::now();
        std::thread joiner([&] { res = POST("/v1/signal", signalBody(room, "000000cc", "QUJD")); });
        CHECK(wsJson(host)["peer"].str() == "000000cc", "the host did not get the signal");
        host.close();
        joiner.join();
        CHECK(isError(res, 404, "no_room") && since(t0) < 1.0, "%d %s after %.2f s", res.status, res.body.c_str(), since(t0));
    }
    const Result r = POST("/v1/signal", signalBody(room, "000000dd", "QUJD"));
    CHECK(isError(r, 404, "no_room"), "%d %s", r.status, r.body.c_str());
}

static void testIdle() {
    const char* env = std::getenv("CF_LOBBY_TEST_IDLE");
    const int seconds = env ? std::atoi(env) : 0;
    if (seconds <= 0) return;
    std::printf("idle host socket for %d s\n", seconds);
    WebSocket host;
    CHECK(wsOpen(host), "connect");
    const std::string room = reg(host, RoomSpec());
    std::string text;
    CHECK(host.recv(text, seconds * 1000) == WebSocket::Recv::Timeout, "something arrived or the socket closed (code %d)", host.closeCode());
    CHECK(host.isOpen(), "closed while idle");
    CHECK(host.sendText("ping") && host.recv(text, 3000) == WebSocket::Recv::Message && text == "pong", "ping after the idle time");
    CHECK(!listed(room).isNull(), "the room went away");
}

// External mode, with the lobby's relay secrets pointing at tools/lobby_worker/test/fake_turn.mjs: what the
// Worker asks Cloudflare's TURN API and what it makes of both documented answers. Run last but one: it
// uses up this address's 10 relay requests per hour.
static Result fakeTurn(const std::string& method, const std::string& path) {
    return port::http::request(method, sTurnFake + path, method == "POST" ? "{}" : "", method == "POST" ? "application/json" : "", 5000);
}
static void testRelayExternal() {
    std::printf("relay credentials through the fake TURN API\n");
    WebSocket host;
    CHECK(wsOpen(host), "connect");
    const std::string id = reg(host, RoomSpec());
    CHECK(isHex(id, 16), "%s", sLastError.c_str());
    const std::string body = "{\"v\":1,\"room\":\"" + id + "\"}";
    CHECK(fakeTurn("POST", "/mode/array").ok(), "the fake TURN API does not answer at %s", sTurnFake.c_str());
    const double before = J(fakeTurn("GET", "/last"))["count"].num(-1);

    Result r = POST("/v1/turn", body);
    if (isError(r, 429, "too_many")) {
        CHECK(false, "this address's 10 relay requests per hour are used up: restart the lobby (wrangler dev) and run again");
        return;
    }
    Json j = J(r);
    CHECK(r.ok() && j["servers"].a.size() == 2, "%d %s", r.status, r.body.c_str());
    if (j["servers"].a.size() == 2)  // UDP only, in the API's order
        CHECK(j["servers"].a[0].str() == "turn.cloudflare.com:3478" && j["servers"].a[1].str() == "turn.cloudflare.com:53", "%s", r.body.c_str());
    CHECK(j["username"].str() == "fake-user" && j["credential"].str() == "fake-pass" && j["ttl"].num(-1) == 21600, "%s", r.body.c_str());
    Json last = J(fakeTurn("GET", "/last"));
    CHECK(last["count"].num(-1) == before + 1, "the TURN API was called %g times", last["count"].num(-1) - before);
    CHECK(last["path"].str() == "/v1/turn/keys/test-key-id/credentials/generate-ice-servers", "path %s", last["path"].str().c_str());
    CHECK(last["authorization"].str() == "Bearer test-token", "authorization %s", last["authorization"].str().c_str());
    CHECK(last["body"].str() == "{\"ttl\":21600}" && last["contentType"].str() == "application/json", "body %s (%s)", last["body"].str().c_str(),
          last["contentType"].str().c_str());

    CHECK(fakeTurn("POST", "/mode/object").ok(), "mode");  // the older answer: one object, a URL without a port
    r = POST("/v1/turn", body);
    j = J(r);
    CHECK(r.ok() && j["servers"].a.size() == 2 && j["username"].str() == "old-user" && j["credential"].str() == "old-pass" && j["ttl"].num(-1) == 21600,
          "%d %s", r.status, r.body.c_str());
    if (j["servers"].a.size() == 2)
        CHECK(j["servers"].a[0].str() == "relay.example:3479" && j["servers"].a[1].str() == "relay.example:3478", "%s", r.body.c_str());

    CHECK(fakeTurn("POST", "/mode/error").ok(), "mode");
    r = POST("/v1/turn", body);
    CHECK(isError(r, 503, "busy"), "TURN API failure: %d %s", r.status, r.body.c_str());
    CHECK(fakeTurn("POST", "/mode/empty").ok(), "mode");
    r = POST("/v1/turn", body);
    CHECK(isError(r, 503, "busy"), "an answer without relay servers: %d %s", r.status, r.body.c_str());

    // No room: answered without asking the TURN API. Bad requests and versions too.
    CHECK(fakeTurn("POST", "/mode/array").ok(), "mode");
    const double calls = J(fakeTurn("GET", "/last"))["count"].num(-1);
    r = POST("/v1/turn", "{\"v\":1,\"room\":\"ffffffffffffffff\"}");
    CHECK(isError(r, 404, "no_room"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/turn", "{\"v\":1,\"room\":\"nothex\"}");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/turn", "{\"v\":2,\"room\":\"" + id + "\"}");
    CHECK(isError(r, 426, "version"), "%d %s", r.status, r.body.c_str());
    CHECK(J(fakeTurn("GET", "/last"))["count"].num(-1) == calls, "the TURN API was asked for a refused request");

    // 10 per hour per address: 5 used (the unknown room counts), so 5 more pass.
    int passes = 0;
    for (int i = 0; i < 12; i++) {
        r = POST("/v1/turn", body);
        if (!r.ok()) break;
        passes++;
    }
    CHECK(isError(r, 429, "too_many") && passes == 5, "%d more relay requests passed, then %d %s", passes, r.status, r.body.c_str());
    CHECK(J(fakeTurn("GET", "/last"))["count"].num(-1) == calls + passes, "the TURN API was asked for a refused request");
}

// External mode, last: 20 signals per minute per address. (The earlier tests may have used some of this
// minute's; the limit must come by the 21st.)
static void testSignalRateExternal() {
    std::printf("signal rate limit\n");
    int passes = 0;
    Result r;
    for (int i = 0; i < 25; i++) {
        r = POST("/v1/signal", signalBody("ffffffffffffffff", "00000001", "QUJD"));
        if (!isError(r, 404, "no_room")) break;
        passes++;
    }
    CHECK(isError(r, 429, "too_many") && passes <= 20, "%d signals passed, then %d %s", passes, r.status, r.body.c_str());
    std::printf("  %d more signals passed before too_many\n", passes);
    r = POST("/v1/signal", "{\"v\":1}");  // a bad request still says what is wrong
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
}

// External mode, CF_LOBBY_TEST_SLOW=1: the real 120 s rule. Nothing but "ping" reaches the lobby for 130 s
// (the Worker's Durable Object sleeps meanwhile: the pings are answered for it), then the list is read.
static void testSlowExternal() {
    std::printf("stale sockets, 130 s (a silent host and one that only sends \"ping\")\n");
    WebSocket quiet, alive;
    CHECK(wsOpen(quiet) && wsOpen(alive), "connect");
    RoomSpec spec;
    spec.name = "Quiet";
    const std::string idQuiet = reg(quiet, spec);
    spec.name = "Alive";
    spec.host = "H\xC3\xB6st";
    spec.town = "Town";
    spec.players = 2;
    spec.max = 3;
    spec.lock = true;
    spec.game = "RUUP01";
    spec.ver = "9.8.7";
    spec.tag = "00112233aabbccdd";
    const std::string idAlive = reg(alive, spec);
    CHECK(isHex(idQuiet, 16) && isHex(idAlive, 16), "reg: %s", sLastError.c_str());
    const auto t0 = Clock::now();
    std::string text;
    while (since(t0) < 130) {
        sleepMs(26000);
        CHECK(alive.sendText("ping") && alive.recv(text, 3000) == WebSocket::Recv::Message && text == "pong", "ping after %.0f s", since(t0));
    }
    const Json room = listed(idAlive);
    CHECK(room["name"].str() == "Alive" && room["host"].str() == "H\xC3\xB6st" && room["town"].str() == "Town" && room["players"].num(-1) == 2 &&
              room["max"].num(-1) == 3 && room["lock"].b && !room["friends"].b && room["game"].str() == "RUUP01" && room["ver"].str() == "9.8.7" &&
              room["tag"].str() == "00112233aabbccdd",
          "the pinging room after 130 s");
    CHECK(room["age"].num(-1) >= 125 && room["age"].num(-1) <= 140, "age %g", room["age"].num(-1));
    CHECK(listed(idQuiet).isNull(), "the silent room is still listed");
    CHECK(quiet.recv(text, 3000) == WebSocket::Recv::Closed && quiet.closeCode() == 1001, "the silent socket: close code %d", quiet.closeCode());
    spec.players = 3;
    CHECK(reg(alive, spec) == idAlive, "the id after the long sleep (%s)", sLastError.c_str());
    syncList();
    CHECK(listed(idAlive)["players"].num(-1) == 3, "the update after the long sleep");
}

// CF_LOBBY_TEST_URL: every check that does not need the stand-in's own options, against a lobby that runs
// already (the Worker under `wrangler dev`).
static int runExternal(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    sExternal = true;
    sSignalWait = 8.0;
    sLobby.base = url;
    // Plain-socket checks need a loopback address and its port.
    const size_t hostAt = url.find("://");
    const std::string hostPort = hostAt == std::string::npos ? "" : url.substr(hostAt + 3);
    const size_t colon = hostPort.rfind(':');
    const std::string host = hostPort.substr(0, colon);
    if (url.rfind("http://", 0) == 0 && colon != std::string::npos && (host == "127.0.0.1" || host == "localhost"))
        sLobby.port = std::atoi(hostPort.c_str() + colon + 1);
    if (const char* fake = std::getenv("CF_LOBBY_TEST_TURN_FAKE")) sTurnFake = fake;
    while (!sTurnFake.empty() && sTurnFake.back() == '/') sTurnFake.pop_back();
    const char* slowEnv = std::getenv("CF_LOBBY_TEST_SLOW");
    const bool slow = slowEnv && std::atoi(slowEnv) > 0;
    std::printf("mode: external lobby at %s (the stand-in's own checks are skipped: see the top of %s)\n", url.c_str(), __FILE__);

    const auto t0 = Clock::now();
    const Result info = GET("/v1/info?v=1");
    if (!info.ok()) {
        std::printf("FAIL no lobby answers at %s: %d %s\n", url.c_str(), info.status, info.error.c_str());
        return 1;
    }
    sRelay = J(info)["turn"].b;
    if (sRelay && sTurnFake.empty())
        std::printf("  the lobby has relay secrets and CF_LOBBY_TEST_TURN_FAKE is not set: POST /v1/turn is not called for a room\n");
    CHECK(sTurnFake.empty() || sRelay, "CF_LOBBY_TEST_TURN_FAKE is set but the lobby has no relay secrets (.dev.vars)");

    testBasics();
    testBadRequests();
    if (sLobby.port) testRawExternal();
    else std::printf("plain-socket checks skipped (not a loopback http:// address)\n");
    testRooms();
    testMessages();
    testSignals();
    testIdle();
    if (slow) testSlowExternal();
    if (sRelay && !sTurnFake.empty()) testRelayExternal();
    testSignalRateExternal();
    std::printf("%s: %d failure(s) in %.1f s (external lobby)\n", sFails ? "FAILED" : "ok", sFails, since(t0));
    return sFails;
}

// The second run: a relay configured, short stale time, small limits.
static void testSecondRun() {
    std::printf("second run: relay credentials, stale sockets, lobby full, signal rate\n");
    Result r = GET("/v1/info?v=1");
    CHECK(r.ok() && J(r)["turn"].type == Json::Bool && J(r)["turn"].b && J(r)["rooms"].num(-1) == 0, "%d %s", r.status, r.body.c_str());
    CHECK(J(GET("/v1/rooms?v=1"))["turn"].b, "rooms: turn");

    WebSocket quiet, alive, third;
    CHECK(wsOpen(quiet) && wsOpen(alive) && wsOpen(third), "connect");
    RoomSpec spec;
    spec.name = "Quiet";
    const std::string idQuiet = reg(quiet, spec);
    spec.name = "Alive";
    const std::string idAlive = reg(alive, spec);
    CHECK(isHex(idQuiet, 16) && isHex(idAlive, 16), "reg: %s", sLastError.c_str());
    const auto t0 = Clock::now();
    // --max-rooms 2: the lobby is full.
    CHECK(reg(third, RoomSpec()).empty() && sLastError == "full", "third room: %s", sLastError.c_str());
    third.close();

    // Relay credentials: static ones, for an existing room only; 4 per hour in this run.
    r = POST("/v1/turn", "{\"v\":1,\"room\":\"" + idAlive + "\"}");
    Json j = J(r);
    CHECK(r.ok() && j["servers"].a.size() == 1 && j["servers"].a[0].str() == "127.0.0.1:3478", "%d %s", r.status, r.body.c_str());
    CHECK(j["username"].str() == "test user" && j["credential"].str() == "p\"w\\d" && j["ttl"].num(-1) == 86400, "%s", r.body.c_str());
    r = POST("/v1/turn", "{\"v\":1,\"room\":\"ffffffffffffffff\"}");
    CHECK(isError(r, 404, "no_room"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/turn", "{\"v\":1,\"room\":\"nothex\"}");
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    r = POST("/v1/turn", "{\"v\":2,\"room\":\"" + idAlive + "\"}");
    CHECK(isError(r, 426, "version"), "%d %s", r.status, r.body.c_str());
    for (int i = 0; i < 2; i++) {
        r = POST("/v1/turn", "{\"v\":1,\"room\":\"" + idQuiet + "\"}");
        CHECK(r.ok(), "%d %s", r.status, r.body.c_str());
    }
    r = POST("/v1/turn", "{\"v\":1,\"room\":\"" + idAlive + "\"}");
    CHECK(isError(r, 429, "too_many"), "fifth relay request: %d %s", r.status, r.body.c_str());

    // A host that sends nothing is closed after the stale time (1.5 s here); "ping" keeps the other one.
    std::string text;
    while (since(t0) < 2.6) {
        CHECK(alive.sendText("ping") && alive.recv(text, 2000) == WebSocket::Recv::Message && text == "pong", "ping");
        sleepMs(300);
    }
    CHECK(quiet.recv(text, 1000) == WebSocket::Recv::Closed, "the silent socket is still open");
    CHECK(quiet.closeCode() == 1001, "close code %d", quiet.closeCode());
    CHECK(listed(idQuiet).isNull(), "the silent room is still listed");
    CHECK(listed(idAlive)["name"].str() == "Alive", "the pinging room went away");
    CHECK(listed(idAlive)["age"].num(-1) >= 2 && listed(idAlive)["age"].num(-1) <= 6, "age %g", listed(idAlive)["age"].num(-1));

    // 3 signals per minute per address in this run.
    for (int i = 0; i < 3; i++) {
        r = POST("/v1/signal", signalBody("ffffffffffffffff", "00000001", "QUJD"));
        CHECK(isError(r, 404, "no_room"), "%d %s", r.status, r.body.c_str());
    }
    r = POST("/v1/signal", signalBody(idAlive, "00000001", "QUJD"));
    CHECK(isError(r, 429, "too_many"), "fourth signal: %d %s", r.status, r.body.c_str());
    r = POST("/v1/signal", "{\"v\":1}");  // a bad request is not counted and still says what is wrong
    CHECK(isError(r, 400, "bad_request"), "%d %s", r.status, r.body.c_str());
    // --trust-proxy: the client is the last X-Forwarded-For entry (the one the proxy added), not the TCP
    // peer and not what the client claimed in front of it.
    {
        const std::string body = signalBody("ffffffffffffffff", "00000001", "QUJD");
        auto viaProxy = [&](const std::string& forwardedFor) {
            Raw raw;
            if (!raw.open()) return std::string("no connection");
            raw.write("POST /v1/signal HTTP/1.1\r\nHost: lobby\r\nX-Forwarded-For: " + forwardedFor + "\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\n\r\n" + body);
            return raw.read(3).substr(0, 12);
        };
        for (int i = 0; i < 3; i++) CHECK(viaProxy("203.0.113.5") == "HTTP/1.1 404", "another client behind the proxy, signal %d", i);
        CHECK(viaProxy("203.0.113.5") == "HTTP/1.1 429", "its fourth signal");
        CHECK(viaProxy("10.9.9.9, 203.0.113.5") == "HTTP/1.1 429", "a claimed address in front does not help");
        CHECK(viaProxy("203.0.113.5, 198.51.100.77") == "HTTP/1.1 404", "a third client");
        CHECK(viaProxy("not an address") == "HTTP/1.1 429", "an unusable header falls back to the TCP peer");
    }

    // Stopping with a socket open: the client sees the connection end.
    const double took = sLobby.stop();
    CHECK(took < 0.5 && sLobby.rc == 0, "stop took %.2f s (rc %d)", took, sLobby.rc);
    CHECK(alive.recv(text, 3000) == WebSocket::Recv::Closed && !alive.isOpen(), "the socket survived the lobby");
    CHECK(gLobbyPort.load() == 0, "port still set");
    r = GET("/v1/info?v=1", 2000);
    CHECK(!r.ok() && !r.error.empty(), "a stopped lobby answered");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    CHECK(jsonQuote("a\"b\\c\n\x01\xC3\xA9") == "\"a\\\"b\\\\c\\n\\u0001\xC3\xA9\"", "%s", jsonQuote("a\"b\\c\n\x01\xC3\xA9").c_str());

    if (const char* url = std::getenv("CF_LOBBY_TEST_URL"); url && *url) return runExternal(url);
    std::printf("mode: stand-in (cf_lobby in this process)\n");
    const auto t0 = Clock::now();
    if (!sLobby.start({"--signal-timeout-ms", "1500", "--signals-per-min", "1000", "--http-timeout-ms", "1500", "--stats-s", "0"})) {
        std::printf("FAIL the lobby did not start\n");
        return 1;
    }
    testBasics();
    testBadRequests();
    testRawHttp();
    testRooms();
    testMessages();
    testSignals();
    testIdle();
    const double took = sLobby.stop();
    CHECK(took < 0.5 && sLobby.rc == 0, "stop took %.2f s (rc %d)", took, sLobby.rc);

    if (!sLobby.start({"--turn", "127.0.0.1:3478", "--turn-user", "test user", "--turn-pass", "p\"w\\d", "--stale-ms", "1500", "--max-rooms", "2",
                       "--signals-per-min", "3", "--turns-per-hour", "4", "--trust-proxy", "--verbose"})) {
        std::printf("FAIL the lobby did not start a second time\n");
        return sFails + 1;
    }
    testSecondRun();
    if (sLobby.th.joinable()) sLobby.stop();

    // A bad option: usage, no server.
    {
        std::vector<std::string> args = {"cf_lobby", "--no-such-option"};
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        CHECK(lobbyMain(int(argv.size()), argv.data()) == 2, "bad option accepted");
    }
    std::printf("%s: %d failure(s) in %.1f s\n", sFails ? "FAILED" : "ok", sFails, since(t0));
    return sFails;
}
