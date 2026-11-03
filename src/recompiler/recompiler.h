#pragma once
// Static recompiler: GameCube/Wii main.dol and REL modules (PowerPC 750CL Gekko / Broadway)
// -> C++ (docs/RECOMPILER.md).
//
// Used both by the build (tools/recomp/cfrecompile.cpp, invoked by CMake for the dev target
// cfrecomp) and by CityFolkRecomp.exe at first-run setup (docs/SETUP.md). It runs on the user's
// own executable and modules; the output is never committed.
//
// Function boundaries come from a symbol map (config/<id>/symbols.txt for the DOL,
// config/<id>/rels/<module>.txt per REL; LM's symbols.txt format) where one exists, completed
// by the recompiler's own control-flow scan (branch/call targets, code addresses in data and in
// relocations). Functions in SDK translation units (splits.txt) are emitted as calls into the
// HLE layer (src/recomp/hle_*.cpp) instead of being translated; every other named function can
// be replaced at run time by an HLE handler of the same name.
//
// REL modules (docs/RECOMPILER.md "REL modules"): each module is translated at its section
// offsets. Relocations are resolved at recompile time where the target is static (the DOL, or
// a function of a known module) and read the module's run-time section base otherwise
// (g_relBase, filled by the OSLink HLE in src/recomp/rel.cpp).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cfrc {

// Generated sources are split into this many files (balanced by instruction count) so the
// build can list them without running the generator first.
constexpr int kChunks = 64;

struct RelInput {
    std::string name;               // file name without extension ("d_a_balloonNP")
    std::vector<std::uint8_t> data;  // the .rel file as on the disc
    std::string map;                 // optional function map (symbols.txt format, section-relative offsets)
};

struct Input {
    std::vector<std::uint8_t> dol;  // the retail main.dol
    std::string version;            // game id ("RUUE01", "GLME01", ...)
    std::string symbols;            // symbols.txt contents (may be empty: scanned)
    std::string splits;             // splits.txt contents (SDK translation-unit ranges; optional)
    std::string hooks;              // tools/recomp/hooks.txt contents
    // Optional (non-US bases): "ADDR NAME TU" per line (LM's tools/recomp/gen_version_map.py).
    std::string versionMap;
    // Non-USA versions: config/<id>/us_map.txt ("<addr> <USA addr> <method> <USA name>") and
    // rels_us_map.txt ("<module> <offset> <USA offset> <method>"). hooks.txt lines tagged "all"
    // name USA functions; these maps find them in this version (docs/SETUP.md "Version-aware code").
    std::string usMap;
    std::string relUsMap;
    std::vector<RelInput> rels;     // REL modules to translate with the DOL
};

struct Output {
    std::vector<std::pair<std::string, std::string>> files;  // name -> contents
    std::string summary;
    unsigned functions = 0;
};

// Returns false (with `error`) if the inputs are unusable.
bool recompile(const Input& in, Output& out, std::string& error);

// Names of the generated files, in order (funcs.h, funcs_NNN.cpp..., func_table.cpp).
std::vector<std::string> outputFileNames();

}  // namespace cfrc
