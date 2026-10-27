// Static recompiler: main.dol + REL modules -> C++. See recompiler.h and docs/RECOMPILER.md.
#include "recompiler/recompiler.h"
#include "recompiler/rel_format.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace cfrc {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u64 = std::uint64_t;

// Translation units replaced by host (HLE) implementations (when a splits.txt names them).
// Unit names are matched with or without a leading "dolphin/" or "revolution/".
constexpr std::string_view kHleTuPrefixes[] = {"os/", "gx/", "vi/", "pad/", "dvd/", "ar/", "card/", "ai/", "dsp/", "db/",
                                               "si/", "exi/", "base/", "ipc/", "TRK_MINNOW_DOLPHIN/", "OdemuExi2/",
                                               "amcstubs/", "odenotstub/"};
// SDK units that are plain computation on guest memory: recompiled like game code.
constexpr std::string_view kHleTuExclude[] = {"os/OSAlloc.c", "os/OSArena.c", "pad/Padclamp.c", "pad/rvlPadClamp.c",
                                              "os/__ppc_eabi_init.cpp", "os/OSFont.c", "os/OSStopwatch.c", "os/OSUtf.c",
                                              "ipc/memory.c"};

bool isHleTu(std::string_view tu) {
    if (tu.substr(0, 8) == "dolphin/") tu.remove_prefix(8);
    else if (tu.substr(0, 11) == "revolution/") tu.remove_prefix(11);
    for (auto e : kHleTuExclude)
        if (tu == e) return false;
    for (auto p : kHleTuPrefixes)
        if (tu.substr(0, p.size()) == p) return true;
    return false;
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    const int n = std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    if (n < int(sizeof(buf))) return std::string(buf, n > 0 ? size_t(n) : 0);
    std::string s(size_t(n) + 1, '\0');
    va_start(ap, f);
    std::vsnprintf(s.data(), s.size(), f, ap);
    va_end(ap);
    s.resize(size_t(n));
    return s;
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        pos = nl + 1;
    }
    return lines;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool parseHex(std::string_view s, u32& out) {
    if (s.empty() || s.size() > 8) return false;
    u32 v = 0;
    for (char ch : s) {
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return false;
        v = v << 4 | u32(d);
    }
    out = v;
    return true;
}

constexpr u32 bits(u32 w, int start, int end) {  // IBM bit numbering (0 = MSB)
    return (w >> (31 - end)) & ((1u << (end - start + 1)) - 1);
}
constexpr s32 sext(u32 v, int n) { return (v & (1u << (n - 1))) ? s32(v) - s32(1u << n) : s32(v); }
constexpr s32 sext26(u32 v) { return (v & 0x02000000) ? s32(v | 0xFC000000) : s32(v); }

u32 mask(u32 mb, u32 me) {
    const u32 a = 0xFFFFFFFFu >> mb;
    const u32 b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}

bool isBctr(u32 w) { return bits(w, 0, 5) == 19 && bits(w, 21, 30) == 528 && !(w & 1); }
bool isBl(u32 w) { return bits(w, 0, 5) == 18 && (w & 1); }

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

struct Section {
    u32 addr, off, size;
};

struct Dol {
    const std::vector<u8>& data;
    std::vector<Section> sections;  // in header order, non-empty only
    size_t textCount = 0;           // the first textCount entries are text sections
    u32 entry = 0;

    explicit Dol(const std::vector<u8>& d) : data(d) {
        if (d.size() < 0x100) throw std::runtime_error("main.dol is too small");
        for (int i = 0; i < 18; i++) {
            const u32 off = be(u32(i * 4)), addr = be(u32(72 + i * 4)), size = be(u32(144 + i * 4));
            if (size) {
                if (size_t(off) + size > d.size()) throw std::runtime_error("main.dol section out of range");
                sections.push_back({addr, off, size});
                if (i < 7) textCount = sections.size();
            }
        }
        entry = be(0xE0);
    }
    u32 be(u32 off) const {
        return u32(data[off]) << 24 | u32(data[off + 1]) << 16 | u32(data[off + 2]) << 8 | data[off + 3];
    }
    u32 word(u32 addr) const {
        for (const auto& s : sections)
            if (s.addr <= addr && addr < s.addr + s.size) return be(s.off + addr - s.addr);
        throw std::runtime_error(fmt("address %08X is outside main.dol", addr));
    }
    bool has(u32 addr) const {
        for (const auto& s : sections)
            if (s.addr <= addr && addr < s.addr + s.size) return true;
        return false;
    }
    bool inText(u32 addr) const {
        for (size_t i = 0; i < textCount; i++)
            if (sections[i].addr <= addr && addr < sections[i].addr + sections[i].size) return true;
        return false;
    }
};

// One relocation: the location it patches lives in the module that owns the list (key), the
// target is `addend` within section `section` of module `module` (module 0: absolute address).
struct Reloc {
    u8 type;
    u8 section;
    u32 module;
    u32 addend;
};

// Code addresses inside a REL module are "keys": (section << 24) | offset. They stay unique per
// module and keep relative branches inside one section valid.
constexpr u32 relKey(u32 sec, u32 off) { return sec << 24 | off; }
constexpr u32 keySec(u32 key) { return key >> 24; }
constexpr u32 keyOff(u32 key) { return key & 0xFFFFFF; }

struct RelSection {
    u32 off = 0, size = 0;
    bool exec = false;
};

struct RelModule {
    std::string name;
    std::vector<u8> data;
    u32 id = 0, version = 0, bssSize = 0;
    std::vector<RelSection> secs;
    u8 prologSec = 0, epilogSec = 0, unresSec = 0;
    u32 prolog = 0, epilog = 0, unres = 0;
    u32 checksum = 0;
    // Relocations applied to this module's sections, by patched location (key).
    std::map<u32, std::vector<Reloc>> relocs;

    u32 be(u32 off) const {
        if (size_t(off) + 4 > data.size()) throw std::runtime_error(name + ": read past the end of the module");
        return relfmt::be32(&data[off]);
    }
    bool has(u32 key) const {
        const u32 s = keySec(key);
        return s < secs.size() && secs[s].off && keyOff(key) < secs[s].size;
    }
    bool inText(u32 key) const { return has(key) && secs[keySec(key)].exec; }
    u32 word(u32 key) const {
        if (!has(key)) throw std::runtime_error(fmt("%s: key %08X is outside the module", name.c_str(), key));
        return be(secs[keySec(key)].off + keyOff(key));
    }
    const std::vector<Reloc>* relocAt(u32 key) const {
        auto it = relocs.find(key);
        return it == relocs.end() ? nullptr : &it->second;
    }
};

RelModule parseRel(const RelInput& in) {
    RelModule m;
    m.name = in.name;
    m.data = in.data;
    if (m.data.size() >= 4 && (std::string_view(reinterpret_cast<const char*>(m.data.data()), 4) == "Yaz0" ||
                               std::string_view(reinterpret_cast<const char*>(m.data.data()), 4) == "Yay0"))
        throw std::runtime_error(in.name + ": compressed REL modules are not supported yet");
    if (m.data.size() < 0x40) throw std::runtime_error(in.name + ": not a REL module");
    m.id = m.be(0x00);
    const u32 nsec = m.be(0x0C), secTable = m.be(0x10);
    m.version = m.be(0x1C);
    m.bssSize = m.be(0x20);
    const u32 impOff = m.be(0x28), impSize = m.be(0x2C);
    m.prologSec = m.data[0x30];
    m.epilogSec = m.data[0x31];
    m.unresSec = m.data[0x32];
    m.prolog = m.be(0x34);
    m.epilog = m.be(0x38);
    m.unres = m.be(0x3C);
    if (m.version < 1 || m.version > 3 || nsec > 255) throw std::runtime_error(in.name + ": unsupported REL version");
    for (u32 i = 0; i < nsec; i++) {
        const u32 o = m.be(secTable + 8 * i), sz = m.be(secTable + 8 * i + 4);
        RelSection s;
        s.off = o & ~3u;
        s.exec = o & 1;
        s.size = sz;
        if (s.off && size_t(s.off) + s.size > m.data.size()) throw std::runtime_error(in.name + ": section out of range");
        m.secs.push_back(s);
    }
    for (u32 k = 0; k + 8 <= impSize; k += 8) {
        const u32 imod = m.be(impOff + k);
        u32 q = m.be(impOff + k + 4);
        u32 sec = 0, p = 0;
        for (;;) {
            if (size_t(q) + 8 > m.data.size()) throw std::runtime_error(in.name + ": relocation list out of range");
            const u32 off = u32(m.data[q]) << 8 | m.data[q + 1];
            const u8 type = m.data[q + 2], tsec = m.data[q + 3];
            const u32 addend = m.be(q + 4);
            q += 8;
            p += off;
            if (type == relfmt::R_DOLPHIN_END) break;
            if (type == relfmt::R_DOLPHIN_SECTION) {
                sec = tsec;
                p = 0;
                continue;
            }
            if (type == relfmt::R_DOLPHIN_NOP || type == relfmt::R_PPC_NONE) continue;
            m.relocs[relKey(sec, p)].push_back({type, tsec, imod, addend});
        }
    }
    m.checksum = relfmt::fileCodeChecksum(m.data.data(), m.data.size());
    return m;
}

struct Func {
    std::string name;
    u32 size;
};

bool autoName(std::string_view n) {
    return n.empty() || n.rfind("fn_", 0) == 0 || n.rfind("func_", 0) == 0 || n.rfind("lbl_", 0) == 0 ||
           n.rfind("zz_", 0) == 0 || n.rfind("r_", 0) == 0;
}

// symbols.txt: "<name> = .<section>:0x<addr>; // type:<type>[ size:0x<size>]...".
// funcs: addr -> (name, size); objs: name -> (addr, size). `secIndex` maps a section name to a
// key prefix for REL maps (nullptr: DOL, addresses are absolute).
void loadSymbols(std::string_view text, std::map<u32, Func>& funcs, std::map<std::string, std::pair<u32, u32>>& objs,
                 const RelModule* rel = nullptr) {
    std::map<u32, std::string> labels;
    for (std::string_view line : splitLines(text)) {
        const size_t sp = line.find(' ');
        if (sp == 0 || sp == std::string_view::npos) continue;
        const std::string_view name = line.substr(0, sp);
        std::string_view rest = line.substr(sp);
        if (rest.substr(0, 4) != " = .") continue;
        rest.remove_prefix(4);
        const size_t colon = rest.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view section = rest.substr(0, colon);
        bool word = !section.empty();
        for (char ch : section)
            if (!(std::isalnum(u8(ch)) || ch == '_')) word = false;
        if (!word) continue;
        rest.remove_prefix(colon + 1);
        if (rest.substr(0, 2) != "0x") continue;
        rest.remove_prefix(2);
        size_t n = 0;
        while (n < rest.size() && std::isxdigit(u8(rest[n]))) n++;
        u32 addr;
        if (!parseHex(rest.substr(0, n), addr)) continue;
        rest.remove_prefix(n);
        if (rest.substr(0, 10) != "; // type:") continue;
        rest.remove_prefix(10);
        n = 0;
        while (n < rest.size() && (std::isalnum(u8(rest[n])) || rest[n] == '_')) n++;
        const std::string_view type = rest.substr(0, n);
        if (type.empty()) continue;
        rest.remove_prefix(n);
        u32 size = 0;
        if (rest.substr(0, 8) == " size:0x") {
            rest.remove_prefix(8);
            n = 0;
            while (n < rest.size() && std::isxdigit(u8(rest[n]))) n++;
            if (n) parseHex(rest.substr(0, n), size);
        }
        bool code = section == "text" || section == "init";
        if (rel) {
            // REL maps: ".text" is the first executable section, ".secN" names section N.
            int idx = -1;
            if (section.substr(0, 3) == "sec") idx = std::atoi(std::string(section.substr(3)).c_str());
            else if (code)
                for (size_t i = 0; i < rel->secs.size(); i++)
                    if (rel->secs[i].exec) {
                        idx = int(i);
                        break;
                    }
            if (idx < 0 || size_t(idx) >= rel->secs.size()) continue;
            code = rel->secs[size_t(idx)].exec;
            addr = relKey(u32(idx), addr);
        }
        if (type == "function" && code) funcs[addr] = {std::string(name), size};
        else if (type == "object") objs[std::string(name)] = {addr, size};
        else if (type == "label" && code) labels[addr] = std::string(name);
    }
    // A label inside a function is an alternate entry point: it becomes its own function
    // running to the end of the containing one.
    std::vector<std::pair<u32, u32>> starts;  // (addr, end)
    for (auto& [a, f] : funcs) starts.push_back({a, a + f.size});
    for (auto& [addr, name] : labels) {
        auto it = std::upper_bound(starts.begin(), starts.end(), addr,
                                   [](u32 v, const std::pair<u32, u32>& s) { return v < s.first; });
        if (it == starts.begin()) continue;
        --it;
        if (addr != it->first && addr < it->second) funcs[addr] = {name, it->second - addr};
    }
}

struct TuRange {
    u32 lo, hi;
};

std::vector<TuRange> loadHleRanges(std::string_view text) {
    std::vector<TuRange> ranges;
    std::string cur;
    for (std::string_view line : splitLines(text)) {
        if (!line.empty() && line[0] != '\t' && line[0] != ' ' && line.back() == ':' && line != "Sections:") {
            cur = std::string(line.substr(0, line.size() - 1));
        } else if (!cur.empty() && isHleTu(cur)) {
            // "\t.text       start:0x... end:0x..."
            std::string_view s = line;
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i == 0) continue;
            s.remove_prefix(i);
            if (s.substr(0, 5) == ".text") s.remove_prefix(5);
            else if (s.substr(0, 5) == ".init") s.remove_prefix(5);
            else continue;
            i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i == 0) continue;
            s.remove_prefix(i);
            if (s.substr(0, 8) != "start:0x") continue;
            s.remove_prefix(8);
            const size_t sp = s.find(' ');
            if (sp == std::string_view::npos) continue;
            u32 lo, hi;
            if (!parseHex(s.substr(0, sp), lo)) continue;
            s.remove_prefix(sp + 1);
            if (s.substr(0, 6) != "end:0x") continue;
            s.remove_prefix(6);
            size_t n = 0;
            while (n < s.size() && std::isxdigit(u8(s[n]))) n++;
            if (!parseHex(s.substr(0, n), hi)) continue;
            ranges.push_back({lo, hi});
        }
    }
    return ranges;
}

// ---------------------------------------------------------------------------
// Code spaces: the DOL (keys = guest addresses) or one REL module (keys = section/offset)
// ---------------------------------------------------------------------------

struct Space {
    const Dol* dol = nullptr;       // the DOL space
    const RelModule* mod = nullptr;  // a REL module's space
    std::map<u32, Func> funcs;
    std::unordered_set<u32> hle;     // emitted as HLE calls (SDK translation units)
    std::unordered_set<u32> hooked;  // translated body in <fn>_orig, replaceable by name at run time
    // Runtime-switchable instruction patches (DOL): address -> (switch id, replacement word).
    std::unordered_map<u32, std::vector<std::pair<u32, u32>>> patches;
    std::unordered_map<u32, std::vector<u32>> jumpTables;  // bctr key -> target keys
    std::unordered_set<u32> jumpTargets;                  // every jump-table target
    size_t addressEntries = 0, detectedTables = 0, scanned = 0;

    bool isRel() const { return mod != nullptr; }
    u32 word(u32 k) const { return mod ? mod->word(k) : dol->word(k); }
    bool has(u32 k) const { return mod ? mod->has(k) : dol->has(k); }
    bool inText(u32 k) const { return (k % 4) == 0 && (mod ? mod->inText(k) : dol->inText(k)); }
    std::vector<std::pair<u32, u32>> textRanges() const {
        std::vector<std::pair<u32, u32>> r;
        if (mod) {
            for (size_t i = 0; i < mod->secs.size(); i++)
                if (mod->secs[i].exec && mod->secs[i].off && mod->secs[i].size)
                    r.push_back({relKey(u32(i), 0), relKey(u32(i), mod->secs[i].size)});
        } else {
            for (size_t i = 0; i < dol->textCount; i++)
                r.push_back({dol->sections[i].addr, dol->sections[i].addr + dol->sections[i].size});
        }
        return r;
    }
    std::string fname(u32 k) const {
        return mod ? fmt("r%03u_%u_%06X", mod->id, keySec(k), keyOff(k)) : fmt("f_%08X", k);
    }
    // Section base expression of a REL key (`rb_` is the current function's own section base).
    std::string base(u32 k, u32 ownSec) const {
        return keySec(k) == ownSec ? "rb_" : fmt("relBase(%uu, %uu)", mod->id, keySec(k));
    }
    // Guest address of key k at run time, as an expression (from a function in section ownSec).
    std::string addr(u32 k, u32 ownSec) const {
        if (!mod) return fmt("0x%08Xu", k);
        return "(" + base(k, ownSec) + fmt(" + 0x%Xu)", keyOff(k));
    }
};

struct Unsupported {
    std::string what;
};

class Recompiler;

class FuncEmitter {
public:
    FuncEmitter(Recompiler& rc, Space& sp, u32 addr, u32 size) : rc_(rc), sp_(sp), start_(addr), end_(addr + size) {}
    std::vector<std::string> translate();

private:
    Recompiler& rc_;
    Space& sp_;
    u32 start_, end_;
    u32 pc_ = 0;
    std::set<u32> labels_;
    std::vector<std::string> lines_;
    // Relocated immediate of the current instruction (REL): its signed and unsigned values as
    // expressions; empty when the instruction has none.
    std::string relS_, relU_;

    static std::string R(u32 n) { return fmt("c.r[%u]", n); }
    static std::string ra0(u32 n) { return n == 0 ? "0u" : fmt("c.r[%u]", n); }
    void E(const std::string& s) { lines_.push_back("    " + s); }
    static std::string rec(u32 reg) { return fmt("c.setCr0(s32(c.r[%u]));", reg); }
    static std::string h8(u32 v) { return fmt("0x%08Xu", v); }
    u32 ownSec() const { return sp_.isRel() ? keySec(start_) : 0; }
    std::string A(u32 key) const { return sp_.addr(key, ownSec()); }
    std::string callExpr(u32 key) const;

    std::string branchCond(u32 bo, u32 bi);
    std::string targetCode(u32 target, bool link, const std::string& cond = "true");
    bool relBranch(u32 w, const Reloc& r);
    void relImmediate(const Reloc& r);
    void insn(u32 w, u32 pc);
    void op31(u32 w, u32 d, u32 a, u32 b);
    void fp(u32 w, u32 op, u32 d, u32 a, u32 b);
    void ps(u32 w, u32 d, u32 a, u32 b);
};

class Recompiler {
public:
    Recompiler(const Dol& dol, const std::vector<RelModule>& rels) : dol(dol), rels(rels) {
        dolSpace.dol = &dol;
        for (const RelModule& m : rels) {
            relSpaces.emplace_back();
            relSpaces.back().mod = &m;
        }
        for (size_t i = 0; i < rels.size(); i++) relById[rels[i].id] = i;
    }

    const Dol& dol;
    const std::vector<RelModule>& rels;
    Space dolSpace;
    std::vector<Space> relSpaces;
    std::unordered_map<u32, size_t> relById;
    std::unordered_set<std::string> hooks;
    std::map<std::string, unsigned> unsupported;

    Space* relSpace(u32 id) {
        auto it = relById.find(id);
        return it == relById.end() ? nullptr : &relSpaces[it->second];
    }

    // Function boundaries of the DOL: the map's functions plus the scan's starts outside them.
    void setupDol(std::map<u32, Func> mapFuncs, const std::map<std::string, std::pair<u32, u32>>& objs,
                  const std::vector<TuRange>& hleRanges, const std::map<u32, std::pair<std::string, std::string>>* vmap);
    void setupRel(Space& sp, std::map<u32, Func> mapFuncs, const std::map<std::string, std::pair<u32, u32>>& objs);
    void run(Output& out, const std::string& version);

private:
    static long containing(const std::vector<u32>& starts, u32 t) {
        auto it = std::upper_bound(starts.begin(), starts.end(), t);
        if (it == starts.begin()) return -1;
        return long(it - starts.begin()) - 1;
    }
    void layout(Space& sp, std::map<u32, Func>& mapFuncs, const std::set<u32>& scanStarts, const char* prefix);
    std::set<u32> scanDol(Space& sp, std::map<std::string, std::pair<u32, u32>>& objs);
    std::set<u32> scanRel(Space& sp);
    bool addInteriorEntries(Space& sp);
    size_t addAddressTakenEntries(Space& sp, const std::set<u32>& cands);
    void findJumpTables(Space& sp, const std::map<std::string, std::pair<u32, u32>>& objs);
};

// ---- function boundaries ----------------------------------------------------

// Starts from the scan become functions that run to the next start (or the end of their code
// section); functions of the map keep their size (0 = up to the next start).
void Recompiler::layout(Space& sp, std::map<u32, Func>& mapFuncs, const std::set<u32>& scanStarts, const char* prefix) {
    std::map<u32, Func> funcs;
    std::vector<std::pair<u32, u32>> covered;  // sized map functions
    for (auto& [a, f] : mapFuncs)
        if (f.size) covered.push_back({a, a + f.size});
    std::sort(covered.begin(), covered.end());
    auto inside = [&](u32 a) {
        auto it = std::upper_bound(covered.begin(), covered.end(), std::make_pair(a, 0xFFFFFFFFu));
        if (it == covered.begin()) return false;
        --it;
        return it->first < a && a < it->second;
    };
    std::set<u32> starts;
    for (auto& [a, f] : mapFuncs)
        if (sp.inText(a)) starts.insert(a);
    for (u32 a : scanStarts)
        if (sp.inText(a) && !inside(a)) starts.insert(a);
    const auto text = sp.textRanges();
    for (auto it = starts.begin(); it != starts.end(); ++it) {
        const u32 a = *it;
        u32 secEnd = a;
        for (auto& [lo, hi] : text)
            if (lo <= a && a < hi) secEnd = hi;
        auto next = std::next(it);
        u32 end = next != starts.end() && *next < secEnd ? *next : secEnd;
        auto m = mapFuncs.find(a);
        if (m != mapFuncs.end() && m->second.size) end = std::min(a + m->second.size, secEnd);
        std::string name = m != mapFuncs.end() ? m->second.name : std::string();
        if (name.empty()) name = sp.isRel() ? fmt("%s%03u_%u_%06X", prefix, sp.mod->id, keySec(a), keyOff(a))
                                            : fmt("%s%08X", prefix, a);
        if (end > a) funcs[a] = {name, end - a};
        if (m == mapFuncs.end()) sp.scanned++;
    }
    sp.funcs = std::move(funcs);
}

// DOL scan: the entry point, every bl target, and code addresses the program materialises
// (lis/addi or lis/ori pairs, words in data sections), except jump-table targets. Jump tables are
// a lis/addi of a data address followed by a bctr; their words are the run of code addresses
// near the reference.
std::set<u32> Recompiler::scanDol(Space& sp, std::map<std::string, std::pair<u32, u32>>& objs) {
    std::set<u32> starts;
    if (sp.inText(dol.entry)) starts.insert(dol.entry);
    std::unordered_set<u32> knownTables;
    for (auto& [name, o] : objs)
        if (name.rfind("jumptable_", 0) == 0) {
            knownTables.insert(o.first);
            for (u32 a = o.first; a < o.first + o.second; a += 4)
                if (dol.has(a)) sp.jumpTargets.insert(dol.word(a));
        }
    auto inData = [&](u32 a) { return dol.has(a) && !dol.inText(a); };
    std::set<u32> cands;
    for (auto& [lo, hi] : sp.textRanges()) {
        starts.insert(lo);
        u32 his[32] = {};
        bool hv[32] = {};
        for (u32 pc = lo; pc < hi; pc += 4) {
            const u32 w = dol.word(pc);
            const u32 op = bits(w, 0, 5);
            if (op == 18 && (w & 1)) {
                const s32 li = sext26(w & 0x03FFFFFC);
                const u32 t = (w & 2) ? u32(li) : pc + u32(li);
                if (sp.inText(t)) starts.insert(t);
            }
            if (op == 15 && bits(w, 11, 15) == 0) {
                his[bits(w, 6, 10)] = (w & 0xFFFF) << 16;
                hv[bits(w, 6, 10)] = true;
            } else if ((op == 14 || op == 24) && hv[bits(w, 11, 15)]) {
                const u32 v = his[bits(w, 11, 15)] + (op == 14 ? u32(sext(w & 0xFFFF, 16)) : (w & 0xFFFF));
                if (sp.inText(v)) cands.insert(v);
                else if (op == 14 && v % 4 == 0 && inData(v) && !knownTables.count(v)) {
                    // A jump table if a bctr follows within a few instructions.
                    bool bctr = false;
                    for (u32 q = pc + 4; q < pc + 48 && q < hi && !bctr; q += 4) bctr = isBctr(dol.word(q));
                    if (bctr) {
                        u32 n = 0;
                        while (dol.has(v + n * 4) && !dol.inText(v + n * 4)) {
                            const u32 t = dol.word(v + n * 4);
                            if (!sp.inText(t) || t + 0x10000 < pc || t > pc + 0x10000) break;
                            n++;
                        }
                        if (n >= 2) {
                            objs[fmt("jumptable_%08X", v)] = {v, n * 4};
                            knownTables.insert(v);
                            for (u32 i = 0; i < n; i++) sp.jumpTargets.insert(dol.word(v + i * 4));
                            sp.detectedTables++;
                        }
                    }
                }
            } else if (op == 18 || op == 16 || op == 19) {
                // control flow ends the pair tracking only for calls (registers are clobbered)
                if (w & 1) std::fill(std::begin(hv), std::end(hv), false);
            }
        }
    }
    std::unordered_set<u32> tableWords;
    for (auto& [name, o] : objs)
        if (name.rfind("jumptable_", 0) == 0)
            for (u32 a = o.first; a < o.first + o.second; a += 4) tableWords.insert(a);
    for (size_t i = dol.textCount; i < dol.sections.size(); i++) {
        const Section& s = dol.sections[i];
        for (u32 off = 0; off + 3 < s.size; off += 4) {
            const u32 a = s.addr + off;
            if (tableWords.count(a)) continue;
            const u32 v = dol.be(s.off + off);
            if (sp.inText(v)) cands.insert(v);
        }
    }
    // DOL code the REL modules call or take the address of (their relocations against module 0).
    for (const RelModule& m : rels)
        for (auto& [key, list] : m.relocs)
            for (const Reloc& r : list)
                if (r.module == 0 && r.type != relfmt::R_PPC_ADDR16_HA && r.type != relfmt::R_PPC_ADDR16_HI &&
                    sp.inText(r.addend))
                    cands.insert(r.addend);
    for (u32 v : cands)
        if (!sp.jumpTargets.count(v)) starts.insert(v);
    return starts;
}

// REL scan: section starts, prolog/epilog/unresolved, relative bl targets, and every relocation
// target in this module's code (from any module's relocations) except jump-table targets.
std::set<u32> Recompiler::scanRel(Space& sp) {
    const RelModule& m = *sp.mod;
    std::set<u32> starts;
    for (auto& [lo, hi] : sp.textRanges()) {
        starts.insert(lo);
        for (u32 k = lo; k < hi; k += 4) {
            const u32 w = m.word(k);
            if (isBl(w) && !m.relocAt(k) && !(w & 2)) {
                const u32 t = k + u32(sext26(w & 0x03FFFFFC));
                if (sp.inText(t)) starts.insert(t);
            }
        }
    }
    if (m.prologSec) starts.insert(relKey(m.prologSec, m.prolog));
    if (m.epilogSec) starts.insert(relKey(m.epilogSec, m.epilog));
    if (m.unresSec) starts.insert(relKey(m.unresSec, m.unres));
    // Jump tables: a LO relocation in code to this module's data, a bctr shortly after, and a run
    // of ADDR32 relocations to code near the reference at that data address.
    for (auto& [key, list] : m.relocs) {
        if (!m.inText(key & ~3u)) continue;
        for (const Reloc& r : list) {
            if (r.type != relfmt::R_PPC_ADDR16_LO || r.module != m.id) continue;
            if (r.section >= m.secs.size() || m.secs[r.section].exec) continue;
            const u32 ref = key & ~3u;
            bool bctr = false;
            u32 bctrKey = 0;
            for (u32 q = ref + 4; q < ref + 48 && m.inText(q) && !bctr; q += 4)
                if (isBctr(m.word(q))) {
                    bctr = true;
                    bctrKey = q;
                }
            if (!bctr) continue;
            std::vector<u32> targets;
            for (u32 n = 0;; n++) {
                const auto* e = m.relocAt(relKey(r.section, r.addend + 4 * n));
                if (!e || e->size() != 1) break;
                const Reloc& t = (*e)[0];
                if (t.type != relfmt::R_PPC_ADDR32 || t.module != m.id || t.section != keySec(ref)) break;
                const u32 tk = relKey(t.section, t.addend);
                if (tk + 0x10000 < ref || tk > ref + 0x10000) break;
                targets.push_back(tk);
            }
            if (targets.size() >= 2) {
                for (u32 t : targets) sp.jumpTargets.insert(t);
                auto& jt = sp.jumpTables[bctrKey];
                jt.insert(jt.end(), targets.begin(), targets.end());
                sp.detectedTables++;
            }
        }
    }
    // Relocation targets in this module's code, from every module's relocations (vtables,
    // function pointers, calls between modules).
    auto addTarget = [&](const Reloc& r) {
        if (r.module != m.id || r.section >= m.secs.size() || !m.secs[r.section].exec) return;
        if (r.type == relfmt::R_PPC_ADDR16_HA || r.type == relfmt::R_PPC_ADDR16_HI) return;  // LO has the same target
        const u32 tk = relKey(r.section, r.addend);
        if (!sp.jumpTargets.count(tk)) starts.insert(tk);
    };
    for (const RelModule& other : rels)
        for (auto& [key, list] : other.relocs)
            for (const Reloc& r : list) addTarget(r);
    return starts;
}

// Branches from one function into the middle of another make the target an extra entry point
// (a function running to the end of the containing one).
bool Recompiler::addInteriorEntries(Space& sp) {
    std::vector<u32> starts;
    for (auto& [a, f] : sp.funcs) starts.push_back(a);
    std::map<u32, Func> extra;
    for (auto& [faddr, f] : sp.funcs) {
        if (sp.hle.count(faddr)) continue;
        for (u32 pc = faddr; pc < faddr + f.size; pc += 4) {
            if (sp.isRel() && sp.mod->relocAt(pc)) continue;  // relocated branches: resolved elsewhere
            const u32 w = sp.word(pc);
            const u32 op = bits(w, 0, 5);
            u32 t;
            if (op == 18) {
                const s32 li = sext26(w & 0x03FFFFFC);
                t = (w & 2) ? u32(li) : pc + u32(li);
            } else if (op == 16) {
                t = pc + u32(sext(w & 0xFFFC, 16));
            } else {
                continue;
            }
            if ((faddr <= t && t < faddr + f.size) || sp.funcs.count(t)) continue;
            const long i = containing(starts, t);
            if (i < 0) continue;
            const u32 ca = starts[size_t(i)];
            const Func& cf = sp.funcs.at(ca);
            const u32 cend = ca + cf.size;
            if (t < cend && !sp.hle.count(ca)) extra[t] = {cf.name + fmt("_entry_%08X", t), cend - t};
        }
    }
    for (auto& [a, f] : extra) sp.funcs[a] = f;
    return !extra.empty();
}

// Code addresses that may be called indirectly but fell inside a function: interior entries.
size_t Recompiler::addAddressTakenEntries(Space& sp, const std::set<u32>& cands) {
    std::vector<u32> starts;
    for (auto& [a, f] : sp.funcs) starts.push_back(a);
    std::map<u32, Func> extra;
    for (u32 t : cands) {
        if (!sp.inText(t) || sp.funcs.count(t) || sp.jumpTargets.count(t)) continue;
        const long i = containing(starts, t);
        if (i < 0) continue;
        const u32 ca = starts[size_t(i)];
        const Func& cf = sp.funcs.at(ca);
        const u32 cend = ca + cf.size;
        if (t < cend && !sp.hle.count(ca)) extra[t] = {cf.name + fmt("_entry_%08X", t), cend - t};
    }
    for (auto& [a, f] : extra) sp.funcs[a] = f;
    return extra.size();
}

// Map each bctr address to jump-table targets: every table whose base the function materialises
// (lis/addi). A base can be loaded once into a register and used by several bctrs, and a stray
// case is harmless (cases match the exact ctr value), so each bctr gets the targets of all of the
// function's tables. (REL tables come from relocations: scanRel.)
void Recompiler::findJumpTables(Space& sp, const std::map<std::string, std::pair<u32, u32>>& objs) {
    if (sp.isRel()) return;
    std::unordered_map<u32, u32> byAddr;
    for (auto& [name, o] : objs)
        if (name.rfind("jumptable_", 0) == 0) byAddr[o.first] = o.second;
    for (auto& [faddr, f] : sp.funcs) {
        if (sp.hle.count(faddr)) continue;
        std::unordered_map<u32, u32> hi;
        std::vector<u32> targets, bctrs;
        for (u32 pc = faddr; pc < faddr + f.size; pc += 4) {
            const u32 w = dol.word(pc);
            const u32 op = bits(w, 0, 5);
            if (op == 15 && bits(w, 11, 15) == 0) {
                hi[bits(w, 6, 10)] = (w & 0xFFFF) << 16;
            } else if (op == 14 && hi.count(bits(w, 11, 15))) {
                const u32 base = hi[bits(w, 11, 15)] + u32(sext(w & 0xFFFF, 16));
                if (auto it = byAddr.find(base); it != byAddr.end())
                    for (u32 i = 0; i < it->second / 4; i++) targets.push_back(dol.word(base + i * 4));
            } else if (isBctr(w)) {
                bctrs.push_back(pc);
            }
        }
        if (!targets.empty())
            for (u32 pc : bctrs) sp.jumpTables[pc] = targets;
    }
}

void Recompiler::setupDol(std::map<u32, Func> mapFuncs, const std::map<std::string, std::pair<u32, u32>>& objsIn,
                          const std::vector<TuRange>& hleRanges,
                          const std::map<u32, std::pair<std::string, std::string>>* vmap) {
    Space& sp = dolSpace;
    std::map<std::string, std::pair<u32, u32>> objs = objsIn;
    const std::set<u32> scan = scanDol(sp, objs);
    if (!mapFuncs.count(dol.entry) && sp.inText(dol.entry)) mapFuncs[dol.entry] = {"__start", 0};
    layout(sp, mapFuncs, scan, "fn_");
    if (vmap) {
        std::unordered_set<std::string> mapped;
        for (auto& [addr, f] : sp.funcs) {
            auto it = vmap->find(addr);
            if (it == vmap->end()) continue;
            f.name = it->second.first;
            mapped.insert(f.name);
            if (isHleTu(it->second.second)) sp.hle.insert(addr);
        }
        for (auto& [addr, f] : sp.funcs)
            if (!vmap->count(addr) && mapped.count(f.name)) f.name = "v_" + f.name;
    } else {
        for (auto& [addr, f] : sp.funcs)
            for (const auto& r : hleRanges)
                if (r.lo <= addr && addr < r.hi) sp.hle.insert(addr);
    }
    // Address-taken code that the layout left inside a function (map functions).
    std::set<u32> cands = scan;
    for (const Section& s : dol.sections) {
        if (dol.inText(s.addr)) continue;
        for (u32 i = 0; i + 3 < s.size; i += 4) cands.insert(dol.be(s.off + i));
    }
    sp.addressEntries = addAddressTakenEntries(sp, cands);
    while (addInteriorEntries(sp)) {
    }
    findJumpTables(sp, objs);
}

void Recompiler::setupRel(Space& sp, std::map<u32, Func> mapFuncs, const std::map<std::string, std::pair<u32, u32>>&) {
    const std::set<u32> scan = scanRel(sp);
    layout(sp, mapFuncs, scan, "r");
    sp.addressEntries = addAddressTakenEntries(sp, scan);
    while (addInteriorEntries(sp)) {
    }
}

// ---------------------------------------------------------------------------
// Instruction translation
// ---------------------------------------------------------------------------

std::string FuncEmitter::callExpr(u32 key) const {
    if (sp_.funcs.count(key)) return sp_.fname(key) + "(c)";
    return "c.call(" + A(key) + ")";
}

std::string FuncEmitter::branchCond(u32 bo, u32 bi) {
    std::vector<std::string> parts;
    if (!(bo & 0b00100)) {
        E("c.ctr--;");
        parts.push_back((bo & 0b00010) ? "c.ctr == 0" : "c.ctr != 0");
    }
    if (!(bo & 0b10000)) {
        const std::string test = fmt("c.crbit(%u)", bi);
        parts.push_back((bo & 0b01000) ? test : "!" + test);
    }
    if (parts.empty()) return "true";
    std::string s = parts[0];
    for (size_t i = 1; i < parts.size(); i++) s += " && " + parts[i];
    return s;
}

std::string FuncEmitter::targetCode(u32 target, bool link, const std::string& cond) {
    const bool always = cond == "true";
    if (link) {
        const std::string call = callExpr(target);
        E("c.lr = " + A(pc_ + 4) + ";");
        return always ? call + ";" : "if (" + cond + ") { " + call + "; }";
    }
    if (start_ <= target && target < end_ && target != start_) {
        labels_.insert(target);
        std::string jump = fmt("goto L_%08X;", target);
        if (target <= pc_) jump = "{ c.poll(); " + jump + " }";  // loop back-edge: let pending interrupts run
        return always ? jump : "if (" + cond + ") " + jump;
    }
    // Tail call (branch to another function, or to our own start).
    const std::string call = callExpr(target);
    return always ? call + "; return;" : "if (" + cond + ") { " + call + "; return; }";
}

// REL: a b/bl whose target comes from a relocation (R_PPC_REL24). Resolved here: into the DOL
// (absolute), into this module, or into another module (called only while that module is
// linked; otherwise the module's _unresolved, as the console's relocated branch would do).
bool FuncEmitter::relBranch(u32 w, const Reloc& r) {
    const bool link = w & 1;
    const RelModule& self = *sp_.mod;
    std::string call;
    if (r.module == 0) {
        Space& d = rc_.dolSpace;
        call = d.funcs.count(r.addend) ? d.fname(r.addend) + "(c)" : fmt("c.call(0x%08Xu)", r.addend);
    } else if (r.module == self.id) {
        const u32 tk = relKey(r.section, r.addend);
        E(targetCode(tk, link));
        return true;
    } else {
        Space* other = rc_.relSpace(r.module);
        const u32 tk = relKey(r.section, r.addend);
        const std::string inner = other && other->funcs.count(tk)
                                      ? other->fname(tk) + "(c)"
                                      : fmt("c.call(relBase(%uu, %uu) + 0x%Xu)", r.module, r.section, r.addend);
        call = fmt("if (relBase(%uu, %uu)) { ", r.module, r.section) + inner + fmt("; } else { rel_unresolved(c, %uu); }", self.id);
        call = "{ " + call + " }";
        if (link) {
            E("c.lr = " + A(pc_ + 4) + ";");
            E(call);
        } else {
            E(call + " return;");
        }
        return true;
    }
    if (link) {
        E("c.lr = " + A(pc_ + 4) + ";");
        E(call + ";");
    } else {
        E(call + "; return;");
    }
    return true;
}

// REL: the 16-bit immediate of the current instruction is patched by OSLink. Its value becomes
// an expression of the target module's run-time section base (constant for DOL targets).
void FuncEmitter::relImmediate(const Reloc& r) {
    if (r.module == 0) {
        const u32 v = r.addend;
        u32 h;
        switch (r.type) {
        case relfmt::R_PPC_ADDR16_LO: h = v & 0xFFFF; break;
        case relfmt::R_PPC_ADDR16_HI: h = v >> 16; break;
        case relfmt::R_PPC_ADDR16_HA: h = ((v + 0x8000) >> 16) & 0xFFFF; break;
        default: h = v & 0xFFFF; break;
        }
        relU_ = fmt("0x%Xu", h);
        relS_ = h8(u32(sext(h, 16)));
        return;
    }
    const std::string v = r.module == sp_.mod->id ? "(" + sp_.base(relKey(r.section, 0), ownSec()) + fmt(" + 0x%Xu)", r.addend)
                                                  : fmt("(relBase(%uu, %uu) + 0x%Xu)", r.module, r.section, r.addend);
    switch (r.type) {
    case relfmt::R_PPC_ADDR16_LO: relU_ = "(" + v + " & 0xFFFFu)"; break;
    case relfmt::R_PPC_ADDR16_HI: relU_ = "(" + v + " >> 16)"; break;
    case relfmt::R_PPC_ADDR16_HA: relU_ = "((" + v + " + 0x8000u) >> 16)"; break;
    default: relU_ = "(" + v + " & 0xFFFFu)"; break;
    }
    relS_ = "u32(s32(s16(u16" + relU_ + ")))";
}

std::vector<std::string> FuncEmitter::translate() {
    std::vector<std::pair<size_t, u32>> labelLines;  // index in lines_ -> address
    if (sp_.isRel()) lines_.push_back(fmt("    const u32 rb_ = relBase(%uu, %uu);", sp_.mod->id, ownSec()));
    for (u32 pc = start_; pc < end_; pc += 4) {
        pc_ = pc;
        const u32 w = sp_.word(pc);
        labelLines.push_back({lines_.size(), pc});
        lines_.push_back("");  // placeholder for the label
        auto one = [&](u32 word) {
            try {
                insn(word, pc);
            } catch (const Unsupported& e) {
                E(fmt("c.unimplemented(0x%08Xu, 0x%08Xu); // ", pc, word) + e.what);
                rc_.unsupported[e.what]++;
            }
        };
        if (auto it = sp_.patches.find(pc); it != sp_.patches.end()) {
            // hooks.txt "patch": the replacement runs while its switch is on.
            for (size_t i = 0; i < it->second.size(); i++) {
                E(fmt(i == 0 ? "if (g_patches[%u]) {" : "} else if (g_patches[%u]) {", it->second[i].first));
                one(it->second[i].second);
            }
            E("} else {");
            one(w);
            E("}");
        } else {
            one(w);
        }
    }
    // Execution that runs off the end continues into the next function.
    const u32 last = sp_.word(end_ - 4);
    const u32 op = bits(last, 0, 5);
    const bool ends = (op == 18 && !(last & 1)) ||
                      (op == 19 && (bits(last, 21, 30) == 16 || bits(last, 21, 30) == 528) && !(last & 1) &&
                       (bits(last, 6, 10) & 0b10100) == 0b10100);
    if (!ends && sp_.inText(end_)) lines_.push_back("    " + callExpr(end_) + "; return;");
    std::vector<std::string> out;
    out.reserve(lines_.size());
    size_t li = 0;
    for (size_t i = 0; i < lines_.size(); i++) {
        if (li < labelLines.size() && labelLines[li].first == i) {
            const u32 a = labelLines[li++].second;
            if (labels_.count(a)) out.push_back(fmt("L_%08X:;", a));
            continue;
        }
        out.push_back(std::move(lines_[i]));
    }
    return out;
}

void FuncEmitter::insn(u32 w, u32 pc) {
    const u32 op = bits(w, 0, 5);
    const u32 d = bits(w, 6, 10);
    const u32 a = bits(w, 11, 15);
    const u32 b = bits(w, 16, 20);
    const s32 simm = sext(w & 0xFFFF, 16);
    const u32 uimm = w & 0xFFFF;
    const u32 usimm = u32(simm);

    relS_.clear();
    relU_.clear();
    if (sp_.isRel()) {
        if (const auto* rl = sp_.mod->relocAt(pc)) {
            for (const Reloc& r : *rl) {
                if (r.type == relfmt::R_PPC_REL24 && op == 18) return void(relBranch(w, r));
                throw Unsupported{fmt("relocation type %u on an instruction", r.type)};
            }
        }
        if (const auto* rl = sp_.mod->relocAt(pc + 2)) {
            const Reloc& r = (*rl)[0];
            if (r.type != relfmt::R_PPC_ADDR16_LO && r.type != relfmt::R_PPC_ADDR16_HI && r.type != relfmt::R_PPC_ADDR16_HA)
                throw Unsupported{fmt("relocation type %u at an immediate", r.type)};
            relImmediate(r);
        }
    }
    // Immediates as expressions: S = sign-extended, U = zero-extended (relocated in REL code).
    const std::string S = relS_.empty() ? h8(usimm) : relS_;
    const std::string U = relU_.empty() ? fmt("0x%Xu", uimm) : relU_;
    const std::string Uhi = relU_.empty() ? fmt("0x%Xu", uimm << 16) : "(" + relU_ + " << 16)";

    if (op == 18) {  // b / bl / ba / bla
        const s32 li = sext26(w & 0x03FFFFFC);
        const u32 target = ((w >> 1) & 1) ? u32(li) : pc + u32(li);
        E(targetCode(target, w & 1));
        return;
    }
    if (op == 16) {  // bc
        const s32 bd = sext(w & 0xFFFC, 16);
        const u32 target = ((w >> 1) & 1) ? u32(bd) : pc + u32(bd);
        const std::string cond = branchCond(d, a);
        E(targetCode(target, w & 1, cond));
        return;
    }
    if (op == 19) {
        const u32 xo = bits(w, 21, 30);
        const bool lk = w & 1;
        if (xo == 16) {  // bclr
            const std::string cond = branchCond(d, a);
            if (lk) E("{ const u32 t = c.lr; c.lr = " + A(pc + 4) + "; if (" + cond + ") c.call(t); }");
            else E(cond != "true" ? "if (" + cond + ") return;" : "return;");
            return;
        }
        if (xo == 528) {  // bcctr
            const std::string cond = branchCond(d | 0b00100, a);
            if (lk) {
                E("c.lr = " + A(pc + 4) + "; if (" + cond + ") c.call(c.ctr);");
            } else {
                auto it = sp_.jumpTables.find(pc);
                if (it != sp_.jumpTables.end() && !it->second.empty()) {
                    std::set<u32> targets(it->second.begin(), it->second.end());
                    std::string cases;
                    for (u32 t : targets) {
                        if (start_ <= t && t < end_) {
                            labels_.insert(t);
                            if (!cases.empty()) cases += ' ';
                            cases += fmt("case 0x%08Xu: goto L_%08X;", sp_.isRel() ? keyOff(t) : t, t);
                        }
                    }
                    const std::string sel = sp_.isRel() ? "u32(c.ctr - rb_)" : "c.ctr";
                    E("if (" + cond + ") { switch (" + sel + ") { " + cases + " default: c.call(c.ctr); return; } }");
                } else {
                    E("if (" + cond + ") { c.call(c.ctr); return; }");
                }
            }
            return;
        }
        const std::string ea = fmt("c.crbit(%u)", a), eb = fmt("c.crbit(%u)", b);
        std::string v;
        switch (xo) {
        case 257: v = "(" + ea + " & " + eb + ")"; break;
        case 449: v = "(" + ea + " | " + eb + ")"; break;
        case 193: v = "(" + ea + " ^ " + eb + ")"; break;
        case 225: v = "!(" + ea + " && " + eb + ")"; break;
        case 33: v = "!(" + ea + " || " + eb + ")"; break;
        case 289: v = "(" + ea + " == " + eb + ")"; break;
        case 129: v = "(" + ea + " && !" + eb + ")"; break;
        case 417: v = "(" + ea + " || !" + eb + ")"; break;
        default: break;
        }
        if (!v.empty()) {
            E(fmt("c.setCrbit(%u, ", d) + v + ");");
            return;
        }
        if (xo == 0) {  // mcrf
            E(fmt("c.cr[%u] = c.cr[%u];", d >> 2, a >> 2));
            return;
        }
        if (xo == 150 || xo == 50) {  // isync, rfi
            E("// isync/rfi");
            return;
        }
        throw Unsupported{fmt("op19 xo%u", xo)};
    }

    // --- integer arithmetic with immediates ---
    switch (op) {
    case 14: E(R(d) + " = " + ra0(a) + " + " + S + ";"); return;    // addi / li
    case 15: E(R(d) + " = " + ra0(a) + " + " + Uhi + ";"); return;  // addis / lis
    case 12:
    case 13:  // addic, addic.
        E("{ const u32 s = " + R(a) + "; " + R(d) + " = s + " + S + "; c.ca = " + R(d) + " < s; }");
        if (op == 13) E(rec(d));
        return;
    case 8:  // subfic
        E("{ const u32 s = " + R(a) + "; " + R(d) + " = " + S + " - s; c.ca = s <= " + S + "; }");
        return;
    case 7: E(R(d) + " = u32(s32(" + R(a) + ") * s32(" + S + "));"); return;  // mulli
    case 10: E(fmt("c.cmpu(%u, ", d >> 2) + R(a) + ", " + U + ");"); return;
    case 11: E(fmt("c.cmps(%u, s32(", d >> 2) + R(a) + "), s32(" + S + "));"); return;
    // logical immediates: d = rS, a = rA
    case 24: E(R(a) + " = " + R(d) + " | " + U + ";"); return;
    case 25: E(R(a) + " = " + R(d) + " | " + Uhi + ";"); return;
    case 26: E(R(a) + " = " + R(d) + " ^ " + U + ";"); return;
    case 27: E(R(a) + " = " + R(d) + " ^ " + Uhi + ";"); return;
    case 28: E(R(a) + " = " + R(d) + " & " + U + ";"); E(rec(a)); return;
    case 29: E(R(a) + " = " + R(d) + " & " + Uhi + ";"); E(rec(a)); return;
    default: break;
    }
    // rotates
    if (op == 21 || op == 20 || op == 23) {
        const std::string sh = op != 23 ? fmt("%u", b) : "(" + R(b) + " & 31)";
        const u32 m = mask(bits(w, 21, 25), bits(w, 26, 30));
        const std::string rot = "c.rotl(" + R(d) + ", " + sh + ")";
        if (op == 20) E(R(a) + " = (" + rot + " & " + h8(m) + ") | (" + R(a) + " & " + h8(~m) + ");");  // rlwimi
        else E(R(a) + " = " + rot + " & " + h8(m) + ";");
        if (w & 1) E(rec(a));
        return;
    }

    // --- loads / stores ---
    const std::string ea = "(" + ra0(a) + " + " + S + ")";
    const std::string uea = "(" + R(a) + " + " + S + ")";
    auto loadExpr = [](u32 o, const std::string& addr) -> std::string {
        switch (o) {
        case 32: return "rd32(" + addr + ")";
        case 34: return "rd8(" + addr + ")";
        case 40: return "rd16(" + addr + ")";
        default: return "u32(s16(rd16(" + addr + ")))";  // 42
        }
    };
    if (op == 32 || op == 34 || op == 40 || op == 42) {
        E(R(d) + " = " + loadExpr(op, ea) + ";");
        return;
    }
    if (op == 33 || op == 35 || op == 41 || op == 43) {  // update forms
        E("{ const u32 e = " + uea + "; " + R(d) + " = " + loadExpr(op - 1, "e") + "; " + R(a) + " = e; }");
        return;
    }
    auto storeFn = [](u32 o) { return o == 36 ? "wr32" : o == 38 ? "wr8" : "wr16"; };
    if (op == 36 || op == 38 || op == 44) {
        E(std::string(storeFn(op)) + "(" + ea + ", " + R(d) + ");");
        return;
    }
    if (op == 37 || op == 39 || op == 45) {
        E("{ const u32 e = " + uea + "; " + storeFn(op - 1) + "(e, " + R(d) + "); " + R(a) + " = e; }");
        return;
    }
    switch (op) {
    case 46: E("{ u32 e = " + ea + fmt("; for (int i = %u; i < 32; i++, e += 4) c.r[i] = rd32(e); }", d)); return;   // lmw
    case 47: E("{ u32 e = " + ea + fmt("; for (int i = %u; i < 32; i++, e += 4) wr32(e, c.r[i]); }", d)); return;  // stmw
    case 48: E(fmt("c.setSingle(%u, rdf32(", d) + ea + "));"); return;  // lfs
    case 49: E("{ const u32 e = " + uea + fmt("; c.setSingle(%u, rdf32(e)); ", d) + R(a) + " = e; }"); return;
    case 50: E(fmt("c.f[%u] = rdf64(", d) + ea + ");"); return;  // lfd
    case 51: E("{ const u32 e = " + uea + fmt("; c.f[%u] = rdf64(e); ", d) + R(a) + " = e; }"); return;
    case 52: E("wrf32(" + ea + fmt(", float(c.f[%u]));", d)); return;  // stfs
    case 53: E("{ const u32 e = " + uea + fmt("; wrf32(e, float(c.f[%u])); ", d) + R(a) + " = e; }"); return;
    case 54: E("wrf64(" + ea + fmt(", c.f[%u]);", d)); return;
    case 55: E("{ const u32 e = " + uea + fmt("; wrf64(e, c.f[%u]); ", d) + R(a) + " = e; }"); return;
    default: break;
    }
    if (op == 56 || op == 57 || op == 60 || op == 61) {  // psq_l / psq_lu / psq_st / psq_stu
        const u32 wbit = bits(w, 16, 16), i = bits(w, 17, 19);
        const u32 off = u32(sext(w & 0xFFF, 12));
        const std::string base = (op == 56 || op == 60) ? ra0(a) : R(a);
        const std::string e = "(" + base + " + " + h8(off) + ")";
        const bool upd = op == 57 || op == 61;
        const char* fn = op <= 57 ? "psqLoad" : "psqStore";
        E("{ const u32 e = " + e + fmt("; c.%s(%u, e, %u, %u);", fn, d, wbit, i) + (upd ? " " + R(a) + " = e;" : "") + " }");
        return;
    }
    if (op == 31) return op31(w, d, a, b);
    if (op == 59 || op == 63) return fp(w, op, d, a, b);
    if (op == 4) return ps(w, d, a, b);
    if (op == 3) {
        E("// twi");
        return;
    }
    if (op == 17) {
        E("// sc");
        return;
    }
    throw Unsupported{fmt("op%u", op)};
}

void FuncEmitter::op31(u32 w, u32 d, u32 a, u32 b) {
    const u32 xo10 = bits(w, 21, 30);
    const u32 xo9 = bits(w, 22, 30);
    const bool rc = w & 1;
    auto recd = [&](u32 reg) {
        if (rc) E(rec(reg));
    };
    if (xo10 == 0) {
        E(fmt("c.cmps(%u, s32(", d >> 2) + R(a) + "), s32(" + R(b) + "));");
        return;
    }
    if (xo10 == 32) {
        E(fmt("c.cmpu(%u, ", d >> 2) + R(a) + ", " + R(b) + ");");
        return;
    }
    {
        std::string e;
        switch (xo9) {
        case 266: e = R(a) + " + " + R(b); break;                                        // add
        case 40: e = R(b) + " - " + R(a); break;                                         // subf
        case 235: e = "u32(s32(" + R(a) + ") * s32(" + R(b) + "))"; break;               // mullw
        case 104: e = "u32(-s32(" + R(a) + "))"; break;                                  // neg
        case 75: e = "u32((s64(s32(" + R(a) + ")) * s64(s32(" + R(b) + "))) >> 32)"; break;  // mulhw
        case 11: e = "u32((u64(" + R(a) + ") * u64(" + R(b) + ")) >> 32)"; break;         // mulhwu
        case 491: e = "c.divw(" + R(a) + ", " + R(b) + ")"; break;                       // divw
        case 459: e = "c.divwu(" + R(a) + ", " + R(b) + ")"; break;                      // divwu
        default: break;
        }
        if (!e.empty()) {
            E(R(d) + " = " + e + ";");
            recd(d);
            return;
        }
    }
    switch (xo9) {
    case 10: E("{ const u32 s = " + R(a) + "; " + R(d) + " = s + " + R(b) + "; c.ca = " + R(d) + " < s; }"); recd(d); return;
    case 138: E("{ const u64 t = u64(" + R(a) + ") + " + R(b) + " + c.ca; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 202: E("{ const u64 t = u64(" + R(a) + ") + c.ca; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 234: E("{ const u64 t = u64(" + R(a) + ") + c.ca + 0xFFFFFFFFull; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 8: E("{ const u32 x = " + R(a) + ", y = " + R(b) + "; " + R(d) + " = y - x; c.ca = y >= x; }"); recd(d); return;
    case 136: E("{ const u64 t = u64(~" + R(a) + ") + " + R(b) + " + c.ca; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 200: E("{ const u64 t = u64(~" + R(a) + ") + c.ca; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    case 232: E("{ const u64 t = u64(~" + R(a) + ") + c.ca + 0xFFFFFFFFull; " + R(d) + " = u32(t); c.ca = t >> 32; }"); recd(d); return;
    default: break;
    }
    // logical: d = rS, a = rA
    {
        const char* o = nullptr;
        switch (xo10) {
        case 28: o = "&"; break;
        case 444: o = "|"; break;
        case 316: o = "^"; break;
        case 60: o = "& ~"; break;
        case 412: o = "| ~"; break;
        default: break;
        }
        if (o) {
            E(R(a) + " = " + R(d) + " " + o + R(b) + ";");
            recd(a);
            return;
        }
    }
    switch (xo10) {
    case 124: E(R(a) + " = ~(" + R(d) + " | " + R(b) + ");"); recd(a); return;  // nor
    case 476: E(R(a) + " = ~(" + R(d) + " & " + R(b) + ");"); recd(a); return;  // nand
    case 284: E(R(a) + " = ~(" + R(d) + " ^ " + R(b) + ");"); recd(a); return;  // eqv
    case 24: E(R(a) + " = (" + R(b) + " & 0x20) ? 0u : (" + R(d) + " << (" + R(b) + " & 31));"); recd(a); return;  // slw
    case 536: E(R(a) + " = (" + R(b) + " & 0x20) ? 0u : (" + R(d) + " >> (" + R(b) + " & 31));"); recd(a); return;  // srw
    case 792: E(fmt("c.sraw(%u, ", a) + R(d) + ", " + R(b) + " & 0x3F);"); recd(a); return;  // sraw
    case 824: E(fmt("c.sraw(%u, ", a) + R(d) + fmt(", %u);", b)); recd(a); return;          // srawi
    case 26: E(R(a) + " = c.cntlzw(" + R(d) + ");"); recd(a); return;
    case 922: E(R(a) + " = u32(s32(s16(" + R(d) + ")));"); recd(a); return;  // extsh
    case 954: E(R(a) + " = u32(s32(s8(" + R(d) + ")));"); recd(a); return;   // extsb
    default: break;
    }
    // indexed loads/stores
    const std::string ea = "(" + ra0(a) + " + " + R(b) + ")";
    const std::string uea = "(" + R(a) + " + " + R(b) + ")";
    auto lx = [](u32 x, const std::string& addr) -> std::string {
        switch (x) {
        case 23: case 55: return "rd32(" + addr + ")";
        case 87: case 119: return "rd8(" + addr + ")";
        case 279: case 311: return "rd16(" + addr + ")";
        case 343: case 375: return "u32(s16(rd16(" + addr + ")))";
        case 534: return "rd32r(" + addr + ")";
        case 790: return "rd16r(" + addr + ")";
        default: return {};
        }
    };
    if (xo10 == 23 || xo10 == 87 || xo10 == 279 || xo10 == 343 || xo10 == 534 || xo10 == 790) {
        E(R(d) + " = " + lx(xo10, ea) + ";");
        return;
    }
    if (xo10 == 55 || xo10 == 119 || xo10 == 311 || xo10 == 375) {
        E("{ const u32 e = " + uea + "; " + R(d) + " = " + lx(xo10, "e") + "; " + R(a) + " = e; }");
        return;
    }
    {
        const char* fn = nullptr;
        switch (xo10) {
        case 151: fn = "wr32"; break;
        case 215: fn = "wr8"; break;
        case 407: fn = "wr16"; break;
        case 662: fn = "wr32r"; break;
        case 918: fn = "wr16r"; break;
        default: break;
        }
        if (fn) {
            E(std::string(fn) + "(" + ea + ", " + R(d) + ");");
            return;
        }
        switch (xo10) {
        case 183: fn = "wr32"; break;
        case 247: fn = "wr8"; break;
        case 439: fn = "wr16"; break;
        default: break;
        }
        if (fn) {
            E("{ const u32 e = " + uea + "; " + fn + "(e, " + R(d) + "); " + R(a) + " = e; }");
            return;
        }
    }
    switch (xo10) {
    case 535: E(fmt("c.setSingle(%u, rdf32(", d) + ea + "));"); return;  // lfsx
    case 567: E("{ const u32 e = " + uea + fmt("; c.setSingle(%u, rdf32(e)); ", d) + R(a) + " = e; }"); return;
    case 599: E(fmt("c.f[%u] = rdf64(", d) + ea + ");"); return;
    case 631: E("{ const u32 e = " + uea + fmt("; c.f[%u] = rdf64(e); ", d) + R(a) + " = e; }"); return;
    case 663: E("wrf32(" + ea + fmt(", float(c.f[%u]));", d)); return;
    case 695: E("{ const u32 e = " + uea + fmt("; wrf32(e, float(c.f[%u])); ", d) + R(a) + " = e; }"); return;
    case 727: E("wrf64(" + ea + fmt(", c.f[%u]);", d)); return;
    case 759: E("{ const u32 e = " + uea + fmt("; wrf64(e, c.f[%u]); ", d) + R(a) + " = e; }"); return;
    case 983: E("wr32(" + ea + fmt(", u32(bits64(c.f[%u])));", d)); return;  // stfiwx
    default: break;
    }
    // psq_lx / psq_stx
    {
        const u32 sub = bits(w, 25, 30);
        if (sub == 6 || sub == 7 || sub == 38 || sub == 39) {
            const u32 wbit = bits(w, 21, 21), i = bits(w, 22, 24);
            const std::string e = (sub == 6 || sub == 7) ? ea : uea;
            const bool upd = sub == 38 || sub == 39;
            const char* fn = (sub == 6 || sub == 38) ? "psqLoad" : "psqStore";
            E("{ const u32 e = " + e + fmt("; c.%s(%u, e, %u, %u);", fn, d, wbit, i) + (upd ? " " + R(a) + " = e;" : "") + " }");
            return;
        }
    }
    // special registers
    const u32 spr = bits(w, 16, 20) << 5 | bits(w, 11, 15);
    if (xo10 == 339) {  // mfspr
        if (spr == 8) E(R(d) + " = c.lr;");
        else if (spr == 9) E(R(d) + " = c.ctr;");
        else if (spr == 1) E(R(d) + " = c.getXer();");
        else if (spr >= 912 && spr <= 919) E(R(d) + fmt(" = c.gqr[%u];", spr - 912));
        else E(R(d) + fmt(" = c.mfspr(%u);", spr));
        return;
    }
    if (xo10 == 467) {  // mtspr
        if (spr == 8) E("c.lr = " + R(d) + ";");
        else if (spr == 9) E("c.ctr = " + R(d) + ";");
        else if (spr == 1) E("c.setXer(" + R(d) + ");");
        else if (spr >= 912 && spr <= 919) E(fmt("c.gqr[%u] = ", spr - 912) + R(d) + ";");
        else E(fmt("c.mtspr(%u, ", spr) + R(d) + ");");
        return;
    }
    if (xo10 == 19) {
        E(R(d) + " = c.getCr();");
        return;
    }
    if (xo10 == 144) {  // mtcrf
        E(fmt("c.mtcrf(0x%02X, ", bits(w, 12, 19)) + R(d) + ");");
        return;
    }
    if (xo10 == 371) {
        E(R(d) + fmt(" = c.mftb(%u);", spr));
        return;
    }
    if (xo10 == 83 || xo10 == 146 || xo10 == 595 || xo10 == 210 || xo10 == 659 || xo10 == 242) {
        if (xo10 == 83) E(R(d) + " = c.msr;");
        else if (xo10 == 146) E("c.msr = " + R(d) + ";");
        else E("// sr access");
        return;
    }
    switch (xo10) {
    case 86: case 54: case 470: case 278: case 246: case 982: case 598: case 854: case 1014: case 758:
        if (xo10 == 1014) E("c.dcbz(" + ea + ");");
        else E("// cache/sync");
        return;
    case 4: E("// tw"); return;
    case 512: E(fmt("c.cr[%u] = u8((c.so ? 8 : 0) | (c.ov ? 4 : 0) | (c.ca ? 2 : 0)); c.ca = 0;", d >> 2)); return;
    default: break;
    }
    throw Unsupported{fmt("op31 xo%u", xo10)};
}

void FuncEmitter::fp(u32 w, u32 op, u32 d, u32 a, u32 b) {
    const u32 cc = bits(w, 21, 25);
    const u32 xo5 = bits(w, 26, 30);
    const u32 xo10 = bits(w, 21, 30);
    const bool rc = w & 1;
    const bool single = op == 59;
    const std::string A = fmt("c.f[%u]", a), B = fmt("c.f[%u]", b), C = fmt("c.f[%u]", cc);
    auto res = [&](const std::string& expr) {
        if (single) E(fmt("c.setSingle(%u, float(", d) + expr + "));");
        else E(fmt("c.f[%u] = ", d) + expr + ";");
        if (rc) E("c.setCr1();");
    };
    std::string e;
    switch (xo5) {
    case 21: e = A + " + " + B; break;
    case 20: e = A + " - " + B; break;
    case 25: e = A + " * " + C; break;
    case 18: e = A + " / " + B; break;
    case 29: e = "fma_(" + A + ", " + C + ", " + B + ")"; break;
    case 28: e = "fma_(" + A + ", " + C + ", -" + B + ")"; break;
    case 31: e = "-fma_(" + A + ", " + C + ", " + B + ")"; break;
    case 30: e = "-fma_(" + A + ", " + C + ", -" + B + ")"; break;
    case 24: if (single) e = "1.0 / " + B; break;
    case 26: e = "1.0 / sqrt_(" + B + ")"; break;
    case 22: e = "sqrt_(" + B + ")"; break;
    default: break;
    }
    if (!e.empty()) {
        res(e);
        return;
    }
    if (xo5 == 23) {  // fsel
        res("(" + A + " >= 0.0) ? " + C + " : " + B);
        return;
    }
    if (op == 63) {
        if (xo10 == 0 || xo10 == 32) {
            E(fmt("c.fcmp(%u, ", d >> 2) + A + ", " + B + (xo10 == 32 ? ", true);" : ", false);"));
            return;
        }
        std::string s;
        switch (xo10) {
        case 72: s = B; break;
        case 40: s = "-" + B; break;
        case 264: s = "fabs_(" + B + ")"; break;
        case 136: s = "-fabs_(" + B + ")"; break;
        default: break;
        }
        if (!s.empty()) {
            E(fmt("c.f[%u] = ", d) + s + ";");
            if (rc) E("c.setCr1();");
            return;
        }
        if (xo10 == 12) {  // frsp
            E(fmt("c.setSingle(%u, float(", d) + B + "));");
            return;
        }
        if (xo10 == 14 || xo10 == 15) {  // fctiw / fctiwz
            E(fmt("c.f[%u] = c.fctiw(", d) + B + (xo10 == 15 ? ", 1);" : ", 0);"));
            return;
        }
        if (xo10 == 583) {  // mffs
            E(fmt("c.f[%u] = dbl(u64(c.fpscr));", d));
            return;
        }
        if (xo10 == 711) {  // mtfsf
            E("c.fpscr = u32(bits64(" + B + "));");
            return;
        }
        if (xo10 == 70 || xo10 == 38 || xo10 == 134 || xo10 == 64) {
            E("// fpscr bit op");
            return;
        }
    }
    throw Unsupported{fmt("fp op%u xo%u", op, xo10)};
}

void FuncEmitter::ps(u32 w, u32 d, u32 a, u32 b) {
    const u32 c_ = bits(w, 21, 25);
    const u32 xo5 = bits(w, 26, 30);
    const u32 xo10 = bits(w, 21, 30);
    const std::string A0 = fmt("c.f[%u]", a), A1 = fmt("c.ps1[%u]", a);
    const std::string B0 = fmt("c.f[%u]", b), B1 = fmt("c.ps1[%u]", b);
    const std::string C0 = fmt("c.f[%u]", c_), C1 = fmt("c.ps1[%u]", c_);
    auto pair = [&](const std::string& e0, const std::string& e1) {
        E("{ const double v0 = double(float(" + e0 + ")), v1 = double(float(" + e1 + fmt(")); c.f[%u] = v0; c.ps1[%u] = v1; }", d, d));
    };
    auto fma = [](const std::string& x, const std::string& y, const std::string& z) {
        return "fma_(" + x + ", " + y + ", " + z + ")";
    };
    std::string e0, e1;
    switch (xo5) {
    case 21: e0 = A0 + " + " + B0; e1 = A1 + " + " + B1; break;
    case 20: e0 = A0 + " - " + B0; e1 = A1 + " - " + B1; break;
    case 25: e0 = A0 + " * " + C0; e1 = A1 + " * " + C1; break;
    case 18: e0 = A0 + " / " + B0; e1 = A1 + " / " + B1; break;
    case 29: e0 = fma(A0, C0, B0); e1 = fma(A1, C1, B1); break;
    case 28: e0 = fma(A0, C0, "-" + B0); e1 = fma(A1, C1, "-" + B1); break;
    case 31: e0 = "-" + fma(A0, C0, B0); e1 = "-" + fma(A1, C1, B1); break;
    case 30: e0 = "-" + fma(A0, C0, "-" + B0); e1 = "-" + fma(A1, C1, "-" + B1); break;
    case 12: e0 = A0 + " * " + C0; e1 = A1 + " * " + C0; break;  // ps_muls0
    case 13: e0 = A0 + " * " + C1; e1 = A1 + " * " + C1; break;  // ps_muls1
    case 14: e0 = fma(A0, C0, B0); e1 = fma(A1, C0, B1); break;  // ps_madds0
    case 15: e0 = fma(A0, C1, B0); e1 = fma(A1, C1, B1); break;  // ps_madds1
    case 10: e0 = A0 + " + " + B1; e1 = C1; break;               // ps_sum0
    case 11: e0 = C0; e1 = A0 + " + " + B1; break;               // ps_sum1
    case 24: e0 = "1.0 / " + B0; e1 = "1.0 / " + B1; break;      // ps_res
    case 26: e0 = "1.0 / sqrt_(" + B0 + ")"; e1 = "1.0 / sqrt_(" + B1 + ")"; break;  // ps_rsqrte
    case 23: e0 = "(" + A0 + " >= 0.0) ? " + C0 + " : " + B0; e1 = "(" + A1 + " >= 0.0) ? " + C1 + " : " + B1; break;  // ps_sel
    default: break;
    }
    if (!e0.empty()) {
        pair(e0, e1);
        if (w & 1) E("c.setCr1();");
        return;
    }
    switch (xo10) {
    case 40: e0 = "-" + B0; e1 = "-" + B1; break;                               // ps_neg
    case 72: e0 = B0; e1 = B1; break;                                           // ps_mr
    case 136: e0 = "-fabs_(" + B0 + ")"; e1 = "-fabs_(" + B1 + ")"; break;      // ps_nabs
    case 264: e0 = "fabs_(" + B0 + ")"; e1 = "fabs_(" + B1 + ")"; break;        // ps_abs
    case 528: e0 = A0; e1 = B0; break;                                          // ps_merge00
    case 560: e0 = A0; e1 = B1; break;                                          // ps_merge01
    case 592: e0 = A1; e1 = B0; break;                                          // ps_merge10
    case 624: e0 = A1; e1 = B1; break;                                          // ps_merge11
    default: break;
    }
    if (!e0.empty()) {
        E("{ const double v0 = " + e0 + ", v1 = " + e1 + fmt("; c.f[%u] = v0; c.ps1[%u] = v1; }", d, d));
        return;
    }
    if (xo10 == 0 || xo10 == 32 || xo10 == 64 || xo10 == 96) {  // ps_cmpu0, ps_cmpo0, ps_cmpu1, ps_cmpo1
        const bool first = xo10 == 0 || xo10 == 32;
        E(fmt("c.fcmp(%u, ", d >> 2) + (first ? A0 : A1) + ", " + (first ? B0 : B1) +
          ((xo10 == 32 || xo10 == 96) ? ", true);" : ", false);"));
        return;
    }
    if (xo10 == 1014) {  // dcbz_l
        E(fmt("c.dcbz(c.r[%u] + c.r[%u]);", a, b));
        return;
    }
    const u32 sub6 = bits(w, 25, 30);
    if (sub6 == 6 || sub6 == 7 || sub6 == 38 || sub6 == 39) {  // psq_lx etc. encoded under op4
        const u32 wbit = bits(w, 21, 21), i = bits(w, 22, 24);
        const std::string e = (sub6 == 6 || sub6 == 7) ? "(" + ra0(a) + fmt(" + c.r[%u])", b) : fmt("(c.r[%u] + c.r[%u])", a, b);
        const bool upd = sub6 == 38 || sub6 == 39;
        const char* fn = (sub6 == 6 || sub6 == 38) ? "psqLoad" : "psqStore";
        E("{ const u32 e = " + e + fmt("; c.%s(%u, e, %u, %u);", fn, d, wbit, i) + (upd ? fmt(" c.r[%u] = e;", a) : "") + " }");
        return;
    }
    throw Unsupported{fmt("ps xo%u/%u", xo10, xo5)};
}

std::string chunkName(int i) { return fmt("funcs_%03d.cpp", i); }

// Symbol names inside generated C++: string literals (escaped) and comments (no backslash, which
// would continue the comment onto the next line). Map names may hold any character
// ("__sinit_\ut_FileStream_cpp").
std::string lit(const std::string& n) {
    std::string o;
    for (char ch : n) {
        if (ch == '\\' || ch == '"') o += '\\';
        if (ch == '?') {  // no trigraphs
            o += "\\?";
            continue;
        }
        o += ch;
    }
    return o;
}
std::string commentText(std::string n) {
    for (char& ch : n)
        if (ch == '\\' || ch == '\n' || ch == '\r') ch = '/';
    return n;
}

void Recompiler::run(Output& out, const std::string& version) {
    struct Item {
        Space* sp;
        u32 key;
    };
    std::vector<Item> items;
    for (auto& [a, f] : dolSpace.funcs) items.push_back({&dolSpace, a});
    for (Space& rs : relSpaces)
        for (auto& [k, f] : rs.funcs) items.push_back({&rs, k});
    // Hookable: hooks.txt names, and (without SDK units from a splits.txt) every named DOL
    // function, so an HLE handler of that name replaces it at run time.
    for (auto& [a, f] : dolSpace.funcs)
        if (!dolSpace.hle.count(a) && (hooks.count(f.name) || !autoName(f.name))) dolSpace.hooked.insert(a);
    for (Space& rs : relSpaces)
        for (auto& [k, f] : rs.funcs)
            if (hooks.count(rs.mod->name + "::" + f.name)) rs.hooked.insert(k);

    const char* banner = "// Generated from the user's main.dol and REL modules by the recompiler. Do not commit.\n";
    std::string header = std::string("#pragma once\n") + banner + "#include \"recomp/ctx.h\"\nnamespace recomp {\n";
    for (const Item& it : items) {
        header += "void " + it.sp->fname(it.key) + "(Ctx& c);\n";
        if (it.sp->hooked.count(it.key)) header += "void " + it.sp->fname(it.key) + "_orig(Ctx& c);\n";
    }
    header += "}\n";
    out.files.push_back({"funcs.h", std::move(header)});

    // Contiguous chunks of roughly equal instruction count.
    u64 total = 0;
    for (const Item& it : items) total += it.sp->funcs[it.key].size / 4 + 8;
    size_t idx = 0;
    u64 done = 0;
    for (int ch = 0; ch < kChunks; ch++) {
        const u64 goal = total * u64(ch + 1) / kChunks;
        std::string text = std::string(banner) + "#include \"funcs.h\"\nnamespace recomp {\n";
        while (idx < items.size() && (done < goal || ch == kChunks - 1)) {
            Space& sp = *items[idx].sp;
            const u32 a = items[idx++].key;
            const Func& f = sp.funcs[a];
            done += f.size / 4 + 8;
            const std::string fn = sp.fname(a);
            const std::string addr = sp.isRel() ? fmt("relBase(%uu, %uu) + 0x%Xu", sp.mod->id, keySec(a), keyOff(a))
                                                : fmt("0x%08Xu", a);
            text += "// " + commentText((sp.isRel() ? sp.mod->name + ": " : std::string()) + f.name) + "\n";
            text += "void " + fn + "(Ctx& c) {\n";
            text += "    c.enter(" + addr + ");\n";
            if (sp.hle.count(a)) {
                text += "    static const HleFn s = hleLookup(\"" + lit(f.name) + "\");\n";
                text += "    if (s) { s(c); } else { hleMissing(\"" + lit(f.name) + "\", " + addr + ", c); }\n";
            } else {
                if (sp.hooked.count(a)) {
                    // The translated body lives in <fn>_orig so a hook can call it.
                    const std::string hname = sp.isRel() ? sp.mod->name + "::" + f.name : f.name;
                    text += "    static const HleFn hook = hleLookup(\"" + lit(hname) + "\");\n";
                    text += "    if (hook) { hook(c); return; }\n";
                    text += "    " + fn + "_orig(c);\n}\n";
                    text += "void " + fn + "_orig(Ctx& c) {\n";
                }
                for (auto& line : FuncEmitter(*this, sp, a, f.size).translate()) {
                    text += line;
                    text += '\n';
                }
            }
            text += "}\n";
        }
        text += "}\n";
        out.files.push_back({chunkName(ch), std::move(text)});
    }

    std::string t = std::string(banner) + "#include \"funcs.h\"\nnamespace recomp {\nextern const FuncEntry kFuncTable[] = {\n";
    for (auto& [a, f] : dolSpace.funcs) t += fmt("    {0x%08Xu, &", a) + dolSpace.fname(a) + "},\n";
    t += "};\n";
    t += "extern const SymEntry kSymTable[] = {\n";
    for (auto& [a, f] : dolSpace.funcs) t += "    {\"" + lit(f.name) + fmt("\", 0x%08Xu},\n", a);
    t += "};\n";
    t += "extern const FuncEntry kOrigTable[] = {\n";
    size_t origCount = 0;
    for (auto& [a, f] : dolSpace.funcs)
        if (dolSpace.hooked.count(a)) {
            t += fmt("    {0x%08Xu, &", a) + dolSpace.fname(a) + "_orig},\n";
            origCount++;
        }
    // Hooked REL functions ("<module>::<name>" in hooks.txt): keyed by relfmt::relOrigKey (rel_format.h), which no
    // DOL address can equal, so callOriginal finds REL originals in the same table (no ABI change).
    for (Space& rs : relSpaces)
        for (auto& [k, f] : rs.funcs)
            if (rs.hooked.count(k)) {
                t += fmt("    {0x%08Xu, &", relfmt::relOrigKey(rs.mod->id, keySec(k), keyOff(k))) + rs.fname(k) + "_orig},\n";
                origCount++;
            }
    if (!origCount) t += "    {0, nullptr},\n";
    t += "};\n";
    // REL modules: per module its functions by key (section << 24 | offset), sorted, and names.
    for (Space& rs : relSpaces) {
        const u32 id = rs.mod->id;
        t += fmt("static const RelFuncEntry kRel%03uFuncs[] = {\n", id);
        for (auto& [k, f] : rs.funcs) t += fmt("    {0x%08Xu, &", k) + rs.fname(k) + "},\n";
        if (rs.funcs.empty()) t += "    {0, nullptr},\n";
        t += "};\n";
        t += fmt("static const char* const kRel%03uNames[] = {\n", id);
        for (auto& [k, f] : rs.funcs) t += "    \"" + lit(f.name) + "\",\n";
        if (rs.funcs.empty()) t += "    \"\",\n";
        t += "};\n";
    }
    t += "extern const RelModuleDesc kRelModules[] = {\n";
    for (Space& rs : relSpaces) {
        const u32 id = rs.mod->id;
        t += fmt("    {%uu, \"", id) + rs.mod->name +
             fmt("\", 0x%08Xu, %zuu, kRel%03uFuncs, kRel%03uNames, %zuu},\n", rs.mod->checksum, rs.mod->secs.size(), id, id,
                 rs.funcs.size());
    }
    if (relSpaces.empty()) t += "    {0, \"\", 0, 0, nullptr, nullptr, 0},\n";
    t += "};\n";
    t += "extern const GameModule kGameModule = {\n";
    t += "    kModuleAbiVersion, \"" + version + "\",\n";
    t += fmt("    kFuncTable, %zuu, kSymTable, %zuu, kOrigTable, %zuu,\n", dolSpace.funcs.size(), dolSpace.funcs.size(), origCount);
    t += fmt("    kRelModules, %zuu,\n", relSpaces.size());
    t += "};\n}\n";
    out.files.push_back({"func_table.cpp", std::move(t)});
    out.functions = unsigned(items.size());
}

}  // namespace

std::vector<std::string> outputFileNames() {
    std::vector<std::string> names{"funcs.h"};
    for (int i = 0; i < kChunks; i++) names.push_back(chunkName(i));
    names.push_back("func_table.cpp");
    return names;
}

bool recompile(const Input& in, Output& out, std::string& error) {
    try {
        const Dol dol(in.dol);
        std::map<u32, Func> all;
        std::map<std::string, std::pair<u32, u32>> objs;
        loadSymbols(in.symbols, all, objs);
        std::map<u32, Func> funcs;
        for (auto& [a, f] : all)
            if (dol.has(a)) funcs[a] = f;
        std::map<u32, std::pair<std::string, std::string>> vmap;
        const bool haveMap = !in.versionMap.empty();
        if (haveMap) {
            for (std::string_view line : splitLines(in.versionMap)) {
                line = trim(line);
                if (line.empty() || line[0] == '#') continue;
                const size_t s1 = line.find(' ');
                const size_t s2 = s1 == std::string_view::npos ? s1 : line.find(' ', s1 + 1);
                u32 addr;
                if (s2 == std::string_view::npos || !parseHex(line.substr(0, s1), addr)) continue;
                vmap[addr] = {std::string(line.substr(s1 + 1, s2 - s1 - 1)), std::string(trim(line.substr(s2 + 1)))};
            }
        }
        std::vector<RelModule> rels;
        std::unordered_set<u32> ids;
        for (const RelInput& ri : in.rels) {
            rels.push_back(parseRel(ri));
            if (!ids.insert(rels.back().id).second || rels.back().id == 0)
                throw std::runtime_error(fmt("%s: module id %u is used twice", ri.name.c_str(), rels.back().id));
        }
        Recompiler rc(dol, rels);
        // hooks.txt: "<symbol> [us] [pal] [jp]". A line applies to the US build unless it lists
        // only other versions, and to PAL / Japan only when tagged "pal" / "jp".
        const bool pal = in.version.size() >= 4 && in.version[3] == 'P';
        const bool jp = in.version.size() >= 4 && in.version[3] == 'J';
        std::vector<std::pair<u32, std::pair<u32, u32>>> patchList;
        for (std::string_view line : splitLines(in.hooks)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string_view> words;
            size_t p = 0;
            while (p < line.size()) {
                size_t q = line.find_first_of(" \t", p);
                if (q == std::string_view::npos) q = line.size();
                if (q > p) words.push_back(line.substr(p, q - p));
                p = q + 1;
            }
            bool tagUs = false, tagPal = false, tagJp = false;
            const bool isPatch = words[0] == "patch";
            for (size_t i = isPatch ? 4 : 1; i < words.size(); i++) {
                if (words[i] == "us") tagUs = true;
                else if (words[i] == "pal") tagPal = true;
                else if (words[i] == "jp") tagJp = true;
            }
            const bool applies = pal ? tagPal : jp ? tagJp : (tagUs || !(tagPal || tagJp));
            if (!applies) continue;
            if (isPatch) {  // "patch <id> <address> <instruction word> [us] [pal]"
                u32 addr = 0, word = 0;
                if (words.size() >= 4 && parseHex(words[2], addr) && parseHex(words[3], word)) {
                    const u32 id = u32(std::strtoul(std::string(words[1]).c_str(), nullptr, 10));
                    patchList.push_back({addr, {id, word}});
                }
                continue;
            }
            rc.hooks.insert(std::string(words[0]));
        }
        rc.setupDol(std::move(funcs), objs, haveMap ? std::vector<TuRange>{} : loadHleRanges(in.splits), haveMap ? &vmap : nullptr);
        for (size_t i = 0; i < rels.size(); i++) {
            std::map<u32, Func> rf;
            std::map<std::string, std::pair<u32, u32>> ro;
            if (!in.rels[i].map.empty()) loadSymbols(in.rels[i].map, rf, ro, &rels[i]);
            rc.setupRel(rc.relSpaces[i], std::move(rf), ro);
        }
        for (auto& [addr, pw] : patchList) rc.dolSpace.patches[addr].push_back(pw);
        // Every DOL hook must name one of its functions (a hook that matches nothing would
        // silently never run). REL hooks are "<module>::<name>".
        {
            std::unordered_set<std::string> names;
            for (const auto& [a, f] : rc.dolSpace.funcs) names.insert(f.name);
            for (const Space& rs : rc.relSpaces)
                for (const auto& [k, f] : rs.funcs) names.insert(rs.mod->name + "::" + f.name);
            std::string missing;
            for (const std::string& h : rc.hooks)
                if (!names.count(h)) missing += (missing.empty() ? "" : ", ") + h;
            if (!missing.empty()) {
                error = "hooks.txt names functions the " + in.version + " build does not have (" + missing + ")";
                return false;
            }
        }
        for (const auto& [addr, list] : rc.dolSpace.patches) {
            auto it = rc.dolSpace.funcs.upper_bound(addr);
            if (it == rc.dolSpace.funcs.begin() || addr >= std::prev(it)->first + std::prev(it)->second.size) {
                error = fmt("hooks.txt patch at %08X is outside every %s function", addr, in.version.c_str());
                return false;
            }
        }
        rc.run(out, in.version);
        const Space& d = rc.dolSpace;
        size_t relFuncs = 0, relTables = 0, relEntries = 0, relScanned = 0;
        for (const Space& rs : rc.relSpaces) {
            relFuncs += rs.funcs.size();
            relTables += rs.jumpTables.size();
            relEntries += rs.addressEntries;
            relScanned += rs.scanned;
        }
        out.summary = fmt("recomp %s: %u functions -> %d files\n"
                          "  DOL: %zu functions (%zu from the scan, %zu HLE units, %zu hookable by name), %zu jump tables "
                          "(%zu detected), %zu address-taken interior entries\n"
                          "  RELs: %zu modules, %zu functions (%zu from the scan), %zu jump tables, %zu address-taken "
                          "interior entries",
                          in.version.c_str(), out.functions, kChunks, d.funcs.size(), d.scanned, d.hle.size(), d.hooked.size(),
                          d.jumpTables.size(), d.detectedTables, d.addressEntries, rc.relSpaces.size(), relFuncs, relScanned,
                          relTables, relEntries);
        for (auto& [k, v] : rc.unsupported) out.summary += fmt("\n  unsupported %s: %u", k.c_str(), v);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace cfrc
