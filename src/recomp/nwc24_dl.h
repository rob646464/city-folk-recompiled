#pragma once
// WiiConnect24's download side as IOS runs it (docs/DISTRIBUTIONS.md, docs/ONLINE.md 1.2): the KD
// daemon (/dev/net/kd/request, /dev/net/kd/time), a network configuration for NCD
// (/dev/net/ncd/manage) and the NAND files the System Menu creates when WiiConnect24 is set up
// (sysconf.cpp prepareWc24). The game's own NWC24 library (recompiled code) registers its download
// tasks in nwc24dl.bin, installs its key file wc24pubk.mod and creates wc24dl.vff; this KD
// processes the tasks: fetches the file (from the Nintendo event archive, distributions.h),
// checks the WC24 container's RSA signature with the title's wc24pubk.mod, decrypts it with its
// AES key and stores the result in wc24dl.vff, where the game reads it.
//
// Only while F1 > Online > Nintendo events is on; otherwise ios_devices.cpp's KdDevice answers
// (WiiConnect24 off: every request accepted, nothing downloaded or written) and NCD stays absent.
//
// Ownership (docs/ONLINE.md "Who owns what"): this file is the download side. The mail side
// (message boxes, friend mail, KD's mail ioctls 0x0A-0x0D) belongs to branch online-1, which
// plugs its handlers in with setMailHandler() and may take over NCD with setNcdOwner().
#include "recomp/ctx.h"
#include "recomp/ios.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace recomp::nwc24 {

// True while WiiConnect24 is reported on to the game (a feature that needs it is enabled).
bool wc24On();

// ios_devices.cpp: /dev/net/kd/request, /dev/net/kd/time, /dev/net/ncd/manage. nullptr (err
// IPC_ENOENT) while WiiConnect24 is off or the path is not one of them.
std::unique_ptr<ios::Handle> openDevice(const std::string& path, s32& err);

// The System Menu's WiiConnect24 files (/shared2/wc24) are created by sysconf.cpp prepareWc24,
// with WiiConnect24 on or off. SYSCONF items for WiiConnect24 (NET.WCFG bit 0 = WiiConnect24 on): sysconf.cpp asks here.
u32 sysconfWcFlags();

// KD mail ioctls (0x0A check, 0x0B send, 0x0C receive, 0x0D save): online-1's mail HLE. The
// handler writes the reply into `out` and returns the IOS result; without one, KD answers
// "nothing to do" (result 0 in out[0]).
using MailHandler = std::function<s32(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen)>;
void setMailHandler(MailHandler h);
// When another module provides /dev/net/ncd/manage (online play), this KD's minimal NCD stays out.
void setNcdOwner(std::function<std::unique_ptr<ios::Handle>(const std::string&, s32&)> open);

// Runs KD's download check now from a host thread (F1 "Receive now", schedule changes). Safe at
// any time; the store waits until the game is not using its WiiConnect24 files.
void requestDownload();
// Status for the F1 page: last download (seconds of the game clock), its result text.
struct Status {
    bool taskRegistered = false;  // the game has its distribution task (rvforestdl) in nwc24dl.bin
    bool anyTask = false;         // the game has any download task (it ran its WiiConnect24 job for a town)
    bool keyInstalled = false;    // the title's wc24pubk.mod exists
    bool vffPresent = false;      // the title's wc24dl.vff exists
    long long lastCheck = 0;      // host time (s) of the last check
    std::string lastResult;
};
Status status();

// Seconds of the guest clock (UTC, incl. the F1 clock offset) since 1970.
long long guestUnixTime();

// Host-side helpers shared with distributions.cpp.
std::vector<u8> readNand(const std::string& nandPath);
bool writeNand(const std::string& nandPath, const std::vector<u8>& data);

}  // namespace recomp::nwc24
