#pragma once
#include "port/install.h"

#include <string_view>
#include <utility>
#include <vector>

namespace setup::embedded {

// The function tables of one release (config/<id>/, docs/DISCOVERY.md).
struct Tables {
    std::string_view symbols;  // symbols.txt (main.dol)
    std::string_view splits;   // splits.txt (SDK translation units)
    std::string_view modules;  // modules.txt (REL list: id, name, SHA-1, sections)
    // rels/<module>.txt: (module file stem, map) pairs.
    std::vector<std::pair<std::string_view, std::string_view>> relMaps;
    // Non-USA versions: us_map.txt / rels_us_map.txt (hooks.txt "all" entries); empty for USA.
    std::string_view usMap, relUsMap;
};
const Tables& tables(port::GameVersion v);

// One REL of modules.txt.
struct RelEntry {
    unsigned id = 0;
    std::string_view name;  // "d_a_balloonNP" (files/rels/<name>.rel)
    std::string_view sha1;  // hex
};
std::vector<RelEntry> relList(port::GameVersion v);

std::string_view hooks();             // tools/recomp/hooks.txt
std::string_view ctxHeader();         // src/recomp/ctx.h
std::string_view moduleAbiHeader();   // src/recomp/module_abi.h
std::string_view moduleRt();          // src/recomp/module_rt.cpp
std::string_view recompilerSource();  // src/recompiler/recompiler.cpp (part of the cache key)

}  // namespace setup::embedded
