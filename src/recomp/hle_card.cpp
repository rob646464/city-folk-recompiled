// HLE: memory card, on top of Aurora's host CARD layer.
//
// Each slot is a folder of Dolphin-compatible .gci files under Aurora's user
// directory (e.g. %APPDATA%/Animal Crossing City Folk Recompiled/USA/Card A); slot A is created
// on first run. CF_CARD_DIR=<dir> places the card folders under <dir> instead.
// Guest structs (CARDFileInfo, CARDStat) are marshalled to and from their host
// counterparts; file data is transferred straight into guest memory. Every
// operation completes immediately: synchronous calls return the result, async
// ones also deliver it to their callback through an interrupt.
#include "recomp/hle.h"
#include "recomp/sched.h"

#include <aurora/card.h>
#include <dolphin/card.h>

#include <cstdlib>
#include <cstring>

namespace recomp {
namespace {

constexpr int kChans = 2;
bool sMounted[kChans]{};

bool validChan(u32 chan) { return chan < kChans; }

// Result for an operation on `chan` that needs a mounted card.
s32 mountedOrError(u32 chan) {
    if (!validChan(chan)) return CARD_RESULT_FATAL_ERROR;
    return sMounted[chan] ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}

// Guest CARDFileInfo: chan 0x00, fileNo 0x04, offset 0x08, length 0x0C, iBlock 0x10.
CARDFileInfo loadFileInfo(u32 g) {
    CARDFileInfo fi{};
    fi.chan = s32(rd32(g + 0x00));
    fi.fileNo = s32(rd32(g + 0x04));
    fi.offset = s32(rd32(g + 0x08));
    fi.length = s32(rd32(g + 0x0C));
    fi.iBlock = u16(rd16(g + 0x10));
    return fi;
}

void storeFileInfo(u32 g, const CARDFileInfo& fi) {
    wr32(g + 0x00, u32(fi.chan));
    wr32(g + 0x04, u32(fi.fileNo));
    wr32(g + 0x08, u32(fi.offset));
    wr32(g + 0x0C, u32(fi.length));
    wr16(g + 0x10, fi.iBlock);
}

// Guest CARDStat (0x6C bytes); same field offsets as the SDK header.
CARDStat loadStat(u32 g) {
    CARDStat st{};
    std::memcpy(st.fileName, hostp(g + 0x00), CARD_FILENAME_MAX);
    st.length = rd32(g + 0x20);
    st.time = rd32(g + 0x24);
    std::memcpy(st.gameName, hostp(g + 0x28), 4);
    std::memcpy(st.company, hostp(g + 0x2C), 2);
    st.bannerFormat = u8(rd8(g + 0x2E));
    st.iconAddr = rd32(g + 0x30);
    st.iconFormat = u16(rd16(g + 0x34));
    st.iconSpeed = u16(rd16(g + 0x36));
    st.commentAddr = rd32(g + 0x38);
    st.offsetBanner = rd32(g + 0x3C);
    st.offsetBannerTlut = rd32(g + 0x40);
    for (int i = 0; i < CARD_ICON_MAX; i++) st.offsetIcon[i] = rd32(g + 0x44 + 4 * i);
    st.offsetIconTlut = rd32(g + 0x64);
    st.offsetData = rd32(g + 0x68);
    return st;
}

void storeStat(u32 g, const CARDStat& st) {
    std::memcpy(hostp(g + 0x00), st.fileName, CARD_FILENAME_MAX);
    wr32(g + 0x20, st.length);
    wr32(g + 0x24, st.time);
    std::memcpy(hostp(g + 0x28), st.gameName, 4);
    std::memcpy(hostp(g + 0x2C), st.company, 2);
    wr8(g + 0x2E, st.bannerFormat);
    wr32(g + 0x30, st.iconAddr);
    wr16(g + 0x34, st.iconFormat);
    wr16(g + 0x36, st.iconSpeed);
    wr32(g + 0x38, st.commentAddr);
    wr32(g + 0x3C, st.offsetBanner);
    wr32(g + 0x40, st.offsetBannerTlut);
    for (int i = 0; i < CARD_ICON_MAX; i++) wr32(g + 0x44 + 4 * i, st.offsetIcon[i]);
    wr32(g + 0x64, st.offsetIconTlut);
    wr32(g + 0x68, st.offsetData);
}

// Guest file names are at most 32 bytes and need not be terminated.
struct Name {
    char s[CARD_FILENAME_MAX + 1]{};
    explicit Name(u32 g) { std::strncpy(s, gstr(g), CARD_FILENAME_MAX); }
};

// Completes an async call: the callback (if any) receives (chan, result).
void complete(u32 cb, u32 chan, s32 result) {
    if (cb) sched::postIrq([cb, chan, result](Ctx& ic) { guestCall(ic, cb, chan, u32(result)); });
}

// ---- operations shared by the sync and async entry points ----

s32 doMount(u32 chan) {
    if (!validChan(chan)) return CARD_RESULT_FATAL_ERROR;
    s32 mem = 0, sector = 0;
    if (CARDProbeEx(s32(chan), &mem, &sector) != CARD_RESULT_READY) {
        sMounted[chan] = false;
        return CARD_RESULT_NOCARD;
    }
    if (!sMounted[chan]) aurora_card_remount(s32(chan));  // pick up files changed on the host
    const s32 r = CARDGetResultCode(s32(chan));
    sMounted[chan] = r == CARD_RESULT_READY;
    return r;
}

s32 doCheck(u32 chan) {
    if (const s32 r = mountedOrError(chan); r != CARD_RESULT_READY) return r;
    return CARDCheck(s32(chan));
}

s32 doFormat(u32 chan) {
    if (const s32 r = mountedOrError(chan); r != CARD_RESULT_READY) return r;
    return CARDFormat(s32(chan));
}

s32 doCreate(u32 chan, u32 name, u32 size, u32 fileInfo) {
    if (const s32 r = mountedOrError(chan); r != CARD_RESULT_READY) return r;
    CARDFileInfo fi{};
    const s32 r = CARDCreate(s32(chan), Name(name).s, size, &fi);
    if (r == CARD_RESULT_READY) {
        fi.length = s32(size);
        storeFileInfo(fileInfo, fi);
    }
    return r;
}

s32 doRead(u32 fileInfo, u32 addr, u32 length, u32 offset) {
    CARDFileInfo fi = loadFileInfo(fileInfo);
    if (const s32 r = mountedOrError(u32(fi.chan)); r != CARD_RESULT_READY) return r;
    const s32 r = CARDRead(&fi, hostp(addr), s32(length), s32(offset));
    if (r == CARD_RESULT_READY) wr32(fileInfo + 0x08, offset + length);
    return r;
}

s32 doWrite(u32 fileInfo, u32 addr, u32 length, u32 offset) {
    CARDFileInfo fi = loadFileInfo(fileInfo);
    if (const s32 r = mountedOrError(u32(fi.chan)); r != CARD_RESULT_READY) return r;
    const s32 r = CARDWrite(&fi, hostp(addr), s32(length), s32(offset));
    if (r == CARD_RESULT_READY) wr32(fileInfo + 0x08, offset + length);
    return r;
}

s32 doSetStatus(u32 chan, u32 fileNo, u32 stat) {
    if (const s32 r = mountedOrError(chan); r != CARD_RESULT_READY) return r;
    const CARDStat st = loadStat(stat);
    return CARDSetStatus(s32(chan), s32(fileNo), &st);
}

void selfTest(Ctx& c);

}  // namespace

HLE(CARDInit) {
    // The game and maker codes come from the disk ID the boot code leaves at 0x80000000.
    char game[5]{}, maker[3]{};
    std::memcpy(game, hostp(0x80000000), 4);
    std::memcpy(maker, hostp(0x80000004), 2);
    if (const char* dir = std::getenv("CF_CARD_DIR")) CARDSetBasePath(dir, -1);
    CARDInit(game, maker);
    for (int i = 0; i < kChans; i++) {
        char path[1024];
        if (CARDProbe(i) && aurora_card_get_path(game, aurora_card_get_type(i), i, path, sizeof(path)) && path[0])
            log("[card] slot %c: %s", 'A' + i, path);
    }
    if (std::getenv("CF_CARD_SELFTEST")) selfTest(c);
}

HLE(CARDProbe) { c.r[3] = validChan(c.r[3]) && CARDProbe(s32(c.r[3])) ? 1 : 0; }

HLE(CARDProbeEx) {  // (chan, s32* memSize, s32* sectorSize)
    if (!validChan(c.r[3])) {
        c.r[3] = u32(CARD_RESULT_FATAL_ERROR);
        return;
    }
    s32 mem = 0, sector = 0;
    const s32 r = CARDProbeEx(s32(c.r[3]), &mem, &sector);
    if (r == CARD_RESULT_READY) {
        if (c.r[4]) wr32(c.r[4], u32(mem));
        if (c.r[5]) wr32(c.r[5], u32(sector));
    }
    c.r[3] = u32(r);
}

HLE(CARDMount) { c.r[3] = u32(doMount(c.r[3])); }  // (chan, workArea, detachCallback)
HLE(CARDMountAsync) {                               // (chan, workArea, detachCallback, attachCallback)
    const u32 chan = c.r[3], cb = c.r[6];
    const s32 r = doMount(chan);
    complete(cb, chan, r);
    c.r[3] = u32(r);
}
HLE(__CARDMountCallback) {}

HLE(CARDUnmount) {
    const u32 chan = c.r[3];
    if (!validChan(chan)) {
        c.r[3] = u32(CARD_RESULT_FATAL_ERROR);
        return;
    }
    const bool was = sMounted[chan];
    sMounted[chan] = false;
    c.r[3] = u32(was ? CARD_RESULT_READY : CARD_RESULT_NOCARD);
}

HLE(CARDCheck) { c.r[3] = u32(doCheck(c.r[3])); }
HLE(CARDCheckAsync) {  // (chan, callback)
    const s32 r = doCheck(c.r[3]);
    complete(c.r[4], c.r[3], r);
    c.r[3] = u32(r);
}
HLE(CARDCheckExAsync) {  // (chan, s32* xferBytes, callback)
    const s32 r = doCheck(c.r[3]);
    if (c.r[4]) wr32(c.r[4], 0);
    complete(c.r[5], c.r[3], r);
    c.r[3] = u32(r);
}

HLE(CARDFormat) { c.r[3] = u32(doFormat(c.r[3])); }
HLE(CARDFormatAsync) {  // (chan, callback)
    const s32 r = doFormat(c.r[3]);
    complete(c.r[4], c.r[3], r);
    c.r[3] = u32(r);
}

HLE(CARDFreeBlocks) {  // (chan, s32* bytesNotUsed, s32* filesNotUsed)
    const u32 chan = c.r[3];
    s32 r = mountedOrError(chan);
    if (r == CARD_RESULT_READY) {
        s32 bytes = 0, files = 0;
        r = CARDFreeBlocks(s32(chan), &bytes, &files);
        if (r == CARD_RESULT_READY) {
            if (c.r[4]) wr32(c.r[4], u32(bytes));
            if (c.r[5]) wr32(c.r[5], u32(files));
        }
    }
    c.r[3] = u32(r);
}

HLE(__CARDReadStatus) { c.r[3] = u32(CARD_RESULT_READY); }

HLE(CARDCreate) { c.r[3] = u32(doCreate(c.r[3], c.r[4], c.r[5], c.r[6])); }  // (chan, name, size, fileInfo)
HLE(CARDCreateAsync) {                                                          // (..., callback)
    const s32 r = doCreate(c.r[3], c.r[4], c.r[5], c.r[6]);
    complete(c.r[7], c.r[3], r);
    c.r[3] = u32(r);
}

HLE(CARDOpen) {  // (chan, name, fileInfo)
    const u32 chan = c.r[3], gfi = c.r[5];
    s32 r = mountedOrError(chan);
    if (r == CARD_RESULT_READY) {
        CARDFileInfo fi{};
        r = CARDOpen(s32(chan), Name(c.r[4]).s, &fi);
        if (r == CARD_RESULT_READY) {
            CARDStat st{};
            if (CARDGetStatus(s32(chan), fi.fileNo, &st) == CARD_RESULT_READY) fi.length = s32(st.length);
            storeFileInfo(gfi, fi);
        } else {
            wr32(gfi + 0x04, u32(-1));
        }
    }
    c.r[3] = u32(r);
}

HLE(CARDClose) {  // (fileInfo)
    CARDFileInfo fi = loadFileInfo(c.r[3]);
    s32 r = mountedOrError(u32(fi.chan));
    if (r == CARD_RESULT_READY) r = CARDClose(&fi);
    wr32(c.r[3] + 0x00, u32(-1));
    c.r[3] = u32(r);
}

HLE(CARDRead) { c.r[3] = u32(doRead(c.r[3], c.r[4], c.r[5], c.r[6])); }  // (fileInfo, buf, length, offset)
HLE(CARDReadAsync) {                                                       // (..., callback)
    const u32 chan = rd32(c.r[3]);
    const s32 r = doRead(c.r[3], c.r[4], c.r[5], c.r[6]);
    complete(c.r[7], chan, r);
    c.r[3] = u32(r);
}

HLE(CARDWrite) { c.r[3] = u32(doWrite(c.r[3], c.r[4], c.r[5], c.r[6])); }  // (fileInfo, buf, length, offset)
HLE(CARDWriteAsync) {                                                        // (..., callback)
    const u32 chan = rd32(c.r[3]);
    const s32 r = doWrite(c.r[3], c.r[4], c.r[5], c.r[6]);
    complete(c.r[7], chan, r);
    c.r[3] = u32(r);
}

HLE(CARDGetStatus) {  // (chan, fileNo, CARDStat*)
    const u32 chan = c.r[3];
    s32 r = mountedOrError(chan);
    if (r == CARD_RESULT_READY) {
        CARDStat st{};
        r = CARDGetStatus(s32(chan), s32(c.r[4]), &st);
        if (r == CARD_RESULT_READY) storeStat(c.r[5], st);
    }
    c.r[3] = u32(r);
}

HLE(CARDSetStatus) { c.r[3] = u32(doSetStatus(c.r[3], c.r[4], c.r[5])); }  // (chan, fileNo, CARDStat*)
HLE(CARDSetStatusAsync) {                                                    // (..., callback)
    const s32 r = doSetStatus(c.r[3], c.r[4], c.r[5]);
    complete(c.r[6], c.r[3], r);
    c.r[3] = u32(r);
}

namespace {

// CF_CARD_SELFTEST: exercises the guest-facing calls on a scratch file in slot A
// (create, write, status round trip, close, reopen, read back, delete).
void selfTest(Ctx& c) {
    const u32 name = hleAlloc(32), fi = hleAlloc(0x20), st = hleAlloc(0x80), buf = hleAlloc(0x4000);
    const u32 pmem = hleAlloc(8), fi2 = hleAlloc(0x20);
    std::strcpy(reinterpret_cast<char*>(hostp(name)), "cfrecomp card selftest");
    bool ok = true;
    auto expect = [&](const char* what, s32 got, s32 want) {
        if (got != want) {
            log("[card] selftest: %s returned %d, expected %d", what, got, want);
            ok = false;
        }
    };
    auto call = [&](HleFn fn, std::initializer_list<u32> args) {
        int i = 3;
        for (u32 a : args) c.r[i++] = a;
        fn(c);
        return s32(c.r[3]);
    };

    expect("CARDProbe", call(hle_CARDProbe, {0}), 1);
    expect("CARDProbeEx", call(hle_CARDProbeEx, {0, pmem, pmem + 4}), CARD_RESULT_READY);
    expect("sector size", s32(rd32(pmem + 4)), 0x2000);
    expect("CARDMount", call(hle_CARDMount, {0, 0, 0}), CARD_RESULT_READY);
    expect("CARDCheck", call(hle_CARDCheck, {0}), CARD_RESULT_READY);
    CARDDelete(0, gstr(name));  // leftovers from an interrupted run
    expect("CARDOpen (missing)", call(hle_CARDOpen, {0, name, fi}), CARD_RESULT_NOFILE);
    expect("CARDCreate", call(hle_CARDCreate, {0, name, 0x4000, fi}), CARD_RESULT_READY);
    for (u32 i = 0; i < 0x4000; i++) wr8(buf + i, (i * 7 + 3) & 0xFF);
    expect("CARDWrite", call(hle_CARDWrite, {fi, buf, 0x2000, 0}), CARD_RESULT_READY);
    expect("CARDWrite 2", call(hle_CARDWrite, {fi, buf + 0x2000, 0x2000, 0x2000}), CARD_RESULT_READY);
    const u32 fileNo = rd32(fi + 4);
    expect("CARDGetStatus", call(hle_CARDGetStatus, {0, fileNo, st}), CARD_RESULT_READY);
    wr32(st + 0x30, 0);
    wr32(st + 0x38, 0x1C00);
    wr8(st + 0x2E, 1);
    wr16(st + 0x34, 0x5);
    wr16(st + 0x36, 0x3);
    expect("CARDSetStatus", call(hle_CARDSetStatus, {0, fileNo, st}), CARD_RESULT_READY);
    expect("CARDClose", call(hle_CARDClose, {fi}), CARD_RESULT_READY);
    expect("CARDUnmount", call(hle_CARDUnmount, {0}), CARD_RESULT_READY);
    expect("CARDOpen (unmounted)", call(hle_CARDOpen, {0, name, fi}), CARD_RESULT_NOCARD);
    expect("CARDMount 2", call(hle_CARDMount, {0, 0, 0}), CARD_RESULT_READY);
    expect("CARDOpen", call(hle_CARDOpen, {0, name, fi}), CARD_RESULT_READY);
    expect("length", s32(rd32(fi + 0x0C)), 0x4000);
    std::memset(hostp(buf), 0, 0x4000);
    expect("CARDRead", call(hle_CARDRead, {fi, buf, 0x4000, 0}), CARD_RESULT_READY);
    for (u32 i = 0; i < 0x4000 && ok; i++)
        if (rd8(buf + i) != ((i * 7 + 3) & 0xFF)) expect("data byte", s32(i), -1);
    std::memset(hostp(st), 0, 0x6C);
    expect("CARDGetStatus 2", call(hle_CARDGetStatus, {0, rd32(fi + 4), st}), CARD_RESULT_READY);
    expect("iconAddr", s32(rd32(st + 0x30)), 0);
    expect("commentAddr", s32(rd32(st + 0x38)), 0x1C00);
    expect("bannerFormat", s32(rd8(st + 0x2E)), 1);
    expect("iconFormat", s32(rd16(st + 0x34)), 5);
    expect("iconSpeed", s32(rd16(st + 0x36)), 3);
    expect("stat length", s32(rd32(st + 0x20)), 0x4000);
    expect("CARDCreate (exists)", call(hle_CARDCreate, {0, name, 0x2000, fi2}), CARD_RESULT_EXIST);
    expect("CARDFreeBlocks", call(hle_CARDFreeBlocks, {0, pmem, pmem + 4}), CARD_RESULT_READY);
    log("[card] selftest: %d bytes / %d files free", s32(rd32(pmem)), s32(rd32(pmem + 4)));
    call(hle_CARDClose, {fi});
    expect("CARDDelete", CARDDelete(0, gstr(name)), CARD_RESULT_READY);
    call(hle_CARDUnmount, {0});
    log("[card] selftest %s", ok ? "PASSED" : "FAILED");
}

}  // namespace
}  // namespace recomp
