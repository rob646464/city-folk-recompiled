#pragma once
// Extracted disc folders: a directory with sys/ (main.dol, fst.bin, boot.bin, ...) and files/,
// as written by Dolphin's "Extract Entire Disc" (for a Wii disc: the game partition's folder,
// e.g. "DATA"). The DVD HLE (src/recomp/hle_dvd.cpp) serves the game's file system from it:
// entry numbers and directory order come from sys/fst.bin, so they are the disc's; file data is
// read from files/<path>. Works for GameCube and Wii folders (the Wii FST stores file offsets
// shifted right by 2; offsets are not used here). Nothing is copied.
//
// Disc images (ISO/WBFS/RVZ/...) go through Aurora's DVD layer (nod) instead.
#include <cstdint>
#include <string>
#include <vector>

namespace port::discfolder {

// True if `path` is a directory that looks like an extracted disc (sys/fst.bin, files/).
bool isDiscFolder(const std::string& utf8Path);

// Opens the folder as the current disc (reads sys/fst.bin). False with `error` if unusable.
bool open(const std::string& utf8Path, std::string& error);
bool active();  // a folder is the current disc
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
// Reads `len` bytes at `off` of file entry `n` into dst; bytes read or -1.
int64_t read(int32_t n, void* dst, uint32_t len, uint32_t off);
// sys/main.dol (empty if missing).
const std::vector<uint8_t>& dol();
// Disc header (sys/boot.bin, 0x440 bytes) and sys/bi2.bin; empty if missing.
const std::vector<uint8_t>& bootBin();
const std::vector<uint8_t>& bi2Bin();

}  // namespace port::discfolder
