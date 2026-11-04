// Wild World save files (docs/MOVE_IN.md "The Wild World save"). See ww_save.h.
//
// File formats (community documentation of the emulators' formats, used as data only):
//   - raw: the 256 KB flash image (DeSmuME "raw", melonDS, flash carts, dumpers); 512 KB dumps
//     hold it twice or padded: the half with a valid save is used;
//   - DeSmuME .dsv: the raw image followed by a footer that starts with
//     "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
//   - No$GBA: "NocashGbaBackupMediaSavDataFile" header; the uncompressed variant ("SRAM", method
//     0) is read; the compressed variant is refused with a message.
#include "game/ww_save.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <fstream>

namespace ww {
namespace {

// The game's tables (USA main.dol): player fields 0x8046DC70 + region * 0x24 (Europe = USA),
// copy-level offsets 0x8046DCDC + region * 0x14, player stride 0x8046DD18, copy size
// 0x8046DD28, name length 0x8046DD38.
const LayoutInfo kLayouts[3] = {
    // Japan
    {0x12224, 0x1D10, 0x4, 0xC, {0x24444, 0x24446, 0x24447}, 0x17EF, 0x1CC6, 0x1CC7, 0x1CFC, 0x1D04, 0x1D06, 0x1D0C,
     0x16CC, 6, 1},
    // North America / Europe
    {0x15FE0, 0x228C, 0x4, 0xC, {0x2BFBC, 0x2BFBE, 0x2BFBF}, 0x1C6B, 0x223C, 0x223D, 0x2276, 0x2280, 0x2282, 0x228A,
     0x1B48, 8, 1},
    // Korea
    {0x173FC, 0x249C, 0x4, 0x14, {0x2E7F4, 0x2E7F6, 0x2E7F7}, 0x1D3B, 0x243C, 0x243D, 0x247E, 0x248C, 0x248E, 0x249A,
     0x1C18, 6, 2},
};
constexpr uint32_t kCatalogBytes = 0x123;  // the block ends where the flags byte [0] starts (all layouts)

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

bool blank(const uint8_t* p, uint32_t n) {
    bool zero = true, ff = true;
    for (uint32_t i = 0; i < n && (zero || ff); i++) {
        zero &= p[i] == 0;
        ff &= p[i] == 0xFF;
    }
    return zero || ff;
}

bool copyValid(const std::vector<uint8_t>& img, const LayoutInfo& L, int c) {
    const uint32_t off = c * L.copySize;
    if (off + L.copySize > img.size()) return false;
    return !blank(img.data() + off, L.copySize) && sum16(img.data() + off, L.copySize) == 0;
}

// The best-scoring layout of an image: 2 = both copies valid, 1 = one, 0 = none.
int score(const std::vector<uint8_t>& img, Layout l) {
    const auto& L = layout(l);
    return int(copyValid(img, L, 0)) + int(copyValid(img, L, 1));
}

bool detect(const std::vector<uint8_t>& img, Layout& out) {
    int best = 0;
    for (Layout l : {Layout::USEU, Layout::JP, Layout::KR}) {
        const int s = score(img, l);
        if (s > best) {
            best = s;
            out = l;
        }
    }
    return best > 0;
}

void fill(Save& s) {
    const auto& L = layout(s.layout);
    s.copyOk[0] = copyValid(s.image, L, 0);
    s.copyOk[1] = copyValid(s.image, L, 1);
    s.copy = s.copyOk[0] ? 0 : s.copyOk[1] ? 1 : -1;
    const uint8_t marker = s.image[L.townMarker[1]];
    s.townData = marker == 2 || marker == 0x1C;
    const uint8_t* c = s.image.data() + (s.copy < 0 ? 0 : s.copy) * L.copySize;
    s.townRaw.assign(c + L.townName, c + L.townName + L.nameLen * L.charBytes);
    for (int i = 0; i < 4; i++) {
        const uint8_t* p = c + L.players + i * L.playerStride;
        Player& pl = s.players[i];
        pl.playerId = le16(p + L.playerId);
        pl.townId = le16(p + L.townId);
        pl.exists = pl.playerId != 0 && pl.townId != 0;  // USA fn_80085894
        pl.gender = p[L.gender];
        pl.face = p[L.face] & 0xF;
        pl.hair = p[L.face] >> 4 & 0xF;
        pl.hairColor = p[L.hairColor] & 7;
        pl.nameRaw.assign(p + L.name, p + L.name + L.nameLen * L.charBytes);
    }
}

bool startsWith(const std::vector<uint8_t>& f, const char* s) {
    const size_t n = std::strlen(s);
    return f.size() >= n && std::memcmp(f.data(), s, n) == 0;
}

// The raw image inside a file, or an empty vector (format name in `fmt`, reason in `error`).
std::vector<uint8_t> extract(const std::vector<uint8_t>& file, std::string& fmt, std::string& error) {
    static const char kDsvSnip[] = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
    if (startsWith(file, "NocashGbaBackupMediaSavDataFile")) {
        if (file.size() < 0x50 || std::memcmp(file.data() + 0x40, "SRAM", 4) != 0) {
            error = "This No$GBA save has no SRAM block.";
            return {};
        }
        const uint32_t method = file[0x44] | file[0x45] << 8 | file[0x46] << 16 | uint32_t(file[0x47]) << 24;
        if (method != 0) {
            error = "This is a compressed No$GBA save. Save it uncompressed in No$GBA (or use a raw .sav).";
            return {};
        }
        // Uncompressed: size at 0x48, data at 0x4C.
        fmt = "No$GBA save (uncompressed)";
        return std::vector<uint8_t>(file.begin() + 0x4C, file.end());
    }
    const auto snip = std::search(file.begin(), file.end(), kDsvSnip, kDsvSnip + sizeof(kDsvSnip) - 1);
    if (snip != file.end()) {
        fmt = "DeSmuME .dsv (footer removed)";
        return std::vector<uint8_t>(file.begin(), snip);
    }
    fmt = "Raw backup";
    return file;
}

}  // namespace

const char* regionName(Region r) {
    switch (r) {
    case Region::JP: return "Japan";
    case Region::US: return "North America";
    case Region::EU: return "Europe / Australia";
    case Region::KR: return "Korea";
    }
    return "?";
}

Layout layoutOf(Region r) {
    return r == Region::JP ? Layout::JP : r == Region::KR ? Layout::KR : Layout::USEU;
}

const char* layoutName(Layout l) {
    return l == Layout::JP ? "Japanese" : l == Layout::KR ? "Korean" : "North American / European";
}

const LayoutInfo& layout(Layout l) { return kLayouts[int(l)]; }

int Save::residents() const {
    int n = 0;
    for (auto& p : players) n += p.exists;
    return n;
}

uint16_t sum16(const uint8_t* p, uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2) s += le16(p + i);
    return uint16_t(s);
}

bool load(const std::vector<uint8_t>& file, Save& out, std::string& error) {
    out = Save{};
    std::string fmt;
    std::vector<uint8_t> raw = extract(file, fmt, error);
    if (raw.empty()) {
        if (error.empty()) error = "The file is empty.";
        return false;
    }
    if (raw.size() > 2 * kBackupSize) {
        error = "The file is larger than a Wild World backup (256 KB).";
        return false;
    }
    // Candidates: the image itself (padded with 0xFF like erased flash) and, for a 512 KB
    // dump, its second half.
    std::vector<std::vector<uint8_t>> cands;
    std::vector<uint8_t> first(raw.begin(), raw.begin() + std::min<size_t>(raw.size(), kBackupSize));
    first.resize(kBackupSize, 0xFF);
    cands.push_back(std::move(first));
    if (raw.size() > kBackupSize) {
        std::vector<uint8_t> second(raw.begin() + kBackupSize, raw.end());
        second.resize(kBackupSize, 0xFF);
        cands.push_back(std::move(second));
    }
    for (size_t i = 0; i < cands.size(); i++) {
        Layout l;
        if (!detect(cands[i], l)) continue;
        out.image = std::move(cands[i]);
        out.layout = l;
        char size[64];
        std::snprintf(size, sizeof size, " (%zu KB%s)", raw.size() / 1024, i ? ", second half" : "");
        out.format = fmt + size;
        fill(out);
        return true;
    }
    error = "No valid Wild World save in this file (neither save copy passes the checksum of any region's "
            "layout). Is it an Animal Crossing: Wild World save?";
    return false;
}

bool loadFile(const std::string& path, Save& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "The file could not be opened.";
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load(data, out, error);
}

Region guessRegion(const Save& s, const std::string& path) {
    if (s.layout == Layout::JP) return Region::JP;
    if (s.layout == Layout::KR) return Region::KR;
    std::string n = path;
    const size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    for (const char* k : {"(europe)", "(eur)", "(pal)", "(australia)", "(aus)", "(e)", "(uk)", "(de)", "(fr)",
                          "(it)", "(es)", "admp", "admx", "admy", "admu", "_eur", "_pal"})
        if (n.find(k) != std::string::npos) return Region::EU;
    return Region::US;
}

std::u16string decodeName(const uint8_t* p, int n, Layout l, const CharTable& jp, const CharTable& latin) {
    std::u16string out;
    for (int i = 0; i < n; i++) {
        char16_t ch;
        if (l == Layout::KR) {
            ch = char16_t(le16(p + 2 * i));
            if (ch >= 0xFF10 && ch <= 0xFF5A) ch = char16_t(ch - 0xFEE0);  // full-width -> ASCII (USA fn_80085400)
        } else {
            ch = (l == Layout::JP ? jp : latin)[p[i]];
        }
        if (ch == 0) break;
        out += ch;
    }
    while (!out.empty() && out.back() == u' ') out.pop_back();
    return out;
}

std::string utf8(const std::u16string& s) {
    std::string o;
    for (char16_t c : s) {
        if (c < 0x80) {
            o += char(c);
        } else if (c < 0x800) {
            o += char(0xC0 | c >> 6);
            o += char(0x80 | (c & 0x3F));
        } else {
            o += char(0xE0 | c >> 12);
            o += char(0x80 | (c >> 6 & 0x3F));
            o += char(0x80 | (c & 0x3F));
        }
    }
    return o;
}

std::vector<uint8_t> convert(const Save& s, Layout to, const CharTable& jp, const CharTable& latin) {
    const auto& S = layout(s.layout);
    const auto& D = layout(to);
    std::vector<uint8_t> out(kBackupSize, 0);
    const uint8_t* src = s.image.data() + (s.copy < 0 ? 0 : s.copy) * S.copySize;
    uint8_t* dst = out.data();
    auto encode = [&](const uint8_t* p, uint8_t* q) {
        const std::u16string name = decodeName(p, S.nameLen, s.layout, jp, latin);
        for (int i = 0; i < D.nameLen; i++) {
            char16_t ch = i < int(name.size()) ? name[i] : 0;
            if (to == Layout::KR) {
                put16(q + 2 * i, uint16_t(ch));
                continue;
            }
            const CharTable& t = to == Layout::JP ? jp : latin;
            int code = 0;
            if (ch) {
                code = -1;
                for (int k = 1; k < 256 && code < 0; k++)
                    if (t[k] == ch) code = k;
                if (code < 0) {  // ASCII letters/digits in full width, else '?'
                    const char16_t alt = ch < 0x80 ? char16_t(ch + 0xFEE0) : ch >= 0xFF01 && ch <= 0xFF5E ? char16_t(ch - 0xFEE0) : 0;
                    for (int k = 1; k < 256 && code < 0; k++)
                        if (alt && t[k] == alt) code = k;
                    for (int k = 1; k < 256 && code < 0; k++)
                        if (t[k] == u'?' || t[k] == u'\xFF1F') code = k;
                    if (code < 0) code = 0;
                }
            }
            q[i] = uint8_t(code);
        }
    };
    encode(src + S.townName, dst + D.townName);
    for (int i = 0; i < 4; i++) {
        const uint8_t* p = src + S.players + i * S.playerStride;
        uint8_t* q = dst + D.players + i * D.playerStride;
        std::memcpy(q + D.catalog, p + S.catalog, kCatalogBytes);
        q[D.flags] = p[S.flags];
        q[D.face] = p[S.face];
        q[D.hairColor] = uint8_t((q[D.hairColor] & ~7) | (p[S.hairColor] & 7));
        put16(q + D.playerId, le16(p + S.playerId));
        put16(q + D.townId, le16(p + S.townId));
        encode(p + S.name, q + D.name);
        q[D.gender] = p[S.gender];
    }
    std::memcpy(dst + D.copySize, dst, D.copySize);
    // Town marker (copy-0-relative absolute offsets, inside the second copy).
    put16(dst + D.townMarker[0], le16(s.image.data() + S.townMarker[0]));
    dst[D.townMarker[1]] = s.image[S.townMarker[1]];
    dst[D.townMarker[2]] = s.image[S.townMarker[2]];
    for (int c = 0; c < 2; c++) {
        uint8_t* cp = dst + c * D.copySize;
        put16(cp, 0);
        put16(cp, uint16_t(0x10000 - sum16(cp, D.copySize)));
    }
    std::fill(out.begin() + 2 * D.copySize, out.end(), uint8_t(0xFF));
    // (the marker of USA/EU/JP/KR lies inside the second copy, so it survives the fill)
    return out;
}

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace ww
