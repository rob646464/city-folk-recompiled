#pragma once
// IOS (the Wii's Starlet I/O processor) as the game sees it through IPC (docs/WII.md "IOS",
// docs/NAND.md). The SDK's IPC client (IOS_Open/Close/Read/Write/Seek/Ioctl/Ioctlv and the
// async forms) is HLE'd in hle_ios.cpp; every library above it (ISFS, NAND, SC, ES, STM, VF,
// RFL, NWC24, KBD, ...) runs as recompiled game code, so its own logic, error handling and
// retry paths stay exact. Devices are host objects behind file descriptors; each request has a
// modelled IOS latency (the reply comes later, as on the console).
#include "recomp/ctx.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace recomp::ios {

// IPC / IOS result codes (the public IOS interface).
constexpr s32 IPC_OK = 0, IPC_EACCES = -1, IPC_EEXIST = -2, IPC_EINVAL = -4, IPC_EMAX = -5, IPC_ENOENT = -6;
// File-system results (/dev/fs and file descriptors).
constexpr s32 FS_EINVAL = -101, FS_EACCES = -102, FS_ECORRUPT = -103, FS_EEXIST = -105, FS_ENOENT = -106,
              FS_ENFILE = -107, FS_EFBIG = -108, FS_EFDEXHAUSTED = -109, FS_ENAMELEN = -110, FS_EFDOPEN = -111,
              FS_EIO = -114, FS_ENOTEMPTY = -115, FS_EDIRDEPTH = -116, FS_EBUSY = -118;
// ES results.
constexpr s32 ES_EINVAL = -1017, ES_ENOENT = -1106;

// Which IOS resource manager serves a request: requests to one manager are answered in order
// (Starlet's FS module handles one request at a time), different managers overlap.
enum class Queue { Fs, Es, Stm, Other, Count };

// Where a reply goes once the request is done (an async request's callback, or a waiting
// synchronous caller). A device that answers only later (STM event hook) keeps it.
struct Reply {
    u32 cb = 0, arg = 0;    // async: guest callback(result, arg); 0 = synchronous caller
    u64 token = 0;          // identifies the request for answerLater()
};

// A request a device has not answered yet (kept by the device, answered with answer()).
constexpr s32 kPending = 0x40000000;

struct Vec {
    u32 addr, len;
};

// An open file descriptor. Every operation returns an IOS result (or kPending: the device
// keeps the reply and calls answer() later). cost() is the modelled IOS time of the request
// just performed, in seconds.
class Handle {
public:
    virtual ~Handle() = default;
    virtual s32 read(u32 /*buf*/, u32 /*len*/) { return IPC_EINVAL; }
    virtual s32 write(u32 /*buf*/, u32 /*len*/) { return IPC_EINVAL; }
    virtual s32 seek(s32 /*off*/, s32 /*whence*/) { return IPC_EINVAL; }
    virtual s32 ioctl(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, const Reply& reply);
    virtual s32 ioctlv(u32 cmd, u32 nin, u32 nout, const Vec* v, const Reply& reply);
    virtual s32 close() { return IPC_OK; }
    // Modelled IOS time of the last request (seconds), before the latency scale.
    virtual double cost() const { return 0.0002; }
    virtual Queue queue() const { return Queue::Other; }
    std::string path;  // as opened (logs)
};

// Opens a device or NAND file; nullptr with `err` set when it fails.
using OpenFn = std::unique_ptr<Handle> (*)(const std::string& path, u32 mode, s32& err, double& cost);

// Answers a request a device returned kPending for (any thread). `result` goes to the caller.
void answer(const Reply& reply, s32 result);

// USB keyboard (/dev/usb/kbd, ios_devices.cpp): the PC keyboard as the Wii's USB keyboard while
// `enabled` (F1 > Online "USB keyboard"); `scancodes` = SDL keyboard state (HID usages). Host thread.
void usbKeyboardPoll(bool enabled, const u8* scancodes, int count);

// Latency multiplier for every IOS request (CF_IOS_LATENCY, default 1; 0 = instant).
double latencyScale();
bool logEnabled(int level = 1);  // CF_IOS_LOG
void logOnce(const std::string& what);

// --- devices (each in its own file) ---
std::unique_ptr<Handle> openFs(const std::string& path, u32 mode, s32& err, double& cost);  // ios_fs.cpp: /dev/fs + NAND files
std::unique_ptr<Handle> openDevice(const std::string& path, u32 mode, s32& err, double& cost);  // ios_devices.cpp: /dev/*

// --- NAND (ios_fs.cpp) ---
// Host folder of the emulated NAND (CF_NAND_DIR, default <dataDir>/nand).
const std::filesystem::path& nandRoot();
// Host path of a NAND path ("/shared2/sys/SYSCONF"); names the host cannot store are escaped.
std::filesystem::path hostPath(const std::string& nandPath);
// Host-side access to a NAND file (port features, e.g. the Mii maker), under the file system's
// lock. The write goes to a temporary file first and replaces the old one only when complete;
// it is refused while the game has the file open (error says why).
bool writeHostFile(const std::string& nandPath, const std::vector<u8>& data, std::string& error);
bool readHostFile(const std::string& nandPath, std::vector<u8>& data);
// File owner/permissions as IOS records them (kept in <nand>/~attributes.txt).
struct Attr {
    u32 owner = 0;
    u16 group = 0;
    u8 ownerPerm = 3, groupPerm = 3, otherPerm = 3, attr = 0;
};
void setAttr(const std::string& nandPath, const Attr& a);
// True while the game has a NAND file at or below `nandPath` open (save import: the game is saving).
bool filesOpenUnder(const std::string& nandPath);

// --- system (ios_devices.cpp) ---
u64 titleId();                     // the running disc title (00010000 52555545 for RUUE01)
std::string titleDataDir();        // "/title/00010000/52555545/data"
// Console buttons (STM event hook): power (0x800) and reset (0x20000). Any thread.
void stmEvent(u32 event);
constexpr u32 kStmEventReset = 0x20000, kStmEventPower = 0x800;
void startTestAids();  // CF_STM_EVENT (OSInit)

}  // namespace recomp::ios

namespace recomp {
// The OS's state-event handler (hle_os.cpp: os/OSStateTM.c is HLE'd): calls the game's power or
// reset callback.
void osStateEvent(u32 event);
}  // namespace recomp
