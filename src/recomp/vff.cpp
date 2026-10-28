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

// --- names: 8.3 short names + VFAT long names (the game's VF library keeps long names, e.g. the
// download "game_data.bin" and its folder "mb") ---

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool sfnChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("_-~!#$%&'()@^`{}", c); }

// "ITEM.BIN" -> "ITEM    BIN" when the name is an upper-case 8.3 name as it is (no long name needed).
bool exactShortName(const std::string& name, char out[11]) {
    std::memset(out, ' ', 11);
    const auto dot = name.find('.');
    const std::string base = name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || ext.find('.') != std::string::npos) return false;
    for (char c : base + ext)
        if (!sfnChar(c) || std::islower(static_cast<unsigned char>(c))) return false;
    std::memcpy(out, base.data(), base.size());
    std::memcpy(out + 8, ext.data(), ext.size());
    return true;
}
std::string displayName(const std::uint8_t* e) {
    std::string b(reinterpret_cast<const char*>(e), 8), x(reinterpret_cast<const char*>(e + 8), 3);
    while (!b.empty() && b.back() == ' ') b.pop_back();
    while (!x.empty() && x.back() == ' ') x.pop_back();
    return x.empty() ? b : b + "." + x;
}
std::uint8_t lfnChecksum(const std::uint8_t* sn) {
    std::uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = std::uint8_t(((s & 1) << 7) + (s >> 1) + sn[i]);
    return s;
}
bool validLong(const std::string& name) {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F || std::strchr("\\/:*?\"<>|", c))
            return false;
    return true;
}

// One root directory file: its 8.3 entry, the first of its directory slots (long-name entries
// first), and its name (the long one when present).
struct Found {
    std::uint32_t sfn = 0, first = 0;
    std::string name;
};
std::vector<Found> rootFiles(const Bytes& vol, const Geo& g, bool withDirs = false) {
    std::vector<Found> out;
    std::string lfn;
    std::uint32_t lfnStart = 0;
    int expect = 0;
    std::uint8_t sum = 0;
    for (std::uint32_t o = g.root; o < g.root + kRootSize; o += 32) {
        const std::uint8_t first = vol[o], attr = vol[o + 11];
        if (first == 0x00) break;
        if (first == 0xE5) {
            expect = 0;
            continue;
        }
        if ((attr & 0x3F) == 0x0F) {  // long-name piece: ordinal (0x40 = last piece), 13 UTF-16 characters
            const int ord = first & 0x1F;
            if (first & 0x40) lfn.assign(size_t(ord) * 13, '\0'), lfnStart = o, expect = ord, sum = vol[o + 13];
            if (!expect || ord != expect || vol[o + 13] != sum) {
                expect = 0;
                continue;
            }
            static const int kPos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int i = 0; i < 13; i++) {
                const std::uint16_t ch = le16(&vol[o + std::uint32_t(kPos[i])]);
                lfn[size_t(ord - 1) * 13 + size_t(i)] = ch && ch != 0xFFFF ? char(ch < 0x80 ? ch : '?') : '\0';
            }
            expect = ord - 1;
            continue;
        }
        if (attr & 0x08) continue;  // volume label
        Found f;
        f.sfn = o;
        f.first = o;
        f.name = displayName(&vol[o]);
        if (expect == 0 && !lfn.empty() && lfnChecksum(&vol[o]) == sum) {
            f.first = lfnStart;
            f.name = lfn.substr(0, lfn.find('\0'));
        }
        lfn.clear();
        expect = 0;
        if ((attr & 0x10) && !withDirs) continue;
        out.push_back(f);
    }
    return out;
}
bool sameName(const std::string& a, const std::string& b) { return upper(a) == upper(b); }
const Found* findFile(const std::vector<Found>& fs, const std::string& name) {
    for (const auto& f : fs)
        if (sameName(f.name, name)) return &f;
    return nullptr;
}

void freeChain(Bytes& vol, const Geo& g, std::uint32_t c) {
    for (std::uint32_t guard = 0; c >= 2 && c < g.dataClusters + 2 && guard < g.dataClusters; guard++) {
        const std::uint32_t next = fatGet(vol, g, c);
        fatSet(vol, g, c, 0);
        if (isEnd(g, next) || next < 2) break;
        c = next;
    }
}
void freeSlots(Bytes& vol, const Found& f) {
    for (std::uint32_t o = f.first; o <= f.sfn; o += 32) vol[o] = 0xE5;
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

// A unique 8.3 alias for a long name: "game_data.bin" -> "GAME_D~1BIN".
void makeAlias(const Bytes& vol, const Geo& g, const std::string& name, char out[11]) {
    const auto dot = name.rfind('.');
    std::string base, ext;
    for (char c : upper(dot == std::string::npos ? name : name.substr(0, dot)))
        if (sfnChar(c)) base += c;
    if (dot != std::string::npos)
        for (char c : upper(name.substr(dot + 1)))
            if (sfnChar(c) && ext.size() < 3) ext += c;
    if (base.empty()) base = "FILE";
    for (int n = 1; n < 1000000; n++) {
        const std::string tail = "~" + std::to_string(n);
        const std::string b = base.substr(0, 8 - std::min<size_t>(8, tail.size())) + tail;
        std::memset(out, ' ', 11);
        std::memcpy(out, b.data(), std::min<size_t>(8, b.size()));
        std::memcpy(out + 8, ext.data(), ext.size());
        bool clash = false;
        for (std::uint32_t o = g.root; o < g.root + kRootSize; o += 32) {
            if (vol[o] == 0) break;
            if (vol[o] != 0xE5 && (vol[o + 11] & 0x3F) != 0x0F && std::memcmp(&vol[o], out, 11) == 0) clash = true;
        }
        if (!clash) return;
    }
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
    // Reserved FAT entries 0 and 1: media byte F0 + end mark, as the game's VF library formats them.
    for (std::uint32_t base : {g.fat1, g.fat2}) {
        v[base] = 0xF0, v[base + 1] = 0xFF, v[base + 2] = 0xFF;
        if (g.fatBits == 16) v[base + 3] = 0xFF;
    }
    return v;
}

std::optional<std::vector<Entry>> list(const Bytes& vol, std::string* error) {
    Geo g;
    if (!parse(vol, g, error)) return std::nullopt;
    std::vector<Entry> out;
    for (const auto& f : rootFiles(vol, g)) out.push_back({f.name, le32(&vol[f.sfn + 28])});
    return out;
}

std::optional<Bytes> read(const Bytes& vol, const std::string& name, std::string* error) {
    Geo g;
    if (!parse(vol, g, error)) return std::nullopt;
    const auto files = rootFiles(vol, g);
    const Found* f = findFile(files, name);
    if (!f) {
        if (error) *error = "no such file";
        return std::nullopt;
    }
    const std::uint32_t e = f->sfn;
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
    if (!parse(vol, g, nullptr)) return false;
    const auto files = rootFiles(vol, g);
    const Found* f = findFile(files, name);
    if (!f) return false;
    freeChain(vol, g, le16(&vol[f->sfn + 26]));
    freeSlots(vol, *f);
    return true;
}

bool write(Bytes& vol, const std::string& name, const Bytes& data, std::string* error) {
    Geo g;
    if (!parse(vol, g, error)) return false;
    if (!validLong(name)) {
        if (error) *error = "not a valid file name: " + name;
        return false;
    }
    {  // replace: the old file's clusters and directory slots are freed first
        const auto files = rootFiles(vol, g);
        if (const Found* f = findFile(files, name)) {
            freeChain(vol, g, le16(&vol[f->sfn + 26]));
            freeSlots(vol, *f);
        }
    }
    char sn[11];
    const bool plain = exactShortName(name, sn);
    if (!plain) makeAlias(vol, g, name, sn);
    const std::uint32_t pieces = plain ? 0 : std::uint32_t((name.size() + 12) / 13);
    const std::uint32_t slots = pieces + 1;
    // Free clusters for the data.
    const std::uint32_t need = std::uint32_t((data.size() + kCluster - 1) / kCluster);
    std::vector<std::uint32_t> got;
    for (std::uint32_t c = 2; c < g.dataClusters + 2 && got.size() < need; c++)
        if (fatGet(vol, g, c) == 0) got.push_back(c);
    if (got.size() < need) {
        if (error) *error = "the volume is full";
        return false;
    }
    // `slots` consecutive free directory entries.
    std::uint32_t e = 0;
    for (std::uint32_t o = g.root, run = 0; o < g.root + kRootSize; o += 32) {
        run = (vol[o] == 0x00 || vol[o] == 0xE5) ? run + 1 : 0;
        if (run == slots) {
            e = o - 32 * (slots - 1);
            break;
        }
    }
    if (!e) {
        if (error) *error = "the root directory is full";
        return false;
    }
    for (size_t i = 0; i < got.size(); i++) {
        fatSet(vol, g, got[i], i + 1 < got.size() ? got[i + 1] : endMark(g));
        const std::uint32_t off = clusterOff(g, got[i]);
        const size_t take = std::min<size_t>(kCluster, data.size() - i * kCluster);
        std::memset(&vol[off], 0, kCluster);
        std::memcpy(&vol[off], data.data() + i * kCluster, take);
    }
    const std::uint8_t sum = lfnChecksum(reinterpret_cast<const std::uint8_t*>(sn));
    static const int kPos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (std::uint32_t p = 0; p < pieces; p++) {  // physical order: last piece first
        const std::uint32_t ord = pieces - p;
        const std::uint32_t o = e + 32 * p;
        std::memset(&vol[o], 0, 32);
        vol[o] = std::uint8_t(ord | (p == 0 ? 0x40 : 0));
        vol[o + 11] = 0x0F;
        vol[o + 13] = sum;
        for (int i = 0; i < 13; i++) {
            const size_t k = size_t(ord - 1) * 13 + size_t(i);
            const std::uint16_t ch = k < name.size() ? std::uint16_t(static_cast<unsigned char>(name[k])) : k == name.size() ? 0 : 0xFFFF;
            putLe16(&vol[o + std::uint32_t(kPos[i])], ch);
        }
    }
    e += 32 * pieces;
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
        if (!write(v, "game_data.bin", a)) return fail("write long name");
        if (!write(v, "B.BIN", b) || !write(v, "game_data.bin", b) || !write(v, "game_data.bin", a)) return fail("rewrite");
        auto ra = read(v, "GAME_DATA.BIN"), rb = read(v, "b.bin");
        if (!ra || *ra != a || !rb || *rb != b) return fail("read back");
        auto l = list(v);
        if (!l || l->size() != 2) return fail("list");
        if (!remove(v, "b.bin") || read(v, "b.bin")) return fail("remove");
        if (!write(v, "a_rather_long_file_name.data", b) || !read(v, "a_rather_long_file_name.data")) return fail("2-piece name");
    }
    return true;
}

}  // namespace recomp::vff
