// WiiConnect24 download side (nwc24_dl.h; docs/DISTRIBUTIONS.md "How delivery works"). Written
// from the public descriptions of KD, nwc24dl.bin, nwc24msg.cfg, VFF and the WC24 container, and
// from the game's own NWC24 library code (which checks every field we write); no emulator code.
#include "recomp/nwc24_dl.h"

#include "recomp/distributions.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/vff.h"
#include "recomp/wc24_crypto.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>

namespace recomp::nwc24 {
namespace fs = std::filesystem;

namespace {

bool logOn() {
    static const bool on = [] {
        const char* v = std::getenv("CF_NWC24_LOG");
        return v && *v && *v != '0';
    }();
    return on;
}
#define NLOG(...)                       \
    do {                                \
        if (logOn()) recomp::log(__VA_ARGS__); \
    } while (0)

std::mutex sM;
MailHandler sMail;
std::function<std::unique_ptr<ios::Handle>(const std::string&, s32&)> sNcdOwner;
int sSuspend = 0;  // KD scheduler suspended by the game (ExecSuspendScheduler / TrySuspend)
Status sStatus;

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
void put32(u8* p, u32 v) { p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v); }
void put16(u8* p, u32 v) { p[0] = u8(v >> 8), p[1] = u8(v); }

// --- the title's files ------------------------------------------------------------------------

std::string titleFile(const char* name) { return ios::titleDataDir() + "/" + name; }

struct Task {
    int index = -1;
    std::string url, file;
    u32 flags = 0;
};

// Download entries (type 3: into the title's wc24dl.vff) of the running title.
std::vector<Task> titleTasks(const std::vector<u8>& dl) {
    std::vector<Task> out;
    if (dl.size() < 0xF800 || std::memcmp(dl.data(), "WcDl", 4) != 0) return out;
    const u32 title = u32(ios::titleId());
    for (int i = 0; i < 0x78; i++) {
        const u8* e = &dl[0x800 + 0x200 * i];
        if (e[2] != 3 || be32(e + 0x08) != title) continue;
        Task t;
        t.index = i;
        t.flags = be32(e + 0x04);
        t.url.assign(reinterpret_cast<const char*>(e + 0xB4), strnlen(reinterpret_cast<const char*>(e + 0xB4), 0xEC));
        t.file.assign(reinterpret_cast<const char*>(e + 0x1A0), strnlen(reinterpret_cast<const char*>(e + 0x1A0), 0x40));
        out.push_back(std::move(t));
    }
    return out;
}

long long consoleSeconds2000() { return (long long)(double(timeBase()) / kTimerClock); }

// The WC24 container: "WC24", version 1, crypt type at 0x0C (1 = AES-128-OFB), IV at 0x30, RSA
// signature at 0x40, payload at 0x140. KD checks the signature over the decrypted payload with the
// title's key (task flag 0x02) and decrypts with the title's AES key (flag 0x08).
bool openContainer(const std::vector<u8>& file, u32 flags, std::vector<u8>& payload, std::string& why) {
    if (file.size() < 0x140) return why = "shorter than a WC24 header", false;
    if (flags & 0x04) {  // not a container: stored as it is, no signature check
        payload = file;
        return true;
    }
    const std::vector<u8> key = readNand(titleFile("wc24pubk.mod"));
    if (key.size() < 0x210) return why = "the title's wc24pubk.mod is missing", false;
    if (be32(file.data()) != 0x57433234 || be32(file.data() + 4) != 1) return why = "not a WC24 container", false;
    payload.assign(file.begin() + 0x140, file.end());
    if (file[0x0C] == 1) {
        if (!(flags & 0x08)) return why = "needs KD's own AES key (task flag 0x08 clear)", false;
        wc24crypto::aes128Ofb(key.data() + 0x200, file.data() + 0x30, payload.data(), payload.size());
    }
    if (!(flags & 0x02)) return why = "needs KD's own RSA key (task flag 0x02 clear)", false;
    if (!wc24crypto::rsa2048VerifySha1(key.data(), file.data() + 0x40, payload.data(), payload.size()))
        return why = "signature does not verify with the title's wc24pubk.mod (KD error -45)", false;
    return true;
}

void setEntryResult(std::vector<u8>& dl, int index, s32 err, bool ok) {
    u8* e = &dl[0x800 + 0x200 * index];
    u8* rec = &dl[0x80 + 0x10 * index];
    const u32 minutes = u32((std::time(nullptr)) / 60);
    put32(e + 0x20, u32(err));
    if (ok) {
        put16(e + 0x1A, 0);  // error count
        put32(e + 0x30, minutes);
        put32(rec + 0x08, minutes);  // last modified
    } else {
        put16(e + 0x1A, be16(e + 0x1A) + 1);
        put32(e + 0x30, 0);
    }
    put32(rec + 0x04, minutes + be16(e + 0x1C));  // next download
}

std::atomic<bool> sWake{false};
std::condition_variable sCv;
std::mutex sCvM;
bool sThreadStarted = false;

// One KD check: every download task of this title that our servers know.
void check() {
    {
        std::lock_guard lk(sM);
        if (sSuspend > 0) return;  // the game is using its WiiConnect24 files
    }
    const std::string dlPath = "/shared2/wc24/nwc24dl.bin";
    std::vector<u8> dl = readNand(dlPath);
    const auto tasks = titleTasks(dl);
    const std::string vffPath = titleFile("wc24dl.vff");
    Status st;
    st.taskRegistered = !tasks.empty();
    {
        std::error_code ec;
        st.keyInstalled = fs::exists(ios::hostPath(titleFile("wc24pubk.mod")), ec);
        st.vffPresent = fs::exists(ios::hostPath(vffPath), ec);
    }
    st.lastCheck = std::time(nullptr);
    bool dlChanged = false;
    for (const Task& t : tasks) {
        distrib::Reply r = distrib::serve(t.url);
        if (r.kind == distrib::Reply::NotFound) {
            NLOG("[nwc24] task %d %s: not served (%s)", t.index, t.url.c_str(), r.what.c_str());
            if (st.lastResult.empty()) st.lastResult = "nothing to download (" + r.what + ")";
            continue;
        }
        if (r.kind == distrib::Reply::NotModified) {
            NLOG("[nwc24] task %d: not modified (%s)", t.index, r.what.c_str());
            if (st.lastResult.empty()) st.lastResult = "up to date: " + r.what;
            continue;
        }
        std::vector<u8> payload;
        std::string why;
        bool ok;
        if (r.kind == distrib::Reply::Container) {
            ok = openContainer(r.body, t.flags, payload, why);
        } else {  // a package built here from the archive: no transport encoding
            payload = r.body;
            ok = true;
        }
        if (ok) {
            std::vector<u8> vol = readNand(vffPath);
            std::string err;
            if (vol.empty()) {
                ok = false, why = "wc24dl.vff does not exist yet (the game creates it)";
            } else if (!vff::write(vol, t.file, payload, &err)) {
                ok = false, why = "wc24dl.vff: " + err;
            } else if (!writeNand(vffPath, vol)) {
                ok = false, why = "could not write wc24dl.vff";
            }
        }
        setEntryResult(dl, t.index, ok ? 0 : -107245, ok);
        dlChanged = true;
        recomp::log("[nwc24] %s %s -> %s/%s: %s", ok ? "downloaded" : "download FAILED", t.url.c_str(),
                    vffPath.c_str(), t.file.c_str(), ok ? r.what.c_str() : why.c_str());
        st.lastResult = ok ? "stored " + r.what : "failed: " + why;
        distrib::onStored(r, ok, why);
    }
    if (tasks.empty()) st.lastResult = "the game has not registered its download task yet";
    if (dlChanged) writeNand(dlPath, dl);
    std::lock_guard lk(sM);
    sStatus = st;
}

void thread() {
    for (;;) {
        std::unique_lock lk(sCvM);
        sCv.wait_for(lk, std::chrono::seconds(60), [] { return sWake.load(); });
        sWake = false;
        lk.unlock();
        if (wc24On()) check();
    }
}

void startThread() {
    std::lock_guard lk(sCvM);
    if (sThreadStarted) return;
    sThreadStarted = true;
    std::thread(thread).detach();
}

// --- devices ---------------------------------------------------------------------------------

s32 outResult(u32 out, u32 outLen, s32 r) {
    if (out && outLen >= 4) wr32(out, u32(r));
    return ios::IPC_OK;
}

class KdRequest final : public ios::Handle {
public:
    s32 ioctl(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, const ios::Reply&) override {
        if (out && outLen) std::memset(hostp(out), 0, outLen);
        NLOG("[nwc24] kd ioctl 0x%02X in %u out %u", cmd, inLen, outLen);
        switch (cmd) {
        case 0x01:  // ExecSuspendScheduler
        case 0x02: {  // ExecTrySuspendScheduler
            std::lock_guard lk(sM);
            sSuspend++;
            return outResult(out, outLen, 0);
        }
        case 0x03: {  // ExecResumeScheduler: the game is done with its files -> check soon
            {
                std::lock_guard lk(sM);
                if (sSuspend > 0) sSuspend--;
            }
            requestDownload();
            return outResult(out, outLen, 0);
        }
        case 0x04:  // GetTimeTriggers: minute-of-hour triggers for download / mail check
            if (out && outLen >= 12) wr32(out + 4, 0), wr32(out + 8, 0);
            return outResult(out, outLen, 0);
        case 0x0A: case 0x0B: case 0x0C: case 0x0D: {  // mail (online-1)
            MailHandler h;
            {
                std::lock_guard lk(sM);
                h = sMail;
            }
            if (h) return h(cmd, in, inLen, out, outLen);
            return outResult(out, outLen, 0);
        }
        case 0x0E:  // DownloadNowEx
            requestDownload();
            return outResult(out, outLen, 0);
        case 0x1E:  // GetSchedulerStat: running, no error
            return outResult(out, outLen, 0);
        default:  // socket/lock/id/filter/debug/wakeup/script/shutdown: accepted
            return outResult(out, outLen, 0);
        }
    }
};

class KdTime final : public ios::Handle {
public:
    s32 ioctl(u32 cmd, u32, u32 inLen, u32 out, u32 outLen, const ios::Reply&) override {
        if (out && outLen) std::memset(hostp(out), 0, outLen);
        NLOG("[nwc24] kd/time ioctl 0x%02X in %u out %u", cmd, inLen, outLen);
        switch (cmd) {
        case 0x14:  // GetUniversalTime: result, u64 seconds since 2000 (UTC)
            if (out && outLen >= 12) {
                const u64 t = u64(guestUnixTime() - 946684800LL);
                wr32(out + 4, u32(t >> 32));
                wr32(out + 8, u32(t));
            }
            return outResult(out, outLen, 0);
        default:  // SetUniversalTime / SetRtcCounter / GetTimeDifference: accepted
            return outResult(out, outLen, 0);
        }
    }
};

// NCD: the console's network settings. One wired connection, set up and tested (what the System
// Menu leaves after a successful connection test). Online play may replace it (setNcdOwner).
class Ncd final : public ios::Handle {
public:
    s32 ioctlv(u32 cmd, u32 nin, u32 nout, const ios::Vec* v, const ios::Reply&) override {
        NLOG("[nwc24] ncd ioctlv 0x%X (%u in, %u out)", cmd, nin, nout);
        for (u32 i = 0; i < nout; i++)
            if (v[nin + i].addr && v[nin + i].len) std::memset(hostp(v[nin + i].addr), 0, v[nin + i].len);
        if (cmd == 0x03 && nout >= 1 && v[nin].len >= 0x1B5C) {  // ReadConfig (config.dat layout)
            const u32 c = v[nin].addr;
            wr32(c + 0x00, 0);  // version / header
            wr8(c + 0x04, 1);   // header: configuration in use
            wr8(c + 0x08, 0xA7);  // connection 1: selected, tested, wired, DHCP, automatic DNS
            if (nout >= 2 && v[nin + 1].len >= 4) wr32(v[nin + 1].addr, 0);
            return ios::IPC_OK;
        }
        if (nout >= 1 && v[nin + nout - 1].len >= 4) wr32(v[nin + nout - 1].addr, 0);
        return ios::IPC_OK;
    }
    s32 ioctl(u32 cmd, u32, u32, u32 out, u32 outLen, const ios::Reply&) override {
        NLOG("[nwc24] ncd ioctl 0x%X", cmd);
        if (out && outLen) std::memset(hostp(out), 0, outLen);
        return ios::IPC_OK;
    }
};

}  // namespace

bool wc24On() { return distrib::enabled(); }

u32 sysconfWcFlags() { return wc24On() ? 1u : 0u; }

std::unique_ptr<ios::Handle> openDevice(const std::string& path, s32& err) {
    err = ios::IPC_ENOENT;
    if (!wc24On()) return nullptr;
    if (path == "/dev/net/ncd/manage") {
        std::function<std::unique_ptr<ios::Handle>(const std::string&, s32&)> owner;
        {
            std::lock_guard lk(sM);
            owner = sNcdOwner;
        }
        if (owner) return owner(path, err);
        err = ios::IPC_OK;
        return std::make_unique<Ncd>();
    }
    if (path == "/dev/net/kd/request") {
        startThread();
        err = ios::IPC_OK;
        return std::make_unique<KdRequest>();
    }
    if (path == "/dev/net/kd/time") {
        err = ios::IPC_OK;
        return std::make_unique<KdTime>();
    }
    return nullptr;
}

void setMailHandler(MailHandler h) {
    std::lock_guard lk(sM);
    sMail = std::move(h);
}
void setNcdOwner(std::function<std::unique_ptr<ios::Handle>(const std::string&, s32&)> open) {
    std::lock_guard lk(sM);
    sNcdOwner = std::move(open);
}

void requestDownload() {
    if (!wc24On()) return;
    startThread();
    sWake = true;
    sCv.notify_all();
}

Status status() {
    std::lock_guard lk(sM);
    return sStatus;
}

long long guestUnixTime() {
    // The console clock is local time since 2000; KD works in UTC (the host's offset).
    const std::time_t now = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    g.tm_isdst = -1;
    const long long tz = (long long)now - (long long)std::mktime(&g);
    return consoleSeconds2000() + 946684800LL - tz;
}

std::vector<u8> readNand(const std::string& nandPath) {
    std::ifstream f(ios::hostPath(nandPath), std::ios::binary);
    if (!f) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool writeNand(const std::string& nandPath, const std::vector<u8>& data) {
    const fs::path p = ios::hostPath(nandPath);
    const fs::path tmp = p.string() + ".kdtmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

}  // namespace recomp::nwc24
