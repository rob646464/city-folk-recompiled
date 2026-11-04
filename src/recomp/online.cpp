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
#include "recomp/online_email.h"
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
    if (a == "mailself") {
        email::sendTestLetter();
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

static std::vector<u8> cleanGameInfo(std::vector<u8> v, const u8 pub[32]);

// nwc24fl.bin (layout from the NWC24 library's reads: GetCachedFLHeader / iReadFriendInfoRaw /
// fn_8040E23C): 0x00 "WcFl", 0x04 version 2, 0x08 entries (100), 0x40 Wii numbers u64[100],
// 0x360 entries 0x140 each: +0 type (1 Wii), +4 status (2 confirmed: City Folk's fn_800E98C0
// takes only those), +8 name (12 UTF-16 units).
void writeWiiFriends() {
    ensureIdentity();
    std::vector<u8> b(32864, 0);
    auto be32 = [&](size_t o, u32 v) {
        b[o] = u8(v >> 24);
        b[o + 1] = u8(v >> 16);
        b[o + 2] = u8(v >> 8);
        b[o + 3] = u8(v);
    };
    std::memcpy(b.data(), "WcFl", 4);
    be32(4, 2);
    be32(8, 100);
    int n = 0;
    for (const auto& f : friends::list()) {
        if (f.state != friends::State::Friend || n >= 100) continue;
        const u64 id = friends::wiiNumberOf(f.pub);
        be32(0x40 + 8 * size_t(n), u32(id >> 32));
        be32(0x44 + 8 * size_t(n), u32(id));
        const size_t e = 0x360 + 0x140 * size_t(n);
        be32(e, 1);
        be32(e + 4, 2);
        // The game's own name for that player (its roster entry's player name) when known, else the F1 name.
        if (f.gameInfo.size() == friends::kGameInfoSize)
            std::memcpy(b.data() + e + 8, cleanGameInfo(f.gameInfo, f.pub).data() + 0x20, 0x12);
        else
            for (size_t i = 0; i < f.name.size() && i < 11; i++) b[e + 8 + 2 * i + 1] = u8(f.name[i]);
        n++;
    }
    const auto p = ios::hostPath("/shared2/wc24/nwc24fl.bin");
    {
        std::ifstream in(p, std::ios::binary);
        const std::vector<u8> cur((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (cur == b) return;
    }
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    std::printf("[online] Wii friend list: %d F1 friends as confirmed Wii friends\n", n);
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

// This player's roster entry as another game stores it (save friend info, 0x78 bytes), in the
// game's own layout: what the host's game writes for a visitor during a visit (seen in the save
// after a real visit) is +0x00 the Wii number letters go to (u64), +0x08 a copy of the town name
// struct (save+0x7EC2: u16 id, 9 UTF-16 units, u8 at +0x14 = 0x01, pad), +0x1E a copy of the player
// name struct (save+0x7ED8, +0x14 = 0x00), +0x34 u8, +0x36 0x42 bytes (save+0x10C8). The letter
// writer's "Dear <name>," reads the player at +0x1E (+0x20 the name).
static std::vector<u8> myGameInfo(u32 save) {
    std::vector<u8> v(friends::kGameInfoSize, 0);
    const u64 wii = friends::myWiiNumber();
    for (int i = 0; i < 8; i++) v[size_t(i)] = u8(wii >> (56 - 8 * i));
    std::memcpy(v.data() + 0x08, hostp(save + 0x7EC2), 0x16);  // town
    std::memcpy(v.data() + 0x1E, hostp(save + 0x7ED8), 0x16);  // player
    std::memcpy(v.data() + 0x36, hostp(save + 0x10C8), 0x42);
    return v;
}

// A friend's entry from the network: names always terminated, the Wii number its key gives. Entries
// from builds before 2026-11-03 put the player first (+0x08, struct byte 0x00) and the town second
// (struct byte 0x01): those are turned into the game's order.
static std::vector<u8> cleanGameInfo(std::vector<u8> v, const u8 pub[32]) {
    if (v[0x08 + 0x14] == 0x00 && v[0x1E + 0x14] == 0x01)
        for (size_t i = 0; i < 0x16; i++) std::swap(v[0x08 + i], v[0x1E + i]);
    v[0x08 + 0x12] = v[0x08 + 0x13] = 0;
    v[0x1E + 0x12] = v[0x1E + 0x13] = 0;
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
    {
        std::string sig;
        for (auto& fr : friends::list())
            if (fr.state == friends::State::Friend) sig += fr.code + fr.name;
        static std::string last;
        if (sig != last) {
            last = sig;
            writeWiiFriends();
        }
    }
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
    // Pelly offers "Other town." only when town flag 0x1F is set (d_a_npc_sp_perikoNP fn_34_3794
    // builds "<town>. / [Other town.] / Future self. / Never mind." and asks fn_801165F4(town, 0x1F)).
    // The game's only setter is in the player-creation scene module (d_a_npc_sp_player_createNP
    // fn_25_BB4, scene 0x3D, not the new-game scene 0x3C); none of the towns we have has it, and a
    // visit does not set it. A town with an F1 friend in its roster can send letters to other towns
    // in this port, so the flag is set then (once; it stays in the save). docs/ONLINE.md section 10.
    if (const u32 town = gu::call(c, VA(0x8010DD60)); town >= 0x80000000u && !gu::call(c, VA(0x801165F4), {town, 0x1Fu})) {
        bool inRoster = false;
        for (auto& fr : fl)
            for (int i = 0; i < 32 && !inRoster; i++) {
                const u32 e = save + 0x44 + 12u * u32(i);
                const u32 type = (rd32(e) >> 11) & 3;
                inRoster |= fr.state == friends::State::Friend && (type == 2 || type == 3) && rd32(e + 4) == fr.pid;
            }
        if (inRoster) {
            gu::call(c, VA(0x80116634), {town, 0x1Fu});
            std::printf("[online] town flag 0x1F set (Pelly: \"Other town.\"): a friend is in the roster\n");
        }
    }
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
            wr32(buf, (3u | 4u) << 11);  // GS profile id entry, buddy (an F1 friendship is mutual)
            wr32(buf + 4, fr.pid);
            wr32(buf + 8, 0);
        }
        if (haveInfo) std::memcpy(hostp(info), gi.data(), gi.size());
        const u32 ok = gu::call(c, fnReg, {u32(at), buf, haveInfo ? info : 0u});
        const std::string as = haveInfo ? ", as " + utf16Name(gi, 0x20) + " of " + utf16Name(gi, 0x0A) : std::string();
        std::printf("[online] friend %s (profile id %u) -> game roster slot %d%s: %s\n", fr.name.c_str(), fr.pid, at, as.c_str(),
                    ok ? (slot >= 0 ? "updated" : "registered") : "refused");
        if (ok && slot < 0) notice(fr.name + " is now in the game's friend roster");
        return;  // one per pass
    }
}

// CF_GIVE_ITEM=<frame>:<item code hex>[x<n>],... (test aid, letters test): puts items into the first
// empty pocket slots of the player being played. Pockets: player data + 0x7F22, 15 x u16, empty =
// 0xFFF1 (fn_8013807C writes them so); item code = 0x9000 + item number * 4 (+ count bits, e.g.
// stationery), the item number being the record's second field in Item/item.arc item.bin
// (fn_800C29D0: number = (code - 0x9000) >> 2). Examples: butterfly paper 0x9640 (+3 = 4 sheets),
// 1,000 Bells 0x91B8.
static void giveItems(Ctx& c) {
    static const char* env = std::getenv("CF_GIVE_ITEM");
    if (!env) return;
    const u32 f = recomp::game::guestFrames();
    for (const char* p = env; *p;) {
        char* e = nullptr;
        const u32 at = u32(std::strtoul(p, &e, 10));
        if (*e != ':') break;
        const u32 code = u32(std::strtoul(e + 1, &e, 16));
        u32 n = 1;
        if (*e == 'x') n = u32(std::strtoul(e + 1, &e, 10));
        if (at == f) {
            const u32 pd = gu::call(c, VA(0x801017B8), {0});
            if (pd >= 0x90000000u && pd < 0x94000000u) {
                u32 given = 0;
                for (u32 i = 0; i < 15 && given < n; i++)
                    if (rd16(pd + 0x7F22 + 2 * i) == 0xFFF1) {
                        wr16(pd + 0x7F22 + 2 * i, u16(code));
                        given++;
                    }
                std::printf("[online-test] frame %u: %u x item %04X into the pockets\n", f, given, code);
            }
        }
        p = *e == ',' ? e + 1 : e;
        if (*e != ',') break;
    }
}

// CF_TOWN_FLAG=<frame>:<n>,... (test aid): sets progress flag n of the save with the game's own
// setter fn_80116634(save, n) (save + 0x735C3 bits; getter fn_801165F4). Research.
static void townFlags(Ctx& c) {
    static const char* env = std::getenv("CF_TOWN_FLAG");
    if (!env) return;
    const u32 f = recomp::game::guestFrames();
    for (const char* p = env; *p;) {
        char* e = nullptr;
        const u32 at = u32(std::strtoul(p, &e, 10));
        if (*e != ':') break;
        const u32 n = u32(std::strtoul(e + 1, &e, 16));
        if (at == f) {
            // The town flags live in the town data (fn_8010DD60, r13-0x2FE0) +0x735C3, not in the save
            // file object fn_80101894 returns.
            const u32 town = gu::call(c, VA(0x8010DD60));
            if (town) gu::call(c, VA(0x80116634), {town, n});
            std::printf("[online-test] frame %u: town flag %X set\n", f, n);
        }
        if (*e != ',') break;
        p = e + 1;
    }
}

// The town's own WiiConnect24 setting (save +0x735E4; the attic telephone's option, Rover's
// question in a new town) is turned on once per town, the first time the port sees that town
// with the console's WiiConnect24 available (the game's check state 1 at 0x8074EBCC), with the
// game's own setter fn_80177DE0. Towns done are remembered in <data>/town_wc24.txt (town id + name),
// so a player who turns it off on the attic telephone keeps it off. Letters to other towns need it
// (Pelly: "You can't send letters to people from out of town because your WiiConnect24 settings
// are turned off"); docs/ONLINE.md section 10. CF_TOWN_WC24_DEFAULT=0 leaves it alone (A/B).
static void townWc24Default(Ctx& c) {
    static const bool off = [] {
        const char* v = std::getenv("CF_TOWN_WC24_DEFAULT");
        return v && *v == '0';
    }();
    if (off) return;
    static u32 next = 0;
    const u32 f = recomp::game::guestFrames();
    if (f < next) return;
    next = f + 60;
    const u32 town = rd32(VA(0x8074E960));  // the town data (fn_8010DD60), null before a town is loaded
    if (town < 0x80000000u || town >= 0x94000000u) return;
    if (rd32(VA(0x8074EBCC)) != 1) return;  // the game's WiiConnect24 check: not (yet) available
    // Only once the town is in play: at the title, on the train and in player creation (scenes
    // 0x44, 0x38, 0x3B, 0x3C) the town data is reloaded from the save afterwards, which undoes it.
    if (const u32 scene = gu::call(c, VA(0x8016266C)) & 0xFF; scene == 0x44 || scene == 0x38 || scene == 0x3B || scene == 0x3C) return;
    char key[64];
    {
        std::string k;
        const u32 t = town + 0x7EC2;  // town name struct: u16 id, 9 UTF-16 units
        char b[8];
        for (u32 i = 0; i < 0x14; i++) {
            std::snprintf(b, sizeof b, "%02x", rd8(t + i));
            k += b;
        }
        std::snprintf(key, sizeof key, "%s", k.c_str());
    }
    static std::string doneFor;
    if (doneFor == key) return;
    const auto path = port::dataDir() / "town_wc24.txt";
    std::vector<std::string> done;
    {
        std::ifstream in(path);
        for (std::string l; std::getline(in, l);)
            if (!l.empty() && l[0] != '#') done.push_back(l.substr(0, l.find(' ')));
    }
    if (std::find(done.begin(), done.end(), key) == done.end()) {
        const bool was = rd8(town + 0x735E4) != 0;
        u32 r = 0;
        if (!was) r = gu::call(c, VA(0x80177DE0));
        std::printf("[online] town WiiConnect24 setting: %s (fn_80177DE0 -> %u, now %u)\n",
                    was ? "already on" : "turned on for this town (first start)", r, rd8(town + 0x735E4));
        if (!was && !rd8(town + 0x735E4)) return;  // the setter refused: try again later
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const bool fresh = !std::filesystem::exists(path, ec);
        std::ofstream out(path, std::ios::app);
        if (fresh) out << "# Towns whose WiiConnect24 setting City Folk Recompiled turned on once (town name struct hex).\n";
        out << key << "\n";
    }
    doneFor = key;
}

// CF_WC24_SEND=<frame> (test aid): the WiiConnect24 step of the game's save flow (save menu state
// fn_801BC9E4) run from that frame: fn_801031B8 resets it, then fn_801031F4 every frame until it
// says done. It goes through Pelly's outbox (letters to other towns, fn_80103064) and starts one
// send task per letter (fn_8017816C -> fn_8017ADD0 -> ... NWC24CommitMsg). Saving in the game (bed,
// save icon) runs the same step; the aid only spares a route to the save menu.
static void wc24SendAid(Ctx& c) {
    static const u32 at = [] {
        const char* v = std::getenv("CF_WC24_SEND");
        return v ? u32(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    static int state = 0;  // 0 waiting, 1 running, 2 done
    if (!at || state == 2) return;
    const u32 f = recomp::game::guestFrames();
    if (state == 0) {
        if (f < at) return;
        gu::call(c, VA(0x801031B8));
        state = 1;
        std::printf("[online-test] frame %u: save flow's WiiConnect24 send step started\n", f);
    }
    if (gu::call(c, VA(0x801031F4))) {
        state = 2;
        std::printf("[online-test] frame %u: save flow's WiiConnect24 send step done\n", f);
    }
}

void onGuestFrame(Ctx& c) {
    if (!sEnvDone) envStart();
    townWc24Default(c);
    wc24SendAid(c);
    giveItems(c);
    townFlags(c);
    // CF_TOWN_WC24=<frame> (test aid): the town's own WiiConnect24 setting (save +0x735E4) turned on
    // with the game's setter fn_80177DE0, what the attic telephone's WiiConnect24 option does (it
    // takes effect only while the console reports WiiConnect24 available).
    if (static const char* tw = std::getenv("CF_TOWN_WC24"); tw && recomp::game::guestFrames() == u32(std::atoi(tw)))
        std::printf("[online-test] town WiiConnect24 setting (fn_80177DE0) -> %u\n", gu::call(c, VA(0x80177DE0)));
    // Research aid (CF_MAIL_PROBE): scene id changes (fn_8016266C) and town flag 0x1F, the flag
    // Pelly's destination list (d_a_npc_sp_perikoNP fn_34_3794) needs for "Other town.".
    if (static const bool probe = envOr("CF_MAIL_PROBE") != nullptr; probe) {
        static u32 lastScene = ~0u, lastFlag = ~0u;
        const u32 scene = gu::call(c, VA(0x8016266C)) & 0xFF;
        const u32 town = gu::call(c, VA(0x8010DD60));
        const u32 flag = town >= 0x80000000u ? (rd8(town + 0x735C3 + 3) >> 7) & 1 : 2;
        if (scene != lastScene)  // scene names fn_801F6B6C compares (0x3C at +0x770, 0x3D at +0x78C)
            for (u32 off : {0x770u, 0x78Cu, 0x7A0u})
                std::printf("[mail-probe] scene name +%X: %.24s\n", off, reinterpret_cast<const char*>(hostp(0x80505D28u + off)  /* USA */));
        if (scene != lastScene || flag != lastFlag)
            std::printf("[mail-probe] frame %u: scene %02X, town flag 1F %u\n", recomp::game::guestFrames(), scene, flag);
        lastScene = scene;
        lastFlag = flag;
    }
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
    email::tick();
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
    if (!snap.replyCode.empty()) {
        ImGui::TextWrapped("No answer from the host yet. Send the host this reply code (they enter it under "
                           "\"Let them in\"):");
        ImGui::TextUnformatted(snap.replyCode.c_str());
        ImGui::SameLine();
        if (menu::SmallButton("Copy##reply")) ImGui::SetClipboardText(snap.replyCode.c_str());
    }
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
            // Hole punching: a joiner whose packets do not get through sends its reply code.
            static char reply[64] = {};
            ImGui::SetNextItemWidth(260.0f * ui::scale());
            ImGui::InputTextWithHint("##reply", "a joiner's reply code", reply, sizeof reply);
            ImGui::SameLine();
            if (menu::SmallButton("Let them in")) {
                std::string e;
                if (net::session().punch(reply, &e)) reply[0] = 0;
                else sError = e;
            }
            ImGui::SetItemTooltip("For a player who cannot reach you (no UPnP, no port forward): they get a reply code\n"
                                  "while joining. Enter it here and both routers open the way (works with most home\n"
                                  "routers; not through every mobile or corporate network).");
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
    dirty |= email::drawSection();
    dirty |= menu::Checkbox("Room notices over the game", &o.notices);
    dirty |= menu::Checkbox("USB keyboard (PC keyboard for chat)", &o.usbKeyboard);
    ImGui::SetItemTooltip("The game sees your PC keyboard as a Wii USB keyboard: type chat messages while online. "
                          "Keys you also use for the Wii Remote act on both, so play with a controller while typing. "
                          "Turning it on takes effect when the game next looks for a keyboard (at the latest after a restart).");
    return dirty;
}

}  // namespace recomp::online
