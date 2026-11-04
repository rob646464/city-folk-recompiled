// Move in from Wild World (docs/MOVE_IN.md).
//
// On a Wii: Rover's "Move from the DS!" (d_a_npc_sp_player_selectNP) starts the DS link manager
// (USA fn_800841F8, object 0x80584E14) in receive mode: DS Download Play of
// /DSApp/Vaga_Sign.srl ("Moving Van", ggid 0x400806), then an MP link on which that program sends
// the Wild World cartridge's backup memory:
//
//   Wii -> DS  "REQS"                           (4-char codes are little-endian words)
//   DS -> Wii  "ACKS", length (u32 LE, <= 0x40000), the data in packets, CRC-32 (u32 LE)
//   Wii -> DS  "ACKS" if the game's CRC-32 (fn_802A9A20) matches, else "NACK" (DS sends again)
//
// A 16-byte message instead of the save reports a DS-side problem: "B2mRWeDsApNoRom" (no Game
// Card: Rover's 0xA7), "B2mRWeDAInvalid" (not the right Game Card: 0xA8), "B2mRWeDsAFailed"
// (could not read it: 0xA9) (fn_800840B8). The game then checks the save itself (two copies,
// 16-bit sums, town marker; fn_80085D3C), lists the residents (fn_800858F0, Rover's 0x9C/0x9D),
// refuses ones already living in town (0x2D/0x2E), and imports name, gender, face, hair style and
// colour and the catalog through its own functions (REL 0x25D8: fn_80137124, fn_800C51D8 ...).
//
// Here: hle_mp.cpp answers the MP/MPDL libraries, and this file is the DS: a virtual child that
// runs the Moving Van's side of that protocol with the save file the player picks in the
// "Move in from Wild World" window. Nothing in the game's save is written by the port.
#include "game/cf_move_in.h"

#include "game/ww_save.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/mp.h"
#include "recomp/runtime.h"
#include "recomp/ui_scale.h"
#include "recomp/version.h"

#include "port/settings.h"

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::movein {
namespace {

using clk = std::chrono::steady_clock;

constexpr u32 kMovingVanGgid = 0x400806;  // DS Download Play of Vaga_Sign.srl (USA fn_800841F8)
constexpr u32 kPacket = 0x1FC;            // data per MP packet (the size the game itself sends in)
constexpr u32 kREQS = 0x52455153, kACKS = 0x41434B53, kNACK = 0x4E41434B;
// DS Download Play of the Moving Van (320 KB; several seconds on a DS). Not shorter: Rover's
// "press B to quit" line has to be up before the link comes and goes, or he reports that he could
// not find a DS (seen with 3 s and the test aid, which turns the DS on at once).
constexpr double kDownloadSeconds = 8.0;

double sinceStart() {
    static const clk::time_point t0 = clk::now();
    return std::chrono::duration<double>(clk::now() - t0).count();
}

bool logOn() {
    static const bool on = [] {
        const char* e = std::getenv("CF_MOVE_IN_LOG");
        return e && *e && *e != '0';
    }();
    return on;
}
#define MVLOG(...)                                                \
    do {                                                          \
        std::printf("[move-in %8.3f] ", sinceStart());            \
        std::printf(__VA_ARGS__);                                 \
        std::printf("\n");                                        \
        std::fflush(stdout);                                      \
    } while (0)

ww::Region gameRegion() {
    switch (ver::id()) {
    case ver::Id::JPN: return ww::Region::JP;
    case ver::Id::PAL: return ww::Region::EU;
    case ver::Id::KOR: return ww::Region::KR;
    default: return ww::Region::US;
    }
}

// A Wii accepts only its own region's Wild World. Community sources report that the European
// City Folk (revision 1, the one this port supports) also takes North American Game Cards
// (Australia uses those): docs/REGION_CONTENT.md.
bool regionAccepted(ww::Region cart, ww::Region game) {
    return cart == game || (game == ww::Region::EU && cart == ww::Region::US);
}

void put32le(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; i++) v.push_back(u8(x >> (8 * i)));
}

// The game's character tables (USA 0x8046D870: Japanese, then North America / Europe at +0x200).
bool charTables(ww::CharTable& jp, ww::CharTable& latin) {
    static bool have = false;
    static ww::CharTable sJp{}, sLatin{};
    if (!have) {
        const u32 a = VA(0x8046D870);
        if (!a) return false;
        for (int i = 0; i < 256; i++) {
            sJp[i] = char16_t(rd16(a + 2 * i));
            sLatin[i] = char16_t(rd16(a + 0x200 + 2 * i));
        }
        have = sLatin[1] == u'A';
        if (!have) return false;
    }
    jp = sJp;
    latin = sLatin;
    return true;
}

std::string nameOf(const std::vector<u8>& raw, ww::Layout l) {
    ww::CharTable jp, latin;
    if (!charTables(jp, latin)) return "?";
    const auto n = ww::decodeName(raw.data(), int(raw.size() / (l == ww::Layout::KR ? 2 : 1)), l, jp, latin);
    std::string s = ww::utf8(n);
    return s.empty() ? std::string("(no name)") : s;
}

// ---- The DS (Moving Van) and the window state -------------------------------------------------

enum class Phase {
    Off,        // Rover is not looking for a DS
    Waiting,    // Rover waits for the Moving Van download; the window asks for a save
    Download,   // the DS is downloading the Moving Van (starts after kDownloadSeconds)
    Linked,     // the program runs; talking to the Wii
    Done,       // the Wii acknowledged the data
};

struct Van : mp::Child {
    std::mutex m;
    Phase phase = Phase::Off;
    clk::time_point readyAt;
    std::vector<u8> payload;     // the backup image (or a 16-byte DS error)
    struct Packet {
        std::vector<u8> bytes;
        bool data;               // part of the payload (else ACKS / length / CRC)
    };
    std::deque<Packet> out;
    size_t queued = 0, sent = 0;  // payload bytes queued / delivered in the current send
    int sends = 0;
    std::string status;          // last protocol event, for the window

    void downloadStarted(u32 ggid) override;
    void downloadEnded() override {
        std::lock_guard lk(m);
        // (after a finished download the DS runs the program: MPDL ends, MP follows)
        if (phase == Phase::Waiting || (phase == Phase::Download && clk::now() < readyAt)) {
            if (logOn()) MVLOG("Rover stopped looking for a DS");
            phase = Phase::Off;
        }
    }
    bool hasProgram(u32 ggid) override {
        std::lock_guard lk(m);
        return ggid == kMovingVanGgid && phase == Phase::Download && clk::now() >= readyAt;
    }
    bool connects(u32 ggid) override {
        std::lock_guard lk(m);
        if (ggid != kMovingVanGgid || phase != Phase::Download || clk::now() < readyAt) return false;
        phase = Phase::Linked;
        status = "The Moving Van is running on the DS.";
        MVLOG("the DS runs the Moving Van and connects to the Wii");
        return true;
    }
    void queueSave() {
        out.clear();
        std::vector<u8> w;
        put32le(w, kACKS);
        out.push_back({w, false});
        w.clear();
        put32le(w, u32(payload.size()));
        out.push_back({w, false});
        for (size_t o = 0; o < payload.size(); o += kPacket)
            out.push_back({std::vector<u8>(payload.begin() + o, payload.begin() + std::min(payload.size(), o + kPacket)), true});
        w.clear();
        put32le(w, ww::crc32(payload.data(), payload.size()));
        out.push_back({w, false});
        queued = payload.size();
        sent = 0;
        sends++;
    }
    void received(const u8* d, u32 len) override {
        std::lock_guard lk(m);
        if (phase != Phase::Linked || len != 4) return;
        const u32 code = u32(d[0]) | u32(d[1]) << 8 | u32(d[2]) << 16 | u32(d[3]) << 24;
        if (code == kREQS) {
            MVLOG("Wii: REQS -> sending %zu bytes (CRC-32 %08X)", payload.size(), ww::crc32(payload.data(), payload.size()));
            queueSave();
            status = "Sending the save...";
        } else if (code == kNACK) {
            MVLOG("Wii: NACK (checksum mismatch) -> sending again");
            queueSave();
        } else if (code == kACKS) {
            MVLOG("Wii: ACKS: the game received the data and its CRC-32 matched");
            phase = Phase::Done;
            status = "Rover has the save.";
        } else {
            MVLOG("Wii: %c%c%c%c", d[3], d[2], d[1], d[0]);
        }
    }
    bool nextPacket(std::vector<u8>& pkt) override {
        std::lock_guard lk(m);
        if (out.empty()) return false;
        if (out.front().data) sent += out.front().bytes.size();
        pkt = std::move(out.front().bytes);
        out.pop_front();
        if (out.empty() && logOn()) MVLOG("sent %zu bytes and the CRC-32", sent);
        return true;
    }
    void disconnected() override {
        std::lock_guard lk(m);
        if (phase == Phase::Linked) {
            MVLOG("the link ended before the transfer finished");
            phase = Phase::Off;
        } else if (phase == Phase::Done) {
            MVLOG("the Wii closed the link");
        }
    }
};
Van& van() {
    static Van v;
    return v;
}

// Window state (host thread).
struct Ui {
    bool open = false;
    bool checkOnly = false;      // opened from F1 (Rover is not waiting)
    std::string path;            // text field
    bool loaded = false;
    ww::Save save;
    std::string error;
    int cart = 1;                // cartridge region (ww::Region)
    int pick = 0;                // -1: let Rover ask
    std::string picked;          // from the file dialog (callback thread)
    std::mutex pickM;
    clk::time_point doneAt;
    char buf[1024] = {};
};
Ui& ui() {
    static Ui u;
    return u;
}

std::atomic<int> sPick{-1};    // the resident to offer (-1 every one), read by the fn_80085894 hook
std::atomic<u32> sBuf{0};      // the link manager's receive buffer (guest)
std::atomic<bool> sOpenRequest{false};

// Test aid CF_MOVE_IN=<save file>[,<resident 0-3 or -1>]: Rover's DS request is answered with that
// file at once (the window still shows). CF_MOVE_IN_REGION=<0-3> sets the cartridge region,
// CF_MOVE_IN_ANY=1 accepts every region.
struct Auto {
    std::string file;
    int pick = -2;
};
const Auto& autoCfg() {
    static const Auto a = [] {
        Auto r;
        if (const char* e = std::getenv("CF_MOVE_IN")) {
            std::string s = e;
            const size_t comma = s.rfind(',');
            if (comma != std::string::npos && comma + 1 < s.size() &&
                (std::isdigit(static_cast<unsigned char>(s[comma + 1])) || s[comma + 1] == '-')) {
                r.pick = std::atoi(s.c_str() + comma + 1);
                s.resize(comma);
            }
            r.file = s;
        }
        return r;
    }();
    return a;
}

bool anyRegion() {
    static const bool env = [] {
        const char* e = std::getenv("CF_MOVE_IN_ANY");
        return e && *e == '1';
    }();
    return env || port::settings().moveIn.anyRegion;
}

void loadSave(Ui& u, const std::string& path) {
    u.path = path;
    std::snprintf(u.buf, sizeof u.buf, "%s", path.c_str());
    u.error.clear();
    u.loaded = ww::loadFile(path, u.save, u.error);
    if (!u.loaded) {
        MVLOG("%s: %s", path.c_str(), u.error.c_str());
        return;
    }
    u.cart = int(ww::guessRegion(u.save, path));
    if (const char* e = std::getenv("CF_MOVE_IN_REGION")) u.cart = std::clamp(std::atoi(e), 0, 3);
    // Default resident: the only one, else let Rover ask.
    u.pick = -1;
    for (int i = 0; i < 4 && u.save.residents() == 1; i++)
        if (u.save.players[i].exists) u.pick = i;
    if (port::settings().moveIn.lastFile != path) {
        port::settings().moveIn.lastFile = path;
        port::saveSettings();
    }
    MVLOG("loaded %s: %s, %s layout, copies %s/%s, town \"%s\", %d resident(s)", path.c_str(), u.save.format.c_str(),
          ww::layoutName(u.save.layout), u.save.copyOk[0] ? "ok" : "bad", u.save.copyOk[1] ? "ok" : "bad",
          nameOf(u.save.townRaw, u.save.layout).c_str(), u.save.residents());
    for (int i = 0; i < 4; i++) {
        const auto& p = u.save.players[i];
        if (p.exists)
            MVLOG("  resident %d: \"%s\" (%s), player id %04X, town id %04X, face %d, hair %d, colour %d", i,
                  nameOf(p.nameRaw, u.save.layout).c_str(), p.gender ? "girl" : "boy", p.playerId, p.townId, p.face,
                  p.hair, p.hairColor);
    }
}

// What the DS sends for the loaded save (the image, a converted image, or the Game Card error).
std::vector<u8> payloadFor(const Ui& u) {
    const ww::Region game = gameRegion(), cart = ww::Region(u.cart);
    if (!regionAccepted(cart, game) && !anyRegion()) {
        MVLOG("Game Card region %s, game %s: the Moving Van reports the wrong Game Card", ww::regionName(cart),
              ww::regionName(game));
        static const char kInvalid[16] = "B2mRWeDAInvalid";
        return std::vector<u8>(kInvalid, kInvalid + 16);
    }
    if (ww::layoutOf(game) != u.save.layout) {
        ww::CharTable jp, latin;
        charTables(jp, latin);
        MVLOG("converting the %s save for the %s game", ww::layoutName(u.save.layout), ww::layoutName(ww::layoutOf(game)));
        return ww::convert(u.save, ww::layoutOf(game), jp, latin);
    }
    return u.save.image;
}

void turnOnDs(Ui& u) {
    auto& v = van();
    std::lock_guard lk(v.m);
    v.payload = payloadFor(u);
    v.phase = Phase::Download;
    v.readyAt = clk::now() + std::chrono::milliseconds(int(kDownloadSeconds * 1000));
    v.status = "The DS is downloading the Moving Van...";
    v.sends = 0;
    v.queued = v.sent = 0;
    sPick = u.pick;
    MVLOG("DS on: Download Play of the Moving Van, resident %s", u.pick < 0 ? "chosen in the game" : std::to_string(u.pick).c_str());
}

void Van::downloadStarted(u32 ggid) {
    if (ggid != kMovingVanGgid) {
        if (logOn()) MVLOG("Download Play offered with ggid %06X (not the Moving Van): no DS answers", ggid);
        return;
    }
    {
        std::lock_guard lk(m);
        phase = Phase::Waiting;
        status.clear();
    }
    MVLOG("Rover asks for the Moving Van on a DS");
    sOpenRequest = true;  // the window opens on the host thread (this runs on a guest thread)
}

// Host thread: Rover started asking for the DS.
void openForRover(Ui& u) {
    u.open = true;
    u.checkOnly = false;
    u.doneAt = {};
    if (!autoCfg().file.empty()) {
        loadSave(u, autoCfg().file);
        if (autoCfg().pick >= -1) u.pick = autoCfg().pick;
        if (u.loaded) turnOnDs(u);
    } else if (!u.loaded && !port::settings().moveIn.lastFile.empty()) {
        loadSave(u, port::settings().moveIn.lastFile);
    }
}

struct Register {
    Register() { mp::setChild(&van()); }
} sRegister;

void SDLCALL onPicked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto& u = ui();
    std::lock_guard lk(u.pickM);
    u.picked = files[0];
}

void browse(const std::string& current) {
    static const SDL_DialogFileFilter filters[] = {
        {"Wild World saves", "sav;dsv;bin;dat;duc;nds.sav"},
        {"All files", "*"},
    };
    std::string dir;
    const size_t slash = current.find_last_of("/\\");
    if (slash != std::string::npos) dir = current.substr(0, slash + 1);
    SDL_ShowOpenFileDialog(&onPicked, nullptr, aurora_get_window(), filters, 2, dir.empty() ? nullptr : dir.c_str(), false);
}

}  // namespace

// Input waits while the window asks for something (not while it only shows the transfer, and not
// for the CF_MOVE_IN test aid).
std::atomic<bool> sBlocksInput{false};
bool windowOpen() { return sBlocksInput; }

void drawWindow() {
    auto& u = ui();
    if (sOpenRequest.exchange(false)) openForRover(u);
    {
        std::lock_guard lk(u.pickM);
        if (!u.picked.empty()) {
            loadSave(u, u.picked);
            u.picked.clear();
        }
    }
    Phase phase;
    std::string status;
    size_t sent, total;
    {
        auto& v = van();
        std::lock_guard lk(v.m);
        phase = v.phase;
        status = v.status;
        sent = v.sent;
        total = v.queued;
    }
    sBlocksInput = u.open && autoCfg().file.empty() && (u.checkOnly || phase == Phase::Waiting);
    if (!u.open) return;
    if (!u.checkOnly && phase == Phase::Off && u.doneAt == clk::time_point{}) {
        // Rover stopped waiting (quit in the game) before a DS showed up.
        u.open = false;
        return;
    }
    if (phase == Phase::Done) {
        if (u.doneAt == clk::time_point{}) u.doneAt = clk::now();
        if (clk::now() - u.doneAt > std::chrono::seconds(4)) {
            u.open = false;
            u.doneAt = {};
            return;
        }
    } else {
        u.doneAt = {};
    }

    const float s = recomp::ui::scale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560 * s, 0), ImGuiCond_Appearing);
    bool keep = true;
    if (ImGui::Begin("Move in from Wild World###movein", &keep, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool busy = phase == Phase::Download || phase == Phase::Linked;
        if (u.checkOnly && phase == Phase::Off)
            ImGui::TextWrapped("Check a Wild World save here. To move in, start a new resident and tell Rover "
                               "\"Move from the DS!\": this window then sends the save to the game.");
        else if (phase == Phase::Waiting)
            ImGui::TextWrapped("Rover is waiting for the Moving Van on a Nintendo DS. Pick your Animal Crossing: "
                               "Wild World save; the port plays the DS with it.");
        ImGui::Spacing();
        ImGui::BeginDisabled(busy);
        ImGui::SetNextItemWidth(380 * s);
        if (ImGui::InputText("##path", u.buf, sizeof u.buf, ImGuiInputTextFlags_EnterReturnsTrue)) loadSave(u, u.buf);
        ImGui::SameLine();
        if (recomp::menu::Button("Browse...")) browse(u.buf);
        ImGui::SameLine();
        if (recomp::menu::Button("Load")) loadSave(u, u.buf);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Raw .sav (256 / 512 KB), DeSmuME .dsv, No$GBA (uncompressed). The file is only read.");

        if (!u.error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", u.error.c_str());
        if (u.loaded) {
            const auto& sv = u.save;
            ImGui::SeparatorText("Save");
            ImGui::Text("Format: %s", sv.format.c_str());
            ImGui::Text("Layout: %s", ww::layoutName(sv.layout));
            ImGui::Text("Checksums: copy 1 %s, copy 2 %s", sv.copyOk[0] ? "OK" : "bad", sv.copyOk[1] ? "OK" : "bad");
            ImGui::Text("Town: %s%s", nameOf(sv.townRaw, sv.layout).c_str(), sv.townData ? "" : " (no town data: Rover will refuse it)");
            // Cartridge region: fixed for the Japanese / Korean layouts, a choice for the shared one.
            ImGui::BeginDisabled(busy);
            if (sv.layout == ww::Layout::USEU) {
                static const char* kRegions[] = {"North America", "Europe / Australia"};
                int sel = u.cart == int(ww::Region::EU) ? 1 : 0;
                ImGui::SetNextItemWidth(220 * s);
                if (ImGui::Combo("Game Card region", &sel, kRegions, 2)) u.cart = sel ? int(ww::Region::EU) : int(ww::Region::US);
                ImGui::SetItemTooltip("North American and European Wild World saves have the same layout; this is the\n"
                                      "region of the Game Card the save came from (guessed from the file name).");
            } else {
                ImGui::Text("Game Card region: %s", ww::regionName(ww::Region(u.cart)));
            }
            ImGui::EndDisabled();
            const ww::Region game = gameRegion();
            const bool ok = regionAccepted(ww::Region(u.cart), game);
            if (!ok && !anyRegion())
                ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                                   "A %s City Folk does not accept a %s Game Card:\nRover will say it is not the right Game Card.\n"
                                   "F1 > Enhancements > Gameplay > \"Wild World saves from any region\" allows it.",
                                   ww::regionName(game), ww::regionName(ww::Region(u.cart)));
            else if (!ok)
                ImGui::TextDisabled("Other region (allowed in F1)%s", ww::layoutOf(game) != sv.layout ? ": converted for this game" : "");

            ImGui::SeparatorText("Resident to move");
            ImGui::BeginDisabled(busy);
            int shown = 0;
            for (int i = 0; i < 4; i++) {
                const auto& p = sv.players[i];
                if (!p.exists) continue;
                shown++;
                char label[128];
                std::snprintf(label, sizeof label, "%s (%s) from %s##p%d", nameOf(p.nameRaw, sv.layout).c_str(),
                              p.gender ? "girl" : "boy", nameOf(sv.townRaw, sv.layout).c_str(), i);
                if (ImGui::RadioButton(label, u.pick == i)) u.pick = i;
            }
            if (shown > 1 && ImGui::RadioButton("Let Rover ask which one##rover", u.pick < 0)) u.pick = -1;
            if (shown == 0) ImGui::TextDisabled("Nobody lives in this town (Rover will refuse it).");
            ImGui::EndDisabled();
            ImGui::TextDisabled("Moves: name, face, hair style and colour, most of the catalog.\n"
                                "Stays behind: pockets, clothes worn, Bells, savings, house, encyclopedias.");
        }

        ImGui::Separator();
        if (phase == Phase::Waiting) {
            ImGui::BeginDisabled(!u.loaded);
            if (recomp::menu::Button("Turn on the DS")) turnOnDs(u);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (recomp::menu::Button("Close")) u.open = false;
            ImGui::SetItemTooltip("No DS shows up: tell Rover to quit (B) in the game.");
        } else if (busy || phase == Phase::Done) {
            ImGui::TextUnformatted(status.c_str());
            if (total) ImGui::ProgressBar(float(std::min(sent, total)) / float(total), ImVec2(380 * s, 0));
            if (phase == Phase::Done) ImGui::TextDisabled("Rover takes it from here.");
        } else if (recomp::menu::Button("Close")) {
            u.open = false;
        }
    }
    ImGui::End();
    if (!keep) u.open = false;
}

bool menuSection() {
    bool changed = false;
    auto& m = port::settings().moveIn;
    ImGui::SeparatorText("Move in from Wild World");
    changed |= recomp::menu::Checkbox("Wild World saves from any region", &m.anyRegion);
    ImGui::SetItemTooltip("Rover's \"Move from the DS!\" takes a Wild World save file (a window opens when he\n"
                          "asks for the DS). Like a Wii, the port accepts only Game Cards of this game's region\n"
                          "(the European game also North American ones). On: every region; Japanese and Korean\n"
                          "saves (another layout and character set) are converted for this game, characters it\n"
                          "lacks become '?'.");
    if (recomp::menu::Button("Check a save file...")) {
        auto& u = ui();
        u.open = true;
        u.checkOnly = true;
        if (!u.loaded && !m.lastFile.empty()) loadSave(u, m.lastFile);
    }
    ImGui::SetItemTooltip("Opens the move-in window to look at a save (its residents, town, checks).");
    if (!m.lastFile.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Last: %s", m.lastFile.c_str());
    }
    return changed;
}

}  // namespace recomp::movein

using namespace recomp;

// The game's MP link poll (USA fn_800E3EE4, this = link manager + 0x19C), once a frame while the
// link is up: the MP indications are delivered here (mp::deliver), on the game's thread and
// outside the link manager's mutex, as the library's own thread would.
HLE(fn_800E3EE4) {
    const u32 mgr = c.r[3] - 0x19C;
    recomp::movein::sBuf = rd32(mgr + 0x1C8);
    recomp::mp::deliver(c);
    callOriginal(c, "fn_800E3EE4");
}

// "Imported resident exists" (USA fn_80085894, r3 = player slot in the received save): with a
// resident picked in the window, the others are not offered (Rover asks "So you're X from Y?").
HLE(fn_80085894) {
    const u32 p = c.r[3], buf = recomp::movein::sBuf;
    const int pick = recomp::movein::sPick;
    if (pick >= 0 && buf && p >= buf && p < buf + ww::kBackupSize) {
        const auto& L = ww::layout(ww::layoutOf(recomp::movein::gameRegion()));
        const u32 off = (p - buf) % L.copySize;
        const int idx = off >= L.players ? int((off - L.players) / L.playerStride) : -1;
        if (idx != pick) {
            c.r[3] = 0;
            return;
        }
    }
    callOriginal(c, "fn_80085894");
}
