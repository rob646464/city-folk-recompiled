// Wii Remote ports: where each Remote's state comes from (wiimote.h, docs/CONTROLS.md).
//
// The emulated source builds a Remote + Nunchuk from what F1 > Controls binds to the port
// (controls.cpp: keyboard/mouse buttons, a gamepad's buttons and sticks) and from the port's
// pointer devices:
//   - mouse: the pointer is where the mouse is over the game image (absolute). Outside the
//     window the Remote "points off the screen" (no sensor bar dots: pointer invalid);
//   - right stick (or the pointer keys): moves the pointer (relative, or absolute: the stick's
//     position is the pointer's);
//   - gyro: a gamepad's gyroscope turns the pointer like aiming a Remote (SDL sensors).
// Several pointer devices can share a port: the one that moved last places the pointer.
// Motion: the Remote/Nunchuk rest level (acc -1 g on y), "shake" inputs swing them.
// The --input script (test runs) drives the Remotes too: Wii button names, STICK*, SHAKE and
// "@x/y" pointer positions (CF_INPUT_CLOCK as for hle_pad.cpp).
// Settings: [wiimote] in settings.ini (port::addSettingsExtension).
#include "recomp/wiimote.h"

#include "port/options.h"
#include "port/settings.h"
#include "recomp/controls.h"
#include "recomp/game.h"
#include "recomp/menu.h"
#include "recomp/menu_nav.h"
#include "recomp/runtime.h"
#include "recomp/ui_scale.h"
#include "recomp/widescreen.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <map>
#include <vector>

namespace recomp {
void log(const char* fmt, ...);       // runtime.cpp
u32 readHostPads(PADStatus* st);      // hle_pad.cpp
u32 viRetraceCount();                 // hle_vi.cpp
}  // namespace recomp

namespace recomp::wiimote {
namespace {

constexpr float kPi = 3.14159265f;

// ---- Configuration -------------------------------------------------------------------

enum class Connect : u8 { Auto, Always, Never };
enum PointerBits : u8 { kPtrMouse = 1, kPtrStick = 2, kPtrGyro = 4 };

struct RemoteConfig {
    Connect connect = Connect::Auto;  // Auto: connected while a device drives the port
    bool nunchuk = true;
    u8 pointer = kPtrStick;           // PointerBits
    // Snap pointer to menu items (menu_nav.h). Auto: while the mouse does not place the pointer.
    menunav::Mode snap = menunav::Mode::Auto;
};

const char* snapKey(menunav::Mode m) {
    return m == menunav::Mode::On ? "on" : m == menunav::Mode::Off ? "off" : "auto";
}

struct Config {
    std::array<RemoteConfig, kRemotes> remotes;
    float stickSpeed = 1.4f;      // relative stick pointer: half-screens per second at full tilt
    bool stickAbsolute = false;   // the stick's position is the pointer's
    float gyroSensitivity = 3.0f; // half-screens per radian
    bool gyroInvertX = false, gyroInvertY = false;
    bool hideCursor = true;       // hide the OS cursor over the game while the mouse points
    float pointerTimeout = 3.0f;  // s without pointer movement until the pointer leaves the screen (0: never)
    float distance = 2.0f;        // reported distance from the sensor bar (m)
    Config() { remotes[0].pointer = kPtrMouse | kPtrStick; }
};

std::mutex sMutex;  // menu (host thread) vs sample (guest thread)
Config sCfg;

const char* connectKey(Connect c) { return c == Connect::Always ? "on" : c == Connect::Never ? "off" : "auto"; }

std::string pointerKey(u8 p) {
    std::string s;
    if (p & kPtrMouse) s += "mouse";
    if (p & kPtrStick) s += s.empty() ? "stick" : "+stick";
    if (p & kPtrGyro) s += s.empty() ? "gyro" : "+gyro";
    return s.empty() ? "none" : s;
}

u8 parsePointer(const std::string& v) {
    u8 p = 0;
    if (v.find("mouse") != std::string::npos) p |= kPtrMouse;
    if (v.find("stick") != std::string::npos) p |= kPtrStick;
    if (v.find("gyro") != std::string::npos) p |= kPtrGyro;
    return p;
}

float parseFloat(const std::string& v, float lo, float hi, float def) {
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end != v.c_str() && std::isfinite(f) ? std::clamp(f, lo, hi) : def;
}

void resetConfig() {
    std::lock_guard lk(sMutex);
    sCfg = Config{};
}

void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section != "wiimote") return;
    std::lock_guard lk(sMutex);
    const bool on = value == "true" || value == "1";
    // remote<N>_connect / remote<N>_extension / remote<N>_pointer
    if (key.size() > 7 && key.rfind("remote", 0) == 0 && key[6] >= '1' && key[6] <= '4' && key[7] == '_') {
        auto& rc = sCfg.remotes[key[6] - '1'];
        const std::string k = key.substr(8);
        if (k == "connect") rc.connect = value == "on" ? Connect::Always : value == "off" ? Connect::Never : Connect::Auto;
        else if (k == "extension") rc.nunchuk = value != "none";
        else if (k == "pointer") rc.pointer = parsePointer(value);
        else if (k == "snap") rc.snap = value == "on" ? menunav::Mode::On : value == "off" ? menunav::Mode::Off : menunav::Mode::Auto;
        return;
    }
    if (key == "pointer_stick_speed") sCfg.stickSpeed = parseFloat(value, 0.2f, 5.0f, sCfg.stickSpeed);
    else if (key == "pointer_stick_absolute") sCfg.stickAbsolute = on;
    else if (key == "gyro_sensitivity") sCfg.gyroSensitivity = parseFloat(value, 0.5f, 10.0f, sCfg.gyroSensitivity);
    else if (key == "gyro_invert_x") sCfg.gyroInvertX = on;
    else if (key == "gyro_invert_y") sCfg.gyroInvertY = on;
    else if (key == "hide_cursor") sCfg.hideCursor = on;
    else if (key == "pointer_timeout") sCfg.pointerTimeout = parseFloat(value, 0.0f, 30.0f, sCfg.pointerTimeout);
    else if (key == "distance") sCfg.distance = parseFloat(value, 0.5f, 5.0f, sCfg.distance);
}

void saveConfig(std::ostream& out) {
    std::lock_guard lk(sMutex);
    out << "[wiimote]\n";
    for (int i = 0; i < kRemotes; i++) {
        const auto& rc = sCfg.remotes[i];
        out << "remote" << i + 1 << "_connect=" << connectKey(rc.connect) << '\n'
            << "remote" << i + 1 << "_extension=" << (rc.nunchuk ? "nunchuk" : "none") << '\n'
            << "remote" << i + 1 << "_pointer=" << pointerKey(rc.pointer) << '\n'
            << "remote" << i + 1 << "_snap=" << snapKey(rc.snap) << '\n';
    }
    out << "pointer_stick_speed=" << sCfg.stickSpeed << '\n'
        << "pointer_stick_absolute=" << (sCfg.stickAbsolute ? "true" : "false") << '\n'
        << "gyro_sensitivity=" << sCfg.gyroSensitivity << '\n'
        << "gyro_invert_x=" << (sCfg.gyroInvertX ? "true" : "false") << '\n'
        << "gyro_invert_y=" << (sCfg.gyroInvertY ? "true" : "false") << '\n'
        << "hide_cursor=" << (sCfg.hideCursor ? "true" : "false") << '\n'
        << "pointer_timeout=" << sCfg.pointerTimeout << '\n'
        << "distance=" << sCfg.distance << '\n';
}

const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

// ---- Mouse (host thread) -------------------------------------------------------------

struct Mouse {
    float x = 0, y = 0, w = 1, h = 1;  // window coordinates
    bool inside = false;
    u32 seq = 0;  // bumped on every move
};
std::mutex sMouseMutex;
Mouse sMouse;

// Window position -> KPAD pointer. The game image fills the window (Aurora's stretch viewport);
// with widescreen on, the game's 2D (where the cursor is drawn) is kept at the game's aspect in
// the middle (widescreen.h), so x is scaled back by that factor.
void mouseToPointer(const Mouse& m, float& px, float& py) {
    px = m.x / std::max(m.w, 1.0f) * 2.0f - 1.0f;
    py = m.y / std::max(m.h, 1.0f) * 2.0f - 1.0f;
    if (ws::active()) px /= ws::factor();
}

// ---- Script (--input) ------------------------------------------------------------------

struct ScriptItem {
    int port = 0;
    u32 start = 0, frames = 6;
    std::string anchor;  // E<module>.<n>.<offset> items: start = offset after that event (see eventTime)
    u32 anchorCount = 0;
    u32 buttons = 0;  // kBtn*
    float sx = 0, sy = 0;
    bool pointer = false, pointerOff = false;
    float px = 0, py = 0;
    std::string action;  // TP=... (test aid actions, takeScriptActions)
};

std::vector<ScriptItem> parseScript(const std::string& script) {
    std::vector<ScriptItem> out;
    std::stringstream ss(script);
    std::string item;
    while (std::getline(ss, item, ',')) {
        ScriptItem p;
        if (item.size() > 3 && (item[0] == 'P' || item[0] == 'p') && item[1] >= '1' && item[1] <= '4' && item[2] == ':') {
            p.port = item[1] - '1';
            item.erase(0, 3);
        }
        const auto colon = item.find(':');
        if (colon == std::string::npos) continue;
        if (item[0] == 'E') {  // E<module>.<n>.<offset> (route `after link`)
            const auto d2 = item.rfind('.', colon), d1 = item.rfind('.', d2 - 1);
            if (d1 == std::string::npos || d2 == std::string::npos || d1 < 1) continue;
            p.anchor = item.substr(1, d1 - 1);
            p.anchorCount = u32(std::strtoul(item.c_str() + d1 + 1, nullptr, 10));
            p.start = u32(std::strtoul(item.c_str() + d2 + 1, nullptr, 10));
        } else {
            p.start = u32(std::strtoul(item.c_str(), nullptr, 10));
        }
        std::string rest = item.substr(colon + 1);
        if (const auto star = rest.find('*'); star != std::string::npos) {
            p.frames = u32(std::strtoul(rest.c_str() + star + 1, nullptr, 10));
            rest.resize(star);
        }
        std::stringstream bs(rest);
        std::string b;
        while (std::getline(bs, b, '+')) {
            if (b.empty()) continue;
            if (b[0] == '@') {  // @x/y pointer position (KPAD units), @off = pointing away
                p.pointer = true;
                if (b == "@off") {
                    p.pointerOff = true;
                } else if (std::sscanf(b.c_str() + 1, "%f/%f", &p.px, &p.py) != 2) {
                    p.pointer = false;
                }
                continue;
            }
            static const std::pair<const char*, u32> kNames[] = {
                {"A", kBtnA}, {"B", kBtnB}, {"1", kBtn1}, {"2", kBtn2}, {"X", kBtn1}, {"Y", kBtn2},
                {"PLUS", kBtnPlus}, {"START", kBtnPlus}, {"MINUS", kBtnMinus}, {"HOME", kBtnHome},
                {"UP", kBtnUp}, {"DOWN", kBtnDown}, {"LEFT", kBtnLeft}, {"RIGHT", kBtnRight},
                {"C", kBtnC}, {"Z", kBtnZ}, {"SHAKE", kBtnShake}, {"NSHAKE", kBtnShakeNunchuk}, {"RECENTER", kBtnRecenter}};
            for (const auto& [name, bit] : kNames)
                if (b == name) p.buttons |= bit;
            if (b.rfind("TP=", 0) == 0) {  // TP=<target>: player teleport (cf_region.cpp, CF_TELEPORT syntax, '/' for ',')
                p.action = b;
                p.frames = 1;
                continue;
            }
            if (b.rfind("STICK=", 0) == 0) {  // STICK=x/y: Nunchuk stick at any position (-1..1, up +)
                std::sscanf(b.c_str() + 6, "%f/%f", &p.sx, &p.sy);
                continue;
            }
            if (b == "STICKUP") p.sy = 1;
            else if (b == "STICKDOWN") p.sy = -1;
            else if (b == "STICKLEFT") p.sx = -1;
            else if (b == "STICKRIGHT") p.sx = 1;
        }
        out.push_back(p);
    }
    return out;
}

const std::vector<ScriptItem>& script() {
    static const std::vector<ScriptItem> s = parseScript(port::options().inputScript);
    return s;
}

u32 scriptClock() {
    static const bool byFrames = [] {
        const char* v = std::getenv("CF_INPUT_CLOCK");
        return v && std::string(v) == "frames";
    }();
    return byFrames ? game::guestFrames() : viRetraceCount();
}

// Script anchors (route `after link <module>`): the script clock at each time a REL module was
// linked (rel.cpp calls noteScriptEvent), so items can follow scene changes whose frame varies.
std::mutex sEventMutex;
std::map<std::string, std::vector<u32>> sEvents;

// The item's start in script clock, or false while its anchor event has not happened yet.
bool itemStart(const ScriptItem& p, u32& start) {
    if (p.anchor.empty()) {
        start = p.start;
        return true;
    }
    std::lock_guard lk(sEventMutex);
    const auto it = sEvents.find(p.anchor);
    if (it == sEvents.end() || it->second.size() < p.anchorCount || p.anchorCount == 0) return false;
    start = it->second[p.anchorCount - 1] + p.start;
    return true;
}

bool scriptUses(int port) {
    for (const auto& p : script())
        if (p.port == port) return true;
    return false;
}

// ---- Emulated source -----------------------------------------------------------------

struct PortState {
    bool initialized = false;
    float px = 0, py = 0;       // pointer (stick/gyro integrate here, the mouse sets it)
    bool valid = true;
    enum Last : u8 { None, MouseDev, StickDev, GyroDev } last = None;
    u32 mouseSeq = 0;
    double lastTime = 0;
    bool gyroOn = false;
    SDL_Gamepad* gyroPad = nullptr;
    bool scriptPointer = false, scriptOff = false;
    float scriptX = 0, scriptY = 0;
    const ScriptItem* scriptItem = nullptr;  // the scripted pointer position applied last
    bool snapped = false;       // the menu snap placed the pointer this sample
    double lastMove = 0;        // when the pointer last moved (or a pointer screen was up): timeout
    bool timedOut = false;
    State lastState;            // for the input test (menu)
};
std::array<PortState, kRemotes> sPort;
std::mutex sStateMutex;  // lastState (menu reads it)

std::array<controls::RemoteInputs, kRemotes> sInputs;
std::array<bool, kRemotes> sDriven{};
u64 sLastPoll = ~0ull;

void poll() {
    const u64 now = timeBase();
    if (sLastPoll != ~0ull && now - sLastPoll < u64(kTimerClock / 1000.0)) return;
    sLastPoll = now;
    PADStatus st[PAD_MAX_CONTROLLERS] = {};
    readHostPads(st);  // device assignment, bindings, menu suppression
    for (int i = 0; i < kRemotes; i++) sDriven[i] = controls::remoteInputs(u32(i), sInputs[i]);
    bool snap = false;
    {
        std::lock_guard lk(sMutex);
        for (const auto& rc : sCfg.remotes) snap |= rc.snap != menunav::Mode::Off;
    }
    menunav::setWanted(snap);
}

// The pointer timeout in seconds (0: never). Test runs (--input) keep the pointer, so existing
// routes behave as before; CF_POINTER_TIMEOUT=<s> sets it for any run (also scripted ones).
const char* const sTimeoutEnv = std::getenv("CF_POINTER_TIMEOUT");
const bool sTimeoutLog = sTimeoutEnv != nullptr || std::getenv("CF_WPAD_LOG") != nullptr;

float pointerTimeout(const Config& cfg) {
    if (sTimeoutEnv && *sTimeoutEnv) return std::max(0.0f, float(std::atof(sTimeoutEnv)));
    return script().empty() ? cfg.pointerTimeout : 0.0f;
}

class EmulatedSource final : public Source {
public:
    const char* kind() const override { return "emulated"; }

    void read(int chan, double now, State& st) override {
        poll();
        RemoteConfig rc;
        Config cfg;
        {
            std::lock_guard lk(sMutex);
            rc = sCfg.remotes[chan];
            cfg = sCfg;
        }
        const auto& in = sInputs[chan];
        const bool menuOpen = menu::isOpen();
        st = State{};
        st.connected = rc.connect == Connect::Always || (rc.connect == Connect::Auto && (sDriven[chan] || scriptUses(chan)));
        if (!st.connected) return;
        st.ext = rc.nunchuk ? kDevNunchuk : kDevCore;
        st.dist = cfg.distance;
        u32 buttons = menuOpen ? 0 : in.buttons;
        float sx = menuOpen ? 0 : in.stickX, sy = menuOpen ? 0 : in.stickY;
        float ptrX = menuOpen ? 0 : in.ptrX, ptrY = menuOpen ? 0 : in.ptrY;

        auto& ps = sPort[chan];
        const double dt = ps.initialized ? std::clamp(now - ps.lastTime, 0.0, 0.1) : 0.0;
        ps.lastTime = now;
        if (!ps.initialized) {
            ps.initialized = true;
            ps.px = ps.py = 0;
            ps.lastMove = now;
        }

        // Script (test runs): buttons and stick add to the bound inputs; a scripted pointer
        // position stays until the next one.
        bool pointerMoved = false;  // a free pointer device (or a scripted position) moved it
        if (!script().empty()) {
            const u32 clk = scriptClock();
            const ScriptItem* latest = nullptr;
            u32 latestStart = 0;
            for (const auto& p : script()) {
                u32 start = 0;
                if (p.port != chan || !itemStart(p, start) || clk < start) continue;
                if (p.pointer && (!latest || start >= latestStart)) {  // the latest started pointer item wins and persists
                    latest = &p;
                    latestStart = start;
                }
                if (clk >= start + p.frames) continue;
                buttons |= p.buttons;
                if (p.sx) sx = p.sx;
                if (p.sy) sy = p.sy;
            }
            if (latest && latest != ps.scriptItem) {
                ps.scriptItem = latest;
                ps.scriptPointer = true;
                ps.scriptOff = latest->pointerOff;
                ps.scriptX = latest->px;
                ps.scriptY = latest->py;
                pointerMoved = true;
            }
        }

        // Pointer devices.
        if (!menuOpen) {
            if (rc.pointer & kPtrMouse) {
                Mouse m;
                {
                    std::lock_guard lk(sMouseMutex);
                    m = sMouse;
                }
                if (m.seq != ps.mouseSeq) {
                    ps.mouseSeq = m.seq;
                    ps.last = PortState::MouseDev;
                    pointerMoved = true;
                }
                if (ps.last == PortState::MouseDev) {
                    mouseToPointer(m, ps.px, ps.py);
                    ps.valid = m.inside && std::fabs(ps.px) <= 1.1f && std::fabs(ps.py) <= 1.1f;
                }
            }
            if (rc.pointer & kPtrStick) {
                const float m = std::sqrt(ptrX * ptrX + ptrY * ptrY);
                if (cfg.stickAbsolute) {
                    if (m > 0.02f) pointerMoved = true;
                    // While the snap holds the pointer on an item, a centred stick does not take it back.
                    if (m > 0.02f || (ps.last == PortState::StickDev && !ps.snapped)) {
                        ps.last = PortState::StickDev;
                        ps.px = ptrX;
                        ps.py = -ptrY;
                        ps.valid = true;
                    }
                } else if (m > 0.02f) {
                    pointerMoved = true;
                    ps.last = PortState::StickDev;
                    // Speed grows with the square of the tilt: fine aim near the centre.
                    ps.px += float(ptrX * m * cfg.stickSpeed * dt);
                    ps.py -= float(ptrY * m * cfg.stickSpeed * dt);
                    ps.valid = true;
                }
            }
            if (rc.pointer & kPtrGyro) {
                SDL_Gamepad* g = controls::portGamepad(u32(chan));
                if (g != ps.gyroPad) {
                    ps.gyroPad = g;
                    ps.gyroOn = g && SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) &&
                                SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
                    if (g) log("[wiimote] remote %d: gyro pointer %s", chan + 1, ps.gyroOn ? "on" : "not available on this controller");
                }
                float rate[3] = {};
                if (ps.gyroOn && SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, rate, 3)) {
                    // SDL: rad/s, x = pitch (up +), y = yaw (left +).
                    const float yaw = rate[1] * (cfg.gyroInvertX ? -1.0f : 1.0f);
                    const float pitch = rate[0] * (cfg.gyroInvertY ? -1.0f : 1.0f);
                    if (std::fabs(yaw) + std::fabs(pitch) > 0.03f) {
                        pointerMoved = true;
                        ps.last = PortState::GyroDev;
                        ps.px -= float(yaw * cfg.gyroSensitivity * dt);
                        ps.py -= float(pitch * cfg.gyroSensitivity * dt);
                        ps.valid = true;
                    }
                }
            }
            // Snap pointer to menu items (menu_nav.h): on a pointer-driven screen the D-pad and
            // the Nunchuk stick move the pointer between the screen's items.
            {
                menunav::Input ni;
                ni.mode = rc.snap;
                ni.mouseDrives = ps.last == PortState::MouseDev && !ps.scriptPointer;
                ni.pointerMoved = pointerMoved;
                ni.buttons = buttons;
                ni.stickX = sx;
                ni.stickY = sy;
                ni.px = ps.scriptPointer ? ps.scriptX : ps.px;
                ni.py = ps.scriptPointer ? ps.scriptY : ps.py;
                ni.now = now;
                const menunav::Output no = menunav::apply(chan, ni);
                buttons = no.buttons;
                sx = no.stickX;
                sy = no.stickY;
                ps.snapped = no.setPointer;
                if (no.setPointer) {
                    ps.px = no.px;
                    ps.py = no.py;
                    ps.valid = true;
                    if (ps.last == PortState::MouseDev || ps.last == PortState::None) ps.last = PortState::StickDev;
                    if (ps.scriptPointer) {
                        ps.scriptOff = false;
                        ps.scriptX = no.px;
                        ps.scriptY = no.py;
                    }
                }
            }
            if (buttons & kBtnRecenter) {
                ps.px = ps.py = 0;
                ps.valid = true;
                pointerMoved = true;
            }
            if (ps.last != PortState::MouseDev) {
                ps.px = std::clamp(ps.px, -1.0f, 1.0f);
                ps.py = std::clamp(ps.py, -1.0f, 1.0f);
            }
        }
        if (ps.scriptPointer) {
            st.pointerValid = !ps.scriptOff;
            st.px = ps.scriptX;
            st.py = ps.scriptY;
        } else {
            st.pointerValid = rc.pointer != 0 && ps.last != PortState::None && ps.valid;
            if (rc.pointer && ps.last == PortState::None && rc.pointer != kPtrMouse) st.pointerValid = true;  // centred
            st.px = ps.px;
            st.py = ps.py;
        }
        {
            // Pointer timeout: a pointer left alone goes off the screen, as a Remote put down or
            // pointed away does, so the field controls are the pointer-less ones. Never while
            // a pointer screen is up (it needs the pointer); moving it brings it back where it was.
            const float timeout = pointerTimeout(cfg);
            if (pointerMoved || ps.snapped || menuOpen || menunav::menuUp()) ps.lastMove = now;
            const bool out = timeout > 0 && st.pointerValid && now - ps.lastMove > timeout;
            if (out != ps.timedOut) {
                ps.timedOut = out;
                if (sTimeoutLog) log("[wiimote] remote %d: pointer %s", chan + 1, out ? "timed out (hidden)" : "back");
            }
            if (out) st.pointerValid = false;
        }

        // Motion: at rest, or swung back and forth (about 7 Hz, +-2.5 g) while a shake input is held.
        const float t = float(now);
        if (buttons & kBtnShake) {
            st.acc = {2.5f * std::sin(2 * kPi * 7.0f * t), -1.0f + 2.0f * std::cos(2 * kPi * 7.0f * t), 0.5f};
        }
        if (st.ext == kDevNunchuk) {
            st.stickX = sx;
            st.stickY = sy;
            if (buttons & kBtnShakeNunchuk)
                st.nunAcc = {2.5f * std::sin(2 * kPi * 7.0f * t), -1.0f + 2.0f * std::cos(2 * kPi * 7.0f * t), 0.5f};
        } else {
            buttons &= ~(kBtnC | kBtnZ);
        }
        st.buttons = buttons & kBtnGameMask;
    }

    void rumble(int chan, bool on) override { controls::controlMotor(u32(chan), on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP); }
};

EmulatedSource sEmulated;

Source& sourceFor(int chan) {
    // A port set to "Real Wii Remote" (wiimote_hid.cpp), else the emulated source.
    if (Source* real = hidSource(chan)) return *real;
    return sEmulated;
}

const bool sLog = [] {
    const char* v = std::getenv("CF_WPAD_LOG");
    return v && *v && *v != '0';
}();

}  // namespace

void noteScriptEvent(const std::string& name) {
    if (script().empty()) return;
    const u32 clk = scriptClock();
    std::lock_guard lk(sEventMutex);
    auto& v = sEvents[name];
    v.push_back(clk);
    if (std::getenv("CF_INPUT_LOG")) log("[input] event %s #%zu at %u", name.c_str(), v.size(), clk);
}

std::vector<std::string> takeScriptActions() {
    std::vector<std::string> out;
    if (script().empty()) return out;
    static std::mutex m;
    static std::vector<bool> fired;
    std::lock_guard lk(m);
    const auto& s = script();
    fired.resize(s.size());
    const u32 clk = scriptClock();
    for (size_t i = 0; i < s.size(); i++) {
        u32 start = 0;
        if (s[i].action.empty() || fired[i] || !itemStart(s[i], start) || clk < start) continue;
        fired[i] = true;
        out.push_back(s[i].action);
        if (std::getenv("CF_INPUT_LOG")) log("[input] action %s at %u", s[i].action.c_str(), clk);
    }
    return out;
}

void sample(int chan, State& out) {
    out = State{};
    if (chan < 0 || chan >= kRemotes) return;
    const double now = double(timeBase()) / kTimerClock;
    sourceFor(chan).read(chan, now, out);
    menunav::notePointer(chan, out.connected && out.pointerValid, out.px, out.py);
    {
        std::lock_guard lk(sStateMutex);
        State& last = sPort[chan].lastState;
        if (sLog && (last.connected != out.connected || last.buttons != out.buttons || last.ext != out.ext ||
                     last.pointerValid != out.pointerValid || std::fabs(last.px - out.px) > 0.05f ||
                     std::fabs(last.py - out.py) > 0.05f || std::fabs(last.stickX - out.stickX) > 0.1f ||
                     std::fabs(last.stickY - out.stickY) > 0.1f))
            log("[wpad] remote %d: %s ext %u buttons %04X pointer %s %.2f,%.2f stick %.2f,%.2f (retrace %u)", chan + 1,
                out.connected ? "connected" : "not connected", out.ext, out.buttons, out.pointerValid ? "on" : "off",
                out.px, out.py, out.stickX, out.stickY, viRetraceCount());
        last = out;
    }
}

void rumble(int chan, bool on) {
    if (chan >= 0 && chan < kRemotes) sourceFor(chan).rumble(chan, on);
}

void speakerControl(int chan, int cmd) {
    if (chan >= 0 && chan < kRemotes) sourceFor(chan).speakerControl(chan, cmd);
}

void speakerData(int chan, const u8* data, u32 len) {
    if (chan >= 0 && chan < kRemotes && data && len) sourceFor(chan).speakerData(chan, data, len);
}

void handleEvent(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        int w = 1, h = 1;
        if (SDL_Window* win = aurora_get_window()) SDL_GetWindowSize(win, &w, &h);
        std::lock_guard lk(sMouseMutex);
        sMouse.x = ev.motion.x;
        sMouse.y = ev.motion.y;
        sMouse.w = float(std::max(w, 1));
        sMouse.h = float(std::max(h, 1));
        sMouse.inside = ev.motion.x >= 0 && ev.motion.y >= 0 && ev.motion.x < w && ev.motion.y < h;
        sMouse.seq++;
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: {
        std::lock_guard lk(sMouseMutex);
        sMouse.inside = false;
        sMouse.seq++;
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_ENTER: {
        std::lock_guard lk(sMouseMutex);
        sMouse.inside = true;
        break;
    }
    default: break;
    }
}

void hostFrame() {
    bool mousePoints = false, hide = false;
    {
        std::lock_guard lk(sMutex);
        hide = sCfg.hideCursor;
        for (int i = 0; i < kRemotes; i++) {
            std::lock_guard lk2(sStateMutex);
            if ((sCfg.remotes[i].pointer & kPtrMouse) && sPort[i].lastState.connected) mousePoints = true;
        }
    }
    bool inside;
    {
        std::lock_guard lk(sMouseMutex);
        inside = sMouse.inside;
    }
    // The game draws its own pointer: the OS cursor over it would be a second one.
    if (hide && mousePoints && inside && !menu::isOpen() && !ImGui::GetIO().WantCaptureMouse)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
}

// ---- Menu ----------------------------------------------------------------------------

bool drawRemotesSection() {
    bool changed = false;
    std::lock_guard lk(sMutex);
    ImGui::SeparatorText("Wii Remotes");
    if (ImGui::BeginTable("remotes", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        const float k = recomp::ui::scale();
        ImGui::TableSetupColumn("Remote", ImGuiTableColumnFlags_WidthFixed, 50 * k);
        ImGui::TableSetupColumn("Connected", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Extension", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Pointer", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kRemotes; i++) {
            auto& rc = sCfg.remotes[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", i + 1);
            ImGui::TableNextColumn();
            static const char* kConnect[] = {"Auto", "Always", "Never"};
            int ci = int(rc.connect);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##connect", &ci, kConnect, 3)) {
                rc.connect = Connect(ci);
                changed = true;
            }
            ImGui::SetItemTooltip("Auto: the Remote is connected while a controller or the keyboard drives this port\n"
                                  "(above). Always / Never: connected or switched off regardless. The game notices a\n"
                                  "Remote connecting or disconnecting as on a Wii.");
            ImGui::TableNextColumn();
            static const char* kExt[] = {"Nunchuk", "None"};
            int ei = rc.nunchuk ? 0 : 1;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##ext", &ei, kExt, 2)) {
                rc.nunchuk = ei == 0;
                changed = true;
            }
            ImGui::SetItemTooltip("What is plugged into the Remote. City Folk plays with the Remote alone (held\n"
                                  "upright) or with a Nunchuk (stick to walk).");
            ImGui::TableNextColumn();
            static const std::pair<u8, const char*> kPtr[] = {
                {kPtrMouse | kPtrStick, "Mouse + right stick"}, {kPtrMouse, "Mouse"}, {kPtrStick, "Right stick"},
                {kPtrGyro | kPtrStick, "Gyro + right stick"}, {kPtrGyro, "Gyro"},
                {kPtrMouse | kPtrGyro | kPtrStick, "Mouse + gyro + stick"}, {0, "None"}};
            const char* cur = "Custom";
            for (const auto& [bits, name] : kPtr)
                if (bits == rc.pointer) cur = name;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##pointer", cur)) {
                for (const auto& [bits, name] : kPtr)
                    if (ImGui::Selectable(name, bits == rc.pointer)) {
                        rc.pointer = bits;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("What aims the Remote at the screen. Mouse: the pointer is where the mouse is over\n"
                                  "the game. Right stick: moves the pointer (also the pointer keys of the bindings).\n"
                                  "Gyro: turning a controller with a gyroscope aims like a Remote. With several, the\n"
                                  "one used last places the pointer.");
            ImGui::TableNextColumn();
            State s;
            {
                std::lock_guard lk2(sStateMutex);
                s = sPort[i].lastState;
            }
            const std::string dev = controls::portDeviceName(u32(i));
            if (s.connected)
                ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "Connected%s%s", dev.empty() ? "" : ": ", dev.c_str());
            else
                ImGui::TextDisabled("%s", rc.connect == Connect::Never ? "Off" : "Not connected");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    changed |= drawHidPortsSection();
    return changed;
}

bool drawPointerTab() {
    bool changed = false;
    std::lock_guard lk(sMutex);
    ImGui::TextWrapped("How the Wii Remote's pointer and motion are emulated. Each Remote's pointer devices are "
                       "chosen in the Ports tab.");
    ImGui::SeparatorText("Snap pointer to menu items");
    ImGui::TextWrapped("Play menus with a normal controller: while a pointer screen is up (pockets, map, shops, "
                       "dialogue choices, keyboards, ...), the D-pad or the left stick moves the pointer from item to "
                       "item and A clicks. The right stick still moves the pointer freely.");
    for (int i = 0; i < kRemotes; i++) {
        static const char* kSnap[] = {"Auto", "On", "Off"};
        auto& rc = sCfg.remotes[i];
        int si = rc.snap == menunav::Mode::On ? 1 : rc.snap == menunav::Mode::Off ? 2 : 0;
        char label[48];
        std::snprintf(label, sizeof label, "Wii Remote %d##snap%d", i + 1, i);
        ImGui::SetNextItemWidth(140 * recomp::ui::scale());
        if (ImGui::Combo(label, &si, kSnap, 3)) {
            rc.snap = si == 1 ? menunav::Mode::On : si == 2 ? menunav::Mode::Off : menunav::Mode::Auto;
            changed = true;
        }
        ImGui::SetItemTooltip("Auto: snaps while the pointer is not placed by the mouse (a controller's D-pad, left\n"
                              "stick, right stick or gyro). Moving the mouse turns it off until a controller moves\n"
                              "the pointer again, so playing with a mouse stays as on a Wii. On: also with the mouse.\n"
                              "Off: the original pointer-only menus; the D-pad and stick always reach the game.\n"
                              "On a menu the D-pad / left stick do not reach the game; in the field they keep\n"
                              "walking and changing tools: \"Recenter pointer\" (right stick press) puts the pointer\n"
                              "on the toolbar there (D-pad left/right, A to open, B to leave).");
    }
    ImGui::SeparatorText("Mouse");
    changed |= recomp::menu::Checkbox("Hide the mouse cursor over the game", &sCfg.hideCursor);
    ImGui::SetItemTooltip("The game draws its own pointer (the hand); the system cursor is hidden while it\n"
                          "points with the mouse. Shown again over this menu.");
    ImGui::SeparatorText("Pointer timeout");
    {
        const float k = recomp::ui::scale();
        ImGui::SetNextItemWidth(200 * k);
        changed |= recomp::menu::SliderFloat("Hide the pointer after", &sCfg.pointerTimeout, 0.0f, 30.0f,
                                             sCfg.pointerTimeout <= 0 ? "Never" : "%.0f s", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("The pointer leaves the screen after this many seconds without moving (mouse, right\n"
                              "stick or gyro), as when a Wii Remote is pointed away: the game then uses its\n"
                              "controls without the pointer. Moving the pointer brings it back where it was.\n"
                              "Pointer screens (pockets, map, shops, dialogue choices) keep it. 0 = never (the\n"
                              "pointer stays until the mouse leaves the window). Real Wii Remotes are not affected.");
    }
    ImGui::SeparatorText("Right stick");
    changed |= recomp::menu::SliderFloat("Pointer speed", &sCfg.stickSpeed, 0.2f, 5.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How fast a fully tilted right stick moves the pointer (half the screen per second at 1).");
    changed |= recomp::menu::Checkbox("Absolute stick pointer", &sCfg.stickAbsolute);
    ImGui::SetItemTooltip("The stick's position is the pointer's (the pointer returns to the centre when the stick\n"
                          "is let go). Off: the stick moves the pointer, which stays where it is left.");
    ImGui::SeparatorText("Gyro");
    changed |= recomp::menu::SliderFloat("Gyro sensitivity", &sCfg.gyroSensitivity, 0.5f, 10.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How far turning the controller moves the pointer (half-screens per radian: at 3 about\n"
                          "20 degrees cross half the screen). Controllers with a gyroscope: DualShock 4,\n"
                          "DualSense, Switch Pro, Joy-Con, Steam Deck/Controller.");
    changed |= recomp::menu::Checkbox("Invert gyro X", &sCfg.gyroInvertX);
    ImGui::SameLine();
    changed |= recomp::menu::Checkbox("Invert gyro Y", &sCfg.gyroInvertY);
    ImGui::TextDisabled("The \"Recenter pointer\" binding puts a stick or gyro pointer back in the middle.");
    ImGui::SeparatorText("Sensor bar");
    changed |= recomp::menu::SliderFloat("Distance (m)", &sCfg.distance, 0.5f, 5.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("How far the emulated Remote is from the sensor bar, as the game reads it.");
    changed |= drawHidPointerSection();
    return changed;
}

void drawInputTest(int chan) {
    if (chan < 0 || chan >= kRemotes) return;
    State s;
    {
        std::lock_guard lk(sStateMutex);
        s = sPort[chan].lastState;
    }
    if (!s.connected) {
        ImGui::TextDisabled("Not connected (or the game has not read the Wii Remotes yet).");
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float k = recomp::ui::scale();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImU32 rim = ImGui::GetColorU32(ImGuiCol_Border), dot = IM_COL32(120, 220, 120, 255),
                text = ImGui::GetColorU32(ImGuiCol_Text);
    // The screen with the pointer.
    const float sw = 120 * k, sh = 68 * k;
    dl->AddRect(o, ImVec2(o.x + sw, o.y + sh), rim);
    if (s.pointerValid) {
        const ImVec2 p(o.x + (s.px + 1) * 0.5f * sw, o.y + (s.py + 1) * 0.5f * sh);
        dl->AddCircleFilled(p, 4 * k, dot);
    }
    dl->AddText(ImVec2(o.x, o.y + sh + 4 * k), text, s.pointerValid ? "Pointer" : "Pointer (off screen)");
    // Nunchuk stick.
    const float r = 30 * k;
    const ImVec2 c(o.x + sw + 30 * k + r, o.y + r + 2 * k);
    if (s.ext == kDevNunchuk) {
        dl->AddCircle(c, r, rim, 32, 1.5f * k);
        dl->AddCircleFilled(ImVec2(c.x + s.stickX * r, c.y - s.stickY * r), 5 * k, dot);
        dl->AddText(ImVec2(c.x - ImGui::CalcTextSize("Nunchuk").x / 2, c.y + r + 4 * k), text, "Nunchuk");
    }
    // Buttons.
    static const std::pair<u32, const char*> kBtns[] = {
        {kBtnA, "A"}, {kBtnB, "B"}, {kBtn1, "1"}, {kBtn2, "2"}, {kBtnPlus, "+"}, {kBtnMinus, "-"}, {kBtnHome, "HOME"},
        {kBtnUp, "Up"}, {kBtnDown, "Down"}, {kBtnLeft, "Left"}, {kBtnRight, "Right"}, {kBtnC, "C"}, {kBtnZ, "Z"}};
    const float bx = o.x + sw + 80 * k + 2 * r;
    float x = bx, y = o.y;
    for (const auto& [bit, name] : kBtns) {
        const ImVec2 sz = ImGui::CalcTextSize(name);
        const ImVec2 a(x, y), b(a.x + sz.x + 10 * k, a.y + sz.y + 4 * k);
        if (s.buttons & bit) dl->AddRectFilled(a, b, dot, 3 * k);
        dl->AddRect(a, b, rim, 3 * k);
        dl->AddText(ImVec2(a.x + 5 * k, a.y + 2 * k), text, name);
        x += sz.x + 16 * k;
        if (bit == kBtnHome || bit == kBtnRight) {
            x = bx;
            y += sz.y + 10 * k;
        }
    }
    ImGui::Dummy(ImVec2(0, std::max(sh + 20 * k, 2 * r + 26 * k)));
    ImGui::TextDisabled("buttons %04X  pointer %.2f, %.2f  acc %.1f %.1f %.1f  stick %.2f, %.2f  %s", s.buttons, s.px, s.py,
                        s.acc.x, s.acc.y, s.acc.z, s.stickX, s.stickY, s.ext == kDevNunchuk ? "Nunchuk" : "no extension");
}

}  // namespace recomp::wiimote
