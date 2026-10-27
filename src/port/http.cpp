// Small HTTPS GET client (see http.h). Transport code follows the updater's (src/port/update.cpp).
#include "port/http.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace port::http {
namespace {

constexpr const char* kUserAgent = "CityFolkRecompiled (game feature)";

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

Result getImpl(const std::string& url, int timeoutMs, size_t maxBytes) {
    Result r;
    Url u;
    if (!parseUrl(url, u)) {
        r.error = "bad URL";
        return r;
    }
    const auto start = std::chrono::steady_clock::now();
    HINTERNET s = WinHttpOpen(widen(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(widen(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        r.error = winHttpError(GetLastError());
        return r;
    }
    WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET c = WinHttpConnect(s, widen(u.host).c_str(), INTERNET_PORT(u.port), 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    bool ok = q != nullptr;
    if (ok) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(q, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        ok = WinHttpSendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
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
};
// libcurl's stable option / info / error numbers.
enum : int {
    kOptWriteData = 10001, kOptUrl = 10002, kOptUserAgent = 10018, kOptWriteFunction = 20011,
    kOptNoProgress = 43, kOptFollowLocation = 52, kOptNoSignal = 99, kOptTimeoutMs = 155, kOptConnectTimeoutMs = 156,
    kInfoResponseCode = 0x200000 + 2,
    kErrResolve = 6, kErrConnect = 7, kErrWrite = 23, kErrTimeout = 28,
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
        api.global_init(3);  // CURL_GLOBAL_DEFAULT
    });
    return api.lib ? &api : nullptr;
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

Result getImpl(const std::string& url, int timeoutMs, size_t maxBytes) {
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
    api->easy_setopt(h, kOptUrl, url.c_str());
    api->easy_setopt(h, kOptUserAgent, kUserAgent);
    api->easy_setopt(h, kOptFollowLocation, 0L);
    api->easy_setopt(h, kOptNoSignal, 1L);
    api->easy_setopt(h, kOptNoProgress, 1L);
    api->easy_setopt(h, kOptConnectTimeoutMs, long(timeoutMs));
    api->easy_setopt(h, kOptTimeoutMs, long(2 * timeoutMs));
    api->easy_setopt(h, kOptWriteFunction, &curlWrite);
    api->easy_setopt(h, kOptWriteData, &sink);
    const int rc = api->easy_perform(h);
    long code = 0;
    api->easy_getinfo(h, kInfoResponseCode, &code);
    r.status = int(code);
    api->easy_cleanup(h);
    if (rc == 0) return r;
    switch (rc) {
    case kErrResolve: r.error = "could not resolve the server name (offline?)"; break;
    case kErrConnect: r.error = "could not connect (offline or blocked?)"; break;
    case kErrTimeout: r.error = "the connection timed out"; break;
    case kErrWrite: r.error = sink.tooLarge ? "the answer was too large" : api->easy_strerror(rc); break;
    default: r.error = api->easy_strerror(rc); break;
    }
    return r;
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

Result get(const std::string& url, int timeoutMs, size_t maxBytes) { return getImpl(url, timeoutMs, maxBytes); }

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

}  // namespace port::http
