#include "setup/disc_check.h"

#include "port/disc_folder.h"
#include "setup/disc.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>

namespace setup {
double crcSecondsOnThisThread();  // disc.cpp

namespace {

struct RetailFile {
    const char* path;
    std::uint32_t size;
    std::uint32_t crc;
};

constexpr RetailFile kFilesRUUE01[] = {
#include "setup/disc_files_RUUE01.inc"
};
constexpr RetailFile kFilesRUUP01[] = {
#include "setup/disc_files_RUUP01.inc"
};
constexpr RetailFile kFilesRUUJ01[] = {
#include "setup/disc_files_RUUJ01.inc"
};
constexpr RetailFile kFilesRUUK01[] = {
#include "setup/disc_files_RUUK01.inc"
};

// Bump when the tables or the cache format change.
constexpr const char* kCacheFormat = "disc-check 1";

std::span<const RetailFile> retailFiles(port::GameVersion v) {
    switch (v) {
    case port::GameVersion::PAL: return kFilesRUUP01;
    case port::GameVersion::JPN: return kFilesRUUJ01;
    case port::GameVersion::KOR: return kFilesRUUK01;
    default: return kFilesRUUE01;
    }
}

std::filesystem::path utf8Path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

std::filesystem::path cacheFile(port::GameVersion v) {
    return port::dataDir() / (std::string("disc_check_") + port::gameId(v) + ".txt");
}

long long fileTime(const std::filesystem::path& p, std::error_code& ec) {
    // libc++ on macOS counts file times in __int128, which has no std::to_string.
    return static_cast<long long>(std::filesystem::last_write_time(p, ec).time_since_epoch().count());
}

// Identifies the disc: a different image file, or the same one rewritten, is checked again. An
// extracted folder: the sizes and modification times of all its retail files (a file replaced or
// edited in place changes the key; about 0.1 s for the 5000-13000 files).
std::string discKey(const std::string& path, port::GameVersion v) {
    std::error_code ec;
    if (const std::string root = port::discfolder::resolve(path); !root.empty()) {
        const auto files = utf8Path(root) / "files";
        std::uint64_t h = 1469598103934665603ull;  // FNV-1a over size and time of every file
        auto mix = [&](std::uint64_t x) {
            for (int i = 0; i < 8; i++) {
                h ^= (x >> (8 * i)) & 0xFF;
                h *= 1099511628211ull;
            }
        };
        for (const auto& f : retailFiles(v)) {
            const auto p = port::longPath(files / utf8Path(f.path + 1));
            std::error_code e2;
            const auto size = std::filesystem::file_size(p, e2);
            mix(e2 ? ~0ull : std::uint64_t(size));
            mix(e2 ? 0ull : std::uint64_t(fileTime(p, e2)));
        }
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        return std::string(kCacheFormat) + "|" + path + "|folder|" + hex;
    }
    const auto p = port::longPath(utf8Path(path));
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) return {};
    const long long time = fileTime(p, ec);
    if (ec) return {};
    return std::string(kCacheFormat) + "|" + path + "|" + std::to_string(size) + "|" + std::to_string(time);
}

}  // namespace

std::string DiscCheck::summary(size_t maxNames) const {
    if (bad.empty()) return "all " + std::to_string(files) + " game files match the retail disc";
    std::string s = std::to_string(bad.size()) + " of " + std::to_string(files) + " game files differ from the retail disc: ";
    for (size_t i = 0; i < bad.size() && i < maxNames; i++) s += (i ? ", " : "") + bad[i];
    if (bad.size() > maxNames) s += ", ...";
    return s;
}

bool discCheckEnabled() {
    const char* e = std::getenv("CF_DISC_CHECK");
    return !(e && std::strcmp(e, "0") == 0);
}
bool discCheckForced() {
    const char* e = std::getenv("CF_DISC_CHECK");
    return e && std::strcmp(e, "force") == 0;
}

std::optional<DiscCheck> cachedDiscCheck(const std::string& path, port::GameVersion v) {
    if (discCheckForced()) return std::nullopt;
    std::ifstream in(port::longPath(cacheFile(v)), std::ios::binary);
    if (!in) return std::nullopt;
    std::string line;
    auto next = [&] {  // one line, CR LF tolerated (a file edited by hand)
        if (!std::getline(in, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    if (!next()) return std::nullopt;
    const std::string key = discKey(path, v);
    if (key.empty() || line != key) return std::nullopt;
    DiscCheck c;
    if (!next() || line.rfind("files ", 0) != 0) return std::nullopt;
    c.files = std::atoi(line.c_str() + 6);
    while (next()) {
        if (line == "accepted") c.accepted = true;
        else if (line.rfind("bad ", 0) == 0) c.bad.push_back(line.substr(4));
    }
    return c;
}

void saveDiscCheck(const std::string& path, port::GameVersion v, const DiscCheck& c) {
    const std::string key = discKey(path, v);
    if (key.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(port::longPath(port::dataDir()), ec);
    std::ofstream out(port::longPath(cacheFile(v)), std::ios::binary | std::ios::trunc);
    out << key << "\nfiles " << c.files << "\n";
    for (const auto& b : c.bad) out << "bad " << b << "\n";
    if (c.accepted) out << "accepted\n";
}

std::optional<DiscCheck> checkDiscFiles(const std::string& path, port::GameVersion v, const std::atomic<bool>* cancel,
                                        const std::function<void(float)>& progress) {
    Disc disc;
    std::string err;
    DiscCheck c;
    if (!disc.open(path, err)) {
        c.bad.push_back("the disc could not be read (" + err + ")");
        return c;
    }
    const auto files = retailFiles(v);
    std::uint64_t total = 0, done = 0;
    for (const auto& f : files) total += f.size;
    for (const auto& f : files) {
        if (cancel && cancel->load()) return std::nullopt;
        std::uint32_t size = 0, crc = 0;
        bool found = false;
        const bool read = disc.fileCrc(f.path, size, crc, found);
        if (!found) c.bad.push_back(std::string(f.path) + " (missing)");
        else if (size != f.size) c.bad.push_back(std::string(f.path) + " (size)");
        else if (!read) c.bad.push_back(std::string(f.path) + " (unreadable)");
        else if (crc != f.crc) c.bad.push_back(std::string(f.path) + " (contents)");
        c.files++;
        done += f.size;
        if (progress) progress(total ? float(double(done) / double(total)) : 1.0f);
    }
    std::printf("[disc] check: %.0f MB, CRC computation %.1f s\n", double(total) / 1048576.0, crcSecondsOnThisThread());
    std::fflush(stdout);
    return c;
}

}  // namespace setup
