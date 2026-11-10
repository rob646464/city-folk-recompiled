// City Folk's save file and the Wii's save containers (cf_save_data.h). Written from the game's
// own code (checksum writers, disassembled from this disc) and public Wii documentation; AES from
// FIPS 197 / SP 800-38A. No third-party code, no keys.
#include "game/cf_save_data.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace cfsave {

namespace {

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
void put32(u8* p, u32 v) {
    p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v);
}

// UTF-16BE units (stop at 0) -> UTF-8.
std::string utf16beToUtf8(const u8* p, int maxUnits) {
    std::string s;
    for (int i = 0; i < maxUnits; i++) {
        const unsigned ch = be16(p + 2 * i);
        if (!ch) break;
        if (ch < 0x80) s += char(ch);
        else if (ch < 0x800) s += char(0xC0 | ch >> 6), s += char(0x80 | (ch & 0x3F));
        else s += char(0xE0 | ch >> 12), s += char(0x80 | (ch >> 6 & 0x3F)), s += char(0x80 | (ch & 0x3F));
    }
    return s;
}

// ---- AES-128 ----------------------------------------------------------------------------------

struct Tables {
    u8 sbox[256], inv[256];
    Tables() {
        // S-box from its definition: multiplicative inverse in GF(2^8) followed by the affine map.
        u8 p = 1, q = 1;
        do {
            p = u8(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));  // p *= 3
            q ^= u8(q << 1), q ^= u8(q << 2), q ^= u8(q << 4);  // q /= 3
            if (q & 0x80) q ^= 0x09;
            const u8 x = u8(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
            sbox[p] = u8(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) inv[sbox[i]] = u8(i);
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

u8 xt(u8 a) { return u8(a << 1 ^ (a & 0x80 ? 0x1B : 0)); }
u8 mul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

void expandKey(const u8 key[16], u8 rk[176]) {
    const auto& t = tables();
    std::memcpy(rk, key, 16);
    u8 rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        u8 w[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            const u8 w0 = w[0];
            w[0] = u8(t.sbox[w[1]] ^ rcon), w[1] = t.sbox[w[2]], w[2] = t.sbox[w[3]], w[3] = t.sbox[w0];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = u8(rk[i - 16 + k] ^ w[k]);
    }
}

void encryptBlock(const u8 rk[176], u8 s[16]) {
    const auto& t = tables();
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round <= 10; round++) {
        u8 n[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) n[r + 4 * c] = t.sbox[s[r + 4 * ((c + r) & 3)]];  // SubBytes + ShiftRows
        if (round != 10)
            for (int c = 0; c < 4; c++) {
                const u8 a0 = n[4 * c], a1 = n[4 * c + 1], a2 = n[4 * c + 2], a3 = n[4 * c + 3];
                n[4 * c] = u8(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                n[4 * c + 1] = u8(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                n[4 * c + 2] = u8(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                n[4 * c + 3] = u8(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        for (int i = 0; i < 16; i++) s[i] = u8(n[i] ^ rk[16 * round + i]);
    }
}

void decryptBlock(const u8 rk[176], u8 s[16]) {
    const auto& t = tables();
    for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (int round = 9; round >= 0; round--) {
        u8 n[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) n[r + 4 * ((c + r) & 3)] = t.inv[s[r + 4 * c]];  // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; i++) n[i] ^= rk[16 * round + i];
        if (round != 0)
            for (int c = 0; c < 4; c++) {
                const u8 a0 = n[4 * c], a1 = n[4 * c + 1], a2 = n[4 * c + 2], a3 = n[4 * c + 3];
                n[4 * c] = u8(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                n[4 * c + 1] = u8(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                n[4 * c + 2] = u8(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                n[4 * c + 3] = u8(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
            }
        std::memcpy(s, n, 16);
    }
}

}  // namespace

void aes128CbcDecrypt(const u8 key[16], u8 iv[16], u8* data, size_t len) {
    u8 rk[176];
    expandKey(key, rk);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        u8 c[16];
        std::memcpy(c, data + o, 16);
        decryptBlock(rk, data + o);
        for (int i = 0; i < 16; i++) data[o + i] ^= iv[i];
        std::memcpy(iv, c, 16);
    }
}

void aes128CbcEncrypt(const u8 key[16], u8 iv[16], u8* data, size_t len) {
    u8 rk[176];
    expandKey(key, rk);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        for (int i = 0; i < 16; i++) data[o + i] ^= iv[i];
        encryptBlock(rk, data + o);
        std::memcpy(iv, data + o, 16);
    }
}

bool aesSelfTest() {
    u8 key[16], pt[16], iv[16] = {};
    for (int i = 0; i < 16; i++) key[i] = u8(i), pt[i] = u8(i * 0x11);
    static const u8 ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                              0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    u8 b[16];
    std::memcpy(b, pt, 16);
    aes128CbcEncrypt(key, iv, b, 16);  // zero IV: CBC of one block = the cipher itself
    if (std::memcmp(b, ct, 16) != 0) return false;
    std::memset(iv, 0, 16);
    aes128CbcDecrypt(key, iv, b, 16);
    return std::memcmp(b, pt, 16) == 0;
}

// ---- names and regions --------------------------------------------------------------------------

Region regionOfGameCode(u32 code) {
    for (int i = 0; i < 4; i++)
        if (kGameCodes[i] == code) return Region(i);
    return kUnknown;
}

const char* regionName(Region r) {
    switch (r) {
    case kJapan: return "Japan";
    case kUsa: return "USA";
    case kEurope: return "Europe";
    case kKorea: return "Korea";
    default: return "unknown";
    }
}

std::string gameCodeText(u32 code) {
    std::string s;
    for (int sh = 24; sh >= 0; sh -= 8) {
        const char ch = char(code >> sh & 0xFF);
        s += std::isprint(static_cast<unsigned char>(ch)) ? ch : '?';
    }
    return s;
}

std::string titleHex(u32 code) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", code);
    return b;
}

// ---- rvforest.dat ------------------------------------------------------------------------------

u32 crc32(const u8* data, size_t n, u32 init, u32 xorout) {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    u32 c = init;
    for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ xorout;
}

const std::vector<Checksum>& checksums() {
    // fn_8010E1CC (the save writers' checksum pass) -> fn_80115E08 (file header fn_801153E4, each
    // resident fn_8011A85C + fn_80136E40, the block after the residents fn_801347B4, the town
    // fn_80115F00), fn_8011719C (second block fn_80143FCC + fn_80117218), fn_80115D0C (third block,
    // its own start value). Load checks compare the same ranges (e.g. fn_8013476C).
    static const std::vector<Checksum> list = [] {
        std::vector<Checksum> v;
        v.push_back({"file header", 0x0, 0x4, 0x1C, 0xFFFFFFFF});
        static const char* kRes[4][2] = {{"resident 1 (part 1)", "resident 1 (part 2)"},
                                         {"resident 2 (part 1)", "resident 2 (part 2)"},
                                         {"resident 3 (part 1)", "resident 3 (part 2)"},
                                         {"resident 4 (part 1)", "resident 4 (part 2)"}};
        for (u32 i = 0; i < kResidents; i++) {
            const u32 p = kResidentBase + i * kResidentSize;
            v.push_back({kRes[i][0], p, p + 4, 0x111C, 0xFFFFFFFF});
            v.push_back({kRes[i][1], p + 0x1120, p + 0x1124, 0x759C, 0xFFFFFFFF});
        }
        v.push_back({"block after the residents", 0x5E240, 0x5E244, 0x1C, 0xFFFFFFFF});
        v.push_back({"town data", 0x5EC60, 0x5EC64, 0x1497C, 0xFFFFFFFF});
        v.push_back({"second block header", 0x735E0, 0x735E4, 0x1C, 0xFFFFFFFF});
        v.push_back({"second block", 0x73600, 0x73604, 0x19BD1C, 0xFFFFFFFF});
        v.push_back({"third block", 0x20F320, 0x20F324, 0x200000, 0x04201018});
        return v;
    }();
    return list;
}

void fixChecksums(std::vector<u8>& save) {
    if (save.size() != kSaveSize) return;
    // Inner ranges first (the town struct lies inside nothing else; ranges do not overlap).
    for (const auto& c : checksums()) put32(save.data() + c.at, crc32(save.data() + c.start, c.len, c.init));
}

std::string SaveInfo::problem() const {
    if (!sizeOk) return "it is not a City Folk save file (rvforest.dat is 4,256,576 bytes)";
    if (!badChecksums.empty()) {
        std::string s = "the file is damaged: its checksum does not match (";
        for (size_t i = 0; i < badChecksums.size() && i < 3; i++) s += (i ? ", " : "") + badChecksums[i];
        if (badChecksums.size() > 3) s += ", ...";
        return s + "), so the game would refuse it";
    }
    if (residents.empty()) return "the save has no resident";
    return "";
}

SaveInfo inspectSave(const std::vector<u8>& save) {
    SaveInfo si;
    si.sizeOk = save.size() == kSaveSize;
    if (!si.sizeOk) return si;
    const u8* d = save.data();
    for (const auto& c : checksums())
        if (be32(d + c.at) != crc32(d + c.start, c.len, c.init)) si.badChecksums.push_back(c.what);
    for (u32 i = 0; i < kResidents; i++) {
        const u8* pd = d + kResidentBase + i * kResidentSize;
        const u16 player = be16(pd + kPlayerStruct);
        if (!player || player == 0xFFFF) continue;
        std::string n = utf16beToUtf8(pd + kPlayerStruct + 2, 9);
        si.residents.push_back(n.empty() ? "(no name)" : n);
        if (si.town.empty()) {
            si.townId = be16(pd + kTownStruct);
            si.town = utf16beToUtf8(pd + kTownStruct + 2, 9);
        }
    }
    const u8 tb = d[kTownRegionByte];
    si.townLanguage = tb >> 4;
    si.townRegion = (tb & 0xF) <= 3 ? Region(tb & 0xF) : kUnknown;
    return si;
}

bool bannerLooksValid(const std::vector<u8>& b) {
    return b.size() >= 0x60C0 && b.size() <= kBannerMaxSize && std::memcmp(b.data(), "WIBN", 4) == 0;
}

// ---- data.bin -----------------------------------------------------------------------------------

bool parseDataBin(const std::vector<u8>& bin, DataBin& out, std::string& error) {
    out = {};
    constexpr size_t kBk = kDataBinHeaderSize;
    if (bin.size() < kBk + kBkHeaderSize) return error = "too short for a Wii SD-card save (data.bin)", false;
    const u8* bk = bin.data() + kBk;
    if (be32(bk) != 0x70 || bk[4] != 'B' || bk[5] != 'k' || be16(bk + 6) != 1)
        return error = "not a Wii SD-card save (no \"Bk\" header at 0xF0C0)", false;
    out.ngId = be32(bk + 8);
    const u32 count = be32(bk + 0x0C);
    const u32 filesSize = be32(bk + 0x10);
    out.titleId = u64(be32(bk + 0x60)) << 32 | be32(bk + 0x64);
    if (count > 64) return error = "the save lists too many files", false;
    size_t off = kBk + kBkHeaderSize;
    const size_t end = off + filesSize;
    if (end > bin.size()) return error = "the file is cut short (its files run past the end)", false;
    for (u32 i = 0; i < count; i++) {
        if (off + 0x80 > end) return error = "the file is cut short (file headers)", false;
        const u8* h = bin.data() + off;
        if (be32(h) != 0x03ADF17E) return error = "a file header is damaged (magic)", false;
        DataBinFile f;
        f.size = be32(h + 4);
        f.perm = h[8], f.attr = h[9], f.type = h[0x0A];
        for (int k = 0x0B; k < 0x50 && h[k]; k++) f.name += char(h[k]);
        std::memcpy(f.iv.data(), h + 0x50, 16);
        f.dataOffset = off + 0x80;
        const size_t padded = f.type == 1 ? (size_t(f.size) + 0x3F) & ~size_t(0x3F) : 0;
        if (f.dataOffset + padded > end) return error = "the file is cut short (\"" + f.name + "\")", false;
        out.files.push_back(f);
        off = f.dataOffset + padded;
    }
    return true;
}

bool decryptDataBin(const std::vector<u8>& bin, const DataBin& info, const u8 sdKey[16], const u8 sdIv[16],
                    DataBinContents& out, std::string& error) {
    out = {};
    std::vector<u8> hdr(bin.begin(), bin.begin() + kDataBinHeaderSize);
    u8 iv[16];
    std::memcpy(iv, sdIv, 16);
    aes128CbcDecrypt(sdKey, iv, hdr.data(), hdr.size());
    const u64 tid = u64(be32(hdr.data())) << 32 | be32(hdr.data() + 4);
    if (tid != info.titleId || std::memcmp(hdr.data() + 0x20, "WIBN", 4) != 0)
        return error = "the SD key or IV is not right (the decrypted header does not name this save's title)", false;
    const u32 bannerSize = be32(hdr.data() + 8);
    if (bannerSize >= 0x20 && bannerSize <= kDataBinHeaderSize - 0x20)
        out.banner.assign(hdr.begin() + 0x20, hdr.begin() + 0x20 + bannerSize);
    for (const auto& f : info.files) {
        if (f.type != 1) continue;  // folders: City Folk keeps its files at the top of its data folder
        const size_t padded = (size_t(f.size) + 0x3F) & ~size_t(0x3F);
        std::vector<u8> data(bin.begin() + f.dataOffset, bin.begin() + f.dataOffset + padded);
        u8 fiv[16];
        std::memcpy(fiv, f.iv.data(), 16);
        aes128CbcDecrypt(sdKey, fiv, data.data(), data.size());
        data.resize(f.size);
        out.files.emplace_back(f.name, std::move(data));
    }
    return true;
}

std::vector<u8> buildDataBin(u64 titleId, const std::vector<u8>& banner,
                             const std::vector<std::pair<std::string, std::vector<u8>>>& files, const u8 key[16],
                             const u8 iv[16]) {
    std::vector<u8> out(kDataBinHeaderSize + kBkHeaderSize, 0);
    put32(out.data(), u32(titleId >> 32));
    put32(out.data() + 4, u32(titleId));
    put32(out.data() + 8, u32(banner.size()));
    out[0x0C] = 0x3C;
    std::memcpy(out.data() + 0x20, banner.data(), std::min(banner.size(), kDataBinHeaderSize - 0x20));
    u8 v[16];
    std::memcpy(v, iv, 16);
    aes128CbcEncrypt(key, v, out.data(), kDataBinHeaderSize);
    u8* bk = out.data() + kDataBinHeaderSize;
    put32(bk, 0x70);
    bk[4] = 'B', bk[5] = 'k', bk[7] = 1;
    put32(bk + 8, 0x0403AC68);  // a made-up NG id
    put32(bk + 0x0C, u32(files.size()));
    u32 filesSize = 0;
    for (size_t i = 0; i < files.size(); i++) {
        const auto& [name, data] = files[i];
        std::vector<u8> h(0x80, 0);
        put32(h.data(), 0x03ADF17E);
        put32(h.data() + 4, u32(data.size()));
        h[8] = 0x3C, h[0x0A] = 1;
        std::memcpy(h.data() + 0x0B, name.data(), std::min<size_t>(name.size(), 0x44));
        for (int k = 0; k < 16; k++) h[0x50 + k] = u8(0xA0 + i * 16 + k);  // per-file IV
        std::vector<u8> enc(data);
        enc.resize((enc.size() + 0x3F) & ~size_t(0x3F), 0);
        u8 fiv[16];
        std::memcpy(fiv, h.data() + 0x50, 16);
        aes128CbcEncrypt(key, fiv, enc.data(), enc.size());
        out.insert(out.end(), h.begin(), h.end());
        out.insert(out.end(), enc.begin(), enc.end());
        filesSize += u32(h.size() + enc.size());
    }
    bk = out.data() + kDataBinHeaderSize;
    put32(bk + 0x10, filesSize);
    put32(bk + 0x1C, filesSize + 0x3C0);
    put32(bk + 0x60, u32(titleId >> 32));
    put32(bk + 0x64, u32(titleId));
    out.resize(out.size() + 0x40 + 0x340, 0);  // signature + certificates (zero here)
    return out;
}

bool parseKey16(const std::string& s, u8 out[16]) {
    if (s.size() == 16) {  // a binary key file
        bool binary = false;
        for (unsigned char ch : s)
            if (!std::isxdigit(ch)) binary = true;
        if (binary) {
            std::memcpy(out, s.data(), 16);
            return true;
        }
    }
    std::string hex;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            i++;
            continue;
        }
        if (std::isxdigit(ch)) hex += char(ch);
        else if (!std::isspace(ch) && ch != ':' && ch != '-' && ch != ',') return false;
    }
    if (hex.size() != 32) return false;
    for (int i = 0; i < 16; i++) out[i] = u8(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return true;
}

}  // namespace cfsave
