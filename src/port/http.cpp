// Small HTTPS client (see http.h): requests and WebSockets. Transport code follows the updater's
// (src/port/update.cpp).
#include "port/http.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#else
#include <cerrno>
#include <dlfcn.h>
#include <poll.h>
#endif

namespace port::http {
namespace {

constexpr const char* kUserAgent = "CityFolkRecompiled (game feature)";
constexpr size_t kMaxWsMessage = 1 << 20;  // a WebSocket message larger than this ends the connection

struct Url {
    std::string scheme, host, path;
    int port = 0;
};

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseUrl(const std::string& url, Url& u) {
    const auto se = url.find("://");
    if (se == std::string::npos) return false;
    u.scheme = lower(url.substr(0, se));
    const auto hs = se + 3;
    const auto pe = url.find_first_of("/?#", hs);
    std::string hostport = url.substr(hs, pe == std::string::npos ? std::string::npos : pe - hs);
    if (hostport.empty() || hostport.find('@') != std::string::npos) return false;
    u.path = pe == std::string::npos ? "/" : url.substr(pe);
    if (const auto h = u.path.find('#'); h != std::string::npos) u.path.resize(h);
    if (u.path.empty() || u.path[0] != '/') u.path = "/" + u.path;
    u.port = u.scheme == "https" ? 443 : 80;
    if (const auto c = hostport.rfind(':'); c != std::string::npos) {
        u.port = std::atoi(hostport.c_str() + c + 1);
        hostport.resize(c);
    }
    u.host = lower(hostport);
    return (u.scheme == "https" || u.scheme == "http") && !u.host.empty() && u.port > 0 && u.port < 65536;
}

// A WebSocket URL: ws:// and wss:// are http:// and https:// to the transport.
bool parseWsUrl(const std::string& url, Url& u, std::string* httpUrl = nullptr) {
    std::string s = url;
    const auto se = s.find("://");
    if (se == std::string::npos) return false;
    const std::string scheme = lower(s.substr(0, se));
    if (scheme == "ws") s = "http" + s.substr(se);
    else if (scheme == "wss") s = "https" + s.substr(se);
    if (httpUrl) *httpUrl = s;
    return parseUrl(s, u);
}

// This PC: never through a proxy (a local lobby / test server).
bool isLoopback(const std::string& host) {
    return host == "localhost" || host.rfind("127.", 0) == 0 || host == "[::1]" || host == "::1";
}

// "the server refused the connection (HTTP 426): <the error JSON's message>"
std::string refusalText(int status, const std::string& body) {
    std::string s = "the server refused the connection (HTTP " + std::to_string(status) + ")";
    Json j;
    if (!body.empty() && parseJson(body, j)) {
        const std::string m = j["message"].str();
        if (!m.empty() && m.size() <= 200) s += ": " + m;
    }
    return s;
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string winHttpError(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "could not resolve the server name (offline?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "could not connect (offline or blocked?)";
    case ERROR_WINHTTP_TIMEOUT: return "the connection timed out";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was reset";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the secure connection failed (certificate)";
    default: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "network error %lu", e);
        return buf;
    }
    }
}

HINTERNET openSession(const Url& u) {
    if (isLoopback(u.host))
        return WinHttpOpen(widen(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET s = WinHttpOpen(widen(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(widen(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    return s;
}

Result requestImpl(const std::string& method, const std::string& url, const std::string& body,
                   const std::string& contentType, int timeoutMs, size_t maxBytes) {
    Result r;
    Url u;
    if (!parseUrl(url, u)) {
        r.error = "bad URL";
        return r;
    }
    const auto start = std::chrono::steady_clock::now();
    HINTERNET s = openSession(u);
    if (!s) {
        r.error = winHttpError(GetLastError());
        return r;
    }
    WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET c = WinHttpConnect(s, widen(u.host).c_str(), INTERNET_PORT(u.port), 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, widen(method).c_str(), widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    bool ok = q != nullptr;
    if (ok) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(q, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        const std::wstring headers = contentType.empty() ? std::wstring() : widen("Content-Type: " + contentType + "\r\n");
        ok = WinHttpSendRequest(q, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : DWORD(-1L),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()),
                                DWORD(body.size()), 0) &&
             WinHttpReceiveResponse(q, nullptr);
    }
    if (!ok) r.error = winHttpError(GetLastError());
    if (ok) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = int(code);
        char buf[16 * 1024];
        for (;;) {
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(2 * timeoutMs)) {
                r.error = "the connection timed out";
                break;
            }
            DWORD got = 0;
            if (!WinHttpReadData(q, buf, DWORD(sizeof(buf)), &got)) {
                r.error = winHttpError(GetLastError());
                break;
            }
            if (got == 0) break;
            if (r.body.size() + got > maxBytes) {
                r.error = "the answer was too large";
                break;
            }
            r.body.append(buf, got);
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return r;
}
#else
struct CurlApi {
    void* lib = nullptr;
    int (*global_init)(long) = nullptr;
    void* (*easy_init)() = nullptr;
    int (*easy_setopt)(void*, int, ...) = nullptr;
    int (*easy_perform)(void*) = nullptr;
    int (*easy_getinfo)(void*, int, ...) = nullptr;
    void (*easy_cleanup)(void*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    // Optional (request bodies / headers, WebSockets): in every libcurl since 7.18.2, checked where used.
    void* (*slist_append)(void*, const char*) = nullptr;
    void (*slist_free_all)(void*) = nullptr;
    int (*easy_send)(void*, const void*, size_t, size_t*) = nullptr;
    int (*easy_recv)(void*, void*, size_t, size_t*) = nullptr;
};
// libcurl's stable option / info / error numbers.
enum : int {
    kOptWriteData = 10001, kOptUrl = 10002, kOptUserAgent = 10018, kOptWriteFunction = 20011,
    kOptNoProgress = 43, kOptFollowLocation = 52, kOptNoSignal = 99, kOptTimeoutMs = 155, kOptConnectTimeoutMs = 156,
    kOptPostFields = 10015, kOptHttpHeader = 10023, kOptCustomRequest = 10036, kOptNoProxy = 10177,
    kOptPost = 47, kOptPostFieldSize = 60, kOptHttpProxyTunnel = 61, kOptHttpVersion = 84, kOptConnectOnly = 141,
    kInfoResponseCode = 0x200000 + 2, kInfoActiveSocket = 0x500000 + 44,
    kErrResolve = 6, kErrConnect = 7, kErrWrite = 23, kErrTimeout = 28, kErrAgain = 81,
    kHttpVersion11 = 2,
};

const CurlApi* curl() {
    static CurlApi api;
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef __APPLE__
        for (const char* name : {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib"}) {
#else
        for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "libcurl.so"}) {
#endif
            api.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (api.lib) break;
        }
        if (!api.lib) return;
        auto sym = [](const char* n) { return dlsym(api.lib, n); };
        api.global_init = reinterpret_cast<int (*)(long)>(sym("curl_global_init"));
        api.easy_init = reinterpret_cast<void* (*)()>(sym("curl_easy_init"));
        api.easy_setopt = reinterpret_cast<int (*)(void*, int, ...)>(sym("curl_easy_setopt"));
        api.easy_perform = reinterpret_cast<int (*)(void*)>(sym("curl_easy_perform"));
        api.easy_getinfo = reinterpret_cast<int (*)(void*, int, ...)>(sym("curl_easy_getinfo"));
        api.easy_cleanup = reinterpret_cast<void (*)(void*)>(sym("curl_easy_cleanup"));
        api.easy_strerror = reinterpret_cast<const char* (*)(int)>(sym("curl_easy_strerror"));
        if (!api.global_init || !api.easy_init || !api.easy_setopt || !api.easy_perform || !api.easy_getinfo ||
            !api.easy_cleanup || !api.easy_strerror) {
            dlclose(api.lib);
            api.lib = nullptr;
            return;
        }
        api.slist_append = reinterpret_cast<void* (*)(void*, const char*)>(sym("curl_slist_append"));
        api.slist_free_all = reinterpret_cast<void (*)(void*)>(sym("curl_slist_free_all"));
        api.easy_send = reinterpret_cast<int (*)(void*, const void*, size_t, size_t*)>(sym("curl_easy_send"));
        api.easy_recv = reinterpret_cast<int (*)(void*, void*, size_t, size_t*)>(sym("curl_easy_recv"));
        api.global_init(3);  // CURL_GLOBAL_DEFAULT
    });
    return api.lib ? &api : nullptr;
}

std::string curlErrorText(const CurlApi* api, int rc) {
    switch (rc) {
    case kErrResolve: return "could not resolve the server name (offline?)";
    case kErrConnect: return "could not connect (offline or blocked?)";
    case kErrTimeout: return "the connection timed out";
    default: return api->easy_strerror(rc);
    }
}

struct Sink {
    Result* r;
    size_t max;
    bool tooLarge = false;
};
size_t curlWrite(char* data, size_t size, size_t n, void* user) {
    auto* s = static_cast<Sink*>(user);
    if (s->r->body.size() + size * n > s->max) {
        s->tooLarge = true;
        return 0;
    }
    s->r->body.append(data, size * n);
    return size * n;
}

Result requestImpl(const std::string& method, const std::string& url, const std::string& body,
                   const std::string& contentType, int timeoutMs, size_t maxBytes) {
    Result r;
    const CurlApi* api = curl();
    if (!api) {
        r.error = "libcurl was not found (install the libcurl4 package)";
        return r;
    }
    void* h = api->easy_init();
    if (!h) {
        r.error = "libcurl could not start";
        return r;
    }
    Sink sink{&r, maxBytes};
    void* headers = nullptr;
    api->easy_setopt(h, kOptUrl, url.c_str());
    api->easy_setopt(h, kOptUserAgent, kUserAgent);
    api->easy_setopt(h, kOptFollowLocation, 0L);
    api->easy_setopt(h, kOptNoSignal, 1L);
    api->easy_setopt(h, kOptNoProgress, 1L);
    api->easy_setopt(h, kOptConnectTimeoutMs, long(timeoutMs));
    api->easy_setopt(h, kOptTimeoutMs, long(2 * timeoutMs));
    api->easy_setopt(h, kOptWriteFunction, &curlWrite);
    api->easy_setopt(h, kOptWriteData, &sink);
    if (Url u; parseUrl(url, u) && isLoopback(u.host)) api->easy_setopt(h, kOptNoProxy, "*");
    const bool isGet = method.empty() || method == "GET";
    if (!body.empty() || method == "POST") {
        // The body stays alive until the request is done (libcurl does not copy it).
        api->easy_setopt(h, kOptPost, 1L);
        api->easy_setopt(h, kOptPostFields, body.c_str());
        api->easy_setopt(h, kOptPostFieldSize, long(body.size()));
    }
    if (!isGet && method != "POST") api->easy_setopt(h, kOptCustomRequest, method.c_str());
    if (!isGet || !body.empty()) {
        if (api->slist_append && api->slist_free_all) {
            // No "Expect: 100-continue" pause, and the caller's content type instead of the form default.
            headers = api->slist_append(headers, "Expect:");
            const std::string ct = "Content-Type: " + (contentType.empty() ? std::string("application/octet-stream") : contentType);
            if (!body.empty() || method == "POST") headers = api->slist_append(headers, ct.c_str());
            if (headers) api->easy_setopt(h, kOptHttpHeader, headers);
        }
    }
    const int rc = api->easy_perform(h);
    long code = 0;
    api->easy_getinfo(h, kInfoResponseCode, &code);
    r.status = int(code);
    api->easy_cleanup(h);
    if (headers) api->slist_free_all(headers);
    if (rc == 0) return r;
    if (rc == kErrWrite && sink.tooLarge) r.error = "the answer was too large";
    else r.error = curlErrorText(api, rc);
    return r;
}

// ---- SHA-1 and base64 (the WebSocket handshake key) ---------------------------------------------
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
        for (size_t i = 0; i < 16; i++)
            w[i] = uint32_t(uint8_t(m[off + 4 * i])) << 24 | uint32_t(uint8_t(m[off + 4 * i + 1])) << 16 |
                   uint32_t(uint8_t(m[off + 4 * i + 2])) << 8 | uint32_t(uint8_t(m[off + 4 * i + 3]));
        for (size_t i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (size_t i = 0; i < 80; i++) {
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

uint32_t randomWord() {
    static thread_local std::mt19937 rng{[] {
        std::random_device rd;
        return uint32_t(rd()) ^ uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
    }()};
    return uint32_t(rng());
}
#endif

// ---- JSON ----------------------------------------------------------------------------------
struct JsonParser {
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
    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) {
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
    bool hex4(unsigned& v) {
        if (end - p < 4) return ok = false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return ok = false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (p >= end || *p != '"') return ok = false;
        p++;
        while (p < end && *p != '"') {
            if (*p != '\\') {
                out += *p++;
                continue;
            }
            if (++p >= end) return ok = false;
            const char c = *p++;
            switch (c) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo = 0;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else cp = 0xFFFD;
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
    Json value() {
        Json v;
        ws();
        if (p >= end || ++depth > 64) {
            ok = false;
            return v;
        }
        const char c = *p;
        if (c == '{') {
            v.type = Json::Object;
            p++;
            ws();
            if (p < end && *p == '}') p++;
            else
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
                    Json item = value();
                    v.keys.push_back(std::move(k));
                    v.vals.push_back(std::move(item));
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
        } else if (c == '[') {
            v.type = Json::Array;
            p++;
            ws();
            if (p < end && *p == ']') p++;
            else
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
        } else if (c == '"') {
            v.type = Json::String;
            string(v.s);
        } else if (c == 't') {
            v.type = Json::Bool;
            v.b = lit("true");
        } else if (c == 'f') {
            v.type = Json::Bool;
            lit("false");
        } else if (c == 'n') {
            lit("null");
        } else {
            v.type = Json::Number;
            const char* s = p;
            while (p < end && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.' ||
                               *p == 'e' || *p == 'E'))
                p++;
            if (p == s) ok = false;
            else v.n = std::strtod(std::string(s, p).c_str(), nullptr);
        }
        depth--;
        return v;
    }
};

}  // namespace

Result get(const std::string& url, int timeoutMs, size_t maxBytes) {
    return requestImpl("GET", url, {}, {}, timeoutMs, maxBytes);
}

Result request(const std::string& method, const std::string& url, const std::string& body, const std::string& contentType,
               int timeoutMs, size_t maxBytes) {
    std::string m = method.empty() ? std::string("GET") : method;
    for (auto& c : m) c = char(std::toupper(static_cast<unsigned char>(c)));
    return requestImpl(m, url, body, contentType, timeoutMs, maxBytes);
}

std::string jsonQuote(const std::string& s) {
    static const char* kHex = "0123456789abcdef";
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else out += ch;
    }
    out += '"';
    return out;
}

std::string urlEncode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

const Json& Json::operator[](const char* key) const {
    static const Json kNull;
    if (type == Object)
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == key) return vals[i];
    return kNull;
}

bool parseJson(const std::string& text, Json& out) {
    JsonParser jp{text.data(), text.data() + text.size()};
    out = jp.value();
    jp.ws();
    return jp.ok && jp.p == jp.end;
}

// ---- WebSocket ---------------------------------------------------------------------------------
#ifdef _WIN32
// WinHTTP's WebSocket receive cannot be given a timeout or cancelled without ending the connection,
// so a reader thread waits in it and hands whole text messages to recv() through a queue.
struct WebSocket::Impl {
    HINTERNET session = nullptr, conn = nullptr, ws = nullptr;
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool readerDone = true;  // under m
    int closeCode = 0;       // under m
    std::atomic<bool> open{false};

    void readLoop() {
        std::string buf(16 * 1024, '\0');
        std::string msg;
        int code = 0;
        for (;;) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            if (WinHttpWebSocketReceive(ws, buf.data(), DWORD(buf.size()), &got, &type) != NO_ERROR) break;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT status = 0;
                BYTE reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH];
                DWORD reasonLen = 0;
                if (WinHttpWebSocketQueryCloseStatus(ws, &status, reason, sizeof(reason), &reasonLen) == NO_ERROR)
                    code = int(status);
                break;
            }
            const bool binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                                type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
            const bool whole = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                               type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            if (!binary) msg.append(buf.data(), got);
            if (msg.size() > kMaxWsMessage) break;
            if (!whole) continue;
            if (!binary) {
                std::lock_guard<std::mutex> lock(m);
                queue.push_back(std::move(msg));
                cv.notify_all();
            }
            msg.clear();
        }
        std::lock_guard<std::mutex> lock(m);
        readerDone = true;
        closeCode = code;
        open = false;
        cv.notify_all();
    }
};

bool WebSocket::connect(const std::string& url, int timeoutMs, std::string* err, int* httpStatus) {
    close();
    Impl& p = *impl_;
    if (httpStatus) *httpStatus = 0;
    std::string why;
    Url u;
    HINTERNET q = nullptr;
    bool ok = parseWsUrl(url, u);
    if (!ok) why = "bad URL";
    if (ok) {
        p.session = openSession(u);
        ok = p.session != nullptr;
        if (ok) {
            WinHttpSetTimeouts(p.session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
            p.conn = WinHttpConnect(p.session, widen(u.host).c_str(), INTERNET_PORT(u.port), 0);
            q = p.conn ? WinHttpOpenRequest(p.conn, L"GET", widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)
                       : nullptr;
            ok = q != nullptr;
        }
        if (ok) {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(q, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            ok = WinHttpSetOption(q, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                 WinHttpSendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                 WinHttpReceiveResponse(q, nullptr);
        }
        if (!ok) why = winHttpError(GetLastError());
    }
    if (ok) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                            &len, WINHTTP_NO_HEADER_INDEX);
        if (code != 101) {
            // Not the upgrade: an error answer (wrong version, too many connections, lobby full).
            std::string body;
            char buf[2048];
            DWORD got = 0;
            while (body.size() < 4096 && WinHttpReadData(q, buf, DWORD(sizeof(buf)), &got) && got) body.append(buf, got);
            if (httpStatus) *httpStatus = int(code);
            why = refusalText(int(code), body);
            ok = false;
        }
    }
    if (ok) {
        p.ws = WinHttpWebSocketCompleteUpgrade(q, 0);
        ok = p.ws != nullptr;
        if (!ok) why = winHttpError(GetLastError());
    }
    if (q) WinHttpCloseHandle(q);
    if (!ok) {
        if (err) *err = why;
        close();
        return false;
    }
    // A host socket idles between keep-alives: no receive timeout to speak of (the default 30 s would
    // end the connection; the peer's silence is the caller's business).
    DWORD day = 24 * 60 * 60 * 1000;
    WinHttpSetOption(p.ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &day, sizeof(day));
    {
        std::lock_guard<std::mutex> lock(p.m);
        p.queue.clear();
        p.readerDone = false;
        p.closeCode = 0;
        p.open = true;
    }
    p.reader = std::thread([&p] { p.readLoop(); });
    return true;
}

bool WebSocket::sendText(const std::string& text) {
    Impl& p = *impl_;
    if (!p.open || !p.ws) return false;
    return WinHttpWebSocketSend(p.ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                text.empty() ? nullptr : const_cast<char*>(text.data()), DWORD(text.size())) == NO_ERROR;
}

WebSocket::Recv WebSocket::recv(std::string& text, int timeoutMs) {
    Impl& p = *impl_;
    std::unique_lock<std::mutex> lock(p.m);
    p.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs),
                  [&p] { return !p.queue.empty() || p.readerDone; });
    if (!p.queue.empty()) {
        text = std::move(p.queue.front());
        p.queue.pop_front();
        return Recv::Message;
    }
    return p.readerDone ? Recv::Closed : Recv::Timeout;
}

void WebSocket::close() {
    Impl& p = *impl_;
    if (p.ws) {
        // Our close frame; the reader ends when the peer's arrives (or already has). Closing the handle
        // ends a receive that still waits.
        WinHttpWebSocketShutdown(p.ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        {
            std::unique_lock<std::mutex> lock(p.m);
            p.cv.wait_for(lock, std::chrono::milliseconds(1500), [&p] { return p.readerDone; });
        }
        WinHttpCloseHandle(p.ws);
        if (p.reader.joinable()) p.reader.join();
        p.ws = nullptr;
    }
    if (p.reader.joinable()) p.reader.join();
    if (p.conn) WinHttpCloseHandle(p.conn);
    if (p.session) WinHttpCloseHandle(p.session);
    p.conn = p.session = nullptr;
    std::lock_guard<std::mutex> lock(p.m);
    p.queue.clear();
    p.readerDone = true;
    p.open = false;
}

int WebSocket::closeCode() const {
    std::lock_guard<std::mutex> lock(impl_->m);
    return impl_->closeCode;
}
#else
// libcurl connects (DNS, proxy tunnel, TLS; CURLOPT_CONNECT_ONLY) and hands over the connection through
// curl_easy_send / curl_easy_recv; the HTTP upgrade and the RFC 6455 frames are done here. (libcurl's own
// WebSocket API needs 7.86 and is compiled out of many distributions' builds, e.g. Ubuntu 24.04's 8.5.)
struct WebSocket::Impl {
    const CurlApi* api = nullptr;
    void* h = nullptr;
    int sock = -1;
    bool open = false;
    bool closeSent = false;
    int closeCode = 0;
    std::string in;        // received, not parsed yet
    std::string partial;   // a fragmented message so far
    int partialOp = 0;     // its opcode (1 text, 2 binary), 0: none

    // Waits until the socket can be read / written; false on timeout.
    bool waitSock(bool forWrite, int ms) {
        struct pollfd pfd {};
        pfd.fd = sock;
        pfd.events = forWrite ? POLLOUT : POLLIN;
        const int n = poll(&pfd, 1, ms < 0 ? 0 : ms);
        return n > 0;
    }
    bool sendAll(const char* data, size_t n, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        size_t done = 0;
        while (done < n) {
            size_t sent = 0;
            const int rc = api->easy_send(h, data + done, n - done, &sent);
            if (rc == 0) {
                done += sent;
                continue;
            }
            if (rc != kErrAgain) return false;
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            waitSock(true, int(left > 250 ? 250 : left));
        }
        return true;
    }
    // More bytes into `in`: 1 some arrived, 0 none within timeoutMs, -1 the connection ended.
    int fill(int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
        for (;;) {
            char buf[8192];
            size_t n = 0;
            const int rc = api->easy_recv(h, buf, sizeof(buf), &n);
            if (rc == 0) {
                if (n == 0) return -1;  // closed by the peer
                in.append(buf, n);
                return 1;
            }
            if (rc != kErrAgain) return -1;
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return 0;
            // Short waits: data can also sit decrypted inside libcurl's TLS layer, where poll() does not see it.
            waitSock(false, int(left > 250 ? 250 : left));
        }
    }
    bool sendFrame(int opcode, const std::string& payload) {
        std::string f;
        f += char(0x80 | opcode);
        const size_t n = payload.size();
        if (n < 126) {
            f += char(0x80 | n);
        } else if (n < 65536) {
            f += char(0x80 | 126);
            f += char(n >> 8);
            f += char(n & 0xFF);
        } else {
            f += char(0x80 | 127);
            for (int i = 7; i >= 0; i--) f += char(uint8_t(uint64_t(n) >> (8 * i)));
        }
        const uint32_t word = randomWord();
        const char mask[4] = {char(word >> 24), char(word >> 16), char(word >> 8), char(word)};
        f.append(mask, 4);
        for (size_t i = 0; i < n; i++) f += char(payload[i] ^ mask[i & 3]);
        return sendAll(f.data(), f.size(), 5000);
    }
    // One frame out of `in`: 1 taken, 0 not complete yet, -1 not a frame this client accepts.
    int takeFrame(int& opcode, bool& fin, std::string& payload) {
        if (in.size() < 2) return 0;
        const uint8_t b0 = uint8_t(in[0]), b1 = uint8_t(in[1]);
        if (b0 & 0x70) return -1;  // no extensions were negotiated
        opcode = b0 & 15;
        fin = (b0 & 0x80) != 0;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (in.size() < 4) return 0;
            len = uint64_t(uint8_t(in[2])) << 8 | uint8_t(in[3]);
            hdr = 4;
        } else if (len == 127) {
            if (in.size() < 10) return 0;
            len = 0;
            for (size_t i = 0; i < 8; i++) len = len << 8 | uint8_t(in[2 + i]);
            hdr = 10;
        }
        if (len > kMaxWsMessage || (opcode >= 8 && (len > 125 || !fin))) return -1;
        const size_t maskLen = masked ? 4 : 0;
        if (in.size() < hdr + maskLen + size_t(len)) return 0;
        payload.assign(in, hdr + maskLen, size_t(len));
        if (masked)
            for (size_t i = 0; i < payload.size(); i++) payload[i] = char(payload[i] ^ in[hdr + (i & 3)]);
        in.erase(0, hdr + maskLen + size_t(len));
        return 1;
    }
    void drop() {
        if (h) api->easy_cleanup(h);
        h = nullptr;
        sock = -1;
        open = false;
        in.clear();
        partial.clear();
        partialOp = 0;
    }
};

bool WebSocket::connect(const std::string& url, int timeoutMs, std::string* err, int* httpStatus) {
    close();
    Impl& p = *impl_;
    if (httpStatus) *httpStatus = 0;
    p.closeCode = 0;
    p.closeSent = false;
    auto failed = [&](const std::string& why) {
        if (err) *err = why;
        p.drop();
        return false;
    };
    Url u;
    std::string httpUrl;
    if (!parseWsUrl(url, u, &httpUrl)) return failed("bad URL");
    p.api = curl();
    if (!p.api) return failed("libcurl was not found (install the libcurl4 package)");
    if (!p.api->easy_send || !p.api->easy_recv) return failed("this libcurl is too old for online play");
    p.h = p.api->easy_init();
    if (!p.h) return failed("libcurl could not start");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto leftMs = [&] {
        return int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    };
    p.api->easy_setopt(p.h, kOptUrl, httpUrl.c_str());
    p.api->easy_setopt(p.h, kOptUserAgent, kUserAgent);
    p.api->easy_setopt(p.h, kOptFollowLocation, 0L);
    p.api->easy_setopt(p.h, kOptNoSignal, 1L);
    p.api->easy_setopt(p.h, kOptNoProgress, 1L);
    p.api->easy_setopt(p.h, kOptConnectTimeoutMs, long(timeoutMs));
    p.api->easy_setopt(p.h, kOptTimeoutMs, long(timeoutMs));
    p.api->easy_setopt(p.h, kOptConnectOnly, 1L);
    p.api->easy_setopt(p.h, kOptHttpVersion, long(kHttpVersion11));  // TLS must not negotiate HTTP/2
    p.api->easy_setopt(p.h, kOptHttpProxyTunnel, 1L);                // through a proxy: a plain tunnel
    if (isLoopback(u.host)) p.api->easy_setopt(p.h, kOptNoProxy, "*");
    const int rc = p.api->easy_perform(p.h);
    if (rc != 0) return failed(curlErrorText(p.api, rc));
    int sock = -1;
    p.api->easy_getinfo(p.h, kInfoActiveSocket, &sock);
    if (sock < 0) return failed("could not connect (offline or blocked?)");
    p.sock = sock;

    // The upgrade request.
    uint8_t nonce[16];
    for (int i = 0; i < 16; i += 4) {
        const uint32_t w = randomWord();
        nonce[i] = uint8_t(w >> 24);
        nonce[i + 1] = uint8_t(w >> 16);
        nonce[i + 2] = uint8_t(w >> 8);
        nonce[i + 3] = uint8_t(w);
    }
    const std::string key = base64(nonce, 16);
    const bool defaultPort = u.port == (u.scheme == "https" ? 443 : 80);
    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + (defaultPort ? std::string() : ":" + std::to_string(u.port)) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                      "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " + kUserAgent + "\r\n\r\n";
    if (!p.sendAll(req.data(), req.size(), leftMs() > 0 ? leftMs() : 1)) return failed("the connection was reset");
    size_t headEnd = std::string::npos;
    while ((headEnd = p.in.find("\r\n\r\n")) == std::string::npos) {
        if (p.in.size() > 16 * 1024) return failed("the server did not answer as a WebSocket");
        const int left = leftMs();
        if (left <= 0) return failed("the connection timed out");
        const int r = p.fill(left);
        if (r < 0) return failed("the connection was reset");
    }
    const std::string head = lower(p.in.substr(0, headEnd + 2));
    std::string rest = p.in.substr(headEnd + 4);
    int status = 0;
    if (head.rfind("http/1.", 0) == 0 && head.size() > 12) status = std::atoi(head.c_str() + 9);
    auto header = [&](const std::string& name) {  // lower-case name; the value in lower case, trimmed
        const size_t at = head.find("\r\n" + name + ":");
        if (at == std::string::npos) return std::string();
        const size_t start = at + 2 + name.size() + 1;
        const size_t end = head.find("\r\n", start);
        std::string v = head.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        return v;
    };
    if (status != 101) {
        // Not the upgrade: an error answer (wrong version, too many connections, lobby full). Its short body follows.
        const size_t want = size_t(std::atoi(header("content-length").c_str()));
        while (rest.size() < want && rest.size() < 4096 && leftMs() > 0) {
            p.in.clear();
            if (p.fill(leftMs() > 1000 ? 1000 : leftMs()) <= 0) break;
            rest += p.in;
        }
        if (httpStatus) *httpStatus = status;
        return failed(status ? refusalText(status, rest) : "the server did not answer as a WebSocket");
    }
    uint8_t digest[20];
    sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", digest);
    if (header("sec-websocket-accept") != lower(base64(digest, 20))) return failed("the server did not answer as a WebSocket");
    p.in = rest;  // frames that came with the answer
    p.open = true;
    return true;
}

bool WebSocket::sendText(const std::string& text) {
    Impl& p = *impl_;
    if (!p.open) return false;
    if (p.sendFrame(1, text)) return true;
    p.drop();
    return false;
}

WebSocket::Recv WebSocket::recv(std::string& text, int timeoutMs) {
    Impl& p = *impl_;
    if (!p.open) return Recv::Closed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;) {
        for (;;) {
            int op = 0;
            bool fin = false;
            std::string payload;
            const int r = p.takeFrame(op, fin, payload);
            if (r == 0) break;
            bool bad = r < 0;
            if (!bad) {
                if (op == 8) {  // close: answer it, done
                    if (payload.size() >= 2) p.closeCode = int(uint8_t(payload[0])) << 8 | uint8_t(payload[1]);
                    if (!p.closeSent) p.sendFrame(8, payload.substr(0, 2));
                    p.closeSent = true;
                    p.drop();
                    return Recv::Closed;
                } else if (op == 9) {
                    if (!p.sendFrame(10, payload)) bad = true;
                } else if (op == 10) {
                    // a pong nobody asked for
                } else if (op == 1 || op == 2) {
                    if (p.partialOp) bad = true;
                    else if (fin) {
                        if (op == 1) {
                            text = std::move(payload);
                            return Recv::Message;
                        }
                    } else {
                        p.partialOp = op;
                        p.partial = std::move(payload);
                    }
                } else if (op == 0) {
                    if (!p.partialOp || p.partial.size() + payload.size() > kMaxWsMessage) bad = true;
                    else {
                        p.partial += payload;
                        if (fin) {
                            const bool isText = p.partialOp == 1;
                            p.partialOp = 0;
                            if (isText) {
                                text = std::move(p.partial);
                                p.partial.clear();
                                return Recv::Message;
                            }
                            p.partial.clear();
                        }
                    }
                } else {
                    bad = true;
                }
            }
            if (bad) {
                p.drop();
                return Recv::Closed;
            }
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const int r = p.fill(left > 0 ? int(left) : 0);
        if (r < 0) {
            p.drop();
            return Recv::Closed;
        }
        if (r == 0) return Recv::Timeout;
    }
}

void WebSocket::close() {
    Impl& p = *impl_;
    if (p.open && p.h) {
        // Our close frame, then a moment for the peer's (messages still on their way are dropped).
        if (!p.closeSent) {
            const char code[2] = {char(1000 >> 8), char(1000 & 0xFF)};
            p.closeSent = p.sendFrame(8, std::string(code, 2));
        }
        const int saved = p.closeCode;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        std::string dropped;
        while (p.open && p.closeSent) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0 || recv(dropped, int(left)) != Recv::Message) break;
        }
        p.closeCode = saved;  // closeCode() tells who closed: our own close is not the peer's
    }
    p.drop();
}

int WebSocket::closeCode() const { return impl_->closeCode; }
#endif

WebSocket::WebSocket() : impl_(new Impl) {}

WebSocket::~WebSocket() {
    close();
    delete impl_;
}

bool WebSocket::isOpen() const { return impl_->open; }

}  // namespace port::http
