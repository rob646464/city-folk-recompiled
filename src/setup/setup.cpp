// First-run setup screen and pipeline (docs/SETUP.md).
#include "setup/setup.h"

#include "port/crash_handler.h"
#include "port/dol.h"
#include "recomp/precache.h"
#include "recomp/ui_scale.h"
#include "setup/disc.h"
#include "setup/module_builder.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace setup {
namespace {

using port::GameVersion;
using port::kVersionCount;
using port::kVersions;

struct Slot {
    std::string path;              // UTF-8, as picked (install.ini)
    bool valid = false;
    std::string message;           // validation result
    std::shared_ptr<Disc> disc;    // opened and verified (valid only)
};

// State shared with the worker thread.
struct Job {
    std::mutex m;
    std::string stage;
    float fraction = 0.0f;
    bool done = false, ok = false;
    std::string error;
    std::atomic<bool> cancel{false};
    std::thread thread;
    ~Job() {
        cancel = true;
        if (thread.joinable()) thread.join();
    }
};

// Results of the file dialogs (SDL may call back on another thread).
std::mutex sPickM;
std::optional<std::string> sPicked[kVersionCount];
std::string sDialogError;  // the file dialog could not open (Linux: no xdg-desktop-portal/zenity)

// Opens and checks `path`. A disc of another region than `slot` moves to its own slot.
void place(Slot slots[], int slot, const std::string& path) {
    Slot s;
    s.path = path;
    if (path.empty()) {
        slots[slot] = s;
        return;
    }
    auto d = std::make_shared<Disc>();
    std::string err;
    if (!d->open(path, err)) {
        s.message = err;
        slots[slot] = s;
        return;
    }
    s.valid = true;
    s.message = describe(d->version()) + ", " + d->kind();
    const int target = int(d->version());
    s.disc = std::move(d);
    if (target != slot) {
        s.message += " (moved here from the " + std::string(port::regionName(kVersions[slot])) + " row)";
        if (!slots[slot].valid) slots[slot] = Slot{};
    }
    slots[target] = std::move(s);
}

struct Plan {
    std::vector<GameVersion> modules;  // modules to (re)build, the base first
    bool empty() const { return modules.empty(); }
};

Plan makePlan(const Slot slots[], GameVersion base) {
    Plan p;
    std::vector<GameVersion> order{base};
    for (GameVersion v : kVersions)
        if (v != base) order.push_back(v);
    for (GameVersion v : order) {
        const Slot& s = slots[int(v)];
        if (s.valid && !moduleUpToDate(*s.disc)) p.modules.push_back(v);
    }
    return p;
}

void startJob(Job& job, const Slot slots[], const Plan& plan) {
    std::shared_ptr<Disc> discs[kVersionCount];
    for (int i = 0; i < kVersionCount; i++) discs[i] = slots[i].disc;
    job.thread = std::thread([&job, discs, plan] {
        auto set = [&](const std::string& stage, float f) {
            std::lock_guard lk(job.m);
            job.stage = stage;
            job.fraction = f;
        };
        auto fail = [&](const std::string& e) {
            std::lock_guard lk(job.m);
            job.error = e;
            job.done = true;
        };
        const float steps = float(plan.modules.size());
        float step = 0.0f;
        for (GameVersion v : plan.modules) {
            const std::string name = port::regionName(v);
            Progress pr;
            pr.cancel = &job.cancel;
            pr.report = [&](const std::string& s, float f) { set(name + " disc: " + s, (step + f) / steps); };
            std::string err;
            if (!buildModule(*discs[int(v)], pr, err)) return fail(err);
            step += 1.0f;
        }
        std::lock_guard lk(job.m);
        job.stage = "Done";
        job.fraction = 1.0f;
        job.ok = true;
        job.done = true;
    });
}

void SDLCALL onPicked(void* userdata, const char* const* files, int) {
    const int slot = int(reinterpret_cast<intptr_t>(userdata));
    std::lock_guard lk(sPickM);
    if (!files) {  // error (cancel is an empty list)
        const char* e = SDL_GetError();
        sDialogError = std::string("The file dialog could not be opened") + (e && *e ? std::string(": ") + e : "") +
                       ". Type or paste the path of the disc image instead.";
        return;
    }
    if (!files[0]) return;  // cancelled
    sPicked[slot] = files[0];
}

std::string dirOf(const std::string& current) {
    const size_t slash = current.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : current.substr(0, slash + 1);
}

void browseImage(int slot, const std::string& current) {
    static const SDL_DialogFileFilter filters[] = {
        {"Wii disc images", "iso;rvz;wbfs;wia;ciso;gcz;nfs"},
        {"All files", "*"},
    };
    const std::string dir = dirOf(current);
    SDL_ShowOpenFileDialog(&onPicked, reinterpret_cast<void*>(intptr_t(slot)), aurora_get_window(), filters, 2,
                           dir.empty() ? nullptr : dir.c_str(), false);
}

void browseFolder(int slot, const std::string& current) {
    const std::string dir = dirOf(current);
    SDL_ShowOpenFolderDialog(&onPicked, reinterpret_cast<void*>(intptr_t(slot)), aurora_get_window(),
                             dir.empty() ? nullptr : dir.c_str(), false);
}

enum class Screen { Pick, Working, Shaders, Failed };

}  // namespace

bool run(const Options& opts, LaunchInfo& launch) {
    recomp::ui::install(recomp::ui::Context::Setup);
    port::loadInstall();
    auto& inst = port::install();
    Slot slots[kVersionCount];
    for (GameVersion v : kVersions) {
        const int i = int(v);
        const std::string& p = !opts.disc[i].empty() ? opts.disc[i] : inst.discOf(v);
        if (!p.empty() && !slots[i].valid) place(slots, i, p);
    }
    if (!opts.anyDisc.empty()) place(slots, int(inst.base), opts.anyDisc);
    GameVersion base = inst.base;

    auto anyValid = [&] {
        for (const Slot& s : slots)
            if (s.valid) return true;
        return false;
    };
    auto chooseBase = [&] {
        if (slots[int(base)].valid) return;
        for (GameVersion v : kVersions)
            if (slots[int(v)].valid) {
                base = v;
                return;
            }
    };
    chooseBase();
    if (!opts.anyDisc.empty()) {  // --disc: play that one
        for (GameVersion v : kVersions)
            if (slots[int(v)].valid && slots[int(v)].path == opts.anyDisc) base = v;
    }

    // Nothing to ask: the configured discs are all readable and the base is available.
    bool showUi = opts.forceUi || opts.autoStart || !anyValid();
    for (const Slot& s : slots)
        if (!s.path.empty() && !s.valid) showUi = true;
    Screen screen = Screen::Pick;
    std::unique_ptr<Job> job;
    Plan plan;
    bool started = false;
    std::chrono::steady_clock::time_point jobStart;

    auto begin = [&] {
        for (GameVersion v : kVersions) inst.discOf(v) = slots[int(v)].valid ? slots[int(v)].path : "";
        inst.base = base;
        port::saveInstall();
        plan = makePlan(slots, base);
        job = std::make_unique<Job>();
        jobStart = std::chrono::steady_clock::now();
        if (!plan.empty()) startJob(*job, slots, plan);
        else job->done = job->ok = true;
        screen = Screen::Working;
        started = true;
    };
    if (!showUi) begin();
    else if (opts.autoStart && anyValid()) begin();

    port::setPhase("setup");
    bool finished = false;
    bool reported = false;
    int framesDrawn = 0;  // the background precache is queued by the first frame (Aurora patch 0013)
    while (!finished) {
        port::heartbeat();
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_EXIT) return false;
            if (ev->type == AURORA_PAUSED) port::setPhase(nullptr);  // waits for the window: not a hang
            if (ev->type == AURORA_UNPAUSED) port::setPhase("setup");
        }
        {
            std::optional<std::string> picked[kVersionCount];
            {
                std::lock_guard lk(sPickM);
                for (int i = 0; i < kVersionCount; i++) picked[i].swap(sPicked[i]);
            }
            for (int i = 0; i < kVersionCount; i++)
                if (picked[i]) {
                    place(slots, i, *picked[i]);
                    // The disc just added is the one to play when it is the only one.
                    for (GameVersion v : kVersions)
                        if (slots[int(v)].valid && slots[int(v)].path == *picked[i] && !slots[int(base)].valid) base = v;
                    chooseBase();
                }
        }
        if (job && screen == Screen::Working && framesDrawn > 0) {
            std::lock_guard lk(job->m);
            if (job->done) {
                if (!reported && !plan.empty()) {
                    reported = true;
                    std::printf("[setup] %s after %.1f s\n", job->ok ? "game modules ready" : "failed",
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - jobStart).count());
                    if (!job->ok) std::printf("[setup] %s\n", job->error.c_str());
                    std::fflush(stdout);
                }
                if (!job->ok) screen = Screen::Failed;
                // Setup was visible (first run, disc change, rebuild after an update): also finish
                // compiling the shaders here, so the first game session runs as smoothly as later
                // ones. Later launches compile whatever is missing in the background (precache.h).
                else if ((showUi || !plan.empty()) && !recomp::precache::progress().finished()) screen = Screen::Shaders;
                else finished = true;
            }
        }
        if (screen == Screen::Shaders && recomp::precache::progress().finished()) finished = true;
        if (finished) break;
        if (!aurora_begin_frame()) continue;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##setup", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        // Menu size (ui_scale.cpp; setup's Auto: 600 lines = 100%), font rasterized at that size.
        const float scale = recomp::ui::scale();
        ImGui::Dummy(ImVec2(0, 12 * scale));
        ImGui::Text("Animal Crossing: City Folk Recompiled");
        ImGui::Separator();
        if (screen == Screen::Pick) {
            ImGui::TextWrapped("This port runs your own copy of Animal Crossing: City Folk (Wii). Choose your disc: "
                               "a disc image (ISO, WBFS, RVZ, WIA, CISO, GCZ) or a folder extracted with Dolphin "
                               "(Properties > Filesystem > Extract Entire Disc). One disc of any region is enough: "
                               "every disc carries every region's events. The game is translated and compiled for "
                               "this PC once; that takes a few minutes.");
            // Portable mode (docs/SETUP.md): where everything goes, or why it was not possible.
            if (const auto& loc = port::dataLocation(); loc.portable) {
                const auto s = loc.root.u8string();
                ImGui::TextDisabled("Portable mode: everything is stored in %s", std::string(s.begin(), s.end()).c_str());
            } else if (!loc.problem.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.4f, 1));
                ImGui::TextWrapped("%s", loc.problem.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 8 * scale));
            for (GameVersion v : kVersions) {
                const int i = int(v);
                ImGui::PushID(i);
                ImGui::Text("%s disc (%s)", port::regionName(v), port::gameId(v));
                ImGui::TextWrapped("%s", slots[i].path.empty() ? "(none)" : slots[i].path.c_str());
                if (ImGui::Button("Disc image...")) browseImage(i, slots[i].path);
                ImGui::SameLine();
                if (ImGui::Button("Extracted folder...")) browseFolder(i, slots[i].path);
#ifndef _WIN32
                // Linux: the file dialog needs xdg-desktop-portal (or zenity/kdialog); a typed or
                // pasted path always works (docs/LINUX.md).
                {
                    static char typed[kVersionCount][1024];
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
                    const bool enter = ImGui::InputTextWithHint("##path", "or type / paste the path here",
                                                                typed[i], sizeof(typed[i]),
                                                                ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button("Use") || enter) && typed[i][0]) {
                        std::string p = typed[i];
                        while (!p.empty() && (p.back() == ' ' || p.back() == '\n')) p.pop_back();
                        if (p.size() >= 2 && (p.front() == '\'' || p.front() == '"') && p.back() == p.front())
                            p = p.substr(1, p.size() - 2);  // pasted from a file manager / shell
                        if (p.rfind("file://", 0) == 0) p = p.substr(7);
                        std::lock_guard lk(sPickM);
                        sPicked[i] = p;
                        typed[i][0] = 0;
                    }
                }
#endif
                if (!slots[i].path.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Remove")) {
                        slots[i] = Slot{};
                        chooseBase();
                    }
                    if (slots[i].valid) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", slots[i].message.c_str());
                    else {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1));
                        ImGui::TextWrapped("%s", slots[i].message.c_str());
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::Dummy(ImVec2(0, 8 * scale));
                ImGui::PopID();
            }
            {
                std::lock_guard lk(sPickM);
                if (!sDialogError.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.4f, 1));
                    ImGui::TextWrapped("%s", sDialogError.c_str());
                    ImGui::PopStyleColor();
                }
            }
            int validCount = 0;
            for (const Slot& s : slots) validCount += s.valid ? 1 : 0;
            if (validCount > 1) {
                ImGui::Text("Version to play:");
                for (GameVersion v : kVersions) {
                    if (!slots[int(v)].valid) continue;
                    ImGui::SameLine();
                    if (ImGui::RadioButton(port::regionName(v), base == v)) base = v;
                }
                ImGui::TextDisabled("Every disc chosen here is prepared; you can switch later in the F1 menu (Game tab).");
            }
            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::BeginDisabled(validCount == 0);
            if (ImGui::Button(started ? "Continue" : "Install and play")) begin();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Quit")) {
                ImGui::End();
                aurora_end_frame();
                return false;
            }
        } else {
            std::string stage, error;
            float f = 0.0f;
            {
                std::lock_guard lk(job->m);
                stage = job->stage;
                f = job->fraction;
                error = job->error;
            }
            if (screen == Screen::Shaders) {
                const auto pr = recomp::precache::progress();
                ImGui::TextWrapped("Compiling shaders for your graphics card. This happens once (and again after a "
                                   "driver or game update).");
                ImGui::Dummy(ImVec2(0, 8 * scale));
                ImGui::ProgressBar(pr.fraction(), ImVec2(-1, 0));
                ImGui::Text("Compiling shaders: %u / %u", pr.done, pr.total);
                // (No "play now": the game waits for its shaders anyway, precache.h holdGame.)
            } else if (screen == Screen::Working) {
                ImGui::TextWrapped("Preparing the game: translating main.dol and its 187 modules for this PC and "
                                   "compiling them. This happens once per disc (and again after updates).");
                ImGui::Dummy(ImVec2(0, 8 * scale));
                ImGui::ProgressBar(f, ImVec2(-1, 0));
                ImGui::TextWrapped("%s", stage.c_str());
                ImGui::Dummy(ImVec2(0, 8 * scale));
                if (ImGui::Button("Cancel")) {
                    job.reset();  // joins the worker after it notices
                    screen = Screen::Pick;
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "Setup failed");
                ImGui::TextWrapped("%s", error.c_str());
                ImGui::Dummy(ImVec2(0, 8 * scale));
                if (ImGui::Button("Back")) {
                    job.reset();
                    screen = Screen::Pick;
                }
            }
        }
        ImGui::End();
        aurora_end_frame();
        framesDrawn++;
        // While the game is prepared and the shaders compile (from the first frame on): frames below
        // the refresh rate, as in the game's shader hold (issue #33; precache.h holdFramePause).
        if (screen != Screen::Pick) recomp::precache::holdFramePause(0.0);
    }
    job.reset();

    // Ready: report what is installed and what to launch.
    for (GameVersion v : kVersions) inst.ready[int(v)] = slots[int(v)].valid;
    inst.running = base;
    launch.version = base;
    launch.disc = slots[int(base)].disc->runPath();
    launch.module = modulePath(base);
    return true;
}

}  // namespace setup
