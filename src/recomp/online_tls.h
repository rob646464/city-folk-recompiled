#pragma once
// A blocking TCP stream with optional TLS for the mail client (online_email.cpp): the platform's
// TLS with the OS trust store (Windows: SChannel, certificate and host name checked by the system;
// other systems: libcurl does the TLS and the protocol, see online_email.cpp). Plain TCP only for
// the local test server (CF_MAIL_TEST_PLAIN).
#include <cstdint>
#include <memory>
#include <string>

namespace online::tls {

class Stream {
public:
    virtual ~Stream() = default;
    // Upgrade the plain connection to TLS for `host` (STARTTLS, or right after connect for
    // implicit TLS). False with *err on a failed handshake / certificate check.
    virtual bool startTls(const std::string& host, std::string* err) = 0;
    virtual bool write(const std::string& data) = 0;
    // One line up to CRLF (without it); false on close / timeout.
    virtual bool readLine(std::string& line) = 0;
    // Exactly n bytes.
    virtual bool readN(size_t n, std::string& out) = 0;
    // Server side (the local test server only): TLS with the test certificate.
    virtual bool acceptTls(std::string* err) = 0;
};

// Connects (15 s timeouts). Windows only (elsewhere null: online_email.cpp uses libcurl).
std::unique_ptr<Stream> connect(const std::string& host, int port, std::string* err);

// Test server (tools/tests/cf_mail_test_server.cpp): wraps an accepted socket; a self-signed
// certificate for CN=localhost made on first use (returns its SHA-256, hex).
std::unique_ptr<Stream> fromAccepted(uintptr_t socket);
std::string testCertSha256();

// Test-only trust override: CF_MAIL_TEST_CERT_SHA256=<hex> (environment only, never a setting)
// makes the client accept exactly that certificate instead of checking the OS trust store.

}  // namespace online::tls
