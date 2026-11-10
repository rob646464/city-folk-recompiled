"""Static check of the frame interpolation's draw coverage (branch interp-match; docs/WII.md "Matching
untagged draws"): every GX draw call site in the recompiled game, and what the interpolation pairs it by.

    py tools/interp/draw_coverage.py [--gen build/windows-clang/recomp_gen] [--config config/RUUE01]
                                     [--sites <CF_MATCH_SITES file> ...] [--all] [--out <file>]

Reads the recompiler's generated C++ (it knows the call graph: one function per game function, direct
calls with their return addresses, indirect calls through CTR) and the function names. A draw site is a
call of GXBegin (immediate-mode geometry) or GXCallDisplayList (a display list), display lists written straight into
the write-gather pipe appear in the runtime part (--sites). Each site is classified:

- tagged: reached by direct calls from a hook that opens a per-thing identity scope (a g3d model's
  G3dProc, a field object's draw, an ef billboard particle, a lyt pane), or inside the library whose
  draws only happen under such a hook (g3d model drawing, lyt panes). Values pair by identity.
- fingerprint: drawn under a shared scope (a ScnProc's user draw, an emitter, a view): pairs by the
  draw's fingerprint and world place (matchDraws). The fingerprint quality is listed: display list
  (address: the mesh), immediate with indexed positions (array + vertex count + textures), immediate
  with direct positions (vertex count + textures; place = the vertices' centroid).
- needs help: 2D draws outside lyt (orthographic: never place-matched), or a run (--sites) shows that
  most of the site's draws found no sure partner.

--sites merges the runtime table written by CF_MATCH_SITES (per GX call site: draws, keyed uniquely,
matched by place, left at the game frame, untagged). Only the user's own build output is read.
"""
import argparse
import collections
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FUNC = re.compile(r"^void ((?:f_[0-9A-F]{8})|(?:r\d{3}_\d+_[0-9A-F]{6}))(_orig)?\(Ctx& c\) \{")
CALL = re.compile(r"\b((?:f_[0-9A-F]{8})|(?:r\d{3}_\d+_[0-9A-F]{6}))\(c\);")
LR = re.compile(r"c\.lr = (?:0x([0-9A-F]{8})u|\(rb_ \+ 0x([0-9A-F]+)u\));")
SYM = re.compile(r"^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function")

SINKS = {"GXBegin": "immediate", "GXCallDisplayList": "display list"}
# Hooks that open a per-thing tag scope (src/game/cf_tags.cpp, cf_hud.cpp) and what they give.
TAG_ROOTS = {
    "G3dProc__Q34nw4r3g3d12ScnMdlSimpleFUlUlPv": ("tagged", "g3d model (per scene object)"),
    "G3dProc__Q34nw4r3g3d6ScnMdlFUlUlPv": ("tagged", "g3d model (per scene object)"),
    "d_fg_drawNP: fn_112_2184": ("tagged", "field object (per place and kind)"),
    "DrawQuad__Q24nw4r2efFRCQ34nw4r4math5MTX34PCQ34nw4r4math5_VEC3b": ("tagged", "ef billboard particle (per particle)"),
    "Draw__Q34nw4r3lyt4PaneFRCQ34nw4r3lyt8DrawInfo": ("tagged", "lyt pane (per pane)"),
    "G3dProc__Q34nw4r3g3d7ScnProcFUlUlPv": ("shared", "g3d ScnProc (one scope for its user draw)"),
    "DrawOpa__Q34nw4r3g3d7ScnRootFv": ("view", "scene view (camera only)"),
    "DrawXlu__Q34nw4r3g3d7ScnRootFv": ("view", "scene view (camera only)"),
}
LIBS = [("4nw4r3g3d", "nw4r::g3d"), ("4nw4r3lyt", "nw4r::lyt"), ("4nw4r2ef", "nw4r::ef"), ("4nw4r2ut", "nw4r::ut"),
        ("4nw4r3snd", "nw4r::snd"), ("3EGG", "EGG"), ("Q23EGG", "EGG"), ("3RFL", "RFL"), ("nw4hbm", "HBM"), ("HBM", "HBM"), ("homebutton", "HBM")]


def lib_of(name):
    for key, lib in LIBS:
        if key in name:
            return lib
    if name.startswith(("GX", "__GX", "GD")):
        return "SDK GX"
    if name.startswith("RFL"):
        return "RFL"
    return None


def load_names(config):
    """DOL address -> name, and the library of every unnamed function sandwiched between two of one library."""
    names = {}
    for line in open(os.path.join(config, "symbols.txt"), encoding="utf-8"):
        m = SYM.match(line)
        if m:
            names[int(m.group(2), 16)] = m.group(1)
    addrs = sorted(names)
    libs = {}
    for i, a in enumerate(addrs):
        lib = lib_of(names[a])
        if lib:
            libs[a] = lib
            continue
        # (the nearest named function on each side, skipping other unnamed ones)
        prev = next((lib_of(names[addrs[j]]) for j in range(i - 1, -1, -1) if not names[addrs[j]].startswith("fn_")), None)
        nxt = next((lib_of(names[addrs[j]]) for j in range(i + 1, len(addrs)) if not names[addrs[j]].startswith("fn_")), None)
        if prev and prev == nxt:
            libs[a] = prev + " (unnamed)"
    return names, libs


def parse_gen(gen):
    funcs = {}  # id -> dict(name, calls [(lr, callee)], indirect, raw)
    for fn in sorted(os.listdir(gen)):
        if not (fn.startswith("funcs_") and fn.endswith(".cpp")):
            continue
        prevline = ""
        cur = None
        lr = None
        for line in open(os.path.join(gen, fn), encoding="utf-8", errors="replace"):
            m = FUNC.match(line)
            if m:
                fid, orig = m.group(1), m.group(2)
                if fid not in funcs:
                    nm = prevline[3:].strip() if prevline.startswith("// ") else fid
                    funcs[fid] = {"name": nm, "calls": [], "indirect": 0, "raw": False, "dl40": False}
                cur = funcs[fid]
                cur["hooked"] = cur.get("hooked", False) or bool(orig)
                lr = None
                prevline = line
                continue
            prevline = line
            if cur is None:
                continue
            if line.startswith("}"):
                cur = None
                continue
            ml = LR.search(line)
            if ml:
                lr = int(ml.group(1), 16) if ml.group(1) else ("rel", int(ml.group(2), 16))
            for mc in CALL.finditer(line):
                callee = mc.group(1)
                if callee == fid or line.strip().startswith(callee + "_orig"):
                    continue
                cur["calls"].append((lr, callee))
                lr = None
            if "c.call(c.ctr)" in line:
                cur["indirect"] += 1
            if "0xCC010000u" in line:
                cur["raw"] = True
            if "0x00000040u;" in line:
                cur["dl40"] = True
    return funcs


def addr_of(fid):
    return int(fid[2:], 16) if fid.startswith("f_") else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default=os.path.join(ROOT, "build", "windows-clang", "recomp_gen"))
    ap.add_argument("--config", default=os.path.join(ROOT, "config", "RUUE01"))
    ap.add_argument("--sites", nargs="*", default=[])
    ap.add_argument("--all", action="store_true", help="list every site (default: game code and libraries summarised)")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    out = open(a.out, "w", encoding="utf-8") if a.out else sys.stdout
    names, libs = load_names(a.config)
    funcs = parse_gen(a.gen)
    byname = collections.defaultdict(list)
    for fid, f in funcs.items():
        byname[f["name"]].append(fid)
    # Reachability downward from every tag root by direct calls.
    reach = collections.defaultdict(set)  # fid -> set of root names
    for root in TAG_ROOTS:
        for rid in byname.get(root, []):
            stack, seen = [rid], {rid}
            while stack:
                x = stack.pop()
                reach[x].add(root)
                for _, callee in funcs.get(x, {}).get("calls", []):
                    if callee in funcs and callee not in seen and callee not in (byname.get("GXBegin", []) + byname.get("GXCallDisplayList", [])):
                        seen.add(callee)
                        stack.append(callee)
    callers = collections.defaultdict(set)
    for fid, f in funcs.items():
        for _, callee in f["calls"]:
            callers[callee].add(fid)
    sink_ids = {fid: SINKS[f["name"]] for fid, f in funcs.items() if f["name"] in SINKS}
    # Functions that call GXSetArray / GXLoadTexObj / GXLoadPosMtxImm (fingerprint and place quality).
    flag_names = {"GXSetArray": "array", "GXLoadTexObj": "texture", "GXLoadPosMtxImm": "matrix", "GXSetVtxDesc": "desc"}
    flag_ids = {fid: flag_names[f["name"]] for fid, f in funcs.items() if f["name"] in flag_names}
    # Runtime table(s).
    runtime = collections.defaultdict(lambda: [0] * 6)
    for path in a.sites:
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            p = line.split()
            site = int(p[0], 16)
            for i in range(6):
                runtime[site][i] += int(p[3 + i])
    sites = []
    for fid, f in funcs.items():
        flags = {flag_ids[c] for _, c in f["calls"] if c in flag_ids}
        for lr, callee in f["calls"]:
            if callee in sink_ids:
                sites.append((fid, lr, sink_ids[callee], flags))
    rows = []
    for fid, lr, sink, flags in sites:
        f = funcs[fid]
        a0 = addr_of(fid)
        name = f["name"]
        lib = lib_of(name) or (libs.get(a0) if a0 else None)
        roots = reach.get(fid, set())
        kinds = {TAG_ROOTS[r][0] for r in roots}
        if lib in ("nw4r::g3d", "nw4r::g3d (unnamed)") and "tagged" not in kinds:
            # g3d draws its models inside their scene object's G3dProc (virtual calls the call graph cannot follow).
            kinds.add("tagged")
            roots = roots | {"(g3d model draw, via virtual G3dProc)"}
        if lib in ("nw4r::lyt", "nw4r::lyt (unnamed)") and "tagged" not in kinds:
            kinds.add("tagged")
            roots = roots | {"(lyt pane draw, via virtual DrawSelf)"}
        if "tagged" in kinds:
            verdict = "tagged"
        elif lib in ("nw4r::ut", "nw4r::ut (unnamed)", "HBM", "RFL", "EGG"):
            verdict = "2D/system"  # text, HOME Menu, Mii icons, frame buffer: 2D or own camera
        else:
            verdict = "fingerprint"
        if sink == "display list" or sink == "raw display list":
            fpq = "display list address"
        elif "array" in flags:
            fpq = "layout + position array + vertex count + textures"
        else:
            fpq = "layout + vertex count + textures; place from vertices" + ("" if "matrix" in flags else " (no matrix load here)")
        site_addr = lr if isinstance(lr, int) else None
        rt = runtime.get(site_addr) if site_addr else None
        if rt and rt[0]:
            draws, unique, matched, snapped, untagged, novalues = rt
            if (matched + snapped) >= 20 and snapped > 0.5 * (matched + snapped) and verdict != "tagged":
                verdict = "needs help"
        rows.append((verdict, lib or "game", name, fid, lr, sink, fpq, sorted(roots), rt))
    order = {"needs help": 0, "fingerprint": 1, "tagged": 2, "2D/system": 3}
    rows.sort(key=lambda r: (order[r[0]], r[1], r[2]))
    cnt = collections.Counter((r[0], r[1].split(" ")[0]) for r in rows)
    print(f"# Draw call sites in {os.path.relpath(a.gen, ROOT)}: {len(rows)} ({len(sink_ids)} draw entry points)", file=out)
    print("\n## Summary (verdict, owner: sites)\n", file=out)
    for (v, lib), n in sorted(cnt.items(), key=lambda x: (order[x[0][0]], -x[1])):
        print(f"- {v:12} {lib:12} {n}", file=out)
    seen_rt = sum(1 for r in rows if r[8] and r[8][0])
    print(f"\nSites drawn in the runs given (--sites): {seen_rt}", file=out)
    print("\n## Sites\n", file=out)
    print("| verdict | owner | function | call site | draw | fingerprint | tag scope roots (direct calls) | runtime draws / unique / matched / at game frame / untagged |", file=out)
    print("|---|---|---|---|---|---|---|---|", file=out)
    for v, lib, name, fid, lr, sink, fpq, roots, rt in rows:
        if not a.all and v in ("tagged", "2D/system") and not (rt and rt[0]):
            continue
        lrs = f"{lr:08X}" if isinstance(lr, int) else (f"+{lr[1]:X}" if lr else "-")
        rts = f"{rt[0]} / {rt[1]} / {rt[2]} / {rt[3]} / {rt[4]}" if rt and rt[0] else ""
        short = name if len(name) <= 70 else name[:67] + "..."
        print(f"| {v} | {lib} | `{short}` | {lrs} | {sink} | {fpq} | {', '.join(roots) if roots else '-'} | {rts} |", file=out)
    # Runtime sites the static scan did not name (REL code at its load address, raw writes).
    known = {r[4] for r in rows if isinstance(r[4], int)}
    extra = [(s, v) for s, v in runtime.items() if s not in known and v[0]]
    if extra:
        print("\n## Runtime draw sites without a static GXBegin / GXCallDisplayList call (REL modules at their load address, raw writes)\n", file=out)
        print("| site | draws / unique / matched / at game frame / untagged |", file=out)
        print("|---|---|", file=out)
        for s, v in sorted(extra, key=lambda x: -x[1][0]):
            print(f"| {s:08X} {names.get(s, '')} | {v[0]} / {v[1]} / {v[2]} / {v[3]} / {v[4]} |", file=out)


if __name__ == "__main__":
    main()
