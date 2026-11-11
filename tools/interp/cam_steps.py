# Camera step uniformity per present (branch cam-jumps): reads a CF_CAM_PRESENT=<file> log and
# reports how evenly the shown main camera moves from present to present.
#   py tools/interp/cam_steps.py <cam log> [--from <present>] [--to <present>] [--list N] [--px 16]
# Each present's camera step is turned into screen pixels of a 2160-line picture: the turn of the
# view direction (angle x focal length), the eye's move relative to the scene depth, and the zoom
# (projection scale change x half the picture). Velocity = step / shown game time since the previous
# present (game frames at 59.94 Hz), so uneven pacing is not counted as camera motion.
# - jerk: the velocity changes by more than <px> px per game frame and by more than half of the
#   larger of the two (a jump forward, a stall, a step back); "in" = both presents replay the same
#   pair of game frames (an interpolation defect), "at" = across a game frame change.
# - cut: presents shown as captured because the tape was judged a camera cut, with the step.
# - snap: presents where the camera was not blended (b as captured) while a pair existed.
import argparse
import math
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="first", type=int, default=0)
    ap.add_argument("--to", dest="last", type=int, default=1 << 60)
    ap.add_argument("--list", type=int, default=20)
    ap.add_argument("--px", type=float, default=16.0)
    ap.add_argument("--min-speed", type=float, default=2.0, help="px per game frame below which the camera counts as still")
    a = ap.parse_args()
    rows = []
    with open(a.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) < 25:
                continue
            r = dict(present=int(p[0]), us=float(p[1]), g=float(p[2]), b=int(p[3]), a=int(p[4]), alpha=float(p[5]),
                     frame=int(p[6]), retrace=int(p[7]), cut=int(p[8]), consec=int(p[9]), how=int(p[10]),
                     nvb=int(p[11]), nva=int(p[12]), mv=int(p[13]), e=tuple(map(float, p[14:17])),
                     f=tuple(map(float, p[17:20])), p0=float(p[20]), p2=float(p[21]), move=float(p[22]),
                     turn=float(p[23]), depth=float(p[24]))
            if a.first <= r["present"] <= a.last:
                rows.append(r)
    if len(rows) < 3:
        print("too few presents")
        return 1
    steps = []  # per present (from the second): px step, velocity px / game frame, kind
    for i in range(1, len(rows)):
        r0, r1 = rows[i - 1], rows[i]
        if not r0["how"] or not r1["how"] or r1["present"] != r0["present"] + 1:
            steps.append(None)
            continue
        # (From the chord, not acos of the dot product: the logged 5 decimals give acos ~5 mrad of noise.)
        ang = 2.0 * math.asin(min(1.0, 0.5 * math.dist(r0["f"], r1["f"])))
        focal = (abs(r1["p2"]) if r1["p2"] else 1.0) * 1080.0
        depth = r1["depth"] if r1["depth"] > 1 else 400.0
        de = math.dist(r0["e"], r1["e"])
        zoom = abs(r1["p2"] - r0["p2"]) / max(abs(r1["p2"]), 1e-3) * 1080.0 * 0.7 if r0["p2"] and r1["p2"] else 0.0
        px = focal * ang + focal * de / depth + zoom
        dg = (r1["g"] - r0["g"]) * 59.94
        if dg <= 1e-3:
            steps.append(dict(px=px, v=None, dg=dg, same=(r1["a"], r1["b"]) == (r0["a"], r0["b"]), i=i))
            continue
        steps.append(dict(px=px, v=px / dg, dg=dg, same=(r1["a"], r1["b"]) == (r0["a"], r0["b"]), i=i))
    jerk_in = jerk_at = 0
    events = []
    prev = None
    for s in steps:
        if s is None or s["v"] is None:
            prev = None
            continue
        if prev is not None:
            dv = abs(s["v"] - prev["v"])
            big = max(s["v"], prev["v"])
            if big >= a.min_speed and dv > a.px and dv > 0.5 * big:
                r = rows[s["i"]]
                if s["same"]:
                    jerk_in += 1
                else:
                    jerk_at += 1
                events.append((dv, r["present"], "in" if s["same"] else "at", prev["v"], s["v"], r))
        prev = s
    # Leaps: one present's step on screen (whatever the time it stood for) far beyond its neighbours'
    # (a stall the shown time jumped over: the picture moves by the whole stall at once).
    leaps = []
    for j, s in enumerate(steps):
        if s is None or rows[s["i"]]["cut"]:
            continue
        nb = [t["px"] for t in steps[max(0, j - 4): j] + steps[j + 1: j + 5] if t is not None]
        if len(nb) < 4:
            continue
        nb.sort()
        med = nb[len(nb) // 2]
        if s["px"] > max(a.px, 3.0 * med):
            leaps.append((s["px"], med, rows[s["i"]]))
    cuts = [r for r in rows if r["cut"]]
    snaps = [r for r in rows if r["how"] == 1 and r["a"] and r["consec"] and not r["cut"] and r["alpha"] < 1.0]
    moving = [s for s in steps if s and s["v"] is not None and s["v"] >= a.min_speed]
    vs = sorted(s["v"] for s in moving)
    print(f"presents {len(rows)} ({rows[0]['present']}..{rows[-1]['present']}), camera moving in {len(moving)}"
          + (f", speed median {vs[len(vs) // 2]:.1f} p95 {vs[int(len(vs) * 0.95)]:.1f} max {vs[-1]:.1f} px/game frame" if vs else ""))
    print(f"jerks (> {a.px:g} px/gf): within an interval {jerk_in}, at a game frame change {jerk_at}; "
          f"cut presents {len(cuts)}; not blended though paired {len(snaps)}")
    print(f"leaps (one present's step > max({a.px:g} px, 3 x its neighbours')): {len(leaps)}")
    for px, med, r in sorted(leaps, key=lambda e: -e[0])[: a.list]:
        print(f"  leap present {r['present']} tape {r['a']}->{r['b']} f{r['frame']} a{r['alpha']:.3f}: {px:.1f} px "
              f"(neighbours {med:.1f})")
    tapes_cut = sorted({(r["b"], r["frame"], r["move"], r["turn"], r["depth"]) for r in cuts})
    for t in tapes_cut[: a.list]:
        print(f"  cut tape {t[0]} f{t[1]}: moved {t[2]:.1f} turned {t[3]:.1f} depth {t[4]:.0f}")
    events.sort(key=lambda e: -e[0])
    for dv, pr, where, v0, v1, r in events[: a.list]:
        print(f"  jerk {where} present {pr} tape {r['a']}->{r['b']} f{r['frame']} a{r['alpha']:.3f}: {v0:.1f} -> {v1:.1f} px/gf "
              f"(views {r['nva']}->{r['nvb']} main {r['mv']}, how {r['how']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
