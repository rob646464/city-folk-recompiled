#!/usr/bin/env python3
"""Names from the strings a function prints (`DWC_LoginAsync() was called`, `NANDOpen(): ...`).

    python tools/discovery/strnames.py --disc "US DATA/DATA" --id RUUE01 [--apply]

SDK and middleware functions report their own name in OSReport/OSPanic messages. For
each DOL function this collects the C strings it addresses (lis/addi pairs, r13/r2
small-data offsets) and takes a name the strings give in self-reporting form. The rule
is measured on functions the reference maps already named (precision printed); with
--apply, names for still-unnamed functions are added to names.txt with confidence `str`.
"""
import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import symfile  # noqa: E402
from ppc import Dol, sext  # noqa: E402

# Only the "X() was called" trace form is reliable: OSPanic/assert texts name the API
# entry (DVDReadAsync for DVDReadAsyncPrio, __OSHotReset for its callers), measured
# at 45% precision on already named functions, so they are not used.
SELF = [
    re.compile(r"^[\s!*+:>]*([A-Za-z_][A-Za-z0-9_]{3,})\([^)]*\)\s*(?:was called|is called)"),
]
GENERIC = re.compile(r"^(fn_|lbl_|entry_)")


def sda_bases(dol, init_regs):
    r = {}
    hi = {}
    for pc in range(init_regs, init_regs + 0x100, 4):
        w = dol.word(pc)
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:
            hi[rd] = (w & 0xFFFF) << 16
        elif op == 24 and rd in hi:  # ori rA, rS
            r[ra] = hi[rd] | (w & 0xFFFF)
        elif op == 14 and ra in hi:
            r[rd] = (hi[ra] + sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
        if w == 0x4E800020:
            break
    return r.get(13), r.get(2)


def cstring(dol, a):
    out = bytearray()
    try:
        for i in range(200):
            w = dol.word((a + i) & ~3)
            b = (w >> (8 * (3 - ((a + i) & 3)))) & 0xFF
            if b == 0:
                break
            if b < 0x20 and b not in (9, 10) or b >= 0x7F:
                return None
            out.append(b)
    except KeyError:
        return None
    return out.decode("ascii") if len(out) >= 4 else None


def func_strings(dol, start, size, r13, r2):
    his = {}
    out = []
    for pc in range(start, start + size, 4):
        w = dol.word(pc)
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        a = None
        if op == 15 and ra == 0:
            his[rd] = (w & 0xFFFF) << 16
            continue
        if op == 14:
            if ra in his:
                a = (his[ra] + sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
            elif ra == 13 and r13:
                a = (r13 + sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
            elif ra == 2 and r2:
                a = (r2 + sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
        if a is not None and dol.has(a) and not dol.is_text(a):
            s = cstring(dol, a)
            if s:
                out.append(s)
    return out


def candidates(strings):
    c = []
    for s in strings:
        for line in s.splitlines() or [s]:
            for rx in SELF:
                m = rx.match(line)
                if m:
                    c.append(m.group(1))
                    break
    return c


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--id", default="RUUE01")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--report")
    a = ap.parse_args()
    cfg = ROOT / "config" / a.id
    dol = Dol(Path(a.disc) / "sys" / "main.dol")
    syms = symfile.parse(cfg / "symbols.txt")
    funcs = sorted((s.addr, s.size or 0, s.name) for s in syms if s.is_func)
    init = next(s.addr for s in syms if s.name == "__init_registers")
    r13, r2 = sda_bases(dol, init)
    print(f"_SDA_BASE_ {r13:08X} _SDA2_BASE_ {r2:08X}")
    names_path = cfg / "names.txt"
    lines = names_path.read_text(encoding="utf-8").splitlines()
    named = {}
    for ln in lines:
        p = ln.split()
        if len(p) >= 4 and not ln.startswith("#") and p[3] != "str":
            named[int(p[1], 16)] = p[2]
    used = Counter(named.values())
    agree = disagree = 0
    report = []
    new = {}
    for start, size, name in funcs:
        cands = Counter(candidates(func_strings(dol, start, size, r13, r2)))
        if not cands:
            continue
        top, n = cands.most_common(1)[0]
        if len(cands) > 1 and cands.most_common(2)[1][1] == n:
            continue
        if start in named:
            if named[start] == top:
                agree += 1
            else:
                disagree += 1
                report.append(f"{start:08X} named {named[start]} strings say {top} {dict(cands)}")
        elif top not in used:
            new[start] = top
            used[top] += 1
    print(f"self-report rule on named functions: {agree} agree, {disagree} disagree"
          f" ({100.0 * agree / max(1, agree + disagree):.1f}% precision); {len(new)} new names")
    for addr, nm in sorted(new.items()):
        report.append(f"{addr:08X} new {nm}")
    if a.report:
        Path(a.report).write_text("\n".join(report) + "\n", encoding="utf-8")
    if a.apply:
        keep = [ln for ln in lines if not (len(ln.split()) >= 4 and ln.split()[3] == "str")]
        keep += [f"dol {addr:08X} {nm} str strings -" for addr, nm in sorted(new.items())]
        names_path.write_text("\n".join(keep) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
