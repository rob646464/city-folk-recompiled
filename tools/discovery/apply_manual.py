#!/usr/bin/env python3
"""Apply config/RUUE01/names_manual.txt to every version's tables without re-running the pipeline.

    python tools/discovery/apply_manual.py [--ids RUUE01,RUUP01,RUUJ01,RUUK01] [--check]

The full pipeline (run_all.py: decomp-toolkit + reference maps) applies names_manual.txt in
sdk_names.py and carries the names to PAL/Japan/Korea by function matching. This does the same
for the manual names alone: USA addresses as written, other versions through their
config/<id>/us_map.txt. In symbols.txt only a generic `fn_<addr>` name is replaced (a function
that already has another name is reported and left alone); names.txt gets the matching lines
(confidence `manual` for USA, `map` elsewhere), as the pipeline would write them.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYM = re.compile(r"^(\S+) = (\.text):0x([0-9A-Fa-f]{8});(.*)$")


def load_manual():
    out = []
    for ln in (ROOT / "config" / "RUUE01" / "names_manual.txt").read_text(encoding="utf-8").splitlines():
        p = ln.split()
        if len(p) >= 3 and p[0] == "dol" and not ln.startswith("#"):
            out.append((int(p[1], 16), p[2], " ".join(p[3:]) or "manual"))
    return out


def us_map(vid):
    m = {}
    path = ROOT / "config" / vid / "us_map.txt"
    for ln in path.read_text(encoding="utf-8").splitlines():
        p = ln.split()
        if len(p) >= 3 and not ln.startswith("#"):
            m[int(p[1], 16)] = (int(p[0], 16), p[2])
    return m


def apply(vid, manual, check):
    cfg = ROOT / "config" / vid
    mapping = None if vid == "RUUE01" else us_map(vid)
    targets = {}
    for us, name, note in manual:
        if mapping is None:
            targets[us] = (name, "manual", note)
        elif us in mapping:
            addr, method = mapping[us]
            targets[addr] = (name, "map", f"RUUE01:manual:{method}")
        else:
            print(f"{vid}: {name} (USA {us:08X}) has no matched function", file=sys.stderr)
    sym_path = cfg / "symbols.txt"
    lines = sym_path.read_text(encoding="utf-8").splitlines()
    names_in_use = {}
    for ln in lines:
        m = SYM.match(ln)
        if m:
            names_in_use[m.group(1)] = int(m.group(3), 16)
    changed = 0
    for i, ln in enumerate(lines):
        m = SYM.match(ln)
        if not m:
            continue
        addr = int(m.group(3), 16)
        if addr not in targets:
            continue
        cur, (want, _, _) = m.group(1), targets[addr]
        if cur == want:
            continue
        if not cur.startswith("fn_"):
            print(f"{vid}: {addr:08X} is already named {cur}, not renamed to {want}", file=sys.stderr)
            continue
        if want in names_in_use and names_in_use[want] != addr:
            print(f"{vid}: {want} already used at {names_in_use[want]:08X}", file=sys.stderr)
            continue
        lines[i] = f"{want} = {m.group(2)}:0x{m.group(3)};{m.group(4)}"
        changed += 1
    found = {int(SYM.match(ln).group(3), 16) for ln in lines if SYM.match(ln)}
    for addr, (want, _, _) in targets.items():
        if addr not in found:
            print(f"{vid}: no function at {addr:08X} for {want}", file=sys.stderr)
    names_path = cfg / "names.txt"
    nl = names_path.read_text(encoding="utf-8").splitlines()
    keep = [ln for ln in nl if not (len(ln.split()) >= 2 and ln.split()[0] == "dol" and
                                    int(ln.split()[1], 16) in targets and ln.split()[2].startswith("fn_"))]
    have = {(int(ln.split()[1], 16)) for ln in keep if len(ln.split()) >= 3 and ln.split()[0] == "dol"}
    for addr, (want, conf, src) in sorted(targets.items()):
        if addr not in have:
            keep.append(f"dol {addr:08X} {want} {conf} {src.replace(' ', '_')} -")
    print(f"{vid}: {changed} functions renamed, {len(targets)} manual names")
    if not check:
        sym_path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        names_path.write_text("\n".join(keep) + "\n", encoding="utf-8", newline="\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ids", default="RUUE01,RUUP01,RUUJ01,RUUK01")
    ap.add_argument("--check", action="store_true", help="report only, write nothing")
    a = ap.parse_args()
    manual = load_manual()
    for vid in a.ids.split(","):
        apply(vid, manual, a.check)


if __name__ == "__main__":
    main()
