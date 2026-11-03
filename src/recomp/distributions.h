#pragma once
// Nintendo event distributions (F1 > Online > Nintendo events; docs/DISTRIBUTIONS.md): the items,
// letters and designs Nintendo sent to City Folk players over WiiConnect24 from December 2008 to
// December 2012. Not on any disc. The content comes from WiiLink's archive of the distribution
// items and letters (used with WiiLink's permission), downloaded at run time into a local cache,
// or from a "distributions" folder of original .enc files the player supplies. It reaches the
// game through the game's own WiiConnect24 path: this module is the "server" KD downloads from
// (nwc24_dl.h), the game's NWC24 code reads the package from wc24dl.vff and Pete brings the letter.
#include "recomp/ctx.h"

#include <string>
#include <vector>

namespace recomp::distrib {

// Settings: port::settings().events (src/port/settings.h NintendoEventSettings), mode values:
enum class Mode : int { Off = 0, Loop = 1, Original = 2, Manual = 3 };
Mode mode();
bool enabled();  // mode != Off (CF_DISTRIB=<mode> overrides for tests)

// --- KD side (nwc24_dl.cpp) ------------------------------------------------------------------
struct Reply {
    enum Kind { NotFound, NotModified, Container, Package } kind = NotFound;
    std::vector<u8> body;  // Container: a WC24 file (.enc, signed); Package: the U8 archive itself
    std::string what;      // log / F1 text
    int dist = -1;         // catalogue index (-1: an original .enc from the folder)
    u32 letterId = 0;
    std::string localFile;  // an original .enc from the folder
};
// What the "server" answers for a download URL of the running title, at the current game clock.
Reply serve(const std::string& url);
// KD stored (or failed to store) a reply in wc24dl.vff.
void onStored(const Reply& r, bool ok, const std::string& why);

// --- the game side ---------------------------------------------------------------------------
// Once per guest frame (src/game/cf_game.cpp onGuestFrame): starts the game's own WiiConnect24
// check while playing when a new package is waiting (USA; settings().liveDelivery).
void onGuestFrame(Ctx& c);

// F1 page (menu.cpp, Online tab); true when a setting changed.
bool drawMenuPage();

// Test aids / tools: catalogue size, schedule queries (docs/DISTRIBUTIONS.md "Testing").
int count();
std::string describeSchedule(long long consoleSeconds2000, int n);
bool selfTest(std::string* failure = nullptr);

}  // namespace recomp::distrib
