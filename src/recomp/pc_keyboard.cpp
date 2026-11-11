// F1 > Controls "Type with the PC keyboard" (docs/CONTROLS.md "Typing with the PC keyboard",
// docs/ONLINE.md 10b): the PC keyboard becomes the Wii's USB keyboard. City Folk's KBD library
// (fn_801F9CAC init, opened once at boot) takes text from it in every software keyboard (chat,
// letters, names, design names, ...), and the Wii Remote key bindings are dropped meanwhile
// (controls.cpp push) so typing never also walks or presses buttons. The mouse and controllers
// keep working.
//
// Key presses come from the SDL events of the host loop (menu::handleEvent), not from the
// keyboard state once a frame: every press and release becomes its own HID report, in order, so
// fast typing (a key down and up within one frame, overlapping keys) is not lost. The KBD library
// turns reports into characters (shift, caps lock, its own key repeat), as with a real keyboard.
// F1 never reaches the game, and nothing does while the menu is open (keys held then are released).
//
// Test aids:
//   CF_KBD_TYPE="<guest frame>:<text>|..."  types text through SDL's event queue (SDL_PushEvent:
//       the same path as a real key: Aurora's input router, then this file), each key 3 host
//       frames down and 3 up. Text: letters (capitals with shift), digits, space, .,!?'-;:/
//       and {ENTER} {BACK} {ESC} {TAB} {UP} {DOWN} {LEFT} {RIGHT} {CAPS} {F1}, {HOLD:<key>:<frames>}
//       (e.g. {HOLD:BACK:60} holds Backspace for 60 host frames, key repeat), {WAIT:<frames>}.
//       Works with the option off too (then the keys are plain key presses: bindings act).
//   {MOUSE1} clicks the left mouse button (window centre).
//   Route token KEYS=<text> does the same from an --input script ('_' = space; scriptType).
//   CF_KBD_LOG=1  every report sent to the game, attach / detach, Wii Remote 1's bound buttons
//       and stick on every change.
#include "recomp/pc_keyboard.h"

#include "port/settings.h"
#include "recomp/controls.h"
#include "recomp/game.h"
#include "recomp/ios.h"
#include "recomp/menu.h"

#include <aurora/aurora.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace recomp::pckbd {
namespace {

const bool sLog = [] {
    const char* v = std::getenv("CF_KBD_LOG");
    return v && *v && *v != '0';
}();
const bool sForceOn = std::getenv("CF_USB_KBD_DEVICE") != nullptr;  // older test aid name: option on

std::vector<u8> sDown;  // HID usages reported down, in press order (modifiers included)
bool sAttached = false;

void sendReport() {
    u8 rep[8] = {};
    int n = 0;
    for (const u8 sc : sDown) {
        if (sc >= 0xE0 && sc <= 0xE7) rep[0] |= u8(1u << (sc - 0xE0));  // HID modifier bits
        else if (n < 6) rep[2 + n++] = sc;                              // SDL scancodes are HID usages
    }
    const bool sent = ios::usbKeyboardReport(rep);
    if (sLog)
        std::printf("[kbd] report %02X %02X %02X %02X %02X %02X %02X%s\n", rep[0], rep[2], rep[3], rep[4], rep[5], rep[6],
                    rep[7], sent ? "" : " (no keyboard open yet: dropped)");
}

void releaseAll() {
    if (sDown.empty()) return;
    sDown.clear();
    sendReport();
}

// ---- CF_KBD_TYPE ----------------------------------------------------------------------

constexpr int kMouseLeft = 0x1000;  // {MOUSE1}: the left mouse button (Wii Remote A by default)

struct Key {
    int sc = 0;
    bool shift = false;
    int hold = 3;  // host frames down
    int wait = 0;  // only a pause of this many host frames (no key)
};
struct Item {
    unsigned frame;
    std::vector<Key> keys;
};

bool keyFor(char ch, Key& k) {
    k.shift = false;
    if (ch >= 'a' && ch <= 'z') k.sc = SDL_SCANCODE_A + (ch - 'a');
    else if (ch >= 'A' && ch <= 'Z') { k.sc = SDL_SCANCODE_A + (ch - 'A'); k.shift = true; }
    else if (ch >= '1' && ch <= '9') k.sc = SDL_SCANCODE_1 + (ch - '1');
    else if (ch == '0') k.sc = SDL_SCANCODE_0;
    else if (ch == ' ') k.sc = SDL_SCANCODE_SPACE;
    else if (ch == '-') k.sc = SDL_SCANCODE_MINUS;
    else if (ch == '\'') k.sc = SDL_SCANCODE_APOSTROPHE;
    else if (ch == ',') k.sc = SDL_SCANCODE_COMMA;
    else if (ch == '.') k.sc = SDL_SCANCODE_PERIOD;
    else if (ch == ';') k.sc = SDL_SCANCODE_SEMICOLON;
    else if (ch == '/') k.sc = SDL_SCANCODE_SLASH;
    else if (ch == ':') { k.sc = SDL_SCANCODE_SEMICOLON; k.shift = true; }
    else if (ch == '!') { k.sc = SDL_SCANCODE_1; k.shift = true; }
    else if (ch == '?') { k.sc = SDL_SCANCODE_SLASH; k.shift = true; }
    else return false;
    return true;
}

int namedKey(const std::string& n) {
    if (n == "ENTER") return SDL_SCANCODE_RETURN;
    if (n == "BACK") return SDL_SCANCODE_BACKSPACE;
    if (n == "ESC") return SDL_SCANCODE_ESCAPE;
    if (n == "TAB") return SDL_SCANCODE_TAB;
    if (n == "RIGHT") return SDL_SCANCODE_RIGHT;
    if (n == "LEFT") return SDL_SCANCODE_LEFT;
    if (n == "DOWN") return SDL_SCANCODE_DOWN;
    if (n == "UP") return SDL_SCANCODE_UP;
    if (n == "CAPS") return SDL_SCANCODE_CAPSLOCK;
    if (n == "F1") return SDL_SCANCODE_F1;
    if (n == "SPACE") return SDL_SCANCODE_SPACE;
    if (n == "MOUSE1") return kMouseLeft;
    if (n.size() == 1) {
        Key k;
        if (keyFor(char(std::tolower(static_cast<unsigned char>(n[0]))), k)) return k.sc;
    }
    return 0;
}

std::vector<Key> parseText(const std::string& text) {
    std::vector<Key> keys;
    for (size_t i = 0; i < text.size(); i++) {
        Key k;
        if (text[i] == '{') {
            const size_t z = text.find('}', i);
            if (z == std::string::npos) break;
            std::string name = text.substr(i + 1, z - i - 1);
            i = z;
            if (name.rfind("WAIT:", 0) == 0) {
                k.wait = std::max(1, std::atoi(name.c_str() + 5));
                keys.push_back(k);
                continue;
            }
            if (name.rfind("HOLD:", 0) == 0) {
                const size_t c2 = name.find(':', 5);
                k.hold = c2 == std::string::npos ? 60 : std::max(1, std::atoi(name.c_str() + c2 + 1));
                name = name.substr(5, c2 == std::string::npos ? std::string::npos : c2 - 5);
            }
            k.sc = namedKey(name);
            if (k.sc) keys.push_back(k);
        } else if (keyFor(text[i], k)) {
            keys.push_back(k);
        }
    }
    return keys;
}

std::vector<Item> parse() {
    std::vector<Item> out;
    const char* e = std::getenv("CF_KBD_TYPE");
    if (!e || !*e) return out;
    const std::string s = e;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find('|', p);
        if (q == std::string::npos) q = s.size();
        const std::string part = s.substr(p, q - p);
        p = q + 1;
        const size_t c = part.find(':');
        if (c == std::string::npos) continue;
        out.push_back({unsigned(std::strtoul(part.c_str(), nullptr, 10)), parseText(part.substr(c + 1))});
    }
    return out;
}

std::mutex sScriptMutex;
std::vector<Key> sScriptKeys;  // KEYS= route tokens (scriptType), taken by the host thread

void pushKey(int sc, bool down, bool shift) {
    SDL_Event ev{};
    SDL_Window* win = aurora_get_window();
    if (sc == kMouseLeft) {
        ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        ev.button.timestamp = SDL_GetTicksNS();
        ev.button.windowID = win ? SDL_GetWindowID(win) : 0;
        ev.button.which = 0;
        ev.button.button = SDL_BUTTON_LEFT;
        ev.button.down = down;
        ev.button.clicks = 1;
        int w = 0, h = 0;
        if (win) SDL_GetWindowSize(win, &w, &h);
        ev.button.x = float(w) * 0.5f;
        ev.button.y = float(h) * 0.5f;
        SDL_PushEvent(&ev);
        return;
    }
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.timestamp = SDL_GetTicksNS();
    ev.key.windowID = win ? SDL_GetWindowID(win) : 0;
    ev.key.which = 0;
    ev.key.scancode = SDL_Scancode(sc);
    ev.key.mod = shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
    ev.key.key = SDL_GetKeyFromScancode(SDL_Scancode(sc), ev.key.mod, false);
    ev.key.down = down;
    ev.key.repeat = false;
    SDL_PushEvent(&ev);
}

void scriptFrame() {
    static std::vector<Item> items = parse();
    static std::deque<Key> queue;
    static int phase = 0;  // host frames into the current key: [0, hold) down, then 3 up
    {
        std::lock_guard lk(sScriptMutex);
        for (const Key& k : sScriptKeys) queue.push_back(k);
        sScriptKeys.clear();
    }
    if (items.empty() && queue.empty()) return;
    const unsigned f = game::guestFrames();
    for (auto it = items.begin(); it != items.end();) {
        if (f >= it->frame) {
            std::printf("[kbd] frame %u: typing %zu keys (SDL events)\n", f, it->keys.size());
            std::fflush(stdout);
            for (auto& k : it->keys) queue.push_back(k);
            it = items.erase(it);
        } else {
            ++it;
        }
    }
    if (queue.empty()) return;
    const Key k = queue.front();
    if (k.wait) {
        if (++phase >= k.wait) {
            phase = 0;
            queue.pop_front();
        }
        return;
    }
    if (phase == 0) {
        if (k.shift) pushKey(SDL_SCANCODE_LSHIFT, true, true);
        pushKey(k.sc, true, k.shift);
    } else if (phase == k.hold) {
        pushKey(k.sc, false, k.shift);
        if (k.shift) pushKey(SDL_SCANCODE_LSHIFT, false, false);
    }
    if (++phase >= k.hold + 3) {
        phase = 0;
        queue.pop_front();
    }
}

}  // namespace

bool typing() { return sForceOn || port::settings().keyboard.typing; }

void scriptType(const std::string& text) {
    const auto keys = parseText(text);
    std::printf("[kbd] frame %u: typing \"%s\" (%zu keys, SDL events)\n", game::guestFrames(), text.c_str(), keys.size());
    std::fflush(stdout);
    std::lock_guard lk(sScriptMutex);
    sScriptKeys.insert(sScriptKeys.end(), keys.begin(), keys.end());
}

bool typesText(int sc) { return sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_SLASH; }  // HID 0x04-0x38

void handleEvent(const SDL_Event& ev, bool menuOpen) {
    if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        releaseAll();
        return;
    }
    if (ev.type != SDL_EVENT_KEY_DOWN && ev.type != SDL_EVENT_KEY_UP) return;
    if (!typing() || !sAttached) return;
    const int sc = int(ev.key.scancode);
    if (sc < SDL_SCANCODE_A || sc > SDL_SCANCODE_RGUI) return;  // outside the HID keyboard page
    const auto it = std::find(sDown.begin(), sDown.end(), u8(sc));
    if (ev.type == SDL_EVENT_KEY_UP) {
        if (it == sDown.end()) return;  // pressed while the menu was open (or before typing was on)
        sDown.erase(it);
        sendReport();
        return;
    }
    // Held keys repeat in the KBD library itself (a keyboard sends one report per change).
    if (ev.key.repeat || it != sDown.end()) return;
    if (menuOpen || sc == SDL_SCANCODE_F1) return;  // the menu's keys
    sDown.push_back(u8(sc));
    sendReport();
}

void hostFrame() {
    const bool on = typing();
    if (on != sAttached) {
        if (!on) releaseAll();
        sAttached = on;
        if (sLog) std::printf("[kbd] typing with the PC keyboard %s\n", on ? "on" : "off");
    }
    ios::usbKeyboardSetAttached(on);  // (also attaches once the game has opened the device)
    if (menu::isOpen()) releaseAll();
    scriptFrame();
    // CF_KBD_LOG: Wii Remote 1's bound inputs on every change (keys inert while typing; the mouse
    // and controllers still act).
    if (sLog) {
        static controls::RemoteInputs last{};
        controls::RemoteInputs ri;
        if (controls::remoteInputs(0, ri) &&
            (ri.buttons != last.buttons || ri.stickX != last.stickX || ri.stickY != last.stickY)) {
            std::printf("[kbd] frame %u: Wii Remote 1 buttons %05X stick %.2f,%.2f\n", game::guestFrames(), ri.buttons, ri.stickX,
                        ri.stickY);
            last = ri;
        }
    }
}

}  // namespace recomp::pckbd
