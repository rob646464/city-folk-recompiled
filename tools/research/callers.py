"""List every caller of a main.dol function in main.dol and in all RELs (dev aid, docs/REGION_CONTENT.md).

    py tools/research/callers.py <DATA folder> <config/<id> folder> <hex addr> [more addrs]

DOL: direct b/bl. RELs: REL24 relocations against module 0 at that address, plus ADDR32 /
ADDR16 HA/LO references (address taken). Prints the enclosing function per hit (symbols.txt /
rels/<module>.txt). Reads only the user's own disc.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "discovery"))
from ppc import Dol, Rel, branch_target, R_PPC_REL24, R_PPC_ADDR32, R_PPC_ADDR16_HA, R_PPC_ADDR16_LO  # noqa: E402

SYM = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")


def load_syms(path):
    out = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        m = SYM.match(line)
        if m:
            out.append((int(m.group(3), 16), int(m.group(4), 16), m.group(1), m.group(2)))
    out.sort()
    return out


def enclosing(syms, addr, sec=None):
    best = None
    for a, s, n, sn in syms:
        if sec and sn != sec:
            continue
        if a <= addr < a + s:
            return f"{n}+0x{addr - a:X}"
        if a <= addr:
            best = n
    return f"?({best})"


def main():
    data, cfg = Path(sys.argv[1]), Path(sys.argv[2])
    targets = [int(x, 16) for x in sys.argv[3:]]
    dol = Dol(data / "sys" / "main.dol")
    dsyms = load_syms(cfg / "symbols.txt")
    for t in targets:
        print(f"== callers of {t:08X} ({enclosing(dsyms, t)})")
        for _, a, o, s, is_text in dol.sections:
            if not is_text:
                continue
            for off in range(0, s, 4):
                w = struct.unpack_from(">I", dol.data, o + off)[0]
                pc = a + off
                if branch_target(w, pc) == t:
                    kind = "bl" if w & 1 else "b"
                    print(f"  main.dol {pc:08X} {kind:2} in {enclosing(dsyms, pc)}")
        # address taken in DOL: lis/addi pairs or data words
        hi, lo = (t + 0x8000) >> 16, t & 0xFFFF
        for _, a, o, s, is_text in dol.sections:
            for off in range(0, s, 4):
                w = struct.unpack_from(">I", dol.data, o + off)[0]
                if w == t:
                    print(f"  main.dol {a + off:08X} data word ({'text' if is_text else 'data'})")
        for rp in sorted((data / "files" / "rels").glob("*.rel")):
            r = Rel(rp)
            rsyms_path = cfg / "rels" / (rp.stem + ".txt")
            rsyms = load_syms(rsyms_path) if rsyms_path.exists() else []
            for sec, off, typ, ts, add in r.relocs.get(0, []):
                if add != t:
                    continue
                kind = {R_PPC_REL24: "bl", R_PPC_ADDR32: "addr32", R_PPC_ADDR16_HA: "ha", R_PPC_ADDR16_LO: "lo"}.get(typ, str(typ))
                where = enclosing(rsyms, off, ".text") if sec == 1 else f"section {sec}"
                print(f"  {rp.stem} .text+0x{off:X} {kind} in {where}")


if __name__ == "__main__":
    main()
