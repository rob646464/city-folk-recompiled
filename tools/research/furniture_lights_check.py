"""Compares the runtime's furniture lights (CF_FURN_LOG=1 `[furn] model ...` lines of a run log)
with the review scanner's proposals (out/furniture_lights/candidates.json).

    py tools/research/furniture_lights_check.py <log.txt> [<candidates.json>]
"""
import json
import re
import sys
from pathlib import Path

LINE = re.compile(r"\[furn\] model \S+ '([^']+)' item ([0-9A-F]{4}) '([^']*)': (\w+) at (\S+) (\S+) (\S+) colour (\S+) (\S+) (\S+) r (\d+) I (\S+)")


def main():
    log = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    cand = Path(sys.argv[2] if len(sys.argv) > 2 else r"C:/Users/rmelv/Desktop/Animal Crossing City Folk Recompiled/out/furniture_lights/candidates.json")
    by_item = {c["item"]: c for c in json.loads(cand.read_text(encoding="utf-8"))}
    seen = set()
    for m in LINE.finditer(log):
        model, item, name, kind = m.group(1), int(m.group(2), 16), m.group(3), m.group(4)
        if (model, item) in seen:
            continue
        seen.add((model, item))
        pos = [float(m.group(k)) for k in (5, 6, 7)]
        col = [float(m.group(k)) for k in (8, 9, 10)]
        r, inten = int(m.group(11)), float(m.group(12))
        c = by_item.get(item)
        if not c:
            print("%-28s %-20s runtime only (exception?) %s r %d I %.2f" % (model, name, kind, r, inten))
            continue
        dpos = max(abs(a - b) for a, b in zip(pos, c["model_pos"]))
        dcol = max(abs(a - b) for a, b in zip(col, c["color"]))
        ok = c["kind"] == kind and dpos < 0.5 and dcol < 0.03 and abs(r - c["radius"]) <= 1 and abs(inten - c["intensity"]) < 0.02
        print("%-28s %-20s %s kind %s/%s pos diff %.2f colour diff %.3f r %d/%d I %.2f/%.2f" % (
            model, name, "same" if ok else "DIFFERS", kind, c["kind"], dpos, dcol, r, c["radius"], inten, c["intensity"]))


if __name__ == "__main__":
    main()
