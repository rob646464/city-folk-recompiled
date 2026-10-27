#pragma once
// Read-only access to a user's disc during setup, independent of the disc the game later runs
// from (the DVD HLE): a Wii disc image (ISO, WBFS, RVZ, WIA, CISO, GCZ, NFS; encrypted or
// decrypted: via nod, the game partition only) or an extracted disc folder (port/disc_folder.h:
// the partition folder or a whole-disc extraction with DATA/).
//
// A disc is accepted only when it is a supported release, identified by game ID + revision from
// the partition header, the SHA-1 of main.dol and the SHA-1 of every REL module against the
// version's config/<id>/modules.txt (docs/SETUP.md "Discs").
#include "port/install.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct NodHandle;

namespace setup {

class Disc {
public:
    Disc() = default;
    ~Disc();
    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;

    // Opens the disc, identifies it and checks main.dol and every REL. Returns false with a
    // user-facing message.
    bool open(const std::string& utf8Path, std::string& error);
    port::GameVersion version() const { return version_; }
    const std::vector<std::uint8_t>& dol() const { return dol_; }
    std::string dolSha1() const;  // hex
    // The REL modules of files/rels, in modules.txt order: (module name, file contents).
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& rels() const { return rels_; }
    // What kind of disc it is, for the setup screen ("RVZ image", "extracted folder", ...).
    const std::string& kind() const { return kind_; }
    // The path the game runs from: the image, or the partition folder an extracted disc resolved to.
    const std::string& runPath() const { return runPath_; }
    // Reads a whole file by disc path ("/rels/d_a_balloonNP.rel"); false if missing.
    bool readFile(const char* path, std::vector<std::uint8_t>& out) const;

private:
    NodHandle* disc_ = nullptr;
    NodHandle* part_ = nullptr;
    std::filesystem::path folder_;  // extracted folder (else the nod partition)
    std::vector<std::uint8_t> dol_;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> rels_;
    port::GameVersion version_ = port::GameVersion::USA;
    std::string kind_, runPath_;
};

// Opens and identifies a disc without keeping it open; for validating a picked file.
bool probeDisc(const std::string& utf8Path, port::GameVersion& version, std::string& error);

// "Animal Crossing: City Folk (USA, RUUE01 rev 1)".
std::string describe(port::GameVersion v);

std::string hex(const std::uint8_t* data, size_t n);

}  // namespace setup
