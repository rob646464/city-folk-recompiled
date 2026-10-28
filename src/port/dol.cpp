#include "port/dol.h"

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <array>
#include <bit>
#include <cstring>

namespace port::dol {
namespace {

struct Section {
    uint32_t fileOffset;
    uint32_t addr;
    uint32_t size;
};

const uint8_t* sDol;
size_t sDolSize;
std::array<Section, 18> sSections;  // 7 text + 11 data
Version sVersion = Version::Unknown;

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

// Minimal SHA-1 (FIPS 180-1) used to identify the retail DOL and to key setup's caches.
std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint64_t bitLen = uint64_t(len) * 8;
    const size_t padded = ((len + 8) / 64 + 1) * 64;
    auto byteAt = [&](size_t i) -> uint8_t {
        if (i < len) return data[i];
        if (i == len) return 0x80;
        if (i >= padded - 8) return uint8_t(bitLen >> (8 * (padded - 1 - i)));
        return 0;
    };
    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint32_t w[80];
        if (chunk + 64 <= len) {  // whole block of the input (setup hashes every REL: keep it quick)
            const uint8_t* q = data + chunk;
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(q[i * 4]) << 24) | (uint32_t(q[i * 4 + 1]) << 16) | (uint32_t(q[i * 4 + 2]) << 8) | q[i * 4 + 3];
        } else {
            for (int i = 0; i < 16; ++i) {
                w[i] = (uint32_t(byteAt(chunk + i * 4)) << 24) | (uint32_t(byteAt(chunk + i * 4 + 1)) << 16) |
                       (uint32_t(byteAt(chunk + i * 4 + 2)) << 8) | byteAt(chunk + i * 4 + 3);
            }
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = std::rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = std::rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = uint8_t(h[i] >> 24);
        out[i * 4 + 1] = uint8_t(h[i] >> 16);
        out[i * 4 + 2] = uint8_t(h[i] >> 8);
        out[i * 4 + 3] = uint8_t(h[i]);
    }
    return out;
}

namespace {

// SHA-1 of each supported retail main.dol (rev 1 game partitions).
constexpr uint8_t kShaRUUE01[20] = {0xa1, 0xce, 0x84, 0xf7, 0x89, 0x86, 0xb1, 0x8c, 0x7b, 0x63, 0xc4, 0xc5, 0x8b, 0x61, 0x84, 0x11, 0x17, 0xf8, 0xc0, 0x87};
constexpr uint8_t kShaRUUP01[20] = {0xea, 0x33, 0x2f, 0x76, 0xb8, 0x4b, 0xf4, 0x6e, 0x4e, 0x0d, 0x78, 0x78, 0x6d, 0x7c, 0xd0, 0x2b, 0xcf, 0xb9, 0x5d, 0x11};
constexpr uint8_t kShaRUUJ01[20] = {0xd8, 0xc6, 0x89, 0x54, 0xe6, 0xe4, 0xf1, 0xaf, 0x8d, 0x94, 0xb6, 0x0e, 0x05, 0xab, 0xaf, 0x64, 0x7d, 0x6e, 0x49, 0x23};
constexpr uint8_t kShaRUUK01[20] = {0x5f, 0x1f, 0xca, 0xbf, 0xeb, 0x22, 0x12, 0x70, 0x41, 0x73, 0xef, 0x54, 0x78, 0x83, 0xbd, 0x54, 0xa2, 0x51, 0x90, 0x5d};

}  // namespace

Version identify(const uint8_t* dol, size_t size) {
    if (!dol || size < 0x100) return Version::Unknown;
    const auto digest = sha1(dol, size);
    if (std::memcmp(digest.data(), kShaRUUE01, 20) == 0) return Version::RUUE01;
    if (std::memcmp(digest.data(), kShaRUUP01, 20) == 0) return Version::RUUP01;
    if (std::memcmp(digest.data(), kShaRUUJ01, 20) == 0) return Version::RUUJ01;
    if (std::memcmp(digest.data(), kShaRUUK01, 20) == 0) return Version::RUUK01;
    return Version::Unknown;
}

const char* versionId(Version v) {
    switch (v) {
    case Version::RUUE01: return "RUUE01";
    case Version::RUUP01: return "RUUP01";
    case Version::RUUJ01: return "RUUJ01";
    case Version::RUUK01: return "RUUK01";
    default: return "unknown";
    }
}

void setDol(const uint8_t* dol, size_t size) {
    sDol = dol;
    sDolSize = size;
    for (size_t i = 0; i < sSections.size(); ++i) {
        sSections[i] = {read_be32(sDol + i * 4), read_be32(sDol + 0x48 + i * 4), read_be32(sDol + 0x90 + i * 4)};
    }
    sVersion = identify(sDol, sDolSize);
}

Version load() {
    s32 size = 0;
    sDol = DVDGetDOLLocation(&size);
    sDolSize = size > 0 ? size_t(size) : 0;
    sVersion = Version::Unknown;
    if (!sDol || sDolSize < 0x100) {
        OSReport("[dol] no DOL found in disc image\n");
        return sVersion;
    }
    for (size_t i = 0; i < sSections.size(); ++i) {
        sSections[i] = {read_be32(sDol + i * 4), read_be32(sDol + 0x48 + i * 4), read_be32(sDol + 0x90 + i * 4)};
    }
    sVersion = identify(sDol, sDolSize);
    if (sVersion == Version::Unknown)
        OSReport("[dol] main.dol does not match a supported retail build (RUUE01/RUUP01/RUUJ01/RUUK01 rev 1)\n");
    return sVersion;
}

Version version() { return sVersion; }

std::span<const uint8_t> bytes(uint32_t addr, size_t size) {
    for (const Section& s : sSections) {
        if (s.size != 0 && addr >= s.addr && addr - s.addr + size <= s.size && s.fileOffset + s.size <= sDolSize) {
            return {sDol + s.fileOffset + (addr - s.addr), size};
        }
    }
    return {};
}

uint8_t be8(uint32_t addr) {
    auto b = bytes(addr, 1);
    return b.empty() ? 0 : b[0];
}

uint16_t be16(uint32_t addr) {
    auto b = bytes(addr, 2);
    return b.empty() ? 0 : uint16_t((b[0] << 8) | b[1]);
}

uint32_t be32(uint32_t addr) {
    auto b = bytes(addr, 4);
    return b.empty() ? 0 : read_be32(b.data());
}

float bef32(uint32_t addr) { return std::bit_cast<float>(be32(addr)); }

}  // namespace port::dol
