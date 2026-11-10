// REL modules at run time (docs/RECOMPILER.md "REL modules"): the OSLink / OSLinkFixed /
// OSUnlink HLE and the table of linked modules.
//
// The game loads a module file into a heap buffer (DVD) and calls OSLink(module, bss). Like the
// SDK's OSLink this places the module where it lies: section offsets, the import table and the
// prolog/epilog/unresolved pointers become absolute, the bss section is placed and cleared,
// and every relocation is applied in guest memory (the module's imports from the DOL, from
// itself and from modules linked before it, and the imports of those modules from it). The
// recompiled code of the module does not read its patched instructions: it was translated at
// its section offsets and reads the section bases from g_relBase, which is filled here. An
// indirect call to an address inside a linked module (vtables, function pointers, prolog)
// finds the recompiled function through the module's table (lookupRelFunc).
//
// Generic: nothing here is specific to one game. The format is in src/recompiler/rel_format.h.
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/wiimote.h"
#include "recompiler/rel_format.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace recomp {

u32 g_relBaseStore[kRelMaxModules * kRelMaxSections];
u32* g_relBase = g_relBaseStore;

namespace {

// OS globals the SDK keeps the module list in (low memory).
constexpr u32 kModuleListHead = 0x800030C8, kModuleListTail = 0x800030CC, kStringTable = 0x800030D0;

struct CodeRange {
    u32 lo, hi, sec;
};
struct Linked {
    u32 id = 0;
    u32 header = 0;  // guest address of the module (OSModuleHeader)
    const RelModuleDesc* desc = nullptr;
    std::vector<CodeRange> code;
};

// Modified only by guest threads inside the HLE below (one guest thread runs at a time); read by
// guest threads for indirect calls without a lock, and under sLinkedM by host diagnostics.
std::vector<Linked> sLinked;
std::mutex sLinkedM;

bool logOn() {
    static const bool on = std::getenv("CF_REL_LOG") != nullptr;
    return on;
}

u32 hdr32(u32 m, u32 off) { return rd32(m + off); }
u32 secInfoAddr(u32 m, u32 sec) { return hdr32(m, 0x10) + 8 * sec; }  // after Link: absolute
u32 secBase(u32 m, u32 sec) { return rd32(secInfoAddr(m, sec)) & ~1u; }

const RelModuleDesc* findDesc(u32 id) {
    if (!g_module || !g_module->rels) return nullptr;
    for (u32 i = 0; i < g_module->relCount; i++)
        if (g_module->rels[i].id == id && g_module->rels[i].funcs) return &g_module->rels[i];
    return nullptr;
}

std::string moduleName(u32 id) {
    if (const RelModuleDesc* d = findDesc(id)) return d->name;
    char b[32];
    std::snprintf(b, sizeof(b), "module %u", id);
    return b;
}

// Applies the relocations `mod` imports from `target` (0: the DOL) to guest memory.
void relocate(u32 target, u32 mod) {
    const u32 idNew = target ? hdr32(target, 0x00) : 0;
    const u32 imp = hdr32(mod, 0x28), impSize = hdr32(mod, 0x2C);
    u32 list = 0;
    for (u32 k = 0; k + 8 <= impSize; k += 8)
        if (rd32(imp + k) == idNew) {
            list = rd32(imp + k + 4);
            break;
        }
    if (!list) return;
    u32 p = 0;
    for (u32 rel = list;; rel += 8) {
        const u32 off = rd16(rel), type = rd8(rel + 2), sec = rd8(rel + 3), addend = rd32(rel + 4);
        p += off;
        u32 x = target ? secBase(target, sec) + addend : addend;
        switch (type) {
        case relfmt::R_PPC_NONE:
        case relfmt::R_DOLPHIN_NOP: break;
        case relfmt::R_PPC_ADDR32: wr32(p, x); break;
        case relfmt::R_PPC_ADDR24: wr32(p, (rd32(p) & ~0x03FFFFFCu) | (x & 0x03FFFFFCu)); break;
        case relfmt::R_PPC_ADDR16: wr16(p, x & 0xFFFF); break;
        case relfmt::R_PPC_ADDR16_LO: wr16(p, x & 0xFFFF); break;
        case relfmt::R_PPC_ADDR16_HI: wr16(p, x >> 16); break;
        case relfmt::R_PPC_ADDR16_HA: wr16(p, ((x + 0x8000) >> 16) & 0xFFFF); break;
        case relfmt::R_PPC_ADDR14:
        case relfmt::R_PPC_ADDR14_BRTAKEN:
        case relfmt::R_PPC_ADDR14_BRNTAKEN: wr32(p, (rd32(p) & ~0xFFFCu) | (x & 0xFFFCu)); break;
        case relfmt::R_PPC_REL24:
            x -= p;
            wr32(p, (rd32(p) & ~0x03FFFFFCu) | (x & 0x03FFFFFCu));
            break;
        case relfmt::R_PPC_REL14:
        case relfmt::R_PPC_REL14_BRTAKEN:
        case relfmt::R_PPC_REL14_BRNTAKEN:
            x -= p;
            wr32(p, (rd32(p) & ~0xFFFCu) | (x & 0xFFFCu));
            break;
        case relfmt::R_DOLPHIN_SECTION: p = secBase(mod, sec); break;
        case relfmt::R_DOLPHIN_END: return;
        default:
            log("[rel] ERROR: unknown relocation type %u in %s", type, moduleName(hdr32(mod, 0)).c_str());
            return;
        }
    }
}

// OSUnlink's undo: the imports `mod` has from the module being unlinked point nowhere again
// (data 0, branches to mod's own _unresolved).
void undo(u32 old, u32 mod) {
    const u32 idOld = hdr32(old, 0x00);
    const u32 imp = hdr32(mod, 0x28), impSize = hdr32(mod, 0x2C);
    u32 list = 0;
    for (u32 k = 0; k + 8 <= impSize; k += 8)
        if (rd32(imp + k) == idOld) {
            list = rd32(imp + k + 4);
            break;
        }
    if (!list) return;
    const u32 unresolved = rd8(mod + 0x32) ? hdr32(mod, 0x3C) : 0;
    u32 p = 0;
    for (u32 rel = list;; rel += 8) {
        const u32 off = rd16(rel), type = rd8(rel + 2), sec = rd8(rel + 3);
        p += off;
        switch (type) {
        case relfmt::R_PPC_ADDR32: wr32(p, 0); break;
        case relfmt::R_PPC_ADDR24: wr32(p, rd32(p) & ~0x03FFFFFCu); break;
        case relfmt::R_PPC_ADDR16:
        case relfmt::R_PPC_ADDR16_LO:
        case relfmt::R_PPC_ADDR16_HI:
        case relfmt::R_PPC_ADDR16_HA: wr16(p, 0); break;
        case relfmt::R_PPC_ADDR14:
        case relfmt::R_PPC_ADDR14_BRTAKEN:
        case relfmt::R_PPC_ADDR14_BRNTAKEN: wr32(p, rd32(p) & ~0xFFFCu); break;
        case relfmt::R_PPC_REL24:
            if (unresolved) wr32(p, (rd32(p) & ~0x03FFFFFCu) | ((unresolved - p) & 0x03FFFFFCu));
            break;
        case relfmt::R_PPC_REL14:
        case relfmt::R_PPC_REL14_BRTAKEN:
        case relfmt::R_PPC_REL14_BRNTAKEN:
            if (unresolved) wr32(p, (rd32(p) & ~0xFFFCu) | ((unresolved - p) & 0xFFFCu));
            break;
        case relfmt::R_DOLPHIN_SECTION: p = secBase(mod, sec); break;
        case relfmt::R_DOLPHIN_END: return;
        default: break;
        }
    }
}

// Registers a module whose section table is absolute (inside Link, before the relocations: the
// checksum is taken over the code as the file holds it).
void registerModule(u32 m) {
    const u32 id = hdr32(m, 0x00), nsec = hdr32(m, 0x0C);
    if (id == 0 || id >= kRelMaxModules || nsec > kRelMaxSections) {
        log("[rel] ERROR: module id %u with %u sections is outside the port's tables", id, nsec);
        return;
    }
    Linked l;
    l.id = id;
    l.header = m;
    l.desc = findDesc(id);
    const u32 table = hdr32(m, 0x10);
    for (u32 s = 0; s < nsec; s++) {
        const u32 off = rd32(table + 8 * s), size = rd32(table + 8 * s + 4);
        g_relBase[id * kRelMaxSections + s] = off & ~1u;
        if ((off & 1) && size) l.code.push_back({off & ~1u, (off & ~1u) + size, s});
    }
    if (!l.desc) {
        log("[rel] ERROR: the game links module %u (%08X), which was not recompiled: calls into it will fail", id, m);
    } else {
        // The code checksum over guest memory (offsets are absolute now).
        const u32 sum = relfmt::codeChecksum(
            [&](u32 off, u8* out, u32 n) {
                // `off` is an address here: the section table holds absolute offsets.
                for (u32 k = 0; k < n; k++) out[k] = u8(rd8(off + k));
            },
            nsec, table);
        static std::unordered_set<u32> sWarned;
        if (sum != l.desc->checksum && sWarned.insert(id).second)
            log("[rel] WARNING: module %s at %08X differs from the file it was recompiled from (checksum %08X, expected "
                "%08X)",
                l.desc->name, m, sum, l.desc->checksum);
    }
    {
        std::lock_guard lk(sLinkedM);
        sLinked.erase(std::remove_if(sLinked.begin(), sLinked.end(), [&](const Linked& x) { return x.id == id; }),
                      sLinked.end());
        sLinked.push_back(std::move(l));
    }
    wiimote::noteScriptEvent(moduleName(id));  // route anchors (`after link <module>`)
    if (logOn()) {
        std::string secs;
        for (u32 s = 0; s < nsec; s++) {
            const u32 off = rd32(table + 8 * s), size = rd32(table + 8 * s + 4);
            if (!size) continue;
            char b[64];
            std::snprintf(b, sizeof(b), " %u:%08X+%X%s", s, off & ~1u, size, (off & 1) ? "x" : "");
            secs += b;
        }
        log("[rel] link %s (id %u) at %08X:%s", moduleName(id).c_str(), id, m, secs.c_str());
    }
}

void unregisterModule(u32 id) {
    if (id >= kRelMaxModules) return;
    for (u32 s = 0; s < kRelMaxSections; s++) g_relBase[id * kRelMaxSections + s] = 0;
    std::lock_guard lk(sLinkedM);
    sLinked.erase(std::remove_if(sLinked.begin(), sLinked.end(), [&](const Linked& x) { return x.id == id; }), sLinked.end());
}

// OSLink / OSLinkFixed (SDK semantics, os/OSLink.c): returns TRUE when the module was linked.
u32 link(u32 m, u32 bss, bool fixed) {
    const u32 version = hdr32(m, 0x1C);
    if (version < 1 || version > 3) {
        log("[rel] OSLink: module at %08X has version %u", m, version);
        return 0;
    }
    if (version >= 2) {
        const u32 align = hdr32(m, 0x40), bssAlign = hdr32(m, 0x44);
        if ((align && m % align) || (bssAlign && bss % bssAlign)) {
            log("[rel] OSLink: module at %08X / bss %08X misaligned (align %u, bss align %u)", m, bss, align, bssAlign);
            return 0;
        }
    }
    // Append to the module list.
    const u32 tail = rd32(kModuleListTail);
    if (tail) wr32(tail + 4, m);
    else wr32(kModuleListHead, m);
    wr32(m + 8, tail);
    wr32(m + 4, 0);
    wr32(kModuleListTail, m);
    // Offsets become addresses.
    const u32 table = hdr32(m, 0x10) + m;
    wr32(m + 0x10, table);
    const u32 nsec = hdr32(m, 0x0C);
    u32 bssAt = bss;
    for (u32 s = 1; s < nsec; s++) {
        const u32 off = rd32(table + 8 * s), size = rd32(table + 8 * s + 4);
        if (off) {
            wr32(table + 8 * s, off + m);
        } else if (size) {
            wr32(table + 8 * s, bssAt);
            bssAt += size;
        }
    }
    wr32(m + 0x24, hdr32(m, 0x24) + m);  // relOffset
    wr32(m + 0x28, hdr32(m, 0x28) + m);  // impOffset
    if (version >= 3) wr32(m + 0x48, hdr32(m, 0x48) + m);  // fixSize
    const u32 imp = hdr32(m, 0x28), impSize = hdr32(m, 0x2C);
    for (u32 k = 0; k + 8 <= impSize; k += 8) wr32(imp + k + 4, rd32(imp + k + 4) + m);
    const auto fixEntry = [&](u32 secByte, u32 field) {
        const u32 sec = rd8(m + secByte);
        if (sec) wr32(m + field, hdr32(m, field) + (rd32(table + 8 * sec) & ~1u));
    };
    fixEntry(0x30, 0x34);  // prolog
    fixEntry(0x31, 0x38);  // epilog
    fixEntry(0x32, 0x3C);  // unresolved
    if (const u32 strings = rd32(kStringTable)) wr32(m + 0x14, hdr32(m, 0x14) + strings);
    registerModule(m);
    // Relocations: from the DOL, then between this module and every linked one (itself included).
    relocate(0, m);
    for (u32 mi = rd32(kModuleListHead); mi; mi = rd32(mi + 4)) {
        relocate(m, mi);
        if (mi != m) relocate(mi, m);
    }
    if (fixed) {
        // OSLinkFixed: the imports from the DOL and from itself (sorted last by makerel) are done
        // for good; the table ends before them so the caller may free what lies past fixSize.
        for (u32 k = 0; k + 8 <= impSize; k += 8) {
            const u32 id = rd32(imp + k);
            if (id == 0 || id == hdr32(m, 0x00)) {
                wr32(m + 0x2C, k);
                break;
            }
        }
    }
    std::memset(hostp(bss), 0, hdr32(m, 0x20));
    return 1;
}

u32 unlink(u32 m) {
    const u32 id = hdr32(m, 0x00);
    // Remove from the module list.
    const u32 next = rd32(m + 4), prev = rd32(m + 8);
    if (next) wr32(next + 8, prev);
    else wr32(kModuleListTail, prev);
    if (prev) wr32(prev + 4, next);
    else wr32(kModuleListHead, next);
    for (u32 mi = rd32(kModuleListHead); mi; mi = rd32(mi + 4)) undo(m, mi);
    if (logOn()) log("[rel] unlink %s (id %u) at %08X", moduleName(id).c_str(), id, m);
    unregisterModule(id);
    // Addresses become offsets again.
    if (const u32 strings = rd32(kStringTable)) wr32(m + 0x14, hdr32(m, 0x14) - strings);
    const u32 table = hdr32(m, 0x10);
    const auto unfix = [&](u32 secByte, u32 field) {
        const u32 sec = rd8(m + secByte);
        if (sec) wr32(m + field, hdr32(m, field) - (rd32(table + 8 * sec) & ~1u));
    };
    unfix(0x30, 0x34);
    unfix(0x31, 0x38);
    unfix(0x32, 0x3C);
    const u32 imp = hdr32(m, 0x28), impSize = hdr32(m, 0x2C);
    for (u32 k = 0; k + 8 <= impSize; k += 8) wr32(imp + k + 4, rd32(imp + k + 4) - m);
    const u32 nsec = hdr32(m, 0x0C), bssSec = rd8(m + 0x33);
    for (u32 s = 1; s < nsec; s++) {
        const u32 off = rd32(table + 8 * s);
        if (!off) continue;
        // bss (no file data): back to 0; file sections: back to their file offset.
        if (s == bssSec || (off & ~1u) < m) wr32(table + 8 * s, 0);
        else wr32(table + 8 * s, off - m);
    }
    wr32(m + 0x10, table - m);
    wr32(m + 0x24, hdr32(m, 0x24) - m);
    wr32(m + 0x28, hdr32(m, 0x28) - m);
    if (hdr32(m, 0x1C) >= 3) wr32(m + 0x48, hdr32(m, 0x48) - m);
    return 1;
}

const Linked* containing(u32 addr, const CodeRange** range) {
    for (const Linked& l : sLinked)
        for (const CodeRange& r : l.code)
            if (addr >= r.lo && addr < r.hi) {
                *range = &r;
                return &l;
            }
    return nullptr;
}

}  // namespace

GuestFn lookupRelFunc(u32 addr) {
    const CodeRange* r = nullptr;
    const Linked* l = containing(addr, &r);
    if (!l || !l->desc) return nullptr;
    const u32 key = r->sec << 24 | (addr - r->lo);
    const RelFuncEntry* end = l->desc->funcs + l->desc->funcCount;
    const RelFuncEntry* it = std::lower_bound(l->desc->funcs, end, key, [](const RelFuncEntry& e, u32 k) { return e.key < k; });
    return it != end && it->key == key ? it->fn : nullptr;
}

std::string relSymbolize(u32 addr) {
    std::lock_guard lk(sLinkedM);
    const CodeRange* r = nullptr;
    const Linked* l = containing(addr, &r);
    if (!l) return {};
    const u32 key = r->sec << 24 | (addr - r->lo);
    char buf[200];
    if (!l->desc) {
        std::snprintf(buf, sizeof(buf), "[module %u sec %u +0x%X]", l->id, r->sec, addr - r->lo);
        return buf;
    }
    const RelFuncEntry* end = l->desc->funcs + l->desc->funcCount;
    const RelFuncEntry* it = std::upper_bound(l->desc->funcs, end, key, [](u32 k, const RelFuncEntry& e) { return k < e.key; });
    if (it == l->desc->funcs) {
        std::snprintf(buf, sizeof(buf), "[%s sec %u +0x%X]", l->desc->name, r->sec, addr - r->lo);
        return buf;
    }
    --it;
    std::snprintf(buf, sizeof(buf), "[%s:%s+0x%X]", l->desc->name, l->desc->names[it - l->desc->funcs], key - it->key);
    return buf;
}

void rel_unresolved(Ctx& c, u32 module) {
    u32 unresolved = 0;
    for (const Linked& l : sLinked)
        if (l.id == module && rd8(l.header + 0x32)) unresolved = rd32(l.header + 0x3C);
    log("[rel] %s called a function of a module that is not linked (lr %08X)", moduleName(module).c_str(), c.lr);
    if (!unresolved) fatal("unresolved call from %s and no _unresolved function", moduleName(module).c_str());
    c.call(unresolved);
}

HLE(OSLink) { c.r[3] = link(c.r[3], c.r[4], false); }
HLE(OSLinkFixed) { c.r[3] = link(c.r[3], c.r[4], true); }
HLE(OSUnlink) { c.r[3] = unlink(c.r[3]); }
// Debugger notifications: nothing listens.
HLE(OSNotifyLink) {}
HLE(OSNotifyUnlink) {}
HLE(OSSetStringTable) { wr32(kStringTable, c.r[3]); }

// Size of a section of a linked module (0 when not linked), from its header's section table.
u32 relSectionSize(u32 module, u32 section) {
    if (section >= kRelMaxSections) return 0;
    std::lock_guard lk(sLinkedM);
    for (const Linked& l : sLinked)
        if (l.id == module && section < hdr32(l.header, 0x0C)) return rd32(secInfoAddr(l.header, section) + 4);
    return 0;
}

}  // namespace recomp
