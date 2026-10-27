#pragma once
// One HTTP(S) GET into memory for features that fetch small files (Nintendo event archive,
// docs/DISTRIBUTIONS.md). WinHTTP on Windows, libcurl loaded at run time elsewhere (the same
// approach as the updater, src/port/update.cpp). Blocking: call it from a worker thread.
#include <string>
#include <vector>

namespace port::http {

struct Result {
    int status = 0;     // HTTP status (0 = no answer)
    std::string error;  // transport failure, or "too large"; empty when an answer came
    std::vector<unsigned char> body;
};

// Redirects (301/302/307/308) are followed up to 3 times within http/https. `headers` are
// "Name: value" lines (e.g. a Host header for a revival service's address). The body is cut
// with error "too large" past maxBytes.
Result get(const std::string& url, const std::vector<std::string>& headers = {}, size_t maxBytes = 8u << 20,
           int timeoutMs = 15000);

}  // namespace port::http
