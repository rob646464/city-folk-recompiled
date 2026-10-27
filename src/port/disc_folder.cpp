#include "port/disc_folder.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace port::discfolder {
namespace {

namespace fs = std::filesystem;

std::string sRoot;
std::vector<Entry> sEntries;
std::vector<uint8_t> sDol, sBoot, sBi2;
bool sActive = false;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool readAll(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// Open host files, by entry (the game reads the same file in many small pieces).
std::mutex sFilesM;
std::unordered_map<int32_t, std::ifstream> sFiles;

}  // namespace

bool isDiscFolder(const std::string& utf8Path) {
    std::error_code ec;
    const fs::path p = u8path(utf8Path);
    return fs::is_directory(p, ec) && fs::is_regular_file(p / "sys" / "fst.bin", ec) && fs::is_directory(p / "files", ec);
}

std::string resolve(const std::string& utf8Path) {
    if (utf8Path.empty()) return {};
    std::error_code ec;
    fs::path p = u8path(utf8Path);
    auto u8 = [](const fs::path& x) {
        const auto s = x.u8string();
        return std::string(s.begin(), s.end());
    };
    auto isPart = [&](const fs::path& d) { return isDiscFolder(u8(d)); };
    if (fs::is_regular_file(p, ec)) {  // a file in <partition>/sys
        const fs::path sys = p.parent_path();
        if (ieq(u8(sys.filename()), "sys") && isPart(sys.parent_path())) return u8(sys.parent_path());
        return {};
    }
    if (!fs::is_directory(p, ec)) return {};
    if (isPart(p)) return u8(p);
    if (ieq(u8(p.filename()), "sys") && isPart(p.parent_path())) return u8(p.parent_path());
    // A whole-disc extraction: DATA/ (Dolphin), else the one partition folder that is not UPDATE.
    if (isPart(p / "DATA")) return u8(p / "DATA");
    std::string found;
    int n = 0;
    for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = u8(it->path().filename());
        if (ieq(name, "UPDATE") || ieq(name, "CHANNEL") || !isPart(it->path())) continue;
        found = u8(it->path());
        n++;
    }
    return n == 1 ? found : std::string();
}

bool open(const std::string& utf8Path, std::string& error) {
    const fs::path p = u8path(utf8Path);
    std::vector<uint8_t> fst;
    if (!readAll(p / "sys" / "fst.bin", fst) || fst.size() < 12) {
        error = "no sys/fst.bin in " + utf8Path;
        return false;
    }
    const uint32_t count = be32(&fst[8]);
    if (count == 0 || size_t(count) * 12 > fst.size()) {
        error = "sys/fst.bin is damaged";
        return false;
    }
    const size_t strings = size_t(count) * 12;
    std::vector<Entry> entries(count);
    entries[0].isDir = true;
    entries[0].next = count;
    entries[0].path = "";
    // Directories are nested by index ranges: walk with a stack of open directories.
    std::vector<uint32_t> stack{0};
    for (uint32_t i = 1; i < count; i++) {
        while (stack.size() > 1 && i >= entries[stack.back()].next) stack.pop_back();
        const uint8_t* e = &fst[size_t(i) * 12];
        Entry& en = entries[i];
        en.isDir = e[0] != 0;
        const uint32_t nameOff = (be32(e) & 0x00FFFFFF);
        if (strings + nameOff < fst.size()) {
            const char* s = reinterpret_cast<const char*>(&fst[strings + nameOff]);
            en.name.assign(s, strnlen(s, fst.size() - strings - nameOff));
        }
        const uint32_t parent = stack.back();
        en.path = entries[parent].path + "/" + en.name;
        if (en.isDir) {
            en.parent = be32(e + 4);
            en.next = be32(e + 8);
            stack.push_back(i);
        } else {
            en.parent = parent;
            en.length = be32(e + 8);
            en.offset = be32(e + 4);
        }
    }
    std::lock_guard lk(sFilesM);
    sFiles.clear();
    sEntries = std::move(entries);
    sRoot = utf8Path;
    sDol.clear();
    readAll(p / "sys" / "main.dol", sDol);
    readAll(p / "sys" / "boot.bin", sBoot);
    readAll(p / "sys" / "bi2.bin", sBi2);
    sActive = true;
    return true;
}

bool active() { return sActive; }
const std::string& root() { return sRoot; }
uint32_t entryCount() { return uint32_t(sEntries.size()); }
const Entry* entry(int32_t n) { return n >= 0 && size_t(n) < sEntries.size() ? &sEntries[size_t(n)] : nullptr; }
const std::vector<uint8_t>& dol() { return sDol; }
const std::vector<uint8_t>& bootBin() { return sBoot; }
const std::vector<uint8_t>& bi2Bin() { return sBi2; }

int32_t pathToEntry(const char* path, uint32_t cwd) {
    if (!path || sEntries.empty()) return -1;
    uint32_t dir = *path == '/' ? 0 : (cwd < sEntries.size() && sEntries[cwd].isDir ? cwd : 0);
    std::string s = path;
    size_t pos = 0;
    while (pos <= s.size()) {
        while (pos < s.size() && s[pos] == '/') pos++;
        if (pos >= s.size()) return int32_t(dir);
        size_t end = s.find('/', pos);
        if (end == std::string::npos) end = s.size();
        const std::string part = s.substr(pos, end - pos);
        pos = end;
        if (part == ".") continue;
        if (part == "..") {
            dir = sEntries[dir].parent;
            continue;
        }
        if (!sEntries[dir].isDir) return -1;
        int32_t found = -1;
        for (uint32_t i = dir + 1; i < sEntries[dir].next;) {
            if (ieq(sEntries[i].name, part)) {
                found = int32_t(i);
                break;
            }
            i = sEntries[i].isDir ? sEntries[i].next : i + 1;
        }
        if (found < 0) return -1;
        dir = uint32_t(found);
        if (!sEntries[dir].isDir && pos < s.size()) return -1;
    }
    return int32_t(dir);
}

int64_t read(int32_t n, void* dst, uint32_t len, uint32_t off) {
    const Entry* e = entry(n);
    if (!e || e->isDir) return -1;
    if (off >= e->length) return 0;
    if (len > e->length - off) len = e->length - off;
    std::lock_guard lk(sFilesM);
    auto it = sFiles.find(n);
    if (it == sFiles.end()) {
        std::ifstream f(u8path(sRoot) / "files" / u8path(e->path.substr(1)), std::ios::binary);
        if (!f) return -1;
        it = sFiles.emplace(n, std::move(f)).first;
        if (sFiles.size() > 64) {  // keep the number of open handles small
            for (auto j = sFiles.begin(); j != sFiles.end();)
                j = j->first == n ? std::next(j) : sFiles.erase(j);
        }
    }
    std::ifstream& f = it->second;
    f.clear();
    f.seekg(off);
    f.read(static_cast<char*>(dst), len);
    return f.gcount();
}

}  // namespace port::discfolder
