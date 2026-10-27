// Test aid CF_NAND_SELFTEST=1 (docs/NAND.md "Testing"): right after the game's NANDInit, runs the
// game's own NAND library (recompiled code: NAND -> ISFS -> IPC -> our IOS file system) through a
// save-like sequence on a scratch file in the title's data folder and logs PASSED / FAILED:
// create, exists check, open, write, seek, read back, length, status, type, NANDCheck, the safe
// open/close a save uses (copy in /tmp, renamed back), delete, missing-file errors.
// CF_NAND_SELFTEST=keep leaves the file behind (a second run checks that it survived a restart).
#include "recomp/hle.h"
#include "recomp/ios.h"
#include "recomp/runtime.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

namespace recomp {
namespace {

// NAND library results (RVL SDK nand.h).
constexpr s32 NAND_OK = 0, NAND_EXISTS = -6, NAND_NOEXISTS = -12;

void selfTest(Ctx& c, bool keep) {
    const u32 path = hleAlloc(64), path2 = hleAlloc(64), fi = hleAlloc(0x100, 32), buf = hleAlloc(0x8000, 32),
              buf2 = hleAlloc(0x8000, 32), word = hleAlloc(32, 32), stat = hleAlloc(32, 32), home = hleAlloc(64, 32),
              safeBuf = hleAlloc(0x4000, 32);
    std::strcpy(reinterpret_cast<char*>(hostp(path)), "cftest.bin");
    std::strcpy(reinterpret_cast<char*>(hostp(path2)), "cfsafe.bin");
    bool ok = true;
    auto expect = [&](const char* what, s32 got, s32 want) {
        if (got != want) {
            log("[nand] selftest: %s returned %d, expected %d", what, got, want);
            ok = false;
        } else {
            log("[nand] selftest: %s -> %d", what, got);
        }
    };
    auto call = [&](const char* fn, std::initializer_list<u32> args) {
        int i = 3;
        for (u32 a : args) c.r[i++] = a;
        const u32 savedLr = c.lr;
        c.call(guestSymbol(fn));
        c.lr = savedLr;
        return s32(c.r[3]);
    };
    constexpr u32 kPermRw = 0x30 | 0x0C | 0x03;  // owner/group/other read+write
    constexpr u32 kAccessRw = 3;

    expect("NANDGetHomeDir", call("NANDGetHomeDir", {home}), NAND_OK);
    log("[nand] selftest: home %s", gstr(home));
    if (std::string(gstr(home)) != ios::titleDataDir()) ok = false;
    if (call("NANDGetType", {path, word}) == NAND_OK) {
        log("[nand] selftest: cftest.bin exists from an earlier run (kept): reading it back");
        expect("NANDOpen (kept)", call("NANDOpen", {path, fi, 1}), NAND_OK);
        expect("NANDRead (kept)", call("NANDRead", {fi, buf2, 0x6000}), 0x6000);
        for (u32 i = 0; i < 0x6000 && ok; i++)
            if (rd8(buf2 + i) != u8(i * 13 + 5)) expect("kept data byte", s32(i), -1);
        expect("NANDClose (kept)", call("NANDClose", {fi}), NAND_OK);
        expect("NANDDelete (kept)", call("NANDDelete", {path}), NAND_OK);
    }
    expect("NANDOpen (missing)", call("NANDOpen", {path, fi, 1}), NAND_NOEXISTS);
    expect("NANDCreate", call("NANDCreate", {path, kPermRw, 0}), NAND_OK);
    expect("NANDCreate (exists)", call("NANDCreate", {path, kPermRw, 0}), NAND_EXISTS);
    expect("NANDOpen", call("NANDOpen", {path, fi, kAccessRw}), NAND_OK);
    for (u32 i = 0; i < 0x6000; i++) wr8(buf + i, u8(i * 13 + 5));
    expect("NANDWrite", call("NANDWrite", {fi, buf, 0x4000}), 0x4000);
    expect("NANDWrite 2", call("NANDWrite", {fi, buf + 0x4000, 0x2000}), 0x2000);
    // (The function named NANDGetLength at 803A8444 returns the second word of IOS's file stats,
    // the position: the game uses it as a "tell"; docs/NAND.md.)
    expect("NANDSeek", call("NANDSeek", {fi, 0x1000, 0}), 0x1000);
    expect("NANDGetLength", call("NANDGetLength", {fi, word}), NAND_OK);
    expect("position", s32(rd32(word)), 0x1000);
    std::memset(hostp(buf2), 0, 0x8000);
    expect("NANDRead", call("NANDRead", {fi, buf2, 0x2000}), 0x2000);
    for (u32 i = 0; i < 0x2000 && ok; i++)
        if (rd8(buf2 + i) != u8((i + 0x1000) * 13 + 5)) expect("data byte", s32(i), -1);
    expect("NANDSeek (end)", call("NANDSeek", {fi, 0, 2}), 0x6000);
    expect("NANDRead (at end)", call("NANDRead", {fi, buf2, 0x20}), 0);
    expect("NANDClose", call("NANDClose", {fi}), NAND_OK);
    expect("NANDGetStatus", call("NANDGetStatus", {path, stat}), NAND_OK);
    log("[nand] selftest: status owner %08X group %04X attr %02X perm %02X", rd32(stat), rd16(stat + 4), rd8(stat + 6),
        rd8(stat + 7));
    expect("permission", rd8(stat + 7), s32(kPermRw));
    expect("NANDGetType", call("NANDGetType", {path, word}), NAND_OK);
    expect("type (file)", rd8(word), 1);
    wr32(word, 0xFFFFFFFF);
    expect("NANDCheck", call("NANDCheck", {36 * 8, 4, word}), NAND_OK);
    expect("NANDCheck answer", s32(rd32(word)), 0);

    // The safe open a save uses: a copy in /tmp is written, then renamed over the original.
    expect("NANDCreate (safe)", call("NANDCreate", {path2, kPermRw, 0}), NAND_OK);
    c.r[7] = 0x4000;  // (5th argument: work buffer length)
    expect("NANDSimpleSafeOpen", call("NANDSimpleSafeOpen", {path2, fi, kAccessRw, safeBuf, 0x4000}), NAND_OK);
    expect("NANDWrite (safe)", call("NANDWrite", {fi, buf, 0x3000}), 0x3000);
    expect("NANDSimpleSafeClose", call("NANDSimpleSafeClose", {fi}), NAND_OK);
    expect("NANDOpen (safe result)", call("NANDOpen", {path2, fi, 1}), NAND_OK);
    expect("NANDSeek (safe length)", call("NANDSeek", {fi, 0, 2}), 0x3000);
    expect("NANDSeek (safe start)", call("NANDSeek", {fi, 0, 0}), 0);
    std::memset(hostp(buf2), 0, 0x8000);
    expect("NANDRead (safe)", call("NANDRead", {fi, buf2, 0x3000}), 0x3000);
    if (std::memcmp(hostp(buf), hostp(buf2), 0x3000) != 0) expect("safe data", -1, 0);
    expect("NANDClose (safe)", call("NANDClose", {fi}), NAND_OK);
    expect("NANDDelete (safe)", call("NANDDelete", {path2}), NAND_OK);

    if (keep) {
        // Rewrite the full test pattern so the next run reads it back.
        log("[nand] selftest: keeping cftest.bin for the next run");
    } else {
        expect("NANDDelete", call("NANDDelete", {path}), NAND_OK);
        expect("NANDDelete (missing)", call("NANDDelete", {path}), NAND_NOEXISTS);
    }
    log("[nand] selftest %s", ok ? "PASSED" : "FAILED");
}

}  // namespace

// The game's NANDInit, then the self-test if asked for.
HLE(NANDInit) {
    callOriginal(c, "NANDInit");
    const s32 result = s32(c.r[3]);
    static const char* sTest = std::getenv("CF_NAND_SELFTEST");
    static bool sDone = false;
    if (sTest && !sDone) {
        sDone = true;
        selfTest(c, std::strcmp(sTest, "keep") == 0);  // (guest calls keep the callee-saved registers)
    }
    c.r[3] = u32(result);
}

}  // namespace recomp
