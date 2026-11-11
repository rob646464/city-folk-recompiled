#pragma once
// Small HTTPS client (requests and WebSockets) for game features that talk to web services
// (real-time weather: src/game/cf_weather.cpp; the online lobby). Blocking: call it from a
// background thread, never the game or render thread. Windows: WinHTTP. Linux / macOS: libcurl
// loaded at run time (no build dependency; a system without it gets an error result and the
// feature stays offline). The updater keeps its own transport (src/port/update.cpp, with redirects
// and integrity checks).
#include <cstddef>
#include <string>
#include <vector>

namespace port::http {

struct Result {
    int status = 0;     // HTTP status, 0 when no answer came
    std::string body;   // response body (up to maxBytes)
    std::string error;  // transport failure in plain words; empty when an answer came
    bool ok() const { return error.empty() && status == 200; }
};

// One GET, no redirects followed. timeoutMs applies to connecting and to each receive step and,
// doubled, to the whole request.
Result get(const std::string& url, int timeoutMs = 10000, size_t maxBytes = 1 << 20);

// Any method with an optional body (POST: contentType e.g. "application/json"). Same rules as get().
// An answer with another status than 200 still comes back with its body (e.g. an API's error JSON):
// status is set, error stays empty, ok() is false.
Result request(const std::string& method, const std::string& url, const std::string& body = {},
               const std::string& contentType = {}, int timeoutMs = 10000, size_t maxBytes = 1 << 20);

// JSON string escaping for building small request bodies: returns the quoted string.
std::string jsonQuote(const std::string& s);

// A WebSocket client (text messages) over ws:// / wss:// (or http:// / https:// URLs, same thing).
// Blocking calls; one thread owns an object. Windows: WinHTTP's WebSocket API (a reader thread inside
// feeds recv(), so a recv() timeout never disturbs the connection). Linux / macOS: the run-time-loaded
// libcurl makes the (TLS) connection and this class speaks the WebSocket protocol on it, so any
// libcurl works (its own WebSocket API is missing from many distributions' builds); without libcurl
// connect() fails with a clear error text. Binary messages are dropped; a message over 1 MiB closes.
class WebSocket {
public:
    WebSocket();
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // The handshake. timeoutMs covers connecting and the server's answer. On failure err says why in
    // plain words and httpStatus is the server's answer if it gave one that was not the upgrade
    // (e.g. 426 or 429 with an error JSON, whose "message" is appended to err), else 0.
    bool connect(const std::string& url, int timeoutMs, std::string* err, int* httpStatus = nullptr);
    bool sendText(const std::string& text);
    enum class Recv { Message, Timeout, Closed };
    Recv recv(std::string& text, int timeoutMs);  // one whole text message
    void close();
    bool isOpen() const;
    // The peer's close status once recv() returned Closed (1000-4999), 0 when the connection was just lost.
    int closeCode() const;

private:
    struct Impl;
    Impl* impl_;
};

// Percent-encoding for a query value (UTF-8 in, RFC 3986 unreserved characters kept).
std::string urlEncode(const std::string& s);

// Just enough JSON for small API answers.
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::string> keys;
    std::vector<Json> vals;

    const Json& operator[](const char* key) const;
    std::string str() const { return type == String ? s : std::string(); }
    double num(double fallback = 0) const { return type == Number ? n : fallback; }
    bool isNull() const { return type == Null; }
};
bool parseJson(const std::string& text, Json& out);

}  // namespace port::http
