"""DSU scripts for the Dolphin reference runs (tools/dolphin/dolphin_run.ps1):
    py tools/dolphin/make_scripts.py <out dir>   -> dsu_beach.txt, dsu_city.txt
Times are wall-clock seconds from Dolphin's first DSU request. Dolphin ran below full speed on the
shared PC, so presses are long (0.12 s ones were missed) and the times generous. With the clean
post-save fixture (no Mr. Resetti) the player is in town at about 100 s; the Gecko teleport
(RUUE01.ini) moves them the moment they stand at the house door.
"""
import os
import sys


def spam(a, b, step=1.0, hold=0.35):
    out, t = [], a
    while t < b:
        out.append(f"{t:.2f} {hold} A")
        t += step
    return out


def save_quit(t):
    # HUD Save button (top right), "Would you like to quit now?" -> "Save and quit." (pointer away +
    # A). Not reached in the 2026-11-03 runs (Dolphin's IR position for this stick value is not on the
    # button); the clean fixture copy restored before each run is what keeps Resetti away.
    return [f"{t} 1.5 ptr=0.88,-0.82", f"{t + 1.6:.1f} 0.35 A", f"{t + 5} 1 ptr=0.0,-0.97"] + spam(t + 6, t + 22)


beach = ["0 1 ptr=0.0,-0.97"] + spam(20, 112) + [
    "115 0.7 stick=0,-1", "117 3.5 stick=1,0", "121 2.5 stick=1,0 B", "124 0.5 stick=0,-1", "125 5 stick=-1,0 B"] + save_quit(132)
city = ["0 1 ptr=0.0,-0.97"] + spam(20, 112) + ["120 0.3 stick=1,0"] + spam(122, 330) + [
    "335 2.5 stick=0,1", "339 5 stick=-1,0", "345 8 stick=1,0"] + save_quit(355)
out = sys.argv[1] if len(sys.argv) > 1 else "."
for name, lines in (("dsu_beach.txt", beach), ("dsu_city.txt", city)):
    with open(os.path.join(out, name), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
print("ok")
