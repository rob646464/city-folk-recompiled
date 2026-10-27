#pragma once
// VFF: the FAT volume images WiiConnect24 keeps on the NAND (a title's wc24dl.vff, the message
// boxes wc24send.mbx / wc24recv.mbx). The game reads them with its own VF library (recompiled
// game code); the host side here is what IOS's KD does: it writes a downloaded file into the
// title's wc24dl.vff, and the provisioning formats empty message boxes. docs/DISTRIBUTIONS.md
// "VFF". Layout: a 0x20-byte header ("VFF ", size), two FAT copies (FAT12 or FAT16 by cluster
// count), a 0x1000-byte root directory, then 0x200-byte clusters. 8.3 names, no long names.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace recomp::vff {

using Bytes = std::vector<std::uint8_t>;

// A new empty volume of `size` bytes (a multiple of 0x200).
Bytes format(std::uint32_t size);

// Files in the root directory (upper-case 8.3 names as stored, e.g. "INFO.BIN").
struct Entry {
    std::string name;
    std::uint32_t size = 0;
};
std::optional<std::vector<Entry>> list(const Bytes& vol, std::string* error = nullptr);
std::optional<Bytes> read(const Bytes& vol, const std::string& name, std::string* error = nullptr);
// Writes (replaces) a root-directory file. False with `error` when the name is not a valid 8.3
// name, the volume is not a VFF or there is no room.
bool write(Bytes& vol, const std::string& name, const Bytes& data, std::string* error = nullptr);
bool remove(Bytes& vol, const std::string& name);

// Self-test on an in-memory volume (format, write, rewrite, read back, list, remove).
bool selfTest(std::string* failure = nullptr);

}  // namespace recomp::vff
