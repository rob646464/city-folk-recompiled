// Online play: identity, F1 > Online, notices and test aids (online.h, docs/ONLINE.md).
#include "recomp/online.h"

#include "port/dol.h"
#include "port/install.h"
#include "port/settings.h"
#include "port/update.h"
#include "recomp/game.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/online_crypto.h"
#include "recomp/online_net.h"

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
    u8 h[32];
    oc::hmacSha256(id, 32, "cf-dwc-profile-id", 17, h);
    u32 pid = (u32(h[0]) << 24 | u32(h[1]) << 16 | u32(h[2]) << 8 | h[3]) & 0x7FFFFFFFu;
    if (pid < 0x10000) pid |= 0x10000;  // never 0 / tiny (DWC: > 0)
    sPid = pid;
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
    std::printf("[online-test] frame %u: %s\n", recomp::game::guestFrames(), a.c_str());
    if (port::dol::version() != port::dol::Version::RUUE01) {
        std::printf("[online-test] actions use USA addresses: skipped\n");
        return;
    }
    if (a == "connect") {
        std::printf("[online-test] connect -> %u\n", gu::call(c, 0x800DCBC8));
    } else if (a == "host") {
        std::printf("[online-test] open gate -> %u\n", gu::call(c, 0x800DCC54));
    } else if (a.rfind("visit:", 0) == 0) {
        std::printf("[online-test] visit -> %u\n", gu::call(c, 0x800DCCEC, {u32(std::atoi(a.c_str() + 6))}));
    } else if (a == "disconnect") {
        std::printf("[online-test] disconnect -> %u\n", gu::call(c, 0x800DCE44));
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
        const u32 save = gu::call(c, 0x80101894);
        if (!save || !pid || slot < 0 || slot >= 32) {
            std::printf("[online-test] friend: no save data / pid\n");
            return;
        }
        const u32 e = save + 4 + 12u * u32(slot);
        wr32(e, 3u << 11);
        wr32(e + 4, pid);
        wr32(e + 8, 0);
        std::printf("[online-test] friend slot %d = pid %u (save %08X)\n", slot, pid, save);
    }
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

u64 friendKey() { return sFriendKey; }

void setFriendKey(u64 key) {
    sFriendKey = key;
    pushIdentity();
}

std::string playerName() {
    if (const char* n = envOr("CF_ONLINE_NAME")) return n;
    const auto& s = port::settings().online.playerName;
    return s.empty() ? "Player" : s;
}

void onGuestFrame(Ctx& c) {
    if (!sEnvDone) envStart();
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
    for (auto& s : sSteps)
        if (s.frame && f >= s.frame) {
            s.frame = 0;
            runAction(c, s.action);
        }
}

void drawOverlay() {
    watchRoom();
    const auto snap = net::session().snapshot();
    const bool showReq = !snap.pending.empty();
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
    }
    ImGui::End();
}

static void memberTable(const net::Snapshot& snap, bool canKick) {
    if (!ImGui::BeginTable("members", canKick ? 4 : 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Player");
    ImGui::TableSetupColumn("Friend code (in game)");
    ImGui::TableSetupColumn("Ping");
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
        if (canKick) {
            ImGui::TableNextColumn();
            if (!m.self && menu::SmallButton("Kick")) net::session().kick(m.id);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
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
    dirty |= menu::Checkbox("Room notices over the game", &o.notices);
    return dirty;
}

}  // namespace recomp::online
