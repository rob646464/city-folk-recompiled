// cf_mail_test_server: a tiny SMTP + IMAP server for testing the game's email letters (docs/ONLINE.md
// "Always online mail") without a real account. Test aid only: any password accepted, everything
// in memory. TLS through SChannel with a self-signed CN=localhost certificate made at start: the
// client trusts it only with CF_MAIL_TEST_CERT_SHA256=<the printed fingerprint> (test-only override).
//
//   cf_mail_test_server [--smtp 39925] [--imap 39943] [--implicit-tls] [--cert-file <path>]
//
// Without --implicit-tls the ports are plain with STARTTLS offered (and required before AUTH /
// LOGIN); with it, TLS starts at connect (465 / 993 style). The fingerprint goes to stdout and,
// with --cert-file, into that file.
// SMTP: EHLO/HELO, STARTTLS, AUTH PLAIN, MAIL FROM, RCPT TO, DATA, RSET, NOOP, QUIT.
// IMAP: CAPABILITY, STARTTLS, LOGIN, SELECT/EXAMINE, CREATE, UID SEARCH SUBJECT "...", UID FETCH <uid>
// BODY.PEEK[], UID MOVE <uid> <folder>, NOOP, LOGOUT. Mailboxes are per login name.
#include "recomp/online_sock.h"
#include "recomp/online_tls.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace online::sock;
namespace tls = ::online::tls;

namespace {

bool sImplicit = false;

struct Msg {
    unsigned uid;
    std::string folder, data;
};
std::mutex sM;
std::map<std::string, std::vector<Msg>> sBoxes;  // address -> messages
unsigned sNextUid = 1;

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string addrOf(const std::string& l) {
    const size_t a = l.find('<'), b = l.find('>');
    return lower(a != std::string::npos && b != std::string::npos ? l.substr(a + 1, b - a - 1) : l.substr(l.find(':') + 1));
}

void smtp(Socket s) {
    auto c = tls::fromAccepted(uintptr_t(s));
    std::string err;
    bool secure = false;
    if (sImplicit) {
        if (!c->acceptTls(&err)) return std::printf("[mail-test] SMTP TLS failed: %s\n", err.c_str()), void();
        secure = true;
    }
    c->write("220 cf-mail-test ESMTP\r\n");
    std::vector<std::string> rcpt;
    std::string l;
    while (c->readLine(l)) {
        const std::string u = lower(l.substr(0, 4));
        if (u == "ehlo") c->write(secure ? "250-cf-mail-test\r\n250-AUTH PLAIN\r\n250 8BITMIME\r\n" : "250-cf-mail-test\r\n250 STARTTLS\r\n");
        else if (u == "helo") c->write("250 cf-mail-test\r\n");
        else if (lower(l) == "starttls" && !secure) {
            c->write("220 go ahead\r\n");
            if (!c->acceptTls(&err)) return std::printf("[mail-test] SMTP STARTTLS failed: %s\n", err.c_str()), void();
            secure = true;
        } else if (u == "auth") c->write(secure ? "235 ok\r\n" : "530 STARTTLS first\r\n");
        else if (u == "mail") rcpt.clear(), c->write("250 ok\r\n");
        else if (u == "rcpt") rcpt.push_back(addrOf(l)), c->write("250 ok\r\n");
        else if (u == "data") {
            c->write("354 go\r\n");
            std::string data, d;
            while (c->readLine(d) && d != ".") data += (d.rfind("..", 0) == 0 ? d.substr(1) : d) + "\r\n";
            std::lock_guard<std::mutex> g(sM);
            for (auto& r : rcpt) sBoxes[r].push_back({sNextUid++, "INBOX", data});
            std::printf("[mail-test] message for %zu recipient(s), %zu bytes\n", rcpt.size(), data.size());
            std::fflush(stdout);
            c->write("250 queued\r\n");
        } else if (u == "quit") {
            c->write("221 bye\r\n");
            break;
        } else c->write("250 ok\r\n");
    }
}

std::string arg(const std::string& l, size_t i) {  // i-th space-separated word (quotes kept together)
    std::vector<std::string> w;
    std::string cur;
    bool q = false;
    for (char ch : l) {
        if (ch == '"') q = !q;
        if (ch == ' ' && !q) {
            if (!cur.empty()) w.push_back(cur);
            cur.clear();
        } else cur += ch;
    }
    if (!cur.empty()) w.push_back(cur);
    return i < w.size() ? w[i] : std::string();
}
std::string unq(std::string s) {
    if (s.size() >= 2 && s.front() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

void imap(Socket s) {
    auto c = tls::fromAccepted(uintptr_t(s));
    std::string err;
    bool secure = false;
    if (sImplicit) {
        if (!c->acceptTls(&err)) return std::printf("[mail-test] IMAP TLS failed: %s\n", err.c_str()), void();
        secure = true;
    }
    c->write("* OK cf-mail-test IMAP4rev1\r\n");
    std::string user, folder = "INBOX", l;
    while (c->readLine(l)) {
        const std::string tag = arg(l, 0), cmd = lower(arg(l, 1));
        if (cmd == "capability") c->write(std::string("* CAPABILITY IMAP4rev1 MOVE UIDPLUS") + (secure ? "" : " STARTTLS") + "\r\n" + tag + " OK\r\n");
        else if (cmd == "starttls" && !secure) {
            c->write(tag + " OK begin TLS\r\n");
            if (!c->acceptTls(&err)) return std::printf("[mail-test] IMAP STARTTLS failed: %s\n", err.c_str()), void();
            secure = true;
        } else if (cmd == "login") {
            if (!secure) c->write(tag + " NO STARTTLS first\r\n");
            else user = lower(unq(arg(l, 2))), c->write(tag + " OK logged in\r\n");
        } else if (cmd == "select" || cmd == "examine") folder = unq(arg(l, 2)), c->write("* FLAGS ()\r\n" + tag + " OK [READ-WRITE] selected\r\n");
        else if (cmd == "create") c->write(tag + " OK created\r\n");
        else if (cmd == "noop") c->write(tag + " OK\r\n");
        else if (cmd == "logout") {
            c->write("* BYE\r\n" + tag + " OK\r\n");
            break;
        } else if (cmd == "uid") {
            const std::string sub = lower(arg(l, 2));
            std::lock_guard<std::mutex> g(sM);
            auto& box = sBoxes[user];
            if (sub == "search") {
                const std::string want = unq(arg(l, 4));
                std::string r = "* SEARCH";
                for (auto& m : box)
                    if (lower(m.folder) == lower(folder) && m.data.find(want) != std::string::npos) r += " " + std::to_string(m.uid);
                c->write(r + "\r\n" + tag + " OK\r\n");
            } else if (sub == "fetch") {
                const unsigned uid = unsigned(std::strtoul(arg(l, 3).c_str(), nullptr, 10));
                for (auto& m : box)
                    if (m.uid == uid)
                        c->write("* 1 FETCH (UID " + std::to_string(uid) + " BODY[] {" + std::to_string(m.data.size()) + "}\r\n" + m.data + ")\r\n");
                c->write(tag + " OK\r\n");
            } else if (sub == "move") {
                const unsigned uid = unsigned(std::strtoul(arg(l, 3).c_str(), nullptr, 10));
                for (auto& m : box)
                    if (m.uid == uid) m.folder = unq(arg(l, 4));
                std::printf("[mail-test] message %u moved to %s\n", uid, unq(arg(l, 4)).c_str());
                std::fflush(stdout);
                c->write(tag + " OK moved\r\n");
            } else c->write(tag + " BAD\r\n");
        } else c->write(tag + " BAD unknown\r\n");
    }
}

void serve(int port, void (*fn)(Socket)) {
    Socket t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(t, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(0x7F000001u);
    if (bind(t, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(t, 8) != 0) {
        std::fprintf(stderr, "port %d in use\n", port);
        std::exit(1);
    }
    while (true) {
        Socket c = accept(t, nullptr, nullptr);
        if (c != kInvalid) std::thread(fn, c).detach();
    }
}

}  // namespace

int main(int argc, char** argv) {
    int sp = 39925, ip = 39943;
    std::string certFile;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--smtp") && i + 1 < argc) sp = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--imap") && i + 1 < argc) ip = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--implicit-tls")) sImplicit = true;
        else if (!std::strcmp(argv[i], "--cert-file") && i + 1 < argc) certFile = argv[++i];
    }
    startup();
    const std::string fp = tls::testCertSha256();
    if (!certFile.empty()) std::ofstream(certFile) << fp;
    std::printf("cf_mail_test_server: SMTP 127.0.0.1:%d, IMAP 127.0.0.1:%d, %s; test certificate SHA-256 %s\n", sp, ip,
                sImplicit ? "implicit TLS" : "STARTTLS", fp.c_str());
    std::fflush(stdout);
    std::thread(serve, sp, &smtp).detach();
    serve(ip, &imap);
}
