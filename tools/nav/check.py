"""Checks "Snap pointer to menu items" moves for logic (docs/CONTROLS.md "Checking the moves").

Input: CF_NAV_TRACE files (JSON lines from src/recomp/menu_nav.cpp: every distinct navigable item
set with its geometry and the move chosen from every item in 8 directions).

    py tools/nav/check.py <trace>... [--replay new|old] [--verbose] [--sets]

--replay rebuilds tools/nav/replay.cpp with the current src/recomp/menu_nav.cpp and recomputes the
moves of every logged set (new = current search, old = CF_NAV_OLD, whole = the first round's modal
rules, CF_NAV_MODAL=whole), so a change of the selection code is checked against every logged
screen without a game run.

Rules (isotropic screen units: KPAD x * 16/9; boxes clipped to the screen like the game side):
  for a move from item S in a direction (left/right/up/down):
  - candidates: past S (centre beyond S's centre, far edge beyond S's far edge), not a box that
    contains S's centre (a panel behind it), not a container (holds a smaller item's centre);
  - "in line": overlapping S's band across the direction (its row / column) by 20 % of the smaller;
  - ERROR wrong-way: the target is not past S;
  - ERROR left-line: an item is in line, the target is not;
  - ERROR skipped: an in-line item lies between S and the target;
  - ERROR axis-jump: the target is neither in line nor near the direction (gap across under the
    distance along, and centres within 40 degrees of it), and not the next row of a grid with a
    short last row (up/down, same group, gap under one item height): "left moved the cursor down";
  - ERROR skipped-row: up / down leaves the grid for another layout although the grid's own next row
    (gap under one item height) is nearer (a short last row above the toolbar);
  - ERROR container: the target is a container;
  - ERROR stuck: no move although an item is in line or near the direction within 30 degrees;
  diagonals (stick): ERROR wrong-way when the target is not in that quadrant.
  WARN unreachable: an item no cardinal move from any other item reaches (scroll arrows, tabs);
  WARN not-reversible: S -dir-> T in line but T -opposite-> is neither S nor in line with S.
"""
import argparse
import json
import os
import subprocess
import sys

ASPECT = 16.0 / 9.0
EDGE = 0.96
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIRS = {"L": (0, -1), "R": (0, 1), "U": (1, -1), "D": (1, 1)}
DIAG = {"UL": (-1, -1), "UR": (1, -1), "DL": (-1, 1), "DR": (1, 1)}
OPP = {"L": "R", "R": "L", "U": "D", "D": "U"}


class G:
    def __init__(self, it):
        x0, y0, x1, y1 = it["b"]
        x0, y0, x1, y1 = max(x0, -EDGE), max(y0, -EDGE), min(x1, EDGE), min(y1, EDGE)
        self.k = it["k"]
        self.n = it["n"]
        self.g = it["g"]
        self.hud = it["hud"]
        self.ch = it.get("ch", 0)
        self.lo = (x0 * ASPECT, y0)
        self.hi = (x1 * ASPECT, y1)
        self.container = False

    def c(self, a):
        return (self.lo[a] + self.hi[a]) * 0.5

    def size(self, a):
        return self.hi[a] - self.lo[a]

    def contains(self, x, y):
        return self.lo[0] <= x <= self.hi[0] and self.lo[1] <= y <= self.hi[1]


def mark_containers(gs):
    for g in gs:
        for o in gs:
            if o is g:
                continue
            if g.lo[0] < o.c(0) < g.hi[0] and g.lo[1] < o.c(1) < g.hi[1] and o.size(0) * o.size(1) * 2 < g.size(0) * g.size(1):
                g.container = True
                break


def relation(s, t, a, sign):
    """How t lies from s for a move along axis a: None (not a candidate) or dict."""
    b = 1 - a
    if t is s or t.container or t.contains(s.c(0), s.c(1)):
        return None
    along = (t.c(a) - s.c(a)) * sign
    sfar = s.hi[a] if sign > 0 else s.lo[a]
    tfar = t.hi[a] if sign > 0 else t.lo[a]
    if along <= 0.01 or (tfar - sfar) * sign <= 0.005:
        return None
    tnear = t.lo[a] if sign > 0 else t.hi[a]
    gap = max(0.0, (tnear - sfar) * sign)
    overlap = min(s.hi[b], t.hi[b]) - max(s.lo[b], t.lo[b])
    inline = overlap > 0.2 * min(s.size(b), t.size(b))
    off = abs(t.c(b) - s.c(b))
    cone = not inline and max(0.0, -overlap) < along and off < 0.84 * along
    strict = cone and off < 0.577 * along
    past = (tnear - sfar) * sign >= -0.25 * s.size(a)  # wholly past S: a candidate for its next row
    return {"along": along, "gap": gap, "inline": inline, "cone": cone, "strict": strict, "past": past}


def screen(gs):
    """A readable screen name from the item names (for the summary)."""
    n = {g.n for g in gs}
    has = lambda p: any(x.startswith(p) for x in n)
    if has("B_SGNkey"):
        return "keyboard (symbols page)"
    if has("B_key"):
        return "keyboard"
    if has("B_myDsgn"):
        return "designs" + (" + submenu" if has("B_msg") else "")
    if has("B_ltrWin"):
        return "letters"
    if has("B_mapBtn") or has("B_scrlBtn"):
        return "town map"
    if has("BC_0") or has("BC_1"):
        return "pockets" + (" + submenu" if has("B_msg") else "")
    if has("B_msg"):
        return "choice box"
    if has("B_cmnBtnA"):
        return "OK button screen"
    if has("B_tb"):
        return "toolbar"
    return label(gs)


def label(gs):
    names = sorted({g.n for g in gs})
    s = ", ".join(names[:6])
    return s + (", ..." if len(names) > 6 else "")


def overlap_frac(a, b):
    w = min(a.hi[0], b.hi[0]) - max(a.lo[0], b.lo[0])
    h = min(a.hi[1], b.hi[1]) - max(a.lo[1], b.lo[1])
    if w <= 0 or h <= 0:
        return 0.0
    return w * h / max(1e-9, min(a.size(0) * a.size(1), b.size(0) * b.size(1)))


def check_set(st, moves, warn, navkeys):
    errs, warns = [], []
    gs = [G(it) for it in st["items"] if it["k"] in navkeys]
    mark_containers(gs)
    by = {g.k: g for g in gs}
    # Modal: two navigable menu layouts drawn over each other (a submenu over a list) mean the one
    # behind still takes moves. Single-item layouts (map markers) may overlap each other.
    count = {}
    for g in gs:
        count[g.g] = count.get(g.g, 0) + 1
    flagged = set()
    for a in gs:
        for b in gs:
            if a.hud or b.hud or a.g >= b.g or (count[a.g] < 2 and count[b.g] < 2):
                continue
            if overlap_frac(a, b) > 0.3 and (a.g, b.g) not in flagged:
                flagged.add((a.g, b.g))
                errs.append(("-", a.n, b.n, "behind (layouts %s and %s both navigable, drawn over each other)" % (a.g, b.g)))
    # A choice box is modal: no item of another menu layout may take moves while its rows are up.
    if any(g.ch for g in gs):
        for g in gs:
            if not g.hud and not g.ch:
                errs.append(("-", g.n, "", "choice (navigable while a choice box is up)"))
    for src in gs:
        for d, (a, sign) in DIRS.items():
            to = moves.get((src.k, d))
            rel = {g.k: relation(src, g, a, sign) for g in gs}
            inl = [k for k, r in rel.items() if r and r["inline"]]
            cone = [k for k, r in rel.items() if r and r["strict"]]
            what = None
            if not to:
                if inl or cone:
                    what = "stuck (%s that way)" % by[(inl or cone)[0]].n
            elif to not in by:
                what = "target not in the set"
            else:
                t = by[to]
                r = rel[to]

                def next_row():
                    # t is in the next row of src's own grid (up / down, gap under one item height).
                    if a != 1 or t.g != src.g or r is None or not r["past"] or r["gap"] > src.size(a):
                        return False
                    same = [by[k] for k, rr in rel.items() if rr and rr["past"] and by[k].g == src.g]
                    nearest = min(same, key=lambda g: rel[g.k]["gap"])
                    ov = min(nearest.hi[a], t.hi[a]) - max(nearest.lo[a], t.lo[a])
                    return ov > 0.5 * min(nearest.size(a), t.size(a))

                if t.container:
                    what = "container"
                elif r is None:
                    what = "wrong-way"
                elif inl and not r["inline"] and not (next_row() and all(by[k].g != src.g and rel[k]["gap"] > r["gap"] for k in inl)):
                    what = "left-line (%s is in line)" % by[inl[0]].n
                elif r["inline"]:
                    tnear = t.lo[a] if sign > 0 else t.hi[a]
                    for k in inl:
                        o = by[k]
                        ofar = o.hi[a] if sign > 0 else o.lo[a]
                        b = 1 - a
                        between = (tnear - ofar) * sign >= -0.002 and min(o.hi[b], t.hi[b]) - max(o.lo[b], t.lo[b]) > 0
                        if k != to and between:
                            what = "skipped %s" % o.n
                            break
                elif not r["cone"] and not next_row():
                    what = "axis-jump"
                if not what and a == 1 and t.g != src.g and r is not None:
                    # Up / down out of a grid past its own next (short) row.
                    same = [by[k] for k, rr in rel.items() if rr and rr["past"] and by[k].g == src.g]
                    if same:
                        nearest = min(same, key=lambda g: rel[g.k]["gap"])
                        if rel[nearest.k]["gap"] <= src.size(a) and rel[nearest.k]["gap"] < r["gap"]:
                            what = "skipped-row (%s)" % nearest.n
            if what:
                errs.append((d, src.n, by[to].n if to in by else (to or "-"), what))
        for d, (sx, sy) in DIAG.items():
            to = moves.get((src.k, d))
            if to and to in by:
                t = by[to]
                if (t.c(0) - src.c(0)) * sx <= 0 or (t.c(1) - src.c(1)) * sy <= 0:
                    errs.append((d, src.n, t.n, "wrong-way"))
    if warn:
        reached = set()
        for (k, d), to in moves.items():
            if d in DIRS and to and to != k:
                reached.add(to)
        for g in gs:
            if g.k not in reached and not g.container and len(gs) > 1:
                warns.append(("-", g.n, "-", "unreachable"))
        for src in gs:
            for d, (a, sign) in DIRS.items():
                to = moves.get((src.k, d))
                if not to or to not in by:
                    continue
                r = relation(src, by[to], a, sign)
                if not r or not r["inline"]:
                    continue
                back = moves.get((to, OPP[d]))
                if back == src.k:
                    continue
                if back not in by or by[back].g != src.g or abs(by[back].c(1 - a) - src.c(1 - a)) > 0.02:
                    warns.append((d, src.n, by[to].n, "not-reversible (back to %s)" % (by[back].n if back in by else "-")))
    return gs, errs, warns


def compiler():
    import glob
    cands = [os.environ.get("CXX")]
    cands += glob.glob(r"C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\Llvm\x64\bin\clang++.exe")
    cands.append(r"C:\Program Files\LLVM\bin\clang++.exe")
    for c in cands:
        if c and os.path.exists(c):
            return c
    return "clang++"


def replay(sets, mode):
    exe = os.path.join(ROOT, "build", "nav_replay.exe" if os.name == "nt" else "nav_replay")
    src = [os.path.join(ROOT, "tools", "nav", "replay.cpp"), os.path.join(ROOT, "src", "recomp", "menu_nav.cpp")]
    if not os.path.exists(exe) or any(os.path.getmtime(s) > os.path.getmtime(exe) for s in src):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        cmd = [compiler(), "-std=c++20", "-O1", "-w", "-D_CRT_SECURE_NO_WARNINGS", "-DMENUNAV_REPLAY", "-I", os.path.join(ROOT, "src"),
               "-I", os.path.join(ROOT, "extern", "aurora", "include"), *src, "-o", exe]
        subprocess.run(cmd, check=True)
    text = []
    for i, st in enumerate(sets):
        text.append("set %d %s %d" % (i, st["mode"], len(st["items"])))
        for it in st["items"]:
            text.append("%s %s %d %d %d %.5f %.5f %.5f %.5f %d" % (it["k"], it["g"], it["hud"], it["t"], it.get("l", 0), *it["b"],
                                                                   it.get("ch", 0)))
    out = subprocess.run([exe] + ([mode] if mode in ("old", "whole") else []), input="\n".join(text) + "\n", capture_output=True,
                         text=True, check=True).stdout
    moves = [dict() for _ in sets]
    nav = [set() for _ in sets]
    for line in out.splitlines():
        p = line.split()
        if p[0] == "m":
            moves[int(p[1])][(p[2], p[3])] = None if p[4] == "-" else p[4]
        elif p[0] == "n":
            nav[int(p[1])].add(p[2])
    return moves, nav


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--replay", choices=["new", "old", "whole"])
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--warn", action="store_true", help="also unreachable / not-reversible warnings")
    ap.add_argument("--sets", action="store_true", help="print every set's items")
    ap.add_argument("--summary", action="store_true", help="only the distinct (screen, rule, direction, from -> to) errors")
    ap.add_argument("--cardinal", action="store_true", help="summary: left/right/up/down only (D-pad)")
    args = ap.parse_args()
    sets, seen = [], set()
    for path in args.traces:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                st = json.loads(line)
                sig = (st["mode"], tuple((it["k"], tuple(round(v, 2) for v in it["b"])) for it in st["items"]))
                if sig in seen:
                    continue
                seen.add(sig)
                st["src"] = os.path.basename(path)
                sets.append(st)
    if args.replay:
        allmoves, allnav = replay(sets, args.replay)
    else:
        allmoves = [{(m[0], m[1]): (m[2] or None) for m in st["moves"]} for st in sets]
        allnav = [{it["k"] for it in st["items"] if it.get("nav", 1) and (st["mode"] != "toolbar" or it["hud"])} for st in sets]
    total_e = total_w = 0
    distinct = {}
    for i, st in enumerate(sets):
        gs, errs, warns = check_set(st, allmoves[i], args.warn, allnav[i])
        total_e += len(errs)
        total_w += len(warns)
        if not args.summary and (args.sets or errs or (warns and args.verbose)):
            print("set %d (%s f%d, %s, %d items): %s" % (i, st["src"], st["f"], st["mode"], len(gs), label(gs)))
        if args.sets:
            for g in gs:
                print("    %-16s g %s %s %.3f,%.3f .. %.3f,%.3f%s" % (g.n, g.g, "hud " if g.hud else "menu", g.lo[0] / ASPECT, g.lo[1],
                                                                      g.hi[0] / ASPECT, g.hi[1], " container" if g.container else ""))
        for e in errs:
            key = (screen(gs), e[3].split(" ")[0], e[0], e[1], e[2])
            distinct[key] = distinct.get(key, 0) + 1
            if not args.summary:
                print("  ERROR %-3s %s -> %s: %s" % e)
        if not args.summary and (args.verbose or args.warn):
            for w in warns:
                print("  warn  %-3s %s -> %s: %s" % w)
    if args.summary:
        # One line per distinct (screen, rule, direction, from -> to), cardinal moves first.
        for k in sorted(distinct, key=lambda k: (k[0], k[1], len(k[2]), k[2], k[3])):
            if args.cardinal and len(k[2]) > 1:
                continue
            print("%-26s %-12s %-3s %s -> %s  (%d sets)" % (k[0], k[1], k[2], k[3], k[4], distinct[k]))
    print("%d sets, %d errors (%d distinct screen/move), %d warnings" % (len(sets), total_e, len(distinct), total_w))
    return 1 if total_e else 0


if __name__ == "__main__":
    sys.exit(main())
