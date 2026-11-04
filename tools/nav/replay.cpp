// Offline replay of "Snap pointer to menu items" item sets (docs/CONTROLS.md "Snap pointer to
// menu items", "Checking the moves"): reads the item sets tools/nav/check.py extracts from a
// CF_NAV_TRACE file and prints the move src/recomp/menu_nav.cpp picks from every item in 8
// directions, so the selection code can be checked against every logged screen without a game run.
// Built by check.py (clang++ -DMENUNAV_REPLAY with src/recomp/menu_nav.cpp).
//   stdin:  "set <id> <menu|toolbar> <n>" then n lines "<key> <group> <hud> <tested> <layer> <x0> <y0> <x1> <y1>"
//           (every visible item of the frame)
//   stdout: "n <id> <key>" for each navigable item (the modal rules applied), "m <id> <from> <dir>
//           <to|->" and "c <id> <key>" for containers
//   argv[1] == "old": the first version's search (CF_NAV_OLD)
#include "recomp/game.h"
#include "recomp/menu_nav.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace recomp {
void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
namespace game {
u32 guestFrames() { return 0; }  // the u32 of recomp/game.h
}  // namespace game
namespace menunav::replay {
const Item* move(const std::vector<Item>& items, const Item& cur, float dx, float dy, bool hudOnly);
bool container(const std::vector<Item>& items, size_t i);
void setOld(bool on);
std::vector<Item> nav(const std::vector<Item>& all, bool& hudOnly);
}  // namespace menunav::replay
}  // namespace recomp

int main(int argc, char** argv) {
    using namespace recomp::menunav;
    replay::setOld(argc > 1 && std::strcmp(argv[1], "old") == 0);
    static const struct {
        const char* n;
        float x, y;
    } kDirs[8] = {{"L", -1, 0}, {"R", 1, 0}, {"U", 0, -1}, {"D", 0, 1},
                  {"UL", -0.7071f, -0.7071f}, {"UR", 0.7071f, -0.7071f}, {"DL", -0.7071f, 0.7071f}, {"DR", 0.7071f, 0.7071f}};
    char line[512];
    while (std::fgets(line, sizeof line, stdin)) {
        int id = 0, n = 0;
        char mode[16] = {};
        if (std::sscanf(line, "set %d %15s %d", &id, mode, &n) != 3) continue;
        std::vector<Item> all(size_t(n > 0 ? n : 0));
        for (auto& it : all) {
            if (!std::fgets(line, sizeof line, stdin)) return 1;
            unsigned key = 0, group = 0, layer = 0;
            int hud = 0, tested = 0;
            std::sscanf(line, "%x %x %d %d %u %f %f %f %f", &key, &group, &hud, &tested, &layer, &it.x0, &it.y0, &it.x1, &it.y1);
            it.key = key;
            it.group = group;
            it.hud = hud != 0;
            it.tested = tested != 0;
            it.layer = u16(layer);
        }
        (void)mode;
        bool hudOnly = false;
        const std::vector<Item> items = replay::nav(all, hudOnly);
        for (const auto& it : items) std::printf("n %d %08X\n", id, it.key);
        for (size_t i = 0; i < items.size(); i++)
            if (replay::container(items, i)) std::printf("c %d %08X\n", id, items[i].key);
        for (const auto& it : items)
            for (const auto& d : kDirs) {
                const Item* to = replay::move(items, it, d.x, d.y, hudOnly);
                if (to) std::printf("m %d %08X %s %08X\n", id, it.key, d.n, to->key);
                else std::printf("m %d %08X %s -\n", id, it.key, d.n);
            }
    }
    return 0;
}
