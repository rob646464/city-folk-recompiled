// Inputs of first-run setup, embedded in the executable so they always match the runtime
// they were built with: the function maps (addresses and names only), the hook list, and the
// freestanding sources compiled into the game module. The arrays are generated at build time
// by tools/recomp/cfembed.cpp (CMakeLists.txt, CF_EMBEDDED_FILES).
#include "setup/embedded.h"

namespace setup::embedded {
namespace data {
#define CF_EMBEDDED(name)       \
    extern const char name[];   \
    extern const unsigned long long name##Size;
CF_EMBEDDED(kSymbolsUs)
CF_EMBEDDED(kSplitsUs)
CF_EMBEDDED(kRelMapsUs)
CF_EMBEDDED(kHooks)
CF_EMBEDDED(kCtxHeader)
CF_EMBEDDED(kModuleAbiHeader)
CF_EMBEDDED(kModuleRt)
CF_EMBEDDED(kRecompilerSource)
#undef CF_EMBEDDED
}  // namespace data

namespace {
std::string_view view(const char* p, unsigned long long n) { return {p, size_t(n)}; }
}  // namespace

using namespace data;

std::string_view symbols() { return view(kSymbolsUs, kSymbolsUsSize); }
std::string_view splits() { return view(kSplitsUs, kSplitsUsSize); }
std::vector<std::pair<std::string_view, std::string_view>> relMaps() {
    // cfembed's folder format: "\x01<stem>\n" before each file.
    std::vector<std::pair<std::string_view, std::string_view>> out;
    const std::string_view all = view(kRelMapsUs, kRelMapsUsSize);
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
std::string_view hooks() { return view(kHooks, kHooksSize); }
std::string_view ctxHeader() { return view(kCtxHeader, kCtxHeaderSize); }
std::string_view moduleAbiHeader() { return view(kModuleAbiHeader, kModuleAbiHeaderSize); }
std::string_view moduleRt() { return view(kModuleRt, kModuleRtSize); }
std::string_view recompilerSource() { return view(kRecompilerSource, kRecompilerSourceSize); }

}  // namespace setup::embedded
