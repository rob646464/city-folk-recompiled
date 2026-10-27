#pragma once
// Recompiles a disc's main.dol and compiles the result into the game module DLL with the
// bundled clang + lld (docs/SETUP.md).
#include "setup/disc.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace setup {

struct Progress {
    std::function<void(const std::string& stage, float fraction)> report;
    std::atomic<bool>* cancel = nullptr;
};

// Where a version's module lives: <dataDir>/game/<GAMEID>/module.dll
std::filesystem::path moduleDir(port::GameVersion v);
std::filesystem::path modulePath(port::GameVersion v);

// Cache key of the module for this disc and this executable (inputs, sources, flags).
std::string moduleKey(const Disc& disc);
bool moduleUpToDate(const Disc& disc);

// Locates clang.exe and lld-link.exe; returns false with a message if missing.
bool findToolchain(std::filesystem::path& clang, std::filesystem::path& lld, std::string& error);

bool buildModule(const Disc& disc, const Progress& progress, std::string& error);

}  // namespace setup
