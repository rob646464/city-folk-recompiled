#!/usr/bin/env python3
"""Show our functions around an address next to a reference map around a name (dev aid
for names_manual.txt).

    python tools/discovery/compare.py --refs <dir> --ref smg_RMGE01 --name IOS_Open --addr 80222530 [--n 8]
"""
import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import sdk_names  # noqa: E402
import symfile  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refs", required=True)
    ap.add_argument("--ref", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--addr", required=True)
    ap.add_argument("--id", default="RUUE01")
    ap.add_argument("--n", type=int, default=8)
    a = ap.parse_args()
    ref = sdk_names.Ref(a.ref, Path(a.refs) / a.ref)
    j = ref.index.get(a.name)
    if j is None:
        j = next(i for i, f in enumerate(ref.funcs) if f[1] == a.name)
    syms = symfile.parse(ROOT / "config" / a.id / "symbols.txt")
    ours = sorted((s.addr, s.size or 0, s.name) for s in syms if s.is_func)
    addr = int(a.addr, 16)
    i = next(k for k, o in enumerate(ours) if o[0] >= addr)
    for d in range(-a.n, a.n + 1):
        o = ours[i + d] if 0 <= i + d < len(ours) else None
        r = ref.funcs[j + d] if 0 <= j + d < len(ref.funcs) else None
        left = f"{o[0]:08X} {o[1]:6X} {o[2][:44]:44}" if o else " " * 60
        right = f"{r[0]:6X} {r[1][:50]}" if r else ""
        print(f"{left} | {right}")


if __name__ == "__main__":
    main()
