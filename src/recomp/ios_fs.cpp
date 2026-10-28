// IOS file system: /dev/fs and NAND file descriptors (docs/NAND.md). The Wii's NAND is a host
// folder (CF_NAND_DIR, default <dataDir>/nand) with the console's paths: /title/00010000/
// 52555545/data/ (the save), /shared2/sys/SYSCONF, /shared2/menu/FaceLib/RFL_DB.dat, /tmp, ...
// Owners and permissions IOS keeps per file are stored in <nand>/~attributes.txt (a name no Wii
// file can have: NAND names are at most 12 characters of the Wii's character set).
// The game's ISFS/NAND/VF/RFL libraries run unchanged on top; what they see is what IOS returns:
// FS result codes, 12-character names, 64-byte paths, usage in 16 KiB clusters, the /tmp folder
// emptied at boot, and modelled latency (NAND writes are slow: the game shows its own "saving"
// messages while they run).
#include "recomp/ios.h"

#include "port/install.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/system.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace recomp::ios {
namespace {

namespace fs = std::filesystem;

constexpr u32 kClusterSize = 0x4000;  // NAND file-system cluster (what GetUsage counts)
constexpr u32 kGameUid = 0x1001;      // IOS user id of the disc title (0x1000 = the System Menu)
constexpr u16 kGameGid = 0x3031;      // group id = maker code "01"
constexpr int kMaxOpenFiles = 15;     // IOS FS file descriptors

const fs::path& rootPath() {
    static const fs::path root = std::getenv("CF_NAND_DIR") ? port::envPath("CF_NAND_DIR") : port::dataDir() / "nand";
    return root;
}

std::recursive_mutex sFsM;
std::map<std::string, Attr> sAttrs;  // NAND path -> attributes (missing = default)
class FileHandle;
std::vector<FileHandle*> sOpen;  // open NAND files (delete/rename of an open file fails)

// --- host paths -------------------------------------------------------------------------

bool hostUnsafe(unsigned char ch) {
    return ch < 0x20 || ch >= 0x7F || std::strchr("<>:\"\\|?*%", ch) != nullptr;
}

// A NAND name as a host file name: characters Windows cannot store become %XX; a trailing dot or
// space and the reserved device names (CON, NUL, COM1...) get their last character escaped.
std::string escapeName(const std::string& n) {
    std::string out;
    for (unsigned char ch : n) {
        if (hostUnsafe(ch)) {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", ch);
            out += b;
        } else {
            out += char(ch);
        }
    }
    auto upper = [](std::string s) {
        for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string stem = upper(out.substr(0, out.find('.')));
    static const char* const kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                            "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                            "LPT7", "LPT8", "LPT9"};
    bool reserved = false;
    for (const char* r : kReserved) reserved |= stem == r;
    if (!out.empty() && (reserved || out.back() == '.' || out.back() == ' ')) {
        const unsigned char last = static_cast<unsigned char>(out.back());
        char b[4];
        std::snprintf(b, sizeof(b), "%%%02X", last);
        out.pop_back();
        out += b;
    }
    return out;
}

std::string unescapeName(const std::string& h) {
    std::string out;
    for (size_t i = 0; i < h.size(); i++) {
        if (h[i] == '%' && i + 2 < h.size()) {
            const std::string hex = h.substr(i + 1, 2);
            char* end = nullptr;
            const long v = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == 0 && hex.size() == 2) {
                out += char(v);
                i += 2;
                continue;
            }
        }
        out += h[i];
    }
    return out;
}

std::vector<std::string> split(const std::string& nand) {
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : nand) {
        if (ch == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

std::string parentOf(const std::string& nand) {
    const size_t s = nand.find_last_of('/');
    return s == 0 || s == std::string::npos ? std::string("/") : nand.substr(0, s);
}

// IOS path rules: absolute, under 64 bytes, names of 1..12 characters, at most 8 levels.
s32 checkPath(const std::string& p) {
    if (p.empty() || p[0] != '/' || p.size() >= 64) return FS_EINVAL;
    if (p.size() > 1 && p.back() == '/') return FS_EINVAL;
    const auto parts = split(p);
    if (parts.size() > 8) return FS_EDIRDEPTH;
    for (const auto& n : parts)
        if (n.size() > 12) return FS_EINVAL;
    return IPC_OK;
}

std::string readPath(u32 a) {
    std::string s;
    for (u32 i = 0; i < 64; i++) {
        const char ch = char(rd8(a + i));
        if (!ch) break;
        s += ch;
    }
    return s;
}

// --- attributes -----------------------------------------------------------------------

fs::path attrFile() { return rootPath() / "~attributes.txt"; }

void saveAttrs() {
    std::ofstream out(attrFile(), std::ios::binary | std::ios::trunc);
    out << "# IOS file attributes of the emulated NAND: owner group owner-perm group-perm other-perm attribute path\n";
    for (const auto& [p, a] : sAttrs) {
        char line[64];
        std::snprintf(line, sizeof(line), "%08X %04X %u %u %u %u ", a.owner, a.group, a.ownerPerm, a.groupPerm,
                      a.otherPerm, a.attr);
        out << line << p << "\n";
    }
}

void loadAttrs() {
    std::ifstream in(attrFile(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string owner, group, path;
        unsigned op = 3, gp = 3, xp = 3, at = 0;
        if (!(ls >> owner >> group >> op >> gp >> xp >> at)) continue;
        std::getline(ls, path);
        while (!path.empty() && (path[0] == ' ')) path.erase(0, 1);
        while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
        if (path.empty()) continue;
        Attr a;
        a.owner = u32(std::strtoul(owner.c_str(), nullptr, 16));
        a.group = u16(std::strtoul(group.c_str(), nullptr, 16));
        a.ownerPerm = u8(op), a.groupPerm = u8(gp), a.otherPerm = u8(xp), a.attr = u8(at);
        sAttrs[path] = a;
    }
}

// Attributes of a path that has none recorded: the game's own files are its own, everything
// else is readable and writable by anyone (the folders the System Menu and IOS create).
Attr defaultAttr(const std::string& p) {
    Attr a;
    if (p.rfind(titleDataDir(), 0) == 0) {
        a.owner = kGameUid;
        a.group = kGameGid;
    }
    return a;
}

Attr attrOf(const std::string& p) {
    auto it = sAttrs.find(p);
    return it == sAttrs.end() ? defaultAttr(p) : it->second;
}

u8 permFor(const Attr& a) {
    if (a.owner == kGameUid) return a.ownerPerm;
    if (a.group == kGameGid) return a.groupPerm;
    return a.otherPerm;
}

void forgetAttrs(const std::string& p) {
    bool changed = false;
    for (auto it = sAttrs.begin(); it != sAttrs.end();) {
        if (it->first == p || it->first.rfind(p + "/", 0) == 0) {
            it = sAttrs.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) saveAttrs();
}

void moveAttrs(const std::string& from, const std::string& to) {
    std::vector<std::pair<std::string, Attr>> moved;
    for (auto it = sAttrs.begin(); it != sAttrs.end();) {
        if (it->first == from || it->first.rfind(from + "/", 0) == 0) {
            moved.emplace_back(to + it->first.substr(from.size()), it->second);
            it = sAttrs.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [p, a] : moved) sAttrs[p] = a;
    if (!moved.empty()) saveAttrs();
}

// --- usage ------------------------------------------------------------------------------

// Clusters and inodes under a host path, as IOS counts them (the path itself included).
void usage(const fs::path& p, u32& clusters, u32& inodes) {
    std::error_code ec;
    inodes++;
    if (fs::is_regular_file(p, ec)) {
        clusters += u32((fs::file_size(p, ec) + kClusterSize - 1) / kClusterSize);
        return;
    }
    for (const auto& e : fs::directory_iterator(p, ec)) {
        if (e.path().filename() == "~attributes.txt") continue;
        usage(e.path(), clusters, inodes);
    }
}

// --- file descriptors -------------------------------------------------------------------

class FileHandle final : public Handle {
public:
    FileHandle(std::string nand, fs::path host, u32 mode, std::FILE* f) : nand_(std::move(nand)), host_(std::move(host)), mode_(mode), f_(f) {
        std::lock_guard lk(sFsM);
        sOpen.push_back(this);
    }
    ~FileHandle() override {
        if (f_) std::fclose(f_);
        std::lock_guard lk(sFsM);
        sOpen.erase(std::find(sOpen.begin(), sOpen.end(), this));
    }
    s32 read(u32 buf, u32 len) override {
        if (!(mode_ & 1)) return cost_ = 0.0001, FS_EACCES;
        const u32 size = this->size();
        const u32 n = pos_ >= size ? 0 : std::min(len, size - pos_);
        if (n) {
            std::fseek(f_, long(pos_), SEEK_SET);
            const size_t got = std::fread(hostp(buf), 1, n, f_);
            pos_ += u32(got);
            cost_ = 0.00015 + double(got) / 4.0e6;
            return s32(got);
        }
        cost_ = 0.00015;
        return 0;
    }
    s32 write(u32 buf, u32 len) override {
        if (!(mode_ & 2)) return cost_ = 0.0001, FS_EACCES;
        std::fseek(f_, long(pos_), SEEK_SET);
        const size_t put = std::fwrite(hostp(buf), 1, len, f_);
        std::fflush(f_);
        pos_ += u32(put);
        dirty_ = true;
        cost_ = 0.0003 + double(put) / 1.5e6;
        return put == len ? s32(put) : FS_EFBIG;
    }
    s32 seek(s32 off, s32 whence) override {
        cost_ = 0.00002;
        const s64 base = whence == 0 ? 0 : whence == 1 ? s64(pos_) : whence == 2 ? s64(size()) : -1;
        if (base < 0) return FS_EINVAL;
        const s64 p = base + off;
        if (p < 0 || p > s64(size())) return FS_EINVAL;
        pos_ = u32(p);
        return s32(p);
    }
    s32 ioctl(u32 cmd, u32, u32, u32 out, u32 outLen, const Reply&) override {
        cost_ = 0.0001;
        if (cmd == 11) {  // GetFileStats -> {length, position}
            if (!out || outLen < 8) return FS_EINVAL;
            wr32(out, size());
            wr32(out + 4, pos_);
            return IPC_OK;
        }
        logOnce("file ioctl " + std::to_string(cmd) + " on " + nand_ + " not handled");
        return FS_EINVAL;
    }
    s32 close() override {
        // Closing a written file commits the file system's metadata (the slow part of a save).
        cost_ = dirty_ ? 0.008 : 0.0002;
        return IPC_OK;
    }
    double cost() const override { return cost_; }
    Queue queue() const override { return Queue::Fs; }
    const std::string& nandPath() const { return nand_; }

private:
    u32 size() const {
        std::error_code ec;
        return u32(fs::file_size(host_, ec));
    }
    std::string nand_;
    fs::path host_;
    u32 mode_;
    std::FILE* f_;
    u32 pos_ = 0;
    bool dirty_ = false;
    double cost_ = 0.0001;
};

bool isOpen(const std::string& nand) {
    for (FileHandle* h : sOpen)
        if (h->nandPath() == nand || h->nandPath().rfind(nand + "/", 0) == 0) return true;
    return false;
}

// Attribute block (create, set/get attributes): owner u32, group u16, path[64], owner/group/
// other permissions, attribute byte, 2 bytes padding = 0x4C bytes.
struct AttrBlock {
    u32 owner;
    u16 group;
    std::string path;
    u8 op, gp, xp, at;
};
AttrBlock readAttrBlock(u32 a) {
    AttrBlock b;
    b.owner = rd32(a);
    b.group = rd16(a + 4);
    b.path = readPath(a + 6);
    b.op = rd8(a + 0x46), b.gp = rd8(a + 0x47), b.xp = rd8(a + 0x48), b.at = rd8(a + 0x49);
    return b;
}

class FsDevice final : public Handle {
public:
    // Every refused /dev/fs request is logged once with its path (save errors in player logs).
    s32 ioctl(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, const Reply& r) override {
        const s32 res = ioctlImpl(cmd, in, inLen, out, outLen, r);
        if (res < 0) {
            std::string path = in && cmd != 0x02 ? readPath(cmd == 0x03 || cmd == 0x05 || cmd == 0x09 ? in + 6 : in) : "";
            if (cmd == 0x08 && in) path += " -> " + readPath(in + 0x40);
            logOnce("/dev/fs ioctl " + std::to_string(cmd) + " " + path + " -> error " + std::to_string(res));
        }
        return res;
    }
    s32 ioctlv(u32 cmd, u32 nin, u32 nout, const Vec* v, const Reply& r) override {
        const s32 res = ioctlvImpl(cmd, nin, nout, v, r);
        if (res < 0) {
            const std::string path = nin >= 1 && v[0].addr ? readPath(v[0].addr) : "";
            logOnce("/dev/fs ioctlv " + std::to_string(cmd) + " " + path + " -> error " + std::to_string(res));
        }
        return res;
    }

private:
    s32 ioctlImpl(u32 cmd, u32 in, u32 inLen, u32 out, u32 outLen, const Reply&) {
        std::lock_guard lk(sFsM);
        std::error_code ec;
        cost_ = 0.0003;
        switch (cmd) {
        case 0x01: return FS_EACCES;  // Format: only the System Menu may
        case 0x02: {                  // GetStats
            if (!out || outLen < 0x1C) return FS_EINVAL;
            u32 clusters = 0, inodes = 0;
            usage(nandRoot(), clusters, inodes);
            // A console with the System Menu and a few channels installed, plenty of room left.
            const u32 used = clusters + 0x1400, total = 0x7000;
            wr32(out, kClusterSize);
            wr32(out + 4, used < total ? total - used : 0);
            wr32(out + 8, used);
            wr32(out + 12, 0);
            wr32(out + 16, 0x1000);
            wr32(out + 20, 0x17FF - (inodes + 0x200));
            wr32(out + 24, inodes + 0x200);
            return IPC_OK;
        }
        case 0x03:    // CreateDir
        case 0x09: {  // CreateFile
            if (!in || inLen < 0x4C) return FS_EINVAL;
            const AttrBlock b = readAttrBlock(in);
            cost_ = 0.006;
            if (const s32 e = checkPath(b.path); e) return e;
            const fs::path p = hostPath(b.path);
            const std::string parent = parentOf(b.path);
            if (!fs::is_directory(hostPath(parent), ec)) return FS_ENOENT;
            if (!(permFor(attrOf(parent)) & 2)) return FS_EACCES;
            if (fs::exists(p, ec)) return FS_EEXIST;
            if (cmd == 0x03) fs::create_directory(p, ec);
            else std::ofstream(p, std::ios::binary);
            if (ec || !fs::exists(p)) return FS_EIO;
            Attr a;
            a.owner = kGameUid;
            a.group = kGameGid;
            a.ownerPerm = b.op & 3, a.groupPerm = b.gp & 3, a.otherPerm = b.xp & 3, a.attr = b.at;
            sAttrs[b.path] = a;
            saveAttrs();
            return IPC_OK;
        }
        case 0x05: {  // SetAttr (owner only; the owner id/group in the block are kept as given)
            if (!in || inLen < 0x4C) return FS_EINVAL;
            const AttrBlock b = readAttrBlock(in);
            cost_ = 0.006;
            if (const s32 e = checkPath(b.path); e) return e;
            if (!fs::exists(hostPath(b.path), ec)) return FS_ENOENT;
            Attr a = attrOf(b.path);
            if (a.owner != kGameUid) return FS_EACCES;
            a.ownerPerm = b.op & 3, a.groupPerm = b.gp & 3, a.otherPerm = b.xp & 3, a.attr = b.at;
            sAttrs[b.path] = a;
            saveAttrs();
            return IPC_OK;
        }
        case 0x06: {  // GetAttr(path) -> attribute block
            const std::string path = readPath(in);
            if (const s32 e = checkPath(path); e) return e;
            if (!fs::exists(hostPath(path), ec)) return FS_ENOENT;
            if (!out || outLen < 0x4C) return FS_EINVAL;
            const Attr a = attrOf(path);
            std::memset(hostp(out), 0, 0x4C);
            wr32(out, a.owner);
            wr16(out + 4, a.group);
            std::memcpy(hostp(out + 6), path.c_str(), path.size());
            wr8(out + 0x46, a.ownerPerm);
            wr8(out + 0x47, a.groupPerm);
            wr8(out + 0x48, a.otherPerm);
            wr8(out + 0x49, a.attr);
            return IPC_OK;
        }
        case 0x07: {  // Delete (a folder with everything in it)
            const std::string path = readPath(in);
            cost_ = 0.006;
            if (const s32 e = checkPath(path); e) return e;
            if (path == "/") return FS_EACCES;
            const fs::path p = hostPath(path);
            if (!fs::exists(p, ec)) return FS_ENOENT;
            if (!(permFor(attrOf(parentOf(path))) & 2)) return FS_EACCES;
            if (isOpen(path)) return FS_EFDOPEN;
            fs::remove_all(p, ec);
            if (ec) return FS_EIO;
            forgetAttrs(path);
            return IPC_OK;
        }
        case 0x08: {  // Rename(old path, new path at +0x40); an existing file at the target is replaced
            const std::string from = readPath(in), to = readPath(in + 0x40);
            cost_ = 0.006;
            if (const s32 e = checkPath(from); e) return e;
            if (const s32 e = checkPath(to); e) return e;
            const fs::path a = hostPath(from), b = hostPath(to);
            if (!fs::exists(a, ec)) return FS_ENOENT;
            if (!fs::is_directory(hostPath(parentOf(to)), ec)) return FS_ENOENT;
            if (isOpen(from)) return FS_EFDOPEN;
            if (fs::is_directory(a, ec) != fs::is_directory(b, ec) && fs::exists(b, ec)) return FS_EINVAL;
            if (fs::exists(b, ec) && fs::is_regular_file(b, ec)) {
                fs::remove(b, ec);
                forgetAttrs(to);
            }
            fs::rename(a, b, ec);
            if (ec) return FS_EIO;
            moveAttrs(from, to);
            return IPC_OK;
        }
        case 0x0A: return IPC_OK;  // SetFileVerCtrl (no-op on retail IOS)
        case 0x0D: return IPC_OK;  // Shutdown: nothing to flush (every write is on disk)
        default: break;
        }
        logOnce("/dev/fs ioctl " + std::to_string(cmd) + " not handled");
        return FS_EINVAL;
    }
    s32 ioctlvImpl(u32 cmd, u32 nin, u32 nout, const Vec* v, const Reply&) {
        std::lock_guard lk(sFsM);
        std::error_code ec;
        cost_ = 0.0005;
        if (cmd == 0x04) {  // ReadDir(path[, max]) -> [names,] count
            if (nin < 1 || nout < 1) return FS_EINVAL;
            const std::string path = readPath(v[0].addr);
            if (const s32 e = checkPath(path); e) return e;
            const fs::path p = hostPath(path);
            if (!fs::exists(p, ec)) return FS_ENOENT;
            if (!fs::is_directory(p, ec)) return FS_EINVAL;
            std::vector<std::string> names;
            for (const auto& e : fs::directory_iterator(p, ec)) {
                const std::string n = unescapeName(e.path().filename().string());
                if (n == "~attributes.txt") continue;
                names.push_back(n);
            }
            std::sort(names.begin(), names.end());
            cost_ += 0.00001 * double(names.size());
            if (nin == 1 && nout == 1) {
                if (v[1].len < 4) return FS_EINVAL;
                wr32(v[1].addr, u32(names.size()));
                return IPC_OK;
            }
            if (nin < 2 || nout < 2) return FS_EINVAL;
            const u32 max = rd32(v[1].addr);
            u32 at = v[2].addr, n = 0;
            const u32 end = v[2].addr + v[2].len;
            for (const auto& nm : names) {
                if (n >= max || at + nm.size() + 1 > end) break;
                std::memcpy(hostp(at), nm.c_str(), nm.size() + 1);
                at += u32(nm.size() + 1);
                n++;
            }
            wr32(v[3].addr, n);
            return IPC_OK;
        }
        if (cmd == 0x0C) {  // GetUsage(path) -> clusters, inodes
            if (nin < 1 || nout < 2) return FS_EINVAL;
            const std::string path = readPath(v[0].addr);
            if (const s32 e = checkPath(path); e) return e;
            const fs::path p = hostPath(path);
            if (!fs::exists(p, ec)) return FS_ENOENT;
            u32 clusters = 0, inodes = 0;
            usage(p, clusters, inodes);
            cost_ += 0.00002 * double(inodes);
            wr32(v[nin].addr, clusters);
            wr32(v[nin + 1].addr, inodes);
            return IPC_OK;
        }
        logOnce("/dev/fs ioctlv " + std::to_string(cmd) + " not handled");
        return FS_EINVAL;
    }
public:
    double cost() const override { return cost_; }
    Queue queue() const override { return Queue::Fs; }

private:
    double cost_ = 0.0003;
};

}  // namespace

// --- public -------------------------------------------------------------------------------

const fs::path& nandRoot() {
    const fs::path& root = rootPath();
    static std::once_flag sInit;
    std::call_once(sInit, [&root] {
        std::lock_guard lk(sFsM);
        std::error_code ec;
        const fs::path& p = root;
        fs::create_directories(p, ec);
        // What the System Menu and IOS leave on every console: the disc title's data folder,
        // the shared folders, the title-type folders (channels, disc titles, DLC...), an empty /tmp
        // (IOS clears it at boot). The save's free-space check (NANDCheck-style, City Folk's "Save
        // and quit") sums GetUsage over /title/00010001-00010007 and /shared2/title and fails with
        // "Could not access the Wii System Memory" when one of them is missing.
        for (const std::string& d : {titleDataDir(), std::string("/shared2/sys"), std::string("/shared2/menu/FaceLib"),
                                     std::string("/shared2/wc24/mbox"), std::string("/shared2/title"),
                                     std::string("/title/00000001/00000002/data"), std::string("/title/00010001"),
                                     std::string("/title/00010002"), std::string("/title/00010003"),
                                     std::string("/title/00010004"), std::string("/title/00010005"),
                                     std::string("/title/00010006"), std::string("/title/00010007"),
                                     std::string("/meta"), std::string("/ticket"), std::string("/tmp")})
            fs::create_directories(hostPath(d), ec);
        for (const auto& e : fs::directory_iterator(hostPath("/tmp"), ec)) fs::remove_all(e.path(), ec);
        loadAttrs();
        recomp::log("[nand] %s", p.string().c_str());
        sys::prepareNand();
    });
    return root;
}

fs::path hostPath(const std::string& nand) {
    fs::path p = rootPath();
    for (const auto& n : split(nand)) {
        const std::string e = escapeName(n);
        p /= fs::path(std::u8string(e.begin(), e.end()));
    }
    return p;
}

void setAttr(const std::string& nandPath, const Attr& a) {
    std::lock_guard lk(sFsM);
    sAttrs[nandPath] = a;
    saveAttrs();
}

std::unique_ptr<Handle> openFs(const std::string& path, u32 mode, s32& err, double& cost) {
    nandRoot();
    cost = 0.0006;
    if (path == "/dev/fs") return std::make_unique<FsDevice>();
    std::lock_guard lk(sFsM);
    if (const s32 e = checkPath(path); e) {
        err = e;
        return nullptr;
    }
    if (mode == 0 || mode > 3) {
        err = FS_EINVAL;
        return nullptr;
    }
    const fs::path host = hostPath(path);
    std::error_code ec;
    if (!fs::is_regular_file(host, ec)) {
        logOnce("open " + path + ": no such NAND file");
        err = fs::is_directory(host, ec) ? FS_EACCES : FS_ENOENT;
        return nullptr;
    }
    if ((permFor(attrOf(path)) & mode) != mode) {
        logOnce("open " + path + " (mode " + std::to_string(mode) + "): permission denied");
        err = FS_EACCES;
        return nullptr;
    }
    if (int(sOpen.size()) >= kMaxOpenFiles) {
        logOnce("open " + path + ": too many open NAND files");
        err = FS_EFDEXHAUSTED;
        return nullptr;
    }
    std::FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(host.c_str(), (mode & 2) ? L"r+b" : L"rb");
#else
    f = std::fopen(host.c_str(), (mode & 2) ? "r+b" : "rb");
#endif
    if (!f) {
        err = FS_EIO;
        return nullptr;
    }
    return std::make_unique<FileHandle>(path, host, mode, f);
}

}  // namespace recomp::ios
