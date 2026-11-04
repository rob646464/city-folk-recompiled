// In-game overlay menu (F1 toggles, Esc closes): Visuals / Sound / Controls / Enhancements / Game.
// Drawn with Aurora's Dear ImGui on the host thread between the guest's present
// and aurora_end_frame. While it is open the game receives neutral pad input.
#include "recomp/menu.h"
#include "game/cf_calendar.h"
#include "game/cf_features.h"
#include "game/cf_mii.h"
#include "recomp/system.h"
#include "recomp/game.h"
#include "recomp/gx_bridge.h"
#include "game/cf_move_in.h"
#include "game/cf_region.h"

#include "port/install.h"
#include "port/options.h"
#include "port/settings.h"
#include "port/update.h"
#include "recomp/audio.h"
#include "recomp/controls.h"
#include "recomp/wiimote.h"
#include "recomp/crt.h"
#include "recomp/distributions.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/frame.h"
#include "recomp/interp.h"
#include "recomp/online.h"
#include "recomp/online_sync.h"
#include "recomp/pacing.h"
#include "recomp/texture_packs.h"
#include "recomp/ui_scale.h"

#include <aurora/aurora.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>

#include <dolphin/vi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
namespace world {
bool drawMenu();  // src/game/cf_world.cpp (F1 > Visuals > Enhanced world)
}
}

namespace recomp::menu {
namespace {

// Test aid: CF_MENU_TAB=<Enhancements sub-tab name> starts with the menu open on that tab.
const char* sEnvTab = std::getenv("CF_MENU_TAB");
bool sOpen = sEnvTab != nullptr;

std::string sOpenSub;  // sub-tab to select once (CF_MENU_ID_CHECK sweep)

// Flags for a sub-tab: selected while CF_MENU_TAB names it, or once when the sweep asks for it.
ImGuiTabItemFlags envTab(const char* name) {
    if (!sOpenSub.empty() && sOpenSub == name) {
        sOpenSub.clear();
        return ImGuiTabItemFlags_SetSelected;
    }
    if (!sEnvTab || std::strcmp(sEnvTab, name) != 0) return 0;
    return ImGuiTabItemFlags_SetSelected;
}
bool sDirty = false;
std::atomic<bool> sQuit{false};  // "Quit game" confirmed, or the game left (requestQuit)
// The menu window. The ID after ### is the old title, so imgui.ini keeps the window's saved size
// and place.
constexpr const char* kMenuWindow = "Animal Crossing City Folk Recompiled  (F1)###Animal Crossing City Folk PC  (F1)";
std::string sOpenTab = sEnvTab ? "Enhancements" : "";  // tab to select once (CF_MENU / CF_MENU_TAB)

// ---- Test aid CF_MENU_ID_CHECK=1: Dear ImGui ID-conflict sweep ----------------------------
// Two widgets with the same ID in one scope (same label without a ##suffix, or the same ##id)
// are drawn red on hover by ImGui and neither can be clicked. ImGui only notices a conflict
// for the hovered item, so this walks every tab and sub-tab (tree nodes opened), moves ImGui's
// mouse over the whole page row by row (scrolling down) and logs each conflicting ID with its
// ID-stack path (from ImGui's ID Stack Tool, i.e. the labels). The menu is made as tall as the
// window (680 wide, the default width) and each page/scroll position is held for a moment
// with a "[menu-id] shot" line, so a capture script can screenshot every page.
// CF_MENU_ID_CHECK=exit quits when the sweep is done; =selftest also draws two buttons with one
// ID on the Game tab (they must be reported: proves the detection works) and quits.
namespace idcheck {
struct Page {
    const char* top;
    const char* sub;
};
constexpr Page kPages[] = {
    {"Visuals", "General"},       {"Visuals", "Rendering"},        {"Visuals", "Enhanced lighting"}, {"Visuals", "Enhanced world"},
    {"Visuals", "Texture packs"},
    {"Sound", nullptr},           {"Controls", "Ports"},           {"Controls", "Bindings"},
    {"Controls", "Pointer"},      {"Controls", "Analog"},       {"Controls", "DSU"},             {"Enhancements", "General"},     {"Enhancements", "Gameplay"},
    {"Enhancements", "Silly"},          {"Enhancements", "Unused content"}, {"Enhancements", "Debug"}, {"Game", nullptr}, {"Miis", nullptr}, {"Online", "Play together"}, {"Online", "Nintendo events"},
};
constexpr int kPageCount = int(sizeof(kPages) / sizeof(kPages[0]));
constexpr float kXs[] = {0.05f, 0.3f, 0.6f, 0.88f};
constexpr int kXCount = int(sizeof(kXs) / sizeof(kXs[0]));
constexpr float kRowStep = 8.0f;
constexpr int kSettle = 20, kDwell = 45;  // frames: tab switch / scroll settle, screenshot hold

const char* sMode = std::getenv("CF_MENU_ID_CHECK");
bool active() { return sMode && *sMode && std::strcmp(sMode, "0") != 0; }
bool selfTest() { return active() && std::strcmp(sMode, "selftest") == 0; }

int sPage = -1, sScrollIndex = 0, sWait = 0, sSample = 0, sResolve = 0, sConflicts = 0;
bool sDone = false, sShotPrinted = false;
float sScroll = 0.0f;
ImRect sClip;               // menu window's inner clip rect (last frame)
float sScrollMax = 0.0f;
ImGuiWindow* sWindow = nullptr;
ImVec2 sHistory[4];          // mouse positions issued in the last frames (detection lags 3 frames)
ImVec2 sResolvePos;
ImGuiID sResolveId = 0;
std::vector<ImGuiID> sSeen;

ImVec2 sNextPos(-1.0f, -1.0f);  // mouse position for the next frame

// The position is applied at the start of the next frame's menu (applyMouse): the SDL backend
// feeds the real cursor every frame while the window has focus, so an input event would lose.
void mouse(ImVec2 p) {
    sNextPos = p;
    for (int i = 3; i > 0; i--) sHistory[i] = sHistory[i - 1];
    sHistory[0] = p;
}

void applyMouse() {
    ImGuiContext& g = *GImGui;
    g.IO.MousePos = sNextPos;
    ImGuiWindow* hovered = nullptr;
    ImGuiWindow* under = nullptr;
    ImGui::FindHoveredWindowEx(sNextPos, false, &hovered, &under);
    g.HoveredWindow = hovered;
    g.HoveredWindowUnderMovingWindow = under;
}

void startPage() {
    const Page& p = kPages[sPage];
    sOpenTab = p.top;
    sScroll = 0.0f;
    sScrollIndex = 0;
    sSample = 0;
    sWait = kSettle + kDwell;
    sShotPrinted = false;
}

// Before the menu window: keep it 680 wide (default) and as tall as the display.
void beforeWindow() {
    if (!active()) return;
    // Never save the sweep's window size/position to imgui.ini: on Windows it lives in the real
    // %APPDATA% (SDL's pref path) even when a test run overrides APPDATA.
    ImGui::GetIO().IniFilename = nullptr;
    if (sDone) return;
    if (sPage >= 0) applyMouse();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float s = recomp::ui::scale();  // (the default width at the current menu size)
    ImGui::SetNextWindowSize(ImVec2(std::min(680.0f * s, ds.x - 16.0f), std::max(400.0f, ds.y - 16.0f)), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
}

// Inside the menu window, after its contents.
void insideWindow() {
    if (!active() || sDone) return;
    sWindow = ImGui::GetCurrentWindow();
    sClip = sWindow->InnerClipRect;
    sScrollMax = ImGui::GetScrollMaxY();
    if (sPage >= 0 && sWait > 0 && sWait == kSettle + kDwell - 8) {
        // Sub-tab (a few frames after the top tab switched, so the right tab bar is drawn).
        const Page& p = kPages[sPage];
        if (p.sub) {
            if (std::strcmp(p.top, "Controls") == 0) recomp::controls::selectSubTab(p.sub);
            else sOpenSub = p.sub;
        }
    }
    if (sWait > 0) ImGui::SetScrollY(sScroll);
}

// After the menu window: detection, mouse movement, paging.
void afterWindow() {
    if (!active() || sDone) return;
    ImGuiContext& g = *GImGui;
    if (sPage < 0) {
        sPage = 0;
        startPage();
        std::printf("[menu-id] sweep start (%d pages)\n", kPageCount);
        std::fflush(stdout);
        return;
    }
    const Page& page = kPages[sPage];
    if (sResolve > 0) {
        // Hold the mouse on the conflicting item while the ID Stack Tool resolves its path.
        mouse(sResolvePos);
        ImGui::SetNextWindowPos(ImVec2(g.IO.DisplaySize.x - 420.0f, g.IO.DisplaySize.y - 160.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(410.0f, 150.0f), ImGuiCond_Always);
        ImGui::ShowIDStackToolWindow();
        if (--sResolve == 0) {
            std::printf("[menu-id] CONFLICT 0x%08X on %s%s%s at (%.0f, %.0f) scroll %.0f: %s\n", sResolveId, page.top,
                        page.sub ? " > " : "", page.sub ? page.sub : "", sResolvePos.x - sClip.Min.x,
                        sResolvePos.y - sClip.Min.y, sScroll, g.DebugIDStackTool.ResultPathBuf.c_str());
            std::fflush(stdout);
        }
        return;
    }
    if (g.DebugDrawIdConflicts != 0 &&
        std::find(sSeen.begin(), sSeen.end(), g.DebugDrawIdConflicts) == sSeen.end()) {
        sSeen.push_back(g.DebugDrawIdConflicts);
        sConflicts++;
        sResolveId = g.DebugDrawIdConflicts;
        sResolvePos = sHistory[2];  // issued 3 frames ago (queued, hovered, counted)
        sResolve = 24;
        mouse(sResolvePos);
        return;
    }
    if (sWait > 0) {
        mouse(ImVec2(g.IO.DisplaySize.x - 2.0f, g.IO.DisplaySize.y - 2.0f));  // off the menu
        if (--sWait < kDwell && !sShotPrinted) {
            sShotPrinted = true;
            std::printf("[menu-id] shot %s%s%s scroll %d\n", page.top, page.sub ? "-" : "", page.sub ? page.sub : "",
                        sScrollIndex);
            std::fflush(stdout);
        }
        return;
    }
    const int rows = std::max(1, int((sClip.GetHeight() - 4.0f) / kRowStep));
    if (sSample < rows * kXCount) {
        const int row = sSample / kXCount, col = sSample % kXCount;
        mouse(ImVec2(sClip.Min.x + sClip.GetWidth() * kXs[col], sClip.Min.y + 2.0f + float(row) * kRowStep));
        sSample++;
        return;
    }
    if (sScroll + 1.0f < sScrollMax) {
        sScroll = std::min(sScrollMax, sScroll + sClip.GetHeight() - 60.0f);
        sScrollIndex++;
        sSample = 0;
        sWait = kSettle + kDwell;
        sShotPrinted = false;
        return;
    }
    if (++sPage < kPageCount) {
        startPage();
        return;
    }
    sDone = true;
    std::printf("[menu-id] sweep done: %d ID conflict%s\n", sConflicts, sConflicts == 1 ? "" : "s");
    std::fflush(stdout);
    if (std::strcmp(sMode, "exit") == 0 || selfTest()) sQuit = true;
}
}  // namespace idcheck

// ---- Test aid CF_MENU_CLICK=<frame>:<action>[;<frame>:<action>...] -----------------------
// Drives the menu the way a player does, at guest frames (game::guestFrames). Actions:
//   open=<Tab>[/<Sub-tab>]  opens the menu on that tab (e.g. open=Enhancements/Unused content)
//   close                   closes it (changes are saved as on Esc)
//   <label>                 clicks the checkbox / button with exactly that label (as passed to
//                           ImGui, "##" suffix included) once it is drawn: scrolled into view,
//                           then activated through ImGui's item activation (the widget's own
//                           behaviour, as a mouse click). Logs "[menu-click]".
// The game keeps running while the menu is open (scripted --input still reaches it).
namespace click {
struct Action {
    u32 frame;
    std::string what;
};
std::vector<Action> sActions = [] {
    std::vector<Action> out;
    const char* v = std::getenv("CF_MENU_CLICK");
    for (const char* p = v; p && *p;) {
        const char* end = std::strchr(p, ';');
        const std::string item(p, end ? size_t(end - p) : std::strlen(p));
        const size_t colon = item.find(':');
        if (colon != std::string::npos)
            out.push_back({u32(std::strtoul(item.c_str(), nullptr, 10)), item.substr(colon + 1)});
        if (!end) break;
        p = end + 1;
    }
    return out;
}();
int sStep = -1;
int sLastSeen = 0;  // ImGui frame the pending widget was last drawn

bool due() { return !sActions.empty() && game::guestFrames() >= sActions.front().frame; }
void done(const char* what) {
    std::printf("[menu-click] %s frame %u\n", what, game::guestFrames());
    std::fflush(stdout);
    sActions.erase(sActions.begin());
    sStep = -1;
}

// Menu-level actions, once per host frame before the menu is drawn.
void frame() {
    if (!due()) return;
    const std::string& w = sActions.front().what;
    // A clicked widget that is no longer drawn (its click changed the page, e.g. "Download and
    // install" becomes a progress bar) is done.
    if (sStep >= 3 && ImGui::GetFrameCount() - sLastSeen > 2) {
        done((w + " (gone after the click)").c_str());
        return;
    }
    if (w.rfind("open=", 0) == 0) {
        const std::string t = w.substr(5);
        const size_t slash = t.find('/');
        sOpen = true;
        sOpenTab = t.substr(0, slash);
        if (slash != std::string::npos) sOpenSub = t.substr(slash + 1);
        if (sOpenTab == "Controls" && !sOpenSub.empty()) {  // its sub-tabs: Ports, Bindings, Analog
            static std::string sControlsSub;
            sControlsSub = sOpenSub;
            recomp::controls::selectSubTab(sControlsSub.c_str());
        }
        done(w.c_str());
    } else if (w == "close") {
        sOpen = false;  // as Esc (handleEvent): changes are saved when the menu closes
        if (sDirty) {
            port::saveSettings();
            sDirty = false;
        }
        done("close");
    }
}

// After an option's widget: click it when it is the pending action.
void afterItem(const char* label) {
    if (!due()) return;
    static const bool sLog = std::getenv("CF_MENU_CLICK_LOG") != nullptr;  // labels seen while a click waits
    if (sLog) std::printf("[menu-click] seen '%s' (waiting for '%s')\n", label, sActions.front().what.c_str());
    // Once found, the widget is followed by its ID: a button whose label changes when clicked
    // ("Gallery" -> "Close gallery###gallery") keeps its ID, not its label.
    static ImGuiID sId = 0;
    // A "###id" label matches by its id part (the shown text in front of it may change).
    auto matches = [&] {
        const std::string& w = sActions.front().what;
        const size_t a = w.find("###");
        const char* b = std::strstr(label, "###");
        return a != std::string::npos && b ? w.compare(a, std::string::npos, b) == 0 : w == label;
    };
    if (sStep >= 0 ? ImGui::GetItemID() != sId : !matches()) return;
    sLastSeen = ImGui::GetFrameCount();
    if (sStep < 0) {
        ImGui::SetScrollHereY(0.5f);
        sId = ImGui::GetItemID();
        sStep = 0;
        return;
    }
    if (++sStep == 3) ImGui::ActivateItemByID(sId);
    else if (sStep == 5) done(sActions.front().what.c_str());
}
}  // namespace click

bool tab(const char* name) {
    const bool select = sOpenTab == name;
    if (select) sOpenTab.clear();
    return ImGui::BeginTabItem(name, nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
}

void applyResolution() { VISetFrameBufferScale(float(port::settings().video.resolutionScale)); }

void applyWindowSettings() {
    const auto& v = port::settings().video;
    applyResolution();
    if (SDL_Window* w = aurora_get_window()) SDL_SetWindowFullscreen(w, v.fullscreen);
    // Automated runs (scripted --input, or CF_BACKGROUND=1) never pause when another window
    // takes focus, so several test instances can run side by side.
    static const bool sBackground = !port::options().inputScript.empty() || std::getenv("CF_BACKGROUND") != nullptr;
    aurora_set_pause_on_focus_lost(port::settings().enhancements.pauseOnFocusLoss && !sBackground);
    if (sBackground) aurora_set_background_input(true);
}

template <typename E>
bool combo(const char* label, E& value, const char* const* names, int count) {
    int i = int(value);
    const bool changed = ImGui::Combo(label, &i, names, count);
    if (changed) value = E(i);
    return changed;
}

// "(note)" after the last item, or wrapped on the next line when it would be cut off.
void pending(const char* what) {
    char text[256];
    std::snprintf(text, sizeof(text), "(%s)", what);
    ImGui::SameLine();
    if (ImGui::CalcTextSize(text).x <= ImGui::GetContentRegionAvail().x) {
        ImGui::TextDisabled("%s", text);
        return;
    }
    ImGui::NewLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Feature gating (port::Avail, docs/SETUP.md): a feature missing its disc is greyed out
// with the reason; one that applies to the other installed version stays selectable.
struct Gate {
    port::Avail a;
    explicit Gate(port::Avail avail) : a(avail) { ImGui::BeginDisabled(blocked()); }
    bool blocked() const {
        return a == port::Avail::NeedsUsDisc || a == port::Avail::NeedsPalDisc || a == port::Avail::NotYet;
    }
    ~Gate() {
        ImGui::EndDisabled();
        if (const char* note = port::availNote(a)) pending(note);
    }
};

// ---- Updates (src/port/update.cpp, docs/SETUP.md "Updates") ----------------------------------
// The release notes of the available version, markdown reduced to headings, bullets and text.
void releaseNotes(const port::update::Info& info) {
    ImGui::BeginChild("##release_notes", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 9), ImGuiChildFlags_Borders);
    for (const auto& l : info.lines) {
        switch (l.kind) {
        case port::update::NoteLine::Heading: ImGui::SeparatorText(l.text.c_str()); break;
        case port::update::NoteLine::Bullet:
            if (l.indent) ImGui::Indent(float(l.indent) * ImGui::GetStyle().IndentSpacing);
            ImGui::Bullet();
            ImGui::TextWrapped("%s", l.text.c_str());
            if (l.indent) ImGui::Unindent(float(l.indent) * ImGui::GetStyle().IndentSpacing);
            break;
        case port::update::NoteLine::Blank: ImGui::Spacing(); break;
        default: ImGui::TextWrapped("%s", l.text.c_str()); break;
        }
    }
    if (info.lines.empty()) ImGui::TextDisabled("(no release notes)");
    ImGui::EndChild();
}

void updatesSection() {
    namespace up = port::update;
    ImGui::SeparatorText("Updates");
    const up::State st = up::state();
    if (std::strcmp(up::currentVersion(), up::buildVersion()) != 0)
        ImGui::Text("Animal Crossing City Folk Recompiled %s (CF_UPDATE_VERSION: checking as %s)", up::buildVersion(), up::currentVersion());
    else
        ImGui::Text("Animal Crossing City Folk Recompiled %s", up::buildVersion());
    auto& u = port::settings().updates;
    sDirty |= recomp::menu::Checkbox("Check for updates at startup", &u.checkAtStartup);
    ImGui::SetItemTooltip("When the game starts, asks GitHub in the background whether a newer release is out\n"
                          "and shows a short notice. Nothing is downloaded or installed without your click.");
    const bool busy = st.phase == up::Phase::Checking || st.phase == up::Phase::Downloading ||
                      st.phase == up::Phase::Verifying || st.phase == up::Phase::Extracting ||
                      st.phase == up::Phase::Installing;
    ImGui::BeginDisabled(busy);
    if (recomp::menu::Button("Check for updates")) up::checkNow();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Asks GitHub for the latest release now (also shows a version you chose to skip).");
    const bool offer = st.phase == up::Phase::Available || st.phase == up::Phase::Downloading ||
                       st.phase == up::Phase::Verifying || st.phase == up::Phase::Extracting ||
                       st.phase == up::Phase::Ready || st.phase == up::Phase::Installing ||
                       (st.phase == up::Phase::Failed && !st.info.tag.empty());
    if (st.phase == up::Phase::Checking) {
        ImGui::SameLine();
        ImGui::TextDisabled("checking...");
    } else if (st.phase == up::Phase::UpToDate) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.message.c_str());
    } else if (st.phase == up::Phase::CheckFailed) {
        ImGui::TextWrapped("%s", st.message.c_str());
    }
    if (!offer) return;
    if (!st.manual && u.skipVersion == st.info.tag) {
        ImGui::TextDisabled("Version %s is available; you chose to skip it ('Check for updates' shows it again).",
                            st.info.tag.c_str());
        return;
    }
    if (up::dismissed() && st.phase == up::Phase::Available) {
        ImGui::TextDisabled("Update available: %s (Later; 'Check for updates' shows it again).", st.info.tag.c_str());
        return;
    }
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "Update available: %s", st.info.tag.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s%s", st.info.title.c_str(), st.info.published.empty() ? "" : ", ", st.info.published.c_str());
    releaseNotes(st.info);
    const bool canInstall = st.packaged && st.installWritable && !st.info.assetName.empty();
    switch (st.phase) {
    case up::Phase::Downloading: {
        const float frac = st.total ? float(double(st.got) / double(st.total)) : 0.0f;
        char label[64];
        std::snprintf(label, sizeof(label), "%.1f / %.1f MB", double(st.got) / 1048576.0, double(st.total) / 1048576.0);
        ImGui::ProgressBar(frac, ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0), label);
        ImGui::SameLine();
        if (recomp::menu::Button("Cancel download")) up::cancel();
        break;
    }
    case up::Phase::Verifying:
    case up::Phase::Extracting:
        ImGui::TextUnformatted(st.message.c_str());
        ImGui::SameLine();
        if (recomp::menu::Button("Cancel download")) up::cancel();
        break;
    case up::Phase::Installing: ImGui::TextUnformatted("Installing..."); break;
    case up::Phase::Ready:
        ImGui::TextWrapped("%s is downloaded and verified. Save your game first: installing restarts the game, and "
                           "progress since your last save is lost. Settings, saves and the installed discs are kept.",
                           st.info.tag.c_str());
        if (recomp::menu::Button("Install and restart")) ImGui::OpenPopup("Install update?");
        ImGui::SameLine();
        if (recomp::menu::Button("Later")) up::later();
        if (ImGui::BeginPopupModal("Install update?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Install %s and restart now?", st.info.tag.c_str());
            ImGui::TextDisabled("Unsaved game progress is lost.");
            if (recomp::menu::Button("Install and restart##confirm")) {
                if (sDirty) {
                    port::saveSettings();
                    sDirty = false;
                }
                ImGui::CloseCurrentPopup();
                up::installAndRestart();  // returns only on failure (message shown)
            }
            ImGui::SameLine();
            if (recomp::menu::Button("Not now")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        break;
    default:  // Available, Failed
        if (!st.message.empty()) {
            if (st.phase == up::Phase::Failed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
            ImGui::TextWrapped("%s", st.message.c_str());
            if (st.phase == up::Phase::Failed) ImGui::PopStyleColor();
        }
        ImGui::BeginDisabled(!canInstall);
        if (recomp::menu::Button("Download and install")) up::download();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Downloads %s (%.0f MB) from GitHub, checks its SHA-256 and unpacks it next to the game.\n"
                              "Nothing is replaced until you click 'Install and restart' afterwards.",
                              st.info.assetName.empty() ? "the package" : st.info.assetName.c_str(),
                              double(st.info.assetSize) / 1048576.0);
        ImGui::SameLine();
        if (recomp::menu::Button("Skip this version")) up::skipThisVersion();
        ImGui::SetItemTooltip("No notice for %s again (a newer release is still announced).", st.info.tag.c_str());
        ImGui::SameLine();
        if (recomp::menu::Button("Later")) up::later();
        ImGui::SetItemTooltip("Hides the notice until the next start.");
        if (!canInstall || st.phase == up::Phase::Failed) {
            if (recomp::menu::Button("Open download page")) SDL_OpenURL(st.info.page.c_str());
            ImGui::SetItemTooltip("%s", st.info.page.c_str());
        }
        break;
    }
}

// A short notice when a newer release was found (startup check): bottom-right corner for a few
// seconds, takes no input, once per session. The details are in F1 > Game.
void updateToast() {
    static double sShownAt = -1.0;
    if (sOpen || !port::update::shouldNotify()) return;
    const double now = ImGui::GetTime();
    if (sShownAt < 0.0) {
        sShownAt = now;
        std::printf("[update] notice shown: %s\n", port::update::state().info.tag.c_str());
        std::fflush(stdout);
    }
    const double age = now - sShownAt;
    constexpr double kShow = 12.0;
    if (age > kShow) return;
    const float alpha = age > kShow - 1.0 ? float(kShow - age) : 1.0f;
    const auto st = port::update::state();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16.0f * recomp::ui::scale(), vp->WorkPos.y + vp->WorkSize.y - 16.0f * recomp::ui::scale()),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##update_toast", nullptr, kFlags)) {
        ImGui::Text("Update available: %s", st.info.tag.c_str());
        ImGui::TextDisabled("F1 > Game to see what's new");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// "Press F1" hint (F1 > Visuals > General, setting [interface] start_hint): top-left for the first
// 8 s once the game shows frames (not during setup or "Preparing shaders"), never again in a session
// after the menu was opened. Takes no input.
void startHint() {
    static double sShownAt = -1.0;
    static bool sDone = false;
    if (sOpen) sDone = true;
    if (sDone || !port::settings().ui.startHint || recomp::frame::guestFrames() == 0) return;
    const double now = ImGui::GetTime();
    if (sShownAt < 0.0) {
        sShownAt = now;
        std::printf("[ui] \"Press F1\" hint shown\n");
        std::fflush(stdout);
    }
    const double age = now - sShownAt;
    constexpr double kShow = 8.0;
    if (age > kShow) {
        sDone = true;
        return;
    }
    const float alpha = age > kShow - 1.0 ? float(kShow - age) : age < 0.3 ? float(age / 0.3) : 1.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 16.0f * recomp::ui::scale(), vp->WorkPos.y + 16.0f * recomp::ui::scale()),
                            ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##start_hint", nullptr, kFlags)) ImGui::TextUnformatted("Press F1 to open Settings");
    ImGui::End();
    ImGui::PopStyleVar();
}

// Under Gamescope (Steam Deck game mode) VSync is always on, so a frame rate that does not divide
// the screen's refresh rate is shown at the even fraction below it (60 fps on a 90 Hz Steam Deck
// OLED: 45). Players then see a lower rate than they picked: a notice once per session, bottom-left,
// after the condition has held for 2 s (the refresh rate is settled). The F1 slider says it too.
void refreshRateToast() {
    if (!recomp::pacing::gamescope()) return;
    static double sSince = -1.0, sShownAt = -1.0;
    static float sFit = 0, sHz = 0;
    static int sTarget = 0;
    const double now = ImGui::GetTime();
    if (sShownAt < 0.0) {
        const auto& v = port::settings().video;
        const int target = recomp::frame::targetFps();
        const float hz = recomp::frame::displayRefreshHz();
        const float fit = v.unlimitedFps || !recomp::frame::interpolating() ? 0.0f : recomp::pacing::fixedRefreshFps(target);
        if (sOpen || fit <= 0 || float(target) > hz + 0.5f || fit >= float(target) - 0.5f) {
            sSince = -1.0;
            return;
        }
        if (sSince < 0.0) sSince = now;
        if (now - sSince < 2.0) return;
        sShownAt = now;
        sFit = fit;
        sHz = hz;
        sTarget = target;
        std::printf("[pacing] notice shown: %d fps shown at %.0f fps on a %.0f Hz screen (Gamescope)\n", target, fit, hz);
        std::fflush(stdout);
    }
    const double age = now - sShownAt;
    constexpr double kShow = 15.0;
    if (sOpen || age > kShow) return;
    const float alpha = age > kShow - 1.0 ? float(kShow - age) : 1.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 16.0f * recomp::ui::scale(), vp->WorkPos.y + vp->WorkSize.y - 16.0f * recomp::ui::scale()), ImGuiCond_Always,
                            ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##refresh_toast", nullptr, kFlags)) {
        ImGui::Text("Running at %.0f fps: %d fps doesn't divide the screen's %.0f Hz evenly.", sFit, sTarget, sHz);
        ImGui::TextDisabled("For %d fps, set Refresh rate to %d Hz in the Quick Access menu (Performance),", sTarget, sTarget);
        ImGui::TextDisabled("or pick %.0f fps / \"Match display refresh rate\" in F1 > Visuals.", sHz);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ---- Data folder / portable mode (src/port/install.cpp, docs/SETUP.md "Portable mode") ---------
void dataSection() {
    ImGui::SeparatorText("Data folder");
    const auto& loc = port::dataLocation();
    const auto u8 = [](const std::filesystem::path& p) {
        const auto s = p.u8string();
        return std::string(s.begin(), s.end());
    };
    static std::string sMoveError;
    if (loc.portable) {
        ImGui::Text("Portable: settings, saves, the game module and caches are stored next to the program in");
        ImGui::TextWrapped("%s", u8(loc.root).c_str());
    } else {
        const auto dirs = port::systemDirs();
        ImGui::Text("System folder%s (settings, saves, the game module, caches):", dirs.size() > 1 ? "s" : "");
        for (auto& d : dirs) ImGui::TextWrapped("%s", u8(d).c_str());
    }
    if (!loc.moveResult.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, loc.moveFailed ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", loc.moveResult.c_str());
        ImGui::PopStyleColor();
    }
    if (!loc.problem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", loc.problem.c_str());
        ImGui::PopStyleColor();
    }
    const bool toPortable = !loc.portable;
    if (toPortable) {
        if (recomp::menu::Button("Make this installation portable...")) {
            sMoveError = port::checkDataMove(true);
            std::printf("[data] make portable: %s\n", sMoveError.empty() ? "confirm" : sMoveError.c_str());
            std::fflush(stdout);
            if (sMoveError.empty()) ImGui::OpenPopup("Make portable?");
        }
        ImGui::SetItemTooltip("Moves everything into a folder 'portable' next to the program, so the whole game folder\n"
                              "can be moved to another drive or PC, and deleting it removes everything.");
    } else {
        ImGui::BeginDisabled(loc.forcedByEnv);
        if (recomp::menu::Button("Move data back to the system folder...")) {
            sMoveError = port::checkDataMove(false);
            std::printf("[data] move back: %s\n", sMoveError.empty() ? "confirm" : sMoveError.c_str());
            std::fflush(stdout);
            if (sMoveError.empty()) ImGui::OpenPopup("Move data back?");
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(loc.forcedByEnv ? "Portable mode is forced by the environment variable CF_PORTABLE=1."
                                              : "Moves everything from the portable folder to the system folder\n"
                                                "(%%APPDATA%% on Windows, ~/.config, ~/.local/share and ~/.cache on Linux).");
    }
    for (const char* popup : {"Make portable?", "Move data back?"}) {
        if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) continue;
        const bool p = popup[0] == 'M' && popup[1] == 'a';
        ImGui::Text("%s", p ? "Move the data into the portable folder and restart?"
                            : "Move the data back to the system folder and restart?");
        ImGui::TextDisabled("Save your game first: the game restarts and unsaved progress is lost.");
        ImGui::TextDisabled("The data is copied and checked first; if anything fails, nothing is changed.");
        if (recomp::menu::Button(p ? "Move and restart##portable" : "Move and restart##system")) {
            port::saveSettings();  // the copy must hold the current settings
            sDirty = false;
            ImGui::CloseCurrentPopup();
            sMoveError = port::requestDataMove(p);  // returns only on failure
        }
        ImGui::SameLine();
        if (recomp::menu::Button(p ? "Cancel##portable" : "Cancel##system")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!sMoveError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", sMoveError.c_str());
        ImGui::PopStyleColor();
    }
}

void gameTab() {
    if (idcheck::selfTest()) {  // CF_MENU_ID_CHECK=selftest: a deliberate ID conflict
        recomp::menu::Button("ID check self-test");
        ImGui::SameLine();
        recomp::menu::Button("ID check self-test");
    }
    auto& inst = port::install();
    if (inst.staticBuild) {
        ImGui::Text("Developer build: %s, recompiled at build time.", port::versionName(inst.running));
        ImGui::TextDisabled("CityFolkRecomp manages discs and versions (docs/SETUP.md).");
    } else {
        for (port::GameVersion v : port::kVersions) {
            ImGui::Text("%s disc:", port::regionName(v));
            ImGui::SameLine();
            if (inst.isReady(v)) ImGui::TextWrapped("%s", inst.discOf(v).c_str());
            else ImGui::TextDisabled("not installed");
        }
        ImGui::Text("Playing: %s version (%s)", port::regionName(inst.running), port::gameId(inst.running));
        if (inst.readyCount() > 1) {
            int base = int(inst.base);
            ImGui::Text("Version to play:");
            bool changed = false;
            for (port::GameVersion v : port::kVersions) {
                if (!inst.isReady(v)) continue;
                ImGui::SameLine();
                changed |= ImGui::RadioButton(port::regionName(v), &base, int(v));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            ImGui::SetItemTooltip("Which installed disc's game runs. Takes a restart.");
            if (changed) {
                inst.base = port::GameVersion(base);
                port::saveInstall();
            }
            if (inst.base != inst.running) {
                pending("restart to switch");
                if (recomp::menu::Button("Restart now")) {
                    port::saveSettings();
                    port::relaunch();
                }
                ImGui::SetItemTooltip("Saves the settings and restarts with the chosen version (unsaved game progress is lost).");
            }
        }
        if (recomp::menu::Button("Add or change discs...")) {
            port::saveSettings();
            port::relaunch(true);
        }
        ImGui::SetItemTooltip("Restarts into the setup screen to add a USA, European, Japanese or Korean disc or replace one\n"
                              "(unsaved game progress is lost).");
    }
    // The Wii's own settings as the game reads them (SYSCONF; src/recomp/sysconf.cpp, docs/NAND.md).
    ImGui::SeparatorText("Console");
    sDirty |= recomp::sys::drawMenu();
    dataSection();
    updatesSection();
}

// Hardware-accuracy toggles (all on by default). Each entry: checkbox bound to
// settings().rendering.<flag>, sDirty |= ..., and a tooltip saying what changes visually
// and roughly what it costs.
void renderingTab() {
    ImGui::TextDisabled("Hardware-accurate rendering. Everything is on by default for the best quality;");
    ImGui::TextDisabled("turn features off only if your PC needs the speed.");
    auto& r = port::settings().rendering;
    ImGui::SeparatorText("Color and depth");
    sDirty |= recomp::menu::Checkbox("Integer-accurate TEV", &r.integerTev);
    ImGui::SetItemTooltip(
        "Color combiner math as on the GameCube: 8-bit inputs, signed 11-bit registers and rounding\n"
        "after every stage. Slightly darker multi-stage shading and exact overflow; tiny GPU cost.");
    // Enhanced lighting turns the 6-bit framebuffer off (hle_gx.cpp): shown greyed out and unticked.
    const bool enhOverride = recomp::gx::enhancedOverridesEfbDepth();
    constexpr const char* kEnhNote =
        "\n\nOff while Enhanced lighting is on (F1 > Visuals > Enhanced lighting): its smooth\n"
        "per-pixel, colored-light, bloom and occlusion gradients would be cut into visible 6-bit\n"
        "steps (and 5-bit in the game's full-screen RGB565 copy). Turn Enhanced lighting off to\n"
        "use this option again; the setting itself is kept.";
    {
        ImGui::BeginDisabled(enhOverride);
        bool shown = r.efbColorDepth && !enhOverride;
        if (recomp::menu::Checkbox("EFB color depth (6-bit + dither)", &shown) && !enhOverride) {
            r.efbColorDepth = shown;
            sDirty = true;
        }
        ImGui::EndDisabled();
        const std::string tip = std::string(
            "The game renders into a 6-bit-per-channel framebuffer with an ordered dither, like the\n"
            "console, and its screen/texture copies keep their format's precision (RGB565: 5-bit red\n"
            "and blue in the full-screen copy the game redraws every frame; RGB5A3 for the Mario\n"
            "painting). Off: smooth 8-bit color (less banding/dither grain in dark rooms). No real cost.") +
                                (enhOverride ? kEnhNote : "");
        ImGui::SetItemTooltip("%s", tip.c_str());
    }
    ImGui::BeginDisabled(!r.efbColorDepth || enhOverride);
    ImGui::Indent();
    {
        bool shown = r.efbBlend6 && !enhOverride;
        if (recomp::menu::Checkbox("6-bit blending", &shown) && !enhOverride) {
            r.efbBlend6 = shown;
            sDirty = true;
        }
        const std::string tip = std::string(
            "Translucent things drawn over the scene (the Game Boy Horror screen, health counter and\n"
            "room captions, glows) are stored in the 6-bit framebuffer after blending, like on the\n"
            "console: the same banding and dither grain as the rest of the image at their edges and\n"
            "see-through parts instead of smooth 8-bit gradients. Subtle in this game.\n"
            "Costs a few full-screen passes per frame (well under 1 ms at 1440p; more with MSAA).") +
                                (enhOverride ? kEnhNote : "");
        ImGui::SetItemTooltip("%s", tip.c_str());
    }
    {
        // Enhancement on top of the 6-bit framebuffer (Aurora 0190); OFF = the console's look.
        // (Already disabled with the parent group while enhanced lighting turns the 6-bit EFB off.)
        const bool forced = recomp::gx::smoothBandingForced();
        bool on = (forced || r.smoothBanding) && !enhOverride;
        ImGui::BeginDisabled(forced);
        if (recomp::menu::Checkbox("Smooth color banding", &on) && !enhOverride) {
            r.smoothBanding = on;
            sDirty = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(
            "The console's 6-bit framebuffer and the 5-bit screen copy the game redraws every frame\n"
            "turn smooth gradients (dark rooms, the flashlight beam and its pool, glows) into visible\n"
            "bands and greenish/bluish blotches, partly hidden by dither grain. On: the same colors and\n"
            "brightness as the accurate picture, but each band is drawn as the smooth average of its\n"
            "steps (plus an invisible sub-step noise against the PC's own 8-bit steps): no contour\n"
            "lines, no blotches, no grain. Off: exactly what the GameCube outputs (default).\n"
            "No measurable cost.%s",
            enhOverride ? "\n\nNot used while Enhanced lighting is on: the 6-bit framebuffer is off then and\n"
                          "the picture is plain 8-bit, with nothing left to smooth." : "");
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
    sDirty |= recomp::menu::Checkbox("Early depth for cut-out textures", &r.earlyZAlphaDepth);
    ImGui::SetItemTooltip(
        "Room geometry and Luigi test depth before texturing. On the console that also writes the\n"
        "depth of pixels the alpha test then cuts out (transparent texels of cut-out textures), so\n"
        "things drawn behind them later are hidden there. Off: cut-out pixels write no depth (PC\n"
        "behaviour). Costs one extra depth-only draw for each such draw.");
    ImGui::SeparatorText("Screen copies");
    sDirty |= recomp::menu::Checkbox("Display copy filter", &r.displayCopyFilter);
    ImGui::SetItemTooltip(
        "The vertical filter (and gamma) the console applies when copying the frame to the TV\n"
        "image: slightly softer vertical detail, smooths the dither. One extra full-screen pass.");
    sDirty |= recomp::menu::Checkbox("Texture copy filter", &r.textureCopyFilter);
    ImGui::SetItemTooltip(
        "The console applies the same vertical filter to the game's screen copies (the full-screen\n"
        "copy it redraws every frame, the half-size and transition copies): the picture is a little\n"
        "softer vertically, as on the GameCube. Off: sharper. Tiny GPU cost.");
    sDirty |= recomp::menu::Checkbox("EFB copies to RAM (accurate)", &r.efbCopyToRam);
    ImGui::SetItemTooltip("Screen copies also exist in console memory format, like on the GameCube.\n"
                          "Textures that read a copy in another format (the shadow depth map is\n"
                          "read as an IA8 texture) get the console's exact bytes, and memory the game\n"
                          "overwrites stops showing a stale copy. Aimed at the rare Luigi mouth/hose flicker.\n"
                          "Off: copies stay GPU-only textures (saves ~0.6 ms CPU per frame).");
    ImGui::SeparatorText("Timing");
    sDirty |= recomp::menu::Checkbox("Snapshot vertex data at draw time", &r.vtxSnapshot);
    ImGui::SetItemTooltip(
        "The vertex data each draw uses is copied when the game issues the draw, as the\n"
        "GameCube's GPU reads it, instead of when the renderer thread gets to it. Without it,\n"
        "a draw can pick up data the game already rewrote for a later draw: Luigi's mouth or\n"
        "the Poltergust hose flicker now and then. Off: saves the copies (about 0.2-0.3 ms CPU per frame).\n"
        "Above the game's own frame rate (interpolation) vertex data is always snapshotted.");
    {
        const bool avail = recomp::pacing::keepWarmAvailable();
        bool shown = avail && r.gpuKeepWarm;
        ImGui::BeginDisabled(!avail);
        if (recomp::menu::Checkbox("Keep GPU clocks up in menus", avail ? &r.gpuKeepWarm : &shown)) sDirty = true;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(
            "When a frame is much lighter than play (a menu over a still picture), the GPU drops to its\n"
            "power-saving clocks within a second; the first frames of the full scene after the menu then\n"
            "take several times longer (a stutter when closing the pause screen or the Game Boy Horror,\n"
            "measured 45 ms instead of 6 at 4K with enhanced lighting). On: filler GPU work keeps the GPU\n"
            "about as busy as in play while the frame is light. Costs GPU power in menus, never frame time.\n"
            "Only active when recent frames were heavy (more than 3 ms of GPU work).%s",
            avail ? "" : "\n\nNot available on Linux: with NVIDIA's driver it made the game freeze.");
    }
    ImGui::SeparatorText("Shadows (flashlight and lightning)");
    sDirty |= recomp::menu::Checkbox("Shadow map at render resolution", &r.shadowRenderResolution);
    ImGui::SetItemTooltip(
        "The game draws its shadows from a depth map rendered from the light. On: that map uses the\n"
        "internal render resolution, so shadow edges stay sharp at high resolutions.\n"
        "Off: the console's 640x480 map (blockier, stair-stepped edges at high resolution).\n"
        "Without 'EFB copies to RAM' the map always follows the render resolution. Tiny GPU cost.");
    sDirty |= recomp::menu::Checkbox("Widescreen shadows", &r.shadowWidescreen);
    ImGui::SetItemTooltip(
        "The game only renders shadows for what a 4:3 screen shows. On: the light's view is widened\n"
        "with the screen, so lightning and flashlight shadows reach the left/right edges at 16:9 and\n"
        "wider (slightly lower horizontal shadow detail). Off: objects near the edges cast no\n"
        "shadows, like widescreen hacks on the original. No cost. Only matters in widescreen.");
    sDirty |= recomp::menu::Checkbox("Shadows at any distance", &r.shadowAnyDistance);
    ImGui::SetItemTooltip(
        "The original game only draws shadows within a limited distance band around Luigi: in big\n"
        "rooms, shadows of things farther away (or close to the camera) drop out as he walks.\n"
        "On: the shadow light's range covers everything in view, so all shadows always render.\n"
        "Off: the original behaviour. No cost.");
}

// Appended to the tooltip of an option the quality preset sets (greyed out unless Custom).
const char* presetNote(bool custom) {
    return custom ? "" : "\n\nSet by the quality preset: choose \"Custom\" to change it.";
}

// Enhanced lighting for City Folk (src/game/cf_light.cpp, src/recomp/enhanced_lighting.cpp;
// docs/RENDERING.md "Enhanced lighting (City Folk)"): a master switch (off by default), a quality
// preset, and every part with its own switch, grouped into Sun & moon / Time of day / Night lights
// / Ambient occlusion / Bloom & sun shafts.
void enhancedLightingTab() {
    auto& l = port::settings().enhancedLighting;
    ImGui::TextDisabled("Optional effects on top of the accurate rendering. Off by default: the original");
    ImGui::TextDisabled("look is one click away. Each part can be switched and tuned on its own.");
    ImGui::Separator();
    sDirty |= recomp::menu::Checkbox("Enhanced lighting", &l.enabled);
    ImGui::SetItemTooltip(
        "Master switch for everything on this page. Off: the game renders exactly as the accurate\n"
        "renderer does (no extra cost). Turning it on builds extra shader variants once (brief hitch).");
    if (!l.enabled) pending("turn on to use the options below");
    ImGui::BeginDisabled(!l.enabled);
    static const char* kPresets[] = {"Low", "Medium", "High", "Ultra", "Custom"};
    sDirty |= combo("Quality preset", l.preset, kPresets, 5);
    ImGui::SetItemTooltip(
        "Sets the shadow-map size and filter, how many night lights cast shadows and the sun-shaft\n"
        "steps. Low suits weaker GPUs; Ultra is for high-end cards at high resolutions. Custom keeps\n"
        "the values chosen below.");
    const bool custom = l.preset == port::EnhancedPreset::Custom;
    sDirty |= recomp::menu::Checkbox("Per-pixel lighting", &l.perPixelLighting);
    ImGui::SetItemTooltip("The game's own lights are computed for every pixel instead of every vertex, with\n"
                          "the same formulas: smoother shading on the ground, houses and characters. Small GPU cost.");

    // ---- Sun & moon ----
    ImGui::SeparatorText("Sun & moon");
    sDirty |= recomp::menu::Checkbox("Real sun and moon shadows", &l.sunShadows);
    ImGui::SetItemTooltip(
        "Characters, trees, houses, furniture and fences cast soft shadows from the sun by day and the\n"
        "moon by night. The light follows the game clock (east in the morning, south at noon, west in\n"
        "the evening; long shadows at sunrise and sunset), weaker and softer under clouds, almost gone\n"
        "in rain and snow. Cost: one extra render of the scene from the light per frame (four cascades),\n"
        "about 0.5-1.5 ms at 1080p (see docs for measurements).");
    ImGui::BeginDisabled(!l.sunShadows);
    ImGui::Indent();
    static const char* kDir[] = {"Time of day (sun path)", "The game's own key light"};
    sDirty |= ImGui::Combo("Light direction", &l.sunDirection, kDir, 2);
    ImGui::SetItemTooltip("Time of day: a real sun path from the game clock (and hemisphere setting).\n"
                          "The game's own key light: the direction the game lights its models from (mostly from\n"
                          "behind the camera, so shadows fall away from you and are mostly hidden).");
    sDirty |= recomp::menu::SliderFloat("Shadow strength", &l.sunShadowStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How much a full shadow darkens (tinted with the sky's colour). The moon's shadows are weaker.");
    sDirty |= recomp::menu::SliderFloat("Shadow softness", &l.sunShadowSoftness, 0.0f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Size of the sun / moon as a light source: sharp where an object touches the ground,\n"
                          "softer farther away. 0: a fixed small blur.");
    sDirty |= recomp::menu::SliderFloat("Shadow distance", &l.sunShadowDistance, 1000.0f, 8000.0f, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How far from the camera shadows are drawn (an acre is 640 units). Farther spreads\n"
                          "the same shadow-map texels over more ground (blurrier). No cost difference.");
    static const char* kSize[] = {"512", "1024", "2048"};
    static const char* kQ[] = {"Low (8 taps)", "Medium (12 taps)", "High (20 taps)"};
    ImGui::BeginDisabled(!custom);
    sDirty |= ImGui::Combo("Shadow map size", &l.sunShadowMapSize, kSize, 3);
    ImGui::SetItemTooltip("Texels per cascade (four cascades; night-light shadows use the same size per cube face).\n"
                          "Larger: sharper shadows, more GPU memory and time.%s", presetNote(custom));
    sDirty |= ImGui::Combo("Shadow filter", &l.sunShadowQuality, kQ, 3);
    ImGui::SetItemTooltip("Filter taps of the soft edge. More: smoother edges, more GPU time.%s", presetNote(custom));
    ImGui::EndDisabled();
    static const char* kBlob[] = {"Keep", "Fade under real shadows", "Hide"};
    sDirty |= ImGui::Combo("Round blob shadows", &l.blobShadows, kBlob, 3);
    ImGui::SetItemTooltip("The game's own round shadows under characters and objects: kept as they are, faded\n"
                          "where a real shadow already falls, or hidden.");
    sDirty |= recomp::menu::Checkbox("Indoor shadows", &l.indoorShadows);
    ImGui::SetItemTooltip("Inside houses and shops: shadows from the room's main light.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    sDirty |= recomp::menu::Checkbox("Sunlight", &l.sunLight);
    ImGui::SetItemTooltip("Warm sunlight on the sides facing the sun (golden near sunrise and sunset, faint cool\n"
                          "moonlight at night), stopped by the shadows. Off: shadows only.");
    ImGui::BeginDisabled(!l.sunLight);
    ImGui::Indent();
    sDirty |= recomp::menu::SliderFloat("Sunlight intensity", &l.sunLightIntensity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Unindent();
    ImGui::EndDisabled();

    // ---- Time of day ----
    ImGui::SeparatorText("Time of day");
    sDirty |= recomp::menu::Checkbox("Time-of-day lighting", &l.timeOfDay);
    ImGui::SetItemTooltip(
        "Golden light at sunrise and sunset, a blue dusk, cool moonlight at night, blended smoothly\n"
        "with the game clock, on top of the game's own hourly colours (the HUD is never tinted).\n"
        "One full-screen pass, ~0.1-0.3 ms.");
    ImGui::BeginDisabled(!l.timeOfDay);
    ImGui::Indent();
    sDirty |= recomp::menu::SliderFloat("Strength##tod", &l.timeOfDayStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Unindent();
    ImGui::EndDisabled();
    sDirty |= recomp::menu::Checkbox("Follow the weather", &l.timeOfDayWeather);
    ImGui::SetItemTooltip("Overcast skies flatten the light (weaker, softer shadows, less colour); rain and snow\n"
                          "almost remove the sun's shadows.");

    // ---- Night lights ----
    ImGui::SeparatorText("Night lights");
    sDirty |= recomp::menu::Checkbox("Night lights that light", &l.nightLights);
    ImGui::SetItemTooltip("Lit house windows, lamps and shop windows cast warm light onto the ground around them at\n"
                          "night; fireflies and fireworks are small coloured lights. Cost grows with the lights in view.");
    ImGui::BeginDisabled(!l.nightLights);
    ImGui::Indent();
    sDirty |= recomp::menu::SliderFloat("Night light intensity", &l.nightLightIntensity, 0.0f, 3.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    sDirty |= recomp::menu::Checkbox("Night light shadows", &l.nightLightShadows);
    ImGui::SetItemTooltip("The strongest night lights cast shadows (cube shadow maps; the preset sets how many).");
    sDirty |= recomp::menu::Checkbox("Follow the game's lights", &l.nightLightsGameState);
    ImGui::SetItemTooltip("A window only lights its surroundings while the game itself shows it lit: villagers at\n"
                          "home and awake, shops while open, your house while someone is in. Off: every building\n"
                          "glows from dusk to dawn.");
    sDirty |= recomp::menu::Checkbox("Fireflies and fireworks", &l.effectLights);
    ImGui::SetItemTooltip("Fireflies (summer nights) and the fireworks show are small coloured lights at the game's\n"
                          "own particles, in their colours. Shares the light budget with the other night lights.");
    sDirty |= recomp::menu::SliderFloat("Light color vibrance", &l.extraLightSaturation, 0.0f, 2.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Unindent();
    ImGui::EndDisabled();

    // ---- Ambient occlusion ----
    ImGui::SeparatorText("Ambient occlusion");
    sDirty |= ImGui::Checkbox("Ambient occlusion", &l.ssao);
    ImGui::SetItemTooltip(
        "Soft contact shading under trees, furniture, fences and characters (horizon-based, from the\n"
        "depth buffer and the models' own normals). Only solid surfaces: water, glass, particles and\n"
        "the HUD keep their brightness. About 0.5-1 ms at 4K.");
    ImGui::BeginDisabled(!l.ssao);
    ImGui::Indent();
    sDirty |= recomp::menu::SliderFloat("Occlusion intensity", &l.ssaoIntensity, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    sDirty |= recomp::menu::SliderFloat("Occlusion radius", &l.ssaoRadius, 10.0f, 200.0f, "%.0f units", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How far nearby surfaces darken a point (the player is about 40 units tall).");
    ImGui::Unindent();
    ImGui::EndDisabled();

    // ---- Bloom & sun shafts ----
    ImGui::SeparatorText("Bloom & sun shafts");
    sDirty |= recomp::menu::Checkbox("Bloom / glow", &l.bloom);
    ImGui::SetItemTooltip("Bright skies, lamps, the aurora and light effects glow. The HUD never glows.\n"
                          "Computed at a capped resolution.");
    ImGui::BeginDisabled(!l.bloom);
    ImGui::Indent();
    sDirty |= recomp::menu::SliderFloat("Glow intensity", &l.bloomIntensity, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    sDirty |= recomp::menu::SliderFloat("Light sources", &l.bloomEmissive, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Glow of what the game draws as added light (lamps, sparkles, fireworks, the aurora).");
    sDirty |= recomp::menu::SliderFloat("Bright surfaces", &l.bloomScene, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    sDirty |= recomp::menu::SliderFloat("Glow threshold", &l.bloomThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    sDirty |= recomp::menu::SliderFloat("Glow radius", &l.bloomRadius, 0.25f, 2.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!l.sunShadows);
    sDirty |= recomp::menu::Checkbox("Sun shafts", &l.sunShafts);
    ImGui::SetItemTooltip("Sunlight scattered in the air, cut into shafts by the shadows of trees and houses\n"
                          "(strongest at sunrise and sunset, gone in rain). Ray-marched against the sun's shadow map\n"
                          "at mask resolution.%s", l.sunShadows ? "" : "\n\nNeeds \"Real sun and moon shadows\".");
    ImGui::BeginDisabled(!l.sunShafts);
    ImGui::Indent();
    sDirty |= recomp::menu::SliderFloat("Shaft intensity", &l.sunShaftIntensity, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    static const char* kShaftQ[] = {"Low (12 steps)", "Medium (20 steps)", "High (32 steps)"};
    ImGui::BeginDisabled(!custom);
    sDirty |= ImGui::Combo("Shaft quality", &l.sunShaftQuality, kShaftQ, 3);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ray-march steps of the shafts.%s", presetNote(custom));
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

// Frame rate: 30 (the game's own rate, drawn directly) up to 320, or the display's refresh rate.
void frameRateControls(port::VideoSettings& v) {
    const float hz = recomp::frame::displayRefreshHz();
    const int target = recomp::frame::targetFps();
    char label[64];
    // The floor is the game's own rate (City Folk 60): "Original", drawn directly.
    const int nativeFps = std::max(port::kMinFps, recomp::game::nativeFps());
    if (v.targetFps < nativeFps) v.targetFps = nativeFps;
    if (v.matchDisplay) std::snprintf(label, sizeof(label), "%d fps (display)", target);
    else if (v.targetFps <= nativeFps) std::snprintf(label, sizeof(label), "Original (%d)", nativeFps);
    else std::snprintf(label, sizeof(label), "%d fps", v.targetFps);
    if (v.unlimitedFps) std::snprintf(label, sizeof(label), "Unlimited");
    ImGui::BeginDisabled(v.matchDisplay || v.unlimitedFps);
    sDirty |= recomp::menu::SliderInt("Frame rate", &v.targetFps, nativeFps, port::kMaxFps, label, ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    const bool sliderHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::BeginDisabled(v.unlimitedFps);
    sDirty |= recomp::menu::Checkbox("Match display refresh rate", &v.matchDisplay);
    ImGui::EndDisabled();
    const bool checkHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    sDirty |= recomp::menu::Checkbox("Unlimited frame rate", &v.unlimitedFps);
    ImGui::SetItemTooltip("Interpolated frames as fast as the PC can draw them: no frame-rate target, no frame\n"
                          "pacing and no cap below the display's refresh rate (the game itself still runs at its own rate).\n"
                          "With VSync on, the display's refresh rate still limits it (with G-SYNC the frame\n"
                          "rate can then sit right at the refresh rate); turn VSync off for a truly unlimited\n"
                          "rate. Uneven frame times can look less smooth than a paced target.");
    const bool vsync = recomp::pacing::vsyncOn();
    // VSync at a fixed refresh rate shows a target that does not divide the refresh rate at the even
    // fraction below it (60 fps at 90 Hz: 45), never an uneven rate (pacing.cpp).
    const float fit = v.unlimitedFps ? 0.0f : recomp::pacing::fixedRefreshFps(target);
    const bool reduced = fit > 0 && float(target) <= hz + 0.5f && fit < float(target) - 0.5f;
    if (v.matchDisplay && hz > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f Hz)", hz);
    } else if (vsync && hz > 0 && float(target) > hz + 0.5f) {
        ImGui::SameLine();
        ImGui::TextDisabled("(VSync limits this to %.0f fps)", hz);
    } else if (reduced) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "(shown at %.0f fps on this %.0f Hz screen)", fit, hz);
    }
    if (sliderHovered || checkHovered) {
        char vs[512] = "";
        if (vsync && hz > 0 && float(target) > hz + 0.5f)
            std::snprintf(vs, sizeof(vs),
                          "\n\nVSync is on: frames are limited to the display's %.0f Hz, so %d fps\n"
                          "is not reached. Turn VSync off (next launch) to go above it (with tearing).",
                          hz, target);
        else if (reduced && recomp::pacing::gamescope())
            std::snprintf(vs, sizeof(vs),
                          "\n\nThe screen refreshes at %.0f Hz and %d fps does not divide it evenly, so frames are\n"
                          "shown at %.0f fps (an uneven rate would stutter). For %d fps, set the screen's refresh\n"
                          "rate to %d Hz in the Steam Quick Access menu (Performance > Refresh rate), or use\n"
                          "\"Match display refresh rate\".",
                          hz, target, fit, target, target);
        else if (reduced)
            std::snprintf(vs, sizeof(vs),
                          "\n\nVSync is on and the display refreshes at %.0f Hz: %d fps does not divide it evenly,\n"
                          "so frames are shown at %.0f fps (an uneven rate would stutter). Pick a rate that\n"
                          "divides the refresh rate, set the display to a matching refresh rate, or turn\n"
                          "VSync off (next launch).",
                          hz, target, fit);
        const int native = std::max(port::kMinFps, recomp::game::nativeFps());
        ImGui::SetTooltip("Original (%d fps) is the game's own rate, drawn exactly as the console does.\n"
                          "Above it, frames in between are interpolated (the game still runs at %d):\n"
                          "movement, the camera, lights and effects are blended between game frames,\n"
                          "shown one game frame (%.0f ms) behind. Cuts and teleports are not blended.\n"
                          "Match display refresh rate: the refresh rate of the monitor the window is on\n"
                          "(currently %.0f Hz).%s",
                          native, native, 1000.0 / native, hz, vs);
    }
    static const char* kVrr[] = {"Auto (detect)", "On", "Off"};
    sDirty |= combo("Variable refresh rate", v.vrr, kVrr, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For G-SYNC / FreeSync displays. Interpolated frames are started on an even schedule\n"
                          "and each one shows the game exactly at its scheduled time, so motion steps are equal.\n"
                          "Variable refresh: up to a little below the refresh rate (inside the VRR range); if a\n"
                          "frame takes longer, the rate drops evenly to what the PC can hold (e.g. 95 fps).\n"
                          "Fixed refresh: the refresh rate or an even fraction of it (120 Hz: 120, 60, 40 fps),\n"
                          "never an uneven rate that would show frames for one or two refreshes.\n"
                          "Auto: variable refresh is detected from the display's refresh timing%s.",
                          recomp::pacing::vrrActive() ? " (detected)" : " (not detected so far)");
    if (const char* st = recomp::pacing::status(); st && *st) {
        ImGui::SameLine();
        ImGui::TextDisabled("now %s", st);
    }
}

// Test aid CF_MENU_SIZE_TEST=1 (with CF_MENU=Visuals): drives ImGui's mouse on the Menu size row
// like a player: clicks "+" until 300%, "-" twice, then drags the slider from the left end to the
// right end. Logs "[ui-test]" lines: after each click whether the mouse is still on the button it
// clicked (the menu is rescaled about the mouse), during the drag that the size in effect does not
// change, after the release the new size. Ends with "[ui-test] done".
// (The SDL backend feeds the real cursor every frame, so the test's mouse position is applied at
// the start of the menu window as in the ID check, and the button state set directly.)
ImVec2 sSizeTestMouse(-1.0f, -1.0f);
void menuSizeTestMouse() {
    if (sSizeTestMouse.x < 0.0f) return;
    ImGuiContext& g = *GImGui;
    g.IO.MousePos = sSizeTestMouse;
    ImGuiWindow* hovered = nullptr;
    ImGuiWindow* under = nullptr;
    ImGui::FindHoveredWindowEx(sSizeTestMouse, false, &hovered, &under);
    g.HoveredWindow = hovered;
    g.HoveredWindowUnderMovingWindow = under;
}
void menuSizeTest(ImVec2 sliderMin, ImVec2 sliderMax) {
    static const bool sOn = std::getenv("CF_MENU_SIZE_TEST") != nullptr;
    static int sFrame = 0, sStep = 0, sClicks = 0;
    static ImVec2 sTarget;
    if (!sOn) return;
    if (sStep < 0) {
        sSizeTestMouse = ImVec2(-1.0f, -1.0f);
        return;
    }
    struct Io {
        ImGuiIO& io;
        void AddMousePosEvent(float x, float y) { sSizeTestMouse = ImVec2(x, y); }
        void AddMouseButtonEvent(int b, bool down) { io.MouseDown[b] = down; }
        ImVec2& MousePos = io.MousePos;
    } io{ImGui::GetIO()};
    const float h = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const ImVec2 plus(sliderMin.x - gap - h * 0.5f, (sliderMin.y + sliderMax.y) * 0.5f);
    const ImVec2 minus(plus.x - gap - h, plus.y);
    const auto inside = [&](ImVec2 c) {
        return std::fabs(io.MousePos.x - c.x) <= h * 0.5f && std::fabs(io.MousePos.y - c.y) <= h * 0.5f;
    };
    const int f = ++sFrame;
    if (sStep == 0) {  // settle
        if (f >= 90) sStep = 1, sFrame = 0;
        return;
    }
    if (sStep == 1) {  // one click: move (0), press (3), release (5), look (25)
        static int sMinus = -1;  // "-" clicks so far (-1: still going up)
        if (f == 1 && sMinus < 0 && (ui::percent() >= 300 || sClicks >= 10)) sMinus = 0;
        if (f == 1) sTarget = sMinus >= 0 ? minus : plus;
        if (f == 1) io.AddMousePosEvent(sTarget.x, sTarget.y);
        if (f == 3) io.AddMouseButtonEvent(0, true);
        if (f == 5) io.AddMouseButtonEvent(0, false);
        if (f == 25) {
            const bool onPlus = inside(plus), onMinus = inside(minus);
            std::printf("[ui-test] click %d: size %d%%, mouse %.0f,%.0f on %s\n", sClicks + 1, ui::percent(), io.MousePos.x,
                        io.MousePos.y, onPlus ? "+" : onMinus ? "-" : "neither button");
            std::fflush(stdout);
            sFrame = 0;
            sClicks++;
            if (sMinus >= 0 && ++sMinus >= 2) sStep = 2;
        }
        return;
    }
    if (sStep == 2) {  // drag from the left end to the right end
        static int sBefore = 0;
        if (f == 1) {
            sBefore = ui::percent();
            io.AddMousePosEvent(sliderMin.x + 2.0f, (sliderMin.y + sliderMax.y) * 0.5f);
        }
        if (f == 3) io.AddMouseButtonEvent(0, true);
        if (f > 4 && f <= 24)
            io.AddMousePosEvent(sliderMin.x + (sliderMax.x - sliderMin.x - 2.0f) * float(f - 4) / 20.0f,
                                (sliderMin.y + sliderMax.y) * 0.5f);
        if (f == 14 || f == 26) {
            std::printf("[ui-test] dragging: size in effect %d%% (before %d%%), %s\n", ui::percent(), sBefore,
                        ui::percent() == sBefore ? "unchanged" : "CHANGED DURING THE DRAG");
            std::fflush(stdout);
        }
        if (f == 27) io.AddMouseButtonEvent(0, false);
        if (f == 45) {
            std::printf("[ui-test] released: size %d%%\n[ui-test] done\n", ui::percent());
            std::fflush(stdout);
            sStep = -1;
        }
    }
}

// Menu size (issue #20; ui_scale.cpp): Auto or a step. Dragging the slider only previews the
// value in its label; the size changes when the slider is released (or at once with - / +), and
// the menu is rescaled about the mouse so the control stays under it.
void menuSizeControl() {
    namespace ui = recomp::ui;
    int& setting = port::settings().ui.menuScale;
    int cur = 0;  // slider index: 0 Auto, else 1 + the nearest step
    if (setting > 0) {
        cur = 1;
        for (int i = 1; i < ui::kStepCount; i++)
            if (std::abs(ui::kSteps[i] - setting) < std::abs(ui::kSteps[cur - 1] - setting)) cur = i + 1;
    }
    static int sDrag = -1;  // index shown while the slider is held
    int idx = sDrag >= 0 ? sDrag : cur;
    char label[64];
    if (idx == 0) std::snprintf(label, sizeof(label), "Auto (%d%%%%)", ui::autoPercent());
    else std::snprintf(label, sizeof(label), "%d%%%%", ui::kSteps[idx - 1]);
    const bool changed = recomp::menu::SliderInt("Menu size", &idx, 0, ui::kStepCount, label,
                                                 ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
    const bool active = ImGui::IsItemActive();
    const ImVec2 itemCenter((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                            (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
    bool commit = false;
    if (active) {
        if (changed || sDrag < 0) sDrag = idx;
    } else if (sDrag >= 0) {
        idx = sDrag;  // released
        sDrag = -1;
        commit = true;
    } else if (changed) {
        commit = true;  // - / + (or a click without a drag)
    }
    ImGui::SetItemTooltip("Size of this menu, its text and the game's other overlays (frame-rate counter,\n"
                          "notices, shader preparation, debug windows). Auto picks it from the window\n"
                          "height and the display's scaling setting: 100%% at 1080p, 200%% on a 4K screen,\n"
                          "150%% on a Steam Deck. Text is drawn at the chosen size, so it stays sharp.\n"
                          "Applies when the slider is released (or at once with - and +). A size too large\n"
                          "for the window is limited to the largest that fits.");
    if (commit) {
        const int value = idx == 0 ? 0 : ui::kSteps[idx - 1];
        if (value != setting) {
            setting = value;
            sDirty = true;
            const ImGuiIO& io = ImGui::GetIO();
            const bool mouse = ImGui::IsMousePosValid() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            recomp::ui::setAnchor(kMenuWindow, mouse ? io.MousePos : itemCenter);
            if (value == 0) std::printf("[ui] menu size set to Auto\n");
            else std::printf("[ui] menu size set to %d%%\n", value);
            std::fflush(stdout);
        }
    }
    menuSizeTest(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (cur > 0 && ui::percent() < ui::kSteps[cur - 1]) {
        char note[96];
        std::snprintf(note, sizeof(note), "%d%% is the largest that fits this window", ui::percent());
        pending(note);
    }
}

void visualsTab() {
    auto& v = port::settings().video;
    ImGui::SeparatorText("Menu");
    menuSizeControl();
    sDirty |= recomp::menu::Checkbox("Show \"Press F1\" hint at start", &port::settings().ui.startHint);
    ImGui::SetItemTooltip("A small notice in the top-left corner for the first seconds of play saying that F1\n"
                          "opens this menu. It is not shown again in a session once the menu has been opened.");
    ImGui::SeparatorText("Frame rate and view");
    frameRateControls(v);
    static const char* kAspects[] = {"Original 4:3", "Match window", "16:9", "16:10", "21:9", "32:9"};
    sDirty |= combo("Aspect ratio", v.aspect, kAspects, 6);
    ImGui::SetItemTooltip("Shape of the 3D view. Original 4:3: as on the console, with black bars in a wider\n"
                          "window. Match window: fills the window. The fixed ratios add black bars as needed.");
    sDirty |= recomp::menu::Checkbox("Keep HUD at 16:9###hudKeep", &v.hudKeep4x3);
    ImGui::SetItemTooltip("On: in a window wider than 16:9 the HUD stays where it is on a 16:9 TV, centred.\n"
                          "Off: the clock moves to the left edge and the Save / camera buttons to the right\n"
                          "edge, where they are clicked, and the pointer reaches the whole window. The\n"
                          "toolbar, dialogue and menus stay centred.");
    sDirty |= recomp::menu::SliderFloat("Field of view", &v.fovScale, 0.75f, 1.5f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Scales the camera's field of view (1 = original). Higher shows more of the room.");

    ImGui::SeparatorText("Resolution and display");
    {
        int& rs = v.resolutionScale;
        char label[64];
        if (rs <= 0) {
            std::snprintf(label, sizeof(label), "Match window");
        } else {
            const AuroraWindowSize ws = aurora_get_window_size();
            const float aspect = ws.native_fb_height ? float(ws.native_fb_width) / float(ws.native_fb_height) : 4.0f / 3.0f;
            const int h = 480 * rs;
            const int w = int(float(h) * std::max(aspect, 4.0f / 3.0f) + 0.5f);
            std::snprintf(label, sizeof(label), "%dx  (%d x %d)", rs, w, h);
        }
        if (recomp::menu::SliderInt("Render resolution", &rs, 0, 8, label, ImGuiSliderFlags_AlwaysClamp)) {
            sDirty = true;
            applyResolution();
        }
        ImGui::SetItemTooltip("Internal resolution as a multiple of the console's 480 lines (1x = 640x480).\n"
                              "Match window: the window's own resolution. Higher is sharper and costs GPU time.");
    }
    if (recomp::menu::Checkbox("Fullscreen", &v.fullscreen)) {
        sDirty = true;
        applyWindowSettings();
    }
    ImGui::SetItemTooltip("The game window fills the whole screen.");
    sDirty |= recomp::menu::Checkbox("VSync", &v.vsync);
    ImGui::SetItemTooltip("Frames wait for the display's refresh: no tearing, but the frame rate is limited to\n"
                          "the refresh rate. Applies on the next launch.%s",
                          recomp::pacing::gamescope()
                              ? "\n\nUnder Gamescope (Steam Deck game mode) VSync is always on: Gamescope composites\n"
                                "every frame, so there is no tearing to avoid."
                              : "");
    pending("next launch");
    {
        // Only the sample counts the graphics device supports (Aurora 0233: Vulkan/Metal 1 and 4,
        // D3D12 also 2 and 8, compatibility-mode devices none; issue #14). A saved count the device
        // lacks shows the one in use, with a note.
        static const char* kNames[] = {"Off", "2x", "4x", "8x"};
        const uint32_t mask = aurora_get_msaa_support() | 1u;
        const char* items[4];
        int counts[4];
        int n = 0, sel = 0;
        for (int i = 0; i < 4; i++) {
            if (!(mask & (1u << i))) continue;
            if ((1 << i) <= v.msaa) sel = n;
            items[n] = kNames[i];
            counts[n++] = 1 << i;
        }
        if (ImGui::Combo("Anti-aliasing", &sel, items, n)) {
            v.msaa = counts[sel];
            sDirty = true;
        }
        ImGui::SetItemTooltip("Multisample anti-aliasing: smoother polygon edges. Higher settings cost more GPU time\n"
                              "and memory. Applies on the next launch. Only the levels your graphics card supports\n"
                              "with the current graphics API are offered (Vulkan: 4x; Direct3D 12: 2x, 4x, 8x).");
        const int inUse = int(aurora_get_msaa());
        char note[128];
        if (counts[sel] != v.msaa) {
            std::snprintf(note, sizeof(note), "%dx not supported by this graphics device; %s in use", v.msaa,
                          inUse > 1 ? kNames[inUse >= 8 ? 3 : inUse >= 4 ? 2 : 1] : "off");
            pending(note);
        } else {
            pending("next launch");
        }
    }
    // (The European game's 50 / 60 Hz is the console's EuRGB60 setting, F1 > Game > Console: the
    // VI rate follows the TV mode the game configures, hle_vi.cpp.)

    ImGui::SeparatorText("CRT filter");
    sDirty |= recomp::crt::drawMenu();
}

// Volume slider shown in percent (the setting is 0..1).
bool volumeSlider(const char* label, float& v) {
    float pct = v * 100.0f;
    if (!recomp::menu::SliderFloat(label, &pct, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) return false;
    v = pct / 100.0f;
    return true;
}

void soundTab() {
    auto& a = port::settings().audio;
    ImGui::SeparatorText("Volume");
    sDirty |= volumeSlider("Master volume", a.masterVolume);
    ImGui::SetItemTooltip("Everything the game plays.");
    sDirty |= volumeSlider("Music volume", a.musicVolume);
    ImGui::SetItemTooltip("Music: the background music, K.K. Slider's songs and records, jingles and the title\n"
                          "theme (the game's music players).");
    sDirty |= volumeSlider("Sound effects volume", a.sfxVolume);
    ImGui::SetItemTooltip("Everything else: footsteps, tools, menus, furniture, ambience, insects, the town tune\n"
                          "chimes, gyroids and the TV.");
    sDirty |= volumeSlider("Voices volume", a.voiceVolume);
    ImGui::SetItemTooltip("The villagers' and Rover's speech (Animalese), also while typing.");
    sDirty |= volumeSlider("Wii Remote speaker volume", a.remoteVolume);
    ImGui::SetItemTooltip("What the game plays on the Wii Remote's own speaker, played through the PC's speakers\n"
                          "(in the centre; on the centre speaker with 5.1 / 7.1). Band-limited to 3 kHz like the\n"
                          "Remote's 6 kHz speaker stream, with a little bass cut for the small speaker. 0%: off.\n"
                          "(The HOME Menu's Wii Remote speaker volume is already part of what it sends.)");
    ImGui::SeparatorText("Output");
    static const char* kOutputs[] = {"Stereo", "Mono", "Surround (matrix-encoded stereo)", "Surround 5.1 (discrete)",
                                     "Surround 7.1 (discrete)"};
    sDirty |= combo("Output", a.output, kOutputs, 5);
    ImGui::SetItemTooltip(
            "The console's sound setting (Wii Settings), read by the game at start: changes apply\n"
            "on the next start.\n"
            "Stereo / Mono: the game's stereo or mono mix.\n"
            "Surround: the console set to surround. City Folk itself then mixes in stereo, as on\n"
            "a Wii, so this sounds like Stereo.\n"
            "5.1 / 7.1: the game keeps mixing in Dolby Pro Logic II and its surround channels go\n"
            "to the surround speakers (the game pans nothing to the rear itself: they carry its\n"
            "reverb returns); the Wii Remote speaker sound goes to the centre speaker. Needs a\n"
            "device with 6 / 8 channels (else falls back to 5.1 or stereo).");
    {
        const int dev = recomp::audio::outDeviceChannels();
        if (dev > 0) ImGui::TextDisabled("Device: %d channels. Playing: %s", dev, recomp::audio::outStatus());
        else ImGui::TextDisabled("Playing: %s", recomp::audio::outStatus());
    }
    sDirty |= recomp::menu::Checkbox("Swap left/right", &a.swapChannels);
    ImGui::SetItemTooltip("Exchanges the left and right channels (e.g. for headphones worn the other way round).");
    sDirty |= recomp::menu::Checkbox("High-quality resampler", &a.sincResampler);
    ImGui::SetItemTooltip("Converts the console's 32028 Hz output to the device rate with a 64-tap\n"
                          "windowed-sinc filter (flat to 13.8 kHz, images above 16 kHz removed).\n"
                          "Off: SDL's resampler, slightly cheaper.");
    ImGui::SeparatorText("DSP accuracy");
    sDirty |= recomp::menu::Checkbox("Reverb / echo", &a.reverb);
    ImGui::SetItemTooltip("The effect returns of the DSP's aux buses (the game's reverb / chorus / delay,\n"
                          "which it still computes). Off: dry sound only.");
    sDirty |= recomp::menu::Checkbox("Voice filters", &a.voiceFilters);
    ImGui::SetItemTooltip("Per-voice low-pass and biquad filters the game asks the DSP for (muffled sounds,\n"
                          "also on the Wii Remote speaker path). Off: those sounds play unfiltered.");
}

void generalEnhancementsTab() {
    auto& e = port::settings().enhancements;
    ImGui::SeparatorText("Interface");
    sDirty |= recomp::menu::Checkbox("Show FPS", &e.showFps);
    ImGui::SetItemTooltip("Frames per second shown in the top-left corner.");
    if (recomp::menu::Checkbox("Pause when window loses focus", &e.pauseOnFocusLoss)) {
        sDirty = true;
        applyWindowSettings();
    }
    ImGui::SetItemTooltip("The game pauses (and goes quiet) while another window is in front.");
}

// Game features (docs/ROADMAP.md "Feature list", docs/FEATURES.md, docs/CALENDAR.md): every one
// off / original by default; the controls live with the features in src/game/.
void gameplayTab() {
    // While visiting another town its host's world settings are in use (online_sync.cpp): those
    // options are locked; the skin tone stays this player's own.
    bool lock = recomp::online::sync::lockBegin();
    recomp::cf::grassMenu(sDirty);
    recomp::online::sync::lockEnd(lock);
    recomp::cf::skinMenu(sDirty);
    ImGui::SeparatorText("Seasons and weather");
    lock = recomp::online::sync::lockBegin();
    sDirty |= recomp::calendar::drawHemisphereMenu();  // src/game/cf_calendar.cpp
    sDirty |= recomp::calendar::drawWeatherMenu();     // src/game/cf_weather.cpp
    // Content from other regions (src/game/cf_region.cpp).
    sDirty |= recomp::region::menuSection();
    recomp::online::sync::lockEnd(lock);
    // Move in from Wild World (src/game/cf_move_in.cpp, docs/MOVE_IN.md).
    sDirty |= recomp::movein::menuSection();
}

// Debug: the game's debug entries (src/game).
void debugTab() { recomp::game::debugMenu(); }

void enhancementsTab() {
    if (ImGui::BeginTabBar("enhancements")) {
        if (ImGui::BeginTabItem("General", nullptr, envTab("General"))) {
            generalEnhancementsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gameplay", nullptr, envTab("Gameplay"))) {
            gameplayTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Debug", nullptr, envTab("Debug"))) {
            debugTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void fpsOverlay() {
    using clock = std::chrono::steady_clock;
    static auto last = clock::now();
    static int frames = 0;
    static float fps = 0.0f;
    frames++;
    const auto now = clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    if (dt >= 0.5f) {
        fps = frames / dt;
        frames = 0;
        last = now;
    }
    ImGui::SetNextWindowPos(ImVec2(8 * recomp::ui::scale(), 8 * recomp::ui::scale()));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%.0f fps", fps);
    // Interpolated presentation: how it is paced, and why it is below the target if it is.
    if (const char* st = recomp::pacing::status(); st && *st) ImGui::TextDisabled("%s", st);
    ImGui::End();
}

}  // namespace

void init() {
    port::loadSettings();
    recomp::ui::install(recomp::ui::Context::Game);
    applyWindowSettings();
    // CF_MENU=<tab> (testing/screenshots): start with the menu open on that tab.
    if (const char* tab = std::getenv("CF_MENU"); tab && *tab) {
        sOpen = true;
        sOpenTab = tab;
    }
    if (idcheck::active()) sOpen = true;
}

void openTreesForCheck() {
    // (also while an CF_MENU_CLICK click is pending, so options inside tree nodes can be reached)
    if (idcheck::active() || click::due()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
}

bool isOpen() { return sOpen || recomp::movein::windowOpen(); }
void openPage(const char* tab) {
    sOpen = true;
    sOpenTab = tab;
}
bool quitRequested() { return sQuit; }
void requestQuit() { sQuit = true; }

void shutdown() {
    // Settings are written when the menu closes; closing the window (or Alt+F4) while it is
    // still open would otherwise drop every change made in this visit.
    if (sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

void handleEvent(const SDL_Event& ev) {
    if (recomp::controls::handleEvent(ev)) return;  // binding capture owns the key
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return;
    if (ev.key.key == SDLK_F1) sOpen = !sOpen;
    else if (ev.key.key == SDLK_ESCAPE && sOpen) sOpen = false;
    if (!sOpen && sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

// Test aid CF_SETTINGS_AT=<frame>:<section>.<key>=<value>[,...][;<frame>:...]: changes settings
// (ini names) live once the game reaches that frame (copyDispCount, the CF_FRAME_DUMP number),
// through the same path as the F1 menu (the settings struct, then the per-frame apply calls
// below). Not saved. E.g. CF_SETTINGS_AT="400:enhanced_lighting.shadow_darkness=0.2".
void scriptedSettings() {
    struct Change {
        u32 frame;
        std::string section, key, value;
    };
    static std::vector<Change> sChanges = [] {
        std::vector<Change> out;
        const char* v = std::getenv("CF_SETTINGS_AT");
        if (!v) return out;
        std::string s = v;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(';', pos);
            if (end == std::string::npos) end = s.size();
            const std::string group = s.substr(pos, end - pos);
            pos = end + 1;
            const size_t colon = group.find(':');
            if (colon == std::string::npos) continue;
            const u32 frame = u32(std::strtoul(group.c_str(), nullptr, 10));
            size_t p = colon + 1;
            while (p < group.size()) {
                size_t e = group.find(',', p);
                if (e == std::string::npos) e = group.size();
                const std::string item = group.substr(p, e - p);
                p = e + 1;
                const size_t dot = item.find('.'), eq = item.find('=');
                if (dot == std::string::npos || eq == std::string::npos || eq < dot) continue;
                out.push_back({frame, item.substr(0, dot), item.substr(dot + 1, eq - dot - 1), item.substr(eq + 1)});
            }
        }
        return out;
    }();
    if (sChanges.empty()) return;
    const u32 now = copyDispCount();
    for (auto it = sChanges.begin(); it != sChanges.end();) {
        if (now >= it->frame) {
            const bool ok = port::setSetting(it->section, it->key, it->value);
            std::printf("[settings] frame %u: %s.%s=%s%s\n", now, it->section.c_str(), it->key.c_str(),
                        it->value.c_str(), ok ? "" : " (unknown key)");
            std::fflush(stdout);
            if (ok && it->section == "video") applyWindowSettings();  // (resolution scale: applied like the F1 slider)
            it = sChanges.erase(it);
        } else {
            ++it;
        }
    }
}

bool Checkbox(const char* label, bool* v) {
    const bool changed = ImGui::Checkbox(label, v);
    click::afterItem(label);
    return changed;
}
bool Button(const char* label) {
    const bool pressed = ImGui::Button(label);
    click::afterItem(label);
    return pressed;
}
bool SmallButton(const char* label) {
    const bool pressed = ImGui::SmallButton(label);
    click::afterItem(label);
    return pressed;
}

namespace {
// Step of a slider's +/- buttons: one unit of the last digit its format shows.
float sliderStep(const char* format) {
    const char* p = format ? std::strchr(format, '%') : nullptr;
    while (p && p[1] == '%') p = std::strchr(p + 2, '%');  // skip literal "%%"
    if (!p) return 1.0f;
    const char* dot = std::strchr(p, '.');
    if (!dot) return 1.0f;
    const int digits = std::atoi(dot + 1);
    return std::pow(10.0f, -float(digits));
}

// "-" and "+" in front of the slider; returns -1, 0 or +1. The slider then takes the rest of the
// item width so the row keeps its usual size.
int stepButtons() {
    const float h = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    const float width = ImGui::CalcItemWidth();
    int dir = 0;
    if (ImGui::Button("-##step", ImVec2(h, h))) dir = -1;
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button("+##step", ImVec2(h, h))) dir = 1;
    ImGui::SameLine(0.0f, gap);
    ImGui::SetNextItemWidth(std::max(width - 2.0f * (h + gap), h));
    return dir;
}
}  // namespace

bool SliderFloat(const char* label, float* v, float min, float max, const char* format, int flags) {
    ImGui::PushID(label);
    const int dir = stepButtons();
    ImGui::PopID();
    bool changed = ImGui::SliderFloat(label, v, min, max, format, flags);
    if (dir != 0) {
        const float step = sliderStep(format);
        const float lo = std::min(min, max), hi = std::max(min, max);
        *v = std::clamp(std::round(*v / step + float(dir)) * step, lo, hi);
        changed = true;
    }
    return changed;
}

bool SliderInt(const char* label, int* v, int min, int max, const char* format, int flags) {
    ImGui::PushID(label);
    const int dir = stepButtons();
    ImGui::PopID();
    bool changed = ImGui::SliderInt(label, v, min, max, format, flags);
    if (dir != 0) {
        *v = std::clamp(*v + dir, std::min(min, max), std::max(min, max));
        changed = true;
    }
    return changed;
}

void draw() {
    click::frame();
    recomp::wiimote::hostFrame();  // mouse cursor over the game (pointer from the mouse)
    scriptedSettings();
    recomp::texpacks::hostFrame();
    recomp::gx::applyAccuracySettings();
    recomp::enh::applySettings();
    recomp::crt::update();
    if (port::settings().enhancements.showFps) fpsOverlay();
    // CF_FPS_LOG=1: presented host frames per second every 2 s (performance measurements).
    static const bool sFpsLog = std::getenv("CF_FPS_LOG") != nullptr;
    if (sFpsLog) {
        using clock = std::chrono::steady_clock;
        static auto last = clock::now();
        static int frames = 0;
        frames++;
        const float dt = std::chrono::duration<float>(clock::now() - last).count();
        if (dt >= 2.0f) {
            std::printf("[fps] %.1f phase %d\n", frames / dt, recomp::enh::perfPhase());
            std::fflush(stdout);
            frames = 0;
            last = clock::now();
        }
    }
    recomp::game::drawOverlay();
    recomp::online::drawOverlay();
    recomp::movein::drawWindow();  // src/game/cf_move_in.cpp (Rover asks for the Moving Van DS)
    // CF_FRAME_TAG=1 (testing, tools/interp/screen_run.py): the replay shown in this present as a
    // bar code in the top-left corner: a white and a black reference cell, then 32 bits (present
    // number low 16, tape seq low 8, alpha x 255), most significant first, 16 px cells.
    static const bool sFrameTag = std::getenv("CF_FRAME_TAG") != nullptr;
    if (sFrameTag) {
        const auto info = recomp::interp::lastPresent();
        const u32 v = (info.present & 0xFFFF) << 16 | u32(info.bSeq & 0xFF) << 8 |
                      u32(std::clamp(int(std::lround(info.alpha * 255.0f)), 0, 255));
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 o = ImGui::GetMainViewport()->Pos;
        const float s = 16.0f / std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
        dl->AddRectFilled(o, ImVec2(o.x + s * 34, o.y + s), IM_COL32(0, 0, 0, 255));
        dl->AddRectFilled(o, ImVec2(o.x + s, o.y + s), IM_COL32(255, 255, 255, 255));
        for (int i = 0; i < 32; i++)
            if (v >> (31 - i) & 1)
                dl->AddRectFilled(ImVec2(o.x + s * (2 + i), o.y), ImVec2(o.x + s * (3 + i), o.y + s), IM_COL32(255, 255, 255, 255));
    }
    updateToast();
    refreshRateToast();
    startHint();
    if (!sOpen) return;
    // Default 680 x 400 and at least 520 x 300 at 100%, scaled with the menu size and kept on the
    // screen (ui_scale.cpp; after the first-use position, which would replace the one it sets).
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    recomp::ui::beforeWindow(kMenuWindow, ImVec2(680, 400), ImVec2(520, 300));
    idcheck::beforeWindow();
    menuSizeTestMouse();
    bool open = true;
    if (ImGui::Begin(kMenuWindow, &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.45f);
        if (ImGui::BeginTabBar("tabs")) {
            if (tab("Visuals")) {
                if (ImGui::BeginTabBar("visuals")) {
                    if (ImGui::BeginTabItem("General", nullptr, envTab("General"))) {
                        visualsTab();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Rendering", nullptr, envTab("Rendering"))) {
                        renderingTab();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Enhanced lighting", nullptr, envTab("Enhanced lighting"))) {
                        enhancedLightingTab();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Enhanced world", nullptr, envTab("Enhanced world"))) {
                        sDirty |= recomp::world::drawMenu();  // src/game/cf_world.cpp
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Texture packs", nullptr, envTab("Texture packs"))) {
                        sDirty |= recomp::texpacks::drawMenu();  // src/recomp/texture_packs.cpp
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (tab("Sound")) {
                soundTab();
                ImGui::EndTabItem();
            }
            if (tab("Controls")) {
                sDirty |= recomp::controls::drawTab();
                ImGui::EndTabItem();
            }
            if (tab("Enhancements")) {
                enhancementsTab();
                ImGui::EndTabItem();
            }
            if (tab("Game")) {
                gameTab();
                ImGui::EndTabItem();
            }
            if (tab("Miis")) {
                sDirty |= recomp::cf::miiMenu();  // src/game/cf_mii.cpp, docs/MIIS.md
                ImGui::EndTabItem();
            }
            if (tab("Online")) {
                if (ImGui::BeginTabBar("##online")) {
                    if (ImGui::BeginTabItem("Play together", nullptr, envTab("Play together"))) {
                        sDirty |= recomp::online::drawTab();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Nintendo events", nullptr, envTab("Nintendo events"))) {
                        const bool lock = recomp::online::sync::lockBegin();
                        sDirty |= recomp::distrib::drawMenuPage();  // docs/DISTRIBUTIONS.md
                        recomp::online::sync::lockEnd(lock);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopItemWidth();
        // Quit (with confirmation): ends the host loop the same way closing the window does.
        ImGui::Separator();
        if (recomp::menu::Button("Restore all defaults")) ImGui::OpenPopup("Restore defaults?");
        ImGui::SetItemTooltip("Every setting in this menu (including controller bindings) goes back to its\n"
                              "first-install default. Save files are not touched.");
        if (ImGui::BeginPopupModal("Restore defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Restore every setting to its default? This cannot be undone.");
            ImGui::TextDisabled("A few options (marked in their tooltips) apply on the next launch.");
            if (recomp::menu::Button("Restore defaults")) {
                port::resetSettingsToDefaults();
                // Settings applied only when their control changes: apply the defaults now too.
                applyWindowSettings();
                sDirty = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (recomp::menu::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (recomp::menu::Button("Quit game")) ImGui::OpenPopup("Quit?");
        ImGui::SetItemTooltip("Closes the game (asks first). Settings are saved; unsaved game progress is lost.");
        if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Quit Luigi's Mansion? Unsaved progress will be lost.");
            if (recomp::menu::Button("Quit")) {
                if (sDirty) {
                    port::saveSettings();
                    sDirty = false;
                }
                sQuit = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (recomp::menu::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        idcheck::insideWindow();
    }
    ImGui::End();
    idcheck::afterWindow();
    if (!open) sOpen = false;
    if (!sOpen && sDirty) {
        port::saveSettings();
        sDirty = false;
    }
}

}  // namespace recomp::menu
