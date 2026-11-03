// TCP + TLS stream for the mail client (online_tls.h). Windows: SChannel through SSPI, written from
// the public SSPI documentation; the system validates the server certificate chain against the OS
// trust store and the host name (SCH_CRED_AUTO_CRED_VALIDATION + the target name).
#include "recomp/online_tls.h"

#include "recomp/online_sock.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#define SECURITY_WIN32
#include <schannel.h>
#include <security.h>
#include <sspi.h>


#endif

namespace online::tls {
namespace sk = ::online::sock;

#if defined(_WIN32)
namespace {

std::wstring widen(const std::string& s) {
    std::wstring w;
    for (char c : s) w += wchar_t(static_cast<unsigned char>(c));
    return w;
}

std::string secErr(SECURITY_STATUS st) {
    switch (st) {
    case SEC_E_UNTRUSTED_ROOT: return "the server's certificate is not trusted";
    case SEC_E_CERT_EXPIRED: return "the server's certificate has expired";
    case SEC_E_WRONG_PRINCIPAL: return "the server's certificate is for another name";
    case CERT_E_CN_NO_MATCH: return "the server's certificate is for another name";
    default: {
        char b[64];
        std::snprintf(b, sizeof b, "TLS error 0x%08lX", static_cast<unsigned long>(st));
        return b;
    }
    }
}

std::string sha256Hex(PCCERT_CONTEXT c) {
    BYTE h[32];
    DWORD n = sizeof h;
    if (!CertGetCertificateContextProperty(c, CERT_SHA256_HASH_PROP_ID, h, &n)) return {};
    std::string s;
    for (DWORD i = 0; i < n; i++) {
        char b[3];
        std::snprintf(b, sizeof b, "%02x", h[i]);
        s += b;
    }
    return s;
}

PCCERT_CONTEXT testCert() {
    static std::once_flag once;
    static PCCERT_CONTEXT cert = nullptr;
    std::call_once(once, [] {
        BYTE name[256];
        DWORD n = sizeof name;
        if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=localhost", CERT_X500_NAME_STR, nullptr, name, &n, nullptr)) return;
        CERT_NAME_BLOB subject{n, name};
        cert = CertCreateSelfSignCertificate(0, &subject, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
    });
    return cert;
}

class WinStream final : public Stream {
public:
    sk::Socket s = sk::kInvalid;
    bool tls = false;
    CredHandle cred{};
    CtxtHandle ctx{};
    bool haveCred = false, haveCtx = false;
    SecPkgContext_StreamSizes sizes{};
    std::string in;    // decrypted (or plain) bytes not yet consumed
    std::string raw;   // received TLS records not yet decrypted

    ~WinStream() override {
        if (haveCtx) DeleteSecurityContext(&ctx);
        if (haveCred) FreeCredentialsHandle(&cred);
        if (s != sk::kInvalid) sk::closeSocket(s);
    }

    bool sendAll(const char* p, size_t n) {
        while (n) {
            const int k = int(::send(s, p, int(n), 0));
            if (k <= 0) return false;
            p += k;
            n -= size_t(k);
        }
        return true;
    }
    bool recvSome(std::string& to) {
        char b[16384];
        const int k = int(::recv(s, b, sizeof b, 0));
        if (k <= 0) return false;
        to.append(b, size_t(k));
        return true;
    }

    bool startTls(const std::string& host, std::string* err) override {
        SCHANNEL_CRED sc{};
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        const char* pin = std::getenv("CF_MAIL_TEST_CERT_SHA256");  // test-only trust override
        sc.dwFlags = (pin && *pin ? SCH_CRED_MANUAL_CRED_VALIDATION : SCH_CRED_AUTO_CRED_VALIDATION) | SCH_USE_STRONG_CRYPTO |
                     SCH_CRED_NO_DEFAULT_CREDS;
        TimeStamp exp;
        SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr,
                                                       &sc, nullptr, nullptr, &cred, &exp);
        if (st != SEC_E_OK) {
            if (err) *err = secErr(st);
            return false;
        }
        haveCred = true;
        const std::wstring target = widen(host);
        const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                            ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;
        std::string buf = std::move(in);
        in.clear();
        bool first = true;
        while (true) {
            SecBuffer inB[2] = {{ULONG(buf.size()), SECBUFFER_TOKEN, buf.empty() ? nullptr : buf.data()}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc inD = {SECBUFFER_VERSION, 2, inB};
            SecBuffer outB[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc outD = {SECBUFFER_VERSION, 1, outB};
            DWORD outFlags = 0;
            st = InitializeSecurityContextW(&cred, first ? nullptr : &ctx, const_cast<wchar_t*>(target.c_str()), flags, 0, 0,
                                            first ? nullptr : &inD, 0, first ? &ctx : nullptr, &outD, &outFlags, &exp);
            if (first) haveCtx = true;
            first = false;
            if (outB[0].cbBuffer && outB[0].pvBuffer) {
                const bool ok = sendAll(static_cast<const char*>(outB[0].pvBuffer), outB[0].cbBuffer);
                FreeContextBuffer(outB[0].pvBuffer);
                if (!ok) {
                    if (err) *err = "the connection was closed during the TLS handshake";
                    return false;
                }
            }
            if (st == SEC_E_INCOMPLETE_MESSAGE) {
                if (!recvSome(buf)) {
                    if (err) *err = "the connection was closed during the TLS handshake";
                    return false;
                }
                continue;
            }
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) {
                // Unused input stays for the next step / the data phase.
                std::string rest;
                if (inB[1].BufferType == SECBUFFER_EXTRA && inB[1].cbBuffer) rest = buf.substr(buf.size() - inB[1].cbBuffer);
                buf = rest;
                if (st == SEC_E_OK) {
                    raw = buf;
                    break;
                }
                if (buf.empty() && !recvSome(buf)) {
                    if (err) *err = "the connection was closed during the TLS handshake";
                    return false;
                }
                continue;
            }
            if (err) *err = secErr(st);
            return false;
        }
        QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes);
        if (pin && *pin) {
            PCCERT_CONTEXT rc = nullptr;
            std::string fp;
            if (QueryContextAttributesW(&ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &rc) == SEC_E_OK && rc) {
                fp = sha256Hex(rc);
                CertFreeCertificateContext(rc);
            }
            if (fp != pin) {
                if (err) *err = "the server's certificate is not the test certificate (CF_MAIL_TEST_CERT_SHA256)";
                return false;
            }
        }
        tls = true;
        return true;
    }

    bool acceptTls(std::string* err) override {
        PCCERT_CONTEXT cert = testCert();
        if (!cert) {
            if (err) *err = "no test certificate";
            return false;
        }
        SCHANNEL_CRED sc{};
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.cCreds = 1;
        sc.paCred = &cert;
        TimeStamp exp;
        SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr, &sc,
                                                       nullptr, nullptr, &cred, &exp);
        if (st != SEC_E_OK) {
            if (err) *err = secErr(st);
            return false;
        }
        haveCred = true;
        std::string buf = std::move(in);
        in.clear();
        bool first = true;
        const DWORD flags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
        while (true) {
            if (buf.empty() && !recvSome(buf)) return false;
            SecBuffer inB[2] = {{ULONG(buf.size()), SECBUFFER_TOKEN, buf.data()}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc inD = {SECBUFFER_VERSION, 2, inB};
            SecBuffer outB[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc outD = {SECBUFFER_VERSION, 1, outB};
            DWORD outFlags = 0;
            st = AcceptSecurityContext(&cred, first ? nullptr : &ctx, &inD, flags, 0, first ? &ctx : nullptr, &outD, &outFlags, &exp);
            if (st == SEC_E_INCOMPLETE_MESSAGE) {
                if (!recvSome(buf)) return false;
                continue;
            }
            if (first && (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED)) haveCtx = true;
            first = false;
            if (outB[0].cbBuffer && outB[0].pvBuffer) {
                const bool ok = sendAll(static_cast<const char*>(outB[0].pvBuffer), outB[0].cbBuffer);
                FreeContextBuffer(outB[0].pvBuffer);
                if (!ok) return false;
            }
            if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED) {
                std::string rest;
                if (inB[1].BufferType == SECBUFFER_EXTRA && inB[1].cbBuffer) rest = buf.substr(buf.size() - inB[1].cbBuffer);
                buf = rest;
                if (st == SEC_E_OK) {
                    raw = buf;
                    break;
                }
                continue;
            }
            if (err) *err = secErr(st);
            return false;
        }
        QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes);
        tls = true;
        return true;
    }

    bool write(const std::string& data) override {
        if (!tls) return sendAll(data.data(), data.size());
        size_t off = 0;
        while (off < data.size()) {
            const size_t n = std::min<size_t>(data.size() - off, sizes.cbMaximumMessage);
            std::vector<char> b(sizes.cbHeader + n + sizes.cbTrailer);
            std::memcpy(b.data() + sizes.cbHeader, data.data() + off, n);
            SecBuffer sb[4] = {{sizes.cbHeader, SECBUFFER_STREAM_HEADER, b.data()},
                               {ULONG(n), SECBUFFER_DATA, b.data() + sizes.cbHeader},
                               {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, b.data() + sizes.cbHeader + n},
                               {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d = {SECBUFFER_VERSION, 4, sb};
            if (EncryptMessage(&ctx, 0, &d, 0) != SEC_E_OK) return false;
            if (!sendAll(b.data(), sb[0].cbBuffer + sb[1].cbBuffer + sb[2].cbBuffer)) return false;
            off += n;
        }
        return true;
    }

    bool fill() {
        if (!tls) return recvSome(in);
        while (true) {
            if (!raw.empty()) {
                SecBuffer sb[4] = {{ULONG(raw.size()), SECBUFFER_DATA, raw.data()}, {0, SECBUFFER_EMPTY, nullptr},
                                   {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
                SecBufferDesc d = {SECBUFFER_VERSION, 4, sb};
                const SECURITY_STATUS st = DecryptMessage(&ctx, &d, 0, nullptr);
                if (st == SEC_E_OK) {
                    std::string extra;
                    for (auto& b : sb) {
                        if (b.BufferType == SECBUFFER_DATA) in.append(static_cast<char*>(b.pvBuffer), b.cbBuffer);
                        if (b.BufferType == SECBUFFER_EXTRA) extra.assign(static_cast<char*>(b.pvBuffer), b.cbBuffer);
                    }
                    raw = extra;
                    return true;
                }
                if (st != SEC_E_INCOMPLETE_MESSAGE) return false;  // close notify, renegotiation, errors
            }
            if (!recvSome(raw)) return false;
        }
    }

    bool readLine(std::string& line) override {
        while (true) {
            const size_t p = in.find("\r\n");
            if (p != std::string::npos) {
                line = in.substr(0, p);
                in.erase(0, p + 2);
                return true;
            }
            if (in.size() > 1024 * 1024 || !fill()) return false;
        }
    }

    bool readN(size_t n, std::string& out) override {
        while (in.size() < n)
            if (!fill()) return false;
        out = in.substr(0, n);
        in.erase(0, n);
        return true;
    }
};

}  // namespace

std::unique_ptr<Stream> connect(const std::string& host, int port, std::string* err) {
    sk::startup();
    sk::Addr a;
    if (!sk::resolve(host, uint16_t(port), &a)) {
        if (err) *err = "could not find " + host;
        return nullptr;
    }
    a = a.normalized();
    auto st = std::make_unique<WinStream>();
    st->s = socket(a.family(), SOCK_STREAM, IPPROTO_TCP);
    if (st->s == sk::kInvalid) {
        if (err) *err = "could not create a socket";
        return nullptr;
    }
    DWORD tmo = 15000;
    setsockopt(st->s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
    setsockopt(st->s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
    if (::connect(st->s, reinterpret_cast<const sockaddr*>(&a.ss), a.len) != 0) {
        if (err) *err = "could not connect to " + host + ":" + std::to_string(port);
        return nullptr;
    }
    return st;
}
std::unique_ptr<Stream> fromAccepted(uintptr_t socket) {
    auto st = std::make_unique<WinStream>();
    st->s = sk::Socket(socket);
    DWORD tmo = 15000;
    setsockopt(st->s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
    return st;
}

std::string testCertSha256() {
    PCCERT_CONTEXT c = testCert();
    return c ? sha256Hex(c) : std::string();
}
#else
std::unique_ptr<Stream> fromAccepted(uintptr_t) { return nullptr; }
std::string testCertSha256() { return {}; }
std::unique_ptr<Stream> connect(const std::string&, int, std::string* err) {
    if (err) *err = "not used on this system (libcurl)";
    return nullptr;
}
#endif

}  // namespace online::tls
