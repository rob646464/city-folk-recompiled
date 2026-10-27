// IOS devices other than the file system (docs/NAND.md "Devices"):
//   /dev/es             title id, data folder, device id, ticket views, title list; a title launch
//                       (OSReturnToMenu: back to the Wii Menu) ends the program
//   /dev/stm/immediate  power/reset/idle requests end the program; LED / dimming accepted
//   /dev/stm/eventhook  held until a console button event (stmEvent): POWER 0x800, RESET 0x20000
//   /dev/di             accepted (disc reads go through the DVD HLE)
//   /dev/sdio/slot0     an SD slot with no card in it
//   /dev/usb/kbd        the USB keyboard module with no keyboard attached (its event request waits)
//   /dev/usb/oh0        the USB host with no device (Wii Speak absent)
//   /dev/net/*          not running (offline: NCD/SO/KD/SSL opens fail, as when the network
//                       modules are not up), except KD and NCD while a WiiConnect24 feature is on
//                       (nwc24_dl.cpp: Nintendo events, docs/DISTRIBUTIONS.md); online play: docs/ONLINE.md
// Written from the public IOS interface documentation, not from another emulator's code.
#include "recomp/ios.h"

#include "port/install.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/nwc24_dl.h"
#include "recomp/runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace recomp::ios {

u64 titleId() {
    // 00010000 = disc title; low word = the game id's first four characters ("RUUE").
    return 0x0001000052555500ull | u8(port::gameId(port::install().running)[3]);
}

std::string titleDataDir() {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "/title/%08x/%08x/data", u32(titleId() >> 32), u32(titleId()));
    return buf;
}

namespace {

constexpr u64 kSystemMenu = 0x0000000100000002ull;
constexpr u32 kDeviceId = 0x0B1C2D3Eu;  // console id (ES_GetDeviceID); a fixed value for every install

void wr64(u32 a, u64 v) {
    wr32(a, u32(v >> 32));
    wr32(a + 4, u32(v));
}
u64 rd64(u32 a) { return u64(rd32(a)) << 32 | rd32(a + 4); }

// The game leaves (title launch = back to the Wii Menu, STM power/reset): the program ends like
// "Quit game"; the request is never answered (on the console IOS reboots).
s32 endProgram(const char* why) {
    recomp::log("[ios] %s: the game leaves; the program ends", why);
    menu::requestQuit();
    return kPending;
}

// --- ES -------------------------------------------------------------------------------------

class EsDevice final : public Handle {
public:
    s32 ioctlv(u32 cmd, u32 nin, u32 nout, const Vec* v, const Reply& r) override {
        auto out = [&](u32 i, u32 need) -> u32 { return nout > i && v[nin + i].len >= need ? v[nin + i].addr : 0; };
        auto in = [&](u32 i, u32 need) -> u32 { return nin > i && v[i].len >= need ? v[i].addr : 0; };
        switch (cmd) {
        case 0x07:  // GetDeviceID -> u32
            if (const u32 o = out(0, 4)) return wr32(o, kDeviceId), IPC_OK;
            return ES_EINVAL;
        case 0x08: {  // LaunchTitle(title id, ticket view): leaving the game (Wii Menu, a channel)
            char why[64];
            std::snprintf(why, sizeof(why), "ES_LaunchTitle %016llX", (unsigned long long)(in(0, 8) ? rd64(v[0].addr) : 0));
            return endProgram(why);
        }
        case 0x0C:    // GetOwnedTitlesCount -> u32
        case 0x0E: {  // GetTitlesCount -> u32
            if (const u32 o = out(0, 4)) return wr32(o, 2), IPC_OK;
            return ES_EINVAL;
        }
        case 0x0D:    // GetOwnedTitles(count) -> u64[]
        case 0x0F: {  // GetTitles(count) -> u64[]
            const u32 n = in(0, 4) ? rd32(v[0].addr) : 0;
            const u32 o = out(0, 8 * n);
            const u64 titles[2] = {kSystemMenu, titleId()};
            for (u32 i = 0; i < n && i < 2 && o; i++) wr64(o + 8 * i, titles[i]);
            return o || n == 0 ? IPC_OK : ES_EINVAL;
        }
        case 0x12: {  // GetTicketViewsCount(title id) -> u32
            if (const u32 o = out(0, 4)) return wr32(o, known(v, nin) ? 1 : 0), IPC_OK;
            return ES_EINVAL;
        }
        case 0x13: {  // GetTicketViews(title id, count) -> views (0xD8 bytes each)
            const u32 o = out(0, 0xD8);
            if (!o || !known(v, nin)) return ES_EINVAL;
            std::memset(hostp(o), 0, 0xD8);
            const u64 t = rd64(v[0].addr);
            wr64(o + 0x04, 0x0001000000000000ull | (t & 0xFFFFFFFFull));  // ticket id
            wr32(o + 0x0C, kDeviceId);
            wr64(o + 0x10, t);
            wr16(o + 0x18, 0xFFFF);  // access mask
            return IPC_OK;
        }
        case 0x16: {  // GetConsumption(title id) -> limits[], count: no play limits
            if (const u32 o = out(1, 4)) wr32(o, 0);
            return IPC_OK;
        }
        case 0x1D: {  // GetDataDir(title id) -> "/title/xxxxxxxx/xxxxxxxx/data"
            const u32 o = out(0, 30);
            if (!o || !in(0, 8)) return ES_EINVAL;
            const u64 t = rd64(v[0].addr);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "/title/%08x/%08x/data", u32(t >> 32), u32(t));
            std::memcpy(hostp(o), buf, std::strlen(buf) + 1);
            return IPC_OK;
        }
        case 0x20:  // GetTitleID -> u64 (the running title)
            if (const u32 o = out(0, 8)) return wr64(o, titleId()), IPC_OK;
            return ES_EINVAL;
        default: break;
        }
        return Handle::ioctlv(cmd, nin, nout, v, r);
    }
    Queue queue() const override { return Queue::Es; }

private:
    static bool known(const Vec* v, u32 nin) {
        if (nin < 1 || v[0].len < 8) return false;
        const u64 t = rd64(v[0].addr);
        return t == titleId() || t == kSystemMenu;
    }
};

// --- STM ------------------------------------------------------------------------------------

std::mutex sStmM;
std::optional<Reply> sHook;  // the event hook request, while one is outstanding
u32 sHookOut = 0;

}  // namespace

// Test aid CF_STM_EVENT=<seconds>[:power|reset]: presses the console button that long after boot.
void startTestAids() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* v = std::getenv("CF_STM_EVENT");
        if (!v) return;
        const double secs = std::atof(v);
        const u32 ev = std::strstr(v, "reset") ? kStmEventReset : kStmEventPower;
        std::thread([secs, ev] {
            std::this_thread::sleep_for(std::chrono::duration<double>(secs));
            stmEvent(ev);
        }).detach();
    });
}

namespace {

class StmHook final : public Handle {
public:
    s32 ioctl(u32 cmd, u32, u32, u32 out, u32 outLen, const Reply& r) override {
        if (cmd != 0x1000) return Handle::ioctl(cmd, 0, 0, out, outLen, r);
        std::lock_guard lk(sStmM);
        if (sHook) return IPC_EEXIST;
        sHook = r;
        sHookOut = out;
        return kPending;
    }
    Queue queue() const override { return Queue::Stm; }
};

class StmImmediate final : public Handle {
public:
    s32 ioctl(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, const Reply& r) override {
        switch (cmd) {
        case 0x3001: {  // ReleaseEventHook: the outstanding hook returns with event 0
            std::lock_guard lk(sStmM);
            if (sHook) {
                if (sHookOut) wr32(sHookOut, 0);
                answer(*sHook, IPC_OK);
                sHook.reset();
            }
            return IPC_OK;
        }
        case 0x2001: return endProgram("STM hot reset");
        case 0x2002: return endProgram("STM hot reset (power down)");
        case 0x2003: return endProgram("STM shutdown");
        case 0x2004: return endProgram("STM standby (idle)");
        case 0x5001:  // VI dimming
        case 0x6001:  // LED flash
        case 0x6002:  // LED mode
            return IPC_OK;
        default: break;
        }
        logOnce("stm ioctl " + std::to_string(cmd) + " (accepted)");
        (void)in, (void)inLen, (void)out, (void)outLen, (void)r;
        return IPC_OK;
    }
    Queue queue() const override { return Queue::Stm; }
};

// --- simple devices -----------------------------------------------------------------------------

// Accepts everything (disc interface: reads are HLE'd at the DVD library level).
class AcceptAll final : public Handle {
public:
    s32 ioctl(u32 cmd, u32, u32, u32, u32, const Reply&) override {
        logOnce(path + " ioctl " + std::to_string(cmd) + " (accepted)");
        return IPC_OK;
    }
    s32 ioctlv(u32 cmd, u32, u32, const Vec*, const Reply&) override {
        logOnce(path + " ioctlv " + std::to_string(cmd) + " (accepted)");
        return IPC_OK;
    }
};

// An SD slot with no card: status 0 (no card inserted); commands to a card fail.
class SdSlot final : public Handle {
public:
    s32 ioctl(u32 cmd, u32, u32, u32 out, u32 outLen, const Reply&) override {
        if (cmd == 0x0B) {  // GetStatus
            if (out && outLen >= 4) wr32(out, 0);
            return IPC_OK;
        }
        if (cmd == 0x04) return IPC_OK;  // ResetCard / clock settings: nothing to do
        logOnce(path + " ioctl " + std::to_string(cmd) + " (no SD card)");
        return IPC_EINVAL;
    }
    s32 ioctlv(u32 cmd, u32, u32, const Vec*, const Reply&) override {
        logOnce(path + " ioctlv " + std::to_string(cmd) + " (no SD card)");
        return IPC_EINVAL;
    }
};

// The USB keyboard module: its event request returns when a keyboard is attached, which never
// happens here; held requests stay pending as on a console without a keyboard.
class UsbKeyboard final : public Handle {
public:
    s32 ioctl(u32 cmd, u32, u32, u32, u32, const Reply& r) override {
        if (r.cb) return kPending;  // (async: held)
        logOnce(path + " synchronous ioctl " + std::to_string(cmd) + " (no keyboard: returns an error)");
        return IPC_EINVAL;
    }
};

// The USB host controller (Wii Speak): no devices.
class UsbHost final : public Handle {
public:
    s32 ioctlv(u32 cmd, u32 nin, u32 nout, const Vec* v, const Reply& r) override {
        if (cmd == 0x0C && nout >= 1) {  // GetDeviceList -> count 0
            if (v[nin].len >= 1) wr8(v[nin].addr, 0);
            return IPC_OK;
        }
        return Handle::ioctlv(cmd, nin, nout, v, r);
    }
    s32 ioctl(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, const Reply& r) override {
        if (r.cb) return kPending;  // attach notifications: no device ever comes
        return Handle::ioctl(cmd, in, inLen, out, outLen, r);
    }
};

}  // namespace

void stmEvent(u32 event) {
    std::lock_guard lk(sStmM);
    recomp::log("[ios] STM event %X (%s)", event, event == kStmEventPower ? "power" : event == kStmEventReset ? "reset" : "?");
    if (sHook) {  // game code waiting on the event hook itself
        if (sHookOut) wr32(sHookOut, event);
        answer(*sHook, IPC_OK);
        sHook.reset();
        return;
    }
    // The OS's own hook (os/OSStateTM.c is HLE'd): its handler calls the game's callbacks.
    osStateEvent(event);
}

std::unique_ptr<Handle> openDevice(const std::string& path, u32, s32& err, double& cost) {
    cost = 0.0002;
    if (path == "/dev/es") return std::make_unique<EsDevice>();
    if (path == "/dev/stm/eventhook") return std::make_unique<StmHook>();
    if (path == "/dev/stm/immediate") return std::make_unique<StmImmediate>();
    if (path == "/dev/di") return std::make_unique<AcceptAll>();
    if (path == "/dev/sdio/slot0" || path == "/dev/sdio/slot1") return std::make_unique<SdSlot>();
    if (path == "/dev/usb/kbd" && std::getenv("CF_USB_KBD_DEVICE")) return std::make_unique<UsbKeyboard>();
    if (path == "/dev/usb/oh0") return std::make_unique<UsbHost>();
    if (path.rfind("/dev/net/", 0) == 0)
        if (auto h = nwc24::openDevice(path, err)) return h;
    logOnce("open " + path + ": no such device");
    err = IPC_ENOENT;
    return nullptr;
}

}  // namespace recomp::ios
