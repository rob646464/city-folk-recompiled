#include "setup/disc.h"

#include "port/disc_folder.h"
#include "port/dol.h"

#include <nod.h>

#include <fstream>
#include <iterator>

namespace setup {
namespace {

std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

bool readAll(const std::filesystem::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

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

Disc::~Disc() {
    nod_free(part_);
    nod_free(disc_);
}

bool Disc::open(const std::string& utf8Path, std::string& error) {
    std::string id;
    if (port::discfolder::isDiscFolder(utf8Path)) {  // extracted disc folder
        folder_ = u8path(utf8Path);
        if (!readAll(folder_ / "sys" / "main.dol", dol_)) {
            error = "The folder has no sys/main.dol.";
            return false;
        }
        std::vector<std::uint8_t> boot;
        if (readAll(folder_ / "sys" / "boot.bin", boot) && boot.size() >= 6) id.assign(reinterpret_cast<const char*>(boot.data()), 6);
    } else {
        if (nod_disc_open(utf8Path.c_str(), nullptr, &disc_) != NOD_RESULT_OK) {
            const char* msg = nod_error_message();
            error = std::string("Could not read the disc image") + (msg ? std::string(": ") + msg : ".");
            return false;
        }
        if (nod_disc_open_partition_kind(disc_, NOD_PARTITION_KIND_DATA, nullptr, &part_) != NOD_RESULT_OK) {
            error = "The disc image has no game partition.";
            return false;
        }
        NodPartitionMeta meta{};
        if (nod_partition_meta(part_, &meta) != NOD_RESULT_OK || !meta.raw_dol.data) {
            error = "The disc image has no main.dol.";
            return false;
        }
        dol_.assign(meta.raw_dol.data, meta.raw_dol.data + meta.raw_dol.size);
        if (meta.raw_boot.data && meta.raw_boot.size >= 6) id.assign(reinterpret_cast<const char*>(meta.raw_boot.data), 6);
    }
    switch (port::dol::identify(dol_.data(), dol_.size())) {
    case port::dol::Version::RUUE01: version_ = port::GameVersion::USA; return true;
    case port::dol::Version::RUUP01: version_ = port::GameVersion::PAL; return true;
    default: break;
    }
    if (id.rfind("RUU", 0) == 0)
        error = "This Animal Crossing: City Folk disc (" + id + ") is not a supported release yet. Supported: USA "
                "(RUUE01), revision 1.";
    else
        error = "This is not an Animal Crossing: City Folk disc" + (id.empty() ? std::string(".") : " (game ID " + id + ").");
    return false;
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
    if (index == NOD_FST_STOP) return false;
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
