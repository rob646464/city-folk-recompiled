// The lobby client (online_lobby.h, docs/ONLINE.md "The lobby"). HTTP API v1:
//   GET  /v1/rooms?v=1&game=<id>      the room list
//   WS   /v1/host?v=1                 a host: {"t":"reg","room":{...}} -> {"t":"reg_ok","id"}; joiners'
//                                     signals {"t":"sig","peer","data"} both ways; "ping" -> "pong"
//   POST /v1/signal {v,room,peer,data} a joiner's signal; the answer is the host's signal
//   POST /v1/turn   {v,room}           relay (TURN) credentials when no direct path opens
// Signal data = one handshake datagram of online_net.cpp, base64.
#include "recomp/online_lobby.h"

#include "port/http.h"
#include "recomp/online_crypto.h"
#include "recomp/online_sock.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace online::lobby {
namespace {

namespace http = port::http;
namespace oc = onlinecrypto;

double secs() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64(const std::vector<u8>& d) {
    std::string o;
    for (size_t i = 0; i < d.size(); i += 3) {
        const u32 n = u32(d[i]) << 16 | (i + 1 < d.size() ? u32(d[i + 1]) << 8 : 0) | (i + 2 < d.size() ? u32(d[i + 2]) : 0);
        o += kB64[n >> 18 & 63];
        o += kB64[n >> 12 & 63];
        o += i + 1 < d.size() ? kB64[n >> 6 & 63] : '=';
        o += i + 2 < d.size() ? kB64[n & 63] : '=';
    }
    return o;
}

bool unb64(const std::string& s, std::vector<u8>& out) {
    out.clear();
    u32 acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const char* p = std::strchr(kB64, c);
        if (!p || !c) return false;
        acc = acc << 6 | u32(p - kB64);
        bits += 6;
        if (bits >= 8) {
            out.push_back(u8(acc >> (bits - 8)));
            bits -= 8;
        }
    }
    return true;
}

std::string hex8(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

std::string hostOf(const std::string& url) {
    const size_t a = url.find("://");
    return a == std::string::npos ? url : url.substr(a + 3);
}

std::string roomJson(const RoomInfo& r) {
    return std::string("{\"name\":") + http::jsonQuote(r.name) + ",\"host\":" + http::jsonQuote(r.host) + ",\"town\":" + http::jsonQuote(r.town) +
           ",\"players\":" + std::to_string(r.players) + ",\"max\":" + std::to_string(r.max) + ",\"lock\":" + (r.lock ? "true" : "false") +
           ",\"friends\":" + (r.friends ? "true" : "false") + ",\"game\":" + http::jsonQuote(r.game) + ",\"ver\":" + http::jsonQuote(r.ver) +
           ",\"tag\":" + http::jsonQuote(r.tag) + "}";
}

std::mutex sListMutex;
ListState sList;
double sListAt = -1;

}  // namespace

std::string normalizeUrl(const std::string& text) {
    std::string u;
    for (char c : text)
        if (static_cast<unsigned char>(c) > 0x20 && c != '"') u += c;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.empty()) return u;
    if (u.rfind("ws://", 0) == 0) u = "http://" + u.substr(5);
    if (u.rfind("wss://", 0) == 0) u = "https://" + u.substr(6);
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) u = "https://" + u;
    return u;
}

std::string errorText(int status, const std::string& body, const std::string& transportError) {
    if (status == 0) return "Could not reach the lobby" + (transportError.empty() ? std::string() : " (" + transportError + ")");
    http::Json j;
    std::string code, message;
    if (http::parseJson(body, j)) {
        code = j["error"].str();
        message = j["message"].str();
    }
    if (code == "version") return message.empty() ? "This lobby needs a newer City Folk Recompiled" : message;
    if (code == "no_room") return "That room is no longer open";
    if (code == "too_many") return "Too many requests to the lobby from here: wait a minute and try again";
    if (code == "full") return "The lobby is full right now. Try again later";
    if (code == "no_turn") return "This lobby has no relay";
    if (code == "timeout") return "The room's host did not answer";
    // Cloudflare's free plan: over the daily request limit the Worker is not run at all (HTTP 429,
    // error 1027) or its Durable Object refuses (our 503 busy).
    if (code == "busy" || status == 429 || status == 503)
        return "The lobby is busy (its free daily limit may be used up). Try again later";
    return "The lobby answered with an error (HTTP " + std::to_string(status) + ")";
}

std::string makeTag(const u8 hostPub[32]) {
    u8 t[8], h[32];
    oc::randomBytes(t, 4);
    oc::hmacSha256(hostPub, 32, "cf-lobby-tag", 12, t, 4, h);
    std::memcpy(t + 4, h, 4);
    return oc::hex(t, 8);
}

bool tagMatches(const std::string& tag, const u8 pub[32]) {
    if (tag.size() != 16) return false;
    u8 t[8], h[32];
    for (int i = 0; i < 8; i++) {
        unsigned v = 0;
        if (std::sscanf(tag.c_str() + 2 * i, "%2x", &v) != 1) return false;
        t[i] = u8(v);
    }
    oc::hmacSha256(pub, 32, "cf-lobby-tag", 12, t, 4, h);
    return std::memcmp(t + 4, h, 4) == 0;
}

void refresh(const std::string& url, const std::string& game) {
    {
        std::lock_guard<std::mutex> g(sListMutex);
        if (sList.loading || url.empty()) return;
        sList.loading = true;
        if (sList.url != url) {
            sList.rooms.clear();
            sList.everLoaded = false;
            sListAt = -1;
        }
        sList.url = url;
    }
    std::thread([url, game] {
        const http::Result r = http::request("GET", url + "/v1/rooms?v=1" + (game.empty() ? "" : "&game=" + http::urlEncode(game)), {}, {}, 8000, 256 * 1024);
        std::vector<Room> rooms;
        std::string err;
        bool relay = false;
        http::Json j;
        if (r.ok() && http::parseJson(r.body, j) && j["rooms"].type == http::Json::Array) {
            relay = j["turn"].b;
            for (auto& e : j["rooms"].a) {
                Room x;
                x.id = e["id"].str();
                x.name = e["name"].str();
                x.host = e["host"].str();
                x.town = e["town"].str();
                x.game = e["game"].str();
                x.ver = e["ver"].str();
                x.tag = e["tag"].str();
                x.players = int(e["players"].num());
                x.max = int(e["max"].num(4));
                x.pingMs = int(e["ping"].num(-1));
                x.age = int(e["age"].num());
                x.lock = e["lock"].b;
                x.friends = e["friends"].b;
                if (x.id.size() == 16 && rooms.size() < 200) rooms.push_back(std::move(x));
            }
        } else {
            err = r.ok() ? "The lobby's answer could not be read" : errorText(r.status, r.body, r.error);
        }
        std::lock_guard<std::mutex> g(sListMutex);
        sList.loading = false;
        sList.error = err;
        if (err.empty()) {
            sList.rooms = std::move(rooms);
            sList.relay = relay;
            sList.everLoaded = true;
            sListAt = secs();
        }
    }).detach();
}

ListState state() {
    std::lock_guard<std::mutex> g(sListMutex);
    ListState s = sList;
    s.age = sListAt < 0 ? -1 : secs() - sListAt;
    return s;
}

// ---- host ---------------------------------------------------------------------------------
namespace {

struct HostState {
    std::mutex m;
    std::string url;
    RoomInfo room;
    bool roomDirty = false;
    std::atomic<bool> stop{false};
    bool listed = false;
    std::string status = "Connecting to the lobby...";
    std::deque<std::pair<u32, std::vector<u8>>> in, out;
};

void hostThread(std::shared_ptr<HostState> st) {
    double backoff = 2;
    auto pause = [&](double s) {
        const double until = secs() + s;
        while (!st->stop && secs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };
    while (!st->stop) {
        http::WebSocket ws;
        std::string err;
        int status = 0;
        if (!ws.connect(st->url + "/v1/host?v=1", 10000, &err, &status)) {
            {
                std::lock_guard<std::mutex> g(st->m);
                st->listed = false;
                st->status = "Not listed: " + errorText(status, {}, err);
            }
            std::printf("[lobby] %s: %s\n", hostOf(st->url).c_str(), errorText(status, {}, err).c_str());
            pause(backoff);
            backoff = std::min(backoff * 2, 60.0);
            continue;
        }
        {
            std::lock_guard<std::mutex> g(st->m);
            st->roomDirty = true;
        }
        double lastPing = secs(), refusedAt = -100;
        while (!st->stop) {
            std::string text;
            const auto r = ws.recv(text, 40);
            if (r == http::WebSocket::Recv::Closed) break;
            if (r == http::WebSocket::Recv::Message && text != "pong") {
                http::Json j;
                if (http::parseJson(text, j)) {
                    const std::string t = j["t"].str();
                    std::lock_guard<std::mutex> g(st->m);
                    if (t == "reg_ok") {
                        if (!st->listed) std::printf("[lobby] room listed on %s (id %s)\n", hostOf(st->url).c_str(), j["id"].str().c_str());
                        st->listed = true;
                        st->status = "Listed on " + hostOf(st->url);
                        backoff = 2;
                    } else if (t == "error") {
                        st->listed = false;
                        const std::string code = j["code"].str();
                        st->status = "Not listed: " + (code == "too_many" ? std::string("too many rooms from this address")
                                                       : code == "full"   ? std::string("the lobby is full right now")
                                                       : j["message"].str().empty() ? code : j["message"].str());
                        refusedAt = secs();
                        std::printf("[lobby] %s\n", st->status.c_str());
                    } else if (t == "sig") {
                        std::vector<u8> d;
                        const u32 peer = u32(std::strtoul(j["peer"].str().c_str(), nullptr, 16));
                        if (unb64(j["data"].str(), d) && !d.empty() && st->in.size() < 64) st->in.push_back({peer, std::move(d)});
                    }
                }
            }
            std::string reg;
            std::vector<std::string> sends;
            {
                std::lock_guard<std::mutex> g(st->m);
                if (st->roomDirty || (!st->listed && refusedAt > 0 && secs() - refusedAt > 30)) {
                    st->roomDirty = false;
                    refusedAt = -100;
                    reg = "{\"t\":\"reg\",\"room\":" + roomJson(st->room) + "}";
                }
                while (!st->out.empty()) {
                    sends.push_back("{\"t\":\"sig\",\"peer\":\"" + hex8(st->out.front().first) + "\",\"data\":\"" + b64(st->out.front().second) + "\"}");
                    st->out.pop_front();
                }
            }
            bool ok = true;
            if (!reg.empty()) ok &= ws.sendText(reg);
            for (auto& s : sends) ok &= ws.sendText(s);
            if (secs() - lastPing > 40) {
                lastPing = secs();
                ok &= ws.sendText("ping");
            }
            if (!ok) break;
        }
        ws.close();
        {
            std::lock_guard<std::mutex> g(st->m);
            st->listed = false;
            if (!st->stop) st->status = "Lost the connection to the lobby: connecting again...";
        }
        if (!st->stop) pause(backoff);
        backoff = std::min(backoff * 2, 60.0);
    }
}

struct HostImpl final : HostChannel {
    std::shared_ptr<HostState> st = std::make_shared<HostState>();
    ~HostImpl() override { st->stop = true; }
    void update(const RoomInfo& r) override {
        std::lock_guard<std::mutex> g(st->m);
        const RoomInfo& o = st->room;
        if (o.name == r.name && o.host == r.host && o.town == r.town && o.players == r.players && o.max == r.max && o.lock == r.lock &&
            o.friends == r.friends && o.tag == r.tag)
            return;
        st->room = r;
        st->roomDirty = true;
    }
    void stop() override { st->stop = true; }
    std::string status() override {
        std::lock_guard<std::mutex> g(st->m);
        return st->status;
    }
    bool listed() override {
        std::lock_guard<std::mutex> g(st->m);
        return st->listed;
    }
    void send(u32 peer, const std::vector<u8>& d) override {
        std::lock_guard<std::mutex> g(st->m);
        if (st->out.size() < 64) st->out.push_back({peer, d});
    }
    bool recv(u32* peer, std::vector<u8>* d) override {
        std::lock_guard<std::mutex> g(st->m);
        if (st->in.empty()) return false;
        *peer = st->in.front().first;
        *d = std::move(st->in.front().second);
        st->in.pop_front();
        return true;
    }
    bool busy() override { return false; }
    std::string error() override { return {}; }
    void requestTurn() override {}
    int turnResult(std::string*, std::string*, std::string*) override { return -1; }
};

// ---- joiner -------------------------------------------------------------------------------
struct JoinState {
    std::mutex m;
    std::string url, room, peer;
    std::atomic<bool> stop{false};
    std::deque<std::vector<u8>> jobs, in;  // an empty job = the relay credentials
    bool working = false;
    std::string error;
    int turn = 0;  // 0 not asked / pending, 1 ok, -1 none
    bool turnAsked = false;
    std::string turnServer, turnUser, turnPass;
};

void joinThread(std::shared_ptr<JoinState> st) {
    for (;;) {
        std::vector<u8> job;
        bool have = false;
        {
            std::lock_guard<std::mutex> g(st->m);
            if (!st->jobs.empty()) {
                job = std::move(st->jobs.front());
                st->jobs.pop_front();
                st->working = have = true;
            }
        }
        if (!have) {
            // Stopped (the session let go of this channel): what it queued last still goes out,
            // e.g. the word to the host that this joiner gave up.
            if (st->stop) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }
        if (job.empty()) {  // relay credentials
            if (st->stop) continue;  // nobody is left to use them
            const http::Result r = http::request("POST", st->url + "/v1/turn", "{\"v\":1,\"room\":\"" + st->room + "\"}", "application/json", 10000);
            http::Json j;
            std::string server, user, pass;
            if (r.ok() && http::parseJson(r.body, j) && j["servers"].type == http::Json::Array) {
                user = j["username"].str();
                pass = j["credential"].str();
                // The relay's address is looked up here, not on the session's network thread.
                for (auto& s : j["servers"].a) {
                    sock::Addr a;
                    if (sock::resolve(s.str(), 3478, &a) && a.normalized().family() == AF_INET) {
                        server = a.normalized().text();
                        break;
                    }
                }
            }
            std::lock_guard<std::mutex> g(st->m);
            st->working = false;
            if (!server.empty() && !user.empty()) {
                st->turnServer = server;
                st->turnUser = user;
                st->turnPass = pass;
                st->turn = 1;
            } else {
                st->turn = -1;
                std::printf("[lobby] no relay: %s\n", r.ok() ? "no usable server in the answer" : errorText(r.status, r.body, r.error).c_str());
            }
            continue;
        }
        const std::string body = "{\"v\":1,\"room\":\"" + st->room + "\",\"peer\":\"" + st->peer + "\",\"data\":\"" + b64(job) + "\"}";
        const http::Result r = http::request("POST", st->url + "/v1/signal", body, "application/json", 13000);
        http::Json j;
        std::vector<u8> d;
        std::lock_guard<std::mutex> g(st->m);
        st->working = false;
        if (r.ok() && http::parseJson(r.body, j) && unb64(j["data"].str(), d) && !d.empty()) {
            st->in.push_back(std::move(d));
        } else if (r.status != 504 && !r.ok()) {  // 504: the host did not answer in time (the session asks again)
            st->error = errorText(r.status, r.body, r.error);
            st->jobs.clear();
        }
    }
}

struct JoinImpl final : net::Signaling {
    std::shared_ptr<JoinState> st = std::make_shared<JoinState>();
    ~JoinImpl() override { st->stop = true; }
    void send(u32, const std::vector<u8>& d) override {
        std::lock_guard<std::mutex> g(st->m);
        if (!d.empty() && st->jobs.size() < 8) st->jobs.push_back(d);
    }
    bool recv(u32* peer, std::vector<u8>* d) override {
        std::lock_guard<std::mutex> g(st->m);
        if (st->in.empty()) return false;
        *peer = 0;
        *d = std::move(st->in.front());
        st->in.pop_front();
        return true;
    }
    bool busy() override {
        std::lock_guard<std::mutex> g(st->m);
        return st->working || !st->jobs.empty();
    }
    std::string error() override {
        std::lock_guard<std::mutex> g(st->m);
        return st->error;
    }
    void requestTurn() override {
        std::lock_guard<std::mutex> g(st->m);
        if (st->turnAsked) return;
        st->turnAsked = true;
        st->jobs.push_back({});
    }
    int turnResult(std::string* server, std::string* user, std::string* pass) override {
        std::lock_guard<std::mutex> g(st->m);
        if (st->turn == 1) {
            *server = st->turnServer;
            *user = st->turnUser;
            *pass = st->turnPass;
        }
        return st->turn;
    }
};

}  // namespace

std::shared_ptr<HostChannel> HostChannel::start(const std::string& url, const RoomInfo& room) {
    auto h = std::make_shared<HostImpl>();
    h->st->url = url;
    h->st->room = room;
    std::thread(hostThread, h->st).detach();
    return h;
}

std::shared_ptr<net::Signaling> joinChannel(const std::string& url, const std::string& roomId) {
    auto j = std::make_shared<JoinImpl>();
    j->st->url = url;
    j->st->room = roomId;
    u8 r[4];
    oc::randomBytes(r, 4);
    j->st->peer = oc::hex(r, 4);
    std::thread(joinThread, j->st).detach();
    return j;
}

}  // namespace online::lobby
