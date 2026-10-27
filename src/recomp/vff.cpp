// VFF volumes (vff.h). Written from the public descriptions of the VFF container and of FAT;
// checked against volumes the game's own VF library creates and reads (docs/DISTRIBUTIONS.md).
#include "recomp/vff.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

namespace recomp::vff {
namespace {

constexpr std::uint32_t kHeader = 0x20, kCluster = 0x200, kRootSize = 0x1000;

struct Geo {
    std::uint32_t size = 0, clusters = 0, fatBits = 0, fatSize = 0;
    std::uint32_t fat1 = 0, fat2 = 0, root = 0, data = 0, dataClusters = 0;
};

std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void putBe32(std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v >> 24), p[1] = std::uint8_t(v >> 16), p[2] = std::uint8_t(v >> 8), p[3] = std::uint8_t(v); }
std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }
std::uint32_t le32(const std::uint8_t* p) { return std::uint32_t(p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24); }
void putLe16(std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v), p[1] = std::uint8_t(v >> 8); }
void putLe32(std::uint8_t* p, std::uint32_t v) { putLe16(p, v), putLe16(p + 2, v >> 16); }

Geo geometry(std::uint32_t size) {
    Geo g;
    g.size = size;
    g.clusters = size / kCluster;
    g.fatBits = g.clusters < 0xFF5 ? 12 : g.clusters < 0xFFF5 ? 16 : 32;
    g.fatSize = (g.clusters * g.fatBits / 8 + kCluster - 1) / kCluster * kCluster;
    g.fat1 = kHeader;
    g.fat2 = g.fat1 + g.fatSize;
    g.root = g.fat2 + g.fatSize;
    g.data = g.root + kRootSize;
    g.dataClusters = g.data < size ? (size - g.data) / kCluster : 0;
    return g;
}

bool parse(const Bytes& vol, Geo& g, std::string* error) {
    auto fail = [&](const char* e) {
        if (error) *error = e;
        return false;
    };
    if (vol.size() < kHeader || std::memcmp(vol.data(), "VFF ", 4) != 0) return fail("not a VFF volume");
    const std::uint32_t size = be32(&vol[8]);
    if (size > vol.size() || size % kCluster) return fail("VFF size field does not match the file");
    g = geometry(size);
    if (g.fatBits == 32 || g.dataClusters < 2) return fail("unsupported VFF geometry");
    return true;
}

std::uint32_t fatGet(const Bytes& vol, const Geo& g, std::uint32_t n) {
    const std::uint8_t* f = &vol[g.fat1];
    if (g.fatBits == 16) return le16(f + 2 * n);
    const std::uint32_t o = n * 3 / 2;
    const std::uint32_t v = le16(f + o);
    return (n & 1) ? v >> 4 : v & 0xFFF;
}
void fatSet(Bytes& vol, const Geo& g, std::uint32_t n, std::uint32_t v) {
    for (std::uint32_t base : {g.fat1, g.fat2}) {
        std::uint8_t* f = &vol[base];
        if (g.fatBits == 16) {
            putLe16(f + 2 * n, v);
        } else {
            const std::uint32_t o = n * 3 / 2;
            std::uint32_t w = le16(f + o);
            w = (n & 1) ? (w & 0x000F) | (v & 0xFFF) << 4 : (w & 0xF000) | (v & 0xFFF);
            putLe16(f + o, w);
        }
    }
}
std::uint32_t endMark(const Geo& g) { return g.fatBits == 16 ? 0xFFFF : 0xFFF; }
bool isEnd(const Geo& g, std::uint32_t v) { return v >= (g.fatBits == 16 ? 0xFFF8u : 0xFF8u); }
std::uint32_t clusterOff(const Geo& g, std::uint32_t c) { return g.data + (c - 2) * kCluster; }

// "item.bin" -> "ITEM    BIN"; false when not a plain 8.3 name.
bool shortName(const std::string& name, char out[11]) {
    std::memset(out, ' ', 11);
    const auto dot = name.find('.');
    const std::string base = name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || ext.find('.') != std::string::npos) return false;
    auto ok = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("_-~!#$%&'()@^`{}", c); };
    for (size_t i = 0; i < base.size(); i++) {
        if (!ok(base[i])) return false;
        out[i] = char(std::toupper(static_cast<unsigned char>(base[i])));
    }
    for (size_t i = 0; i < ext.size(); i++) {
        if (!ok(ext[i])) return false;
        out[8 + i] = char(std::toupper(static_cast<unsigned char>(ext[i])));
    }
    return true;
}
std::string displayName(const std::uint8_t* e) {
    std::string b(reinterpret_cast<const char*>(e), 8), x(reinterpret_cast<const char*>(e + 8), 3);
    while (!b.empty() && b.back() == ' ') b.pop_back();
    while (!x.empty() && x.back() == ' ') x.pop_back();
    return x.empty() ? b : b + "." + x;
}

// Root directory entry with this name (byte offset in vol), or 0.
std::uint32_t findEntry(const Bytes& vol, const Geo& g, const char sn[11]) {
    for (std::uint32_t o = g.root; o < g.root + kRootSize; o += 32) {
        const std::uint8_t first = vol[o];
        if (first == 0x00) break;
        if (first == 0xE5 || (vol[o + 11] & 0x0F) == 0x0F || (vol[o + 11] & 0x18)) continue;  // free / LFN / dir / label
        if (std::memcmp(&vol[o], sn, 11) == 0) return o;
    }
    return 0;
}

void freeChain(Bytes& vol, const Geo& g, std::uint32_t c) {
    for (std::uint32_t guard = 0; c >= 2 && c < g.dataClusters + 2 && guard < g.dataClusters; guard++) {
        const std::uint32_t next = fatGet(vol, g, c);
        fatSet(vol, g, c, 0);
        if (isEnd(g, next) || next < 2) break;
        c = next;
    }
}

void dosTime(std::uint16_t& date, std::uint16_t& time) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    date = std::uint16_t(((std::max(tm.tm_year, 80) - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    time = std::uint16_t((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

}  // namespace

Bytes format(std::uint32_t size) {
    size = size / kCluster * kCluster;
    const Geo g = geometry(size);
    Bytes v(size, 0);
    std::memcpy(v.data(), "VFF ", 4);
    v[4] = 0xFE, v[5] = 0xFF;  // byte order mark
    v[6] = 0x01, v[7] = 0x00;
    putBe32(&v[8], size);
    v[12] = 0x00, v[13] = 0x20;  // header size
    // Reserved FAT entries 0 and 1: media byte F8 + end marks.
    for (std::uint32_t base : {g.fat1, g.fat2}) {
        v[base] = 0xF8, v[base + 1] = 0xFF, v[base + 2] = 0xFF;
        if (g.fatBits == 16) v[base + 3] = 0xFF;
    }
    return v;
}

std::optional<std::vector<Entry>> list(const Bytes& vol, std::string* error) {
    Geo g;
    if (!parse(vol, g, error)) return std::nullopt;
    std::vector<Entry> out;
    for (std::uint32_t o = g.root; o < g.root + kRootSize; o += 32) {
        if (vol[o] == 0x00) break;
        if (vol[o] == 0xE5 || (vol[o + 11] & 0x0F) == 0x0F || (vol[o + 11] & 0x18)) continue;
        out.push_back({displayName(&vol[o]), le32(&vol[o + 28])});
    }
    return out;
}

std::optional<Bytes> read(const Bytes& vol, const std::string& name, std::string* error) {
    Geo g;
    if (!parse(vol, g, error)) return std::nullopt;
    char sn[11];
    if (!shortName(name, sn)) {
        if (error) *error = "not an 8.3 name";
        return std::nullopt;
    }
    const std::uint32_t e = findEntry(vol, g, sn);
    if (!e) {
        if (error) *error = "no such file";
        return std::nullopt;
    }
    const std::uint32_t size = le32(&vol[e + 28]);
    std::uint32_t c = le16(&vol[e + 26]);
    Bytes out;
    out.reserve(size);
    while (out.size() < size) {
        if (c < 2 || c >= g.dataClusters + 2) {
            if (error) *error = "broken cluster chain";
            return std::nullopt;
        }
        const std::uint32_t take = std::min<std::uint32_t>(kCluster, size - std::uint32_t(out.size()));
        const std::uint32_t off = clusterOff(g, c);
        out.insert(out.end(), vol.begin() + off, vol.begin() + off + take);
        c = fatGet(vol, g, c);
    }
    return out;
}

bool remove(Bytes& vol, const std::string& name) {
    Geo g;
    char sn[11];
    if (!parse(vol, g, nullptr) || !shortName(name, sn)) return false;
    const std::uint32_t e = findEntry(vol, g, sn);
    if (!e) return false;
    freeChain(vol, g, le16(&vol[e + 26]));
    vol[e] = 0xE5;
    return true;
}

bool write(Bytes& vol, const std::string& name, const Bytes& data, std::string* error) {
    Geo g;
    if (!parse(vol, g, error)) return false;
    char sn[11];
    if (!shortName(name, sn)) {
        if (error) *error = "not an 8.3 name: " + name;
        return false;
    }
    std::uint32_t e = findEntry(vol, g, sn);
    if (e) freeChain(vol, g, le16(&vol[e + 26]));
    // Free clusters for the data.
    const std::uint32_t need = std::uint32_t((data.size() + kCluster - 1) / kCluster);
    std::vector<std::uint32_t> got;
    for (std::uint32_t c = 2; c < g.dataClusters + 2 && got.size() < need; c++)
        if (fatGet(vol, g, c) == 0) got.push_back(c);
    if (got.size() < need) {
        if (error) *error = "the volume is full";
        if (e) vol[e] = 0xE5;
        return false;
    }
    if (!e) {
        for (std::uint32_t o = g.root; o < g.root + kRootSize; o += 32)
            if (vol[o] == 0x00 || vol[o] == 0xE5) {
                e = o;
                break;
            }
        if (!e) {
            if (error) *error = "the root directory is full";
            return false;
        }
    }
    for (size_t i = 0; i < got.size(); i++) {
        fatSet(vol, g, got[i], i + 1 < got.size() ? got[i + 1] : endMark(g));
        const std::uint32_t off = clusterOff(g, got[i]);
        const size_t take = std::min<size_t>(kCluster, data.size() - i * kCluster);
        std::memset(&vol[off], 0, kCluster);
        std::memcpy(&vol[off], data.data() + i * kCluster, take);
    }
    std::memset(&vol[e], 0, 32);
    std::memcpy(&vol[e], sn, 11);
    vol[e + 11] = 0x20;  // archive
    std::uint16_t d, t;
    dosTime(d, t);
    putLe16(&vol[e + 14], t), putLe16(&vol[e + 16], d), putLe16(&vol[e + 18], d);
    putLe16(&vol[e + 22], t), putLe16(&vol[e + 24], d);
    putLe16(&vol[e + 26], got.empty() ? 0 : got[0]);
    putLe32(&vol[e + 28], std::uint32_t(data.size()));
    return true;
}

bool selfTest(std::string* failure) {
    auto fail = [&](const char* w) {
        if (failure) *failure = w;
        return false;
    };
    for (std::uint32_t size : {0x5A000u, 0x200000u}) {  // FAT12 (wc24dl.vff) and FAT16 (wc24send.mbx)
        Bytes v = format(size);
        Bytes a(5000), b(700);
        for (size_t i = 0; i < a.size(); i++) a[i] = std::uint8_t(i * 7);
        for (size_t i = 0; i < b.size(); i++) b[i] = std::uint8_t(i ^ 0x5A);
        if (!write(v, "rvforest.bin", a) && !write(v, "a.bin", a)) return fail("write");
        if (!write(v, "b.bin", b) || !write(v, "a.bin", b) || !write(v, "a.bin", a)) return fail("rewrite");
        auto ra = read(v, "A.BIN"), rb = read(v, "b.bin");
        if (!ra || *ra != a || !rb || *rb != b) return fail("read back");
        auto l = list(v);
        if (!l || l->size() < 2) return fail("list");
        if (!remove(v, "b.bin") || read(v, "b.bin")) return fail("remove");
    }
    return true;
}

}  // namespace recomp::vff
