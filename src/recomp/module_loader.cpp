// Connects the runtime to the recompiled game: either the tables linked into this
// executable (cfrecomp, CF_STATIC_MODULE) or a game module DLL built by setup
// (CityFolkRecomp.exe, docs/SETUP.md).
#include "recomp/module_loader.h"

#include "port/install.h"
#include "recomp/hle.h"
#include "recomp/module_abi.h"
#include "recomp/runtime.h"

#include <cmath>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace recomp {
namespace {

RuntimeApi makeApi() {
    RuntimeApi api{};
    api.abiVersion = kModuleAbiVersion;
    api.size = sizeof(RuntimeApi);
    api.mem = g_mem;
    api.irqPending = g_irqPending;
    api.traceOn = g_traceOn;
    api.watchOn = g_watchOn;
    api.patches = g_patches;
    api.relBase = g_relBase;
    api.watchStore = &watch_store;
    api.mmioRead = &mmio_rd;
    api.mmioWrite = &mmio_wr;
    api.serviceIrq = &service_irq;
    api.traceEnter = &trace_enter;
    api.fatalMessage = [](const char* msg) { fatal("%s", msg); };
    api.hleLookup = &hleLookup;
    api.hleMissing = &hleMissing;
    api.mfspr = [](Ctx& c, u32 spr) { return c.mfspr(spr); };
    api.mtspr = [](Ctx& c, u32 spr, u32 v) { c.mtspr(spr, v); };
    api.mftb = [](Ctx& c, u32 spr) { return c.mftb(spr); };
    api.callMissing = [](Ctx& c, u32 addr) { callMissing(c, addr); };
    api.callOther = [](Ctx& c, u32 addr) { callOther(c, addr); };
    api.relUnresolved = [](Ctx& c, u32 module) { rel_unresolved(c, module); };
    api.unimplemented = [](Ctx& c, u32 pc, u32 insn) { c.unimplemented(pc, insn); };
    api.fma = [](double a, double b, double c) { return std::fma(a, b, c); };
    api.trunc = [](double v) { return std::trunc(v); };
    api.nearbyint = [](double v) { return std::nearbyint(v); };
    api.sqrt = [](double v) { return std::sqrt(v); };
    return api;
}

}  // namespace

bool loadGameModule(const std::filesystem::path& path, std::string& error) {
    if (!g_mem) {
        error = "guest memory must be set up before loading the game module";
        return false;
    }
    static RuntimeApi api;  // the module keeps a pointer to it
    api = makeApi();
#ifdef _WIN32
    HMODULE h = LoadLibraryW(port::longPath(path).c_str());  // any length (docs/SETUP.md "Long paths")
    if (!h) {
        error = "could not load " + port::displayPath(path) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto init = reinterpret_cast<ModuleInitFn>(GetProcAddress(h, CF_MODULE_INIT_SYMBOL));
#else
    void* h = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        error = dlerror();
        return false;
    }
    auto init = reinterpret_cast<ModuleInitFn>(dlsym(h, CF_MODULE_INIT_SYMBOL));
#endif
    if (!init) {
        error = port::displayPath(path) + " is not a game module";
        return false;
    }
    const GameModule* m = init(&api);
    if (!m) {
        error = port::displayPath(path) + " was built for a different version of the game executable";
        return false;
    }
    g_module = m;
    return true;
}

void setGameModule(const GameModule* module) { g_module = module; }

}  // namespace recomp
