#pragma once
// What the user installed: their disc images and what setup derived from them
// (docs/SETUP.md). Persisted to %APPDATA%\Animal Crossing City Folk Recompiled\install.ini; the derived
// files live next to it. The F1 menu reads it to grey out features whose disc is missing.
#include <filesystem>
#include <string>
#include <vector>

namespace port {

// The four releases (docs/SETUP.md "Discs"), revision 1 each: USA, Europe, Japan, Korea (the only
// Korean release). Every disc carries every region's content, so one disc of any region is a complete
// install; several can be installed side by side.
enum class GameVersion { USA, PAL, JPN, KOR };  // RUUE01 / RUUP01 / RUUJ01 / RUUK01
constexpr int kVersionCount = 4;
constexpr GameVersion kVersions[kVersionCount] = {GameVersion::USA, GameVersion::PAL, GameVersion::JPN,
                                                  GameVersion::KOR};

const char* gameId(GameVersion v);       // "RUUE01" / "RUUP01" / "RUUJ01" / "RUUK01"
const char* versionName(GameVersion v);  // "USA" / "PAL" / "JPN" / "KOR" (install.ini, logs)
const char* regionName(GameVersion v);   // "USA" / "Europe" / "Japan" / "Korea" (what players read)
// The disc revision the port knows (by its hashes) for a version: 1 for all four. It is the outer
// disc header's byte 7; the Korean disc's game partition header (sys/boot.bin, which the console
// copies to 0x80000000) says 0 instead (partitionRevision).
int supportedRevision(GameVersion v);
int partitionRevision(GameVersion v);
// "RUUE01"... (also a disc's first 6 header bytes) -> version; false for anything else.
bool versionFromId(const char* id, GameVersion& v);

struct InstallState {
    // Path of the user's disc for each version (image or extracted folder, UTF-8), empty if none.
    std::string disc[kVersionCount];
    GameVersion base = GameVersion::USA;  // which version runs (when several are installed)

    // Filled at startup from the setup results (not persisted).
    bool ready[kVersionCount] = {};  // game module built and current
    GameVersion running = GameVersion::USA;  // version of the game that is running

    bool staticBuild = false;  // cfrecomp: one version linked in, no setup

    std::string& discOf(GameVersion v) { return disc[int(v)]; }
    const std::string& discOf(GameVersion v) const { return disc[int(v)]; }
    bool isReady(GameVersion v) const { return ready[int(v)]; }
    int readyCount() const {
        int n = 0;
        for (bool r : ready) n += r;
        return n;
    }
};

// Feature availability for the F1 menu (docs/SETUP.md, "Feature gating"). A feature written for
// one version's game is greyed out when that version is not installed; with it installed but
// another one running, it stays selectable and says where it applies.
enum class Avail {
    Yes,           // works in the running game
    OtherBase,     // works when the other version is the base (installed too)
    NeedsUsDisc,   // the USA disc is not installed
    NeedsPalDisc,  // the European disc is not installed
    NotYet,        // installed, but not implemented for the running version yet
};
Avail usCodeFeature();      // enhancements written against the USA game's code/addresses
Avail palBaseFeature();     // 50/60 Hz: options of the European game itself
const char* availNote(Avail a);  // short explanation, nullptr for Yes

InstallState& install();

// Where the port writes (created on demand; docs/SETUP.md, docs/LINUX.md):
//   Windows: all three are %APPDATA%\Animal Crossing City Folk Recompiled.
//   Linux:   configDir  $XDG_CONFIG_HOME/Animal Crossing City Folk Recompiled (~/.config/...): settings.ini, install.ini
//            dataDir    $XDG_DATA_HOME/Animal Crossing City Folk Recompiled (~/.local/share/...): game modules,
//                       NAND, screenshots
//            cacheDir   $XDG_CACHE_HOME/Animal Crossing City Folk Recompiled (~/.cache/...): GPU pipeline/shader caches
//   Portable mode (both): all three are <program folder>/portable (the Windows layout).
std::filesystem::path configDir();
std::filesystem::path dataDir();
std::filesystem::path cacheDir();

// Portable mode (docs/SETUP.md "Portable mode"): a file portable.txt or a folder portable next to
// the program (or CF_PORTABLE=1) keeps every file of the port in <program folder>/portable.
// Decided once, at the first call (which also carries out a data move the F1 menu asked for
// before the restart); CF_PORTABLE=0 forces the system folders (testing).
struct DataLocation {
    bool portable = false;
    std::filesystem::path root;    // portable: <exeDir>/portable
    std::filesystem::path exeDir;  // folder of the running program
    bool requested = false;        // portable.txt / portable folder / CF_PORTABLE=1
    bool forcedByEnv = false;      // CF_PORTABLE=1: the menu cannot switch it off
    std::string problem;           // why requested portable mode is off (folder not writable)
    std::string moveResult;        // outcome of a data move carried out at this start ("" none)
    bool moveFailed = false;
};
const DataLocation& dataLocation();
std::filesystem::path exeDir();
// The system folders (Windows: one, Linux: config, data, cache), whatever mode runs.
std::vector<std::filesystem::path> systemDirs();
// Whether the program folder can be written (probe file).
bool exeDirWritable();
// F1 > Game: whether a move to (toPortable) or from portable mode can start now; "" or the reason.
std::string checkDataMove(bool toPortable);
// Saves the request next to the program and restarts; the next start moves the data before it
// opens any file (copy, verify, switch, then remove the old copy; nothing is lost on failure).
// Returns only on failure (the reason).
std::string requestDataMove(bool toPortable);
// A path inside the program folder (relative to it) that holds the player's data in portable
// mode: the updater never replaces or removes these (update.cpp).
bool isPortableDataPath(const std::filesystem::path& relToExeDir);
// An environment variable holding a path (read as UTF-16 on Windows, so any characters survive).
std::filesystem::path envPath(const char* name);

// Long paths on Windows (docs/SETUP.md "Long paths"). The executable's manifest declares
// longPathAware, but Windows honours that only when the system policy (LongPathsEnabled) is on;
// otherwise every file call fails past 259 characters and reports existing files as missing.
//   longPath:  p as an extended-length path (\\?\C:\... or \\?\UNC\server\share\...): absolute,
//              normalized, backslashes. File calls (std::filesystem, fstream, CreateFileW,
//              CreateProcessW's program, LoadLibraryW) take it at any length. Not for paths
//              given to other programs (clang/lld/tar reject it) or shown to the player.
//   plainPath: p without that prefix (messages, logs, tool arguments).
//   longPathsEnabled: this process gets long paths without the prefix (manifest + policy).
// Elsewhere both are p unchanged.
std::filesystem::path longPath(const std::filesystem::path& p);
std::filesystem::path plainPath(const std::filesystem::path& p);
std::string displayPath(const std::filesystem::path& p);  // UTF-8 of plainPath
bool longPathsEnabled();
// The longest path a program that is not long-path aware can open (MAX_PATH - 1 on Windows).
constexpr size_t kMaxPlainPath = 259;
// The deepest file the port writes below dataDir() (memory card files, the module build's
// obj/ and include/recomp/ files), in characters including the separator: a data folder longer
// than kMaxPlainPath minus this cannot be used without long paths (warned at startup).
constexpr size_t kDeepestDataFile = 52;
// The deepest game folder the program runs from (Windows): from a deeper one the prebuilt Dawn
// / Direct3D 12 start-up fails without a word (measured: 234 runs, 236 and deeper stop at the
// adapter request, with or without long paths), so main() refuses it with installPathProblem().
constexpr size_t kMaxInstallPath = 234;
std::string installPathProblem();  // "" when the program's folder is short enough
// A message for the player when long paths are off and the data folder is too deep for the
// parts that use plain paths (Aurora's memory cards, caches, imgui.ini); "" when all is well.
std::string dataPathProblem();

void loadInstall();
void saveInstall();

// Restarts the executable with the same arguments (after the user changed the install);
// `setup` opens the setup screen (add or change discs). `exe`: the program to start instead of
// the running one's file (the updater: on Linux /proc/self/exe names the replaced file); on Linux
// it is exec'd in place of this process (same PID, so a launcher script keeps waiting for it).
void relaunch(bool setup = false, const std::filesystem::path& exe = {});
#ifndef _WIN32
// The running program's file (Linux: /proc/self/exe, macOS: _NSGetExecutablePath, links resolved);
// empty when it cannot be found.
std::filesystem::path selfExePath();
// Its folder; never empty: the current directory when selfExePath() is empty (the behaviour
// before selfExePath existed), so callers never join paths onto an empty relative path.
std::filesystem::path selfExeDir();
#endif
void setLaunchArgs(int argc, char** argv);

}  // namespace port
