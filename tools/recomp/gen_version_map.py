#!/usr/bin/env python3
"""USA -> PAL / Japan / Korea addresses for host code (docs/SETUP.md "Version-aware code").

    py tools/recomp/gen_version_map.py              scan src/ for VA(0x...), write src/recomp/version_addrs.inc
    py tools/recomp/gen_version_map.py show ADDR    how one USA address maps (sites, votes)
    py tools/recomp/gen_version_map.py diff FUNC...  instructions of USA functions that differ in another
                                                    version other than in addresses (moved fields)
    py tools/recomp/gen_version_map.py reldiff MODULE OFF...
                                                    the same for REL functions (USA .text offsets)
    py tools/recomp/gen_version_map.py field FUNC OFF [OFF...]
                                                    the instructions of USA function FUNC (name or
                                                    address) that use displacement OFF, and the
                                                    same instructions in every other version

Host code is written against the USA game (RUUE01 rev 1). Every USA guest address it uses is
wrapped in VA(0x...) (src/recomp/version.h); this script maps each one into the other three
versions and writes the table version.h reads:

- code addresses (a function start or an address inside one, e.g. a return address): the
  matched function (config/<id>/us_map.txt, made by tools/discovery/version_map.py) plus the same
  offset. The function must be a `hash` match, or the instruction at that offset must be the same
  (addresses masked) in both versions;
- data addresses: every place the USA code builds the address (lis + addi/ori/load/store, or
  r13/r2 small-data offsets) in a function that is matched in the other version is decoded at the
  same instruction there; the majority wins (the script prints the vote). A data address no code
  builds directly is found through the nearest address below it that code does build (a struct
  base; same offset assumed, flagged "base+").

Struct field offsets are not mapped: hash matches mask the immediates of loads and stores, so a
field can move while the function still matches. Check each one with `field` and write
VOFF(us, pal, jp, kr) where they differ.

Reads only the user's own discs: `<repo>/<X> DATA/DATA/sys/main.dol` (a worktree falls back to
the main checkout; --data-root overrides). Output is addresses only (committed).
"""
import argparse
import re
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools" / "discovery"))

import symfile  # noqa: E402
from ppc import Dol, sext  # noqa: E402

VERSIONS = [("RUUE01", "US DATA"), ("RUUP01", "PAL DATA"), ("RUUJ01", "JAPAN DATA"), ("RUUK01", "KOREA DATA")]
OUT = ROOT / "src" / "recomp" / "version_addrs.inc"
VA_RE = re.compile(r"\bVA\(\s*0x([0-9A-Fa-f]{8})u?\s*\)")


def data_root(arg):
    if arg:
        return Path(arg)
    for r in [ROOT] + list(ROOT.parents):
        if (r / "US DATA" / "DATA" / "sys" / "main.dol").exists():
            return r
    sys.exit("no 'US DATA/DATA/sys/main.dol' found above the repo; pass --data-root")


def mask(w):
    op = w >> 26
    if op == 18:  # b/bl
        return w & 0xFC000003
    if op == 16:  # bc
        return w & 0xFFFF0003
    if op in (14, 15, 24, 25) or 32 <= op <= 55:
        return w & 0xFFFF0000
    return w


class Version:
    def __init__(self, gid, folder, root):
        self.id = gid
        self.dol = Dol(root / folder / "DATA" / "sys" / "main.dol")
        cfg = ROOT / "config" / gid
        syms = symfile.parse(cfg / "symbols.txt")
        self.funcs = sorted((s.addr, s.size or 0, s.name) for s in syms if s.is_func and self.dol.is_text(s.addr))
        self.starts = [f[0] for f in self.funcs]
        self.by_name = {f[2]: f for f in self.funcs}
        init = next(s.addr for s in syms if s.name == "__init_registers")
        self.r13, self.r2 = sda_bases(self.dol, init)
        # USA function start -> (this version's start, method)
        self.from_us = {}
        m = cfg / "us_map.txt"
        if m.exists():
            for ln in m.read_text(encoding="utf-8").splitlines():
                p = ln.split()
                if len(p) >= 3 and not ln.startswith("#"):
                    self.from_us[int(p[1], 16)] = (int(p[0], 16), p[2])

    def func_at(self, addr):
        import bisect
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            a, s, n = self.funcs[i]
            if a <= addr < a + max(s, 4):
                return self.funcs[i]
        return None

    def word(self, a):
        return self.dol.word(a)

    def refs(self, func):
        """(instruction address, effective address) of every address the function builds."""
        a0, size, _ = func
        out = []
        hi = {}
        for pc in range(a0, a0 + size, 4):
            w = self.word(pc)
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:  # lis
                hi[rd] = imm << 16
                continue
            base = None
            if ra in hi:
                base = hi[ra]
            elif ra == 13 and self.r13:
                base = self.r13
            elif ra == 2 and self.r2:
                base = self.r2
            if base is not None and (op == 14 or op == 12 or 32 <= op <= 55):  # addi(c), loads/stores
                out.append((pc, (base + sext(imm, 16)) & 0xFFFFFFFF))
            elif base is not None and op == 24 and rd in hi:  # ori rA, rS, lo
                out.append((pc, (hi[rd] | imm) & 0xFFFFFFFF))
            # the register this instruction writes loses its lis value (stores, compares and
            # branches write none; FP loads write an FPR)
            if op == 31:  # X-form: arithmetic writes rD, logical ops rA
                hi.pop(rd, None)
                hi.pop(ra, None)
                dst = None
            elif op in (24, 25, 26, 27, 28, 29, 21, 20, 23):
                dst = ra
            elif op in (7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 46):
                dst = rd
            else:
                dst = None
            if dst is not None:
                hi.pop(dst, None)
            if w == 0x4E800020 or (w >> 26 == 18 and (w & 1) == 0):  # blr / b: straight-line state ends
                hi = {}
        return out


def sda_bases(dol, init):
    r, hi = {}, {}
    for pc in range(init, init + 0x100, 4):
        w = dol.word(pc)
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:
            hi[rd] = (w & 0xFFFF) << 16
        elif op == 24 and rd in hi:
            r[ra] = hi[rd] | (w & 0xFFFF)
        elif op == 14 and ra in hi:
            r[rd] = (hi[ra] + sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
        if w == 0x4E800020:
            break
    return r.get(13), r.get(2)


class Mapper:
    def __init__(self, root):
        self.v = [Version(g, f, root) for g, f in VERSIONS]
        self.us = self.v[0]
        self._index = None

    def ref_index(self):
        """USA effective address -> [(function, instruction address)]."""
        if self._index is None:
            idx = {}
            for f in self.us.funcs:
                for pc, ea in self.us.refs(f):
                    idx.setdefault(ea, []).append((f, pc))
            self._index = idx
        return self._index

    def map_code(self, addr, ver):
        f = self.us.func_at(addr)
        if not f:
            return None, "not in a USA function"
        m = ver.from_us.get(f[0])
        if not m:
            return None, f"{f[2]} has no match in {ver.id}"
        start, method = m
        off = addr - f[0]
        vf = ver.func_at(start)
        if not vf or start + off >= vf[0] + vf[1] and off:
            return None, f"{f[2]}+0x{off:X} past the end of {ver.id}'s function"
        same = mask(self.us.word(addr)) == mask(ver.word(start + off))
        if method != "hash" and not same:
            return None, f"{f[2]} is a '{method}' match and the instruction at +0x{off:X} differs"
        return start + off, ("start of " if not off else "") + f"{f[2]}" + (f"+0x{off:X}" if off else "") + \
            ("" if method == "hash" else f" ({method})")

    def map_data(self, addr, ver, verbose=False):
        idx = self.ref_index()
        sites = idx.get(addr, [])
        delta, note = 0, ""
        if not sites:
            below = [ea for ea in idx if addr - 0x8000 <= ea < addr]
            if below:
                base = max(below)
                sites, delta, note = idx[base], addr - base, f"base+ {base:08X}+0x{addr - base:X}"
        votes = Counter()
        for f, pc in sites:
            m = ver.from_us.get(f[0])
            if not m:
                continue
            start, method = m
            vpc = start + (pc - f[0])
            vf = ver.func_at(start)
            if not vf or vpc >= vf[0] + vf[1]:
                continue
            if mask(self.us.word(pc)) != mask(ver.word(vpc)):
                continue
            for p2, ea2 in ver.refs(vf):
                if p2 == vpc:
                    votes[(ea2 + delta) & 0xFFFFFFFF] += 1
                    break
        if verbose:
            print(f"  {ver.id}: {len(sites)} USA sites {note}; votes " +
                  ", ".join(f"{a:08X} x{n}" for a, n in votes.most_common(5)))
        if not votes:
            return None, f"no matched reference ({len(sites)} USA sites)"
        best, n = votes.most_common(1)[0]
        total = sum(votes.values())
        txt = f"data, {n}/{total} sites" + (f", {note}" if note else "")
        if n * 2 <= total:
            return None, txt + " (no majority)"
        return best, txt

    def map(self, addr, ver, verbose=False):
        if ver is self.us:
            return addr, "USA"
        if self.us.dol.is_text(addr):
            return self.map_code(addr, ver)
        if 0x80000000 <= addr < 0x80004000:
            return addr, "OS globals (fixed)"
        return self.map_data(addr, ver, verbose)


def scan_sources():
    found = {}
    for p in sorted((ROOT / "src").rglob("*")):
        if p.suffix not in (".cpp", ".h", ".inc") or p == OUT:
            continue
        for m in VA_RE.finditer(p.read_text(encoding="utf-8", errors="replace")):
            found.setdefault(int(m.group(1), 16), set()).add(p.relative_to(ROOT).as_posix())
    return found


def cmd_generate(mp):
    found = scan_sources()
    rows, bad = [], 0
    for a in sorted(found):
        vals, notes = [a], []
        for ver in mp.v[1:]:
            r, why = mp.map(a, ver)
            if r is None:
                bad += 1
                print(f"UNMAPPED {a:08X} in {ver.id}: {why}  ({', '.join(sorted(found[a]))})")
                r = 0
            vals.append(r)
            notes.append(why)
        uniq = sorted(set(n.split(" (")[0] if n.startswith("data") else n for n in notes))
        rows.append((vals, "; ".join(uniq)))
    lines = ["// Generated by tools/recomp/gen_version_map.py. Do not edit.",
             "// USA (RUUE01) address -> {USA, PAL RUUP01, Japan RUUJ01, Korea RUUK01} for every VA(...) in src/",
             "// (version.h). 0 = no mapping (that feature is off on that version)."]
    for vals, note in rows:
        lines.append("    {" + ", ".join(f"0x{x:08X}u" for x in vals) + "},  // " + note)
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(rows)} addresses, {bad} unmapped -> {OUT.relative_to(ROOT)}")
    return 1 if bad else 0


def cmd_show(mp, addr):
    a = int(addr, 16)
    print(f"{a:08X}: {'code' if mp.us.dol.is_text(a) else 'data'}")
    for ver in mp.v[1:]:
        r, why = mp.map(a, ver, verbose=True)
        print(f"  -> {ver.id} {'--------' if r is None else f'{r:08X}'}  {why}")


def cmd_field(mp, func, offs):
    f = mp.us.by_name.get(func) or mp.us.func_at(int(func, 16))
    if not f:
        sys.exit(f"no USA function {func}")
    want = {int(o, 16) for o in offs}
    print(f"{f[2]} @ {f[0]:08X} size 0x{f[1]:X}")
    for pc in range(f[0], f[0] + f[1], 4):
        w = mp.us.word(pc)
        op, ra, imm = w >> 26, (w >> 16) & 31, w & 0xFFFF
        if not ((op == 14 or 32 <= op <= 55) and imm in want and ra not in (0, 2, 13)):
            continue
        row = [f"+0x{pc - f[0]:04X} {w:08X}"]
        for ver in mp.v[1:]:
            m = ver.from_us.get(f[0])
            if not m:
                row.append(f"{ver.id}: no match")
                continue
            w2 = ver.word(m[0] + pc - f[0])
            row.append(f"{ver.id}: {w2:08X}" + ("" if w2 == w else " DIFFERS"))
        print("  " + "  ".join(row))


def cmd_diff(mp, funcs):
    for fn in funcs:
        f = mp.us.by_name.get(fn) or mp.us.func_at(int(fn, 16))
        if not f:
            print(f"no USA function {fn}")
            continue
        usref = {pc for pc, _ in mp.us.refs(f)}
        print(f"{f[2]} @ {f[0]:08X} size 0x{f[1]:X}")
        for ver in mp.v[1:]:
            m = ver.from_us.get(f[0])
            if not m:
                print(f"  {ver.id}: no match")
                continue
            vf = ver.func_at(m[0])
            vref = {pc - m[0] for pc, _ in ver.refs(vf)} if vf else set()
            diffs = []
            for o in range(0, f[1], 4):
                w1 = mp.us.word(f[0] + o)
                try:
                    w2 = ver.word(m[0] + o)
                except KeyError:
                    break
                if w1 == w2 or (f[0] + o in usref and o in vref) or (w1 >> 26 in (15, 18) and mask(w1) == mask(w2)):
                    continue
                diffs.append(f"+0x{o:X} {w1:08X}->{w2:08X}")
            print(f"  {ver.id} ({m[1]}, size {'same' if vf and vf[1] == f[1] else hex(vf[1]) if vf else '?'}): " +
                  ("identical apart from addresses" if not diffs else f"{len(diffs)} differ: " + " ".join(diffs[:12])))


def cmd_reldiff(mp, root, module, offs):
    sys.path.insert(0, str(ROOT / "tools" / "discovery"))
    from ppc import Rel
    rels = {}
    for gid, folder in VERSIONS:
        rels[gid] = Rel(root / folder / "DATA" / "files" / "rels" / f"{module}.rel")
    mid = rels["RUUE01"].id
    for off in offs:
        uo = int(off, 16)
        size = None
        for ln in (ROOT / "config" / "RUUE01" / "rels" / f"{module}.txt").read_text(encoding="utf-8").splitlines():
            m = re.match(r"^(\S+) = \.text:0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)", ln)
            if m and int(m.group(2), 16) == uo:
                size, name = int(m.group(3), 16), m.group(1)
        if size is None:
            print(f"{module} +{uo:X}: no USA function starts there")
            continue
        print(f"{module}::{name} size 0x{size:X}")
        for gid, _ in VERSIONS[1:]:
            vo = None
            for ln in (ROOT / "config" / gid / "rels_us_map.txt").read_text(encoding="utf-8").splitlines():
                p = ln.split()
                if len(p) >= 4 and p[0] == str(mid) and int(p[2], 16) == uo:
                    vo, meth = int(p[1], 16), p[3]
            if vo is None:
                print(f"  {gid}: no match")
                continue
            diffs = []
            for o in range(0, size, 4):
                w1, w2 = rels["RUUE01"].sec_word(1, uo + o), rels[gid].sec_word(1, vo + o)
                if w1 != w2 and not (w1 >> 26 == 18 and mask(w1) == mask(w2)):
                    diffs.append(f"+0x{o:X} {w1:08X}->{w2:08X}")
            print(f"  {gid} at +{vo:X} ({meth}): " + ("identical" if not diffs else f"{len(diffs)} differ: " + " ".join(diffs[:12])))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", nargs="?", default="generate", choices=["generate", "show", "field", "diff", "reldiff"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--data-root")
    a = ap.parse_args()
    root = data_root(a.data_root)
    mp = Mapper(root)
    if a.cmd == "diff":
        cmd_diff(mp, a.args)
        return 0
    if a.cmd == "reldiff":
        cmd_reldiff(mp, root, a.args[0], a.args[1:])
        return 0
    if a.cmd == "show":
        for x in a.args:
            cmd_show(mp, x)
        return 0
    if a.cmd == "field":
        cmd_field(mp, a.args[0], a.args[1:])
        return 0
    return cmd_generate(mp)


if __name__ == "__main__":
    sys.exit(main())
