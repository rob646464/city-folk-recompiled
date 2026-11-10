// Save import / export (cf_save_import.h; docs/NAND.md "Importing a Wii or Dolphin save").
//
// - Sources: a folder (Dolphin's user folder or its Wii NAND, an extracted Wii NAND backup, a title
//   folder, the data folder itself, a folder holding an SD card's private/wii/title/RUUx/data.bin)
//   or a file (rvforest.dat, banner.bin, data.bin); Dolphin's default user folders found by a click.
// - Checks: the file's size and every checksum the game keeps (cfsave::inspectSave), the region:
//   the save's title (folder name or data.bin header; a loose rvforest.dat: its town byte) against
//   the running version. The save format and checksums are the same in all four versions and the
//   game loads another version's save (docs/SETUP.md "Version-aware code"), so a mismatch is
//   refused unless the player ticks "import anyway".
// - Applied at the next start, before the game opens a NAND file (applyAtStart from ios_fs.cpp):
//   the staged files (<data>/save_import/pending/) replace rvforest.dat and banner.bin after the
//   current data folder was copied to <data>/save_backups/<timestamp>/ and the copy verified.
//   Restoring a backup is staged the same way (and backs up the save it replaces first).
// - Export: the port's save into a Dolphin user folder's Wii/title/00010000/<title>/data/ (whatever
//   was there is backed up first).
#include "game/cf_save_import.h"

#include "game/cf_save_data.h"
#include "port/install.h"
#include "port/settings.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/ios.h"
#include "recomp/menu.h"
#include "recomp/ui_scale.h"

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace recomp::cf::saveimport {

namespace {

namespace fs = std::filesystem;
using namespace cfsave;

constexpr const char* kSaveName = "rvforest.dat";
constexpr const char* kBannerName = "banner.bin";
// The game's own files are created owner read/write only (NAND.md "The game's save").
const ios::Attr kGameFileAttr{0x1001, 0x3031, 3, 0, 0, 0};

std::string pathText(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path fromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool readFile(const fs::path& p, std::vector<u8>& out, size_t maxSize = 64u << 20) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    if (ec || n > maxSize) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.resize(size_t(n));
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(n));
    return bool(in) || n == 0;
}

// Writes next to the target, then renames over it: a half-written file never replaces anything.
bool writeFileAtomic(const fs::path& p, const std::vector<u8>& data, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".cf-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        out.flush();
        if (!out) {
            fs::remove(tmp, ec);
            return err = "could not write " + pathText(tmp), false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return err = "could not replace " + pathText(p) + ": " + ec.message(), false;
    }
    return true;
}

bool sameContents(const fs::path& a, const std::vector<u8>& data) {
    std::vector<u8> b;
    return readFile(a, b) && b == data;
}

std::string timeText(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}
std::time_t toTimeT(fs::file_time_type ft) {
    // Portable (libc++ has no clock_cast for file_clock everywhere): shift by "now" on both clocks.
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sys);
}
std::string fileTimeText(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? std::string() : timeText(toTimeT(t));
}

// ---- data folder layout ----------------------------------------------------------------------

fs::path importDir() { return port::dataDir() / "save_import"; }
fs::path pendingDir() { return importDir() / "pending"; }
fs::path backupsDir() { return port::dataDir() / "save_backups"; }

u32 runningCode() { return u32(ios::titleId()); }
Region runningRegion() { return regionOfGameCode(runningCode()); }

using Ini = std::map<std::string, std::string>;
Ini readIni(const fs::path& p) {
    Ini ini;
    std::ifstream in(p, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        ini[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return ini;
}
bool writeIni(const fs::path& p, const Ini& ini, std::string& err) {
    std::string s;
    for (const auto& [k, v] : ini) s += k + "=" + v + "\n";
    return writeFileAtomic(p, std::vector<u8>(s.begin(), s.end()), err);
}

std::string residentsText(const SaveInfo& si) {
    std::string s;
    for (size_t i = 0; i < si.residents.size(); i++) s += (i ? ", " : "") + si.residents[i];
    return s;
}

// A new folder name for a backup: the local time, "-2", "-3" ... when taken.
fs::path newBackupDir(const char* suffix) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char b[40];
    std::strftime(b, sizeof b, "%Y-%m-%d_%H-%M-%S", &tm);
    std::string base = std::string(b) + suffix;
    std::error_code ec;
    fs::path d = backupsDir() / base;
    for (int n = 2; fs::exists(d, ec); n++) d = backupsDir() / (base + "-" + std::to_string(n));
    return d;
}

std::string attrText(const ios::Attr& a) {
    char b[48];
    std::snprintf(b, sizeof b, "%08X %04X %u %u %u %u", a.owner, a.group, a.ownerPerm, a.groupPerm, a.otherPerm, a.attr);
    return b;
}
ios::Attr parseAttr(const std::string& s) {
    ios::Attr a = kGameFileAttr;
    unsigned owner = 0, group = 0, op = 3, gp = 0, xp = 0, at = 0;
    if (std::sscanf(s.c_str(), "%x %x %u %u %u %u", &owner, &group, &op, &gp, &xp, &at) == 6)
        a = {owner, u16(group), u8(op), u8(gp), u8(xp), u8(at)};
    return a;
}

// Copies every file of `dir` into <backup>/files and checks each copy; writes backup.ini.
// `files` lists what was copied. False (and nothing kept) when a copy fails.
bool backupFolder(const fs::path& dir, const fs::path& backup, const std::string& reason, const std::string& nandDir,
                  const std::function<ios::Attr(const std::string&)>& attrOf, std::string& err) {
    std::error_code ec;
    fs::create_directories(backup / "files", ec);
    Ini ini;
    ini["reason"] = reason;
    ini["when"] = timeText(std::time(nullptr));
    ini["from"] = nandDir.empty() ? pathText(dir) : nandDir;
    if (!nandDir.empty()) ini["title_dir"] = nandDir;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string name = pathText(e.path().filename());
        if (name.size() > 7 && name.compare(name.size() - 7, 7, ".cf-tmp") == 0) continue;
        std::vector<u8> data;
        if (!readFile(e.path(), data) || !writeFileAtomic(backup / "files" / e.path().filename(), data, err) ||
            !sameContents(backup / "files" / e.path().filename(), data)) {
            if (err.empty()) err = "could not copy " + name + " into the backup";
            fs::remove_all(backup, ec);
            return false;
        }
        ini["file." + name] = std::to_string(data.size());
        if (attrOf && !nandDir.empty()) ini["attr." + name] = attrText(attrOf(nandDir + "/" + name));
        if (name == kSaveName) {
            const SaveInfo si = inspectSave(data);
            ini["town"] = si.town;
            ini["residents"] = residentsText(si);
            ini["save_time"] = fileTimeText(e.path());
            ini["save_valid"] = si.valid() ? "1" : "0";
        }
    }
    if (!writeIni(backup / "backup.ini", ini, err)) {
        fs::remove_all(backup, ec);
        return false;
    }
    return true;
}

// ---- sources ------------------------------------------------------------------------------------

struct Source {
    std::string kind;   // "Dolphin's NAND", "Wii NAND folder", ...
    fs::path where;     // folder holding rvforest.dat, or the data.bin file
    u32 code = 0;       // title's game code from the path or the data.bin header (0 = not known)
    bool isDataBin = false;
    DataBin bin;
    std::vector<u8> binBytes;
    bool decrypted = false;
    std::vector<u8> save, banner;
    SaveInfo info;
    std::string error;  // could not be read / not decrypted yet
    std::string time;   // file date

    // The title the save belongs to: its folder / header, else the region the town was made in.
    u32 effectiveCode() const {
        if (code) return code;
        return info.townRegion != kUnknown ? kGameCodes[info.townRegion] : 0;
    }
    Region region() const { return regionOfGameCode(effectiveCode()); }
    bool readable() const { return error.empty() && info.sizeOk; }
};

// Session-only SD key / IV (never written anywhere).
struct Key {
    bool set = false;
    u8 key[16]{}, iv[16]{};
} sKey;

void fillFromSave(Source& s) {
    s.info = inspectSave(s.save);
    if (!s.info.sizeOk) s.error = s.info.problem();
}

bool decryptSource(Source& s) {
    if (!s.isDataBin || s.decrypted) return s.decrypted;
    if (!sKey.set) {
        s.error = "needs the SD key";
        return false;
    }
    DataBinContents c;
    std::string err;
    if (!decryptDataBin(s.binBytes, s.bin, sKey.key, sKey.iv, c, err)) {
        s.error = err;
        return false;
    }
    s.decrypted = true;
    s.error.clear();
    s.banner = c.banner;
    bool found = false;
    for (auto& [name, data] : c.files)
        if (lower(name) == kSaveName) s.save = std::move(data), found = true;
    if (!found) {
        s.error = "the SD-card save holds no rvforest.dat";
        return false;
    }
    fillFromSave(s);
    return s.error.empty();
}

Source loadFolder(const fs::path& dir, u32 code, const std::string& kind) {
    Source s;
    s.kind = kind;
    s.where = dir;
    s.code = code;
    if (!readFile(dir / kSaveName, s.save, kSaveSize * 2)) s.error = "rvforest.dat could not be read";
    else fillFromSave(s);
    std::vector<u8> b;
    if (readFile(dir / kBannerName, b, kBannerMaxSize) && bannerLooksValid(b)) s.banner = std::move(b);
    s.time = fileTimeText(dir / kSaveName);
    return s;
}

Source loadDataBin(const fs::path& file) {
    Source s;
    s.kind = "Wii SD card export (data.bin)";
    s.where = file;
    s.isDataBin = true;
    s.time = fileTimeText(file);
    std::string err;
    if (!readFile(file, s.binBytes, 16u << 20) || !parseDataBin(s.binBytes, s.bin, err)) {
        s.error = err.empty() ? "could not be read" : err;
        return s;
    }
    if (u32(s.bin.titleId >> 32) == 0x00010000) s.code = u32(s.bin.titleId);
    if (regionOfGameCode(s.code) == kUnknown) {
        s.error = "it is the save of another game (" + gameCodeText(u32(s.bin.titleId)) + ")";
        return s;
    }
    decryptSource(s);
    return s;
}

// The title code named by a folder on the path ("52555545"), 0 if none.
u32 codeFromPath(const fs::path& p) {
    for (fs::path q = p; !q.empty(); q = q.parent_path()) {
        const std::string n = lower(pathText(q.filename()));
        for (u32 c : kGameCodes)
            if (n == titleHex(c) || n == lower(gameCodeText(c))) return c;
        if (q == q.parent_path()) break;
    }
    return 0;
}

bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// Dolphin's NAND root of a user folder: Config/Dolphin.ini [General] NANDRootPath, else <user>/Wii.
fs::path dolphinNandRoot(const fs::path& user) {
    std::ifstream in(user / "Config" / "Dolphin.ini", std::ios::binary);
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') section = line;
        if (section != "[General]") continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        if (k == "NANDRootPath" && !v.empty()) {
            fs::path r = fromU8(v);
            if (r.is_relative()) r = user / r;
            return r;
        }
    }
    return user / "Wii";
}
bool looksLikeDolphinUser(const fs::path& d) { return isDir(d / "Wii") || isFile(d / "Config" / "Dolphin.ini"); }

void addUnique(std::vector<Source>& out, Source s) {
    for (const auto& o : out) {
        std::error_code ec;
        if (fs::equivalent(o.where, s.where, ec)) return;
    }
    out.push_back(std::move(s));
}

// Every save at or below a picked path, in the layouts people have them in.
std::vector<Source> scan(const fs::path& picked) {
    std::vector<Source> out;
    if (isFile(picked)) {
        const std::string n = lower(pathText(picked.filename()));
        std::error_code ec;
        const auto size = fs::file_size(picked, ec);
        if (n == kSaveName || n == kBannerName) {
            return scan(picked.parent_path());
        } else if (n == "data.bin" || (size > kDataBinHeaderSize + kBkHeaderSize && size != kSaveSize)) {
            out.push_back(loadDataBin(picked));
        } else if (size == kSaveSize) {  // a renamed rvforest.dat
            Source s;
            s.kind = "save file";
            s.where = picked.parent_path();
            s.code = codeFromPath(picked.parent_path());
            readFile(picked, s.save, kSaveSize);
            fillFromSave(s);
            s.time = fileTimeText(picked);
            out.push_back(std::move(s));
        } else {
            return scan(picked.parent_path());
        }
        return out;
    }
    if (!isDir(picked)) return out;
    auto tryFolder = [&](const fs::path& d, const std::string& kind) {
        if (isFile(d / kSaveName)) addUnique(out, loadFolder(d, codeFromPath(d), kind));
    };
    auto tryNandRoot = [&](const fs::path& root, const std::string& kind) {
        for (u32 c : kGameCodes) tryFolder(root / "title" / "00010000" / titleHex(c) / "data", kind);
    };
    auto tryDataBins = [&](const fs::path& d) {
        for (u32 c : kGameCodes)
            for (const char* sub : {"private/wii/title", "private/WII/TITLE", "wii/title", "title", ""}) {
                const fs::path f = (*sub ? d / fs::path(sub).make_preferred() : d) / gameCodeText(c) / "data.bin";
                if (isFile(f)) addUnique(out, loadDataBin(f));
            }
        if (isFile(d / "data.bin")) addUnique(out, loadDataBin(d / "data.bin"));
    };
    tryFolder(picked, "save folder");
    tryFolder(picked / "data", "title folder");
    tryNandRoot(picked, "Wii NAND folder");
    if (looksLikeDolphinUser(picked)) tryNandRoot(dolphinNandRoot(picked), "Dolphin's NAND");
    tryNandRoot(picked / "nand", "Wii NAND folder");
    for (u32 c : kGameCodes) {
        tryFolder(picked / titleHex(c) / "data", "title folder");
        tryFolder(picked / "00010000" / titleHex(c) / "data", "Wii NAND folder");
    }
    tryDataBins(picked);
    return out;
}

// Dolphin user folders on this PC (CF_DOLPHIN_DIRS = ';'-separated list replaces the defaults).
std::vector<fs::path> dolphinUserDirs() {
    std::vector<fs::path> c;
    if (const char* env = std::getenv("CF_DOLPHIN_DIRS")) {
        std::stringstream ss(env);
        std::string item;
        while (std::getline(ss, item, ';'))
            if (!item.empty()) c.push_back(fromU8(item));
    } else {
#ifdef _WIN32
        wchar_t buf[2048];
        DWORD size = sizeof buf;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Dolphin Emulator", L"UserConfigPath", RRF_RT_REG_SZ, nullptr,
                         buf, &size) == ERROR_SUCCESS)
            c.push_back(fs::path(buf));
        if (const fs::path a = port::envPath("APPDATA"); !a.empty()) c.push_back(a / "Dolphin Emulator");
        PWSTR docs = nullptr;  // Documents, wherever it is redirected (OneDrive, another drive)
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs)
            c.push_back(fs::path(docs) / "Dolphin Emulator");
        CoTaskMemFree(docs);
        if (const fs::path u = port::envPath("USERPROFILE"); !u.empty()) c.push_back(u / "Documents" / "Dolphin Emulator");
#else
        const fs::path home = port::envPath("HOME");
#ifdef __APPLE__
        if (!home.empty()) c.push_back(home / "Library" / "Application Support" / "Dolphin");
#else
        if (const fs::path x = port::envPath("XDG_DATA_HOME"); !x.empty()) c.push_back(x / "dolphin-emu");
        if (!home.empty()) {
            c.push_back(home / ".local" / "share" / "dolphin-emu");
            c.push_back(home / ".dolphin-emu");
            c.push_back(home / ".var" / "app" / "org.DolphinEmu.dolphin-emu" / "data" / "dolphin-emu");
        }
#endif
#endif
    }
    std::vector<fs::path> out;
    for (const auto& d : c) {
        if (!looksLikeDolphinUser(d)) continue;
        bool dup = false;
        for (const auto& o : out) {
            std::error_code ec;
            dup |= fs::equivalent(o, d, ec);
        }
        if (!dup) out.push_back(d);
    }
    return out;
}

// ---- the region rule -----------------------------------------------------------------------------

// "" = may be imported into the running version; otherwise why not (allowOther lifts a region
// mismatch, nothing else).
std::string importProblem(const Source& s, bool allowOther) {
    if (!s.error.empty()) return s.error;
    if (!s.info.valid()) return s.info.problem();
    const Region r = s.region();
    if (r == kUnknown) return "the save's version is not known";
    if (r != runningRegion() && !allowOther)
        return std::string("this is a save of the ") + regionName(r) + " version, and you play the " +
               regionName(runningRegion()) + " version";
    return "";
}

// ---- staging and applying --------------------------------------------------------------------------

std::mutex sM;
std::string sStartResult;  // what this start's import / restore did
bool sStartFailed = false;

bool stageImport(const Source& s, bool allowOther, std::string& err) {
    const std::string why = importProblem(s, allowOther);
    if (!why.empty()) return err = "Not imported: " + why + ".", false;
    std::error_code ec;
    const fs::path tmp = importDir() / "pending.cf-tmp";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    Ini ini;
    ini["kind"] = "import";
    ini["title_dir"] = ios::titleDataDir();
    ini["source"] = s.kind + ": " + pathText(s.where);
    ini["source_version"] = regionName(s.region());
    ini["other_version"] = s.region() != runningRegion() ? "1" : "0";
    ini["town"] = s.info.town;
    ini["residents"] = residentsText(s.info);
    ini["staged"] = timeText(std::time(nullptr));
    if (!writeFileAtomic(tmp / kSaveName, s.save, err)) return false;
    if (bannerLooksValid(s.banner) && !writeFileAtomic(tmp / kBannerName, s.banner, err)) return false;
    if (!writeIni(tmp / "pending.ini", ini, err)) return false;
    fs::remove_all(pendingDir(), ec);
    fs::rename(tmp, pendingDir(), ec);
    if (ec) return err = "could not stage the import: " + ec.message(), false;
    log("[save] import staged: %s (town %s, %s version) -> %s at the next start", ini["source"].c_str(),
        s.info.town.c_str(), regionName(s.region()), ini["title_dir"].c_str());
    return true;
}

bool stageRestore(const std::string& backupName, std::string& err) {
    std::error_code ec;
    if (!isFile(backupsDir() / fromU8(backupName) / "backup.ini")) return err = "That backup no longer exists.", false;
    const fs::path tmp = importDir() / "pending.cf-tmp";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const Ini b = readIni(backupsDir() / fromU8(backupName) / "backup.ini");
    Ini ini;
    ini["kind"] = "restore";
    ini["backup"] = backupName;
    const auto t = b.find("title_dir");
    ini["title_dir"] = t != b.end() ? t->second : ios::titleDataDir();
    ini["staged"] = timeText(std::time(nullptr));
    if (!writeIni(tmp / "pending.ini", ini, err)) return false;
    fs::remove_all(pendingDir(), ec);
    fs::rename(tmp, pendingDir(), ec);
    if (ec) return err = "could not stage the restore: " + ec.message(), false;
    log("[save] restore of backup %s staged for the next start", backupName.c_str());
    return true;
}

bool validTitleDir(const std::string& d) {
    // /title/00010000/5255554x/data of one of the four versions
    for (u32 c : kGameCodes)
        if (d == "/title/00010000/" + titleHex(c) + "/data") return true;
    return false;
}

std::vector<AttrChange> applyPending(const std::function<ios::Attr(const std::string&)>& attrOf) {
    std::vector<AttrChange> changes;
    const Ini p = readIni(pendingDir() / "pending.ini");
    const auto get = [&](const char* k) {
        const auto it = p.find(k);
        return it == p.end() ? std::string() : it->second;
    };
    const std::string kind = get("kind"), titleDir = get("title_dir");
    std::error_code ec;
    auto fail = [&](const std::string& why) {
        sStartResult = (kind == "restore" ? "The backup was not restored: " : "The save was not imported: ") + why +
                       ". Your save is unchanged.";
        sStartFailed = true;
        log("[save] ERROR: %s", sStartResult.c_str());
        const fs::path failed = importDir() / "failed";
        fs::remove_all(failed, ec);
        fs::rename(pendingDir(), failed, ec);  // not retried at every start
        return std::vector<AttrChange>{};
    };
    if ((kind != "import" && kind != "restore") || !validTitleDir(titleDir)) return fail("the staged request is damaged");

    // What goes in: name -> data, and the attributes for each.
    std::vector<std::pair<std::string, std::vector<u8>>> in;
    std::map<std::string, ios::Attr> attrs;
    std::string what;
    if (kind == "import") {
        std::vector<u8> save, banner;
        if (!readFile(pendingDir() / kSaveName, save)) return fail("the staged rvforest.dat is missing");
        const SaveInfo si = inspectSave(save);
        if (!si.valid()) return fail("the staged rvforest.dat is not valid (" + si.problem() + ")");
        in.emplace_back(kSaveName, std::move(save));
        if (readFile(pendingDir() / kBannerName, banner) && bannerLooksValid(banner)) {
            // Another version's banner has that version's text: keep this version's own when there
            // is one (the game writes it again at every save anyway).
            if (get("other_version") != "1" || !isFile(ios::hostPath(titleDir + "/" + kBannerName)))
                in.emplace_back(kBannerName, std::move(banner));
        }
        what = "town " + si.town + " (" + residentsText(si) + ") from " + get("source");
    } else {
        const fs::path b = backupsDir() / fromU8(get("backup"));
        const Ini bi = readIni(b / "backup.ini");
        if (bi.empty()) return fail("backup " + get("backup") + " is gone");
        for (const auto& e : fs::directory_iterator(b / "files", ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::vector<u8> data;
            const std::string name = pathText(e.path().filename());
            if (!readFile(e.path(), data)) return fail("backup file " + name + " could not be read");
            const auto sz = bi.find("file." + name);
            if (sz != bi.end() && sz->second != std::to_string(data.size()))
                return fail("backup file " + name + " has the wrong size");
            const auto at = bi.find("attr." + name);
            attrs[name] = at != bi.end() ? parseAttr(at->second) : kGameFileAttr;
            in.emplace_back(name, std::move(data));
        }
        const auto town = bi.find("town");
        what = "backup " + get("backup") + (town != bi.end() && !town->second.empty() ? " (town " + town->second + ")" : "");
    }

    // Back up what is there now, and check the copy, before anything is replaced.
    const fs::path target = ios::hostPath(titleDir);
    fs::create_directories(target, ec);
    bool any = false;
    for (const auto& e : fs::directory_iterator(target, ec)) any |= e.is_regular_file(ec);
    std::string backupName;
    if (any) {
        const fs::path b = newBackupDir(kind == "import" ? "" : "-before-restore");
        std::string err;
        const std::string reason =
            kind == "import" ? "before importing " + what : "before restoring " + what;
        if (!backupFolder(target, b, reason, titleDir, attrOf, err)) return fail("the backup failed (" + err + ")");
        backupName = pathText(b.filename());
        log("[save] backup of %s -> %s", titleDir.c_str(), pathText(b).c_str());
    }

    // Replace. A restore puts the folder back as it was (files that were not in it go; they are in
    // the backup just made); an import replaces the save and its banner and keeps the rest (the
    // WiiConnect24 files the game makes itself).
    if (kind == "restore") {
        for (const auto& e : fs::directory_iterator(target, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string name = pathText(e.path().filename());
            if (std::none_of(in.begin(), in.end(), [&](const auto& f) { return f.first == name; })) {
                fs::remove(e.path(), ec);
                changes.push_back({titleDir + "/" + name, true, {}});
            }
        }
    }
    for (const auto& [name, data] : in) {
        std::string err;
        if (!writeFileAtomic(target / fromU8(name), data, err) || !sameContents(target / fromU8(name), data)) {
            // Half done: put the backup back so the folder is never a mix.
            sStartResult = "Writing " + name + " failed (" + err + ")";
            if (!backupName.empty()) {
                for (const auto& e : fs::directory_iterator(backupsDir() / fromU8(backupName) / "files", ec)) {
                    std::vector<u8> d;
                    std::string e2;
                    if (readFile(e.path(), d)) writeFileAtomic(target / e.path().filename(), d, e2);
                }
                sStartResult += "; the previous save was put back from backup " + backupName;
            }
            return fail(sStartResult);
        }
        const auto at = attrs.find(name);
        changes.push_back({titleDir + "/" + name, false, at != attrs.end() ? at->second : kGameFileAttr});
    }
    fs::remove_all(pendingDir(), ec);
    sStartResult = (kind == "import" ? "Imported " : "Restored ") + what + "." +
                   (backupName.empty() ? " (There was no save before.)" : " The save it replaced is in backup " + backupName + ".");
    sStartFailed = false;
    log("[save] %s", sStartResult.c_str());
    return changes;
}

}  // namespace

std::vector<AttrChange> applyAtStart(const std::function<ios::Attr(const std::string&)>& attrOf) {
    std::lock_guard lk(sM);
    std::error_code ec;
    // Test aids: CF_SAVE_IMPORT=<folder or file>[;any] stages an import now (first save found that
    // the running version may take; "any" ticks "import anyway"); CF_SD_KEY / CF_SD_IV = the
    // player's SD key and IV (hex) for a data.bin; CF_SAVE_RESTORE=<backup name|latest>.
    if (const char* imp = std::getenv("CF_SAVE_IMPORT"); imp && *imp) {
        std::string spec = imp;
        bool any = false;
        if (const size_t semi = spec.rfind(';'); semi != std::string::npos && spec.substr(semi + 1) == "any")
            any = true, spec.resize(semi);
        if (const char* k = std::getenv("CF_SD_KEY"), *v = std::getenv("CF_SD_IV"); k && v)
            sKey.set = parseKey16(k, sKey.key) && parseKey16(v, sKey.iv);
        auto found = scan(fromU8(spec));
        std::string err = found.empty() ? "no City Folk save found at " + spec : "";
        bool staged = false;
        for (auto& s : found) {
            const std::string why = importProblem(s, any);
            log("[save] CF_SAVE_IMPORT found %s: %s (%s version, town %s, residents %s, %s)%s%s", s.kind.c_str(),
                pathText(s.where).c_str(), regionName(s.region()), s.info.town.c_str(), residentsText(s.info).c_str(),
                s.time.c_str(), why.empty() ? "" : " - refused: ", why.c_str());
            if (!staged && why.empty()) staged = stageImport(s, any, err);
        }
        if (!staged) {
            sStartResult = "CF_SAVE_IMPORT: nothing imported" + (err.empty() ? std::string(".") : ": " + err);
            sStartFailed = true;
            log("[save] %s", sStartResult.c_str());
        }
    }
    if (const char* r = std::getenv("CF_SAVE_RESTORE"); r && *r) {
        std::string name = r, err;
        if (name == "latest") {
            name.clear();
            for (const auto& e : fs::directory_iterator(backupsDir(), ec))
                if (isFile(e.path() / "backup.ini") && pathText(e.path().filename()) > name) name = pathText(e.path().filename());
        }
        if (name.empty() || !stageRestore(name, err)) log("[save] CF_SAVE_RESTORE: %s", name.empty() ? "no backup" : err.c_str());
    }
    if (!isFile(pendingDir() / "pending.ini")) return {};
    return applyPending(attrOf);
}

// ---- F1 menu ----------------------------------------------------------------------------------

namespace {

struct Backup {
    std::string name, when, reason, town, residents, saveTime;
};

struct Ui {
    bool importOpen = false, exportOpen = false;
    std::vector<Source> found;
    int sel = -1;
    bool allowOther = false;
    std::string status;
    bool statusErr = false;
    std::vector<Backup> backups;
    bool backupsScanned = false;
    std::vector<fs::path> dolphins;
    bool dolphinsScanned = false;
    char keyText[80] = "", ivText[80] = "";
    std::string pendingText;  // what waits for the next start
    std::string restoreName, exportTarget;
} ui;

std::mutex sPickM;
std::string sPicked, sPickedKeys, sPickedExport, sDialogError;

void SDLCALL onPick(void*, const char* const* files, int) {
    std::lock_guard lk(sPickM);
    if (!files) sDialogError = SDL_GetError();
    else if (files[0]) sPicked = files[0];
}
void SDLCALL onPickKeys(void*, const char* const* files, int) {
    std::lock_guard lk(sPickM);
    if (!files) sDialogError = SDL_GetError();
    else if (files[0]) sPickedKeys = files[0];
}
void SDLCALL onPickExport(void*, const char* const* files, int) {
    std::lock_guard lk(sPickM);
    if (!files) sDialogError = SDL_GetError();
    else if (files[0]) sPickedExport = files[0];
}

void setStatus(const std::string& s, bool err = false) {
    ui.status = s;
    ui.statusErr = err;
    log("[save] %s", s.c_str());
}

void scanBackups() {
    ui.backups.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(backupsDir(), ec)) {
        const Ini b = readIni(e.path() / "backup.ini");
        if (b.empty()) continue;
        auto g = [&](const char* k) {
            const auto it = b.find(k);
            return it == b.end() ? std::string() : it->second;
        };
        ui.backups.push_back({pathText(e.path().filename()), g("when"), g("reason"), g("town"), g("residents"), g("save_time")});
    }
    std::sort(ui.backups.begin(), ui.backups.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
    ui.backupsScanned = true;
}

void refreshPending() {
    const Ini p = readIni(pendingDir() / "pending.ini");
    ui.pendingText.clear();
    if (p.empty()) return;
    auto g = [&](const char* k) {
        const auto it = p.find(k);
        return it == p.end() ? std::string() : it->second;
    };
    ui.pendingText = g("kind") == "restore" ? "Backup " + g("backup") + " is restored at the next start."
                                            : "Town " + g("town") + " (" + g("residents") + ") is imported at the next start.";
}

void usePicked(const std::string& path) {
    ui.found = scan(fromU8(path));
    ui.sel = -1;
    ui.allowOther = false;
    for (size_t i = 0; i < ui.found.size(); i++)
        if (ui.sel < 0 && importProblem(ui.found[i], false).empty()) ui.sel = int(i);
    if (ui.sel < 0 && !ui.found.empty()) ui.sel = 0;
    if (ui.found.empty())
        setStatus("No City Folk save found at " + path + ". Pick the folder holding rvforest.dat, a Dolphin user "
                  "folder, a Wii NAND folder, or a data.bin.", true);
    else
        setStatus("Found " + std::to_string(ui.found.size()) + " save" + (ui.found.size() > 1 ? "s" : "") + " at " + path + ".");
}

void findDolphin() {
    ui.dolphins = dolphinUserDirs();
    ui.dolphinsScanned = true;
}

// True while the game has a file of its data folder (or a /tmp safe copy) open: a save is being written.
bool gameBusy() { return ios::filesOpenUnder(ios::titleDataDir()) || ios::filesOpenUnder("/tmp"); }

bool exportTo(const fs::path& picked, std::string& msg) {
    if (gameBusy()) return msg = "The game is saving right now; try again in a moment.", false;
    fs::path root;
    if (isDir(picked / "title") || isDir(picked / "shared2") || lower(pathText(picked.filename())) == "wii") root = picked;
    else if (looksLikeDolphinUser(picked)) root = dolphinNandRoot(picked);
    else return msg = "This does not look like a Dolphin user folder (it has no Wii folder or Config\\Dolphin.ini).", false;
    std::vector<u8> save, banner;
    const std::string dir = ios::titleDataDir();
    if (!ios::readHostFile(dir + "/" + kSaveName, save)) return msg = "There is no save to export yet.", false;
    const SaveInfo si = inspectSave(save);
    if (!si.valid()) return msg = "The port's save is not valid (" + si.problem() + "); nothing exported.", false;
    ios::readHostFile(dir + "/" + kBannerName, banner);
    const fs::path target = root / "title" / "00010000" / titleHex(runningCode()) / "data";
    std::string err, backupNote;
    std::error_code ec;
    bool any = false;
    for (const auto& e : fs::directory_iterator(target, ec)) any |= e.is_regular_file(ec);
    if (any) {
        const fs::path b = newBackupDir("-dolphin");
        if (!backupFolder(target, b, "Dolphin's save in " + pathText(target) + ", before an export from the port", "", {}, err))
            return msg = "Could not back up Dolphin's save there (" + err + "); nothing exported.", false;
        backupNote = " Dolphin's previous save is in backup " + pathText(b.filename()) + ".";
    }
    if (!writeFileAtomic(target / kSaveName, save, err) ||
        (bannerLooksValid(banner) && !writeFileAtomic(target / kBannerName, banner, err)))
        return msg = "Export failed: " + err, false;
    msg = "Exported town " + si.town + " (" + residentsText(si) + ") to " + pathText(target) + "." + backupNote;
    log("[save] %s", msg.c_str());
    return true;
}

void restart(bool now) {
    if (!now) return;
    if (std::getenv("CF_SAVE_NO_RELAUNCH")) {  // test runs: quit instead of starting again
        menu::requestQuit();
        return;
    }
    port::saveSettings();
    port::relaunch();
}

void drawSource(int i, Source& s) {
    ImGui::PushID(i);
    const std::string why = importProblem(s, ui.allowOther);
    ImGui::BeginDisabled(!s.readable() && !(s.isDataBin && !s.decrypted));
    ImGui::RadioButton("##pick", &ui.sel, i);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s: %s", s.kind.c_str(), pathText(s.where).c_str());
    if (s.info.sizeOk) {
        const u32 code = s.effectiveCode();
        ImGui::Text("%s version (%s)%s: town %s, residents %s", regionName(s.region()), gameCodeText(code).c_str(),
                    s.code ? "" : " by its town data", s.info.town.c_str(), residentsText(s.info).c_str());
        if (!s.time.empty()) ImGui::TextDisabled("File date %s", s.time.c_str());
    }
    if (!why.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.45f, 1.0f));
        ImGui::TextWrapped("Cannot import: %s.", why.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "OK: every checksum matches.");
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void keySection(Source& s) {
    ImGui::TextWrapped(
        "data.bin is encrypted with the Wii's SD-card key, a Nintendo key this port does not include. "
        "Easiest: import data.bin into Dolphin (Tools > Import Wii Save...) and pick Dolphin's folder here, or "
        "copy the plain save files from a Wii NAND backup (BootMii nand.bin opened with a NAND extractor: "
        "title/00010000/%s/data). (BootMii's keys.bin holds your console's own keys, not the SD key.)",
        titleHex(s.code).c_str());
    ImGui::TextWrapped("If you have the SD key and IV yourself, enter them (32 hex digits each) or load the two "
                       "files sd-key and sd-iv; they are kept only until the game closes.");
    ImGui::SetNextItemWidth(330.0f * ui::scale());
    ImGui::InputText("SD key##sdkey", ui.keyText, sizeof ui.keyText, ImGuiInputTextFlags_Password);
    ImGui::SetNextItemWidth(330.0f * ui::scale());
    ImGui::InputText("SD IV##sdiv", ui.ivText, sizeof ui.ivText, ImGuiInputTextFlags_Password);
    if (menu::Button("Use this key")) {
        Key k;
        k.set = parseKey16(ui.keyText, k.key) && parseKey16(ui.ivText, k.iv);
        if (!k.set) {
            setStatus("The key and the IV must be 32 hex digits each.", true);
        } else {
            sKey = k;
            s.error.clear();
            if (decryptSource(s)) setStatus("data.bin decrypted.");
            else setStatus("data.bin: " + s.error + ".", true), sKey.set = false;
        }
    }
    ImGui::SameLine();
    if (menu::Button("Load sd-key / sd-iv files...")) {
        if (const char* t = std::getenv("CF_SAVE_IMPORT_KEYS_PICK")) sPickedKeys = t;
        else SDL_ShowOpenFolderDialog(&onPickKeys, nullptr, aurora_get_window(), nullptr, false);
    }
    ImGui::SetItemTooltip("Pick the folder that holds the files sd-key and sd-iv (16 bytes each, or hex text).");
}

void importSection() {
    ImGui::Indent();
    ImGui::TextWrapped("Pick Dolphin's user folder (or its Wii folder), an extracted Wii NAND backup, the save's own "
                       "folder (rvforest.dat, banner.bin), or an SD card's data.bin (private/wii/title/%s/data.bin).",
                       gameCodeText(runningCode()).c_str());
    if (menu::Button("Pick a folder...")) {
        if (const char* t = std::getenv("CF_SAVE_IMPORT_PICK")) usePicked(t);
        else SDL_ShowOpenFolderDialog(&onPick, nullptr, aurora_get_window(), nullptr, false);
    }
    ImGui::SameLine();
    if (menu::Button("Pick a file...")) {
        static const SDL_DialogFileFilter kFilters[] = {{"Wii save (rvforest.dat, data.bin)", "dat;bin"}, {"All files", "*"}};
        if (const char* t = std::getenv("CF_SAVE_IMPORT_PICK")) usePicked(t);
        else SDL_ShowOpenFileDialog(&onPick, nullptr, aurora_get_window(), kFilters, 2, nullptr, false);
    }
    ImGui::SameLine();
    if (menu::Button("Find Dolphin saves")) {
        findDolphin();
        ui.found.clear();
        for (const auto& d : ui.dolphins)
            for (auto& s : scan(d)) addUnique(ui.found, std::move(s));
        ui.sel = -1;
        for (size_t i = 0; i < ui.found.size(); i++)
            if (ui.sel < 0 && importProblem(ui.found[i], false).empty()) ui.sel = int(i);
        if (ui.sel < 0 && !ui.found.empty()) ui.sel = 0;
        setStatus(ui.dolphins.empty() ? "No Dolphin user folder found; pick it with \"Pick a folder...\"."
                  : ui.found.empty()  ? "Dolphin was found, but it has no City Folk save."
                                      : "Found " + std::to_string(ui.found.size()) + " save(s) in Dolphin.",
                  ui.found.empty());
    }
    ImGui::SetItemTooltip("Looks in Dolphin's usual user folders (%%APPDATA%%\\Dolphin Emulator, Documents\\Dolphin "
                          "Emulator, the folder Dolphin's settings name).");

    for (int i = 0; i < int(ui.found.size()); i++) drawSource(i, ui.found[size_t(i)]);
    if (ui.sel >= 0 && ui.sel < int(ui.found.size())) {
        Source& s = ui.found[size_t(ui.sel)];
        if (s.isDataBin && !s.decrypted) keySection(s);
        const Region r = s.region();
        if (s.readable() && s.info.valid() && r != kUnknown && r != runningRegion()) {
            ImGui::TextWrapped("The save is from the %s version. City Folk's save is the same in every version and "
                               "the %s game loads it, but a Wii never would: the town keeps its own region's "
                               "holidays, and its text stays in the characters it was written in.",
                               regionName(r), regionName(runningRegion()));
            menu::Checkbox("Import it into this version anyway", &ui.allowOther);
        }
        const bool ok = importProblem(s, ui.allowOther).empty();
        const bool busy = gameBusy();
        ImGui::BeginDisabled(!ok || busy);
        bool now = false, later = false;
        now = menu::Button("Import and restart");
        ImGui::SameLine();
        later = menu::Button("Import at the next start");
        ImGui::EndDisabled();
        if (busy) {
            ImGui::SameLine();
            ImGui::TextDisabled("(the game is saving)");
        }
        if (now || later) {
            std::string err;
            if (stageImport(s, ui.allowOther, err)) {
                refreshPending();
                setStatus("Town " + s.info.town + " will be imported at the next start; your current save is backed up "
                          "first (save_backups).");
                ui.found.clear();
                ui.sel = -1;
                restart(now);
            } else {
                setStatus(err, true);
            }
        }
        ImGui::TextDisabled("The current save is copied to save_backups first. Unsaved progress is lost when it restarts.");
    }
    ImGui::Unindent();
}

void backupsSection() {
    if (!ui.backupsScanned) scanBackups();
    menu::openTreesForCheck();
    if (!ImGui::TreeNode("Backups##saves", "Backups (%d)", int(ui.backups.size()))) return;
    if (menu::Button("Open the backups folder")) {
        std::error_code ec;
        fs::create_directories(backupsDir(), ec);
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", port::plainPath(backupsDir()).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        std::string url = "file://" + pathText(backupsDir());
        for (size_t i = 0; i < url.size(); i++)
            if (url[i] == ' ') url.replace(i, 1, "%20");
        SDL_OpenURL(url.c_str());
#endif
    }
    ImGui::SameLine();
    if (menu::Button("Refresh##backups")) scanBackups();
    if (ui.backups.empty()) ImGui::TextDisabled("No backups yet: one is made before every import or restore.");
    for (const auto& b : ui.backups) {
        ImGui::PushID(b.name.c_str());
        ImGui::Text("%s", b.name.c_str());
        ImGui::SameLine();
        const bool dolphin = b.name.find("-dolphin") != std::string::npos;
        ImGui::BeginDisabled(dolphin || gameBusy());
        if (menu::SmallButton("Restore...")) {
            ui.restoreName = b.name;
            ImGui::OpenPopup("Restore this backup?");
        }
        ImGui::EndDisabled();
        if (dolphin) ImGui::SetItemTooltip("Dolphin's save from before an export (in the backups folder, to copy back by hand).");
        ImGui::Indent();
        if (!b.town.empty()) ImGui::TextDisabled("Town %s (%s)%s%s", b.town.c_str(), b.residents.c_str(),
                                                 b.saveTime.empty() ? "" : ", saved ", b.saveTime.c_str());
        ImGui::TextDisabled("%s", b.reason.c_str());
        ImGui::Unindent();
        if (ImGui::BeginPopupModal("Restore this backup?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Restore backup %s (town %s)?", ui.restoreName.c_str(), b.town.c_str());
            ImGui::TextDisabled("The current save is backed up first. The game restarts; unsaved progress is lost.");
            bool now = menu::Button("Restore and restart");
            ImGui::SameLine();
            bool later = menu::Button("Restore at the next start");
            ImGui::SameLine();
            if (menu::Button("Cancel##restore")) ImGui::CloseCurrentPopup();
            if (now || later) {
                std::string err;
                ImGui::CloseCurrentPopup();
                if (stageRestore(ui.restoreName, err)) {
                    refreshPending();
                    setStatus("Backup " + ui.restoreName + " will be restored at the next start.");
                    restart(now);
                } else {
                    setStatus(err, true);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void exportSection() {
    ImGui::Indent();
    ImGui::TextWrapped("Copies this save into a Dolphin user folder (Wii/title/00010000/%s/data). Close Dolphin first; "
                       "a save already there is copied to save_backups first.",
                       titleHex(runningCode()).c_str());
    if (!ui.dolphinsScanned) findDolphin();
    for (size_t i = 0; i < ui.dolphins.size(); i++) {
        ImGui::PushID(int(i));
        if (menu::SmallButton("Export here")) {
            std::string msg;
            const bool ok = exportTo(ui.dolphins[i], msg);
            setStatus(msg, !ok);
            ui.backupsScanned = false;
        }
        ImGui::SameLine();
        ImGui::TextWrapped("%s", pathText(ui.dolphins[i]).c_str());
        ImGui::PopID();
    }
    if (menu::Button("Pick a Dolphin folder...")) {
        if (const char* t = std::getenv("CF_SAVE_EXPORT_PICK")) sPickedExport = t;
        else SDL_ShowOpenFolderDialog(&onPickExport, nullptr, aurora_get_window(), nullptr, false);
    }
    ImGui::Unindent();
}

void loadKeyFolder(const std::string& path) {
    std::vector<u8> k, v;
    const fs::path d = fromU8(path);
    Key key;
    key.set = readFile(d / "sd-key", k, 256) && readFile(d / "sd-iv", v, 256) &&
              parseKey16(std::string(k.begin(), k.end()), key.key) && parseKey16(std::string(v.begin(), v.end()), key.iv);
    if (!key.set) {
        setStatus("No usable sd-key and sd-iv files in " + path + ".", true);
        return;
    }
    sKey = key;
    for (auto& s : ui.found)
        if (s.isDataBin && !s.decrypted) {
            s.error.clear();
            decryptSource(s);
        }
    setStatus("SD key loaded (kept until the game closes).");
}

}  // namespace

void menuSection() {
    {
        std::string picked, keys, exp, derr;
        {
            std::lock_guard lk(sPickM);
            std::swap(picked, sPicked);
            std::swap(keys, sPickedKeys);
            std::swap(exp, sPickedExport);
            std::swap(derr, sDialogError);
        }
        if (!picked.empty()) usePicked(picked);
        if (!keys.empty()) loadKeyFolder(keys);
        if (!exp.empty()) {
            std::string msg;
            const bool ok = exportTo(fromU8(exp), msg);
            setStatus(msg, !ok);
            ui.backupsScanned = false;
        }
        if (!derr.empty()) setStatus("The file dialog could not be opened: " + derr, true);
    }
    static bool once = (refreshPending(), true);
    (void)once;

    ImGui::SeparatorText("Save data");
    {
        // Read again only when the file changed (it is 4 MB).
        const std::string dir = ios::titleDataDir();
        const fs::path host = ios::hostPath(dir + "/" + kSaveName);
        static fs::file_time_type sTime{};
        static bool sHave = false;
        static SaveInfo sInfo;
        static std::string sWhen;
        std::error_code ec;
        const auto t = fs::last_write_time(host, ec);
        if (ec) {
            sHave = false;
        } else if (!sHave || t != sTime) {
            std::vector<u8> save;
            sHave = ios::readHostFile(dir + "/" + kSaveName, save);
            sInfo = inspectSave(save);
            sTime = t;
            sWhen = fileTimeText(host);
        }
        if (sHave) {
            ImGui::TextWrapped("This save (%s version): town %s, residents %s; saved %s%s.", regionName(runningRegion()),
                               sInfo.town.c_str(), residentsText(sInfo).c_str(), sWhen.c_str(),
                               sInfo.valid() ? "" : " (not valid!)");
        } else {
            ImGui::TextDisabled("No save yet (%s version).", regionName(runningRegion()));
        }
    }
    {
        std::lock_guard lk(sM);
        if (!sStartResult.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, sStartFailed ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
            ImGui::TextWrapped("At this start: %s", sStartResult.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (!ui.pendingText.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", ui.pendingText.c_str());
        ImGui::SameLine();
        if (menu::SmallButton("Cancel##pending")) {
            std::error_code ec;
            fs::remove_all(pendingDir(), ec);
            refreshPending();
            setStatus("The staged import / restore was cancelled.");
        }
    }
    if (menu::Button(ui.importOpen ? "Close import###saveimport" : "Import a Wii / Dolphin save...###saveimport"))
        ui.importOpen = !ui.importOpen;
    ImGui::SetItemTooltip("Replaces this save with one from Dolphin, a Wii NAND backup or a Wii's SD card, at the next\n"
                          "start (before the game reads it). The current save is backed up first.");
    ImGui::SameLine();
    if (menu::Button(ui.exportOpen ? "Close export###saveexport" : "Export to Dolphin...###saveexport")) ui.exportOpen = !ui.exportOpen;
    if (ui.importOpen) importSection();
    if (ui.exportOpen) exportSection();
    if (!ui.status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui.statusErr ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", ui.status.c_str());
        ImGui::PopStyleColor();
    }
    backupsSection();
}

void drawOverlay() {
    // Test aid CF_SAVE_EXPORT=<Dolphin user or NAND folder>[,<guest frame>] (default frame 600).
    static const char* env = std::getenv("CF_SAVE_EXPORT");
    static bool done = false;
    if (!env || done) return;
    std::string spec = env;
    u32 frame = 600;
    if (const size_t comma = spec.rfind(','); comma != std::string::npos) {
        frame = u32(std::strtoul(spec.c_str() + comma + 1, nullptr, 10));
        spec.resize(comma);
    }
    if (game::guestFrames() < frame) return;
    done = true;
    std::string msg;
    const bool ok = exportTo(fromU8(spec), msg);
    log("[save] CF_SAVE_EXPORT %s: %s", ok ? "ok" : "FAILED", msg.c_str());
}

}  // namespace recomp::cf::saveimport
