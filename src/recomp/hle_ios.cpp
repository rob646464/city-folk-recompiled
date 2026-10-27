// HLE: IPC to IOS (the Wii's I/O processor; docs/WII.md "IOS", docs/NAND.md). The SDK's IPC
// client (ipc/ipcclt.c: IOS_Open/Close/Read/Write/Seek/Ioctl/Ioctlv and their async forms) is
// replaced here; the libraries above it (ISFS/NAND, SC, ES, STM, VF, RFL, NWC24, KBD, ...) run as
// recompiled game code and talk to host devices (ios.h):
//   - NAND files and /dev/fs: ios_fs.cpp (a host folder mirroring the Wii's NAND);
//   - /dev/es, /dev/stm/*, /dev/di, /dev/sdio/*, /dev/usb/*, /dev/net/*: ios_devices.cpp.
// Requests are performed when the game issues them (so their order is the program's), and the
// reply comes after a modelled IOS latency: async callbacks as an interrupt, synchronous callers
// wait with the CPU given up (other guest threads and interrupts run meanwhile, as on the console
// where the thread blocks on the IPC reply). Requests to one resource manager are answered in
// order. CF_IOS_LOG=1: opens and unhandled requests; 2: every request and its result.
// CF_IOS_LATENCY=<scale> (default 1; 0 = replies at once).
// Written from the public IOS interface documentation, not from another emulator's code.
#include "recomp/ios.h"

#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace recomp::ios {
namespace {

using Clock = std::chrono::steady_clock;

int logLevel() {
    static const int level = std::getenv("CF_IOS_LOG") ? std::atoi(std::getenv("CF_IOS_LOG")) : 0;
    return level;
}

constexpr s32 kMaxFds = 32;  // IOS's descriptor table size
std::mutex sM;
std::unique_ptr<Handle> sFds[kMaxFds];
Clock::time_point sBusyUntil[size_t(Queue::Count)];

// Synchronous callers waiting for a pending request (STM event hook issued synchronously).
std::mutex sWaitM;
std::condition_variable sWaitCv;
std::map<u64, s32> sAnswered;
u64 sNextToken = 1;

const char* queueName(Queue q) {
    switch (q) {
    case Queue::Fs: return "fs";
    case Queue::Es: return "es";
    case Queue::Stm: return "stm";
    default: return "other";
    }
}

}  // namespace

double latencyScale() {
    static const double scale = [] {
        const char* v = std::getenv("CF_IOS_LATENCY");
        return v ? std::max(0.0, std::atof(v)) : 1.0;
    }();
    return scale;
}

bool logEnabled(int level) { return logLevel() >= level; }

void logOnce(const std::string& what) {
    static std::set<std::string> seen;
    static std::mutex m;
    std::lock_guard lk(m);
    if (logLevel() > 0 || seen.insert(what).second) recomp::log("[ios] %s", what.c_str());
}

s32 Handle::ioctl(u32 cmd, u32, u32, u32, u32, const Reply&) {
    logOnce(path + " ioctl " + std::to_string(cmd) + " not handled (invalid)");
    return IPC_EINVAL;
}
s32 Handle::ioctlv(u32 cmd, u32 nin, u32 nout, const Vec*, const Reply&) {
    logOnce(path + " ioctlv " + std::to_string(cmd) + " (" + std::to_string(nin) + " in, " + std::to_string(nout) +
            " out) not handled (invalid)");
    return IPC_EINVAL;
}

void answer(const Reply& reply, s32 result) {
    if (reply.cb) {
        const u32 cb = reply.cb, arg = reply.arg;
        sched::postIrq([cb, arg, result](Ctx& ic) { guestCall(ic, cb, u32(result), arg); });
        return;
    }
    {
        std::lock_guard lk(sWaitM);
        sAnswered[reply.token] = result;
    }
    sWaitCv.notify_all();
}

namespace {

// When a request issued now on queue `q` taking `cost` seconds is answered.
Clock::time_point schedule(Queue q, double cost) {
    static std::mutex m;
    std::lock_guard lk(m);
    const auto now = Clock::now();
    auto& busy = sBusyUntil[size_t(q)];
    const auto start = std::max(now, busy);
    busy = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(cost * latencyScale()));
    return busy;
}

// Delivers a result: async = callback interrupt at `at`; sync = the caller waits until `at`.
// Returns what the IOS_* function itself returns.
s32 deliver(s32 result, Clock::time_point at, const Reply& reply, bool async) {
    if (async) {
        if (result != kPending) {
            const double delay = std::max(0.0, std::chrono::duration<double>(at - Clock::now()).count());
            const u32 cb = reply.cb, arg = reply.arg;
            if (cb) sched::postIrqAfter(delay, [cb, arg, result](Ctx& ic) { guestCall(ic, cb, u32(result), arg); });
        }
        return IPC_OK;
    }
    if (result == kPending) {
        // A synchronous request the device answers later: block until it does.
        s32 r = 0;
        sched::withCpuReleased([&] {
            std::unique_lock lk(sWaitM);
            sWaitCv.wait(lk, [&] { return sAnswered.count(reply.token) != 0; });
            r = sAnswered[reply.token];
            sAnswered.erase(reply.token);
        });
        return r;
    }
    if (at > Clock::now() && !sched::inIrq()) sched::withCpuReleased([&] { std::this_thread::sleep_until(at); });
    return result;
}

Reply makeReply(u32 cb, u32 arg) {
    Reply r;
    r.cb = cb;
    r.arg = arg;
    std::lock_guard lk(sWaitM);
    r.token = sNextToken++;
    return r;
}

void logResult(const char* op, s32 fd, const std::string& path, s32 result, const char* extra = "") {
    if (logLevel() >= 2) recomp::log("[ios] %s fd %d %s%s -> %d", op, fd, path.c_str(), extra, result);
}

s32 doOpen(u32 pathAddr, u32 mode, u32 cb, u32 arg, bool async) {
    const std::string path = gstr(pathAddr);
    s32 err = IPC_OK;
    double cost = 0.0002;
    std::unique_ptr<Handle> h = (path.rfind("/dev/", 0) == 0 && path != "/dev/fs") ? openDevice(path, mode, err, cost)
                                                                                    : openFs(path, mode, err, cost);
    s32 result = err;
    Queue q = path.rfind("/dev/", 0) == 0 && path != "/dev/fs" ? Queue::Other : Queue::Fs;
    if (h) {
        q = h->queue();
        h->path = path;
        std::lock_guard lk(sM);
        result = IPC_EMAX;
        for (s32 fd = 0; fd < kMaxFds; fd++)
            if (!sFds[fd]) {
                sFds[fd] = std::move(h);
                result = fd;
                break;
            }
    }
    if (result >= 0) logOnce("open " + path + " (mode " + std::to_string(mode) + ") -> fd " + std::to_string(result));
    else if (logLevel() >= 2) recomp::log("[ios] open %s (mode %u) -> %d", path.c_str(), mode, result);
    const auto at = schedule(q, cost);
    return deliver(result, at, makeReply(cb, arg), async);
}

// Runs `op` on fd's handle with the table locked; unknown fd = IPC_EINVAL.
template <class F>
s32 onFd(s32 fd, u32 cb, u32 arg, bool async, const char* opName, F&& op) {
    const Reply reply = makeReply(cb, arg);
    s32 result;
    double cost = 0.0001;
    Queue q = Queue::Other;
    std::string path;
    {
        std::lock_guard lk(sM);
        Handle* h = fd >= 0 && fd < kMaxFds ? sFds[fd].get() : nullptr;
        if (!h) {
            result = IPC_EINVAL;
        } else {
            result = op(*h, reply);
            cost = h->cost();
            q = h->queue();
            path = h->path;
        }
    }
    logResult(opName, fd, path, result);
    const auto at = schedule(q, cost);
    return deliver(result, at, reply, async);
}

s32 doClose(s32 fd, u32 cb, u32 arg, bool async) {
    const Reply reply = makeReply(cb, arg);
    s32 result = IPC_EINVAL;
    double cost = 0.0001;
    Queue q = Queue::Other;
    std::string path;
    {
        std::lock_guard lk(sM);
        if (fd >= 0 && fd < kMaxFds && sFds[fd]) {
            result = sFds[fd]->close();
            cost = sFds[fd]->cost();
            q = sFds[fd]->queue();
            path = sFds[fd]->path;
            sFds[fd].reset();
        }
    }
    logResult("close", fd, path, result);
    const auto at = schedule(q, cost);
    return deliver(result, at, reply, async);
}

std::vector<Vec> vecs(u32 vec, u32 n) {
    std::vector<Vec> v(n);
    for (u32 i = 0; i < n; i++) v[i] = {rd32(vec + 8 * i), rd32(vec + 8 * i + 4)};
    return v;
}

}  // namespace
}  // namespace recomp::ios

namespace recomp {
using namespace ios;

// The IPC buffer (ipcMain.c): the MEM2 range IOS request structures and the libraries' IOS heaps
// (iosCreateHeap) are carved from; set by OSInit (low memory 0x80003130/34).
namespace {
u32 sIpcLo = 0, sIpcHi = 0;
void ipcBufferInit() {
    if (!sIpcHi) {
        sIpcLo = rd32(0x80003130);
        sIpcHi = rd32(0x80003134);
    }
}
}  // namespace
HLE(IPCInit) { ipcBufferInit(); }
HLE(IPCGetBufferHi) {
    ipcBufferInit();
    c.r[3] = sIpcHi;
}
HLE(IPCGetBufferLo) {
    ipcBufferInit();
    c.r[3] = sIpcLo;
}
HLE(IPCSetBufferLo) {
    ipcBufferInit();
    sIpcLo = c.r[3];
}
HLE(IPCCltInit) { c.r[3] = 0; }
HLE(IPCCltReInit) { c.r[3] = 0; }
HLE(IPCReInit) {}
HLE(IPCReadReg) { c.r[3] = 0; }
HLE(IPCWriteReg) {}
HLE(IPCInterruptHandler) {}

HLE(IOS_Open) { c.r[3] = u32(doOpen(c.r[3], c.r[4], 0, 0, false)); }
HLE(IOS_OpenAsync) { c.r[3] = u32(doOpen(c.r[3], c.r[4], c.r[5], c.r[6], true)); }
HLE(IOS_Close) { c.r[3] = u32(doClose(s32(c.r[3]), 0, 0, false)); }
HLE(IOS_CloseAsync) { c.r[3] = u32(doClose(s32(c.r[3]), c.r[4], c.r[5], true)); }

HLE(IOS_Read) {
    const u32 buf = c.r[4], len = c.r[5];
    c.r[3] = u32(onFd(s32(c.r[3]), 0, 0, false, "read", [&](Handle& h, const Reply&) { return h.read(buf, len); }));
}
HLE(IOS_ReadAsync) {
    const u32 buf = c.r[4], len = c.r[5];
    c.r[3] = u32(onFd(s32(c.r[3]), c.r[6], c.r[7], true, "read", [&](Handle& h, const Reply&) { return h.read(buf, len); }));
}
HLE(IOS_Write) {
    const u32 buf = c.r[4], len = c.r[5];
    c.r[3] = u32(onFd(s32(c.r[3]), 0, 0, false, "write", [&](Handle& h, const Reply&) { return h.write(buf, len); }));
}
HLE(IOS_WriteAsync) {
    const u32 buf = c.r[4], len = c.r[5];
    c.r[3] = u32(onFd(s32(c.r[3]), c.r[6], c.r[7], true, "write", [&](Handle& h, const Reply&) { return h.write(buf, len); }));
}
HLE(IOS_Seek) {
    const s32 off = s32(c.r[4]), wh = s32(c.r[5]);
    c.r[3] = u32(onFd(s32(c.r[3]), 0, 0, false, "seek", [&](Handle& h, const Reply&) { return h.seek(off, wh); }));
}
HLE(IOS_SeekAsync) {
    const s32 off = s32(c.r[4]), wh = s32(c.r[5]);
    c.r[3] = u32(onFd(s32(c.r[3]), c.r[6], c.r[7], true, "seek", [&](Handle& h, const Reply&) { return h.seek(off, wh); }));
}
HLE(IOS_Ioctl) {
    const u32 cmd = c.r[4], in = c.r[5], inLen = c.r[6], out = c.r[7], outLen = c.r[8];
    c.r[3] = u32(onFd(s32(c.r[3]), 0, 0, false, "ioctl",
                      [&](Handle& h, const Reply& r) { return h.ioctl(cmd, in, inLen, out, outLen, r); }));
}
HLE(IOS_IoctlAsync) {
    const u32 cmd = c.r[4], in = c.r[5], inLen = c.r[6], out = c.r[7], outLen = c.r[8];
    c.r[3] = u32(onFd(s32(c.r[3]), c.r[9], c.r[10], true, "ioctl",
                      [&](Handle& h, const Reply& r) { return h.ioctl(cmd, in, inLen, out, outLen, r); }));
}
HLE(IOS_Ioctlv) {
    const u32 cmd = c.r[4], nin = c.r[5], nout = c.r[6];
    const std::vector<Vec> v = vecs(c.r[7], nin + nout);
    c.r[3] = u32(onFd(s32(c.r[3]), 0, 0, false, "ioctlv",
                      [&](Handle& h, const Reply& r) { return h.ioctlv(cmd, nin, nout, v.data(), r); }));
}
HLE(IOS_IoctlvAsync) {
    const u32 cmd = c.r[4], nin = c.r[5], nout = c.r[6];
    const std::vector<Vec> v = vecs(c.r[7], nin + nout);
    c.r[3] = u32(onFd(s32(c.r[3]), c.r[8], c.r[9], true, "ioctlv",
                      [&](Handle& h, const Reply& r) { return h.ioctlv(cmd, nin, nout, v.data(), r); }));
}
// IOS_IoctlvReboot: an ioctlv after which IOS reboots (ES_LaunchTitle). The ES device ends the
// program for title launches (ios_devices.cpp).
HLE(IOS_IoctlvReboot) {
    const u32 cmd = c.r[4], nin = c.r[5], nout = c.r[6];
    const std::vector<Vec> v = vecs(c.r[7], nin + nout);
    c.r[3] = u32(onFd(s32(c.r[3]), 0, 0, false, "ioctlv (reboot)",
                      [&](Handle& h, const Reply& r) { return h.ioctlv(cmd, nin, nout, v.data(), r); }));
}
HLE(IPCiProfInit) {}
HLE(IPCiProfQueueReq) {}
HLE(IPCiProfAck) {}
HLE(IPCiProfReply) {}

}  // namespace recomp
