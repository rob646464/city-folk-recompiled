#pragma once
// Self-updater (docs/SETUP.md, "Updates"): checks GitHub Releases of the public repository
// for a newer version, downloads the package for this platform, verifies its SHA-256,
// stages it next to the running files and swaps them in when the player says so.
// Nothing here runs without the player's click except the background check at startup
// (setting updates.check_at_startup). Settings, saves, game modules and caches (APPDATA /
// XDG folders, or the portable folder next to the program) are never touched; the game module rebuilds itself when the new runtime
// needs it (module key, docs/SETUP.md).
//
// All work happens on a worker thread; the F1 menu polls state() every frame.
// Test overrides: CF_UPDATE_FEED=<https/http://localhost URL, file:// URL or file path> (release
// JSON object or array instead of the GitHub API), CF_UPDATE_VERSION=<pretend current version>.
#include <cstdint>
#include <string>
#include <vector>

namespace port::update {

const char* currentVersion();  // the running program's version ("1.1"; CF_UPDATE_VERSION overrides)
const char* buildVersion();    // the version baked in at build time (CMake project version)

enum class Phase {
    Idle,         // nothing checked yet (or check disabled)
    Checking,
    UpToDate,
    Available,    // newer release found (info filled)
    CheckFailed,  // offline, rate limited, bad answer (message says which)
    Downloading,
    Verifying,
    Extracting,
    Ready,        // staged: install() swaps the files and restarts
    Installing,
    Failed,       // download / verify / install error (message)
};

// One line of the release notes, markdown reduced to what the menu draws.
struct NoteLine {
    enum Kind { Text, Heading, Bullet, Blank } kind = Text;
    std::string text;
    int indent = 0;  // bullet nesting
};

struct Info {
    std::string tag;         // "v1.2"
    std::string version;     // "1.2"
    std::string title;       // release name
    std::string notes;       // release body as plain text (markdown stripped)
    std::vector<NoteLine> lines;  // the same, line by line for the menu
    std::string page;        // release page URL (html_url)
    std::string published;   // "2026-10-16"
    std::string assetName;   // package for this platform ("" if the release has none)
    std::uint64_t assetSize = 0;
};

struct State {
    Phase phase = Phase::Idle;
    Info info;
    std::string message;           // status / error text for the menu
    std::uint64_t got = 0, total = 0;  // download progress
    bool manual = false;           // last check was the menu button (skipped versions shown too)
    bool installWritable = true;   // install folder can be written (false: offer the download page)
    bool packaged = true;          // running from a release folder (not a build tree)
};

State state();

// Background check (no-op when updates.check_at_startup is off, in developer builds and build
// trees unless CF_UPDATE_FEED is set, and with CF_UPDATE_CHECK=0).
void startupCheck();
// Menu "Check for updates": always runs (shows a skipped version too).
void checkNow();
// Download, verify and stage the available release's package (progress in state()).
void download();
void cancel();
// Swaps the staged files in and restarts the program. Only call after the player confirmed.
// Returns only on failure (state().message).
void installAndRestart();

// Whether the notice (toast / F1 section) should be shown: an available version not skipped,
// not dismissed with "Later" this session.
bool shouldNotify();
void skipThisVersion();  // remembers the available tag in settings (updates.skip_version)
void later();            // hides the notice until the next start
bool dismissed();

// At startup: deletes the files an earlier update renamed out of the way (*.old) and its
// staging folder. Cheap; retries in the background while the previous process exits.
void cleanupAfterUpdate();

// Version comparison: -1 / 0 / 1 (a < b, equal, a > b). "v1.1" == "1.1.0" < "1.1.1".
int compareVersions(const std::string& a, const std::string& b);

}  // namespace port::update
