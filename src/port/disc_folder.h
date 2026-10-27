#pragma once
// The game's disc file system, from one of two sources:
//   - an extracted disc folder: a directory with sys/ (main.dol, fst.bin, boot.bin, ...) and
//     files/, as written by Dolphin's "Extract Entire Disc" (for a Wii disc: the game partition's
//     folder, e.g. "DATA"; resolve() also accepts the folder above it);
//   - a disc image (Wii ISO encrypted or decrypted, WBFS, RVZ, WIA, CISO, GCZ, NFS) through nod:
//     the game partition only, its fst.bin and files read through nod's partition reader.
// The DVD HLE (src/recomp/hle_dvd.cpp) serves the game's file system from here for both, so entry
// numbers, directory order, name matching and reads at the end of a file behave the same. Entry
// numbers and directory order come from fst.bin, so they are the disc's. Works for GameCube and
// Wii discs (the Wii FST stores file offsets shifted right by 2; offsets are not used here).
// Nothing is copied.
#include <cstdint>
#include <string>
#include <vector>

namespace port::discfolder {

// True if `path` is a directory that looks like an extracted disc (sys/fst.bin, files/).
bool isDiscFolder(const std::string& utf8Path);
// The game partition folder a picked path stands for, or "" if none: the partition folder itself
// (sys/ + files/), the folder Dolphin's "Extract Entire Disc" writes for a Wii disc (its DATA/
// subfolder; "P-<id>"-style names of other tools too: the one subfolder with sys/ + files/ that is
// not the update partition), or a file inside the partition's sys/ (main.dol, boot.bin, ...:
// what a file dialog can pick).
std::string resolve(const std::string& utf8Path);

// Opens the folder (sys/fst.bin) or the image (its game partition) as the current disc. False
// with `error` if unusable.
bool open(const std::string& utf8Path, std::string& error);
bool active();   // a disc is open
bool isImage();  // it is an image (else a folder)
const std::string& root();

struct Entry {
    bool isDir = false;
    uint32_t parent = 0;  // directories: parent entry
    uint32_t next = 0;    // directories: first entry after this directory's contents
    uint32_t length = 0;  // files: size in bytes
    uint32_t offset = 0;  // files: the FST offset field (disc offset; >> 2 on the Wii): unique per file
    std::string name;     // as stored in the FST
    std::string path;     // "/dir/file"
};

uint32_t entryCount();
const Entry* entry(int32_t n);
// DVDConvertPathToEntrynum: absolute ("/a/b") or relative to `cwd` (an entry number); "." and
// ".." are understood; names compare case-insensitively like the SDK. -1 if not found.
int32_t pathToEntry(const char* path, uint32_t cwd = 0);
// Reads `len` bytes at `off` of file entry `n` into dst; bytes read (fewer at the end of the
// file, 0 from its end on) or -1.
int64_t read(int32_t n, void* dst, uint32_t len, uint32_t off);
// main.dol (empty if missing).
const std::vector<uint8_t>& dol();
// The game partition's header (boot.bin, 0x440 bytes) and bi2.bin; empty if missing.
const std::vector<uint8_t>& bootBin();
const std::vector<uint8_t>& bi2Bin();

}  // namespace port::discfolder
