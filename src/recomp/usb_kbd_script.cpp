// CF_KBD_TYPE test aid (docs/ONLINE.md "USB keyboard chat"): types text on the emulated USB
// keyboard at guest frames, as if the keys were pressed on the PC keyboard.
//   CF_KBD_TYPE="<frame>:<text>|<frame>:<text>..."   text: letters, digits, space, .,!?'-;
//   {ENTER} {BACK} {ESC} {UP} {DOWN} {LEFT} {RIGHT} for those keys. Each key is held for 3 host
//   frames and released for 3 (shift added for capitals and shifted symbols).
// Only active with the USB keyboard on (F1 > Online or CF_USB_KBD_DEVICE).
#include "recomp/game.h"
#include "recomp/usb_kbd_script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace recomp::kbdscript {
namespace {

struct Key {
    int sc;
    bool shift;
};
struct Item {
    unsigned frame;
    std::vector<Key> keys;
};

bool keyFor(char ch, Key& k) {
    k.shift = false;
    if (ch >= 'a' && ch <= 'z') k.sc = 4 + (ch - 'a');
    else if (ch >= 'A' && ch <= 'Z') k = {4 + (ch - 'A'), true};
    else if (ch >= '1' && ch <= '9') k.sc = 30 + (ch - '1');
    else if (ch == '0') k.sc = 39;
    else if (ch == ' ') k.sc = 44;
    else if (ch == '-') k.sc = 45;
    else if (ch == '\'') k.sc = 52;
    else if (ch == ',') k.sc = 54;
    else if (ch == '.') k.sc = 55;
    else if (ch == ';') k.sc = 51;
    else if (ch == '!') k = {30, true};
    else if (ch == '?') k = {56, true};
    else return false;
    return true;
}

std::vector<Item> parse() {
    std::vector<Item> out;
    const char* e = std::getenv("CF_KBD_TYPE");
    if (!e || !*e) return out;
    std::string s = e;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find('|', p);
        if (q == std::string::npos) q = s.size();
        const std::string part = s.substr(p, q - p);
        p = q + 1;
        const size_t c = part.find(':');
        if (c == std::string::npos) continue;
        Item it;
        it.frame = unsigned(std::strtoul(part.c_str(), nullptr, 10));
        const std::string text = part.substr(c + 1);
        for (size_t i = 0; i < text.size(); i++) {
            Key k{};
            if (text[i] == '{') {
                const size_t z = text.find('}', i);
                const std::string name = text.substr(i + 1, z - i - 1);
                i = z;
                k.sc = name == "ENTER" ? 40 : name == "BACK" ? 42 : name == "ESC" ? 41 : name == "RIGHT" ? 79 : name == "LEFT" ? 80
                     : name == "DOWN" ? 81 : name == "UP" ? 82 : 0;
                if (k.sc) it.keys.push_back(k);
            } else if (keyFor(text[i], k)) {
                it.keys.push_back(k);
            }
        }
        out.push_back(std::move(it));
    }
    return out;
}

}  // namespace

void apply(u8* sc, int count) {
    static std::vector<Item> items = parse();
    static std::deque<Key> queue;
    static int phase = 0;  // host frames into the current key (0-2 down, 3-5 up)
    if (items.empty() && queue.empty()) return;
    const unsigned f = game::guestFrames();
    for (auto it = items.begin(); it != items.end();) {
        if (f >= it->frame) {
            std::printf("[kbd] frame %u: typing %zu keys\n", f, it->keys.size());
            for (auto& k : it->keys) queue.push_back(k);
            it = items.erase(it);
        } else {
            ++it;
        }
    }
    if (queue.empty()) return;
    const Key k = queue.front();
    if (phase < 3) {
        if (k.sc < count) sc[k.sc] = 1;
        if (k.shift && 0xE1 < count) sc[0xE1] = 1;  // left shift
    }
    if (++phase >= 6) {
        phase = 0;
        queue.pop_front();
    }
}

}  // namespace recomp::kbdscript
