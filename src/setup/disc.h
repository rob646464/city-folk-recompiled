#pragma once
// Read-only access to a user's disc during setup, independent of the disc the game later runs
// from (the DVD HLE): a Wii disc image (ISO, WBFS, RVZ, ... via nod: the game partition) or an
// extracted disc folder (sys/, files/; port/disc_folder.h).
#include "port/install.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct NodHandle;

namespace setup {

class Disc {
public:
    Disc() = default;
    ~Disc();
    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;

    // Opens the disc and identifies its main.dol. Returns false with a user-facing message.
    bool open(const std::string& utf8Path, std::string& error);
    port::GameVersion version() const { return version_; }
    const std::vector<std::uint8_t>& dol() const { return dol_; }
    std::string dolSha1() const;  // hex
    // Reads a whole file by disc path ("/rels/d_a_balloonNP.rel"); false if missing.
    bool readFile(const char* path, std::vector<std::uint8_t>& out) const;

private:
    NodHandle* disc_ = nullptr;
    NodHandle* part_ = nullptr;
    std::filesystem::path folder_;  // extracted folder (else the nod partition)
    std::vector<std::uint8_t> dol_;
    port::GameVersion version_ = port::GameVersion::USA;
};

// Opens and identifies a disc without keeping it open; for validating a picked file.
bool probeDisc(const std::string& utf8Path, port::GameVersion& version, std::string& error);

std::string hex(const std::uint8_t* data, size_t n);

}  // namespace setup
