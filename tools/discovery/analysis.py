"""Discovery passes on top of decomp-toolkit's analysis (docs/DISCOVERY.md).

decomp-toolkit finds the functions of each module; these passes make the table complete
for the recompiler:
  * jump tables: every `lwzx; mtctr; bctr` switch gets a `jumptable_` object (dtk names
    some of them `@<n>`/`lbl_` and misses a few);
  * entry points: every call/branch target and every code address the program
    materialises (data words, relocations, lis/addi pairs) is a function start: a
    function is split there when that is clean, otherwise the target becomes a label
    (the recompiler turns a label inside a function into an alternate entry point);
  * code in gaps between functions becomes functions.
"""
import bisect
from collections import defaultdict

from ppc import (sext, R_PPC_ADDR32, R_PPC_REL24, R_PPC_ADDR16_HA, R_PPC_ADDR16_LO,
                 R_PPC_ADDR16_HI, branch_target, bc_target)

BCTR = 0x4E800420
BLR = 0x4E800020
RFI = 0x4C000064


def op(w):
    return w >> 26


def rd(w):
    return (w >> 21) & 31


def ra(w):
    return (w >> 16) & 31


def rb(w):
    return (w >> 11) & 31


def is_mtctr(w):
    return (w & 0xFC1FFFFF) == 0x7C0903A6


def is_lwzx(w):
    return op(w) == 31 and ((w >> 1) & 0x3FF) == 23


def ends_flow(w):
    """Unconditional end of straight-line flow (no fall-through)."""
    if w in (BLR, BCTR, RFI):
        return True
    if op(w) == 18 and not (w & 1):  # b / ba
        return True
    if op(w) == 16 and (w & 0x02800000) == 0x02800000 and not (w & 1):  # bc always
        return True
    return False


class Funcs:
    """Function table of one module: start -> [size, name]; plus labels."""

    def __init__(self, module):
        self.m = module
        self.f = {}
        self.labels = {}  # addr -> name (interior entry points)
        self.origin = {}  # addr -> how it was found (for new entries)
        for s in module.syms:
            if s.is_func and module.is_code_sym(s):
                self.f[s.addr] = [s.size or 0, s.name]
            elif s.type == "label" and module.is_code_sym(s):
                self.labels[s.addr] = s.name
        self._index()

    def _index(self):
        self.starts = sorted(self.f)

    def containing(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        if i < 0:
            return None
        s = self.starts[i]
        return s if a < s + self.f[s][0] else None

    def next_start(self, a):
        i = bisect.bisect_right(self.starts, a)
        return self.starts[i] if i < len(self.starts) else None

    def fname(self, a):
        if self.m.kind == "dol":
            return f"fn_{a:08X}"
        return f"fn_{self.m.id}_{a:X}"


def find_jump_tables(m, funcs):
    """Returns {bctr pc: (table location, [case targets])}. Location is an absolute
    address (DOL) or (section index, offset) (REL)."""
    out = {}
    rel = m.kind == "rel"
    tsec = m.text.index if rel else None
    for start in funcs.starts:
        size = funcs.f[start][0]
        end = start + size
        val = {}
        pending = {}
        ctr = None
        bound = None
        for pc in range(start, end, 4):
            w = m.word(pc)
            o = op(w)
            r = m.reloc_at.get((tsec, pc + 2)) if rel else None
            if r and r[0] == m.id and r[1] in (R_PPC_ADDR16_HA, R_PPC_ADDR16_LO) and o in (14, 15):
                val[rd(w)] = ("s", r[2], r[3])
                continue
            if o == 15:  # lis / addis
                if ra(w) == 0:
                    val[rd(w)] = (w & 0xFFFF) << 16
                elif isinstance(val.get(ra(w)), int):
                    val[rd(w)] = (val[ra(w)] + ((w & 0xFFFF) << 16)) & 0xFFFFFFFF
                else:
                    val.pop(rd(w), None)
            elif o == 14:  # addi / li
                if ra(w) != 0 and isinstance(val.get(ra(w)), int):
                    val[rd(w)] = (val[ra(w)] + sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
                elif ra(w) != 0 and isinstance(val.get(ra(w)), tuple) and not rel:
                    val.pop(rd(w), None)
                elif ra(w) != 0 and isinstance(val.get(ra(w)), tuple):
                    pass  # REL: addi without relocation on a symbolic base keeps it
                else:
                    val.pop(rd(w), None)
            elif o == 24:  # ori
                if isinstance(val.get(rd(w)), int):
                    val[ra(w)] = val[rd(w)] | (w & 0xFFFF)
            elif o == 10 and not (w & 0x00200000):  # cmplwi
                bound = w & 0xFFFF
            elif is_lwzx(w):
                for base in (val.get(ra(w)), val.get(rb(w))):
                    if base is not None:
                        pending[rd(w)] = base
                        break
                else:
                    pending.pop(rd(w), None)
            elif is_mtctr(w):
                ctr = pending.get(rd(w))
            elif w == BCTR and ctr is not None:
                cases = []
                n = bound + 1 if bound is not None and bound < 4096 else 4096
                for i in range(n):
                    if rel:
                        if not isinstance(ctr, tuple):
                            break
                        e = m.reloc_at.get((ctr[1], ctr[2] + 4 * i))
                        if not e or e[0] != m.id or e[1] != R_PPC_ADDR32 or e[2] != tsec:
                            break
                        t = e[3]
                    else:
                        if not isinstance(ctr, int) or not m.has(ctr + 4 * i) or m.is_code(ctr + 4 * i):
                            break
                        t = m.word(ctr + 4 * i)
                    if t % 4 or not m.is_code(t):
                        break
                    # without a bound only cases inside the function count; with one,
                    # cases past the end mean dtk split the switch's tail off
                    if not (start <= t < end) and (bound is None or not (start <= t < end + 0x4000)):
                        break
                    cases.append(t)
                if cases:
                    loc = (ctr[1], ctr[2]) if rel else ctr
                    out[pc] = (loc, cases)
                ctr = None
    return out


def merge_switch_tails(m, fs, tabs, report):
    """Cases past the end of their function: absorb the following functions (dtk
    split the code after a bctr off as functions of their own)."""
    n = 0
    for pc, (loc, cases) in sorted(tabs.items()):
        c = fs.containing(pc)
        if c is None:
            continue
        top = max(cases)
        end = c + fs.f[c][0]
        if top < end:
            continue
        while end <= top:
            nxt = end if end in fs.f else None
            if nxt is None:
                break
            report.append(f"{m.name}: switch at {pc:08X} absorbs {fs.f[nxt][1]} into {fs.f[c][1]}")
            fs.f[c][0] += fs.f[nxt][0]
            end = c + fs.f[c][0]
            del fs.f[nxt]
            n += 1
        fs._index()
    return n


def collect_targets(dol, rels, F, jts):
    """Entry targets per module: {mid: {addr: kind}} with kind 'call' (bl/REL24/b out of a
    function) or 'ptr' (address taken)."""
    tg = defaultdict(dict)

    def add(mid, a, kind):
        if tg[mid].get(a) != "call":
            tg[mid][a] = kind

    jt_words = defaultdict(set)  # mid -> table word locations
    for mid, tabs in jts.items():
        for pc, (loc, cases) in tabs.items():
            for i in range(len(cases)):
                if mid == 0:
                    jt_words[0].add(loc + 4 * i)
                else:
                    jt_words[mid].add((loc[0], loc[1] + 4 * i))
    # dtk's own jump tables
    for s in dol.syms:
        if s.name.startswith("jumptable_") and s.size:
            for a in range(s.addr, s.addr + s.size, 4):
                jt_words[0].add(a)
    for mid, m in rels.items():
        for s in m.syms:
            if s.name.startswith("jumptable_") and s.size and s.section in m.secindex:
                si = m.secindex[s.section]
                for a in range(s.addr, s.addr + s.size, 4):
                    jt_words[mid].add((si, a))

    # branches inside code
    for mid, m in [(0, dol)] + sorted(rels.items()):
        fs = F[mid]
        skip = set()
        if m.kind == "rel":
            ti = m.text.index
            skip = {off for (sec, off), r in m.reloc_at.items() if sec == ti and r[1] == R_PPC_REL24}
        for start in fs.starts:
            size = fs.f[start][0]
            for pc in range(start, start + size, 4):
                if pc in skip:
                    continue
                w = m.word(pc)
                t = branch_target(w, pc)
                if t is not None:
                    if (w & 1) or not (start <= t < start + size):
                        if m.is_code(t):
                            add(mid, t, "call")
                    continue
                t = bc_target(w, pc)
                if t is not None and not (start <= t < start + size) and m.is_code(t):
                    add(mid, t, "call")
        # lis/addi pairs (DOL only: REL code addresses are relocations)
        if m.kind == "dol":
            for start in fs.starts:
                his = {}
                for pc in range(start, start + fs.f[start][0], 4):
                    w = m.word(pc)
                    if op(w) == 15 and ra(w) == 0:
                        his[rd(w)] = (w & 0xFFFF) << 16
                    elif op(w) in (14, 24) and ra(w) in his and op(w) == 14:
                        t = (his[ra(w)] + sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
                        if t % 4 == 0 and m.is_code(t):
                            add(0, t, "ptr")
                    elif op(w) == 24 and rd(w) in his:
                        t = his[rd(w)] | (w & 0xFFFF)
                        if t % 4 == 0 and m.is_code(t):
                            add(0, t, "ptr")
    # DOL data words
    for lo, hi, off in dol.dol.data_ranges():
        data = dol.dol.data
        for a in range(lo, hi - 3, 4):
            if a in jt_words[0]:
                continue
            v = int.from_bytes(data[off + a - lo: off + a - lo + 4], "big")
            if v % 4 == 0 and dol.is_code(v):
                add(0, v, "ptr")
    # REL relocations
    for mid, m in rels.items():
        ti = m.text.index
        for mod, lst in m.rel.relocs.items():
            for sec, off, typ, tsec, a in lst:
                if mod == 0:
                    if not dol.is_code(a):
                        continue
                    add(0, a, "call" if typ == R_PPC_REL24 else "ptr")
                    continue
                if mod not in rels:
                    continue
                tm = rels[mod]
                if tsec != tm.text.index:
                    continue
                if typ == R_PPC_REL24:
                    add(mod, a, "call")
                elif typ in (R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HA, R_PPC_ADDR16_HI):
                    if mod == mid and (sec, off) in jt_words[mid]:
                        continue
                    add(mod, a, "ptr")
    return tg


def apply_targets(m, fs, targets, report, exclude=lambda a: False):
    """Make every target an entry point. Returns counters."""
    st = defaultdict(int)
    for t in sorted(targets):
        if t in fs.f:
            continue
        c = fs.containing(t)
        if c is None and exclude(t):
            st["excluded"] += 1
            continue
        if c is None:
            # in a gap: a new function up to the next function start
            nxt = fs.next_start(t)
            hi = next(h for lo, h in m.code if lo <= t < h)
            end = min(nxt, hi) if nxt is not None else hi
            # stop after the last flow-ending instruction before end
            e = t
            last_end = None
            while e < end:
                if ends_flow(m.word(e)):
                    last_end = e + 4
                e += 4
            end = last_end or end
            fs.f[t] = [end - t, fs.fname(t)]
            fs.origin[t] = "gap-" + targets[t]
            fs._index()
            st["new_in_gap"] += 1
            report.append(f"{m.name}: new function {t:08X} size {end - t:#x} ({targets[t]} target in a gap)")
            continue
        size, name = fs.f[c]
        end = c + size
        prev = m.word(t - 4)
        clean = ends_flow(prev)
        if clean:
            # no branch in the first part may land in the second part (other than at t)
            # and no branch in the second part may land in the first part
            for pc in range(c, end, 4):
                w = m.word(pc)
                bt = branch_target(w, pc)
                if bt is None:
                    bt = bc_target(w, pc)
                if bt is None or (w & 1):
                    continue
                if pc < t and t < bt < end:
                    clean = False
                    break
                if pc >= t and c <= bt < t:
                    clean = False
                    break
        if clean:
            fs.f[c][0] = t - c
            fs.f[t] = [end - t, fs.fname(t)]
            fs.origin[t] = "split-" + targets[t]
            fs._index()
            st["split"] += 1
        elif targets[t] == "ptr" and not ends_flow(prev):
            # an address-shaped constant in the middle of straight-line code (round
            # values like 0x80300000, section bounds): not an entry point. The
            # recompiler still adds entries for materialised addresses itself.
            st["ptr_ignored"] += 1
        else:
            if t not in fs.labels:
                fs.labels[t] = fs.fname(t).replace("fn_", "entry_", 1)
                fs.origin[t] = "label-" + targets[t]
                st["label"] += 1
                report.append(f"{m.name}: interior entry {t:08X} in {name} ({targets[t]})")
    return st


def code_gaps(m, fs):
    """Non-zero words in code ranges outside every function: [(lo, hi)]."""
    gaps = []
    for lo, hi in m.code:
        a = lo
        while a < hi:
            c = fs.containing(a)
            if c is not None:
                a = c + fs.f[c][0]
                continue
            if m.word(a) == 0:
                a += 4
                continue
            b = a
            while b < hi and fs.containing(b) is None and m.word(b) != 0:
                b += 4
            gaps.append((a, b))
            a = b
    return gaps
