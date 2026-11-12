// Photos to the screenshots folder (F1 > Game > SD Card; docs/FEATURES.md "Photos to the screenshots
// folder", docs/NAND.md "SD card").
//
// How the game saves a photo (USA rev 1 addresses; the same functions on all four discs, hooked by
// their USA names through hooks.txt "all"):
// - Button 1 takes a photo; the game keeps one unsaved photo and its JPEG (its own encoder, the
//   frame as drawn, so Enhanced lighting and the other render options are in it): buffer
//   fn_800FADCC(), size fn_800FADD4().
// - Photos window (d_menu_photoNP) > "SD Card" > "Save Photo" runs four steps, each a worker thread
//   (fn_8008F0D8 starts it, fn_8008F278 polls; result code at r13-0x33A4: 1 = done, others pick the
//   SYS_SDcard message: 3 no card, 8 card full, 0xC no photo data, 2 "Unable to save.", ...):
//     fn_8008F330  attach the SD drive (fn_8021C600 type 0: the eSOL PrFILE2 driver pfd_sddrv over
//                  /dev/sdio/slot0; drive table 0x80588C20, +8 letter, +9 bit 0x10 = card in) + mount
//     fn_8008F398  mkdir "<d>:\DCIM" (EEXIST is fine)
//     fn_8008F798  pick the file: the highest "\DCIM\<100..999>NIN01" folder, the highest 4-digit
//                  number of "????????.JPG" in it + 1 -> "\DCIM\100NIN01\RUU_0001.JPG" (a new
//                  folder when there is none or it reached 9999; DCF layout)
//     fn_8008F854  create the file (fn_8021C7F4), write the JPEG (fn_8021C98C), close (fn_8021C908);
//                  on a failed write it closes and removes the file
//   "Erase All": attach + mount, fn_8008F950 (any "\DCIM\<n>NIN01\RUU_????.JPG"? none = 0xC
//   "There is no Animal Crossing photo data on this SD Card."), fn_8008FB48 (removes every
//   RUU_????.JPG of every NIN01 folder, then the folders left empty). The photo the game holds is
//   not touched by either.
// - The PrFILE2 calls go through thin wrappers (fn_8021C600 .. fn_8021CC38) that only this photo code
//   calls; they post requests to PrFILE2's own file thread (fn_8021B80C), which runs the FAT code and
//   the SD driver.
//
// The port (setting sdCard.photosToFolder, on by default): those wrappers are a small SD card host
// at the file level. A card is always in; "\DCIM" exists; the NIN01 folders the game makes live for
// the session (every one of them is the same screenshots folder); a file in one of them is a file of
// the screenshots folder: the game lists its 8.3 names to number the next photo (RUU_0001.JPG,
// RUU_0002.JPG, ... continuing after the highest one already there, also across restarts) and writes
// its own JPEG bytes into it. A name that exists already is never overwritten (exclusive create; a
// dated name instead). Erase All: while fn_8008F950 / fn_8008FB48 run the card shows no photos, so
// the game reports "There is no Animal Crossing photo data on this SD Card." and nothing is removed;
// the remove / rmdir requests never delete anything in any case. Off: every wrapper runs the
// original (no card in the slot, as before). The mode is taken when the game attaches the drive, so
// switching it in F1 mid-save does not mix the two.
//
// Where: <program folder>/screenshots (next to CityFolkRecomp.exe; in portable mode that is also
// where portable/ is); when that folder cannot be written (e.g. Program Files),
// <data folder>/screenshots. Created at the first save.
//
// Test aids: CF_SD_PHOTOS=0|1 (overrides the setting), CF_SD_PHOTO_DIR=<dir> (instead of
// <program folder>/screenshots; a path that cannot be created tests the fallback), CF_SD_LOG=1
// (every SD call with its arguments and result). `[photo]` lines: the folder, each saved file
// (name, size in pixels and bytes, time), Erase All.
#include "game/cf_features.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/menu.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace fs = std::filesystem;

namespace recomp::cf {
namespace {

// PrFILE2 error numbers (errno values) the game's photo code tells apart (fn_8008EE94).
constexpr u32 kENOENT = 2, kEACCES = 13, kEEXIST = 17, kENOSPC = 28;
// Where the game reads a found entry's 8.3 name in the find buffer (fn_8008F624: name + 0x22D,
// the number at +0x231).
constexpr u32 kDtaName = 0x22D;
constexpr u32 kHandleBase = 0x5D0001;  // handles we give out (the game only passes them back)

bool envFlag(const char* name, bool def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    return *v != '0';
}
bool sdLog() {
    static const bool on = envFlag("CF_SD_LOG", false);
    return on;
}
// The setting, or CF_SD_PHOTOS for test runs.
bool featureOn() {
    static const int env = [] {
        const char* v = std::getenv("CF_SD_PHOTOS");
        return v && *v ? (*v != '0' ? 1 : 0) : -1;
    }();
    return env >= 0 ? env == 1 : port::settings().sdCard.photosToFolder;
}

std::string upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
std::string utf8(const fs::path& p) { return port::displayPath(p); }

// ---- the folder -------------------------------------------------------------------------------

std::mutex sFolderMutex;  // the guest's save thread decides, the F1 menu reads
fs::path sFolder;         // decided at the first SD access of the session ("" not yet)

fs::path primaryFolder() {
    const fs::path env = port::envPath("CF_SD_PHOTO_DIR");
    return env.empty() ? port::exeDir() / "screenshots" : env;
}
fs::path fallbackFolder() { return port::dataDir() / "screenshots"; }

// A file can be created in `dir` (an existing folder): probe file, removed again.
bool probeWritable(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(port::longPath(dir), ec)) return false;
    const fs::path probe = dir / (".cf_write_probe_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#ifdef _WIN32
    HANDLE h = CreateFileW(port::longPath(probe).wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return false;
    ::close(fd);
    ::unlink(probe.c_str());
    return true;
#endif
}

// The folder photos go to: the program's screenshots folder when it is (or can be made) writable,
// else the data folder's. Nothing is created here.
fs::path decideFolder() {
    {
        std::lock_guard lk(sFolderMutex);
        if (!sFolder.empty()) return sFolder;
    }
    const fs::path primary = primaryFolder();
    std::error_code ec;
    bool ok;
    std::string why;
    if (fs::exists(port::longPath(primary), ec)) {
        ok = probeWritable(primary);
        why = "cannot be written";
    } else {
        // Not there yet: the closest existing parent must take a new folder.
        fs::path parent = primary.parent_path();
        while (!parent.empty() && !fs::exists(port::longPath(parent), ec) && parent != parent.parent_path())
            parent = parent.parent_path();
        ok = !parent.empty() && probeWritable(parent);
        why = "cannot be created";
    }
    fs::path chosen = primary;
    if (!ok) {
        chosen = fallbackFolder();
        log("[photo] %s %s (read-only folder?): photos go to %s", utf8(primary).c_str(), why.c_str(), utf8(chosen).c_str());
    } else {
        log("[photo] screenshots folder: %s", utf8(chosen).c_str());
    }
    std::lock_guard lk(sFolderMutex);
    if (sFolder.empty()) sFolder = chosen;
    return sFolder;
}
void switchToFallback(const std::string& why) {
    const fs::path fb = fallbackFolder();
    std::lock_guard lk(sFolderMutex);
    if (sFolder == fb) return;
    log("[photo] %s: %s; photos go to %s", utf8(sFolder).c_str(), why.c_str(), utf8(fb).c_str());
    sFolder = fb;
}
// For the F1 menu: the folder photos went / will go to, without deciding.
fs::path shownFolder(bool* exists) {
    fs::path f;
    {
        std::lock_guard lk(sFolderMutex);
        f = sFolder;
    }
    std::error_code ec;
    if (f.empty()) {
        f = primaryFolder();
        if (!fs::is_directory(port::longPath(f), ec) && fs::is_directory(port::longPath(fallbackFolder()), ec)) f = fallbackFolder();
    }
    *exists = fs::is_directory(port::longPath(f), ec);
    return f;
}

// ---- the card ---------------------------------------------------------------------------------

struct OpenFile {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    fs::path path;
    std::string guestPath;
    std::vector<u8> data;  // what was written (size / dimensions for the log)
    bool failed = false;
    std::chrono::steady_clock::time_point start;
    double writeMs = 0;
};

struct Find {
    std::vector<std::string> names;
    size_t next = 0;
};

bool sActive = false;  // the drive is attached in our mode (taken at attach)
bool sHide = false;    // Erase All's check / erase are running: the card shows no photos
u32 sErr = 0;
u32 sNextHandle = kHandleBase;
std::map<u32, OpenFile> sFiles;
std::map<u32, Find> sFinds;    // by find buffer address
std::set<std::string> sDirs;   // "\DCIM\<name>" folders made this session (upper case)
std::string sRemovedPartial;   // a half-written file we deleted ourselves (the game removes it next)
std::atomic<int> sSaved{0};    // photos saved this session (F1 shows it)

void setErr(u32 e) { sErr = e; }

// "<d>:\DCIM\100NIN01\RUU_0001.JPG" -> {"DCIM", "100NIN01", "RUU_0001.JPG"} (upper case).
std::vector<std::string> splitPath(const char* p) {
    std::string s = p ? p : "";
    if (s.size() >= 2 && s[1] == ':') s = s.substr(2);
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == '\\' || ch == '/') {
            if (!cur.empty()) out.push_back(upper(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) out.push_back(upper(cur));
    return out;
}

// DOS wildcards as the game uses them: '?' one character, '*' any run (case already folded).
bool wildMatch(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') return wildMatch(pat + 1, s) || (*s && wildMatch(pat, s + 1));
    if (!*s) return false;
    return (*pat == '?' || *pat == *s) && wildMatch(pat + 1, s + 1);
}

// A name the game can see on a FAT card without long names: 8.3, printable ASCII.
bool isShortName(const std::string& n) {
    if (n.empty() || n.size() > 12) return false;
    const size_t dot = n.find('.');
    if (dot != std::string::npos && (dot == 0 || dot > 8 || n.size() - dot - 1 > 3 || n.find('.', dot + 1) != std::string::npos))
        return false;
    if (dot == std::string::npos && n.size() > 8) return false;
    for (char ch : n) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u <= 0x20 || u >= 0x7F || std::strchr("\"*+,/:;<=>?[\\]|", ch)) return false;
    }
    return true;
}

// Photo-folder files the card shows: regular files with 8.3 names, upper case.
std::vector<std::string> folderNames() {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = decideFolder();
    for (auto it = fs::directory_iterator(port::longPath(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string n = upper(utf8(it->path().filename()));
        if (isShortName(n)) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool isDcim(const std::vector<std::string>& c) { return !c.empty() && c[0] == "DCIM"; }
bool isPhotoDir(const std::vector<std::string>& c) { return c.size() == 2 && isDcim(c) && sDirs.count(c[1]); }

// Entries of a folder of the card ({} for the root). False: no such folder.
bool listDir(const std::vector<std::string>& dir, std::vector<std::string>* out) {
    out->clear();
    if (dir.empty()) {
        out->push_back("DCIM");
        return true;
    }
    if (dir.size() == 1 && isDcim(dir)) {
        out->assign(sDirs.begin(), sDirs.end());
        return true;
    }
    if (isPhotoDir(dir)) {
        *out = folderNames();
        return true;
    }
    return false;
}

void writeName(u32 dta, const std::string& n) {
    for (u32 i = 0; i < 13; i++) wr8(dta + kDtaName + i, i < n.size() ? u32(static_cast<unsigned char>(n[i])) : 0u);
}

// Width x height from a JPEG's SOF marker ("?" when there is none).
std::string jpegSize(const std::vector<u8>& d) {
    size_t i = 2;
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) return "not a JPEG";
    while (i + 9 < d.size()) {
        if (d[i] != 0xFF) return "?";
        const u8 m = d[i + 1];
        const size_t len = (size_t(d[i + 2]) << 8) | d[i + 3];
        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            const unsigned h = (unsigned(d[i + 5]) << 8) | d[i + 6], w = (unsigned(d[i + 7]) << 8) | d[i + 8];
            return std::to_string(w) + "x" + std::to_string(h);
        }
        i += 2 + len;
    }
    return "?";
}

// Creates `p` only if it does not exist. 0 ok, else 1 exists, 2 folder not writable / missing,
// 3 other error.
int createNew(OpenFile& f, const fs::path& p) {
#ifdef _WIN32
    f.h = CreateFileW(port::longPath(p).wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f.h != INVALID_HANDLE_VALUE) return 0;
    const DWORD e = GetLastError();
    std::error_code ec;
    // A folder (or anything else) of that name: access denied, but it is a name taken.
    if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS || fs::exists(port::longPath(p), ec)) return 1;
    if (e == ERROR_ACCESS_DENIED || e == ERROR_PATH_NOT_FOUND || e == ERROR_WRITE_PROTECT) return 2;
    return 3;
#else
    f.fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (f.fd >= 0) return 0;
    if (errno == EEXIST) return 1;
    if (errno == EACCES || errno == ENOENT || errno == EROFS || errno == EPERM) return 2;
    return 3;
#endif
}
void closeFile(OpenFile& f) {
#ifdef _WIN32
    if (f.h != INVALID_HANDLE_VALUE) CloseHandle(f.h);
    f.h = INVALID_HANDLE_VALUE;
#else
    if (f.fd >= 0) ::close(f.fd);
    f.fd = -1;
#endif
}
// 0 ok, else the error number for the game.
u32 writeBytes(OpenFile& f, const u8* p, size_t n) {
#ifdef _WIN32
    while (n) {
        DWORD done = 0;
        const DWORD chunk = DWORD(std::min<size_t>(n, 1u << 30));
        if (!WriteFile(f.h, p, chunk, &done, nullptr) || done == 0)
            return (GetLastError() == ERROR_DISK_FULL || GetLastError() == ERROR_HANDLE_DISK_FULL) ? kENOSPC : kEACCES;
        p += done;
        n -= done;
    }
#else
    while (n) {
        const ssize_t w = ::write(f.fd, p, n);
        if (w <= 0) return errno == ENOSPC ? kENOSPC : kEACCES;
        p += w;
        n -= size_t(w);
    }
#endif
    return 0;
}

std::string timeStamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

// Opens the photo file for the game's name; never replaces an existing file (dated name then).
u32 openPhoto(const std::string& guestPath, const std::string& name) {
    OpenFile f;
    f.guestPath = guestPath;
    f.start = std::chrono::steady_clock::now();
    const bool wild = name.find_first_of("?*") != std::string::npos || !isShortName(name);
    const std::string base = name.substr(0, name.rfind('.') == std::string::npos ? name.size() : name.rfind('.'));
    const std::string ext = name.rfind('.') == std::string::npos ? ".JPG" : name.substr(name.rfind('.'));
    for (int pass = 0; pass < 2; pass++) {
        const fs::path dir = decideFolder();
        std::error_code ec;
        fs::create_directories(port::longPath(dir), ec);
        int r = 1;
        fs::path p;
        for (int n = 0; n < 100 && r == 1; n++) {
            std::string fn;
            if (n == 0 && !wild) fn = name;
            else fn = (wild ? std::string("RUU") : base) + "_" + timeStamp() + (n > 1 ? "_" + std::to_string(n) : "") + ext;
            p = dir / fn;
            r = createNew(f, p);
        }
        if (r == 0) {
            f.path = p;
            const u32 h = sNextHandle++;
            sFiles[h] = std::move(f);
            return h;
        }
        if (r == 2 && pass == 0 && dir != fallbackFolder()) {
            switchToFallback("cannot write " + utf8(p.filename()));
            continue;
        }
        log("[photo] could not create a file in %s (error %d)", utf8(dir).c_str(), r);
        break;
    }
    setErr(kEACCES);
    return 0;
}

void logCall(const char* what, const std::string& args, u32 r) {
    if (sdLog()) log("[sd] %s(%s) = %d (errnum %u)%s", what, args.c_str(), s32(r), sErr, sHide ? " [erase: card shows no photos]" : "");
}

}  // namespace

// --- the PrFILE2 wrappers of the photo code (hooks.txt) ------------------------------------------

// attach(type, ?, ?, drive table): type 0 = the SD card.
HLE(fn_8021C600) {
    if (!featureOn() || c.r[3] != 0) {
        sActive = false;
        callOriginal(c, "fn_8021C600");
        return;
    }
    const u32 drv = c.r[6];
    if (!sActive) {
        sFinds.clear();
        decideFolder();
    }
    sActive = true;
    if (drv) {
        if (!rd8(drv + 8)) wr8(drv + 8, 'A');
        wr8(drv + 9, rd8(drv + 9) | 0x10);  // a card is in
    }
    setErr(0);
    c.r[3] = 0;
    logCall("attach", "SD", 0);
}

// detach(letter)
HLE(fn_8021C820) {
    if (!sActive) {
        callOriginal(c, "fn_8021C820");
        return;
    }
    for (auto& [h, f] : sFiles) closeFile(f);
    sFiles.clear();
    sFinds.clear();
    sActive = false;
    c.r[3] = 0;
    logCall("detach", "", 0);
}

// mount(letter)
HLE(fn_8021CA84) {
    if (!sActive) {
        callOriginal(c, "fn_8021CA84");
        return;
    }
    setErr(0);
    c.r[3] = 0;
    logCall("mount", "", 0);
}

// unmount(letter, mode)
HLE(fn_8021CC38) {
    if (!sActive) {
        callOriginal(c, "fn_8021CC38");
        return;
    }
    setErr(0);
    c.r[3] = 0;
    logCall("unmount", "", 0);
}

// errnum()
HLE(fn_8021C8C8) {
    if (!sActive) {
        callOriginal(c, "fn_8021C8C8");
        return;
    }
    c.r[3] = sErr;
}

// fsfirst(pattern, attributes, find buffer)
HLE(fn_8021C934) {
    if (!sActive) {
        callOriginal(c, "fn_8021C934");
        return;
    }
    const std::string pat = gstr(c.r[3]);
    const u32 dta = c.r[5];
    std::vector<std::string> comp = splitPath(pat.c_str());
    std::vector<std::string> entries;
    Find f;
    if (!comp.empty() && !sHide) {
        const std::string namePat = comp.back();
        comp.pop_back();
        if (listDir(comp, &entries))
            for (auto& e : entries)
                if (wildMatch(namePat.c_str(), e.c_str())) f.names.push_back(e);
    }
    if (f.names.empty()) {
        sFinds.erase(dta);
        setErr(kENOENT);
        c.r[3] = u32(-1);
    } else {
        writeName(dta, f.names[0]);
        f.next = 1;
        sFinds[dta] = std::move(f);
        setErr(0);
        c.r[3] = 0;
    }
    logCall("fsfirst", pat + (c.r[3] == 0 ? " -> " + sFinds[dta].names[0] : ""), c.r[3]);
}

// fsnext(find buffer)
HLE(fn_8021C960) {
    if (!sActive) {
        callOriginal(c, "fn_8021C960");
        return;
    }
    const u32 dta = c.r[3];
    auto it = sFinds.find(dta);
    if (sHide || it == sFinds.end() || it->second.next >= it->second.names.size()) {
        if (it != sFinds.end()) sFinds.erase(it);
        setErr(kENOENT);
        c.r[3] = u32(-1);
        logCall("fsnext", "", c.r[3]);
        return;
    }
    const std::string n = it->second.names[it->second.next++];
    writeName(dta, n);
    setErr(0);
    c.r[3] = 0;
    logCall("fsnext", "-> " + n, 0);
}

// mkdir(path)
HLE(fn_8021CA58) {
    if (!sActive) {
        callOriginal(c, "fn_8021CA58");
        return;
    }
    const std::string path = gstr(c.r[3]);
    const auto comp = splitPath(path.c_str());
    u32 r = 0;
    if (comp.size() == 1 && isDcim(comp)) {
        setErr(kEEXIST);  // the card has a DCIM folder
        r = u32(-1);
    } else if (comp.size() == 2 && isDcim(comp)) {
        if (sDirs.insert(comp[1]).second) setErr(0);
        else setErr(kEEXIST), r = u32(-1);
    } else {
        setErr(kEACCES);
        r = u32(-1);
    }
    c.r[3] = r;
    logCall("mkdir", path, r);
}

// fopen / create(path, mode) -> handle or 0
HLE(fn_8021C7F4) {
    if (!sActive) {
        callOriginal(c, "fn_8021C7F4");
        return;
    }
    const std::string path = gstr(c.r[3]);
    const auto comp = splitPath(path.c_str());
    u32 h = 0;
    if (comp.size() == 3 && isPhotoDir({comp[0], comp[1]})) {
        h = openPhoto(path, comp[2]);
        if (h) setErr(0);
    } else {
        setErr(kENOENT);
    }
    c.r[3] = h;
    logCall("fopen", path, h);
}

// fwrite(buffer, size, count, handle) -> count written
HLE(fn_8021C98C) {
    if (!sActive) {
        callOriginal(c, "fn_8021C98C");
        return;
    }
    const u32 buf = c.r[3], size = c.r[4], count = c.r[5], h = c.r[6];
    auto it = sFiles.find(h);
    u32 r = 0;
    if (it == sFiles.end() || it->second.failed) {
        setErr(kEACCES);
    } else {
        OpenFile& f = it->second;
        const u64 n = u64(size) * count;
        std::vector<u8> bytes(size_t(std::min<u64>(n, 64u << 20)));
        for (size_t i = 0; i < bytes.size(); i++) bytes[i] = u8(rd8(buf + u32(i)));
        const auto t0 = std::chrono::steady_clock::now();
        const u32 e = bytes.size() == n ? writeBytes(f, bytes.data(), bytes.size()) : kENOSPC;
        f.writeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (e) {
            // Never leave a broken file behind: this one is ours (made by this call's fopen).
            closeFile(f);
            std::error_code ec;
            fs::remove(port::longPath(f.path), ec);
            log("[photo] writing %s failed (error %u): file removed", utf8(f.path).c_str(), e);
            sRemovedPartial = upper(f.guestPath);
            f.failed = true;
            setErr(e);
        } else {
            f.data.insert(f.data.end(), bytes.begin(), bytes.end());
            setErr(0);
            r = count;
        }
    }
    c.r[3] = r;
    logCall("fwrite", std::to_string(size) + " x " + std::to_string(count), r);
}

// fclose(handle)
HLE(fn_8021C908) {
    if (!sActive) {
        callOriginal(c, "fn_8021C908");
        return;
    }
    const u32 h = c.r[3];
    auto it = sFiles.find(h);
    u32 r = 0;
    if (it == sFiles.end()) {
        setErr(kEACCES);
        r = u32(-1);
    } else {
        OpenFile& f = it->second;
        const bool ok = !f.failed;
        closeFile(f);
        if (ok) {
            sSaved++;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f.start).count();
            log("[photo] saved %s (%s, %zu bytes) to %s in %.1f ms (file writes %.1f ms), guest frame %u",
                utf8(f.path.filename()).c_str(), jpegSize(f.data).c_str(), f.data.size(), utf8(f.path.parent_path()).c_str(), ms,
                f.writeMs, game::guestFrames());
        }
        sFiles.erase(it);
        setErr(0);
    }
    c.r[3] = r;
    logCall("fclose", "", r);
}

// remove(path): never deletes a photo of the screenshots folder.
HLE(fn_8021CBE0) {
    if (!sActive) {
        callOriginal(c, "fn_8021CBE0");
        return;
    }
    const std::string path = gstr(c.r[3]);
    if (upper(path) == sRemovedPartial) {
        sRemovedPartial.clear();  // the half-written file: already removed by fwrite
    } else {
        log("[photo] remove %s refused: the port never deletes files of the screenshots folder", path.c_str());
    }
    setErr(0);
    c.r[3] = 0;
    logCall("remove", path, 0);
}

// rmdir(path): the session folder is forgotten, nothing on disk changes.
HLE(fn_8021CC0C) {
    if (!sActive) {
        callOriginal(c, "fn_8021CC0C");
        return;
    }
    const std::string path = gstr(c.r[3]);
    setErr(0);
    c.r[3] = 0;
    logCall("rmdir", path, 0);
}

// Erase All's two steps: "any photo data?" and the erase. While they run the card shows no
// photos, so the game says there is no photo data and nothing is removed.
HLE(fn_8008F950) {
    if (!sActive) {
        callOriginal(c, "fn_8008F950");
        return;
    }
    sHide = true;
    callOriginal(c, "fn_8008F950");
    sHide = false;
    log("[photo] Erase All: the card shows no photo data (the screenshots folder is never erased), guest frame %u",
        game::guestFrames());
}
HLE(fn_8008FB48) {
    if (!sActive) {
        callOriginal(c, "fn_8008FB48");
        return;
    }
    sHide = true;
    callOriginal(c, "fn_8008FB48");
    sHide = false;
}

// --- F1 > Game > SD Card -------------------------------------------------------------------------

void sdCardMenu(bool& dirty) {
    ImGui::SeparatorText("SD Card");
    auto& s = port::settings().sdCard;
    bool exists = false;
    const fs::path dir = shownFolder(&exists);
    const std::string where = utf8(dir);
    dirty |= menu::Checkbox("Save photos to the screenshots folder (SD Card)", &s.photosToFolder);
    ImGui::SetItemTooltip(
        "Photos you take with the camera button (1) are saved as JPEG files when you choose\n"
        "Photos > SD Card > Save Photo, into the folder 'screenshots' next to CityFolkRecomp.exe\n"
        "(the data folder's 'screenshots' if the program folder cannot be written):\n"
        "  %s\n"
        "The file is the game's own photo: its resolution and picture, with Enhanced lighting and\n"
        "the other graphics options as they were when you took it. Names RUU_0001.JPG, RUU_0002.JPG, ...;\n"
        "existing files are never replaced. Erase All never deletes anything from this folder\n"
        "(the game says there is no photo data on the card).\n"
        "Off: as on a Wii without an SD Card (the game says no card is inserted).",
        where.c_str());
    if (const char* v = std::getenv("CF_SD_PHOTOS"); v && *v) {
        ImGui::SameLine();
        ImGui::TextDisabled("(CF_SD_PHOTOS=%s)", v);
    }
    ImGui::BeginDisabled(!exists);
    if (menu::Button("Open screenshots folder")) {
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", port::plainPath(dir).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        std::string url = "file://" + where;
        for (size_t i = 0; i < url.size(); i++)
            if (url[i] == ' ') url.replace(i, 1, "%20");
        SDL_OpenURL(url.c_str());
#endif
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(exists ? "Opens %s" : "%s\nis made when you save the first photo.", where.c_str());
    ImGui::SameLine();
    const int saved = sSaved.load();
    ImGui::TextDisabled("%s", saved ? (std::to_string(saved) + (saved == 1 ? " photo" : " photos") + " saved this session").c_str()
                                    : (exists ? "" : "(made at the first save)"));
}

}  // namespace recomp::cf
