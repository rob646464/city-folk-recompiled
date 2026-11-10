// Mii maker: F1 > Miis (docs/MIIS.md).
//
// The Mii database of the emulated console is the NAND file /shared2/menu/FaceLib/RFL_DB.dat
// (in the data folder's nand\ folder, so portable mode and other data folders have their own).
// City Folk's RFL library (recompiled game code, not HLE) reads it once at boot (RFLInitRes ->
// RFLiBootLoadDatabaseAsync) into a buffer in RFL's heap and answers every later question
// (RFLIsAvailableOfficialData, RFLSearchOfficialData, RFLiGetCharInfo...) from that buffer. So:
// - Saving in the editor writes the file (a temporary file, then a rename, under the NAND file
//   system's lock and never while the game has it open: ios::writeHostFile).
// - The same bytes then go into RFL's buffer in memory at a safe point of a guest frame: RFL
//   initialised and idle (no asynchronous database or resource operation, RFLiIsWorking), and
//   the Mii picker of Shampoodle (d_menu_miiNP) not open, so a list the game is showing never
//   changes under it. The "database not found" / "broken" flags RFL set at boot (a console that
//   never had Miis) are cleared then, because the file now exists and is valid: RFL behaves as
//   if it had read this file at boot. Nothing else in RFL is derived from the file (the hidden
//   Mii area after 0x1F1E0 is read only on demand and kept as it was).
// - Masks the player already owns keep their own copy of the Mii (RFL store data in the town
//   save, RFLAddMiddleDBStoreData), as on the console.
//
// The preview is drawn by the game's RFL: the edited Mii goes into a one-Mii "middle database"
// (RFLInitMiddleDB + RFLAddMiddleDBStoreData, as the game does for a worn mask, fn_8008B3A8),
// RFLMakeIcon builds the 3D head from the disc's parts (Face/RFLRes01.arc, loaded by the game)
// and renders it into the EFB, then copies it to a texture with GXCopyTex: the same path, the
// same moment of the frame and the same arguments (background colour, transparent) the game
// uses for its own Mii icons (its icon queue fn_8000A57C at the start of the stage draw, d_s_stageNP
// fn_125_C0; hooked, our request runs right after the game's). The copy is read back into guest
// memory (Aurora's EFB copy to RAM, a watched range: also with that option off) and shown in the
// menu as an ImGui texture (Aurora 0410 updates it in place). The view turns by multiplying a
// rotation about the head's vertical axis into the model matrix RFL sets for the icon
// (RFLSetMtx hooked only while our request draws). RFL makes its icon buffer in its own heap: the
// size is checked against the heap's free space first (128 or 64 px instead of 256 if needed).
//
// Test aids: CF_MII_LOG=1 (sync, preview, notice), CF_MII_AT=<frame>:<cmd>[;...] (or | between commands) (editor commands
// at guest frames, host side: new | random | name=<text> | set=<field>=<value> | select=<slot> |
// yaw=<degrees> | expr=<n> | save | import=<path>[,merge|,replace] | export=<path> |
// exportdb=<path> | shot=<path.ppm> | delete | duplicate | open (opens F1 > Miis)).
#include "game/cf_mii.h"

#include "game/cf_mii_data.h"
#include "port/install.h"
#include "port/settings.h"
#include "recomp/efb_copy.h"
#include "recomp/frame.h"
#include "recomp/game.h"
#include "recomp/hle.h"
#include "recomp/ios.h"
#include "recomp/menu.h"
#include "recomp/runtime.h"
#include "recomp/ui_scale.h"

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/efb_ram.h>
#include <aurora/imgui.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <ostream>
#include <random>
#include <string>
#include <vector>

namespace recomp::cf {
namespace {

namespace fs = std::filesystem;
using mii::F;
using mii::Mii;

constexpr const char* kDbPath = "/shared2/menu/FaceLib/RFL_DB.dat";
constexpr u32 kMiiMenuModule = 165;  // d_menu_miiNP: Shampoodle's Mii picker
constexpr u32 kHarrietModule = 47;   // d_a_npc_sp_catherineNP: Harriet
constexpr int kTexMax = 256;         // preview icon (RFLMakeIcon), RGB5A3

const bool sLog = std::getenv("CF_MII_LOG") != nullptr;

// ---- state shared between the host (menu) and the guest thread ------------------------------

struct Shared {
    std::mutex m;
    // Saved database waiting to go into RFL's buffer (guest thread).
    std::vector<u8> pending;
    bool hasPending = false;
    u32 syncedGeneration = 0, pendingGeneration = 0;
    // Preview request (host -> guest).
    u8 reqRaw[mii::kRawSize] = {};
    float reqYaw = 0.0f;
    int reqExpr = 0;
    u32 reqSeq = 0;
    bool wanted = false;      // the Miis page was drawn recently
    u32 wantedUntil = 0;      // guest frame
    // Preview result (guest -> host).
    std::vector<u8> rgba = std::vector<u8>(size_t(kTexMax) * kTexMax * 4);
    u32 outSeq = 0, outSize = 0;
    std::string previewError;
    u32 lastStageFrame = 0;   // guest frame of the last stage draw (preview possible)
    // RFL's default Miis (RFLiGetDefaultData), read once.
    std::vector<Mii> defaults;
    // Mii picker / Harriet state for the hint.
    bool pickerOpen = false, harrietHere = false;
    int guestCount = -1;      // Miis RFL sees (valid ones), -1 unknown
    u32 noticeAt = 0;         // guest frame the hint was raised
} sh;

// ---- guest helpers -----------------------------------------------------------------------------

// Guest calls from the frame hooks run inside other HLE handlers: keep every register as it was.
struct RegSave {
    explicit RegSave(Ctx& c) : c_(c) {
        std::memcpy(r_, c.r, sizeof r_);
        std::memcpy(f_, c.f, sizeof f_);
        std::memcpy(ps_, c.ps1, sizeof ps_);
        std::memcpy(cr_, c.cr, sizeof cr_);
        lr_ = c.lr, ctr_ = c.ctr, ca_ = c.ca, so_ = c.so, ov_ = c.ov, fpscr_ = c.fpscr;
    }
    ~RegSave() {
        std::memcpy(c_.r, r_, sizeof r_);
        std::memcpy(c_.f, f_, sizeof f_);
        std::memcpy(c_.ps1, ps_, sizeof ps_);
        std::memcpy(c_.cr, cr_, sizeof cr_);
        c_.lr = lr_, c_.ctr = ctr_, c_.ca = ca_, c_.so = so_, c_.ov = ov_, c_.fpscr = fpscr_;
    }
    Ctx& c_;
    u32 r_[32];
    double f_[32], ps_[32];
    u8 cr_[8];
    u32 lr_, ctr_, ca_, so_, ov_, fpscr_;
};

u32 call(Ctx& c, u32 fn, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0, u32 a4 = 0, u32 a5 = 0) {
    const u32 lr = c.lr;
    c.r[3] = a0, c.r[4] = a1, c.r[5] = a2, c.r[6] = a3, c.r[7] = a4, c.r[8] = a5;
    c.call(fn);
    c.lr = lr;
    return c.r[3];
}

struct Syms {
    u32 available, isWorking, getManager, getDbManager, defaultData, info2raw, initMiddleDb, addStoreData,
        makeIcon, modelBufferSize, heapFree;
    Syms()
        : available(guestSymbol("RFLAvailable")), isWorking(guestSymbol("RFLiIsWorking")),
          getManager(guestSymbol("RFLiGetManager")), getDbManager(guestSymbol("RFLiGetDBManager")),
          defaultData(guestSymbol("RFLiGetDefaultData")), info2raw(guestSymbol("convertInfo2RawCore_")),
          initMiddleDb(guestSymbol("RFLInitMiddleDB")), addStoreData(guestSymbol("RFLAddMiddleDBStoreData")),
          makeIcon(guestSymbol("RFLMakeIcon")), modelBufferSize(guestSymbol("RFLGetModelBufferSize")),
          heapFree(guestSymbol("MEMGetAllocatableSizeForExpHeapEx")) {}
};
const Syms& syms() {
    static const Syms s;
    return s;
}

// Offset of the RFL manager's error-flags byte (USA 0x1B3C), read from RFLiNotFoundError's
// `addi r4, r3, <off>` so a version with another layout is noticed (then the flags stay).
u32 flagsOffset() {
    static const u32 off = [] {
        const u32 fn = guestSymbol("RFLiNotFoundError");
        for (u32 i = 0; i < 6; i++) {
            const u32 w = rd32(fn + 4 * i);
            if ((w & 0xFFFF0000u) == 0x38830000u) return w & 0xFFFFu;  // addi r4, r3, imm
        }
        return 0u;
    }();
    return off;
}

bool rflIdle(Ctx& c) {
    const Syms& s = syms();
    return call(c, s.available) != 0 && call(c, s.isWorking) == 0;
}

struct GuestBufs {
    u32 store = 0, mdb = 0, mdbBuf = 0, setting = 0, mtx = 0, info = 0, raw = 0;
    u32 tex[2] = {};
};
GuestBufs& bufs() {
    static GuestBufs b = [] {
        GuestBufs g;
        g.store = hleAlloc(0x60, 32);
        g.mdb = hleAlloc(0x20, 32);
        g.mdbBuf = hleAlloc(0x40 * 2, 32);
        g.setting = hleAlloc(0x20, 32);
        g.mtx = hleAlloc(0x30, 32);
        g.info = hleAlloc(0x80, 32);
        g.raw = hleAlloc(0x60, 32);
        for (u32& t : g.tex) {
            t = hleAlloc(u32(kTexMax * kTexMax * 2), 32);
            aurora_efb_ram_watch(hostp(t), u32(kTexMax * kTexMax * 2));
        }
        return g;
    }();
    return b;
}

// ---- preview: render (guest), read back (writeback listener) ----------------------------------

std::atomic<bool> sRotating{false};
float sRotYaw = 0.0f;
struct TexSlot {
    u32 seq = 0, size = 0, frame = 0;
    bool busy = false;
    u32 writes = 0;
};
TexSlot sTex[2];
int sNextTex = 0;
u32 sRenderedSeq = 0;

void decodeRgb5a3(const u8* src, u32 size, u8* rgba) {
    // GX RGB5A3 in 4x4 texel tiles; output is size x size RGBA8 at the top-left of a kTexMax
    // square (the rest transparent).
    std::memset(rgba, 0, size_t(kTexMax) * kTexMax * 4);
    const u32 tilesX = size / 4;
    for (u32 y = 0; y < size; y++)
        for (u32 x = 0; x < size; x++) {
            const u32 tile = (y / 4) * tilesX + x / 4;
            const u8* p = src + tile * 32 + ((y % 4) * 4 + x % 4) * 2;
            const u16 v = u16(p[0] << 8 | p[1]);
            u8 r, g, b, a;
            if (v & 0x8000) {
                r = u8(((v >> 10) & 31) * 255 / 31), g = u8(((v >> 5) & 31) * 255 / 31), b = u8((v & 31) * 255 / 31), a = 255;
            } else {
                a = u8(((v >> 12) & 7) * 255 / 7), r = u8(((v >> 8) & 15) * 17), g = u8(((v >> 4) & 15) * 17), b = u8((v & 15) * 17);
            }
            u8* o = rgba + (size_t(y) * kTexMax + x) * 4;
            o[0] = r, o[1] = g, o[2] = b, o[3] = a;
        }
}

void onWriteback(const void* dest, u32 size) {
    for (int i = 0; i < 2; i++) {
        if (!sTex[i].busy || !bufs().tex[i] || dest != hostp(bufs().tex[i])) continue;
        (void)size;
        // RFLiMakeIcon copies into the icon twice: RFLiSetupCopyTex first copies the region
        // with clear (to empty it: that copy holds whatever the EFB had), then the finished icon.
        if (++sTex[i].writes < 2) continue;
        std::lock_guard lk(sh.m);
        decodeRgb5a3(hostp(bufs().tex[i]), sTex[i].size, sh.rgba.data());
        sh.outSeq = sTex[i].seq;
        sh.outSize = sTex[i].size;
        sTex[i].busy = false;
        if (sLog) log("[mii] preview %u landed (%u px)", sh.outSeq, sh.outSize);
    }
}

void renderPreview(Ctx& c) {
    u8 raw[mii::kRawSize];
    u32 seq;
    float yaw;
    int expr;
    {
        std::lock_guard lk(sh.m);
        sh.lastStageFrame = game::guestFrames();
        if (!sh.wanted || game::guestFrames() > sh.wantedUntil || sh.reqSeq == sRenderedSeq) return;
        std::memcpy(raw, sh.reqRaw, sizeof raw);
        seq = sh.reqSeq;
        yaw = sh.reqYaw;
        expr = sh.reqExpr;
    }
    // Two buffers: a copy is read back a few frames later (interpolated frames replay later), and
    // one that never lands (a dropped frame) frees its buffer after a second.
    const u32 now = game::guestFrames();
    for (TexSlot& t : sTex)
        if (t.busy && now > t.frame + 60) t.busy = false;
    int slot = sNextTex;
    if (sTex[slot].busy) slot ^= 1;
    if (sTex[slot].busy) return;
    RegSave save(c);
    const Syms& s = syms();
    if (!rflIdle(c)) return;
    GuestBufs& b = bufs();
    // Store data = record + CRC (RFLAddMiddleDBStoreData checks it).
    Mii m;
    std::memcpy(m.raw, raw, sizeof raw);
    u8 store[mii::kStoreSize];
    mii::toStoreData(m, store);
    for (u32 i = 0; i < mii::kStoreSize; i++) wr8(b.store + i, store[i]);
    call(c, s.initMiddleDb, b.mdb, 4, b.mdbBuf, 1);  // type 4 with one entry, as fn_8008B3A8
    const s32 add = s32(call(c, s.addStoreData, b.mdb, b.store));
    if (add != 0) {
        std::lock_guard lk(sh.m);
        sh.previewError = "RFL did not accept this Mii (RFLAddMiddleDBStoreData error " + std::to_string(add) + ")";
        sRenderedSeq = seq;
        return;
    }
    // Icon size: 256 px when RFL's heap can hold the model (RFLiMakeIcon allocates it there with
    // the resolution it picks for the size: 256 with the deluxe face textures, else 128; 64 up to 64 px).
    const u32 mgr = call(c, s.getManager);
    const u32 deluxe = mgr ? rd32(mgr + 0x1B38) : 0;
    const u32 heap = mgr ? rd32(mgr + 0xC) : 0;
    const u32 freeBytes = heap ? call(c, s.heapFree, heap, 32) : 0;
    u32 size = 0;
    for (u32 want : {256u, 128u, 64u}) {
        const u32 res = want > 128 ? (deluxe ? 256u : 128u) : want > 64 ? 128u : 64u;
        const u32 need = call(c, s.modelBufferSize, res, 1u << expr);
        if (need + 0x1000 <= freeBytes) {
            size = want;
            break;
        }
    }
    if (!size) {
        std::lock_guard lk(sh.m);
        sh.previewError = "RFL's memory is full right now (" + std::to_string(freeBytes) + " bytes free)";
        sRenderedSeq = seq;
        return;
    }
    // RFLIconSetting: width, height, background = direct colour, colour (0,0,0,0) = transparent,
    // opaque and translucent parts (as fn_8000A5D0).
    wr16(b.setting + 0, size);
    wr16(b.setting + 2, size);
    wr32(b.setting + 4, 1);
    wr32(b.setting + 8, 0);
    wr32(b.setting + 12, 0);
    const u32 tex = b.tex[slot];
    sRotYaw = yaw;
    sRotating = true;
    const s32 r = s32(call(c, s.makeIcon, tex, 7, b.mdb, 0, u32(expr), b.setting));
    sRotating = false;
    sRenderedSeq = seq;
    if (r != 0) {
        std::lock_guard lk(sh.m);
        sh.previewError = "RFLMakeIcon error " + std::to_string(r);
        return;
    }
    sTex[slot] = {seq, size, now, true, 0};
    sNextTex = slot ^ 1;
    {
        std::lock_guard lk(sh.m);
        sh.previewError.clear();
    }
    if (sLog) log("[mii] preview %u drawn by RFLMakeIcon: %u px into %08X (heap free %u, yaw %.0f, expression %d)", seq,
                  size, tex, freeBytes, double(yaw), expr);
}

// The game's Mii icon queue at the start of the stage draw; ours goes right after it.
void iconQueue(Ctx& c) {
    callOriginal(c, "fn_8000A57C");
    renderPreview(c);
}
static HleReg sIconQueueReg("fn_8000A57C", &iconQueue);

// RFLSetMtx(model, mtx): while our preview draws, the model turns about its vertical axis.
void setMtx(Ctx& c) {
    if (sRotating && sRotYaw != 0.0f) {
        const u32 src = c.r[4], dst = bufs().mtx;
        const float a = sRotYaw * 3.14159265f / 180.0f, cs = std::cos(a), sn = std::sin(a);
        for (u32 row = 0; row < 3; row++) {
            const u32 s = src + row * 16, d = dst + row * 16;
            const float m0 = rdf32(s), m1 = rdf32(s + 4), m2 = rdf32(s + 8), m3 = rdf32(s + 12);
            wrf32(d, m0 * cs - m2 * sn);
            wrf32(d + 4, m1);
            wrf32(d + 8, m0 * sn + m2 * cs);
            wrf32(d + 12, m3);
        }
        c.r[4] = dst;
    }
    callOriginal(c, "RFLSetMtx");
}
static HleReg sSetMtxReg("RFLSetMtx", &setMtx);

// RFL's database queries: who asks (CF_MII_LOG; the Shampoodle hint, below).
std::atomic<u32> sQueryFrame{0};
void noteQuery(Ctx& c, const char* what) {
    static u32 sLogged = 0;
    const bool harriet = relBase(kHarrietModule, 1) != 0;
    if (harriet) sQueryFrame = game::guestFrames();
    if (sLog && harriet && sLogged < 200) {
        sLogged++;
        log("[mii] %s(%08X, %08X) from %s at frame %u", what, c.r[3], c.r[4], relSymbolize(c.lr).c_str(), game::guestFrames());
    }
}
void isAvailable(Ctx& c) {
    noteQuery(c, "RFLIsAvailableOfficialData");
    callOriginal(c, "RFLIsAvailableOfficialData");
}
static HleReg sIsAvailableReg("RFLIsAvailableOfficialData", &isAvailable);
void searchOfficial(Ctx& c) {
    noteQuery(c, "RFLSearchOfficialData");
    callOriginal(c, "RFLSearchOfficialData");
}
static HleReg sSearchReg("RFLSearchOfficialData", &searchOfficial);
void additionalInfo(Ctx& c) {
    noteQuery(c, "RFLGetAdditionalInfo");
    callOriginal(c, "RFLGetAdditionalInfo");
}
static HleReg sAdditionalReg("RFLGetAdditionalInfo", &additionalInfo);

// ---- guest frame: defaults, database sync, hint -------------------------------------------------

void readDefaults(Ctx& c) {
    const Syms& s = syms();
    GuestBufs& b = bufs();
    std::vector<Mii> out;
    for (u32 i = 0; i < 6; i++) {
        call(c, s.defaultData, b.info, i);
        call(c, s.info2raw, b.info, b.raw);
        Mii m;
        for (u32 k = 0; k < mii::kRawSize; k++) m.raw[k] = u8(rd8(b.raw + k));
        out.push_back(m);
    }
    std::lock_guard lk(sh.m);
    sh.defaults = out;
    if (sLog) log("[mii] RFL's %zu default Miis read", out.size());
}

int countGuestMiis(u32 dbBuf) {
    int n = 0;
    for (int i = 0; i < mii::kMaxMiis; i++) {
        Mii m;
        for (u32 k = 0; k < mii::kRawSize; k++) m.raw[k] = u8(rd8(dbBuf + mii::kDbMiisOff + i * mii::kRawSize + k));
        n += !m.empty() && m.problem().empty();
    }
    return n;
}

}  // namespace

void miiOnGuestFrame(Ctx& c) {
    static bool sInit = false;
    if (!sInit) {
        sInit = true;
        efbcopy::addWritebackListener(&onWriteback);
    }
    const bool picker = relBase(kMiiMenuModule, 1) != 0, harriet = relBase(kHarrietModule, 1) != 0;
    std::vector<u8> image;
    u32 gen = 0;
    bool needDefaults;
    {
        std::lock_guard lk(sh.m);
        if (picker && !sh.pickerOpen && sLog) log("[mii] Mii picker (d_menu_miiNP) opened at frame %u", game::guestFrames());
        if (!picker && sh.pickerOpen && sLog) log("[mii] Mii picker closed at frame %u", game::guestFrames());
        sh.pickerOpen = picker;
        sh.harrietHere = harriet;
        needDefaults = sh.defaults.empty();
        if (sh.hasPending && !picker) {
            image = sh.pending;
            gen = sh.pendingGeneration;
        }
    }
    static u32 sCheck = 0;
    const bool countNow = (++sCheck % 30) == 0;
    if (!needDefaults && image.empty() && !countNow) return;
    RegSave save(c);
    const Syms& s = syms();
    if (call(c, s.available) == 0) return;
    if (needDefaults) readDefaults(c);
    if (call(c, s.isWorking) != 0) return;
    const u32 dbm = call(c, s.getDbManager);
    const u32 dbBuf = dbm ? rd32(dbm) : 0;
    if (!dbBuf) return;
    if (!image.empty() && image.size() >= mii::kDbMainSize) {
        for (size_t i = 0; i < mii::kDbMainSize; i++) wr8(dbBuf + u32(i), image[i]);
        // The file exists and is valid now: clear "not found" (bit 0), "needs repair" (1) and the
        // broken-file flags (2, 3) RFL may have set at boot.
        const u32 mgr = call(c, s.getManager), off = flagsOffset();
        u32 before = 0;
        if (mgr && off) {
            before = rd8(mgr + off);
            wr8(mgr + off, before & ~0x0Fu);
        }
        std::lock_guard lk(sh.m);
        if (sh.pendingGeneration == gen) {
            sh.hasPending = false;
            sh.pending.clear();
        }
        sh.syncedGeneration = gen;
        log("[mii] database handed to the game's RFL at frame %u (%d Miis; RFL flags %02X -> %02X)", game::guestFrames(),
            countGuestMiis(dbBuf), before, before & ~0x0Fu);
    }
    const int n = countGuestMiis(dbBuf);
    std::lock_guard lk(sh.m);
    // The hint: in Shampoodle (Harriet's module linked) or with the Mii picker open while the
    // console has no Miis. Harriet's makeover checks the game's own Mii list (built from RFL by
    // the game's Mii manager) and only says "there are no Miis", so the whole visit counts.
    if ((picker || harriet) && n == 0) {
        if (!sh.noticeAt || game::guestFrames() > sh.noticeAt + 60)
            log("[mii] %s with no Miis on the console: hint shown (frame %u)", picker ? "Mii picker open" : "in Shampoodle",
                game::guestFrames());
        sh.noticeAt = game::guestFrames();
    }
    if (n != sh.guestCount && sLog) log("[mii] the game's RFL has %d Miis", n);
    sh.guestCount = n;
}

namespace {

// ---- host: database, editor --------------------------------------------------------------------

struct Editor {
    bool loaded = false;
    mii::Database saved;   // what the file holds
    mii::Database edit;    // working copy
    bool fileExisted = false, crcOk = true;
    fs::file_time_type mtime{};
    int sel = -1;
    bool dirty = false;
    std::string status;
    bool statusError = false;
    float yaw = 0.0f;
    int expr = 0;
    std::string importPath;  // waiting for merge / replace
    mii::Database importDb;
    int importCount = 0;
    bool importAsk = false;
    ImTextureID tex = 0;
    u32 shownSeq = 0, shownSize = 0;
    u32 reqSeq = 0;
    u8 lastReq[mii::kRawSize] = {};
    float lastYaw = -1000.0f;
    int lastExpr = -1;
    u32 systemId = 0;
    u32 lastDrawFrame = 0;
} ed;

std::mutex sPickM;
std::string sPickedImport, sPickedExport, sPickedExportDb, sDialogError;

fs::path dbHostPath() { return ios::hostPath(kDbPath); }

std::string u8path(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

void setStatus(const std::string& s, bool error = false) {
    ed.status = s;
    ed.statusError = error;
    log("[mii] %s", s.c_str());
}

void loadDb() {
    ed.loaded = true;
    std::vector<u8> bytes;
    std::error_code ec;
    const fs::path p = dbHostPath();
    ed.fileExisted = ios::readHostFile(kDbPath, bytes);
    ed.mtime = fs::last_write_time(p, ec);
    ed.crcOk = true;
    if (!ed.fileExisted) {
        ed.saved.format();
    } else {
        std::string err;
        if (!ed.saved.parse(bytes, ed.crcOk, err)) {
            setStatus("The Mii database on this console is damaged (" + err + "); saving makes a new one.", true);
            ed.saved.format();
        } else if (!ed.crcOk) {
            setStatus("The Mii database's checksum is wrong (the game treats it as broken); saving repairs it.", true);
        }
    }
    ed.edit = ed.saved;
    ed.dirty = false;
    if (ed.sel < 0 || ed.edit.slots[ed.sel].empty()) {
        ed.sel = -1;
        for (int i = 0; i < mii::kMaxMiis; i++)
            if (!ed.edit.slots[i].empty()) {
                ed.sel = i;
                break;
            }
    }
    log("[mii] database %s: %s, %d Miis", u8path(p).c_str(), ed.fileExisted ? (ed.crcOk ? "read" : "read (bad CRC)") : "absent",
        ed.saved.count());
}

bool saveDb() {
    for (int i = 0; i < mii::kMaxMiis; i++) {
        Mii& m = ed.edit.slots[i];
        if (m.empty()) continue;
        if (m.name().empty()) m.setName(u"Mii");
        if (const std::string p = m.problem(); !p.empty()) {
            setStatus("Not saved: \"" + mii::toUtf8(m.name()) + "\" has " + p + ".", true);
            ed.sel = i;
            return false;
        }
    }
    const std::vector<u8>& image = ed.edit.encode();
    std::string err;
    if (!ios::writeHostFile(kDbPath, image, err)) {
        setStatus("Not saved: " + err + ". Try again in a moment.", true);
        return false;
    }
    std::error_code ec;
    ed.mtime = fs::last_write_time(dbHostPath(), ec);
    ed.saved = ed.edit;
    ed.fileExisted = true;
    ed.crcOk = true;
    ed.dirty = false;
    {
        std::lock_guard lk(sh.m);
        sh.pending = image;
        sh.hasPending = true;
        sh.pendingGeneration++;
    }
    setStatus("Saved " + std::to_string(ed.saved.count()) + " Miis to the console's Mii database.");
    return true;
}

// ---- console system ID (settings.ini [mii] system_id) ----------------------------------------

u32 sSystemId = 0;
void resetConfig() { sSystemId = 0; }
void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    if (section == "mii" && key == "system_id") sSystemId = u32(std::strtoul(value.c_str(), nullptr, 16));
}
void saveConfig(std::ostream& out) {
    if (!sSystemId) return;
    char b[16];
    std::snprintf(b, sizeof b, "%08X", sSystemId);
    out << "\n[mii]\nsystem_id=" << b << "\n";
}
const bool sReg = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

bool sSettingsDirty = false;
u32 systemId() {
    if (!sSystemId) {
        // The console's own ID for the Miis made here (on a Wii: a checksum and the last three
        // bytes of its MAC address). RFL only needs it to be non-zero and stable.
        std::random_device rd;
        sSystemId = (rd() & 0xFFFFFFu) | 0x80000000u;
        sSettingsDirty = true;
    }
    return sSystemId;
}

u32 uniqueMiiId() {
    u32 id = mii::newMiiId(std::time(nullptr));
    for (;;) {
        bool clash = false;
        for (const Mii& m : ed.edit.slots) clash |= !m.empty() && m.miiId() == id;
        if (!clash) return id;
        id = 0x80000000u | ((id + 1) & 0x1FFFFFFFu);
    }
}

Mii baseMii(int gender) {
    std::vector<Mii> defs;
    {
        std::lock_guard lk(sh.m);
        defs = sh.defaults;
    }
    for (const Mii& d : defs)
        if (d.get(F::Gender) == gender) {
            Mii m = d;
            m.set(F::Favorite, 0);
            return m;
        }
    // Before the game has started RFL: a plain face of our own.
    Mii m;
    const std::pair<F, int> kBase[] = {
        {F::Gender, gender}, {F::Height, 64}, {F::Weight, 64}, {F::HairStyle, gender ? 12 : 33}, {F::HairColor, 1},
        {F::BrowType, 6}, {F::BrowRotation, 6}, {F::BrowColor, 1}, {F::BrowSize, 4}, {F::BrowY, 10}, {F::BrowX, 2},
        {F::EyeType, 2}, {F::EyeRotation, 4}, {F::EyeY, 12}, {F::EyeSize, 4}, {F::EyeX, 2}, {F::NoseType, 1},
        {F::NoseSize, 4}, {F::NoseY, 9}, {F::MouthType, 23}, {F::MouthSize, 4}, {F::MouthY, 13}, {F::GlassesSize, 4},
        {F::GlassesY, 10}, {F::MustacheSize, 4}, {F::MustacheY, 10}, {F::MoleSize, 4}, {F::MoleY, 20}, {F::MoleX, 2},
    };
    for (auto [f, v] : kBase) m.set(f, v);
    return m;
}

std::mt19937& rng() {
    static std::mt19937 r = [] {
        const char* s = std::getenv("CF_MII_SEED");
        return std::mt19937(s ? u32(std::strtoul(s, nullptr, 10)) : std::random_device{}());
    }();
    return r;
}
int rnd(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng()); }

Mii randomMii() {
    const int gender = rnd(0, 1);
    Mii m = baseMii(gender);
    // Shapes and colours anywhere in their range; sizes and positions near the default face so
    // the result stays a plausible face.
    const auto near = [&](F f, int spread) {
        const auto& fi = mii::fieldInfo(f);
        m.set(f, std::clamp(m.get(f) + rnd(-spread, spread), int(fi.min), int(fi.max)));
    };
    for (F f : {F::FaceShape, F::SkinTone, F::HairStyle, F::HairColor, F::BrowType, F::BrowColor, F::EyeType, F::EyeColor,
                F::NoseType, F::MouthType, F::MouthColor, F::FavColor})
        m.set(f, rnd(mii::fieldInfo(f).min, mii::fieldInfo(f).max));
    m.set(F::FaceFeature, rnd(0, 3) == 0 ? rnd(1, 11) : 0);
    m.set(F::HairFlip, rnd(0, 1));
    m.set(F::GlassesType, rnd(0, 3) == 0 ? rnd(1, 8) : 0);
    m.set(F::GlassesColor, rnd(0, 5));
    if (gender == 0 && rnd(0, 3) == 0) {
        m.set(F::MustacheType, rnd(0, 3));
        m.set(F::BeardType, rnd(0, 3));
    }
    m.set(F::FacialHairColor, m.get(F::HairColor));
    m.set(F::MoleOn, rnd(0, 7) == 0);
    for (auto [f, sp] : {std::pair{F::BrowSize, 1}, {F::BrowY, 2}, {F::BrowX, 1}, {F::BrowRotation, 2}, {F::EyeSize, 1},
                         {F::EyeY, 2}, {F::EyeX, 1}, {F::EyeRotation, 1}, {F::NoseSize, 1}, {F::NoseY, 2},
                         {F::MouthSize, 1}, {F::MouthY, 2}, {F::MoleY, 6}, {F::MoleX, 4}})
        near(f, sp);
    m.set(F::Height, rnd(20, 108));
    m.set(F::Weight, rnd(20, 108));
    static const char16_t* const kNames[] = {u"Alex", u"Sam", u"Robin", u"Kai", u"Jo", u"Mika", u"Nico", u"Ari",
                                             u"Rae", u"Toni", u"Lou", u"Pat", u"Remy", u"Sky", u"Wren", u"Quinn"};
    m.setName(kNames[rnd(0, int(std::size(kNames)) - 1)]);
    return m;
}

int addMii(const Mii& src, bool newId) {
    const int slot = ed.edit.firstFree();
    if (slot < 0) {
        setStatus("The console holds 100 Miis already: delete one first.", true);
        return -1;
    }
    Mii m = src;
    if (newId) {
        m.setIds(uniqueMiiId(), systemId());
        m.set(F::Downloaded, 0);
        m.set(F::Favorite, 0);
    }
    ed.edit.slots[slot] = m;
    ed.sel = slot;
    ed.dirty = true;
    return slot;
}

std::vector<u8> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeFile(const fs::path& p, const u8* data, size_t n) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), std::streamsize(n));
    return bool(out);
}

void importFile(const std::string& path, const char* mode /* nullptr = ask */) {
    const fs::path p = fs::path(std::u8string(path.begin(), path.end()));
    const std::vector<u8> bytes = readFile(p);
    if (bytes.empty()) {
        setStatus("Could not read " + path + ".", true);
        return;
    }
    if (bytes.size() >= mii::kDbMainSize) {
        mii::Database db;
        bool crcOk = true;
        std::string err;
        if (!db.parse(bytes, crcOk, err)) {
            setStatus("Not imported: " + err + ".", true);
            return;
        }
        int n = 0;
        for (const Mii& m : db.slots) n += !m.empty() && m.problem().empty();
        ed.importDb = db;
        ed.importCount = n;
        ed.importPath = path;
        if (!mode) {
            ed.importAsk = true;
            return;
        }
        const bool replace = std::strcmp(mode, "replace") == 0;
        if (replace) {
            for (Mii& m : ed.edit.slots) m = Mii{};
            for (int i = 0; i < mii::kMaxMiis; i++)
                if (!db.slots[i].empty() && db.slots[i].problem().empty()) ed.edit.slots[i] = db.slots[i];
            ed.dirty = true;
            ed.sel = -1;
            setStatus("Replaced the Miis with the " + std::to_string(n) + " from " + port::displayPath(p.filename()) +
                      " (not saved yet).");
        } else {
            int added = 0, same = 0, full = 0;
            for (const Mii& m : db.slots) {
                if (m.empty() || !m.problem().empty()) continue;
                if (ed.edit.findId(m) >= 0) {
                    same++;
                    continue;
                }
                const int slot = ed.edit.firstFree();
                if (slot < 0) {
                    full++;
                    continue;
                }
                ed.edit.slots[slot] = m;
                added++;
            }
            ed.dirty |= added > 0;
            setStatus("Added " + std::to_string(added) + " Miis from " + port::displayPath(p.filename()) +
                      (same ? " (" + std::to_string(same) + " already here)" : "") +
                      (full ? "; " + std::to_string(full) + " did not fit (100 at most)" : "") + " (not saved yet).",
                      full > 0);
        }
        return;
    }
    Mii m;
    std::string err;
    if (!mii::parseMiiFile(bytes, m, err)) {
        setStatus("Not imported: " + err + ".", true);
        return;
    }
    if (const int at = ed.edit.findId(m); at >= 0) {
        ed.edit.slots[at] = m;
        ed.sel = at;
        ed.dirty = true;
        setStatus("\"" + mii::toUtf8(m.name()) + "\" was here already: replaced it with the file's version (not saved yet).");
        return;
    }
    if (addMii(m, false) >= 0) setStatus("Imported \"" + mii::toUtf8(m.name()) + "\" (not saved yet).");
}

void exportMii(int slot, const std::string& path) {
    if (slot < 0 || ed.edit.slots[slot].empty()) return;
    const fs::path p = fs::path(std::u8string(path.begin(), path.end()));
    Mii m = ed.edit.slots[slot];
    if (writeFile(p, m.raw, mii::kRawSize)) setStatus("Exported \"" + mii::toUtf8(m.name()) + "\" to " + path + ".");
    else setStatus("Could not write " + path + ".", true);
}

void exportDb(const std::string& path) {
    const fs::path p = fs::path(std::u8string(path.begin(), path.end()));
    mii::Database db = ed.edit;
    const auto& img = db.encode();
    if (writeFile(p, img.data(), img.size())) setStatus("Exported the Mii database (" + std::to_string(db.count()) + " Miis) to " + path + ".");
    else setStatus("Could not write " + path + ".", true);
}

void SDLCALL onPickImport(void*, const char* const* files, int) {
    std::lock_guard lk(sPickM);
    if (!files) sDialogError = SDL_GetError();
    else if (files[0]) sPickedImport = files[0];
}
void SDLCALL onPickExport(void*, const char* const* files, int) {
    std::lock_guard lk(sPickM);
    if (!files) sDialogError = SDL_GetError();
    else if (files[0]) sPickedExport = files[0];
}
void SDLCALL onPickExportDb(void*, const char* const* files, int) {
    std::lock_guard lk(sPickM);
    if (!files) sDialogError = SDL_GetError();
    else if (files[0]) sPickedExportDb = files[0];
}

std::string safeFileName(const std::u16string& name) {
    std::string s;
    for (char ch : mii::toUtf8(name))
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '-' || ch == '_') s += ch;
    return s.empty() ? "Mii" : s;
}

// ---- preview (host side) -----------------------------------------------------------------------

void requestPreview(const Mii& m) {
    Mii p = m;
    if (p.name().empty()) p.setName(u"?");
    if (p.empty()) p.setIds(0x80000001u, systemId());
    const u32 frame = game::guestFrames();
    if (std::memcmp(p.raw, ed.lastReq, sizeof ed.lastReq) != 0 || ed.yaw != ed.lastYaw || ed.expr != ed.lastExpr) {
        std::memcpy(ed.lastReq, p.raw, sizeof ed.lastReq);
        ed.lastYaw = ed.yaw;
        ed.lastExpr = ed.expr;
        ed.reqSeq++;
    }
    std::lock_guard lk(sh.m);
    std::memcpy(sh.reqRaw, p.raw, sizeof sh.reqRaw);
    sh.reqYaw = ed.yaw;
    sh.reqExpr = ed.expr;
    sh.reqSeq = ed.reqSeq;
    sh.wanted = true;
    sh.wantedUntil = frame + 120;
}

void uploadPreview() {
    std::lock_guard lk(sh.m);
    if (sh.outSeq == ed.shownSeq) return;
    if (!ed.tex) ed.tex = aurora_imgui_add_texture(kTexMax, kTexMax, sh.rgba.data());
    else aurora_imgui_update_texture(ed.tex, kTexMax, kTexMax, sh.rgba.data());
    ed.shownSeq = sh.outSeq;
    ed.shownSize = sh.outSize;
}

void writePreviewPpm(const std::string& path) {
    std::lock_guard lk(sh.m);
    const u32 n = sh.outSize;
    if (!n) {
        log("[mii] shot %s: no preview yet", path.c_str());
        return;
    }
    std::ofstream out(fs::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    out << "P6\n" << n << " " << n << "\n255\n";
    // Composited over the menu's dark grey so transparent pixels show.
    for (u32 y = 0; y < n; y++)
        for (u32 x = 0; x < n; x++) {
            const u8* p = &sh.rgba[(size_t(y) * kTexMax + x) * 4];
            const int a = p[3];
            for (int k = 0; k < 3; k++) out.put(char((p[k] * a + 40 * (255 - a)) / 255));
        }
    log("[mii] shot %s (%u px, preview %u)", path.c_str(), n, sh.outSeq);
}

// ---- the page ----------------------------------------------------------------------------------

const char* const kFavColors[] = {"Red", "Orange", "Yellow", "Light green", "Green", "Blue",
                                  "Light blue", "Pink", "Purple", "Brown", "White", "Black"};
const char* const kMonths[] = {"(not set)", "January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December"};
const char* const kExpressions[] = {"Normal", "Smile", "Angry", "Sad", "Surprised", "Blink"};

bool slider(const char* label, Mii& m, F f, const char* tip = nullptr, int shownOffset = 1) {
    const auto& fi = mii::fieldInfo(f);
    int v = m.get(f) + shownOffset;
    char fmt[24];
    std::snprintf(fmt, sizeof fmt, "%%d / %d", fi.max + shownOffset);
    const bool changed = menu::SliderInt(label, &v, fi.min + shownOffset, fi.max + shownOffset, fmt);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    if (changed) m.set(f, v - shownOffset);
    return changed;
}
bool position(const char* label, Mii& m, F f, const char* tip) {
    // Positions and sizes start at 0 in the data; shown as stored.
    return slider(label, m, f, tip, 0);
}
bool check(const char* label, Mii& m, F f, bool inverted = false) {
    bool v = (m.get(f) != 0) != inverted;
    const bool changed = menu::Checkbox(label, &v);
    if (changed) m.set(f, v != inverted ? 1 : 0);
    return changed;
}
bool nameField(const char* label, Mii& m, bool creator) {
    char buf[64];
    const std::string cur = mii::toUtf8(creator ? m.creator() : m.name());
    std::snprintf(buf, sizeof buf, "%s", cur.c_str());
    if (!ImGui::InputText(label, buf, sizeof buf)) return false;
    const std::u16string s = mii::fromUtf8(buf, 10);
    if (creator) m.setCreator(s);
    else m.setName(s);
    return true;
}

bool editAttributes(Mii& m) {
    bool ch = false;
    if (!ImGui::BeginTabBar("##miiparts", ImGuiTabBarFlags_FittingPolicyScroll)) return false;
    if (ImGui::BeginTabItem("Profile")) {
        ch |= nameField("Name", m, false);
        ImGui::SetItemTooltip("Up to 10 characters. A Mii needs a name.");
        ch |= nameField("Creator", m, true);
        int g = m.get(F::Gender);
        if (ImGui::RadioButton("Male", g == 0)) g = 0, ch = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Female", g == 1)) g = 1, ch = true;
        m.set(F::Gender, g);
        int month = m.get(F::BirthMonth);
        if (ImGui::Combo("Birthday month", &month, kMonths, 13)) {
            m.set(F::BirthMonth, month);
            m.set(F::BirthDay, month ? std::max(1, m.get(F::BirthDay)) : 0);
            ch = true;
        }
        if (month) {
            static const int kDays[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            int day = std::min(m.get(F::BirthDay), kDays[month]);
            if (menu::SliderInt("Birthday day", &day, 1, kDays[month])) ch = true;
            m.set(F::BirthDay, day);
        }
        int fav = m.get(F::FavColor);
        if (ImGui::Combo("Favourite colour", &fav, kFavColors, 12)) m.set(F::FavColor, fav), ch = true;
        ch |= check("Favourite Mii", m, F::Favorite);
        ImGui::SetItemTooltip("The Mii Channel's \"favourite\" mark (shown with a star there).");
        ch |= position("Height", m, F::Height, "0 = shortest, 127 = tallest.");
        ch |= position("Weight", m, F::Weight, "0 = thinnest, 127 = widest.");
        ch |= check("Mingle", m, F::NoMingle, true);
        ImGui::SetItemTooltip("Allowed to travel to other consoles (the Mii Channel's Mingle setting).");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Face")) {
        ch |= slider("Face shape", m, F::FaceShape);
        ch |= slider("Skin tone", m, F::SkinTone);
        ch |= slider("Features / makeup", m, F::FaceFeature, "1 = none; freckles, wrinkles, blush, makeup...");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Hair")) {
        ch |= slider("Hairstyle", m, F::HairStyle);
        ch |= slider("Hair colour", m, F::HairColor);
        ch |= check("Part on the other side", m, F::HairFlip);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Eyebrows")) {
        ch |= slider("Eyebrow type", m, F::BrowType);
        ch |= slider("Eyebrow colour", m, F::BrowColor);
        ch |= position("Eyebrow size", m, F::BrowSize, "0 = smallest.");
        ch |= position("Eyebrow rotation", m, F::BrowRotation, "Tilt.");
        ch |= position("Eyebrow height", m, F::BrowY, "Lower numbers are higher on the face.");
        ch |= position("Eyebrow spacing", m, F::BrowX, "Distance between the eyebrows.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Eyes")) {
        ch |= slider("Eye type", m, F::EyeType);
        ch |= slider("Eye colour", m, F::EyeColor);
        ch |= position("Eye size", m, F::EyeSize, "0 = smallest.");
        ch |= position("Eye rotation", m, F::EyeRotation, "Tilt.");
        ch |= position("Eye height", m, F::EyeY, "Lower numbers are higher on the face.");
        ch |= position("Eye spacing", m, F::EyeX, "Distance between the eyes.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Nose")) {
        ch |= slider("Nose type", m, F::NoseType);
        ch |= position("Nose size", m, F::NoseSize, "0 = smallest.");
        ch |= position("Nose height", m, F::NoseY, "Lower numbers are higher on the face.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Mouth")) {
        ch |= slider("Mouth type", m, F::MouthType);
        ch |= slider("Lip colour", m, F::MouthColor);
        ch |= position("Mouth size", m, F::MouthSize, "0 = smallest.");
        ch |= position("Mouth height", m, F::MouthY, "Lower numbers are higher on the face.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Facial hair")) {
        ch |= slider("Mustache", m, F::MustacheType, "1 = none.");
        ch |= slider("Beard", m, F::BeardType, "1 = none.");
        ch |= slider("Facial hair colour", m, F::FacialHairColor);
        ch |= position("Mustache size", m, F::MustacheSize, "0 = smallest.");
        ch |= position("Mustache height", m, F::MustacheY, "Lower numbers are higher on the face.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Glasses")) {
        ch |= slider("Glasses", m, F::GlassesType, "1 = none.");
        ch |= slider("Glasses colour", m, F::GlassesColor);
        ch |= position("Glasses size", m, F::GlassesSize, "0 = smallest.");
        ch |= position("Glasses height", m, F::GlassesY, "Lower numbers are higher on the face.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Mole")) {
        ch |= check("Mole", m, F::MoleOn);
        ch |= position("Mole size", m, F::MoleSize, "0 = smallest.");
        ch |= position("Mole height", m, F::MoleY, "Lower numbers are higher on the face.");
        ch |= position("Mole sideways", m, F::MoleX, "0 = the middle of the face.");
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    return ch;
}

void drawPreview(const Mii& m) {
    requestPreview(m);
    uploadPreview();
    const float s = 200.0f * ui::scale();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + s, p0.y + s), IM_COL32(40, 40, 40, 255), 6.0f);
    if (ed.tex && ed.shownSize) {
        const float uv = float(ed.shownSize) / float(kTexMax);
        ImGui::Image(ed.tex, ImVec2(s, s), ImVec2(0, 0), ImVec2(uv, uv));
    } else {
        ImGui::Dummy(ImVec2(s, s));
    }
    // Drag to turn the head.
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##miiview", ImVec2(s, s));
    if (ImGui::IsItemActive()) {
        ed.yaw += ImGui::GetIO().MouseDelta.x * 0.8f;
        ed.yaw = std::fmod(ed.yaw + 540.0f, 360.0f) - 180.0f;
    }
    ImGui::SetItemTooltip("Drag to turn the head. Drawn by the game's own Mii library with the disc's Mii parts.");
    std::string err;
    u32 lastStage;
    {
        std::lock_guard lk(sh.m);
        err = sh.previewError;
        lastStage = sh.lastStageFrame;
    }
    const u32 now = game::guestFrames();
    const char* hint = nullptr;
    if (!err.empty()) hint = err.c_str();
    else if (!lastStage || now > lastStage + 30) hint = "The preview appears once the game shows a scene (title town, in town, inside).";
    if (hint) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + s);
        ImGui::TextDisabled("%s", hint);
        ImGui::PopTextWrapPos();
    }
    ImGui::SetNextItemWidth(s * 0.62f);
    ImGui::Combo("##expr", &ed.expr, kExpressions, int(std::size(kExpressions)));
    ImGui::SetItemTooltip("Expression in the preview (not stored: the game picks the mask's expression).");
    ImGui::SameLine();
    if (menu::SmallButton("Face front")) ed.yaw = 0.0f;
}

}  // namespace

bool miiMenu() {
    if (!ed.loaded) loadDb();
    ed.lastDrawFrame = game::guestFrames();
    // A file changed outside the editor (copied in while the game runs): reload when nothing is
    // being edited.
    {
        std::error_code ec;
        const auto t = fs::last_write_time(dbHostPath(), ec);
        if (!ec && t != ed.mtime && !ed.dirty) {
            loadDb();
            setStatus("The Mii database file changed on disk: reloaded.");
        }
    }
    {
        std::string imp, exp, expDb, derr;
        {
            std::lock_guard lk(sPickM);
            std::swap(imp, sPickedImport);
            std::swap(exp, sPickedExport);
            std::swap(expDb, sPickedExportDb);
            std::swap(derr, sDialogError);
        }
        if (!imp.empty()) importFile(imp, nullptr);
        if (!exp.empty()) exportMii(ed.sel, exp);
        if (!expDb.empty()) exportDb(expDb);
        if (!derr.empty()) setStatus("The file dialog could not be opened: " + derr, true);
    }

    ImGui::TextWrapped("The Miis of this console's Mii database, for Shampoodle's Mii masks. Saving updates the database "
                       "in the data folder and hands it to the game (when no Mii list is open there).");
    const int count = ed.edit.count();

    // Left: the list and its buttons.
    const float listW = 190.0f * ui::scale();
    ImGui::BeginChild("##miilist", ImVec2(listW, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Miis: %d / 100", count);
    ImGui::Separator();
    for (int i = 0; i < mii::kMaxMiis; i++) {
        const Mii& m = ed.edit.slots[i];
        if (m.empty()) continue;
        std::string label = mii::toUtf8(m.name());
        if (label.empty()) label = "(no name)";
        if (m.get(F::Favorite)) label += " *";
        const bool changed = std::memcmp(m.raw, ed.saved.slots[i].raw, mii::kRawSize) != 0;
        if (changed) label += " (edited)";
        label += "##slot" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), ed.sel == i)) ed.sel = i;
    }
    if (!count) ImGui::TextDisabled("No Miis yet.");
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginGroup();
    // Buttons row.
    ImGui::BeginDisabled(count >= mii::kMaxMiis);
    if (menu::Button("New Mii")) ImGui::OpenPopup("##newmii");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("A new Mii starting from RFL's plain face (or a random one).");
    if (ImGui::BeginPopup("##newmii")) {
        if (ImGui::MenuItem("Male")) {
            Mii m = baseMii(0);
            m.setName(u"New Mii");
            if (addMii(m, true) >= 0) setStatus("New Mii added (not saved yet).");
        }
        if (ImGui::MenuItem("Female")) {
            Mii m = baseMii(1);
            m.setName(u"New Mii");
            if (addMii(m, true) >= 0) setStatus("New Mii added (not saved yet).");
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(count >= mii::kMaxMiis);
    if (menu::Button("Random")) {
        if (addMii(randomMii(), true) >= 0) setStatus("Random Mii added (not saved yet).");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Adds a Mii with random parts.");
    const bool has = ed.sel >= 0 && !ed.edit.slots[ed.sel].empty();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has || count >= mii::kMaxMiis);
    if (menu::Button("Duplicate")) {
        Mii m = ed.edit.slots[ed.sel];
        if (addMii(m, true) >= 0) setStatus("Copied as a new Mii (not saved yet).");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("A copy with its own Mii ID (a new Mii, not the same one twice).");
    ImGui::SameLine();
    ImGui::BeginDisabled(!has);
    if (menu::Button("Delete")) ImGui::OpenPopup("Delete Mii?");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Delete Mii?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"? (Takes effect when you save.)", has ? mii::toUtf8(ed.edit.slots[ed.sel].name()).c_str() : "");
        ImGui::TextDisabled("Mii masks the player already has keep their own copy.");
        if (menu::Button("Delete##confirm")) {
            if (has) ed.edit.slots[ed.sel] = Mii{};
            ed.dirty = true;
            ed.sel = -1;
            setStatus("Mii deleted (not saved yet).");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (menu::Button("Cancel##delete")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (menu::Button("Import...")) {
        static const SDL_DialogFileFilter kFilters[] = {
            {"Wii Miis and Mii databases", "mii;miigx;rsd;dat"},
            {"All files", "*"},
        };
        SDL_ShowOpenFileDialog(&onPickImport, nullptr, aurora_get_window(), kFilters, 2, nullptr, false);
    }
    ImGui::SetItemTooltip("A Wii Mii (.mii / .miigx: 74 bytes; .rsd) or a whole Mii database (RFL_DB.dat from a\n"
                          "Wii NAND dump or Dolphin's Wii folder: shared2\\menu\\FaceLib\\RFL_DB.dat).");
    ImGui::SameLine();
    ImGui::BeginDisabled(!has);
    if (menu::Button("Export...")) {
        static const SDL_DialogFileFilter kFilters[] = {{"Wii Mii", "mii"}};
        const std::string def = safeFileName(ed.edit.slots[ed.sel].name()) + ".mii";
        SDL_ShowSaveFileDialog(&onPickExport, nullptr, aurora_get_window(), kFilters, 1, def.c_str());
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Saves the selected Mii as a .mii file (the 74-byte Wii Mii record).");
    ImGui::SameLine();
    if (menu::Button("Export all...")) {
        static const SDL_DialogFileFilter kFilters[] = {{"Wii Mii database", "dat"}};
        SDL_ShowSaveFileDialog(&onPickExportDb, nullptr, aurora_get_window(), kFilters, 1, "RFL_DB.dat");
    }
    ImGui::SetItemTooltip("Saves every Mii (also unsaved edits) as an RFL_DB.dat, e.g. for Dolphin or a Wii.");

    if (ed.importAsk) {
        ImGui::OpenPopup("Import Mii database");
        ed.importAsk = false;
    }
    if (ImGui::BeginPopupModal("Import Mii database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s holds %d Miis.", ed.importPath.c_str(), ed.importCount);
        if (menu::Button("Add the new ones")) {
            importFile(ed.importPath, "merge");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("Keeps the Miis here and adds the file's Miis that are not here yet (same Mii ID).");
        ImGui::SameLine();
        if (menu::Button("Replace all")) {
            importFile(ed.importPath, "replace");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("The console's Miis become exactly the file's.");
        ImGui::SameLine();
        if (menu::Button("Cancel##import")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (has) {
        Mii& m = ed.edit.slots[ed.sel];
        ImGui::BeginGroup();
        drawPreview(m);
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        if (editAttributes(m)) ed.dirty = true;
        ImGui::PopItemWidth();
        ImGui::TextDisabled("Mii ID %08X, console %08X%s", m.miiId(), m.systemId(),
                            m.get(F::Downloaded) ? " (from another console)" : "");
        ImGui::EndGroup();
    } else {
        ImGui::TextDisabled(count ? "Pick a Mii on the left." : "Make a Mii with New Mii or Random, or import one.");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!ed.dirty);
    if (menu::Button("Save to console")) saveDb();
    ImGui::SetItemTooltip("Writes the console's Mii database (RFL_DB.dat in the data folder's NAND) and gives it to the game.");
    ImGui::SameLine();
    if (menu::Button("Revert")) {
        ed.edit = ed.saved;
        ed.dirty = false;
        if (ed.sel >= 0 && ed.edit.slots[ed.sel].empty()) ed.sel = -1;
        setStatus("Changes thrown away.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        bool pend, picker;
        {
            std::lock_guard lk(sh.m);
            pend = sh.hasPending;
            picker = sh.pickerOpen;
        }
        if (ed.dirty) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Unsaved changes");
        else if (pend) ImGui::TextDisabled(picker ? "Saved; the game gets it once its Mii list closes." : "Saved; handing it to the game...");
        else if (ed.fileExisted) ImGui::TextDisabled("Saved.");
    }
    if (!ed.status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ed.statusError ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.6f, 0.85f, 0.6f, 1.0f));
        ImGui::TextWrapped("%s", ed.status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    const bool d = sSettingsDirty;
    sSettingsDirty = false;
    return d;
}

namespace {

// CF_MII_AT test aid: editor commands at guest frames (host thread).
void runScript() {
    static const char* sScript = std::getenv("CF_MII_AT");
    static size_t sPos = 0;
    if (!sScript) return;
    const u32 frame = game::guestFrames();
    while (sScript[sPos]) {
        const char* p = sScript + sPos;
        char* end = nullptr;
        const u32 at = u32(std::strtoul(p, &end, 10));
        if (!end || *end != ':') {
            sScript = nullptr;
            return;
        }
        if (frame < at) return;
        const char* cmdStart = end + 1;
        const char* semi = std::strpbrk(cmdStart, ";|");
        const std::string cmd(cmdStart, semi ? size_t(semi - cmdStart) : std::strlen(cmdStart));
        sPos = semi ? size_t(semi - sScript) + 1 : std::strlen(sScript);
        if (!ed.loaded) loadDb();
        log("[mii] CF_MII_AT frame %u: %s", frame, cmd.c_str());
        const auto arg = [&](const char* name) -> std::string {
            const size_t n = std::strlen(name);
            return cmd.compare(0, n, name) == 0 ? cmd.substr(n) : std::string("\x01");
        };
        std::string a;
        if (cmd == "open") menu::openPage("Miis");
        else if (cmd == "new") {
            Mii m = baseMii(0);
            m.setName(u"New Mii");
            addMii(m, true);
        } else if (cmd == "random") addMii(randomMii(), true);
        else if (cmd == "save") saveDb();
        else if (cmd == "delete") {
            if (ed.sel >= 0) ed.edit.slots[ed.sel] = Mii{}, ed.dirty = true, ed.sel = -1;
        } else if (cmd == "duplicate") {
            if (ed.sel >= 0) addMii(ed.edit.slots[ed.sel], true);
        } else if ((a = arg("name=")) != "\x01") {
            if (ed.sel >= 0) ed.edit.slots[ed.sel].setName(mii::fromUtf8(a, 10)), ed.dirty = true;
        } else if ((a = arg("select=")) != "\x01") ed.sel = std::atoi(a.c_str());
        else if ((a = arg("yaw=")) != "\x01") ed.yaw = float(std::atof(a.c_str()));
        else if ((a = arg("expr=")) != "\x01") ed.expr = std::clamp(std::atoi(a.c_str()), 0, 5);
        else if ((a = arg("import=")) != "\x01") {
            const size_t comma = a.rfind(',');
            const std::string mode = comma == std::string::npos ? "merge" : a.substr(comma + 1);
            importFile(comma == std::string::npos ? a : a.substr(0, comma), mode.c_str());
        } else if ((a = arg("export=")) != "\x01") exportMii(ed.sel, a);
        else if ((a = arg("exportdb=")) != "\x01") exportDb(a);
        else if ((a = arg("shot=")) != "\x01") writePreviewPpm(a);
        else if ((a = arg("set=")) != "\x01") {
            // set=<field index>=<value>
            const size_t eq = a.find('=');
            if (ed.sel >= 0 && eq != std::string::npos) {
                ed.edit.slots[ed.sel].set(F(std::clamp(std::atoi(a.c_str()), 0, int(F::Count) - 1)), std::atoi(a.c_str() + eq + 1));
                ed.dirty = true;
            }
        } else log("[mii] CF_MII_AT: unknown command %s", cmd.c_str());
    }
}

}  // namespace

void miiDrawOverlay() {
    runScript();
    // Keep the preview going while a CF_MII_AT script runs with the page closed.
    if (std::getenv("CF_MII_AT") && ed.loaded && ed.sel >= 0 && !ed.edit.slots[ed.sel].empty() &&
        game::guestFrames() > ed.lastDrawFrame + 2) {
        requestPreview(ed.edit.slots[ed.sel]);
        uploadPreview();
    }
    // The Shampoodle hint: while in Shampoodle (or the Mii picker is open) with no Miis on the
    // console, then fading out over a second.
    u32 at;
    int count;
    {
        std::lock_guard lk(sh.m);
        at = sh.noticeAt;
        count = sh.guestCount;
    }
    if (!at || count != 0) return;
    const u32 frames = game::guestFrames() - at;  // 0 while the picker is open
    constexpr u32 kShow = 90;  // the count is refreshed every 30 frames
    if (frames > kShow || menu::isOpen()) return;
    const float alpha = frames > kShow - 60 ? float(kShow - frames) / 60.0f : 1.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 16.0f * ui::scale()), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##mii_hint", nullptr, kFlags)) {
        ImGui::TextUnformatted("Harriet's makeovers need a Mii, and this console has none yet.");
        ImGui::TextDisabled("Make one in F1 > Miis (or import yours) and save, then leave the shop and come back.");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace recomp::cf
