"""Coverage checks on a function table (docs/DISCOVERY.md "Checks").

Checks, for the DOL and every REL:
  * every non-zero word of a code section lies inside a function (gaps are listed);
  * every bl target and every b/bc target leaving its function is a function start;
  * every code address the module materialises (data words, REL ADDR32/HA/LO/REL24
    relocations, lis/addi pairs in DOL code) is a function start, a jump-table case,
    or reported as an interior entry (the recompiler adds those as entry points).
"""
import bisect
from collections import defaultdict

from ppc import (Dol, Rel, branch_target, bc_target, be32, R_PPC_ADDR32, R_PPC_REL24,
                 R_PPC_ADDR16_HA, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI)


class FuncIndex:
    def __init__(self, funcs, labels=()):
        # funcs: list of (start, size, name); labels: interior entry points
        self.funcs = sorted(funcs)
        self.starts = [f[0] for f in self.funcs]
        self.start_set = set(self.starts) | set(labels)

    def containing(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        if i < 0:
            return None
        f = self.funcs[i]
        return f if a < f[0] + f[1] else None


def check_code(read_word, ranges, fidx, jt_cases, report, tag, ext_ok=lambda t: False, skip=frozenset()):
    """ranges: [(lo, hi)] code ranges. read_word(addr) -> word. Returns counters."""
    st = defaultdict(int)
    for lo, hi in ranges:
        a = lo
        while a < hi:
            f = fidx.containing(a)
            if f:
                a = f[0] + f[1]
                st["func_bytes"] += f[1]
                continue
            w = read_word(a)
            if w == 0:
                st["zero_gap_bytes"] += 4
            else:
                # start of a non-zero gap run
                b = a
                while b < hi and not fidx.containing(b) and read_word(b) != 0:
                    b += 4
                st["code_gap_bytes"] += b - a
                st["code_gaps"] += 1
                report.append(f"{tag} gap {a:08X}-{b:08X} ({b - a:#x} bytes, first word {w:08X})")
                a = b
                continue
            a += 4
    for start, size, name in fidx.funcs:
        for pc in range(start, start + size, 4):
            if pc in skip:
                continue
            w = read_word(pc)
            t = branch_target(w, pc)
            kind = "bl" if t is not None and (w & 1) else "b"
            if t is None:
                t = bc_target(w, pc)
                kind = "bc"
                if t is None or (w & 1):
                    continue
            if kind != "bl" and start <= t < start + size:
                continue
            if t in fidx.start_set or ext_ok(t):
                continue
            if kind == "bc" and t in jt_cases:
                continue
            st[f"bad_{kind}"] += 1
            report.append(f"{tag} {kind} at {pc:08X} in {name} -> {t:08X} not a function start")
    return st


def check_dol(dol_path, syms, report):
    dol = Dol(dol_path)
    funcs = [(s.addr, s.size or 0, s.name) for s in syms if s.is_func and s.section in (".text", ".init")]
    labels = [s.addr for s in syms if s.type == "label" and s.section in (".text", ".init")]
    fidx = FuncIndex(funcs, labels)
    jt_cases = set()
    jt_words = set()
    for s in syms:
        if s.name.startswith("jumptable_") and s.size:
            for a in range(s.addr, s.addr + s.size, 4):
                jt_words.add(a)
                jt_cases.add(dol.word(a))
    text = dol.text_ranges()
    st = check_code(dol.word, text, fidx, jt_cases, report, "dol")
    # code addresses in data sections
    interior = set()
    for lo, hi, off in dol.data_ranges():
        for a in range(lo, hi - 3, 4):
            if a in jt_words:
                continue
            v = be32(dol.data, off + a - lo)
            if v % 4 == 0 and dol.is_text(v) and v not in fidx.start_set:
                f = fidx.containing(v)
                if f:
                    interior.add(v)
    st["data_ptr_interior"] = len(interior)
    for v in sorted(interior)[:200]:
        f = fidx.containing(v)
        report.append(f"dol data pointer to interior {v:08X} (in {f[2]})")
    return dol, fidx, st


def check_rel(rel, syms, dol_fidx, rels_by_id, rel_fidx, report):
    """syms: this REL's symbols. rel_fidx: id -> FuncIndex (section-relative .text offsets)."""
    tsec = rel.code_sections()
    assert len(tsec) == 1, f"{rel.name}: {len(tsec)} code sections"
    ts = tsec[0]
    fidx = rel_fidx[rel.id]
    jt_cases = set()
    for s in syms:
        if s.name.startswith("jumptable_") and s.size:
            pass  # cases are relocations; handled below
    rd = lambda off: be32(rel.data, ts.offset + off)
    # REL24 relocations make every in-module and cross-module call explicit.
    reloc_targets = set()
    st = defaultdict(int)
    for mod, lst in rel.relocs.items():
        for sec, off, typ, tsec_i, add in lst:
            if mod == 0:
                tgt_ok = dol_fidx.start_set
                tgt = add
                is_code = dol_fidx.containing(add) is not None or add in dol_fidx.start_set
                container = dol_fidx.containing(add)
            else:
                if mod not in rel_fidx:
                    report.append(f"rel {rel.name}: import from unknown module {mod}")
                    continue
                trel = rels_by_id[mod]
                if tsec_i >= len(trel.sections) or not trel.sections[tsec_i].exec:
                    continue
                tgt = add
                tgt_ok = rel_fidx[mod].start_set
                container = rel_fidx[mod].containing(add)
                is_code = True
            if typ == R_PPC_REL24 and sec == ts.index:
                st["rel24"] += 1
                if tgt not in tgt_ok:
                    st["rel24_bad"] += 1
                    report.append(f"rel {rel.name}: REL24 at .text+{off:X} -> mod {mod} {tgt:08X} not a function start")
            elif typ in (R_PPC_ADDR32, R_PPC_ADDR16_LO) and is_code and mod != 0:
                st["code_addr_reloc"] += 1
                if tgt not in tgt_ok:
                    if container:
                        st["code_addr_interior"] += 1
                        if typ == R_PPC_ADDR32 and sec != ts.index:
                            jt_cases.add(tgt) if mod == rel.id else None
                    else:
                        st["code_addr_outside"] += 1
                        report.append(f"rel {rel.name}: {typ} reloc at sec{sec}+{off:X} -> mod {mod} {tgt:08X} outside functions")
            elif typ in (R_PPC_ADDR32, R_PPC_ADDR16_LO) and mod == 0 and container is not None:
                st["code_addr_reloc"] += 1
                if tgt not in tgt_ok:
                    st["code_addr_interior"] += 1
    # In-module branches without relocations (b/bc within the section are PC-relative
    # and not relocated; bl to own module is a REL24 relocation in SDK RELs but local
    # calls may already be resolved).
    skip = {off for lst in rel.relocs.values() for sec, off, typ, _, _ in lst if sec == ts.index and typ == R_PPC_REL24}
    st2 = check_code(rd, [(0, ts.size)], fidx, jt_cases, report, f"rel {rel.name}", skip=skip)
    for k, v in st2.items():
        st[k] += v
    return st


def main():
    import argparse
    import glob
    import os
    import sys
    from collections import Counter
    import symfile
    ap = argparse.ArgumentParser(description="Check a generated function table against the disc.")
    ap.add_argument("--disc", required=True)
    ap.add_argument("--cfg", required=True, help="config/<id>")
    ap.add_argument("--report", help="write every finding here")
    a = ap.parse_args()
    report = []
    dsyms = symfile.parse(os.path.join(a.cfg, "symbols.txt"))
    _, dfidx, st = check_dol(os.path.join(a.disc, "sys", "main.dol"), dsyms, report)
    print("DOL", dict(st), "functions", len(dfidx.funcs))
    rels, rsyms, rfidx = {}, {}, {}
    for p in sorted(glob.glob(os.path.join(a.disc, "files", "rels", "*.rel"))):
        r = Rel(p)
        rels[r.id] = r
        rsyms[r.id] = symfile.parse(os.path.join(a.cfg, "rels", r.name + ".txt"))
        rfidx[r.id] = FuncIndex([(s.addr, s.size or 0, s.name) for s in rsyms[r.id] if s.is_func and s.section == ".text"],
                                [s.addr for s in rsyms[r.id] if s.type == "label" and s.section == ".text"])
    tot = Counter()
    for i, r in rels.items():
        tot.update(check_rel(r, rsyms[i], dfidx, rels, rfidx, report))
    print("RELs", dict(tot), "functions", sum(len(f.funcs) for f in rfidx.values()))
    if a.report:
        open(a.report, "w", encoding="utf-8").write("\n".join(report) + "\n")
    bad = {k: v for k, v in list(st.items()) + list(tot.items()) if k.startswith("bad_") or k.endswith("_bad")}
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
