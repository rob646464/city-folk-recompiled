#!/usr/bin/env python3
"""Build the function tables for one disc version (docs/DISCOVERY.md).

    python tools/discovery/discover.py --disc "US DATA/DATA" --id RUUE01 --dtk <dtk.exe>
        [--work <scratch dir>] [--out config/RUUE01] [--skip-dtk]

1. decomp-toolkit (`dtk dol split`, dev-only tool, MIT) analyses main.dol and every REL
   in files/rels/ into <work>/config (functions, objects, relocations).
2. analysis.py completes the function tables (jump tables, entry points of every call,
   relocation and data-pointer target, code in gaps).
3. Names from config/<id>/names.txt (written by sdk_names.py / version_map.py) replace
   dtk's fn_ names.
4. Writes config/<id>/symbols.txt (DOL), config/<id>/rels/<module>.txt (one per REL,
   section-relative), config/<id>/modules.txt (module ids, sections, imports) and
   config/<id>/coverage.txt.

Reads only the user's own disc files; writes address/name tables only.
"""
import argparse
import hashlib
import os
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import analysis  # noqa: E402
import modules  # noqa: E402
import symfile  # noqa: E402
from symfile import Sym  # noqa: E402


def sha1(p):
    return hashlib.sha1(Path(p).read_bytes()).hexdigest()


def dtk_path_arg(p):
    # dtk on Windows wants drive-less absolute paths in its YAML ("/Users/..."), on the
    # current drive.
    s = str(Path(p).resolve()).replace("\\", "/")
    if len(s) > 2 and s[1] == ":":
        s = s[2:]
    return s


def run_dtk(dtk, disc, work):
    work.mkdir(parents=True, exist_ok=True)
    dol = disc / "sys" / "main.dol"
    lines = [f'object: "{dtk_path_arg(dol)}"', f"hash: {sha1(dol)}",
             "symbols: config/symbols.txt", "splits: config/splits.txt", "modules:"]
    for rel in sorted((disc / "files" / "rels").glob("*.rel")):
        lines += [f'- object: "{dtk_path_arg(rel)}"', f"  hash: {sha1(rel)}",
                  f"  symbols: config/{rel.stem}/symbols.txt", f"  splits: config/{rel.stem}/splits.txt"]
    (work / "config.yml").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    # dtk refuses to overwrite nothing; start from a clean config so it re-analyses
    cfg = work / "config"
    if cfg.exists():
        for p in sorted(cfg.rglob("*"), reverse=True):
            p.unlink() if p.is_file() else p.rmdir()
    subprocess.run([str(dtk), "dol", "split", "config.yml", "out"], cwd=work, check=True)


def load_names(path):
    """names.txt: '<module> <addr> <name> <confidence> <source...>' -> {(mid, addr): name}"""
    names = {}
    if not path.exists():
        return names
    for ln in path.read_text(encoding="utf-8").splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        parts = ln.split()
        mid, addr, name = parts[0], int(parts[1], 16), parts[2]
        names[(0 if mid in ("dol", "main", "0") else int(mid), addr)] = name
    return names


def build(dol, rels, report):
    F = {0: analysis.Funcs(dol)}
    for mid, m in rels.items():
        F[mid] = analysis.Funcs(m)
    stats = defaultdict(Counter)
    jts = {}
    allm = [(0, dol)] + sorted(rels.items())
    # two rounds: splits and new functions can expose new tables/targets
    for rnd in range(3):
        changed = 0
        for mid, m in allm:
            jts[mid] = analysis.find_jump_tables(m, F[mid])
            k = analysis.merge_switch_tails(m, F[mid], jts[mid], report)
            if k:
                stats[mid]["switch_merge"] += k
                changed += k
                jts[mid] = analysis.find_jump_tables(m, F[mid])
        tg = analysis.collect_targets(dol, rels, F, jts)
        for mid, m in allm:
            excl = (lambda a: in_trk_vectors(dol, a) or in_object(dol, a)) if mid == 0 else (lambda a: False)
            st = analysis.apply_targets(m, F[mid], tg.get(mid, {}), report, excl)
            stats[mid].update(st)
            changed += sum(st.values())
        # code in gaps
        for mid, m in allm:
            fs = F[mid]
            for lo, hi in analysis.code_gaps(m, fs):
                if mid == 0 and in_trk_vectors(dol, lo):
                    continue
                if mid == 0 and in_object(dol, lo):
                    continue
                fs.f[lo] = [hi - lo, fs.fname(lo)]
                fs.origin[lo] = "gap-code"
                fs._index()
                stats[mid]["gap_code_funcs"] += 1
                changed += 1
                report.append(f"{m.name}: code in a gap {lo:08X}-{hi:08X} made a function")
        if not changed:
            break
    for mid, m in allm:
        jts[mid] = analysis.find_jump_tables(m, F[mid])
    return F, jts, stats


def in_trk_vectors(dol, a):
    lo = hi = None
    for s in dol.syms:
        if s.name == "gTRKInterruptVectorTable":
            lo = s.addr
        elif s.name == "gTRKInterruptVectorTableEnd":
            hi = s.addr
    return lo is not None and hi is not None and lo <= a < hi


def in_object(m, a):
    for s in m.syms:
        if s.type == "object" and s.size and s.addr <= a < s.addr + s.size and m.is_code(s.addr):
            return True
    return False


def module_syms(m, fs, tabs, names):
    """Final symbol list of one module."""
    out = []
    taken = {}
    funcs = dict(fs.f)
    code_sec = (lambda a: m.code_sec_name(a))
    for s in m.syms:
        if s.is_func and m.is_code_sym(s):
            continue  # re-emitted below with final sizes
        if s.type == "label" and m.is_code_sym(s):
            continue
        out.append(s)
    for a, (size, name) in funcs.items():
        n = names.get((m.id, a), name)
        attrs = ""
        out.append(Sym(n, code_sec(a), a, "function", size, attrs))
    for a, name in fs.labels.items():
        out.append(Sym(names.get((m.id, a), name), code_sec(a), a, "label", None, "scope:local"))
    # jump tables: rename the object dtk put at the table (or add one)
    by_loc = {}
    for s in out:
        if s.type == "object":
            by_loc.setdefault((s.section, s.addr), s)
    for pc, (loc, cases) in sorted(tabs.items()):
        if m.kind == "dol":
            sec, addr = m.sec_name(loc), loc
            jname = f"jumptable_{addr:08X}"
        else:
            sec, addr = m.secnames.get(loc[0], f".sec{loc[0]}"), loc[1]
            jname = f"jumptable_{m.id}_{sec.lstrip('.')}_{addr:X}"
        size = 4 * len(cases)
        s = by_loc.get((sec, addr))
        if s is not None:
            if not s.name.startswith("jumptable_"):
                s.name = jname
            s.size = max(s.size or 0, size)
        else:
            s = Sym(jname, sec, addr, "object", size, "scope:local")
            out.append(s)
            by_loc[(sec, addr)] = s
    # keep dtk's original attributes (scope etc.) for functions it already had
    attrs = {(s.section, s.addr): s.attrs for s in m.syms if s.is_func}
    for s in out:
        if s.is_func and (s.section, s.addr) in attrs:
            s.attrs = attrs[(s.section, s.addr)]
    order = {}
    if m.kind == "dol":
        for i, (idx, addr, *_r) in enumerate(m.dol.sections):
            order[m.secnames.get(idx)] = addr
        key = lambda s: (s.addr, 0 if s.type == "function" else 1)
    else:
        for idx, nm in m.secnames.items():
            order[nm] = idx
        key = lambda s: (order.get(s.section, 99), s.addr, 0 if s.type == "function" else 1)
    out.sort(key=key)
    return out


def write_dol(path, dol, syms, header):
    lines = [f"// {h}" for h in header]
    lines += [s.line() for s in syms]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", required=True, help="extracted game partition (sys/, files/)")
    ap.add_argument("--id", required=True, help="game id, e.g. RUUE01")
    ap.add_argument("--dtk", help="decomp-toolkit executable")
    ap.add_argument("--work", help="scratch dir for dtk output (default build/discovery/<id>)")
    ap.add_argument("--out", help="output dir (default config/<id>)")
    ap.add_argument("--skip-dtk", action="store_true", help="reuse <work>/config")
    a = ap.parse_args()
    disc = Path(a.disc)
    work = Path(a.work) if a.work else ROOT / "build" / "discovery" / a.id
    out = Path(a.out) if a.out else ROOT / "config" / a.id
    if not a.skip_dtk:
        if not a.dtk:
            sys.exit("--dtk is required (or --skip-dtk with an existing --work)")
        run_dtk(Path(a.dtk), disc, work)
    dol, rels = modules.load(disc, work / "config")
    report = []
    F, jts, stats = build(dol, rels, report)
    names = load_names(out / "names.txt")
    out.mkdir(parents=True, exist_ok=True)
    (out / "rels").mkdir(exist_ok=True)

    # DOL
    dsyms = module_syms(dol, F[0], jts[0], names)
    write_dol(out / "symbols.txt", dol, dsyms, [
        f"{a.id} main.dol sha1:{sha1(disc / 'sys' / 'main.dol')} entry:0x{dol.dol.entry:08X}",
        "Generated by tools/discovery/discover.py (docs/DISCOVERY.md). Do not edit; edit names.txt.",
    ])
    write_units(out, dsyms)
    # RELs
    mod_lines =["# <id> <name> <sha1> <section:index:size:flags...> imports:<module ids>"]
    for mid, m in sorted(rels.items()):
        rs = module_syms(m, F[mid], jts[mid], names)
        r = m.rel
        secs = " ".join(f"{m.secnames[s.index]}:{s.index}:0x{s.size:X}:{'x' if s.exec else ('b' if s.bss else 'd')}"
                        for s in r.sections if s.size)
        imports = ",".join(str(i) for i in sorted(r.relocs))
        hdr = [
            f"module:{mid} name:{m.name} file:files/rels/{m.name}.rel sha1:{sha1(r.path)}",
            f"version:{r.version} sections: {secs}",
            f"prolog:{m.secnames.get(r.prolog_sec, '-')}:0x{r.prolog:X} epilog:{m.secnames.get(r.epilog_sec, '-')}:0x{r.epilog:X} "
            f"unresolved:{m.secnames.get(r.unresolved_sec, '-')}:0x{r.unresolved:X} bss_size:0x{r.bss_size:X} "
            f"align:0x{r.align:X} bss_align:0x{r.bss_align:X} fix_size:0x{r.fix_size:X}",
            f"imports (modules this one relocates against): {imports}",
            "Addresses are offsets into the named section. Generated by tools/discovery/discover.py.",
        ]
        write_dol(out / "rels" / f"{m.name}.txt", m, rs, hdr)
        mod_lines.append(f"{mid} {m.name} {sha1(r.path)} {secs} imports:{imports}")
    (out / "modules.txt").write_text("\n".join(mod_lines) + "\n", encoding="utf-8", newline="\n")

    # coverage
    cov = coverage(dol, rels, F, jts, names, stats)
    (out / "coverage.txt").write_text("\n".join(cov) + "\n", encoding="utf-8", newline="\n")
    (work / "report.txt").write_text("\n".join(report) + "\n", encoding="utf-8", newline="\n")
    print("\n".join(cov[:12]))


def write_units(out, dsyms):
    """config/<id>/splits.txt: .text ranges of the library units the reference maps
    place the named functions in (consecutive functions of one unit; unnamed functions
    between two functions of the same unit are included). Library code only."""
    tus = {}
    p = out / "names.txt"
    if p.exists():
        for ln in p.read_text(encoding="utf-8").splitlines():
            q = ln.split()
            if len(q) >= 6 and not ln.startswith("#") and q[0] == "dol" and q[5] != "-":
                tus[int(q[1], 16)] = q[5]
    funcs = [s for s in dsyms if s.is_func]
    ranges = []  # [tu, lo, hi]
    pending = []
    for s in funcs:
        tu = tus.get(s.addr)
        if tu is None:
            pending.append(s)
            continue
        if ranges and ranges[-1][0] == tu:
            ranges[-1][2] = s.addr + (s.size or 0)
        else:
            ranges.append([tu, s.addr, s.addr + (s.size or 0)])
        pending = []
    lines = ["// Library translation units of main.dol from the reference maps (tools/discovery/sdk_names.py).",
             "// Partial: only .text, only units with named functions. Format as decomp-toolkit splits.txt.",
             "Sections:", "\t.init       type:code align:4", "\t.text       type:code align:32", ""]
    seen = Counter()
    for tu, lo, hi in ranges:
        seen[tu] += 1
        name = tu if seen[tu] == 1 else f"{tu}#{seen[tu]}"
        lines += [f"{name}:", f"\t.text       start:0x{lo:08X} end:0x{hi:08X}", ""]
    (out / "splits.txt").write_text("\n".join(lines), encoding="utf-8", newline="\n")


def coverage(dol, rels, F, jts, names, stats):
    lines = ["# <module> <name> functions labels jumptables named code_bytes in_functions zero_gap other_gap_bytes new(split/label/gap)"]
    tot = Counter()
    for mid, m in [(0, dol)] + sorted(rels.items()):
        fs = F[mid]
        code = sum(hi - lo for lo, hi in m.code)
        infn = sum(fs.f[s][0] for s in fs.starts)
        gaps = analysis.code_gaps(m, fs)
        gapb = sum(hi - lo for lo, hi in gaps)
        named = sum(1 for s in fs.starts
                    if not names.get((mid, s), fs.f[s][1]).startswith(("fn_", "entry_")))
        st = stats[mid]
        new = f"{st['split']}/{st['label']}/{st['new_in_gap'] + st['gap_code_funcs']}"
        lines.append(f"{mid} {m.name} {len(fs.f)} {len(fs.labels)} {len(jts[mid])} {named} {code} {infn} {code - infn - gapb} {gapb} {new}")
        k = "dol" if mid == 0 else "rel"
        tot[k + "_funcs"] += len(fs.f)
        tot[k + "_labels"] += len(fs.labels)
        tot[k + "_jt"] += len(jts[mid])
        tot[k + "_named"] += named
        tot[k + "_code"] += code
        tot[k + "_infn"] += infn
        tot[k + "_gap"] += gapb
    head = [
        f"# DOL: {tot['dol_funcs']} functions ({tot['dol_named']} named), {tot['dol_labels']} interior entry labels, "
        f"{tot['dol_jt']} jump tables, {tot['dol_infn']}/{tot['dol_code']} code bytes in functions, {tot['dol_gap']} non-zero bytes outside",
        f"# RELs ({len(rels)}): {tot['rel_funcs']} functions ({tot['rel_named']} named), {tot['rel_labels']} labels, "
        f"{tot['rel_jt']} jump tables, {tot['rel_infn']}/{tot['rel_code']} code bytes in functions, {tot['rel_gap']} non-zero bytes outside",
    ]
    return head + lines


if __name__ == "__main__":
    main()
