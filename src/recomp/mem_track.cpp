// Test aid CF_MEM_TRACK=1 (Windows, issue #34): records every C++ heap allocation of the runtime,
// Aurora and the other static libraries (global operator new/delete; not malloc, not Dawn's DLL)
// by its call stack, and lists in each [mem] report the call sites whose live bytes grew most since
// tracking began. Off by default (one branch per new/delete). docs/RENDERING.md "Memory use over
// time".
#include "recomp/mem_report.h"

#include "recomp/hle.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>

namespace {

constexpr uint32_t kPtrBits = 23;  // 8M live allocations (128 MiB table)
constexpr uint64_t kPtrSlots = 1ull << kPtrBits;
constexpr uint32_t kSiteBits = 16;
constexpr uint32_t kSiteSlots = 1u << kSiteBits;
constexpr int kFrames = 10;

struct PtrSlot {
    void* ptr;
    uint32_t size;
    uint32_t site;
};
struct Site {
    uint32_t hash;
    uint32_t used;
    void* frames[kFrames];
    int64_t liveBytes, liveCount, baseBytes, baseCount;
};

// 0 = not decided, 1 = off, 2 = on. sHeapMode (CF_MEM_TRACK=2): every module's HeapAlloc/HeapFree
// imports are redirected instead (Dawn, the shader compiler, the graphics driver, the C runtime's
// malloc), and operator new is not recorded separately (it allocates through malloc).
volatile LONG sState = 0;
bool sHeapMode = false;
SRWLOCK sLock = SRWLOCK_INIT;
PtrSlot* sPtrs = nullptr;
Site* sSites = nullptr;
thread_local int tInside = 0;
bool sBaseTaken = false;
uint64_t sUsedSlots = 0;

bool enabled() {
    LONG s = sState;
    if (s == 0) {
        char v[8] = {};
        const bool on = GetEnvironmentVariableA("CF_MEM_TRACK", v, sizeof(v)) > 0 && (v[0] == '1' || v[0] == '2');
        sHeapMode = on && v[0] == '2';
        if (on) {
            sPtrs = static_cast<PtrSlot*>(VirtualAlloc(nullptr, kPtrSlots * sizeof(PtrSlot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            sSites = static_cast<Site*>(VirtualAlloc(nullptr, kSiteSlots * sizeof(Site), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
        s = on && sPtrs && sSites ? 2 : 1;
        InterlockedExchange(&sState, s);
    }
    return s == 2;
}

uint64_t ptrHash(void* p) {
    uint64_t x = reinterpret_cast<uintptr_t>(p);
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return x & (kPtrSlots - 1);
}

uint32_t siteOf(void* const* frames, int n, uint32_t hash) {
    uint32_t i = hash & (kSiteSlots - 1);
    for (uint32_t k = 0; k < kSiteSlots; k++, i = (i + 1) & (kSiteSlots - 1)) {
        Site& s = sSites[i];
        if (!s.used) {
            s.used = 1;
            s.hash = hash;
            std::memcpy(s.frames, frames, sizeof(void*) * n);
            return i;
        }
        if (s.hash == hash && std::memcmp(s.frames, frames, sizeof(void*) * n) == 0) return i;
    }
    return 0;  // full: lumped into slot 0
}

void record(void* p, size_t size) {
    if (!p || tInside) return;
    tInside = 1;
    void* frames[kFrames] = {};
    ULONG hash = 0;
    const int n = RtlCaptureStackBackTrace(2, kFrames, frames, &hash);
    AcquireSRWLockExclusive(&sLock);
    if (sUsedSlots < kPtrSlots / 8 * 7) {  // (full: stop recording rather than probe forever)
        const uint32_t site = siteOf(frames, n, hash);
        sSites[site].liveBytes += int64_t(size);
        sSites[site].liveCount++;
        for (uint64_t i = ptrHash(p);; i = (i + 1) & (kPtrSlots - 1)) {
            if (sPtrs[i].ptr == p) {  // freed behind our back (an unhooked path): replace
                sSites[sPtrs[i].site].liveBytes -= sPtrs[i].size;
                sSites[sPtrs[i].site].liveCount--;
                sPtrs[i] = {p, uint32_t(std::min<size_t>(size, 0xFFFFFFFFu)), site};
                break;
            }
            if (!sPtrs[i].ptr) {
                sPtrs[i] = {p, uint32_t(std::min<size_t>(size, 0xFFFFFFFFu)), site};
                sUsedSlots++;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&sLock);
    tInside = 0;
}

void forget(void* p) {
    if (!p) return;
    AcquireSRWLockExclusive(&sLock);
    uint64_t i = ptrHash(p);
    for (; sPtrs[i].ptr; i = (i + 1) & (kPtrSlots - 1))
        if (sPtrs[i].ptr == p) break;
    if (sPtrs[i].ptr == p) {
        Site& s = sSites[sPtrs[i].site];
        s.liveBytes -= sPtrs[i].size;
        s.liveCount--;
        // Backward-shift deletion (linear probing without tombstones).
        uint64_t hole = i;
        for (uint64_t j = (i + 1) & (kPtrSlots - 1); sPtrs[j].ptr; j = (j + 1) & (kPtrSlots - 1)) {
            const uint64_t home = ptrHash(sPtrs[j].ptr);
            const bool between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
            if (!between) {
                sPtrs[hole] = sPtrs[j];
                hole = j;
            }
        }
        sPtrs[hole] = {};
        sUsedSlots--;
    }
    ReleaseSRWLockExclusive(&sLock);
}

void* allocate(size_t size) {
    void* p = std::malloc(size ? size : 1);
    if (sState != 1 && enabled() && !sHeapMode) record(p, size);
    return p;
}
void release(void* p) {
    if (sState == 2 && !sHeapMode) forget(p);
    std::free(p);
}
void* allocateAligned(size_t size, size_t align) {
    void* p = _aligned_malloc(size ? size : 1, align);
    if (sState != 1 && enabled() && !sHeapMode) record(p, size);
    return p;
}
void releaseAligned(void* p) {
    if (sState == 2 && !sHeapMode) forget(p);
    _aligned_free(p);
}

// --- CF_MEM_TRACK=2: heap import hooks ---
using RtlAllocateHeapFn = void*(NTAPI*)(void*, ULONG, SIZE_T);
using RtlFreeHeapFn = BOOLEAN(NTAPI*)(void*, ULONG, void*);
using RtlReAllocateHeapFn = void*(NTAPI*)(void*, ULONG, void*, SIZE_T);
RtlAllocateHeapFn sRealAlloc = nullptr;
RtlFreeHeapFn sRealFree = nullptr;
RtlReAllocateHeapFn sRealReAlloc = nullptr;

void* NTAPI hookAlloc(void* heap, ULONG flags, SIZE_T size) {
    void* p = sRealAlloc(heap, flags, size);
    record(p, size);
    return p;
}
BOOLEAN NTAPI hookFree(void* heap, ULONG flags, void* p) {
    if (p && !tInside) forget(p);
    return sRealFree(heap, flags, p);
}
void* NTAPI hookReAlloc(void* heap, ULONG flags, void* old, SIZE_T size) {
    void* p = sRealReAlloc(heap, flags, old, size);
    if (p) {
        if (old && !tInside) forget(old);
        record(p, size);
    }
    return p;
}

// Redirects the module's import address table entries that point at the heap functions.
void hookModule(HMODULE mod, const void* const* targets, void* const* hooks, int n) {
    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    if (!dir.VirtualAddress || !dir.Size) return;
    auto* iat = reinterpret_cast<void**>(base + dir.VirtualAddress);
    const size_t count = dir.Size / sizeof(void*);
    for (size_t i = 0; i < count; i++)
        for (int k = 0; k < n; k++)
            if (iat[i] == targets[k]) {
                DWORD old = 0;
                if (VirtualProtect(&iat[i], sizeof(void*), PAGE_READWRITE, &old)) {
                    iat[i] = hooks[k];
                    VirtualProtect(&iat[i], sizeof(void*), old, &old);
                }
            }
}

void hookAllModules() {
    if (!sHeapMode || sState != 2) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!sRealAlloc) {
        sRealAlloc = reinterpret_cast<RtlAllocateHeapFn>(GetProcAddress(ntdll, "RtlAllocateHeap"));
        sRealFree = reinterpret_cast<RtlFreeHeapFn>(GetProcAddress(ntdll, "RtlFreeHeap"));
        sRealReAlloc = reinterpret_cast<RtlReAllocateHeapFn>(GetProcAddress(ntdll, "RtlReAllocateHeap"));
    }
    if (!sRealAlloc || !sRealFree || !sRealReAlloc) return;
    // Kernel32/KernelBase HeapAlloc forward to ntdll; some modules import the KernelBase stubs.
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll"), k32 = GetModuleHandleW(L"kernel32.dll");
    const void* targets[9] = {reinterpret_cast<void*>(sRealAlloc), reinterpret_cast<void*>(sRealFree),
                              reinterpret_cast<void*>(sRealReAlloc)};
    void* hooks[9] = {reinterpret_cast<void*>(&hookAlloc), reinterpret_cast<void*>(&hookFree),
                      reinterpret_cast<void*>(&hookReAlloc)};
    int n = 3;
    for (HMODULE m : {kb, k32}) {
        if (!m) continue;
        targets[n] = reinterpret_cast<void*>(GetProcAddress(m, "HeapAlloc")), hooks[n++] = reinterpret_cast<void*>(&hookAlloc);
        targets[n] = reinterpret_cast<void*>(GetProcAddress(m, "HeapFree")), hooks[n++] = reinterpret_cast<void*>(&hookFree);
        targets[n] = reinterpret_cast<void*>(GetProcAddress(m, "HeapReAlloc")), hooks[n++] = reinterpret_cast<void*>(&hookReAlloc);
    }
    HMODULE mods[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); i++)
        if (mods[i] != ntdll && mods[i] != kb && mods[i] != k32) hookModule(mods[i], targets, hooks, n);
}

}  // namespace

void* operator new(size_t n) {
    if (void* p = allocate(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) {
    if (void* p = allocate(n)) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void* operator new(size_t n, std::align_val_t a) {
    if (void* p = allocateAligned(n, size_t(a))) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n, std::align_val_t a) {
    if (void* p = allocateAligned(n, size_t(a))) return p;
    throw std::bad_alloc();
}
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocateAligned(n, size_t(a)); }
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocateAligned(n, size_t(a)); }
void operator delete(void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }

namespace recomp::mem {

void trackInit() {
    if (!enabled()) return;
    tInside = 1;
    hookAllModules();
    tInside = 0;
    log("[mem] CF_MEM_TRACK=%d: recording %s", sHeapMode ? 2 : 1,
        sHeapMode ? "every module's heap allocations (HeapAlloc imports)" : "C++ allocations (operator new)");
}

void trackReport() {
    if (sState != 2) return;
    struct Row {
        uint32_t site;
        int64_t grew, live, count;
    };
    static Row* rows = static_cast<Row*>(VirtualAlloc(nullptr, kSiteSlots * sizeof(Row), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    uint32_t n = 0;
    int64_t total = 0, totalGrew = 0;
    tInside = 1;
    hookAllModules();  // modules loaded since (heap mode)
    // Heap mode: live bytes per module of the first frame outside the allocators.
    constexpr int kMods = 48;
    struct ModRow {
        HMODULE mod;
        int64_t live, grew;
    };
    static ModRow mods[kMods];
    int nMods = 0;
    static HMODULE sSystem[8] = {};
    if (sHeapMode && !sSystem[0]) {
        const wchar_t* names[] = {L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll", L"ucrtbase.dll", L"vcruntime140.dll",
                                  L"msvcp140.dll", L"vcruntime140_1.dll"};
        for (int k = 0; k < 7; k++) sSystem[k] = GetModuleHandleW(names[k]);
    }
    const auto moduleOf = [](void* pc) {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(pc), &m);
        return m;
    };
    AcquireSRWLockExclusive(&sLock);
    for (uint32_t i = 0; i < kSiteSlots; i++) {
        Site& s = sSites[i];
        if (!s.used) continue;
        if (!sBaseTaken) s.baseBytes = s.liveBytes, s.baseCount = s.liveCount;
        total += s.liveBytes;
        totalGrew += s.liveBytes - s.baseBytes;
        if (s.liveBytes - s.baseBytes > 0) rows[n++] = {i, s.liveBytes - s.baseBytes, s.liveBytes, s.liveCount - s.baseCount};
        if (sHeapMode && s.liveBytes > 0) {
            HMODULE owner = nullptr;
            for (int f = 0; f < kFrames && s.frames[f]; f++) {
                HMODULE m = moduleOf(s.frames[f]);
                if (std::find(std::begin(sSystem), std::end(sSystem), m) == std::end(sSystem)) {
                    owner = m;
                    break;
                }
            }
            int k = 0;
            while (k < nMods && mods[k].mod != owner) k++;
            if (k == nMods && nMods < kMods) mods[nMods++] = {owner, 0, 0};
            if (k < nMods) mods[k].live += s.liveBytes, mods[k].grew += s.liveBytes - s.baseBytes;
        }
    }
    const bool first = !sBaseTaken;
    sBaseTaken = true;
    std::sort(rows, rows + n, [](const Row& a, const Row& b) { return a.grew > b.grew; });
    n = std::min<uint32_t>(n, 12);
    Site top[12];
    for (uint32_t k = 0; k < n; k++) top[k] = sSites[rows[k].site];
    ReleaseSRWLockExclusive(&sLock);
    log("[mem] tracked %s: %.1f MB live, %+.1f MB since the first report%s", sHeapMode ? "heap allocations" : "C++ heap",
        total / 1048576.0, totalGrew / 1048576.0, first ? " (baseline taken now)" : "");
    if (nMods) {
        std::sort(mods, mods + nMods, [](const ModRow& a, const ModRow& b) { return a.live > b.live; });
        char line[1024];
        int len = std::snprintf(line, sizeof(line), "[mem]   by module (live, since the first report):");
        for (int k = 0; k < nMods && k < 10 && len < int(sizeof(line)) - 100; k++) {
            char path[MAX_PATH] = "?";
            if (mods[k].mod) GetModuleFileNameA(mods[k].mod, path, MAX_PATH);
            const char* base = path;
            for (const char* q = path; *q; q++)
                if (*q == '\\' || *q == '/') base = q + 1;
            len += std::snprintf(line + len, sizeof(line) - len, " %s %.0f MB (%+.0f)", base, mods[k].live / 1048576.0,
                                 mods[k].grew / 1048576.0);
        }
        log("%s", line);
    }
    if (first) {
        tInside = 0;
        return;
    }
    static bool sSym = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    for (uint32_t k = 0; k < n; k++) {
        char line[1600];
        int len = std::snprintf(line, sizeof(line), "[mem]   %+.2f MB (%+lld allocations, %.2f MB live):", rows[k].grew / 1048576.0,
                                (long long)rows[k].count, rows[k].live / 1048576.0);
        int shown = 0;
        for (int f = 0; f < kFrames && top[k].frames[f] && shown < 6 && len < int(sizeof(line)) - 200; f++) {
            const DWORD64 pc = reinterpret_cast<DWORD64>(top[k].frames[f]);
            const char* name = "?";
            auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            char modName[300] = "";
            HMODULE m = moduleOf(top[k].frames[f]);
            if (sHeapMode && std::find(std::begin(sSystem), std::end(sSystem), m) != std::end(sSystem)) continue;
            if (sSym && SymFromAddr(GetCurrentProcess(), pc, &disp, si)) {
                name = si->Name;
            } else if (m) {
                char path[MAX_PATH] = "";
                GetModuleFileNameA(m, path, MAX_PATH);
                const char* base = path;
                for (const char* q = path; *q; q++)
                    if (*q == '\\' || *q == '/') base = q + 1;
                std::snprintf(modName, sizeof(modName), "%s+0x%llx", base,
                              (unsigned long long)(pc - reinterpret_cast<DWORD64>(m)));
                name = modName;
            }
            // Skip the allocator plumbing.
            if (std::strstr(name, "operator new") || std::strstr(name, "allocate") || std::strstr(name, "_Allocate") ||
                std::strstr(name, "std::_") || std::strstr(name, "make_shared") || std::strstr(name, "absl::container_internal"))
                continue;
            IMAGEHLP_LINE64 li{};
            li.SizeOfStruct = sizeof(li);
            DWORD ldisp = 0;
            if (sSym && SymGetLineFromAddr64(GetCurrentProcess(), pc, &ldisp, &li)) {
                const char* file = li.FileName;
                for (const char* q = li.FileName; *q; q++)
                    if (*q == '\\' || *q == '/') file = q + 1;
                len += std::snprintf(line + len, sizeof(line) - len, " %s (%s:%lu) <", name, file, li.LineNumber);
            } else {
                len += std::snprintf(line + len, sizeof(line) - len, " %s <", name);
            }
            shown++;
        }
        log("%s", line);
    }
    tInside = 0;
}

}  // namespace recomp::mem
#else
namespace recomp::mem {
void trackInit() {}
void trackReport() {}
}  // namespace recomp::mem
#endif
