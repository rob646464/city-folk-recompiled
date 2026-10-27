// One HTTP(S) GET into memory (http_get.h). Same transport code as the updater.
#include "port/http_get.h"

#include "port/update.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace port::http {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Url {
    std::string scheme, host, path;
    int port = 0;
};
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
    if (const auto c = hostport.rfind(':'); c != std::string::npos && hostport.find(']') == std::string::npos) {
        u.port = std::atoi(hostport.c_str() + c + 1);
        hostport.resize(c);
    }
    u.host = lower(hostport);
    return (u.scheme == "https" || u.scheme == "http") && !u.host.empty() && u.port > 0 && u.port < 65536;
}

std::string userAgent() { return std::string("CityFolkRecompiled/") + port::update::buildVersion(); }

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string winErr(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "could not resolve the server name (offline?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "could not connect (offline or blocked?)";
    case ERROR_WINHTTP_TIMEOUT: return "the connection timed out";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the secure connection failed (certificate)";
    default: {
        char b[48];
        std::snprintf(b, sizeof(b), "network error %lu", e);
        return b;
    }
    }
}
// One request; location set on a redirect.
void once(const std::string& url, const std::vector<std::string>& headers, size_t maxBytes, int timeoutMs, Result& r,
          std::string& location) {
    Url u;
    if (!parseUrl(url, u)) {
        r.error = "bad URL";
        return;
    }
    HINTERNET s = WinHttpOpen(widen(userAgent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(widen(userAgent()).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        r.error = winErr(GetLastError());
        return;
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
        std::string all;
        for (auto& h : headers) all += h + "\r\n";
        if (!all.empty()) WinHttpAddRequestHeaders(q, widen(all).c_str(), DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        ok = WinHttpSendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
             WinHttpReceiveResponse(q, nullptr);
    }
    if (!ok) r.error = winErr(GetLastError());
    if (ok) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = int(code);
        wchar_t loc[2048];
        len = sizeof(loc);
        if (WinHttpQueryHeaders(q, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &len, WINHTTP_NO_HEADER_INDEX)) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, loc, int(len / sizeof(wchar_t)), nullptr, 0, nullptr, nullptr);
            location.assign(size_t(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, loc, int(len / sizeof(wchar_t)), location.data(), n, nullptr, nullptr);
        }
        std::vector<char> buf(64 * 1024);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(q, buf.data(), DWORD(buf.size()), &got)) {
                r.error = winErr(GetLastError());
                break;
            }
            if (got == 0) break;
            if (r.body.size() + got > maxBytes) {
                r.error = "too large";
                break;
            }
            r.body.insert(r.body.end(), buf.data(), buf.data() + got);
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
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
    void* (*slist_append)(void*, const char*) = nullptr;
    void (*slist_free_all)(void*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
};
enum : int {
    kOptWriteData = 10001, kOptUrl = 10002, kOptUserAgent = 10018, kOptHttpHeader = 10023, kOptHeaderData = 10029,
    kOptWriteFunction = 20011, kOptHeaderFunction = 20079, kOptNoProgress = 43, kOptFollowLocation = 52,
    kOptNoSignal = 99, kOptTimeoutMs = 155, kOptConnectTimeoutMs = 156, kInfoResponseCode = 0x200000 + 2,
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
        api.slist_append = reinterpret_cast<void* (*)(void*, const char*)>(sym("curl_slist_append"));
        api.slist_free_all = reinterpret_cast<void (*)(void*)>(sym("curl_slist_free_all"));
        api.easy_strerror = reinterpret_cast<const char* (*)(int)>(sym("curl_easy_strerror"));
        if (!api.global_init || !api.easy_init || !api.easy_setopt || !api.easy_perform || !api.easy_getinfo ||
            !api.easy_cleanup || !api.slist_append || !api.slist_free_all || !api.easy_strerror) {
            dlclose(api.lib);
            api.lib = nullptr;
            return;
        }
        api.global_init(3);
    });
    return api.lib ? &api : nullptr;
}
struct Ctx {
    Result* r;
    size_t max;
    std::string* location;
};
size_t onData(char* d, size_t sz, size_t n, void* u) {
    auto* c = static_cast<Ctx*>(u);
    if (c->r->body.size() + sz * n > c->max) {
        c->r->error = "too large";
        return 0;
    }
    c->r->body.insert(c->r->body.end(), d, d + sz * n);
    return sz * n;
}
size_t onHeader(char* d, size_t sz, size_t n, void* u) {
    auto* c = static_cast<Ctx*>(u);
    std::string line(d, sz * n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    const auto colon = line.find(':');
    if (colon != std::string::npos && lower(line.substr(0, colon)) == "location") {
        std::string v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        *c->location = v;
    }
    return sz * n;
}
void once(const std::string& url, const std::vector<std::string>& headers, size_t maxBytes, int timeoutMs, Result& r,
          std::string& location) {
    const CurlApi* api = curl();
    if (!api) {
        r.error = "libcurl was not found (install the libcurl4 package)";
        return;
    }
    void* h = api->easy_init();
    if (!h) {
        r.error = "libcurl could not start";
        return;
    }
    Ctx ctx{&r, maxBytes, &location};
    void* list = nullptr;
    for (auto& hd : headers) list = api->slist_append(list, hd.c_str());
    const std::string ua = userAgent();
    api->easy_setopt(h, kOptUrl, url.c_str());
    api->easy_setopt(h, kOptUserAgent, ua.c_str());
    if (list) api->easy_setopt(h, kOptHttpHeader, list);
    api->easy_setopt(h, kOptFollowLocation, 0L);
    api->easy_setopt(h, kOptNoSignal, 1L);
    api->easy_setopt(h, kOptNoProgress, 1L);
    api->easy_setopt(h, kOptConnectTimeoutMs, long(timeoutMs));
    api->easy_setopt(h, kOptTimeoutMs, long(timeoutMs * 4));
    api->easy_setopt(h, kOptWriteFunction, &onData);
    api->easy_setopt(h, kOptWriteData, &ctx);
    api->easy_setopt(h, kOptHeaderFunction, &onHeader);
    api->easy_setopt(h, kOptHeaderData, &ctx);
    const int rc = api->easy_perform(h);
    long code = 0;
    api->easy_getinfo(h, kInfoResponseCode, &code);
    r.status = int(code);
    api->easy_cleanup(h);
    if (list) api->slist_free_all(list);
    if (rc != 0 && r.error.empty()) r.error = api->easy_strerror(rc);
}
#endif

}  // namespace

Result get(const std::string& url, const std::vector<std::string>& headers, size_t maxBytes, int timeoutMs) {
    std::string cur = url;
    for (int hop = 0;; hop++) {
        Result r;
        std::string location;
        once(cur, headers, maxBytes, timeoutMs, r, location);
        const bool redirect = r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
        if (!redirect || location.empty() || hop >= 3) return r;
        if (location.rfind("http://", 0) != 0 && location.rfind("https://", 0) != 0) {
            Url u;
            if (!parseUrl(cur, u)) return r;
            location = u.scheme + "://" + u.host + (location[0] == '/' ? "" : "/") + location;
        }
        cur = location;
    }
}

}  // namespace port::http
