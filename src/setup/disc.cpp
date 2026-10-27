#include "setup/disc.h"

#include "port/disc_folder.h"
#include "port/dol.h"
#include "setup/embedded.h"

#include <nod.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace setup {
namespace {

std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

bool readAll(const std::filesystem::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream in(port::longPath(p), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

const char* formatName(NodFormat f) {
    switch (f) {
    case NOD_FORMAT_ISO: return "ISO";
    case NOD_FORMAT_CISO: return "CISO";
    case NOD_FORMAT_GCZ: return "GCZ";
    case NOD_FORMAT_NFS: return "NFS";
    case NOD_FORMAT_RVZ: return "RVZ";
    case NOD_FORMAT_WBFS: return "WBFS";
    case NOD_FORMAT_WIA: return "WIA";
    case NOD_FORMAT_TGC: return "TGC";
    }
    return "disc";
}

constexpr std::uint8_t kWiiMagic[4] = {0x5D, 0x1C, 0x9E, 0xA3};
constexpr std::uint8_t kGcMagic[4] = {0xC2, 0x33, 0x9F, 0x3D};

const char* kSupported = "Supported: Animal Crossing: City Folk revision 1 of the USA (RUUE01), European (RUUP01) or "
                         "Japanese (RUUJ01) disc.";

}  // namespace

std::string hex(const std::uint8_t* data, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[data[i] >> 4];
        s += d[data[i] & 15];
    }
    return s;
}

std::string describe(port::GameVersion v) {
    return std::string("Animal Crossing: City Folk (") + port::regionName(v) + ", " + port::gameId(v) + " rev 1)";
}

Disc::~Disc() {
    nod_free(part_);
    nod_free(disc_);
}

bool Disc::open(const std::string& utf8Path, std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> boot;
    if (const std::string root = port::discfolder::resolve(utf8Path); !root.empty()) {  // extracted folder
        folder_ = u8path(root);
        runPath_ = root;
        kind_ = "extracted folder";
        if (!readAll(folder_ / "sys" / "main.dol", dol_)) {
            error = "The folder has no sys/main.dol.";
            return false;
        }
        readAll(folder_ / "sys" / "boot.bin", boot);
    } else {
        std::error_code ec;
        if (std::filesystem::is_directory(u8path(utf8Path), ec)) {
            error = "This folder is not an extracted Wii disc: it needs the game partition's sys/ and files/ folders "
                    "(Dolphin: right-click the game > Properties > Filesystem > Extract Entire Disc), or pick a disc "
                    "image file.";
            return false;
        }
        if (nod_disc_open(utf8Path.c_str(), nullptr, &disc_) != NOD_RESULT_OK) {
            const char* msg = nod_error_message();
            error = std::string("Could not read the disc image") + (msg && *msg ? std::string(": ") + msg : ".") +
                    " Supported: Wii ISO, WBFS, RVZ, WIA, CISO, GCZ, NFS, or an extracted disc folder.";
            return false;
        }
        runPath_ = utf8Path;
        NodDiscMeta dm{};
        kind_ = nod_disc_meta(disc_, &dm) == NOD_RESULT_OK ? std::string(formatName(dm.format)) + " image" : "disc image";
        NodDiscHeader dh{};
        if (nod_disc_header(disc_, &dh) == NOD_RESULT_OK) {
            if (std::memcmp(dh.gcn_magic, kGcMagic, 4) == 0) {
                error = "This is a GameCube disc (" + std::string(dh.game_id, 6) + "), not Animal Crossing: City Folk (Wii).";
                return false;
            }
            if (std::memcmp(dh.wii_magic, kWiiMagic, 4) == 0 && dh.no_partition_encryption) kind_ += ", decrypted";
        }
        if (nod_disc_open_partition_kind(disc_, NOD_PARTITION_KIND_DATA, nullptr, &part_) != NOD_RESULT_OK) {
            const char* msg = nod_error_message();
            error = std::string("The disc image has no readable game partition") + (msg && *msg ? std::string(": ") + msg : ".");
            return false;
        }
        NodPartitionMeta meta{};
        if (nod_partition_meta(part_, &meta) != NOD_RESULT_OK || !meta.raw_dol.data) {
            error = "The disc image's game partition has no main.dol.";
            return false;
        }
        dol_.assign(meta.raw_dol.data, meta.raw_dol.data + meta.raw_dol.size);
        if (meta.raw_boot.data) boot.assign(meta.raw_boot.data, meta.raw_boot.data + meta.raw_boot.size);
    }

    // 1. Game ID and revision (the game partition's header), 2. main.dol, 3. every REL.
    if (boot.size() < 0x20) {
        error = "The disc has no readable header (sys/boot.bin).";
        return false;
    }
    const std::string id(reinterpret_cast<const char*>(boot.data()), 6);
    const unsigned rev = boot[7];
    if (std::memcmp(&boot[0x18], kWiiMagic, 4) != 0) {
        error = std::memcmp(&boot[0x1C], kGcMagic, 4) == 0
                    ? "This is a GameCube disc (" + id + "), not Animal Crossing: City Folk (Wii)."
                    : "This is not a Wii disc.";
        return false;
    }
    if (!port::versionFromId(id.c_str(), version_)) {
        if (id.rfind("RUU", 0) == 0)
            error = "This Animal Crossing: City Folk disc (" + id + ") is a release this port does not support. " + kSupported;
        else
            error = "This is not Animal Crossing: City Folk (game ID " + id + "). " + kSupported;
        return false;
    }
    if (rev != 1) {
        error = "This is revision " + std::to_string(rev) + " of " + id + "; only revision 1 is supported (Dolphin shows "
                "the revision in the game's Properties > Info). " + kSupported;
        return false;
    }
    const auto dv = port::dol::identify(dol_.data(), dol_.size());
    if (std::strcmp(port::dol::versionId(dv), id.c_str()) != 0) {
        error = "main.dol does not match the retail " + id + " rev 1 (SHA-1 " + dolSha1() +
                "): the disc is modified or a bad dump. Dump the disc again or use an unmodified copy.";
        return false;
    }
    const auto list = embedded::relList(version_);
    rels_.clear();
    rels_.reserve(list.size());
    std::string bad;
    int badCount = 0;
    for (const auto& e : list) {
        std::vector<std::uint8_t> data;
        const std::string name(e.name);
        if (!readFile(("/rels/" + name + ".rel").c_str(), data)) {
            error = "The disc has no rels/" + name + ".rel (an incomplete extraction or a bad dump?).";
            return false;
        }
        const auto d = port::dol::sha1(data.data(), data.size());
        if (hex(d.data(), d.size()) != e.sha1) {
            if (badCount++ < 3) bad += (bad.empty() ? "" : ", ") + name;
        }
        rels_.emplace_back(name, std::move(data));
    }
    if (badCount) {
        error = std::to_string(badCount) + " REL module" + (badCount > 1 ? "s differ" : " differs") + " from the retail " +
                id + " rev 1 (" + bad + (badCount > 3 ? ", ..." : "") +
                "): the disc is modified or a bad dump. Dump the disc again or use an unmodified copy.";
        return false;
    }
    std::printf("[setup] %s: %s, %s (main.dol + %zu RELs verified in %.0f ms)\n", utf8Path.c_str(),
                describe(version_).c_str(), kind_.c_str(), rels_.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    std::fflush(stdout);
    return true;
}

std::string Disc::dolSha1() const {
    const auto d = port::dol::sha1(dol_.data(), dol_.size());
    return hex(d.data(), d.size());
}

bool Disc::readFile(const char* path, std::vector<std::uint8_t>& out) const {
    if (!folder_.empty()) {
        std::string rel = path;
        while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        return readAll(folder_ / "files" / u8path(rel), out);
    }
    NodNodeKind kind{};
    uint32_t length = 0;
    const uint32_t index = nod_partition_find_file(part_, path, &kind, &length);
    if (index == NOD_FST_STOP || kind != NOD_NODE_KIND_FILE) return false;
    NodHandle* file = nullptr;
    if (nod_partition_open_file(part_, index, &file) != NOD_RESULT_OK) return false;
    out.resize(length);
    size_t done = 0;
    while (done < length) {
        const int64_t n = nod_read(file, out.data() + done, length - done);
        if (n <= 0) break;
        done += size_t(n);
    }
    nod_free(file);
    return done == length;
}

bool probeDisc(const std::string& utf8Path, port::GameVersion& version, std::string& error) {
    Disc d;
    if (!d.open(utf8Path, error)) return false;
    version = d.version();
    return true;
}

}  // namespace setup
