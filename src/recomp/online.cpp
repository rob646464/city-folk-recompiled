// Online play: identity, F1 > Online, notices and test aids (online.h, docs/ONLINE.md).
#include "recomp/online.h"

#include "port/dol.h"
#include "port/install.h"
#include "port/settings.h"
#include "port/update.h"
#include "recomp/game.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/ios.h"
#include "recomp/ui_scale.h"
#include "recomp/menu.h"
#include "recomp/online_crypto.h"
#include "recomp/version.h"
#include "recomp/online_ed25519.h"
#include "recomp/online_friends.h"
#include "recomp/online_mail.h"
#include "recomp/online_net.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::online {
namespace {

namespace net = ::online::net;
namespace oc = ::onlinecrypto;

std::atomic<u32> sPid{0};
std::atomic<u64> sFriendKey{0};
std::once_flag sIdentOnce;
u8 sSeed[32];

const char* envOr(const char* name, const char* def = nullptr) {
    const char* v = std::getenv(name);
    return v && *v ? v : def;
}

// Per-install identity: 32 random bytes in <data>/online/identity.bin (never sent; the profile id
// and later the friend keypair derive from it).
void loadIdentity() {
    namespace fs = std::filesystem;
    u8 id[32] = {};
    const fs::path dir = port::dataDir() / "online";
    const fs::path file = dir / "identity.bin";
    bool ok = false;
    {
        std::ifstream in(file, std::ios::binary);
        if (in.read(reinterpret_cast<char*>(id), 32)) ok = true;
    }
    if (!ok) {
        oc::randomBytes(id, 32);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(id), 32);
    }
    // Ed25519 seed for the friend key; the DWC profile id is derived from the public key, so a
    // peer that proves its key (online_friends.cpp) also proves its in-game friend code.
    oc::hmacSha256(id, 32, "cf-ed25519-seed", 15, sSeed);
    u8 pub[32];
    oc::ed25519PublicKey(sSeed, pub);
    sPid = friends::pidOf(pub);
}

void ensureIdentity() { std::call_once(sIdentOnce, loadIdentity); }

std::string fcText(u64 k) {
    if (!k) return "(after the game's first connection)";
    char b[32];
    std::snprintf(b, sizeof b, "%04u-%04u-%04u", unsigned(k / 100000000ull % 10000), unsigned(k / 10000 % 10000), unsigned(k % 10000));
    return b;
}

void pushIdentity() {
    ensureIdentity();
    net::Identity id;
    id.name = playerName();
    id.gameId = port::dol::versionId(port::dol::version());
    id.portVersion = port::update::buildVersion();
    id.pid = sPid;
    id.friendKey = sFriendKey;
    net::session().setIdentity(id);
}

net::HostOptions hostOptions(const std::string& password) {
    const auto& o = port::settings().online;
    net::HostOptions h;
    h.port = u16(o.port);
    h.roomName = o.roomName;
    h.password = password;
    h.askBeforeJoin = o.askBeforeJoin;
    h.allowDirect = o.allowDirect;
    h.lanVisible = o.lanVisible;
    h.portMapping = o.portMapping;
    h.relay = envOr("CF_ONLINE_RELAY") ? envOr("CF_ONLINE_RELAY") : o.relay;
    return h;
}

// Notices (room events), shown for a few seconds over the game.
struct Notice {
    std::string text;
    double until;
};
std::mutex sNoticeMutex;
std::vector<Notice> sNotices;
double secs() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}
void notice(const std::string& t) {
    std::printf("[online] %s\n", t.c_str());
    std::lock_guard<std::mutex> g(sNoticeMutex);
    sNotices.push_back({t, secs() + 6});
}

// UI state.
char sName[32], sRoom[48], sPassword[64], sJoinCode[160], sJoinPw[64], sDirect[96], sDirectPw[64];
bool sUiInit = false;
std::string sError;
net::State sLastState = net::State::Off;
std::vector<net::Member> sLastMembers;

void copyStr(char* dst, size_t n, const std::string& s) {
    std::snprintf(dst, n, "%s", s.c_str());
}

void watchRoom() {
    auto snap = net::session().snapshot();
    if (snap.state != sLastState) {
        if (snap.state == net::State::Failed && !snap.message.empty()) notice(snap.message);
        if (snap.state == net::State::Joined) notice(snap.message);
        sLastState = snap.state;
    }
    for (auto& m : snap.members) {
        bool had = false;
        for (auto& o : sLastMembers) had |= o.id == m.id;
        if (!had && !m.self && (snap.state == net::State::Hosting || !sLastMembers.empty())) notice(m.name + " joined the room");
    }
    for (auto& o : sLastMembers) {
        bool still = false;
        for (auto& m : snap.members) still |= o.id == m.id;
        if (!still && !o.self && !snap.members.empty()) notice(o.name + " left the room");
    }
    sLastMembers = snap.members;
}

// ---- test aids ----
struct TestStep {
    u32 frame;
    std::string action;
};
std::vector<TestStep> sSteps;
std::vector<TestStep> sRepeat;  // steps an action asked to run again
bool sEnvDone = false;
std::string sCodeFile;
bool sJoinPending = false;

void envStart() {
    sEnvDone = true;
    pushIdentity();
    if (const char* f = envOr("CF_ONLINE_PID_FILE")) {
        std::ofstream(f) << sPid.load() << "\n";
    }
    sCodeFile = envOr("CF_ONLINE_CODE_FILE", "");
    if (const char* h = envOr("CF_ONLINE_HOST")) {
        auto o = hostOptions(envOr("CF_ONLINE_PASSWORD", ""));
        o.port = u16(std::atoi(h) > 1023 ? std::atoi(h) : net::kDefaultPort);
        o.portMapping = envOr("CF_ONLINE_UPNP") != nullptr;
        std::string err;
        if (!net::session().host(o, &err)) std::printf("[online] CF_ONLINE_HOST failed: %s\n", err.c_str());
    }
    if (envOr("CF_ONLINE_JOIN")) sJoinPending = true;
    if (const char* t = envOr("CF_ONLINE_TEST")) {
        std::string s = t;
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(';', p);
            if (e == std::string::npos) e = s.size();
            const std::string item = s.substr(p, e - p);
            const size_t colon = item.find(':');
            if (colon != std::string::npos) sSteps.push_back({u32(std::strtoul(item.c_str(), nullptr, 10)), item.substr(colon + 1)});
            p = e + 1;
        }
    }
}

void tryJoinFromEnv() {
    std::string target = envOr("CF_ONLINE_JOIN", "");
    if (target == "file") {
        std::ifstream in(sCodeFile);
        std::string code;
        if (!(in >> code) || code.empty()) return;  // the host has not written it yet
        target = code;
    }
    sJoinPending = false;
    std::string err;
    const bool ok = target.find(':') != std::string::npos || target.find('.') != std::string::npos
                        ? net::session().joinAddress(target, envOr("CF_ONLINE_PASSWORD", ""), &err)
                        : net::session().joinCode(target, envOr("CF_ONLINE_PASSWORD", ""), &err);
    if (!ok) std::printf("[online] CF_ONLINE_JOIN failed: %s\n", err.c_str());
}

// The game's network system (USA addresses; test aid only): the calls Copper's gate makes.
void runAction(Ctx& c, const std::string& a) {
    if (a.rfind("visit:", 0) != 0) std::printf("[online-test] frame %u: %s\n", recomp::game::guestFrames(), a.c_str());
    // F1 friends (any version): addfriend:<member id> as the member list's button, acceptfriends
    // accepts every pending request, friends lists them.
    if (a.rfind("addfriend:", 0) == 0) {
        const u8 id = u8(std::atoi(a.c_str() + 10));
        std::printf("[online-test] member %u relation %d, code %s\n", id, int(friends::relation(id)), friends::memberCode(id).c_str());
        friends::requestFriend(id);
        return;
    }
    if (a == "mailtest") {
        std::printf("[online-test] %d test letters queued\n", mail::queueTestLetter());
        return;
    }
    if (a == "removefriends") {  // F1 "Remove" on every friend (the game's roster follows)
        for (auto& f : friends::list()) friends::remove(f.code);
        return;
    }
    if (a == "acceptfriends" || a == "friends") {
        for (auto& f : friends::list()) {
            std::printf("[online-test] friend %s %s state %d pid %u\n", f.name.c_str(), f.code.c_str(), int(f.state), f.pid);
            if (a == "acceptfriends" && f.state == friends::State::RequestReceived) friends::accept(f.code);
        }
        return;
    }
    if (a == "userdata") {
        // DWC user data of the save (save+4; friend list save+0x44): created by the game with the save file
        // (fn_8011A874 -> fn_8011A918 -> DWC_CreateUserData); a new town that was never saved
        // has none yet, and the online system refuses to start without it.
        const u32 save = gu::call(c, VA(0x80101894));
        static const u32 check = guestSymbol("DWC_CheckUserData");
        const u32 valid = save && check ? gu::call(c, check, {save + 4}) : 0;
        std::printf("[online-test] save %08X user data %s\n", save, valid ? "valid" : "missing: creating it as the game does");
        if (save && !valid) gu::call(c, VA(0x8011A918), {save});
    } else if (a == "authid") {
        // The save's user data gets the authentic login id a first WFC connection gives (as
        // online_dwc.cpp's login writes it), so Copper does not first issue a friend code.
        const u32 save = gu::call(c, VA(0x80101894));
        static const u32 clr = guestSymbol("DWC_ClearDirtyFlag");
        if (save && clr) {
            const u32 u = save + 4, pid = profileId();
            wr32(u + 0x10, ((((rd32(u + 0x10) >> 11) & ~3u) | 1u) << 11));
            wr32(u + 0x14, pid ^ 0x5A5A0000u);
            wr32(u + 0x18, pid);
            wr32(u + 0x1C, pid);
            gu::call(c, clr, {u});
            std::printf("[online-test] save user data: authentic login id, profile id %u\n", pid);
        }
    } else if (a == "setup") {
        gu::call(c, VA(0x800DCBC4));  // online system setup (Copper calls it before connecting)
    } else if (a == "diag") {
        // Why fn_800E4ECC (start) refuses: its conditions one by one.
        const u32 sys = rd32(VA(0x8074E7B8));
        const u32 scene = gu::call(c, VA(0x80106AAC));
        std::printf("[online-test] diag: scene +0x65=%u +8=%u; sys 3649=%u 3664=%u 3670=%u 385c=%u; SC item 0x14 bit=%u; "
                    "user data %08X check=%u\n",
                    rd8(scene + 0x65), rd32(scene + 8), rd8(sys + 0x3649), rd32(sys + 0x3664), rd8(sys + 0x3670),
                    rd32(sys + 0x385C), gu::call(c, VA(0x803ADAE4)), rd32(sys + 0x383C), gu::call(c, VA(0x800E593C), {rd32(sys + 0x383C)}));
    } else if (a == "start") {
        std::printf("[online-test] start -> %u\n", gu::call(c, VA(0x800E4ECC), {rd32(VA(0x8074E7B8))}));
    } else if (a == "connect") {
        std::printf("[online-test] connect -> %u\n", gu::call(c, VA(0x800DCBC8)));
    } else if (a == "host") {
        std::printf("[online-test] open gate -> %u\n", gu::call(c, VA(0x800DCC54)));
    } else if (a.rfind("visit:", 0) == 0) {
        // Copper calls this every frame until it returns 1 (connected), then the trip starts.
        static int tries = 0;
        const u32 r = gu::call(c, VA(0x800DCCEC), {u32(std::atoi(a.c_str() + 6))});
        if (r || tries % 60 == 0) std::printf("[online-test] visit -> %u (try %d)\n", r, tries);
        if (!r && ++tries < 3600) sRepeat.push_back({recomp::game::guestFrames() + 1, a});
        return;
    } else if (a == "disconnect") {
        std::printf("[online-test] disconnect -> %u\n", gu::call(c, VA(0x800DCE44)));
    } else if (a.rfind("friend:", 0) == 0) {
        // friend:<slot>:<pid or @file>: registers a friend in the save's DWC friend list, as the
        // game's roster does with a friend code (type 3 profile entry here).
        const size_t c2 = a.find(':', 7);
        const int slot = std::atoi(a.c_str() + 7);
        std::string v = c2 == std::string::npos ? "" : a.substr(c2 + 1);
        u32 pid = 0;
        if (!v.empty() && v[0] == '@') {
            std::ifstream in(v.substr(1));
            in >> pid;
        } else {
            pid = u32(std::strtoul(v.c_str(), nullptr, 10));
        }
        const u32 save = gu::call(c, VA(0x80101894));
        if (!save || !pid || slot < 0 || slot >= 32) {
            std::printf("[online-test] friend: no save data / pid\n");
            return;
        }
        const u32 e = save + 0x44 + 12u * u32(slot);  // friend list = save+0x44 (user data = save+4)
        wr32(e, 3u << 11);
        wr32(e + 4, pid);
        wr32(e + 8, 0);
        std::printf("[online-test] friend slot %d = pid %u (save %08X)\n", slot, pid, save);
    }
    // The online system's state (+0x385C: 0 none, 1 set up, 3 connecting, 5 hosting, 6 visiting).
    if (const u32 sys = rd32(VA(0x8074E7B8)); sys >= 0x80000000u && sys < 0x93F00000u) std::printf("[online-test] online system state %u\n", rd32(sys + 0x385C));
}

}  // namespace

bool logOn() {
    static const bool on = envOr("CF_ONLINE_LOG") != nullptr;
    return on;
}

bool active() { return net::session().active(); }

u32 profileId() {
    ensureIdentity();
    return sPid;
}

u64 wiiNumber() {
    ensureIdentity();
    return friends::myWiiNumber();
}

u64 friendKey() { return sFriendKey; }

const u8* identitySeed() {
    ensureIdentity();
    return sSeed;
}

void postNotice(const std::string& text) { notice(text); }

void setFriendKey(u64 key) {
    sFriendKey = key;
    pushIdentity();
}

std::string playerName() {
    if (const char* n = envOr("CF_ONLINE_NAME")) return n;
    const auto& s = port::settings().online.playerName;
    return s.empty() ? "Player" : s;
}

// This player's roster entry as another game stores it (save friend info, 0x78 bytes; layout from
// fn_8011AD7C, which clears one, and fn_800AC160, which builds the DWC status data from the same
// save fields): +0x00 u64 (0 here), +0x08 player (u16 id, 9 UTF-16 name units, u8 kind 0x0A),
// +0x1E town (u16 id, name, u8 kind 0x02), +0x34 u8, +0x36 0x42 bytes (save+0x10C8).
static std::vector<u8> myGameInfo(u32 save) {
    std::vector<u8> v(friends::kGameInfoSize, 0);
    const u64 wii = friends::myWiiNumber();  // +0x00: the Wii number letters go to
    for (int i = 0; i < 8; i++) v[size_t(i)] = u8(wii >> (56 - 8 * i));
    const u32 a = save + 0x7EC2, b = save + 0x7ED8;  // the two 0x16-byte name structs
    const bool aIsPlayer = rd8(a + 0x14) == 0x0A;
    const u32 player = aIsPlayer ? a : b, town = aIsPlayer ? b : a;
    std::memcpy(v.data() + 0x08, hostp(player), 0x16);
    std::memcpy(v.data() + 0x1E, hostp(town), 0x16);
    std::memcpy(v.data() + 0x36, hostp(save + 0x10C8), 0x42);
    return v;
}

// A friend's entry from the network: names always terminated, kinds as the game sets them.
static std::vector<u8> cleanGameInfo(std::vector<u8> v, const u8 pub[32]) {
    v[0x08 + 0x12] = v[0x08 + 0x13] = 0;
    v[0x1E + 0x12] = v[0x1E + 0x13] = 0;
    v[0x08 + 0x14] = 0x0A;
    v[0x1E + 0x14] = 0x02;
    const u64 wii = friends::wiiNumberOf(pub);  // the number that belongs to its key, whatever it sent
    for (int i = 0; i < 8; i++) v[size_t(i)] = u8(wii >> (56 - 8 * i));
    return v;
}

static std::string utf16Name(const std::vector<u8>& v, size_t off) {
    std::string s;
    for (size_t i = 0; i < 9; i++) {
        const u16 ch = u16(v[off + 2 * i] << 8 | v[off + 2 * i + 1]);
        if (!ch) break;
        s += ch < 0x80 ? char(ch) : '?';
    }
    return s;
}

// F1 friends into the game's own friend roster (docs/ONLINE.md section 10): a friend whose
// profile id is not in the save's DWC friend list yet is registered the way the roster menu does
// it after a friend code is typed (cf_RosterRegisterOffline: slot, friend data, friend info), only
// while the game is offline and a town save with valid DWC user data is loaded. The friend info
// (names, town) is the one the friend's own game sent (INFO); an entry registered before that is
// updated once it arrives. The save keeps it when the game next saves.
static void rosterSync(Ctx& c) {
    static u32 next = 0;
    const u32 f = recomp::game::guestFrames();
    if (f < next) return;
    next = f + 120;
    static const u32 fnSave = guestSymbol("cf_SaveData"), fnReg = guestSymbol("cf_RosterRegisterOffline"),
                     fnCheck = guestSymbol("DWC_CheckUserData");
    if (!fnSave || !fnReg || !fnCheck) return;
    // cf_RosterRegisterOffline itself refuses while the game is connected (online system state > 1).
    const u32 save = gu::call(c, fnSave);
    if (!save || !gu::call(c, fnCheck, {save + 4})) return;
    friends::setMyGameInfo(myGameInfo(save));
    // The game registers friends only while offline (online system state <= 1).
    if (const u32 sys = rd32(VA(0x8074E7B8)); sys >= 0x80000000u && sys < 0x93F00000u && rd32(sys + 0x385C) > 1) return;
    // Friends removed or blocked in F1 leave the game's roster the way its roster menu deletes
    // one (cf_RosterDeleteOffline: later entries move up), one per pass.
    static const u32 fnDel = guestSymbol("cf_RosterDeleteOffline");
    const auto current = friends::list();
    for (u32 pid : friends::pendingRosterRemovals()) {
        bool again = false;  // a friend again since: its entry stays
        for (auto& fr : current) again |= fr.pid == pid && fr.state == friends::State::Friend;
        int slot = -1;
        if (again) {
            friends::rosterRemovalDone(pid);
            continue;
        }
        for (int i = 0; i < 32 && slot < 0; i++) {
            const u32 e = save + 0x44 + 12u * u32(i);
            const u32 type = (rd32(e) >> 11) & 3;
            if ((type == 2 || type == 3) && rd32(e + 4) == pid) slot = i;
        }
        if (slot < 0 || !fnDel) {
            friends::rosterRemovalDone(pid);
            continue;
        }
        const u32 ok = gu::call(c, fnDel, {u32(slot)});
        std::printf("[online] profile id %u removed from the game roster (slot %d): %s\n", pid, slot, ok ? "done" : "refused");
        if (ok) friends::rosterRemovalDone(pid);
        return;
    }
    auto fl = friends::list();
    bool any = false;
    for (auto& fr : fl) any |= fr.state == friends::State::Friend;
    if (!any) return;
    static u32 buf = 0, info = 0;
    if (!buf) buf = hleAlloc(16);
    if (!info) info = hleAlloc(u32(friends::kGameInfoSize));
    for (auto& fr : fl) {
        if (fr.state != friends::State::Friend || !fr.pid) continue;
        int empty = -1, slot = -1;
        for (int i = 0; i < 32; i++) {
            const u32 e = save + 0x44 + 12u * u32(i);
            const u32 type = (rd32(e) >> 11) & 3;
            if (type == 0 && empty < 0) empty = i;
            if ((type == 2 || type == 3) && rd32(e + 4) == fr.pid && slot < 0) slot = i;
        }
        const bool haveInfo = fr.gameInfo.size() == friends::kGameInfoSize;
        std::vector<u8> gi;
        if (haveInfo) gi = cleanGameInfo(fr.gameInfo, fr.pub);
        // A friend in the room sends its entry right after proving its key: wait for it, so the
        // roster gets the names at once.
        if (slot < 0 && !haveInfo && friends::inRoom(fr.code)) continue;
        if (slot >= 0) {
            // Registered: refresh the names once the friend's own entry is known.
            const u32 cur = save + 0x1C8 + u32(friends::kGameInfoSize) * u32(slot);
            if (!haveInfo || std::memcmp(hostp(cur), gi.data(), friends::kGameInfoSize) == 0) continue;
        } else if (empty < 0) {
            static std::string told;
            if (told != fr.code) notice("The game's friend roster is full: " + fr.name + " was not added");
            told = fr.code;
            continue;
        }
        const int at = slot >= 0 ? slot : empty;
        if (slot >= 0) {
            std::memcpy(hostp(buf), hostp(save + 0x44 + 12u * u32(slot)), 12);
        } else {
            wr32(buf, 3u << 11);
            wr32(buf + 4, fr.pid);
            wr32(buf + 8, 0);
        }
        if (haveInfo) std::memcpy(hostp(info), gi.data(), gi.size());
        const u32 ok = gu::call(c, fnReg, {u32(at), buf, haveInfo ? info : 0u});
        const std::string as = haveInfo ? ", as " + utf16Name(gi, 0x0A) + " of " + utf16Name(gi, 0x20) : std::string();
        std::printf("[online] friend %s (profile id %u) -> game roster slot %d%s: %s\n", fr.name.c_str(), fr.pid, at, as.c_str(),
                    ok ? (slot >= 0 ? "updated" : "registered") : "refused");
        if (ok && slot < 0) notice(fr.name + " is now in the game's friend roster");
        return;  // one per pass
    }
}

void onGuestFrame(Ctx& c) {
    if (!sEnvDone) envStart();
    rosterSync(c);
    if (sJoinPending) tryJoinFromEnv();
    if (!sCodeFile.empty() && envOr("CF_ONLINE_HOST")) {
        static std::string written;
        const auto code = net::session().snapshot().code;
        if (!code.empty() && code != written) {
            std::ofstream(sCodeFile) << code << "\n";
            written = code;
        }
    }
    const u32 f = recomp::game::guestFrames();
    if (logOn()) {
        // The online system's and the network thread's states (USA addresses) on change.
        static u32 lastSys = ~0u, lastThr = ~0u;
        const u32 sys = rd32(VA(0x8074E7B8)), thr = rd32(VA(0x8074E7CC));
        auto ok = [](u32 a) { return a >= 0x80000000u && a < 0x93F00000u; };
        const u32 s1 = ok(sys) ? rd32(sys + 0x385C) : 0xFFFF, s2 = ok(thr) ? rd32(thr + 0x368) : 0xFFFF;
        if (s1 != lastSys || s2 != lastThr) {
            std::printf("[online] frame %u: online system state %u, network thread state %u\n", f, s1, s2);
            lastSys = s1;
            lastThr = s2;
        }
    }
    for (size_t i = 0; i < sSteps.size(); i++)
        if (sSteps[i].frame && f >= sSteps[i].frame) {
            sSteps[i].frame = 0;
            const std::string a = sSteps[i].action;
            runAction(c, a);
        }
    for (auto& r : sRepeat) sSteps.push_back(r);
    sRepeat.clear();
    // The game's own session state while connected (online system +0x3664: 3 = in a session).
    if (logOn()) {
        static u32 last = ~0u;
        const u32 sys = rd32(VA(0x8074E7B8));
        if (sys >= 0x80000000u && sys < 0x93F00000u && rd32(sys + 0x3664) != last) {
            last = rd32(sys + 0x3664);
            std::printf("[online] frame %u: online system session state (+0x3664) %u\n", f, last);
        }
    }
}

void drawOverlay() {
    watchRoom();
    friends::tick();
    {
        int n = 0;
        const bool* keys = SDL_GetKeyboardState(&n);
        static std::vector<u8> sc;
        sc.assign(size_t(n), 0);
        for (int i = 0; i < n && keys; i++) sc[size_t(i)] = keys[i] ? 1 : 0;
        ios::usbKeyboardPoll(port::settings().online.usbKeyboard, sc.data(), n);
    }
    const auto snap = net::session().snapshot();
    const auto freq = friends::incoming();
    const bool showReq = !snap.pending.empty() || !freq.empty();
    std::vector<Notice> live;
    {
        std::lock_guard<std::mutex> g(sNoticeMutex);
        const double t = secs();
        sNotices.erase(std::remove_if(sNotices.begin(), sNotices.end(), [&](const Notice& n) { return n.until < t; }), sNotices.end());
        live = sNotices;
    }
    if (!showReq && (live.empty() || !port::settings().online.notices)) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 12, vp->Pos.y + 12), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##onlineNotices", nullptr, fl)) {
        if (port::settings().online.notices)
            for (auto& n : live) ImGui::TextUnformatted(n.text.c_str());
        for (auto& r : snap.pending) {
            ImGui::PushID(int(r.ticket));
            ImGui::Text("%s (%s) wants to join your room", r.name.c_str(), r.address.c_str());
            if (ImGui::SmallButton("Let in")) net::session().answerJoinRequest(r.ticket, true);
            ImGui::SameLine();
            if (ImGui::SmallButton("Decline")) net::session().answerJoinRequest(r.ticket, false);
            ImGui::PopID();
        }
        for (auto& f : freq) {
            ImGui::PushID(f.code.c_str());
            ImGui::Text("%s (%s) wants to be your friend", f.name.c_str(), f.code.c_str());
            if (ImGui::SmallButton("Accept")) friends::accept(f.code);
            ImGui::SameLine();
            if (ImGui::SmallButton("Decline")) friends::decline(f.code);
            ImGui::SameLine();
            if (ImGui::SmallButton("Block")) friends::block(f.code);
            ImGui::PopID();
        }
    }
    ImGui::End();
}

static void memberTable(const net::Snapshot& snap, bool canKick) {
    if (!ImGui::BeginTable("members", canKick ? 5 : 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Player");
    ImGui::TableSetupColumn("Friend code (in game)");
    ImGui::TableSetupColumn("Ping");
    ImGui::TableSetupColumn("Friend");
    if (canKick) ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (auto& m : snap.members) {
        ImGui::PushID(m.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%s%s%s", m.name.c_str(), m.host ? " (host)" : "", m.self ? " (you)" : "");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(m.self ? fcText(sFriendKey).c_str() : fcText(m.friendKey).c_str());
        ImGui::TableNextColumn();
        if (m.self || m.rttMs < 0) ImGui::TextDisabled("-");
        else ImGui::Text("%d ms", m.rttMs);
        ImGui::TableNextColumn();
        if (!m.self) {
            switch (friends::relation(m.id)) {
            case friends::Rel::Unknown: ImGui::TextDisabled("checking key..."); break;
            case friends::Rel::Verified:
                if (menu::SmallButton("Add friend")) friends::requestFriend(m.id);
                ImGui::SetItemTooltip("Sends %s a friend request (their key %s came over the room).", m.name.c_str(),
                                      friends::memberCode(m.id).c_str());
                break;
            case friends::Rel::Friend: ImGui::TextUnformatted("Friend"); break;
            case friends::Rel::RequestSent: ImGui::TextDisabled("Request sent"); break;
            case friends::Rel::RequestReceived:
                if (menu::SmallButton("Accept request")) friends::requestFriend(m.id);
                break;
            case friends::Rel::Blocked: ImGui::TextDisabled("Blocked"); break;
            }
        }
        if (canKick) {
            ImGui::TableNextColumn();
            if (!m.self && menu::SmallButton("Kick")) net::session().kick(m.id);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void friendsSection() {
    if (!ImGui::CollapsingHeader("Friends")) return;
    ImGui::Text("Your friend code: %s", friends::myCode().c_str());
    ImGui::SameLine();
    if (menu::SmallButton("Copy##fc")) ImGui::SetClipboardText(friends::myCode().c_str());
    ImGui::SetItemTooltip("From this install's key (never sent itself). In a room, \"Add friend\" next to a player\n"
                          "sends a request; nobody needs to copy codes. Friends are kept in this PC's data folder.");
    const auto fl = friends::list();
    if (fl.empty()) ImGui::TextDisabled("No friends yet: join a room and use \"Add friend\" in its member list.");
    for (auto& f : fl) {
        ImGui::PushID(f.code.c_str());
        const char* st = f.state == friends::State::Friend ? "friend" : f.state == friends::State::RequestSent ? "request sent"
                       : f.state == friends::State::RequestReceived ? "wants to be your friend" : "blocked";
        ImGui::Text("%s  %s  (%s)", f.name.c_str(), f.code.c_str(), st);
        ImGui::SameLine();
        if (f.state == friends::State::RequestReceived) {
            if (menu::SmallButton("Accept")) friends::accept(f.code);
            ImGui::SameLine();
            if (menu::SmallButton("Decline")) friends::decline(f.code);
            ImGui::SameLine();
        }
        if (f.state != friends::State::Blocked && menu::SmallButton("Block")) friends::block(f.code);
        ImGui::SameLine();
        if (menu::SmallButton(f.state == friends::State::Blocked ? "Unblock###rm" : "Remove###rm")) friends::remove(f.code);
        ImGui::PopID();
    }
    ImGui::TextDisabled("Letters between friends and visits without a room: not yet (docs/ONLINE.md section 10).");
}

bool drawTab() {
    auto& o = port::settings().online;
    bool dirty = false;
    pushIdentity();
    if (!sUiInit) {
        sUiInit = true;
        copyStr(sName, sizeof sName, o.playerName);
        copyStr(sRoom, sizeof sRoom, o.roomName);
        copyStr(sDirect, sizeof sDirect, o.lastDirect);
    }
    const auto snap = net::session().snapshot();
    const bool inRoom = snap.state == net::State::Hosting || snap.state == net::State::Joined;
    const bool busy = snap.state == net::State::Connecting || snap.state == net::State::WaitingForHost;

    ImGui::TextWrapped("Play with other players of City Folk Recompiled: one player hosts a room, the others join it; "
                       "then visit towns through the game's own gate (Copper) as on the Wii. No account and no server: "
                       "the host's game is the room. Players register each other's friend code in the game, as on Nintendo WFC.");
    ImGui::Separator();
    const char* stateText = snap.state == net::State::Hosting ? "Hosting a room"
                          : snap.state == net::State::Joined ? "In a room"
                          : snap.state == net::State::Connecting ? "Connecting..."
                          : snap.state == net::State::WaitingForHost ? "Waiting for the host..."
                          : snap.state == net::State::Failed ? "Not in a room"
                                                             : "Not in a room";
    ImGui::Text("Room: %s", stateText);
    if (!snap.message.empty() && snap.state != net::State::Hosting) ImGui::TextWrapped("%s", snap.message.c_str());
    ImGui::Text("Game: %s", dwcStatusText().c_str());
    ImGui::Text("Your friend code: %s", fcText(sFriendKey).c_str());
    ImGui::SetItemTooltip("The game's 12-digit friend code. Others enter it in their game's friend roster,\n"
                          "and you enter theirs, so the gate lets you visit each other.");
    if (!sError.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", sError.c_str());

    ImGui::BeginDisabled(inRoom || busy);
    if (ImGui::InputText("Player name", sName, sizeof sName)) {
        o.playerName = sName;
        dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Shown to the others in a room (not your character's name in the game).");

    if (inRoom || busy) {
        ImGui::Separator();
        if (snap.state == net::State::Hosting) {
            ImGui::Text("Room code%s:", snap.hasPassword ? " (password protected)" : "");
            ImGui::SameLine();
            ImGui::TextUnformatted(snap.code.c_str());
            ImGui::SameLine();
            if (menu::SmallButton("Copy")) ImGui::SetClipboardText(snap.code.c_str());
            ImGui::SetItemTooltip("Give this code to the players you want in your room (chat, message, ...).\n"
                                  "It holds your address and a random key; it changes every time you host.\n"
                                  "The password (if any) is not in it: tell it separately.");
            if (!snap.lanEndpoint.empty()) ImGui::Text("On your network: %s", snap.lanEndpoint.c_str());
            if (!snap.publicEndpoint.empty()) ImGui::Text("On the internet: %s", snap.publicEndpoint.c_str());
            ImGui::TextWrapped("Internet: %s", snap.reachability.c_str());
            memberTable(snap, true);
            for (auto& r : snap.pending) {
                ImGui::PushID(int(r.ticket));
                ImGui::Text("%s (%s) asks to join", r.name.c_str(), r.address.c_str());
                ImGui::SameLine();
                if (menu::SmallButton("Let in")) net::session().answerJoinRequest(r.ticket, true);
                ImGui::SameLine();
                if (menu::SmallButton("Decline")) net::session().answerJoinRequest(r.ticket, false);
                ImGui::PopID();
            }
            if (menu::Button("Close room")) net::session().leave();
        } else {
            if (!snap.roomName.empty()) ImGui::Text("\"%s\"", snap.roomName.c_str());
            memberTable(snap, false);
            if (menu::Button(busy ? "Cancel###leave" : "Leave room###leave")) net::session().leave();
        }
        ImGui::TextDisabled("Leaving or closing the room while visiting ends the visit (the game reports a lost connection).");
        net::session().lanScan(false);
        return dirty;
    }

    if (ImGui::CollapsingHeader("Host a room", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::InputText("Room name", sRoom, sizeof sRoom)) {
            o.roomName = sRoom;
            dirty = true;
        }
        ImGui::InputText("Password (optional)##host", sPassword, sizeof sPassword, ImGuiInputTextFlags_Password);
        ImGui::SetItemTooltip("Joiners must enter it. It is never sent or saved: joiners prove they know it\n"
                              "(challenge-response, PBKDF2) and it keys the room's encryption.");
        if (ImGui::InputInt("UDP port", &o.port)) {
            o.port = std::clamp(o.port, 1024, 65535);
            dirty = true;
        }
        dirty |= menu::Checkbox("Ask me before someone joins", &o.askBeforeJoin);
        dirty |= menu::Checkbox("Map the port on my router (UPnP / NAT-PMP)", &o.portMapping);
        ImGui::SetItemTooltip("Asks your router to forward the UDP port to this PC while you host, and finds\n"
                              "your internet address (STUN, a public service). Without it only players on your\n"
                              "network (or a VPN such as Tailscale, ZeroTier, Hamachi) can join.");
        {
            char rb[128];
            std::snprintf(rb, sizeof rb, "%s", o.relay.c_str());
            ImGui::SetNextItemWidth(220.0f * ui::scale());
            if (ImGui::InputTextWithHint("Relay (optional)", "host:29912", rb, sizeof rb)) {
                o.relay = rb;
                dirty = true;
            }
            ImGui::SetItemTooltip("A relay that your group runs (tools: cf_relay). Joiners whose packets cannot reach\n"
                                  "you directly come in through it, and friends' letters wait there while you are\n"
                                  "offline. Room traffic stays end-to-end encrypted; the relay only forwards it.");
        }
        dirty |= menu::Checkbox("Show on the local network", &o.lanVisible);
        dirty |= menu::Checkbox("Allow direct connections (IP:port, no code)", &o.allowDirect);
        ImGui::SetItemTooltip("For port forwarding / VPN setups: joiners that know only your address may join.\n"
                              "Set a password if you leave this on with a public address.");
        if (menu::Button("Host")) {
            pushIdentity();
            std::string err;
            sError = net::session().host(hostOptions(sPassword), &err) ? "" : err;
        }
    }
    if (ImGui::CollapsingHeader("Join with a code", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("Room code", sJoinCode, sizeof sJoinCode);
        ImGui::InputText("Password##join", sJoinPw, sizeof sJoinPw, ImGuiInputTextFlags_Password);
        if (menu::Button("Join")) {
            pushIdentity();
            std::string err;
            sError = net::session().joinCode(sJoinCode, sJoinPw, &err) ? "" : err;
        }
    }
    if (ImGui::CollapsingHeader("Rooms on this network")) {
        net::session().lanScan(true);
        const auto rooms = net::session().lanRooms();
        if (rooms.empty()) ImGui::TextDisabled("No rooms heard yet (hosts announce every 2 seconds).");
        int i = 0;
        for (auto& r : rooms) {
            ImGui::PushID(i++);
            ImGui::Text("%s%s - %s, %d/%d, %s", r.hasPassword ? "[locked] " : "", r.roomName.c_str(), r.hostName.c_str(), r.members,
                        r.maxMembers, r.address.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(!r.compatible);
            if (menu::SmallButton("Join")) {
                pushIdentity();
                std::string err;
                sError = net::session().joinLan(r, r.hasPassword ? sJoinPw : "", &err) ? "" : err;
            }
            ImGui::EndDisabled();
            if (!r.compatible) ImGui::SetItemTooltip("Another region (%s): towns visit only within one region.", r.gameId.c_str());
            ImGui::PopID();
        }
        ImGui::TextDisabled("A locked room uses the password typed under \"Join with a code\".");
    } else {
        net::session().lanScan(false);
    }
    if (ImGui::CollapsingHeader("Direct connect")) {
        if (ImGui::InputText("Address (IP:port)", sDirect, sizeof sDirect)) {
            o.lastDirect = sDirect;
            dirty = true;
        }
        ImGui::InputText("Password##direct", sDirectPw, sizeof sDirectPw, ImGuiInputTextFlags_Password);
        if (menu::Button("Connect")) {
            pushIdentity();
            std::string err;
            sError = net::session().joinAddress(sDirect, sDirectPw, &err) ? "" : err;
        }
        ImGui::TextWrapped("For hosts whose router has no UPnP: forward the UDP port (default 29910) on the host's router, or "
                           "use a VPN (Tailscale, ZeroTier, Hamachi) and enter the host's VPN address.");
    }
    friendsSection();
    dirty |= menu::Checkbox("Room notices over the game", &o.notices);
    dirty |= menu::Checkbox("USB keyboard (PC keyboard for chat)", &o.usbKeyboard);
    ImGui::SetItemTooltip("The game sees your PC keyboard as a Wii USB keyboard: type chat messages while online. "
                          "Keys you also use for the Wii Remote act on both, so play with a controller while typing. "
                          "Turning it on takes effect when the game next looks for a keyboard (at the latest after a restart).");
    return dirty;
}

}  // namespace recomp::online
