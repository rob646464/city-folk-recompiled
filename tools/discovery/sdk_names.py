#!/usr/bin/env python3
"""Name the DOL's SDK / library functions from other Wii decomps' symbol maps.

    python tools/discovery/sdk_names.py --id RUUE01 --refs <dir> [--fetch]

Reference maps: the committed decomp-toolkit configs (symbols.txt + splits.txt, address
and name data only) of public CC0 Wii decomps that link the same RVL SDK, NintendoWare,
EGG and MSL libraries (list in REFS; `--fetch` downloads them into --refs; nothing of
them is committed here, no source code is read).

Method (docs/DISCOVERY.md "Names"):
  * anchors: functions decomp-toolkit's own signatures named (`dtk`), plus every
    function a reference names that sits at a run of equal function sizes;
  * from each anchor, walk forward and backward through our function list and the
    reference's library functions together while the function sizes are equal
    (libraries keep their link order, functions keep their order inside a unit);
    after a size mismatch the walk resynchronises only on 3 equal sizes in a row;
  * a function between two size-matched neighbours whose own size differs (a
    different SDK revision of the same function) gets the name with confidence `mid`;
  * the references vote; disagreements are reported and left to the higher vote.
Writes config/<id>/names.txt: `<module> <addr> <name> <confidence> <sources> <tu>`.
"""
import argparse
import re
import sys
import urllib.request
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import symfile  # noqa: E402

# name -> (repository, branch, config path). All CC0-1.0.
REFS = {
    "smg_RMGE01": ("SMGCommunity/Petari", "master", "config/RMGE01"),
    "mkw_RMCP01": ("doldecomp/mkw", "main", "config/RMCP01"),
    "brawl_RSBE01_02": ("doldecomp/brawl", "main", "config/RSBE01_02"),
    "ss_SOUE01": ("zeldaret/ss", "main", "config/SOUE01"),
    "ogws_RSPE01_01": ("doldecomp/ogws", "master", "config/RSPE01_01"),
    "tp_RZDE01_02": ("zeldaret/tp", "main", "config/RZDE01_02"),
    "spm_EU0": ("SeekyCt/spm-decomp", "master", "config/EU0"),
}

# translation units that are shared libraries (not game code)
LIB_TU = re.compile(
    r"^(revolution/|RVL_SDK/|rvl/|sdk/|nw4r/|egg/|MSL|PowerPC_EABI_Support/|Runtime|runtime/|"
    r"MetroTRK/|TRK_MINNOW_DOLPHIN/|RVLFaceLib|rfl/|homebutton|hbm/|revolution/homebuttonLib|"
    r"dwc/|DWC|gamespy/|nhttp|NdevExi2A|libc/|PPCEABI|Runtime\.PPCEABI)", re.I)
# names that are library functions even when their unit is not split in the reference
LIB_NAME = re.compile(
    r"^(OS|__OS|GX|__GX|VI|__VI|DVD|__DVD|NAND|__NAND|ISFS|IOS_|IPC|__IPC|ES_|__ES|SC|__SC|STM|__STM|"
    r"WPAD|__WPAD|KPAD|WUD|__WUD|AX|__AX|AI|__AI|DSP|__DSP|ARC|TPL|MEM|__MEM|NWC24|__NWC24|SO|__SO|NCD|"
    r"DWC|HBM|CARD|__CARD|SI|__SI|PAD|__PAD|EXI|__EXI|DB|__DB|PPC|MTX|PSMTX|PSVEC|C_MTX|C_VEC|VEC|"
    r"RFL|__RFL|NHTTP|SSL|ENC|KBD|MP|MPDL|PMIC|VF|bta_|bte_|BTA_|BTE_|BTM_|BTU_|L2CA_|L2C_|SDP_|HCI|"
    r"btu_|btm_|l2c_|sdp_|gap_|hid|HID|GKI_|gki_|__init_|__start|__fini|__ct__Q2[0-9]+nw4r|"
    r"__ct__Q3[0-9]+nw4r|.*nw4r|.*Q23EGG|.*Q33EGG|.*4EGG|_save|_rest|__save|__rest|__ptmf|__dt__|"
    r"mem|str|wcs|sprintf|vsprintf|snprintf|vsnprintf|printf|fprintf|vprintf|sscanf|atoi|atof|strto|"
    r"abs|labs|fabs|sqrt|sin|cos|tan|atan|acos|asin|exp|log|pow|fmod|floor|ceil|frexp|ldexp|modf|"
    r"__kernel|__ieee|__fpclass|__signbit|__va_arg|__div2|__mod2|__shl2|__shr2|__cvt_|__register_|"
    r"__destroy_|__construct|__flush|__fill|TRK|__TRK|gTRK|nub|InitMetroTRK|EnableMetroTRK|"
    r"__close|__read|__write|__put|__get|fwrite|fread|fopen|fclose|fflush|fseek|ftell|__file|"
    r"__ExceptionTable|__find|__end_critical|__begin_critical|__unexpected|__terminate|"
    r"__cxa|__dynamic_cast|__RTTI|__as__|__eq__)", re.I)
GENERIC = re.compile(r"^(fn_|lbl_|entry_|func_|FUN_|sub_|zz_|@)")


def fetch(refdir):
    raw = "https://raw." + "git" + "hubusercontent.com/"
    for name, (repo, br, path) in REFS.items():
        d = refdir / name
        d.mkdir(parents=True, exist_ok=True)
        for f in ("symbols.txt", "splits.txt"):
            if not (d / f).exists():
                (d / f).write_bytes(urllib.request.urlopen(f"{raw}{repo}/{br}/{path}/{f}").read())
                print("fetched", name, f)


def canon_tu(tu):
    """One spelling per unit across the references (RVL_SDK/vi/vi.c, rvl/vi/vi.c and
    revolution/VI/vi.c are the same file)."""
    if not tu:
        return tu
    base = tu.rsplit("/", 1)[-1]
    m = re.match(r"^(?:RVL_SDK|rvl|revolution)/(?:homebuttonLib/)?([^/]+)/(.*)$", tu)
    if m and not tu.startswith("revolution/homebuttonLib"):
        return f"revolution/{m.group(1).lower()}/{m.group(2)}"
    if "homebutton" in tu.lower() or tu.startswith("hbm/"):
        rest = re.sub(r"^(?:revolution/)?(?:homebuttonLib|homebuttonMiniLib|hbm)/", "", tu)
        return "homebutton/" + rest
    if "MSL" in tu:
        return "MSL_C/" + base
    if re.search(r"(^|/)Runtime", tu, re.I) or tu.startswith("runtime/"):
        return "Runtime/" + base
    if "TRK" in tu:
        return "TRK/" + base
    if tu.lower().startswith(("rvlfacelib", "rfl/")):
        return "RVLFaceLib/" + base
    return tu


def parse_splits(path):
    """-> [(lo, hi, tu)] for .text/.init ranges"""
    out = []
    tu = None
    for ln in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if ln and not ln[0].isspace() and ln.endswith(":") and ln != "Sections:":
            tu = ln[:-1]
            continue
        s = ln.strip()
        if tu and (s.startswith(".text") or s.startswith(".init")):
            m = re.search(r"start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", s)
            if m:
                out.append((int(m.group(1), 16), int(m.group(2), 16), tu))
    out.sort()
    return out


class Ref:
    def __init__(self, name, d):
        self.name = name
        syms = symfile.parse(d / "symbols.txt")
        splits = parse_splits(d / "splits.txt") if (d / "splits.txt").exists() else []
        fs = [s for s in syms if s.is_func and s.section in (".text", ".init") and s.size]
        fs.sort(key=lambda s: s.addr)
        import bisect
        los = [r[0] for r in splits]
        self.funcs = []  # (size, name, tu, lib)
        for s in fs:
            i = bisect.bisect_right(los, s.addr) - 1
            tu = splits[i][2] if i >= 0 and s.addr < splits[i][1] else ""
            named = not GENERIC.match(s.name)
            lib = named and (bool(LIB_TU.match(tu)) if tu else bool(LIB_NAME.match(s.name)))
            self.funcs.append((s.size, s.name, canon_tu(tu), lib))
        cnt = Counter(f[1] for f in self.funcs)
        self.index = {f[1]: i for i, f in enumerate(self.funcs) if cnt[f[1]] == 1}


def kgram_seeds(ours, ref, k=5, min_bytes=0x180):
    """Pairs (our index, ref index) where k consecutive function sizes agree and the
    k-gram is unique on both sides (and not trivially small)."""
    def grams(sizes, ok):
        out = defaultdict(list)
        for i in range(len(sizes) - k + 1):
            if all(ok[i:i + k]):
                g = tuple(sizes[i:i + k])
                if sum(g) >= min_bytes and len(set(g)) > 2:
                    out[g].append(i)
        return out
    rs = [f[0] for f in ref.funcs]
    rok = [f[3] for f in ref.funcs]
    os_ = [o[1] for o in ours]
    rg = grams(rs, rok)
    og = grams(os_, [True] * len(os_))
    pairs = []
    for g, js in rg.items():
        if len(js) == 1 and len(og.get(g, ())) == 1:
            i, j = og[g][0], js[0]
            pairs.extend((i + t, j + t) for t in range(k))
    return pairs


def walk(ours, ref, oi, ri, step, props, srcname):
    """From a paired (oi, ri), walk in direction step proposing names."""
    n, m = len(ours), len(ref.funcs)
    i, j = oi + step, ri + step
    misses = 0
    prev_ok = True
    while 0 <= i < n and 0 <= j < m:
        rsize, rname, rtu, rlib = ref.funcs[j]
        if not rlib:
            break
        if ours[i][1] == rsize:
            props[i].append((rname, "high", srcname, rtu))
            i += step
            j += step
            misses = 0
            prev_ok = True
            continue
        # size mismatch: resync on 3 equal sizes after skipping in either list
        best = None
        for di, dj in ((1, 1), (1, 0), (0, 1), (2, 1), (1, 2), (2, 0), (0, 2), (2, 2), (3, 0), (0, 3)):
            # 3 equal sizes in a row, or 2 that are large enough to be distinctive
            k = 0
            run_bytes = 0
            while k < 3:
                a, b = i + step * (di + k), j + step * (dj + k)
                if not (0 <= a < n and 0 <= b < m) or not ref.funcs[b][3] or ours[a][1] != ref.funcs[b][0]:
                    break
                run_bytes += ours[a][1]
                k += 1
            if k == 3 or (k == 2 and run_bytes >= 0x200):
                best = (di, dj)
                break
        if best is None:
            break
        di, dj = best
        if di == 1 and dj == 1 and prev_ok:
            # same function, another revision (both neighbours match)
            props[i].append((rname, "mid", srcname, rtu))
        i += step * di
        j += step * dj
        prev_ok = False
        misses += 1
        if misses > 6:
            break


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--id", default="RUUE01")
    ap.add_argument("--refs", required=True, help="dir with <ref>/symbols.txt + splits.txt")
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--report")
    a = ap.parse_args()
    refdir = Path(a.refs)
    if a.fetch:
        fetch(refdir)
    cfg = ROOT / "config" / a.id
    syms = symfile.parse(cfg / "symbols.txt")
    ours = sorted((s.addr, s.size or 0, s.name) for s in syms if s.is_func)
    refs = [Ref(n, refdir / n) for n in REFS if (refdir / n / "symbols.txt").exists()]
    # existing names (dtk signatures) are anchors
    final = {}  # our index -> (name, conf, sources, tu)
    prev = {}
    if (cfg / "names.txt").exists():
        for ln in (cfg / "names.txt").read_text(encoding="utf-8").splitlines():
            p = ln.split()
            if len(p) >= 4 and not ln.startswith("#"):
                prev[int(p[1], 16)] = p[3]
    for i, (addr, size, name) in enumerate(ours):
        if prev.get(addr, "dtk") != "dtk":
            continue  # a name this tool gave on an earlier run: decide again
        if not GENERIC.match(name):
            final[i] = (name, "dtk", "dtk", "")
    log = []
    multi = set()  # names some reference uses for several functions (statics)
    for ref in refs:
        c = Counter(f[1] for f in ref.funcs)
        multi |= {n for n, v in c.items() if v > 1}
    for rnd in range(6):
        props = defaultdict(list)
        for ref in refs:
            # seed pairs: every named function of ours that the ref names uniquely
            pairs = []
            for i, (nm, conf, src, tu) in final.items():
                j = ref.index.get(nm)
                if j is not None and ref.funcs[j][3] and ref.funcs[j][0] == ours[i][1]:
                    pairs.append((i, j))
            if rnd == 0:
                pairs += kgram_seeds(ours, ref)
            for i, j in pairs:
                props[i].append((ref.funcs[j][1], "high", ref.name, ref.funcs[j][2]))
                walk(ours, ref, i, j, 1, props, ref.name)
                walk(ours, ref, i, j, -1, props, ref.name)
        added = 0
        used = {v[0]: k for k, v in final.items()}
        for i in sorted(props):
            if i in final and final[i][1] == "dtk":
                continue
            votes = Counter()
            info = {}
            for nm, conf, src, tu in props[i]:
                votes[nm] += 2 if conf == "high" else 1
                info.setdefault(nm, [set(), conf, tu])
                info[nm][0].add(src)
                if conf == "high":
                    info[nm][1] = "high"
                if tu and not info[nm][2]:
                    info[nm][2] = tu
            (nm, v), *rest = votes.most_common()
            if rest and rest[0][1] == v:
                log.append(f"tie at {ours[i][0]:08X}: {votes.most_common(4)}")
                continue
            if rest:
                log.append(f"disagree at {ours[i][0]:08X}: {votes.most_common(4)}")
            if nm in used and used[nm] != i and nm not in multi:
                log.append(f"duplicate {nm} at {ours[i][0]:08X} and {ours[used[nm]][0]:08X}")
                continue
            srcs, conf, tu = info[nm]
            new = (nm, conf, ",".join(sorted(srcs)), tu)
            if final.get(i, (None,))[0] != nm:
                added += 1
            final[i] = new
            used[nm] = i
        print(f"round {rnd}: {len(final)} named (+{added})")
        if not added:
            break
    # gap fill: between two named functions that a reference also has in the same order,
    # an equal number of unnamed functions on both sides are the same functions
    for rnd in range(3):
        props = defaultdict(list)
        named_idx = sorted(final)
        for ref in refs:
            # our named functions this reference also has, consecutive in our order
            anchors = [i for i in named_idx if final[i][0] in ref.index]
            for ia, ib in zip(anchors, anchors[1:]):
                if ib - ia < 2 or ib - ia > 40:
                    continue
                ja, jb = ref.index.get(final[ia][0]), ref.index.get(final[ib][0])
                if ja is None or jb is None or not 1 < jb - ja <= 40:
                    continue
                mid = ref.funcs[ja + 1:jb]
                if not all(f[3] for f in mid):
                    continue
                if jb - ja == ib - ia and all(final[ia + 1 + k][0] == f[1] for k, f in enumerate(mid)
                                              if ia + 1 + k in final):
                    for k, f in enumerate(mid):
                        props[ia + 1 + k].append((f[1], f[2], ref.name))
                    continue
                # different counts (functions added/stripped): order-preserving
                # alignment on equal sizes (LCS); only non-trivial sizes are named
                osz = [ours[k][1] for k in range(ia + 1, ib)]
                rsz = [f[0] for f in mid]
                L = [[0] * (len(rsz) + 1) for _ in range(len(osz) + 1)]
                for x in range(len(osz) - 1, -1, -1):
                    for y in range(len(rsz) - 1, -1, -1):
                        L[x][y] = L[x + 1][y + 1] + 1 if osz[x] == rsz[y] else max(L[x + 1][y], L[x][y + 1])
                x = y = 0
                while x < len(osz) and y < len(rsz):
                    if osz[x] == rsz[y] and L[x][y] == L[x + 1][y + 1] + 1:
                        if osz[x] >= 0x20:
                            props[ia + 1 + x].append((mid[y][1], mid[y][2], ref.name))
                        x += 1
                        y += 1
                    elif L[x + 1][y] >= L[x][y + 1]:
                        x += 1
                    else:
                        y += 1
        used = {v[0] for v in final.values()}
        added = 0
        for i, ps in sorted(props.items()):
            if i in final:
                continue
            votes = Counter(p[0] for p in ps)
            (nm, v), *rest = votes.most_common()
            if (rest and rest[0][1] == v) or (nm in used and nm not in multi):
                continue
            srcs = sorted({p[2] for p in ps if p[0] == nm})
            tu = next(p[1] for p in ps if p[0] == nm)
            final[i] = (nm, "gap", ",".join(srcs), tu)
            used.add(nm)
            added += 1
        print(f"gap fill {rnd}: +{added}")
        if not added:
            break

    lines = ["# <module> <addr> <name> <confidence: dtk|high|mid|gap|str|manual> <sources> <translation unit>",
             "# Generated by tools/discovery/sdk_names.py (docs/DISCOVERY.md). Manual names: names_manual.txt"]
    manual = cfg / "names_manual.txt"
    man = {}
    if manual.exists():
        for ln in manual.read_text(encoding="utf-8").splitlines():
            p = ln.split()
            if len(p) >= 3 and not ln.startswith("#"):
                man[int(p[1], 16)] = p[2:]
    for i, (addr, size, name) in enumerate(ours):
        if addr in man:
            p = man.pop(addr)
            lines.append(f"dol {addr:08X} {p[0]} manual {' '.join(p[1:]) or 'manual'} -")
        elif i in final:
            nm, conf, src, tu = final[i]
            lines.append(f"dol {addr:08X} {nm} {conf} {src} {tu or '-'}")
    for addr, p in sorted(man.items()):
        lines.append(f"dol {addr:08X} {p[0]} manual {' '.join(p[1:]) or 'manual'} -")
    (cfg / "names.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    if a.report:
        Path(a.report).write_text("\n".join(log) + "\n", encoding="utf-8")
    c = Counter(v[1] for v in final.values())
    print("named:", dict(c), "conflicts logged:", len(log))


if __name__ == "__main__":
    main()
