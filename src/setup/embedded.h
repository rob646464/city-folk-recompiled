#pragma once
#include <string_view>
#include <utility>
#include <vector>

namespace setup::embedded {

std::string_view symbols();           // config/RUUE01/symbols.txt
std::string_view splits();            // config/RUUE01/splits.txt
// config/RUUE01/rels/<module>.txt: (module file stem, map) pairs.
std::vector<std::pair<std::string_view, std::string_view>> relMaps();
std::string_view hooks();             // tools/recomp/hooks.txt
std::string_view ctxHeader();         // src/recomp/ctx.h
std::string_view moduleAbiHeader();   // src/recomp/module_abi.h
std::string_view moduleRt();          // src/recomp/module_rt.cpp
std::string_view recompilerSource();  // src/recompiler/recompiler.cpp (part of the cache key)

}  // namespace setup::embedded
