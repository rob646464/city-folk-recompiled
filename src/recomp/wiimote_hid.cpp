// Real Wii Remotes over HID (docs/CONTROLS.md "Real Wii Remotes"): RVL-CNT-01 and -TR paired with
// the PC over Bluetooth, or a Mayflash DolphinBar in its Wii Remote mode (mode 4).
//
// A port whose source is "Real Wii Remote" (F1 > Controls > Ports) takes its Remote state from a
// real one instead of the emulated source (wiimote.cpp). Devices are found and opened with SDL's
// hidapi (SDL_hid_*), only Nintendo 057E:0306 / 057E:0330; SDL's own Wii joystick driver is kept
// off (SDL_HINT_JOYSTICK_HIDAPI_WII) while a port uses real Remotes, so it never sets the Remote's
// report mode under us. Nothing is enumerated or opened while every port is emulated (the default).
//
// Threads: a manager thread (enumeration every 2 s and on hidapi device changes, port
// assignment, LEDs) and one I/O thread per opened Remote, which owns its Proto (wiimote_proto.cpp:
// setup, report decoding, command queue, speaker stream) and publishes a snapshot (decoded input +
// pointer from the IR dots). The guest thread (KPADRead through wiimote::sample) only copies the
// snapshot of its port's Remote; rumble / speaker calls are queued for the I/O thread.
//
// Settings: [wiimote_hid] (remote<N>_source=emulated|real, remote<N>_device=auto|<serial>,
// pointer_range, pointer_offset, invert_x, invert_y, speaker). Sensor bar position and IR
// sensitivity are the console's (F1 > Enhancements > General > Console, SYSCONF BT.BAR / BT.SENS).
// Test aids: CF_WIIMOTE_HID=0 (never touch HID devices; ports stay emulated), CF_WIIMOTE_HID=nodevices
// (real-Remote ports and menu without any HID access), CF_WIIMOTE_HID_LOG=1.
#include "recomp/wiimote.h"
#include "recomp/wiimote_proto.h"

#include "port/settings.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"
#include "recomp/system.h"
#include "recomp/ui_scale.h"

#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::wiimote {
namespace {

using namespace std::chrono_literals;
using hid::Proto;

double hostNow() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

const bool sDisabled = [] {
    const char* v = std::getenv("CF_WIIMOTE_HID");
    return v && *v == '0';
}();
// CF_WIIMOTE_HID=nodevices (test runs): the real-Remote ports and menu work, but hidapi is never
// initialized or enumerated, so no device is touched ("No Wii Remote found").
const bool sNoDevices = [] {
    const char* v = std::getenv("CF_WIIMOTE_HID");
    return v && std::strcmp(v, "nodevices") == 0;
}();
const bool sLog = [] {
    const char* v = std::getenv("CF_WIIMOTE_HID_LOG");
    return v && *v && *v != '0';
}();

// ---- Configuration -------------------------------------------------------------------------

struct PortCfg {
    bool real = false;          // source: real Wii Remote
    std::string device;         // "" = auto (next free Remote), else the Remote's serial
};
struct Config {
    PortCfg ports[kRemotes];
    float range = 1.0f, offset = 0.0f;
    bool invertX = false, invertY = false;
    bool speaker = true;        // play the game's Remote speaker sound on the Remote
};
std::mutex sCfgMutex;
Config sCfg;

void applySdlHint();

void resetConfig() {
    std::lock_guard lk(sCfgMutex);
    sCfg = Config{};
}

float parseF(const std::string& v, float lo, float hi, float def) {
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end != v.c_str() && std::isfinite(f) ? std::clamp(f, lo, hi) : def;
}

void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "wiimote_hid") return;
    {
        std::lock_guard lk(sCfgMutex);
        const bool on = value == "true" || value == "1";
        if (key.size() > 7 && key.rfind("remote", 0) == 0 && key[6] >= '1' && key[6] <= '4' && key[7] == '_') {
            PortCfg& pc = sCfg.ports[key[6] - '1'];
            const std::string k = key.substr(8);
            if (k == "source") pc.real = value == "real";
            else if (k == "device") pc.device = value == "auto" ? "" : value;
        } else if (key == "pointer_range") sCfg.range = parseF(value, 0.5f, 2.0f, sCfg.range);
        else if (key == "pointer_offset") sCfg.offset = parseF(value, -1.0f, 1.0f, sCfg.offset);
        else if (key == "invert_x") sCfg.invertX = on;
        else if (key == "invert_y") sCfg.invertY = on;
        else if (key == "speaker") sCfg.speaker = on;
    }
    applySdlHint();
}

void saveConfig(std::ostream& out) {
    std::lock_guard lk(sCfgMutex);
    out << "[wiimote_hid]\n";
    for (int i = 0; i < kRemotes; i++)
        out << "remote" << i + 1 << "_source=" << (sCfg.ports[i].real ? "real" : "emulated") << '\n'
            << "remote" << i + 1 << "_device=" << (sCfg.ports[i].device.empty() ? "auto" : sCfg.ports[i].device) << '\n';
    out << "pointer_range=" << sCfg.range << '\n'
        << "pointer_offset=" << sCfg.offset << '\n'
        << "invert_x=" << (sCfg.invertX ? "true" : "false") << '\n'
        << "invert_y=" << (sCfg.invertY ? "true" : "false") << '\n'
        << "speaker=" << (sCfg.speaker ? "true" : "false") << '\n';
}

const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

bool anyReal() {
    std::lock_guard lk(sCfgMutex);
    for (const auto& p : sCfg.ports)
        if (p.real) return true;
    return false;
}

// SDL's own Wii driver (off by default in SDL 3) must not open the Remotes we drive: it would
// set its own report mode and LEDs. Forced off while a port uses real Remotes.
void applySdlHint() {
    static bool forced = false;
    if (sDisabled || forced || !anyReal()) return;
    forced = true;
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_WII, "0", SDL_HINT_OVERRIDE);
}

std::string narrow(const wchar_t* w) {
    std::string s;
    if (!w) return s;
    for (; *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

// ---- Devices -------------------------------------------------------------------------------

struct SdlLink final : hid::Link {
    SDL_hid_device* h = nullptr;
    int write(const hid::u8* d, size_t n) override { return SDL_hid_write(h, d, n); }
};

struct Snapshot {
    bool connected = false;     // set up and reporting
    bool answering = false;     // answered at least once (maybe still setting up)
    hid::Input in;
    hid::PointerState ptr;
};

struct Device {
    std::string path, serial, product;
    u16 pid = 0;
    SDL_hid_device* h = nullptr;
    std::atomic<bool> stop{false}, gone{false};
    std::atomic<int> port{-1};
    std::mutex m;               // everything below
    Snapshot snap;
    // Requests for the I/O thread.
    bool rumble = false;
    std::deque<int> speakerCmds;
    std::vector<u8> speakerBytes;
    bool speakerEnabled = true;
    hid::PointerParams params;
    Proto::Options opts;
    std::string label() const { return product + (serial.empty() ? "" : " (" + serial + ")"); }
};

std::mutex sDevMutex;  // the device list
std::vector<std::shared_ptr<Device>> sDevices;
std::atomic<bool> sManagerStarted{false};

// A paired Remote that is switched off fails every write: its path is reopened every few seconds,
// logged the first time only.
bool firstFailure(const std::string& path) {
    static std::mutex m;
    static std::vector<std::string> seen;
    std::lock_guard lk(m);
    if (std::find(seen.begin(), seen.end(), path) != seen.end()) return sLog;
    seen.push_back(path);
    return true;
}

void ioThread(std::shared_ptr<Device> dev) {
    SdlLink link;
    link.h = dev->h;
    std::unique_ptr<Proto> proto;
    double started = 0;
    int lastPort = -2;
    bool lastRumble = false;
    bool wasConnected = false;
    u32 lastIrSeq = 0;
    hid::PointerState ptr;
    auto restart = [&](double now) {
        proto = std::make_unique<Proto>(link);
        Proto::Options o;
        {
            std::lock_guard lk(dev->m);
            o = dev->opts;
        }
        proto->setOptions(o);
        const int p = dev->port.load();
        proto->setLeds(p >= 0 ? u8(1 << p) : 0);
        lastPort = p;
        lastRumble = false;
        proto->start(now);
        started = now;
        ptr = {};
    };
    restart(hostNow());
    u8 buf[64];
    while (!dev->stop.load()) {
        const int n = SDL_hid_read_timeout(dev->h, buf, sizeof(buf), 4);
        const double now = hostNow();
        if (n < 0) {
            if (firstFailure(dev->path)) log("[wiimote-hid] %s: read failed, closing", dev->label().c_str());
            break;
        }
        if (n > 0) proto->onInput(buf, size_t(n), now);
        // Requests from the game / menu.
        hid::PointerParams pp;
        {
            std::lock_guard lk(dev->m);
            if (dev->rumble != lastRumble) {
                lastRumble = dev->rumble;
                proto->setRumble(lastRumble);
            }
            for (int cmd : dev->speakerCmds)
                if (dev->speakerEnabled || cmd == 0) proto->speakerControl(cmd);
            dev->speakerCmds.clear();
            if (!dev->speakerBytes.empty()) {
                if (dev->speakerEnabled) proto->speakerData(dev->speakerBytes.data(), dev->speakerBytes.size());
                dev->speakerBytes.clear();
            }
            proto->setOptions(dev->opts);
            pp = dev->params;
        }
        const int p = dev->port.load();
        if (p != lastPort) {
            lastPort = p;
            proto->setLeds(p >= 0 ? u8(1 << p) : 0);
        }
        proto->tick(now);
        if (proto->failed()) {
            if (firstFailure(dev->path))
                log("[wiimote-hid] %s: write failed (turned off or out of range), closing", dev->label().c_str());
            break;
        }
        const hid::Input& in = proto->input();
        if (in.irSeq != lastIrSeq) {
            lastIrSeq = in.irSeq;
            hid::updatePointer(ptr, in.dots, in.acc, pp);
        }
        const bool reporting = in.ready && in.lastReport >= 0 && now - in.lastReport < 1.0;
        if (reporting != wasConnected) {
            wasConnected = reporting;
            log("[wiimote-hid] %s %s", dev->label().c_str(), reporting ? "connected" : "stopped reporting");
            if (reporting && sLog)
                log("[wiimote-hid]   calibration %s (zero %u %u %u, 1 g %u %u %u), battery %u/4 (0x%02X)",
                    proto->calibration().fromEeprom ? "from the Remote" : "default", proto->calibration().zero[0],
                    proto->calibration().zero[1], proto->calibration().zero[2], proto->calibration().one[0],
                    proto->calibration().one[1], proto->calibration().one[2], in.battery, in.batteryRaw);
        }
        // Not answering (off, asleep, an empty DolphinBar slot) or gone quiet: start over now and
        // then; a Remote that wakes up answers the status request.
        if ((!proto->answered() && now - started > 3.0) || (proto->answered() && now - in.lastReport > 3.0)) restart(now);
        {
            std::lock_guard lk(dev->m);
            dev->snap.connected = reporting;
            dev->snap.answering = proto->answered();
            dev->snap.in = in;
            dev->snap.ptr = ptr;
        }
    }
    if (proto) {
        proto->setRumble(false);
        proto->speakerControl(0);
    }
    {
        std::lock_guard lk(dev->m);
        dev->snap = Snapshot{};
    }
    SDL_hid_close(dev->h);
    dev->h = nullptr;
    dev->gone = true;
}

// Port assignment: each real-Remote port takes its pinned Remote (by serial) or the first
// answering Remote no other port has.
void assignPorts() {
    Config cfg;
    {
        std::lock_guard lk(sCfgMutex);
        cfg = sCfg;
    }
    std::lock_guard lk(sDevMutex);
    std::vector<int> want(sDevices.size(), -1);
    for (int p = 0; p < kRemotes; p++) {
        if (!cfg.ports[p].real) continue;
        int pick = -1;
        for (size_t i = 0; i < sDevices.size() && pick < 0; i++)
            if (want[i] < 0 && !cfg.ports[p].device.empty() && sDevices[i]->serial == cfg.ports[p].device) pick = int(i);
        if (pick < 0 && cfg.ports[p].device.empty()) {
            // Keep the current holder if it still answers, else the first free answering one.
            for (size_t i = 0; i < sDevices.size() && pick < 0; i++)
                if (want[i] < 0 && sDevices[i]->port.load() == p) pick = int(i);
            for (size_t i = 0; i < sDevices.size() && pick < 0; i++) {
                bool answering;
                {
                    std::lock_guard lk2(sDevices[i]->m);
                    answering = sDevices[i]->snap.connected;
                }
                bool pinnedElsewhere = false;
                for (int q = 0; q < kRemotes; q++)
                    if (q != p && cfg.ports[q].real && !cfg.ports[q].device.empty() && cfg.ports[q].device == sDevices[i]->serial)
                        pinnedElsewhere = true;
                if (want[i] < 0 && answering && !pinnedElsewhere && sDevices[i]->port.load() < 0) pick = int(i);
            }
        }
        if (pick >= 0) want[size_t(pick)] = p;
    }
    const float range = cfg.range, offset = cfg.offset;
    const auto& sys = sys::settings();
    for (size_t i = 0; i < sDevices.size(); i++) {
        Device& d = *sDevices[i];
        if (d.port.exchange(want[i]) != want[i])
            log("[wiimote-hid] %s -> %s", d.label().c_str(), want[i] >= 0 ? ("Wii Remote " + std::to_string(want[i] + 1)).c_str() : "no port");
        std::lock_guard lk2(d.m);
        d.params.barAbove = sys.sensorBar == 1;
        d.params.range = range;
        d.params.offsetY = offset;
        d.params.invertX = cfg.invertX;
        d.params.invertY = cfg.invertY;
        d.opts.irSensitivity = sys.irSensitivity;
        d.opts.speakerVolume = sys.speakerVolume;
        d.speakerEnabled = cfg.speaker;
    }
}

void managerThread() {
    if (sNoDevices) {
        log("[wiimote-hid] CF_WIIMOTE_HID=nodevices: not looking for Wii Remotes");
        return;
    }
    if (SDL_hid_init() != 0) {
        log("[wiimote-hid] hidapi not available: %s", SDL_GetError());
        return;
    }
    Uint32 lastChange = ~0u;
    double lastEnum = -100;
    std::vector<std::string> failedPaths;  // logged once
    for (;;) {
        const bool want = anyReal();
        const double now = hostNow();
        if (!want) {
            // Every port emulated again: let the Remotes go.
            std::vector<std::shared_ptr<Device>> close;
            {
                std::lock_guard lk(sDevMutex);
                close.swap(sDevices);
            }
            for (auto& d : close) d->stop = true;
            std::this_thread::sleep_for(250ms);
            lastEnum = -100;
            continue;
        }
        {
            std::lock_guard lk(sDevMutex);
            sDevices.erase(std::remove_if(sDevices.begin(), sDevices.end(), [](const auto& d) { return d->gone.load(); }),
                           sDevices.end());
        }
        const Uint32 change = SDL_hid_device_change_count();
        if (change != lastChange || now - lastEnum > 2.0) {
            lastChange = change;
            lastEnum = now;
            SDL_hid_device_info* list = SDL_hid_enumerate(hid::kVendorNintendo, 0);
            for (SDL_hid_device_info* e = list; e; e = e->next) {
                if (!hid::isWiiRemote(e->vendor_id, e->product_id) || !e->path) continue;
                {
                    std::lock_guard lk(sDevMutex);
                    if (std::any_of(sDevices.begin(), sDevices.end(), [&](const auto& d) { return d->path == e->path; })) continue;
                }
                SDL_hid_device* h = SDL_hid_open_path(e->path);
                if (!h) {
                    if (std::find(failedPaths.begin(), failedPaths.end(), e->path) == failedPaths.end()) {
                        failedPaths.push_back(e->path);
                        log("[wiimote-hid] cannot open %s: %s", e->path, SDL_GetError());
                    }
                    continue;
                }
                auto d = std::make_shared<Device>();
                d->path = e->path;
                d->serial = narrow(e->serial_number);
                d->product = narrow(e->product_string);
                if (d->product.empty()) d->product = e->product_id == hid::kProductRemoteTR ? "RVL-CNT-01-TR" : "RVL-CNT-01";
                d->pid = e->product_id;
                d->h = h;
                if (sLog) log("[wiimote-hid] opened %s (%04X:%04X) %s", d->label().c_str(), e->vendor_id, e->product_id, e->path);
                {
                    std::lock_guard lk(sDevMutex);
                    sDevices.push_back(d);
                }
                assignPorts();  // options before the first status request
                std::thread(ioThread, d).detach();
            }
            SDL_hid_free_enumeration(list);
        }
        assignPorts();
        std::this_thread::sleep_for(100ms);
    }
}

void ensureManager() {
    if (sDisabled || !anyReal()) return;
    applySdlHint();
    if (!sManagerStarted.exchange(true)) std::thread(managerThread).detach();
}

std::shared_ptr<Device> deviceFor(int chan) {
    std::lock_guard lk(sDevMutex);
    for (auto& d : sDevices)
        if (d->port.load() == chan && !d->gone.load()) return d;
    return nullptr;
}

// ---- Source --------------------------------------------------------------------------------

class HidSource final : public Source {
public:
    const char* kind() const override { return "real Wii Remote"; }

    void read(int chan, double /*now*/, State& st) override {
        st = State{};
        ensureManager();
        auto d = deviceFor(chan);
        if (!d) return;
        Snapshot s;
        {
            std::lock_guard lk(d->m);
            s = d->snap;
        }
        if (!s.connected) return;
        const bool menuOpen = menu::isOpen();
        st.connected = true;
        const bool nunchuk = s.in.ext == hid::Ext::Nunchuk;
        st.ext = nunchuk ? kDevNunchuk : kDevCore;
        st.buttons = menuOpen ? 0 : s.in.buttons & kBtnGameMask;
        if (!nunchuk) st.buttons &= ~(kBtnC | kBtnZ);
        st.pointerValid = s.ptr.valid && !menuOpen;
        st.px = s.ptr.px;
        st.py = s.ptr.py;
        st.dist = s.ptr.dist;
        st.roll = s.ptr.roll;
        st.acc = {s.in.acc.x, s.in.acc.y, s.in.acc.z};
        if (nunchuk) {
            st.stickX = menuOpen ? 0 : s.in.stickX;
            st.stickY = menuOpen ? 0 : s.in.stickY;
            st.nunAcc = {s.in.nunAcc.x, s.in.nunAcc.y, s.in.nunAcc.z};
        }
        st.battery = s.in.battery;
    }

    void rumble(int chan, bool on) override {
        if (auto d = deviceFor(chan)) {
            std::lock_guard lk(d->m);
            d->rumble = on;
        }
    }

    void speakerControl(int chan, int cmd) override {
        if (auto d = deviceFor(chan)) {
            std::lock_guard lk(d->m);
            d->speakerCmds.push_back(cmd);
        }
    }

    void speakerData(int chan, const u8* data, u32 len) override {
        if (auto d = deviceFor(chan)) {
            std::lock_guard lk(d->m);
            d->speakerBytes.insert(d->speakerBytes.end(), data, data + len);
        }
    }
};
HidSource sHid;

}  // namespace

Source* hidSource(int chan) {
    if (sDisabled || chan < 0 || chan >= kRemotes) return nullptr;
    std::lock_guard lk(sCfgMutex);
    return sCfg.ports[chan].real ? &sHid : nullptr;
}

void hidShutdown() {
    std::lock_guard lk(sDevMutex);
    for (auto& d : sDevices) {
        {
            std::lock_guard lk2(d->m);
            d->rumble = false;
        }
        d->stop = true;
    }
    // The I/O threads stop the motor and close their device within a few ms.
    for (int i = 0; i < 20; i++) {
        bool all = true;
        for (auto& d : sDevices) all &= d->gone.load();
        if (all) break;
        std::this_thread::sleep_for(5ms);
    }
}

// ---- Menu ----------------------------------------------------------------------------------

// Test aid CF_WIIMOTE_HID_MENU=1 (screenshots): the Ports tab scrolled to this section, pairing help open.
const bool sMenuShot = std::getenv("CF_WIIMOTE_HID_MENU") != nullptr;

bool drawHidPortsSection() {
    bool changed = false;
    ImGui::SeparatorText("Real Wii Remotes");
    if (sMenuShot) ImGui::SetScrollHereY(0.0f);
    if (sDisabled) {
        ImGui::TextDisabled("Turned off for this run (CF_WIIMOTE_HID=0).");
        return false;
    }
    std::vector<std::shared_ptr<Device>> devs;
    {
        std::lock_guard lk(sDevMutex);
        devs = sDevices;
    }
    Config cfg;
    {
        std::lock_guard lk(sCfgMutex);
        cfg = sCfg;
    }
    const float k = recomp::ui::scale();
    if (ImGui::BeginTable("realremotes", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Remote", ImGuiTableColumnFlags_WidthFixed, 50 * k);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Which", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kRemotes; i++) {
            PortCfg& pc = cfg.ports[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", i + 1);
            ImGui::TableNextColumn();
            static const char* kSrc[] = {"Emulated", "Real Wii Remote"};
            int si = pc.real ? 1 : 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##source", &si, kSrc, 2)) {
                pc.real = si == 1;
                changed = true;
            }
            ImGui::SetItemTooltip("Emulated: this Wii Remote is built from the controller / keyboard / mouse above.\n"
                                  "Real Wii Remote: a Wii Remote paired with this PC over Bluetooth (or through a\n"
                                  "DolphinBar in mode 4) drives it: buttons, IR pointer (needs a sensor bar),\n"
                                  "motion, Nunchuk, rumble and speaker. The controller assigned above is then unused.");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!pc.real);
            std::string cur = "Auto";
            for (const auto& d : devs)
                if (!pc.device.empty() && d->serial == pc.device) cur = d->label();
            if (!pc.device.empty() && cur.rfind("Auto", 0) == 0) cur = pc.device + " (not found)";
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##which", cur.c_str())) {
                if (ImGui::Selectable("Auto", pc.device.empty())) {
                    pc.device.clear();
                    changed = true;
                }
                for (const auto& d : devs) {
                    if (d->serial.empty()) continue;
                    const std::string label = d->label() + "##" + d->path;
                    if (ImGui::Selectable(label.c_str(), pc.device == d->serial)) {
                        pc.device = d->serial;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Auto: the first answering Wii Remote no other port uses (its player LED shows\n"
                                  "the port). Or always the same Remote, by its Bluetooth address.");
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (!pc.real) {
                ImGui::TextDisabled("-");
            } else {
                std::shared_ptr<Device> mine;
                for (const auto& d : devs)
                    if (d->port.load() == i) mine = d;
                Snapshot s;
                if (mine) {
                    std::lock_guard lk(mine->m);
                    s = mine->snap;
                }
                if (mine && s.connected) {
                    const char* ext = s.in.ext == hid::Ext::Nunchuk ? ", Nunchuk"
                                      : s.in.ext == hid::Ext::Other ? ", unsupported extension"
                                      : s.in.ext == hid::Ext::Identifying ? ", extension..." : "";
                    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "Connected, battery %u/4%s%s", s.in.battery, ext,
                                       s.ptr.valid ? "" : ", no sensor bar seen");
                    ImGui::SetItemTooltip("%s", mine->label().c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "Searching...");
                    ImGui::SetItemTooltip("No answering Wii Remote for this port yet. Press a button on a paired Remote\n"
                                          "(one paired with 1 + 2 must be paired again; see \"How to pair\" below).");
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (anyReal() || cfg.ports[0].real || cfg.ports[1].real || cfg.ports[2].real || cfg.ports[3].real) {
        if (devs.empty()) ImGui::TextDisabled("No Wii Remote found (Nintendo RVL-CNT-01 / -TR, DolphinBar mode 4).");
        for (const auto& d : devs) {
            Snapshot s;
            {
                std::lock_guard lk(d->m);
                s = d->snap;
            }
            const int p = d->port.load();
            ImGui::BulletText("%s: %s%s", d->label().c_str(),
                              s.connected ? "answering" : s.answering ? "setting up" : "not answering (off or asleep)",
                              p >= 0 ? (", Wii Remote " + std::to_string(p + 1)).c_str() : "");
        }
    }
    menu::openTreesForCheck();
    if (sMenuShot) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::TreeNode("How to pair a Wii Remote")) {
        ImGui::TextWrapped(
            "Windows: Settings > Bluetooth & devices > Add device > Bluetooth. Press the red SYNC button "
            "under the Remote's battery cover (or hold 1 + 2) and pick \"Nintendo RVL-CNT-01\" (or -TR). If "
            "Windows asks for a PIN, skip it (leave it empty). A Remote paired with SYNC reconnects when one "
            "of its buttons is pressed; one paired with 1 + 2 has to be paired again each time.");
        ImGui::TextWrapped(
            "Mayflash DolphinBar: press its mode button until it is in mode 4 (Wii Remote), then sync the "
            "Remotes with the bar (SYNC on the bar, then SYNC on the Remote). The bar is also the sensor bar.");
        ImGui::TextWrapped(
            "Linux: pair with your desktop's Bluetooth settings or bluetoothctl. The game needs read/write "
            "access to the Remote's /dev/hidraw node (a udev rule for 057e:0306 and 057e:0330). macOS: "
            "pair in System Settings > Bluetooth (support depends on the macOS version).");
        ImGui::TextWrapped(
            "The IR pointer needs a sensor bar (or the DolphinBar) at the TV. Its position (above / below) "
            "and sensitivity are the console settings in Enhancements > General > Console.");
        ImGui::TreePop();
    }
    if (changed) {
        {
            std::lock_guard lk(sCfgMutex);
            for (int i = 0; i < kRemotes; i++) sCfg.ports[i] = cfg.ports[i];
        }
        ensureManager();
    }
    return changed;
}

bool drawHidPointerSection() {
    bool changed = false;
    std::lock_guard lk(sCfgMutex);
    ImGui::SeparatorText("Real Wii Remotes");
    changed |= recomp::menu::SliderFloat("Pointer range##hid", &sCfg.range, 0.5f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How far the pointer moves for a turn of a real Wii Remote (1 = default). Higher: smaller\n"
                          "movements cross the screen.");
    changed |= recomp::menu::SliderFloat("Pointer height##hid", &sCfg.offset, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Moves a real Remote's pointer up (negative) or down, on top of the sensor bar position\n"
                          "(console setting).");
    changed |= recomp::menu::Checkbox("Invert IR X##hid", &sCfg.invertX);
    ImGui::SameLine();
    changed |= recomp::menu::Checkbox("Invert IR Y##hid", &sCfg.invertY);
    ImGui::SetItemTooltip("Only if the pointer moves the wrong way with your Remote (it should not).");
    changed |= recomp::menu::Checkbox("Remote speaker sound##hid", &sCfg.speaker);
    ImGui::SetItemTooltip("Plays the sounds the game sends to the Wii Remote speaker on the Remote itself.\n"
                          "Over Windows' Bluetooth stack the speaker can crackle.");
    return changed;
}

}  // namespace recomp::wiimote
