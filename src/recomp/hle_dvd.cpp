// HLE: DVD file system. The disc (an extracted folder, or an image's game partition read through
// nod: ISO, WBFS, RVZ, ...) is served by port/disc_folder.h; the Aurora DVD paths below are only
// a fallback when no disc was opened there. Game files may also be served by the game's own code (game.h
// overlayEntry). Reads complete on the host with modelled latency; async completions are
// delivered as interrupts.
#include "port/disc_folder.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <cstring>
#include <dolphin/dvd.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace recomp {
namespace {

namespace folder = port::discfolder;
// Per command block: the generation of its latest async read (DVDCancel drops older completions).
// Guest threads and the interrupts that complete reads run one at a time.
std::unordered_map<u32, u32> sReadGen;

// A destination the game may fill with file data: MEM1 above the OS globals (0x80003100), the
// host window above MEM1 (ctx.h), or MEM2 below the IOS area. A failed game heap allocation
// (null buffer) would otherwise land in low memory. Refused writes are logged.
bool destOk(u32 dest, u32 size, const char* what, u32 lr) {
    const u64 end = u64(dest) + size;
    const u32 seg = dest >> 28;
    const u64 base = seg == 0xC ? 0xC0000000ull : seg == 0xD ? 0xD0000000ull : seg == 0x9 ? 0x90000000ull : 0x80000000ull;
    const u64 off = dest - base, offEnd = end - base;
    const bool mem1 = (seg == 0x8 || seg == 0xC) && off >= 0x3100 && offEnd <= kLowWindowMask + 1ull;
    const bool mem2 = (seg == 0x9 || seg == 0xD) && offEnd <= kMem2Size;
    if (mem1 || mem2) return true;
    log("[dvd] ERROR: %s of %X bytes to %08X refused (lr %08X): the game's buffer allocation failed?", what, size,
        dest, lr);
    return false;
}

// One open file of the game (keyed by its guest DVDFileInfo).
struct HostFile {
    s32 overlay = -1;     // game.h overlay entry
    s32 entry = -1;       // FST entry
    bool folder = false;  // folder disc
    bool aurora = false;  // image disc: Aurora's file info
    DVDFileInfo fi{};
    u32 length = 0;
    u32 start = 0;  // the disc start address the game sees (+0x30)
};
std::mutex sFilesM;
std::unordered_map<u32, HostFile> sFiles;
// Files by their disc start address (DVDFileInfo +0x30). Games copy DVDFileInfo structs (NW4R's
// DvdFileStream keeps its own copy of the archive's) and read through the copy: the copy is
// recognised by its start address and length.
std::unordered_map<u32, HostFile> sByStart;
std::unordered_map<u32, u32> sNames;  // FST entry -> guest copy of its name
u32 sCwd = 0;                          // DVDChangeDir (folder discs)

bool logOn() {
    static const bool on = std::getenv("CF_DVD_LOG") != nullptr;
    return on;
}

// Disc latency. Games rely on reads taking time (LM: BootScene switches the current heap right
// after queueing an async open whose callback allocates), so a read keeps the requesting thread
// blocked while other threads run. Faster than the console's drive, but never instantaneous.
double readLatency(u32 bytes) {
    static const double scale = [] {
        const char* env = std::getenv("CF_DVD_SPEED");  // 1 = console speed
        return env ? 1.0 / std::atof(env) : 0.1;
    }();
    return scale * (0.010 + bytes / 3.0e6);
}

HostFile* hostFile(u32 guest) {
    std::lock_guard lk(sFilesM);
    auto it = sFiles.find(guest);
    if (it != sFiles.end()) {
        if (rd32(guest + 0x30) == it->second.start) return &it->second;
        sFiles.erase(it);  // the struct now holds another file
    }
    const u32 start = rd32(guest + 0x30);
    auto s = sByStart.find(start);
    if (s == sByStart.end() || rd32(guest + 0x34) != s->second.length) return nullptr;
    HostFile copy = s->second;
    if (copy.aurora) DVDFastOpen(copy.entry, &copy.fi);  // its own Aurora handle
    if (logOn()) log("[dvd] copied file info %08X -> entry %d", guest, copy.entry);
    return &(sFiles[guest] = copy);
}

void syncGuest(u32 guest, const HostFile& f, u32 startAddr) {
    wr32(guest + 0x0C, 0);  // DVD_STATE_END
    wr32(guest + 0x30, startAddr);
    wr32(guest + 0x34, f.length);
}

s32 pathToEntry(const char* path) {
    if (folder::active()) return folder::pathToEntry(path, sCwd);
    return DVDConvertPathToEntrynum(path);
}

bool openEntry(u32 guest, s32 entry, const char* what) {
    HostFile f;
    f.entry = entry;
    if (game::isOverlayEntry(entry)) {
        f.overlay = entry;
        f.length = game::overlayLength(entry);
        f.start = 0xF0000000u | u32(entry);
    } else if (folder::active()) {
        const folder::Entry* e = folder::entry(entry);
        if (!e || e->isDir) return false;
        f.folder = true;
        f.length = e->length;
        f.start = e->offset;
    } else {
        if (!DVDFastOpen(entry, &f.fi)) return false;
        f.aurora = true;
        f.length = f.fi.length;
        f.start = f.fi.startAddr;
    }
    if (logOn()) log("[dvd] open %s -> entry %d (%u bytes)", what, entry, f.length);
    {
        std::lock_guard lk(sFilesM);
        sFiles[guest] = f;
        sByStart[f.start] = f;
    }
    syncGuest(guest, f, f.start);
    return true;
}

s32 readFile(HostFile& f, void* dst, s32 len, s32 off, s32 prio) {
    if (f.overlay >= 0) return game::overlayRead(f.overlay, dst, len, off);
    if (f.folder) return s32(folder::read(f.entry, dst, u32(len), u32(off)));
    if (f.aurora) return DVDReadPrio(&f.fi, dst, len, off, prio);
    return -1;
}

}  // namespace

HLE(DVDInit) {}
HLE(DVDGetDriveStatus) { c.r[3] = 0; }
// The disc header id (game code, maker, disc, version) the apploader leaves at 0x80000000.
HLE(DVDGetCurrentDiskID) { c.r[3] = 0x80000000; }
HLE(DVDCheckDisk) { c.r[3] = 1; }
// DVDCheckDiskAsync(DVDCommandBlock*, callback): the disc is always in the drive (the game asks
// before a reset). The callback gets 1 (disc present) after a drive status round trip.
HLE(DVDCheckDiskAsync) {
    const u32 block = c.r[3], cb = c.r[4];
    wr32(block + 0x0C, 1);  // busy
    sched::postIrqAfter(0.001, [block, cb](Ctx& ic) {
        wr32(block + 0x0C, 0);
        if (cb) guestCall(ic, cb, 1, block);
    });
    c.r[3] = 1;
}
HLE(DVDGetCommandBlockStatus) { c.r[3] = rd32(c.r[3] + 0x0C); }
HLE(DVDConvertPathToEntrynum) {
    const char* path = gstr(c.r[3]);
    if (const s32 e = game::overlayEntry(path); e >= 0) {
        c.r[3] = u32(e);
        return;
    }
    const s32 n = pathToEntry(path);
    if (n < 0) log("[dvd] path not found: %s", path);
    c.r[3] = u32(n);
}
HLE(DVDEntrynumIsDir) {
    if (folder::active()) {
        const folder::Entry* e = folder::entry(s32(c.r[3]));
        c.r[3] = e && e->isDir ? 1 : 0;
        return;
    }
    c.r[3] = 0;
}

HLE(DVDOpen) {
    const char* path = gstr(c.r[3]);
    s32 e = game::overlayEntry(path);
    if (e < 0) e = pathToEntry(path);
    c.r[3] = e >= 0 && openEntry(c.r[4], e, path) ? 1 : 0;
    if (!c.r[3]) log("[dvd] open failed: %s", path);
}
HLE(DVDFastOpen) { c.r[3] = openEntry(c.r[4], s32(c.r[3]), "(entry)") ? 1 : 0; }
HLE(DVDClose) {
    std::lock_guard lk(sFilesM);
    if (auto it = sFiles.find(c.r[3]); it != sFiles.end()) {
        if (it->second.aurora) DVDClose(&it->second.fi);
        sFiles.erase(it);
    }
    c.r[3] = 1;
}

HLE(DVDReadPrio) {
    HostFile* f = hostFile(c.r[3]);
    watchRange(c.r[4], c.r[5], "DVDReadPrio");
    // The host read (disc image decompression) can take several milliseconds for a big read:
    // give up the CPU meanwhile, as the console's thread sleeps while the drive works.
    s32 result = -1;
    const bool ok = f && destOk(c.r[4], c.r[5], "DVDReadPrio", c.lr);
    if (ok) sched::withCpuReleased([&] { result = readFile(*f, hostp(c.r[4]), s32(c.r[5]), s32(c.r[6]), s32(c.r[7])); });
    // Block this thread until the "drive" finishes.
    static std::atomic<u32> sWaitKey{0xF0000000};  // sleep-queue keys need not be real memory
    const u32 done = sWaitKey.fetch_add(8);
    std::atomic<bool>* flag = new std::atomic<bool>(false);
    sched::postIrqAfter(readLatency(c.r[5]), [done, flag](Ctx&) {
        flag->store(true);
        sched::wakeup(done);
    });
    while (!flag->load()) sched::sleepOn(done);
    delete flag;
    if (result < 0) log("[dvd] ERROR: DVDReadPrio(%08X, %08X, %X, %X) failed (lr %08X)", c.r[3], c.r[4], c.r[5], c.r[6], c.lr);
    c.r[3] = u32(result);
}

HLE(DVDReadAsyncPrio) {
    const u32 guest = c.r[3], cb = c.r[7];
    HostFile* f = hostFile(guest);
    if (!f) {
        log("[dvd] ERROR: DVDReadAsyncPrio on an unknown file info %08X (lr %08X)", guest, c.lr);
        c.r[3] = 0;
        return;
    }
    watchRange(c.r[4], c.r[5], "DVDReadAsyncPrio");
    s32 result = -1;
    const bool ok = destOk(c.r[4], c.r[5], "DVDReadAsyncPrio", c.lr);
    const auto read = [&] {
        if (ok) result = readFile(*f, hostp(c.r[4]), s32(c.r[5]), s32(c.r[6]), s32(c.r[8]));
    };
    // As in DVDReadPrio, a slow host read must not hold off interrupts. On the console this call
    // returns at once, so with interrupts off (a caller's critical section) no other thread may
    // run in it: then the read just happens in place.
    if (sched::interruptsEnabled()) sched::withCpuReleased(read);
    else read();
    wr32(guest + 0x0C, 1);  // busy until the completion interrupt
    wr32(guest + 0x38, cb);
    wr32(guest + 0x20, u32(result));
    const u32 gen = ++sReadGen[guest];
    sched::postIrqAfter(readLatency(c.r[5]), [guest, cb, result, gen](Ctx& ic) {
        if (sReadGen[guest] != gen) return;  // cancelled (DVDCancel) or reissued
        wr32(guest + 0x0C, 0);
        if (cb) guestCall(ic, cb, u32(result), guest);
    });
    c.r[3] = 1;
}

// DVDCancel(block) / DVDCancelAsync(block, cb) (nw4r::ut::DvdLockedFileStream::Cancel: a sound
// stream stopped while its read is in flight; first seen on the Korean disc in Rover's bus scene,
// any version can do it): a read still in flight ends at once with
// DVD_RESULT_CANCELED (-3) to its callback, as the SDK's cancel does; its completion interrupt then
// does nothing. The data has already been copied (the host read is immediate), which a cancelled
// read may leave in the buffer on a console too.
namespace {
void cancelRead(Ctx& c, u32 block) {
    if (rd32(block + 0x0C) == 0) return;  // idle or done
    ++sReadGen[block];
    wr32(block + 0x0C, 10);  // DVD_STATE_CANCELED
    if (const u32 cb = rd32(block + 0x38)) guestCall(c, cb, u32(-3), block);
}
}  // namespace
HLE(DVDCancel) {
    cancelRead(c, c.r[3]);
    c.r[3] = 0;
}
HLE(DVDCancelAsync) {
    const u32 block = c.r[3], cb = c.r[4];
    cancelRead(c, block);
    if (cb) guestCall(c, cb, 0, block);
    c.r[3] = 1;
}

HLE(DVDPrepareStreamAsync) {  // streamed audio is not emulated (Wii games do not use DTK)
    const u32 guest = c.r[3], cb = c.r[6];
    sched::postIrq([guest, cb](Ctx& ic) {
        if (cb) guestCall(ic, cb, 0, guest);
    });
    c.r[3] = 1;
}
HLE(DVDCancelStream) { c.r[3] = 0; }

// DVDDir: entryNum, location (next entry to read), next (end of the directory).
HLE(DVDOpenDir) {
    if (folder::active()) {
        const s32 n = folder::pathToEntry(gstr(c.r[3]), sCwd);
        const folder::Entry* e = folder::entry(n);
        if (!e || !e->isDir) {
            c.r[3] = 0;
            return;
        }
        wr32(c.r[4], u32(n));
        wr32(c.r[4] + 4, u32(n) + 1);
        wr32(c.r[4] + 8, e->next);
        c.r[3] = 1;
        return;
    }
    DVDDir d{};
    const int ok = DVDOpenDir(gstr(c.r[3]), &d);
    if (ok) {
        wr32(c.r[4], d.entryNum);
        wr32(c.r[4] + 4, d.location);
        wr32(c.r[4] + 8, d.next);
    }
    c.r[3] = u32(ok);
}
HLE(DVDReadDir) {
    u32 entryNum = 0, isDir = 0;
    std::string name;
    int ok = 0;
    if (folder::active()) {
        const u32 loc = rd32(c.r[3] + 4), end = rd32(c.r[3] + 8);
        if (const folder::Entry* e = loc < end ? folder::entry(s32(loc)) : nullptr) {
            ok = 1;
            entryNum = loc;
            isDir = e->isDir;
            name = e->name;
            wr32(c.r[3] + 4, e->isDir ? e->next : loc + 1);
        }
    } else {
        DVDDir d{rd32(c.r[3]), rd32(c.r[3] + 4), rd32(c.r[3] + 8)};
        DVDDirEntry e{};
        ok = DVDReadDir(&d, &e);
        if (ok) {
            wr32(c.r[3] + 4, d.location);
            entryNum = e.entryNum;
            isDir = e.isDir;
            name = e.name ? e.name : "";
        }
    }
    if (ok) {
        u32& g = sNames[entryNum];
        if (!g) {
            g = hleAlloc(u32(name.size() + 1), 4);
            std::memcpy(hostp(g), name.c_str(), name.size() + 1);
        }
        wr32(c.r[4], entryNum);
        wr32(c.r[4] + 4, isDir);
        wr32(c.r[4] + 8, g);
    }
    c.r[3] = u32(ok);
}
HLE(DVDCloseDir) { c.r[3] = 1; }
HLE(DVDChangeDir) {
    const s32 n = folder::active() ? folder::pathToEntry(gstr(c.r[3]), sCwd) : -1;
    const folder::Entry* e = folder::entry(n);
    if (e && e->isDir) sCwd = u32(n);
    c.r[3] = e && e->isDir ? 1 : 0;
}

}  // namespace recomp
