#pragma once
// Small HTTPS GET client for game features that talk to web services (real-time weather:
// src/game/cf_weather.cpp). Blocking: call it from a background thread, never the game or render
// thread. Windows: WinHTTP. Linux / macOS: libcurl loaded at run time (no build dependency; a
// system without it gets an error result and the feature stays offline). The updater keeps its own
// transport (src/port/update.cpp, with redirects and integrity checks).
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
