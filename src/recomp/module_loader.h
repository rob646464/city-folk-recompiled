#pragma once
#include "recomp/ctx.h"

#include <filesystem>
#include <string>

namespace recomp {

// Loads a game module DLL built by setup and makes it the current game (g_module).
// g_mem, tracing and watch state must already be initialised.
bool loadGameModule(const std::filesystem::path& path, std::string& error);

// cfrecomp: the generated tables are linked into the executable.
void setGameModule(const GameModule* module);

}  // namespace recomp
