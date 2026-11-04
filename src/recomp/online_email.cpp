// "Always online mail" (online_email.h, docs/ONLINE.md "Letters: A. email").
//
// Out: for each outbox letter whose friend has told us its address (friends EMAIL, in a room), one
// mail by SMTP: subject "[City Folk letter] from <player>", a short plain-text body for people, and
// the letter as an attachment "letter.cfl" = "CFEM" 1 | from key[32] | to key[32] | sealed blob
// (sealed for the friend pair and signed, online_mail.cpp sealFor).
// In: IMAP, INBOX, UID SEARCH SUBJECT "[City Folk letter]"; each hit is fetched (BODY.PEEK[], so it
// stays unread), and only an attachment that is addressed to us, comes from a friend's key and
// opens is taken in; the mail is then moved to the "City Folk" folder (UID MOVE when the server
// has it, else left where it is and remembered by UID). Everything else is not touched.
// Worker thread: at start and every 5 minutes (CF_MAIL_POLL_S for tests), never a game or render
// thread. TLS only: implicit (993/465) or STARTTLS (143/587), with the OS certificate checks
// (online_tls.cpp; test-only override CF_MAIL_TEST_CERT_SHA256).
#include "recomp/online_email.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/menu.h"
#include "recomp/online.h"
#include "recomp/online_crypto.h"
#include "recomp/online_friends.h"
#include "recomp/online_mail.h"
#include "recomp/online_tls.h"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace recomp::online::email {
namespace {

namespace tls = ::online::tls;
namespace oc = ::onlinecrypto;
using u8 = uint8_t;
using u64 = uint64_t;

const char* kSubjectTag = "[City Folk letter]";

std::mutex sMutex;
std::string sStatus = "Off";
std::atomic<bool> sRunning{false}, sWake{false}, sTestWanted{false};

void status(const std::string& s) {
    std::lock_guard<std::mutex> g(sMutex);
    sStatus = s;
    if (online::logOn()) std::printf("[email] %s\n", s.c_str());
}

std::string b64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = unsigned(u8(in[i])) << 16 | unsigned(u8(in[i + 1])) << 8 | u8(in[i + 2]);
        o += t[v >> 18], o += t[(v >> 12) & 63], o += t[(v >> 6) & 63], o += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = unsigned(u8(in[i])) << 16;
        o += t[v >> 18], o += t[(v >> 12) & 63], o += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = unsigned(u8(in[i])) << 16 | unsigned(u8(in[i + 1])) << 8;
        o += t[v >> 18], o += t[(v >> 12) & 63], o += t[(v >> 6) & 63], o += '=';
    }
    return o;
}
std::string unb64(const std::string& in) {
    std::string o;
    unsigned v = 0;
    int bits = 0;
    for (char c : in) {
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (d < 0) continue;
        v = v << 6 | unsigned(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o += char((v >> bits) & 0xFF);
        }
    }
    return o;
}

std::string passwordFor(const std::string& address);

struct Account {
    std::string address, imapHost, smtpHost, password;
    int imapPort = 993, smtpPort = 465;
};

bool account(Account& a, std::string* why) {
    const auto& o = port::settings().online;
    a.address = o.emailAddress;
    a.imapHost = o.emailImapHost;
    a.smtpHost = o.emailSmtpHost;
    a.imapPort = o.emailImapPort;
    a.smtpPort = o.emailSmtpPort;
    if (a.address.find('@') == std::string::npos || a.imapHost.empty() || a.smtpHost.empty()) {
        *why = "Enter your address and the servers";
        return false;
    }
    a.password = passwordFor(a.address);
    if (a.password.empty()) {
        *why = "No password stored for " + a.address;
        return false;
    }
    return true;
}

// ---- connections ----
bool expect(tls::Stream& s, const char* code, std::string* last) {  // SMTP reply (multi-line)
    std::string l;
    do {
        if (!s.readLine(l)) return false;
        if (last) *last = l;
    } while (l.size() > 3 && l[3] == '-');
    return l.rfind(code, 0) == 0;
}

std::unique_ptr<tls::Stream> open(const std::string& host, int port, bool implicit, std::string* err) {
    auto s = tls::connect(host, port, err);
    if (!s) return nullptr;
    if (implicit && !s->startTls(host, err)) return nullptr;
    return s;
}

bool smtpSend(const Account& a, const std::string& to, const std::string& message, std::string* err) {
    const bool implicit = a.smtpPort == 465 || std::getenv("CF_MAIL_TEST_IMPLICIT");  // test aid: implicit TLS on other ports
    auto s = open(a.smtpHost, a.smtpPort, implicit, err);
    if (!s) return false;
    std::string last;
    if (!expect(*s, "220", &last)) return *err = "SMTP: " + last, false;
    s->write("EHLO cityfolk.local\r\n");
    if (!expect(*s, "250", &last)) return *err = "SMTP: " + last, false;
    if (!implicit) {
        s->write("STARTTLS\r\n");
        if (!expect(*s, "220", &last)) return *err = "SMTP: the server offers no STARTTLS (" + last + ")", false;
        if (!s->startTls(a.smtpHost, err)) return false;
        s->write("EHLO cityfolk.local\r\n");
        if (!expect(*s, "250", &last)) return *err = "SMTP: " + last, false;
    }
    s->write("AUTH PLAIN " + b64(std::string(1, '\0') + a.address + std::string(1, '\0') + a.password) + "\r\n");
    if (!expect(*s, "235", &last)) return *err = "SMTP sign-in refused (app password?): " + last, false;
    s->write("MAIL FROM:<" + a.address + ">\r\n");
    if (!expect(*s, "250", &last)) return *err = "SMTP: " + last, false;
    s->write("RCPT TO:<" + to + ">\r\n");
    if (!expect(*s, "25", &last)) return *err = "SMTP: " + last, false;
    s->write("DATA\r\n");
    if (!expect(*s, "354", &last)) return *err = "SMTP: " + last, false;
    std::string body;
    size_t i = 0;
    while (i < message.size()) {  // dot-stuffing
        size_t e = message.find("\r\n", i);
        if (e == std::string::npos) e = message.size();
        std::string line = message.substr(i, e - i);
        if (!line.empty() && line[0] == '.') line = "." + line;
        body += line + "\r\n";
        i = e + 2;
    }
    s->write(body + ".\r\n");
    if (!expect(*s, "250", &last)) return *err = "SMTP: " + last, false;
    s->write("QUIT\r\n");
    return true;
}

struct Imap {
    std::unique_ptr<tls::Stream> s;
    int tag = 0;
    bool move = false;
    // Sends a command; collects untagged lines (with literals appended) until the tagged reply.
    bool cmd(const std::string& c, std::vector<std::string>* lines, std::string* status) {
        const std::string t = "a" + std::to_string(++tag);
        if (!s->write(t + " " + c + "\r\n")) return false;
        std::string l;
        while (s->readLine(l)) {
            if (l.rfind(t + " ", 0) == 0) {
                if (status) *status = l.substr(t.size() + 1);
                return l.compare(t.size() + 1, 2, "OK") == 0;
            }
            // literal {n}
            if (!l.empty() && l.back() == '}') {
                const size_t b = l.rfind('{');
                const size_t n = size_t(std::strtoul(l.c_str() + b + 1, nullptr, 10));
                std::string lit;
                if (n > 8 * 1024 * 1024 || !s->readN(n, lit)) return false;
                l += "\n" + lit;
                std::string rest;
                if (!s->readLine(rest)) return false;
                l += rest;
            }
            if (lines) lines->push_back(l);
        }
        return false;
    }
};

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

bool imapOpen(const Account& a, Imap& m, std::string* err) {
    const bool implicit = a.imapPort == 993 || std::getenv("CF_MAIL_TEST_IMPLICIT");
    m.s = open(a.imapHost, a.imapPort, implicit, err);
    if (!m.s) return false;
    std::string greet;
    if (!m.s->readLine(greet)) return *err = "IMAP: no greeting", false;
    std::vector<std::string> caps;
    std::string st;
    if (!implicit) {
        if (!m.cmd("STARTTLS", nullptr, &st)) return *err = "IMAP: the server offers no STARTTLS (" + st + ")", false;
        if (!m.s->startTls(a.imapHost, err)) return false;
    }
    if (!m.cmd("LOGIN " + quote(a.address) + " " + quote(a.password), nullptr, &st))
        return *err = "IMAP sign-in refused (app password?): " + st, false;
    if (m.cmd("CAPABILITY", &caps, nullptr))
        for (auto& c : caps) m.move |= c.find(" MOVE") != std::string::npos;
    return true;
}

std::string makeMessage(const Account& a, const std::string& to, const std::string& attachment) {
    std::string bnd;
    {
        u8 r[12];
        oc::randomBytes(r, sizeof r);
        bnd = "cf-" + oc::hex(r, sizeof r);
    }
    const std::string who = online::playerName();
    std::string m;
    m += "From: <" + a.address + ">\r\nTo: <" + to + ">\r\n";
    m += std::string("Subject: ") + kSubjectTag + " from " + who + "\r\n";
    m += "MIME-Version: 1.0\r\nX-CF-Letter: 1\r\n";
    m += "Content-Type: multipart/mixed; boundary=\"" + bnd + "\"\r\n\r\n";
    m += "--" + bnd + "\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n";
    m += "This is a letter from " + who + ", sent by Animal Crossing: City Folk Recompiled.\r\n"
         "Your game picks it up by itself; you can leave this mail alone.\r\n\r\n";
    m += "--" + bnd + "\r\nContent-Type: application/octet-stream; name=\"letter.cfl\"\r\n"
         "Content-Transfer-Encoding: base64\r\nContent-Disposition: attachment; filename=\"letter.cfl\"\r\n\r\n";
    const std::string enc = b64(attachment);
    for (size_t i = 0; i < enc.size(); i += 76) m += enc.substr(i, 76) + "\r\n";
    m += "--" + bnd + "--\r\n";
    return m;
}

std::string attachmentFor(const mail::Sealed& sl, const u8 from[32]) {
    std::string a = "CFEM";
    a += char(1);
    a.append(reinterpret_cast<const char*>(from), 32);
    a.append(reinterpret_cast<const char*>(sl.peer), 32);
    a.append(reinterpret_cast<const char*>(sl.blob.data()), sl.blob.size());
    return a;
}

// The letter.cfl part of a fetched message (base64 after its part headers).
bool findAttachment(const std::string& msg, std::string& out) {
    const size_t n = msg.find("filename=\"letter.cfl\"");
    if (n == std::string::npos) return false;
    size_t a = msg.find("\r\n\r\n", n);
    if (a == std::string::npos) return false;
    a += 4;
    const size_t b = msg.find("\r\n--", a);
    out = unb64(msg.substr(a, b == std::string::npos ? std::string::npos : b - a));
    return out.size() > 5 + 64 && out.compare(0, 4, "CFEM") == 0 && out[4] == 1;
}

std::filesystem::path seenFile() { return port::dataDir() / "online" / "mail" / "imap_seen.txt"; }

void pass() {
    Account a;
    std::string why;
    if (!account(a, &why)) return status(why);
    int sent = 0, got = 0;
    // 1. Out.
    for (auto& sl : mail::sealedOutbox()) {
        const std::string to = friends::friendEmail(sl.peer);
        if (to.empty()) continue;
        std::string err;
        if (smtpSend(a, to, makeMessage(a, to, attachmentFor(sl, friends::myPublicKey())), &err)) {
            mail::sentByEmail(sl.id);
            sent++;
        } else {
            return status("Sending failed: " + err);
        }
    }
    if (sTestWanted.exchange(false)) {
        const auto sl = mail::sealTestForSelf();
        std::string err;
        if (!smtpSend(a, a.address, makeMessage(a, a.address, attachmentFor(sl, friends::myPublicKey())), &err))
            return status("Test letter not sent: " + err);
        status("Test letter sent to " + a.address + "; checking for it...");
    }
    // 2. In.
    Imap m;
    std::string err;
    if (!imapOpen(a, m, &err)) return status(err);
    std::string st;
    if (!m.cmd("SELECT INBOX", nullptr, &st)) return status("IMAP: " + st);
    std::vector<std::string> res;
    m.cmd(std::string("UID SEARCH SUBJECT ") + quote(kSubjectTag), &res, nullptr);
    std::set<std::string> seen;
    {
        std::ifstream in(seenFile());
        std::string l;
        while (std::getline(in, l)) seen.insert(l);
    }
    bool createdFolder = false;
    for (auto& r : res) {
        if (r.rfind("* SEARCH", 0) != 0) continue;
        size_t p = 8;
        while (p < r.size()) {
            const unsigned long uid = std::strtoul(r.c_str() + p, nullptr, 10);
            p = r.find(' ', p + 1);
            if (p == std::string::npos) p = r.size();
            if (!uid) continue;
            const std::string key = a.address + " " + std::to_string(uid);
            if (seen.count(key)) continue;
            std::vector<std::string> f;
            if (!m.cmd("UID FETCH " + std::to_string(uid) + " BODY.PEEK[]", &f, nullptr) || f.empty()) continue;
            std::string att;
            if (!findAttachment(f[0], att)) continue;  // not ours: left alone
            const u8* from = reinterpret_cast<const u8*>(att.data() + 5);
            const u8* to = from + 32;
            if (std::memcmp(to, friends::myPublicKey(), 32) != 0) continue;
            const std::vector<u8> blob(att.begin() + 5 + 64, att.end());
            const mail::Import r2 = mail::importSealed(from, blob);
            if (r2 == mail::Import::NotFromAFriend || r2 == mail::Import::Broken) continue;  // left alone
            if (r2 == mail::Import::Taken) got++;
            if (r2 == mail::Import::Self) status("Test letter to yourself arrived: sending and receiving work");
            if (m.move) {
                if (!createdFolder) m.cmd("CREATE \"City Folk\"", nullptr, nullptr), createdFolder = true;
                m.cmd("UID MOVE " + std::to_string(uid) + " \"City Folk\"", nullptr, nullptr);
            } else {
                std::error_code ec;
                std::filesystem::create_directories(seenFile().parent_path(), ec);
                std::ofstream(seenFile(), std::ios::app) << key << "\n";
            }
        }
    }
    m.cmd("LOGOUT", nullptr, nullptr);
    std::lock_guard<std::mutex> g(sMutex);
    if (sStatus.rfind("Test letter", 0) != 0 || sent || got) {
        char b[160];
        std::snprintf(b, sizeof b, "Checked %s: %d sent, %d received", a.address.c_str(), sent, got);
        sStatus = b;
        if (online::logOn()) std::printf("[email] %s\n", b);
    }
}

void worker() {
    const char* pv = std::getenv("CF_MAIL_POLL_S");
    const double every = pv ? std::atof(pv) : 300.0;
    double next = 0;
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    while (sRunning) {
        const double t = std::chrono::duration<double>(clock::now() - t0).count();
        if (t >= next || sWake.exchange(false)) {
            pass();
            next = t + every;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

#if defined(_WIN32)
std::wstring target(const std::string& address) {
    std::wstring w = L"CityFolkRecompiled/mail/";
    for (char c : address) w += wchar_t(u8(c));
    return w;
}
#endif

std::string passwordFor(const std::string& address) {
    if (const char* t = std::getenv("CF_MAIL_PASSWORD")) return t;  // test aid
#if defined(_WIN32)
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target(address).c_str(), CRED_TYPE_GENERIC, 0, &c)) return {};
    std::string p(reinterpret_cast<const char*>(c->CredentialBlob), c->CredentialBlobSize);
    CredFree(c);
    return p;
#else
    (void)address;
    return {};
#endif
}

}  // namespace

bool sFieldsStale = true;  // the F1 text fields are refilled from the settings

const std::vector<Preset>& presets() {
    static const std::vector<Preset> v = {
        {"Gmail", "imap.gmail.com", 993, "smtp.gmail.com", 465, true, "needs 2-step verification and an app password"},
        {"Yahoo", "imap.mail.yahoo.com", 993, "smtp.mail.yahoo.com", 465, true, "needs an app password"},
        {"iCloud", "imap.mail.me.com", 993, "smtp.mail.me.com", 587, true, "needs 2-factor authentication and an app-specific password"},
        {"Fastmail", "imap.fastmail.com", 993, "smtp.fastmail.com", 465, true, "needs an app password"},
        {"GMX", "imap.gmx.com", 993, "mail.gmx.com", 465, true, "enable IMAP/POP3 in GMX's settings first"},
        {"Outlook / Hotmail (personal)", "outlook.office365.com", 993, "smtp-mail.outlook.com", 587, false,
         "not supported: Microsoft no longer allows password sign-in for IMAP/SMTP (OAuth only)"},
        {"Other (enter the servers)", "", 993, "", 465, true, "ports 993/465 (TLS) or 143/587 (STARTTLS)"},
    };
    return v;
}

bool storePassword(const std::string& address, const std::string& password, std::string* err) {
#if defined(_WIN32)
    const std::wstring t = target(address);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = const_cast<wchar_t*>(t.c_str());
    c.CredentialBlobSize = DWORD(password.size());
    c.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(password.data()));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    std::wstring user;
    for (char ch : address) user += wchar_t(u8(ch));
    c.UserName = const_cast<wchar_t*>(user.c_str());
    if (CredWriteW(&c, 0)) return true;
    if (err) *err = "Windows Credential Manager refused it (error " + std::to_string(GetLastError()) + ")";
    return false;
#else
    (void)address, (void)password;
    if (err) *err = "this build has no access to the system's keychain yet: nothing was stored";
    return false;
#endif
}

bool havePassword(const std::string& address) { return !passwordFor(address).empty(); }

void forgetPassword(const std::string& address) {
#if defined(_WIN32)
    CredDeleteW(target(address).c_str(), CRED_TYPE_GENERIC, 0);
#else
    (void)address;
#endif
}

static bool presetSupported(const std::string& name) {
    for (const auto& p : presets())
        if (name == p.name) return p.supported;
    return true;  // "Other" / older settings: the servers typed in
}

void tick() {
    const auto& o = port::settings().online;
    const bool on = (o.emailOn && presetSupported(o.emailPreset)) || std::getenv("CF_MAIL_ON");
    friends::setMyEmail(on ? port::settings().online.emailAddress : std::string());
    if (on && !sRunning) {
        sRunning = true;
        std::thread(worker).detach();
    } else if (!on && sRunning) {
        sRunning = false;
        status("Off");
    }
}

void sendTestLetter() {
    sTestWanted = true;
    sWake = true;
}

bool drawSection() {
    auto& o = port::settings().online;
    bool dirty = false;
    ImGui::SeparatorText("Letters while your friend is away");
    ImGui::TextWrapped("Without email, a letter to a friend goes when both games are running at the same time "
                       "(\"Deliver whenever both games are running\"); until then it waits on your PC.");
    ImGui::SetItemTooltip("How it works: your game and your friend's find each other directly, without a server.\n"
                          "- Both games must be running at the same time (any screen, no room needed).\n"
                          "- You must have been in a room together once (that is how the games learn each other's\n"
                          "  addresses); after that a changed address is picked up automatically.\n"
                          "- Over the internet it needs automatic port opening on one router (UPnP / NAT-PMP, F1 >\n"
                          "  Online \"Map the port\") or both PCs on the same network; strict mobile / carrier\n"
                          "  networks may block it.\n"
                          "- A letter is sent when you save (as on the Wii) and kept until your friend's game confirms it.\n"
                          "For letters while your friend's game is closed, use \"Always online mail\" below.");
    dirty |= menu::Checkbox("Always online mail (use my email account)", &o.emailOn);
    ImGui::SetItemTooltip("Letters travel as encrypted attachments through your own email account, like WiiConnect24 did.\n"
                          "Your friends' games learn your address from you automatically (never typed by anyone).\n"
                          "Only letters signed by your friends are taken; your other mail is never touched.");
    if (!o.emailOn) return dirty;
    const auto& ps = presets();
    int cur = int(ps.size()) - 1;
    for (int i = 0; i < int(ps.size()); i++)
        if (o.emailPreset == ps[size_t(i)].name) cur = i;
    if (ImGui::BeginCombo("Provider", ps[size_t(cur)].name)) {
        for (int i = 0; i < int(ps.size()); i++)
            if (ImGui::Selectable(ps[size_t(i)].name, i == cur)) {
                o.emailPreset = ps[size_t(i)].name;
                if (*ps[size_t(i)].imapHost) {
                    o.emailImapHost = ps[size_t(i)].imapHost;
                    o.emailImapPort = ps[size_t(i)].imapPort;
                    o.emailSmtpHost = ps[size_t(i)].smtpHost;
                    o.emailSmtpPort = ps[size_t(i)].smtpPort;
                }
                sFieldsStale = true;  // the server fields show the preset's servers
                dirty = true;
            }
        ImGui::EndCombo();
    }
    const Preset& p = ps[size_t(cur)];
    if (!p.supported) ImGui::TextColored(ImVec4(1, 0.55f, 0.4f, 1), "%s", p.note);
    else ImGui::TextDisabled("%s", p.note);
    static char addr[128], imap[128], smtp[128], pw[128];
    if (sFieldsStale) {
        std::snprintf(addr, sizeof addr, "%s", o.emailAddress.c_str());
        std::snprintf(imap, sizeof imap, "%s", o.emailImapHost.c_str());
        std::snprintf(smtp, sizeof smtp, "%s", o.emailSmtpHost.c_str());
        sFieldsStale = false;
    }
    if (!p.supported) {
        ImGui::TextWrapped("Choose another provider: this one cannot be used, so no mail is sent or fetched.");
        return dirty;
    }
    if (ImGui::InputText("Your address", addr, sizeof addr)) o.emailAddress = addr, dirty = true;
    if (ImGui::InputText("IMAP server", imap, sizeof imap)) o.emailImapHost = imap, dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    dirty |= ImGui::InputInt("##imapport", &o.emailImapPort, 0);
    if (ImGui::InputText("SMTP server", smtp, sizeof smtp)) o.emailSmtpHost = smtp, dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    dirty |= ImGui::InputInt("##smtpport", &o.emailSmtpPort, 0);
    ImGui::InputText("App password", pw, sizeof pw, ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    static std::string pwMsg;
    if (menu::SmallButton("Save password")) {
        std::string e;
        pwMsg = storePassword(o.emailAddress, pw, &e) ? "Stored in Windows Credential Manager (not in settings.ini)" : e;
        std::memset(pw, 0, sizeof pw);
    }
    ImGui::SetItemTooltip("Kept only in the system's credential store, never in settings.ini.");
    if (!pwMsg.empty()) ImGui::TextDisabled("%s", pwMsg.c_str());
    else if (!o.emailAddress.empty()) ImGui::TextDisabled("%s", havePassword(o.emailAddress) ? "Password stored" : "No password stored yet");
    const bool ready = !o.emailAddress.empty() && !o.emailImapHost.empty() && !o.emailSmtpHost.empty() && havePassword(o.emailAddress);
    ImGui::BeginDisabled(!ready);
    if (menu::SmallButton("Send test letter to myself")) sendTestLetter();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(ready ? "Sends a sealed test letter to your own address and fetches it back: checks the servers, TLS\n"
                                  "and the password. Your game takes it like a friend's letter (it is not a game letter)."
                                : "Enter your address and servers and save the password first.");
    {
        std::lock_guard<std::mutex> g(sMutex);
        ImGui::TextWrapped("Mail: %s", sStatus.c_str());
    }
    return dirty;
}

}  // namespace recomp::online::email
