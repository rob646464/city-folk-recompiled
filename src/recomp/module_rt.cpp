// Game module support code: compiled together with the generated sources into the game
// module DLL at setup (docs/SETUP.md). Provides what ctx.h declares but does not inline,
// by forwarding to the runtime's RuntimeApi, plus the few C library symbols the compiler
// may reference. FREESTANDING: no system headers.
#include "recomp/module_abi.h"

#ifdef _WIN32
#define CF_MODULE_EXPORT __declspec(dllexport)
#else
// Linux (.so): the module is compiled with -fvisibility=hidden and linked -Bsymbolic, so
// only this entry point is exported and nothing in the module can be interposed by a
// same-named symbol of the executable or a library (g_mem, memcpy, ...).
#define CF_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef _WIN32
// Stack probe for frames larger than a page (Windows x64: frame size in rax; only flags may
// change). Windows grows thread stacks through a guard page, so a function whose frame is
// bigger than 4 KiB must touch each page in order or it faults past the guard page. The
// compiler calls this (stack probes are on in module_builder.cpp); same algorithm as the
// CRT's __chkstk, which the freestanding module cannot link.
extern "C" __attribute__((naked)) void __chkstk() {
    asm volatile(
        "pushq %rcx\n\t"
        "pushq %rax\n\t"
        "leaq 24(%rsp), %rcx\n\t"
        "cmpq $0x1000, %rax\n\t"
        "jb 2f\n\t"
        "1:\n\t"
        "subq $0x1000, %rcx\n\t"
        "orq $0, (%rcx)\n\t"
        "subq $0x1000, %rax\n\t"
        "cmpq $0x1000, %rax\n\t"
        "ja 1b\n\t"
        "2:\n\t"
        "subq %rax, %rcx\n\t"
        "orq $0, (%rcx)\n\t"
        "popq %rax\n\t"
        "popq %rcx\n\t"
        "retq");
}
#endif  // _WIN32 (Linux thread stacks are fully mapped; the compiler emits no probes there)

namespace recomp {

extern const GameModule kGameModule;  // func_table.cpp (generated)

namespace {
const RuntimeApi* sApi;
}  // namespace

u8* g_mem;
bool* g_irqPending;
bool g_traceOn;
bool g_watchOn;
u8* g_patches;
u32* g_relBase;

void watch_store(u32 a, u32 v, int size) { sApi->watchStore(a, v, size); }
u32 mmio_rd(u32 a, int size) { return sApi->mmioRead(a, size); }
void mmio_wr(u32 a, u32 v, int size) { sApi->mmioWrite(a, v, size); }
void service_irq(Ctx& c) { sApi->serviceIrq(c); }
void trace_enter(u32 addr, Ctx& c) { sApi->traceEnter(addr, c); }
void fatal(const char* fmt, ...) {
    sApi->fatalMessage(fmt);
    __builtin_unreachable();
}
HleFn hleLookup(const char* name) { return sApi->hleLookup(name); }
void hleMissing(const char* name, u32 addr, Ctx& c) { sApi->hleMissing(name, addr, c); }
void rel_unresolved(Ctx& c, u32 module) { sApi->relUnresolved(c, module); }

u32 Ctx::mfspr(u32 spr) { return sApi->mfspr(*this, spr); }
void Ctx::mtspr(u32 spr, u32 v) { sApi->mtspr(*this, spr, v); }
u32 Ctx::mftb(u32 spr) { return sApi->mftb(*this, spr); }
void Ctx::unimplemented(u32 pc, u32 insn) {
    sApi->unimplemented(*this, pc, insn);
    __builtin_unreachable();
}

// Indirect calls (bctrl/blrl/virtual calls): binary search of the module's own table.
void Ctx::call(u32 addr) {
    const FuncEntry* lo = kGameModule.funcs;
    u32 n = kGameModule.funcCount;
    while (n > 0) {
        const u32 half = n / 2;
        if (lo[half].addr < addr) {
            lo += half + 1;
            n -= half + 1;
        } else {
            n = half;
        }
    }
    if (lo != kGameModule.funcs + kGameModule.funcCount && lo->addr == addr) [[likely]] {
        lo->fn(*this);
        return;
    }
    sApi->callOther(*this, addr);  // a linked REL module's function (rel.cpp), or a missing one
}

}  // namespace recomp

using namespace recomp;

extern "C" {

#ifdef _WIN32
// Referenced by MSVC-ABI object files that use floating point.
int _fltused = 0;
#endif

// The compiler may lower struct copies, __builtin_mem* and math builtins to calls.
void* memcpy(void* dst, const void* src, decltype(sizeof(0)) n) {
    auto* d = static_cast<volatile u8*>(dst);
    auto* s = static_cast<const u8*>(src);
    while (n--) *d++ = *s++;
    return dst;
}
void* memmove(void* dst, const void* src, decltype(sizeof(0)) n) {
    auto* d = static_cast<volatile u8*>(dst);
    auto* s = static_cast<const u8*>(src);
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dst;
}
void* memset(void* dst, int v, decltype(sizeof(0)) n) {
    auto* d = static_cast<volatile u8*>(dst);
    while (n--) *d++ = u8(v);
    return dst;
}
int memcmp(const void* a, const void* b, decltype(sizeof(0)) n) {
    auto* x = static_cast<const u8*>(a);
    auto* y = static_cast<const u8*>(b);
    for (; n; n--, x++, y++)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
double fma(double a, double b, double c) { return sApi->fma(a, b, c); }
double trunc(double v) { return sApi->trunc(v); }
double nearbyint(double v) { return sApi->nearbyint(v); }
double sqrt(double v) { return sApi->sqrt(v); }

CF_MODULE_EXPORT const GameModule* cf_module_init(const RuntimeApi* api) {
    if (!api || api->abiVersion != kModuleAbiVersion || api->size != sizeof(RuntimeApi) ||
        kGameModule.abiVersion != kModuleAbiVersion)
        return nullptr;
    sApi = api;
    g_mem = api->mem;
    g_irqPending = api->irqPending;
    g_traceOn = api->traceOn;
    g_watchOn = api->watchOn;
    g_patches = api->patches;
    g_relBase = api->relBase;
    return &kGameModule;
}

#ifdef _WIN32
// No C runtime: the loader calls this directly (lld-link /entry).
int __stdcall lm_module_entry(void*, unsigned, void*) { return 1; }
#endif

}  // extern "C"
