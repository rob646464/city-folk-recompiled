#pragma once
// First-run setup of CityFolkRecomp.exe (docs/SETUP.md): asks for the user's disc (USA, Europe,
// Japan and/or Korea; one is enough), recompiles each disc's main.dol + REL modules into a game module
// in %APPDATA%\Animal Crossing City Folk Recompiled\game\<id>, and says what to launch.
#include "port/install.h"

#include <filesystem>
#include <string>

namespace setup {

struct LaunchInfo {
    port::GameVersion version = port::GameVersion::USA;
    std::string disc;              // UTF-8 path the game runs from (image, or the partition folder)
    std::filesystem::path module;  // game module DLL for that disc
};

struct Options {
    bool forceUi = false;    // --setup: show the setup screen even if everything is ready
    bool autoStart = false;  // --setup-auto: start installing without waiting for a click (testing)
    // --usa-disc / --pal-disc / --jpn-disc / --kor-disc: prefill a region's slot; --disc: any region (it goes
    // to the slot of the region it is).
    std::string disc[port::kVersionCount];
    std::string anyDisc;
};

// Requires an initialised Aurora window (the UI is Dear ImGui). Returns false if the user
// quit. Fills port::install() (discs, readiness flags) for the F1 menu.
bool run(const Options& options, LaunchInfo& launch);

}  // namespace setup
