// Inputs of first-run setup, embedded in the executable so they always match the runtime
// they were built with: the function tables of all four releases (addresses and names only),
// the hook list, and the freestanding sources compiled into the game module. The arrays are
// generated at build time by tools/recomp/cfembed.cpp (CMakeLists.txt, CF_EMBEDDED_FILES).
#include "setup/embedded.h"

#include <mutex>

namespace setup::embedded {
namespace data {
#define CF_EMBEDDED(name)       \
    extern const char name[];   \
    extern const unsigned long long name##Size;
#define CF_EMBEDDED_VERSION(id) \
    CF_EMBEDDED(kSymbols_##id)  \
    CF_EMBEDDED(kSplits_##id)   \
    CF_EMBEDDED(kModules_##id)  \
    CF_EMBEDDED(kRelMaps_##id)
CF_EMBEDDED_VERSION(RUUE01)
CF_EMBEDDED_VERSION(RUUP01)
CF_EMBEDDED_VERSION(RUUJ01)
CF_EMBEDDED_VERSION(RUUK01)
CF_EMBEDDED(kUsMap_RUUP01)
CF_EMBEDDED(kRelUsMap_RUUP01)
CF_EMBEDDED(kUsMap_RUUJ01)
CF_EMBEDDED(kRelUsMap_RUUJ01)
CF_EMBEDDED(kUsMap_RUUK01)
CF_EMBEDDED(kRelUsMap_RUUK01)
CF_EMBEDDED(kHooks)
CF_EMBEDDED(kCtxHeader)
CF_EMBEDDED(kModuleAbiHeader)
CF_EMBEDDED(kModuleRt)
CF_EMBEDDED(kRecompilerSource)
#undef CF_EMBEDDED_VERSION
#undef CF_EMBEDDED
}  // namespace data

namespace {
std::string_view view(const char* p, unsigned long long n) { return {p, size_t(n)}; }

// cfembed's folder format: "\x01<stem>\n" before each file.
std::vector<std::pair<std::string_view, std::string_view>> splitFolder(std::string_view all) {
    std::vector<std::pair<std::string_view, std::string_view>> out;
    size_t p = 0;
    while (p < all.size() && all[p] == '\x01') {
        const size_t nl = all.find('\n', p);
        if (nl == std::string_view::npos) break;
        const std::string_view stem = all.substr(p + 1, nl - p - 1);
        size_t end = all.find('\x01', nl + 1);
        if (end == std::string_view::npos) end = all.size();
        out.push_back({stem, all.substr(nl + 1, end - nl - 1)});
        p = end;
    }
    return out;
}
}  // namespace

using namespace data;

const Tables& tables(port::GameVersion v) {
    static Tables sTables[port::kVersionCount];
    static std::once_flag sOnce;
    std::call_once(sOnce, [] {
#define CF_TABLES(i, id)                                                          \
    sTables[i].symbols = view(kSymbols_##id, kSymbols_##id##Size);               \
    sTables[i].splits = view(kSplits_##id, kSplits_##id##Size);                  \
    sTables[i].modules = view(kModules_##id, kModules_##id##Size);               \
    sTables[i].relMaps = splitFolder(view(kRelMaps_##id, kRelMaps_##id##Size));
        CF_TABLES(0, RUUE01)
        CF_TABLES(1, RUUP01)
        CF_TABLES(2, RUUJ01)
        CF_TABLES(3, RUUK01)
#undef CF_TABLES
#define CF_US_MAPS(i, id)                                                             sTables[i].usMap = view(kUsMap_##id, kUsMap_##id##Size);                         sTables[i].relUsMap = view(kRelUsMap_##id, kRelUsMap_##id##Size);
        CF_US_MAPS(1, RUUP01)
        CF_US_MAPS(2, RUUJ01)
        CF_US_MAPS(3, RUUK01)
#undef CF_US_MAPS
    });
    return sTables[int(v)];
}

std::vector<RelEntry> relList(port::GameVersion v) {
    // "<id> <name> <sha1> <sections...> imports:..." per line; '#' comments.
    std::vector<RelEntry> out;
    const std::string_view all = tables(v).modules;
    size_t p = 0;
    while (p < all.size()) {
        size_t nl = all.find('\n', p);
        if (nl == std::string_view::npos) nl = all.size();
        std::string_view line = all.substr(p, nl - p);
        p = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t s1 = line.find(' '), s2 = s1 == std::string_view::npos ? s1 : line.find(' ', s1 + 1);
        if (s2 == std::string_view::npos) continue;
        size_t s3 = line.find(' ', s2 + 1);
        if (s3 == std::string_view::npos) s3 = line.size();
        RelEntry e;
        for (char ch : line.substr(0, s1)) e.id = e.id * 10 + unsigned(ch - '0');
        e.name = line.substr(s1 + 1, s2 - s1 - 1);
        e.sha1 = line.substr(s2 + 1, s3 - s2 - 1);
        out.push_back(e);
    }
    return out;
}

std::string_view hooks() { return view(kHooks, kHooksSize); }
std::string_view ctxHeader() { return view(kCtxHeader, kCtxHeaderSize); }
std::string_view moduleAbiHeader() { return view(kModuleAbiHeader, kModuleAbiHeaderSize); }
std::string_view moduleRt() { return view(kModuleRt, kModuleRtSize); }
std::string_view recompilerSource() { return view(kRecompilerSource, kRecompilerSourceSize); }

}  // namespace setup::embedded
