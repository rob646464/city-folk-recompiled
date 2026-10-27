// Test aid CF_PROFILE=<roles>,<start s>,<seconds>[,<file>] (Windows x64): a sampling profiler for
// the threads that cpu_qos.cpp gives a role (host, render, fifo, guest, timer, audio; several joined
// with '+', e.g. "guest+host,200,30"). Every ~0.5 ms each thread of those roles is suspended, its stack
// unwound (RtlVirtualUnwind, no allocation while it is suspended) and resumed; at the end the samples
// are symbolized (dbghelp, cfrecomp.pdb) and the self and inclusive counts per function written to
// <file> (default lm_profile.txt in the working directory). Waiting threads are sampled too: their
// stacks end in the wait (NtWaitFor..., SleepConditionVariable...), so waits show up by caller.
#include "recomp/cpu_qos.h"
#include "recomp/hle.h"

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace recomp::cpu {
namespace {

struct Target {
    HANDLE h;
    int role;
};
std::mutex sM;
std::vector<Target> sTargets;
unsigned sRoleMask = 0;
double sStart = 0, sSecs = 0;
std::string sFile = "lm_profile.txt";

constexpr int kDepth = 48;
struct Sample {
    int role;
    int n;
    DWORD64 pc[kDepth];
};

const char* const kNames[] = {"host", "render", "fifo", "guest", "timer", "audio"};

void run() {
    std::this_thread::sleep_for(std::chrono::duration<double>(sStart));
    log("[profile] sampling for %.0f s", sSecs);
    std::vector<Sample> samples;
    samples.reserve(size_t(sSecs * 2000 * 4) + 16);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(sSecs);
    timeBeginPeriod(1);
    while (std::chrono::steady_clock::now() < end) {
        std::vector<Target> targets;
        {
            std::lock_guard lk(sM);
            targets = sTargets;
        }
        for (const Target& t : targets) {
            if (samples.size() == samples.capacity()) break;
            if (SuspendThread(t.h) == DWORD(-1)) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            Sample s{};
            s.role = t.role;
            if (GetThreadContext(t.h, &ctx)) {
                for (int i = 0; i < kDepth && ctx.Rip; i++) {
                    s.pc[s.n++] = ctx.Rip;
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                    if (!fn) {  // leaf function: return address on the stack
                        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                        ctx.Rsp += 8;
                        continue;
                    }
                    PVOID handler;
                    DWORD64 frame;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler, &frame, nullptr);
                }
            }
            ResumeThread(t.h);
            if (s.n) samples.push_back(s);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    timeEndPeriod(1);
    log("[profile] %zu samples; symbolizing", samples.size());
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    std::unordered_map<DWORD64, std::string> names;
    auto name = [&](DWORD64 pc) -> const std::string& {
        auto it = names.find(pc);
        if (it != names.end()) return it->second;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::string n;
        if (SymFromAddr(proc, pc, &disp, si)) n = si->Name;
        else {
            char b[32];
            std::snprintf(b, sizeof(b), "?%llx", (unsigned long long)pc);
            n = b;
        }
        IMAGEHLP_MODULE64 mi{};
        mi.SizeOfStruct = sizeof(mi);
        if (SymGetModuleInfo64(proc, pc, &mi) && std::strcmp(mi.ModuleName, "cfrecomp") != 0) n = std::string(mi.ModuleName) + "!" + n;
        return names[pc] = n;
    };
    FILE* f = std::fopen(sFile.c_str(), "w");
    if (!f) return;
    for (int role = 0; role < int(Role::Count); role++) {
        std::map<std::string, int> self, incl;
        int total = 0;
        for (const Sample& s : samples) {
            if (s.role != role) continue;
            // Guest threads that wait for the (one) guest CPU are not running game code.
            bool idle = false;
            for (int i = 0; i < s.n && !idle; i++) idle = name(s.pc[i]).find("waitForCpu") != std::string::npos;
            if (idle) continue;
            total++;
            self[name(s.pc[0])]++;
            std::vector<const std::string*> seen;
            for (int i = 0; i < s.n; i++) {
                const std::string& n = name(s.pc[i]);
                if (std::find(seen.begin(), seen.end(), &n) != seen.end()) continue;
                seen.push_back(&n);
                incl[n]++;
            }
        }
        if (!total) continue;
        std::fprintf(f, "== %s: %d samples\n-- self\n", kNames[role], total);
        std::vector<std::pair<int, std::string>> v;
        for (auto& [k, c] : self) v.push_back({c, k});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 60; i++) std::fprintf(f, "%6.2f%% %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
        std::fprintf(f, "-- inclusive\n");
        v.clear();
        for (auto& [k, c] : incl) v.push_back({c, k});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 150; i++) std::fprintf(f, "%6.2f%% %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
    }
    std::fclose(f);
    log("[profile] written to %s", sFile.c_str());
}

}  // namespace

// Called by setCurrent: registers the calling thread for CF_PROFILE.
void profileRegister(int role) {
    static const bool sOn = [] {
        const char* v = std::getenv("CF_PROFILE");
        if (!v || !*v) return false;
        char roles[64] = {}, file[512] = {};
        if (std::sscanf(v, "%63[^,],%lf,%lf,%511[^\n]", roles, &sStart, &sSecs, file) < 3) return false;
        if (file[0]) sFile = file;
        for (char* p = std::strtok(roles, "+"); p; p = std::strtok(nullptr, "+"))
            for (int r = 0; r < int(Role::Count); r++)
                if (!std::strncmp(p, kNames[r], std::strlen(kNames[r]))) sRoleMask |= 1u << r;
        std::thread(run).detach();
        return true;
    }();
    if (!sOn || !(sRoleMask & (1u << role))) return;
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    std::lock_guard lk(sM);
    sTargets.push_back({h, role});
}

}  // namespace recomp::cpu
#else
namespace recomp::cpu {
void profileRegister(int) {}
}  // namespace recomp::cpu
#endif
