#!/usr/bin/env python3
"""Map guest addresses to the containing function: addr2sym.py 8000E808 8019CCC0 ..."""
import bisect, re, sys
from pathlib import Path
rows = []
for l in (Path(__file__).parents[2] / "decomp/config/GLME01/symbols.txt").read_text().splitlines():
    m = re.match(r'(\S+) = \.(?:text|init):0x([0-9A-F]+); // type:function size:0x(\w+)', l)
    if m:
        rows.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
rows.sort()
keys = [r[0] for r in rows]
for a in sys.argv[1:]:
    t = int(a, 16)
    i = bisect.bisect_right(keys, t) - 1
    s, size, n = rows[i]
    print(f"{t:08X} {n}+0x{t - s:X}" + ("" if t < s + size else " (past end)"))
