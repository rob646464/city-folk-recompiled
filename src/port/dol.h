#pragma once
// Read-only access to the retail main.dol inside the user's disc image.
//
// Large constant data that the original executable embeds (static archives,
// fonts, parameter tables) is read from here at runtime instead of being
// transcribed into this repository. Addresses are GameCube virtual addresses as
// listed in config/<id>/symbols.txt. All multi-byte values in the
// DOL are big-endian; use the be* helpers or byte-swap explicitly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace port::dol {

// Animal Crossing: City Folk: rev 1 discs USA RUUE01, Europe RUUP01, Japan RUUJ01, Korea RUUK01.
enum class Version { Unknown, RUUE01, RUUP01, RUUJ01, RUUK01 };
const char* versionId(Version v);  // "RUUE01", ..., "unknown"

// Identifies a main.dol image by SHA-1 (Unknown if it is not a supported retail build).
Version identify(const uint8_t* dol, size_t size);
std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len);

// Locates and validates the DOL of the currently opened disc. Must be called
// after aurora_dvd_open(). Returns the detected version (Unknown if the DOL
// does not match a supported retail build).
Version load();
// The DOL of an extracted disc folder (the DVD layer has none): kept by the caller.
void setDol(const uint8_t* dol, size_t size);
Version version();

// Returns `size` bytes at virtual address `addr`, or an empty span if the range
// is not fully inside one initialized DOL section.
std::span<const uint8_t> bytes(uint32_t addr, size_t size);

uint8_t be8(uint32_t addr);
uint16_t be16(uint32_t addr);
uint32_t be32(uint32_t addr);
float bef32(uint32_t addr);

}  // namespace port::dol
