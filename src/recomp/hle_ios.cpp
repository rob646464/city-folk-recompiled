// HLE: IPC to IOS (the Wii's I/O processor; docs/WII.md "IOS"). The SDK's IPC client
// (ipc/ipcclt.c: IOS_Open/Close/Read/Write/Seek/Ioctl/Ioctlv and their async forms) is replaced
// here; the libraries above it (ISFS/NAND, SC, ES, STM, WPAD, ...) run as recompiled game code
// and talk to these device stand-ins:
//   - NAND files (paths starting with '/'): a host folder (CF_NAND_DIR, default
//     <dataDir>/nand), opened read/write; /dev/fs answers the file-system ioctls;
//   - /dev/es: the running title's id and plausible defaults;
//   - /dev/stm/immediate, /dev/stm/eventhook: accepted, ioctls succeed (the event hook never
//     fires: no Power/Reset button events);
//   - /dev/di: accepted (disc reads go through the DVD HLE);
//   - everything else (Bluetooth, USB, network, SD): "no such device" (IPC_ENOENT).
// Every open, and every ioctl a device does not handle, is logged once (CF_IOS_LOG=1: all).
// Written from the public IOS interface documentation, not from another emulator's code.
#include "port/install.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace recomp {
namespace {

namespace fs = std::filesystem;

constexpr s32 IPC_OK = 0, IPC_EACCES = -1, IPC_EINVAL = -4, IPC_ENOENT = -6;
constexpr s32 FS_EINVAL = -101, FS_EACCES = -102, FS_EEXIST = -105, FS_ENOENT = -106;

enum class Dev { File, Fs, Es, Stm, Di, Other };
struct Fd {
    Dev dev = Dev::Other;
    std::string path;
    fs::path host;
    u32 pos = 0;
    u32 mode = 0;
};
std::mutex sM;
std::map<s32, Fd> sFds;
s32 sNextFd = 3;

bool logAll() {
    static const bool on = std::getenv("CF_IOS_LOG") != nullptr;
    return on;
}
void logOnce(const std::string& what) {
    static std::set<std::string> seen;
    static std::mutex m;
    std::lock_guard lk(m);
    if (logAll() || seen.insert(what).second) log("[ios] %s", what.c_str());
}

fs::path nandRoot() {
    static const fs::path root = [] {
        fs::path p = std::getenv("CF_NAND_DIR") ? port::envPath("CF_NAND_DIR") : port::dataDir() / "nand";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }();
    return root;
}
fs::path hostPath(const std::string& nand) {
    std::string rel = nand;
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    return nandRoot() / fs::path(std::u8string(rel.begin(), rel.end()));
}

u64 titleId() { return 0x0001000052555545ull | (port::install().running == port::GameVersion::PAL ? 0x0000000000000A00ull : 0); }

s32 openPath(const std::string& path, u32 mode) {
    Fd f;
    f.path = path;
    f.mode = mode;
    if (path == "/dev/fs") f.dev = Dev::Fs;
    else if (path == "/dev/es") f.dev = Dev::Es;
    else if (path.rfind("/dev/stm/", 0) == 0) f.dev = Dev::Stm;
    else if (path == "/dev/di") f.dev = Dev::Di;
    else if (path.rfind("/dev/", 0) == 0) {
        logOnce("open " + path + ": no such device");
        return IPC_ENOENT;
    } else {
        f.dev = Dev::File;
        f.host = hostPath(path);
        std::error_code ec;
        if (!fs::is_regular_file(f.host, ec)) {
            logOnce("open " + path + ": no such NAND file (" + f.host.string() + ")");
            return FS_ENOENT;
        }
    }
    std::lock_guard lk(sM);
    const s32 fd = sNextFd++;
    sFds[fd] = f;
    logOnce("open " + path + " -> fd " + std::to_string(fd));
    return fd;
}

Fd* fdOf(s32 fd) {
    std::lock_guard lk(sM);
    auto it = sFds.find(fd);
    return it == sFds.end() ? nullptr : &it->second;
}

s32 doClose(s32 fd) {
    std::lock_guard lk(sM);
    return sFds.erase(fd) ? IPC_OK : IPC_EINVAL;
}

s32 doRead(s32 fd, u32 buf, u32 len) {
    Fd* f = fdOf(fd);
    if (!f) return IPC_EINVAL;
    if (f->dev != Dev::File) return 0;
    std::ifstream in(f->host, std::ios::binary);
    in.seekg(f->pos);
    in.read(reinterpret_cast<char*>(hostp(buf)), len);
    const u32 n = u32(in.gcount());
    f->pos += n;
    return s32(n);
}

s32 doWrite(s32 fd, u32 buf, u32 len) {
    Fd* f = fdOf(fd);
    if (!f) return IPC_EINVAL;
    if (f->dev != Dev::File) return s32(len);
    std::fstream out(f->host, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) return FS_EACCES;
    out.seekp(f->pos);
    out.write(reinterpret_cast<const char*>(hostp(buf)), len);
    f->pos += len;
    return s32(len);
}

s32 doSeek(s32 fd, s32 off, s32 whence) {
    Fd* f = fdOf(fd);
    if (!f) return IPC_EINVAL;
    if (f->dev != Dev::File) return 0;
    std::error_code ec;
    const s64 size = s64(fs::file_size(f->host, ec));
    s64 p = whence == 0 ? off : whence == 1 ? s64(f->pos) + off : size + off;
    if (p < 0 || p > size) return FS_EINVAL;
    f->pos = u32(p);
    return s32(p);
}

std::string fsPath(u32 a) {
    std::string s;
    for (u32 i = 0; i < 64; i++) {
        const char ch = char(rd8(a + i));
        if (!ch) break;
        s += ch;
    }
    return s;
}

// /dev/fs ioctls (in = request block).
s32 fsIoctl(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
    std::error_code ec;
    switch (cmd) {
    case 0x03: {  // CreateDir (attributes block: owner, group, path at +6)
        const fs::path p = hostPath(fsPath(in + 6));
        if (fs::exists(p, ec)) return FS_EEXIST;
        fs::create_directories(p, ec);
        return ec ? FS_EACCES : IPC_OK;
    }
    case 0x09: {  // CreateFile
        const fs::path p = hostPath(fsPath(in + 6));
        if (fs::exists(p, ec)) return FS_EEXIST;
        if (!fs::is_directory(p.parent_path(), ec)) return FS_ENOENT;
        std::ofstream(p, std::ios::binary);
        return IPC_OK;
    }
    case 0x07: {  // Delete
        const fs::path p = hostPath(fsPath(in));
        if (!fs::exists(p, ec)) return FS_ENOENT;
        fs::remove_all(p, ec);
        return IPC_OK;
    }
    case 0x08: {  // Rename (old path, new path at +64)
        const fs::path a = hostPath(fsPath(in)), b = hostPath(fsPath(in + 64));
        if (!fs::exists(a, ec)) return FS_ENOENT;
        fs::rename(a, b, ec);
        return ec ? FS_EACCES : IPC_OK;
    }
    case 0x06: {  // GetAttr (path) -> owner id, group, path, permissions, attribute
        const fs::path p = hostPath(fsPath(in));
        if (!fs::exists(p, ec)) return FS_ENOENT;
        if (out && outLen >= 0x4C) {
            std::memset(hostp(out), 0, 0x4C);
            wr32(out, 0x1000);
            wr16(out + 4, 0x3031);
            std::strncpy(reinterpret_cast<char*>(hostp(out + 6)), fsPath(in).c_str(), 63);
            wr8(out + 0x46, 3);  // owner rw
            wr8(out + 0x47, 3);
            wr8(out + 0x48, 3);
        }
        return IPC_OK;
    }
    case 0x05: {  // SetAttr
        return fs::exists(hostPath(fsPath(in + 6)), ec) ? IPC_OK : FS_ENOENT;
    }
    case 0x02: return IPC_OK;  // GetStats: zeros
    case 0x01: return IPC_OK;  // Format
    default: break;
    }
    logOnce("/dev/fs ioctl " + std::to_string(cmd) + " not handled (returns 0)");
    (void)inLen;
    return IPC_OK;
}

// File fd ioctl 11: GetFileStats (length, position).
s32 fileIoctl(Fd& f, u32 cmd, u32 out) {
    if (cmd == 11 && out) {
        std::error_code ec;
        wr32(out, u32(fs::file_size(f.host, ec)));
        wr32(out + 4, f.pos);
        return IPC_OK;
    }
    logOnce("file ioctl " + std::to_string(cmd) + " on " + f.path + " not handled");
    return IPC_OK;
}

s32 doIoctl(s32 fd, u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen) {
    Fd* f = fdOf(fd);
    if (!f) return IPC_EINVAL;
    switch (f->dev) {
    case Dev::Fs: return fsIoctl(cmd, in, inLen, out, outLen);
    case Dev::File: return fileIoctl(*f, cmd, out);
    case Dev::Stm:
        if (cmd == 0x1000) return IPC_OK;  // event hook: never answered on the console either until an event
        logOnce("stm ioctl " + std::to_string(cmd) + " (ok)");
        return IPC_OK;
    default: break;
    }
    logOnce(f->path + " ioctl " + std::to_string(cmd) + " not handled (returns 0)");
    return IPC_OK;
}

// Ioctlv: vec = in-count + out-count {u32 addr, u32 length} pairs.
s32 doIoctlv(s32 fd, u32 cmd, u32 nin, u32 nout, u32 vec) {
    Fd* f = fdOf(fd);
    if (!f) return IPC_EINVAL;
    auto vaddr = [&](u32 i) { return rd32(vec + 8 * i); };
    auto vlen = [&](u32 i) { return rd32(vec + 8 * i + 4); };
    if (f->dev == Dev::Es) {
        switch (cmd) {
        case 0x20:  // GetTitleID -> u64
            if (nout >= 1 && vlen(nin) >= 8) {
                wr32(vaddr(nin), u32(titleId() >> 32));
                wr32(vaddr(nin) + 4, u32(titleId()));
            }
            return IPC_OK;
        case 0x1D:  // GetDataDir(title id) -> "/title/00010000/xxxxxxxx/data"
            if (nout >= 1 && vlen(nin) >= 30) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "/title/%08x/%08x/data", u32(titleId() >> 32), u32(titleId()));
                std::memcpy(hostp(vaddr(nin)), buf, std::strlen(buf) + 1);
            }
            return IPC_OK;
        default: break;
        }
    } else if (f->dev == Dev::Fs) {
        if (cmd == 0x04) {  // ReadDir(path[, max]) -> count [, names]
            std::error_code ec;
            const fs::path p = hostPath(fsPath(vaddr(0)));
            if (!fs::is_directory(p, ec)) return FS_ENOENT;
            std::vector<std::string> names;
            for (const auto& e : fs::directory_iterator(p, ec)) names.push_back(e.path().filename().string());
            if (nin == 1 && nout == 1) {
                wr32(vaddr(1), u32(names.size()));
            } else if (nin >= 2 && nout >= 2) {
                const u32 max = rd32(vaddr(1));
                u32 at = vaddr(2), n = 0;
                for (const auto& nm : names) {
                    if (n >= max) break;
                    std::memcpy(hostp(at), nm.c_str(), nm.size() + 1);
                    at += u32(nm.size() + 1);
                    n++;
                }
                wr32(vaddr(3), n);
            }
            return IPC_OK;
        }
        if (cmd == 0x0C) {  // GetUsage(path) -> blocks, inodes
            if (nout >= 2) {
                wr32(vaddr(nin), 0);
                wr32(vaddr(nin + 1), 0);
            }
            return IPC_OK;
        }
    }
    logOnce(f->path + " ioctlv " + std::to_string(cmd) + " (" + std::to_string(nin) + " in, " + std::to_string(nout) +
            " out) not handled (returns 0)");
    return IPC_OK;
}

// IOS replies arrive later on the console; async completions come as interrupts after a short
// delay (game code may rely on the callback not running inside the call).
void complete(s32 result, u32 cb, u32 arg) {
    if (!cb) return;
    sched::postIrqAfter(0.0002, [result, cb, arg](Ctx& ic) { guestCall(ic, cb, u32(result), arg); });
}

}  // namespace

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
HLE(IOS_Open) { c.r[3] = u32(openPath(gstr(c.r[3]), c.r[4])); }
HLE(IOS_OpenAsync) {
    const s32 r = openPath(gstr(c.r[3]), c.r[4]);
    complete(r, c.r[5], c.r[6]);
    c.r[3] = 0;
}
HLE(IOS_Close) { c.r[3] = u32(doClose(s32(c.r[3]))); }
HLE(IOS_CloseAsync) {
    complete(doClose(s32(c.r[3])), c.r[4], c.r[5]);
    c.r[3] = 0;
}
HLE(IOS_Read) { c.r[3] = u32(doRead(s32(c.r[3]), c.r[4], c.r[5])); }
HLE(IOS_ReadAsync) {
    complete(doRead(s32(c.r[3]), c.r[4], c.r[5]), c.r[6], c.r[7]);
    c.r[3] = 0;
}
HLE(IOS_Write) { c.r[3] = u32(doWrite(s32(c.r[3]), c.r[4], c.r[5])); }
HLE(IOS_WriteAsync) {
    complete(doWrite(s32(c.r[3]), c.r[4], c.r[5]), c.r[6], c.r[7]);
    c.r[3] = 0;
}
HLE(IOS_Seek) { c.r[3] = u32(doSeek(s32(c.r[3]), s32(c.r[4]), s32(c.r[5]))); }
HLE(IOS_SeekAsync) {
    complete(doSeek(s32(c.r[3]), s32(c.r[4]), s32(c.r[5])), c.r[6], c.r[7]);
    c.r[3] = 0;
}
HLE(IOS_Ioctl) { c.r[3] = u32(doIoctl(s32(c.r[3]), c.r[4], c.r[5], c.r[6], c.r[7], c.r[8])); }
HLE(IOS_IoctlAsync) {
    complete(doIoctl(s32(c.r[3]), c.r[4], c.r[5], c.r[6], c.r[7], c.r[8]), c.r[9], c.r[10]);
    c.r[3] = 0;
}
HLE(IOS_Ioctlv) { c.r[3] = u32(doIoctlv(s32(c.r[3]), c.r[4], c.r[5], c.r[6], c.r[7])); }
HLE(IOS_IoctlvAsync) {
    complete(doIoctlv(s32(c.r[3]), c.r[4], c.r[5], c.r[6], c.r[7]), c.r[8], c.r[9]);
    c.r[3] = 0;
}
HLE(IOS_IoctlvReboot) {
    log("[ios] IOS_IoctlvReboot (fd %d cmd %u): ignored", s32(c.r[3]), c.r[4]);
    c.r[3] = 0;
}
HLE(IPCiProfInit) {}
HLE(IPCiProfQueueReq) {}
HLE(IPCiProfAck) {}
HLE(IPCiProfReply) {}

}  // namespace recomp
