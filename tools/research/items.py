"""Dump item.bin (BITM records) from extracted item.arc. items.py <item.bin> [out.tsv]"""
import struct, sys

sys.stdout.reconfigure(encoding="utf-8")
d = open(sys.argv[1], "rb").read()
n, rs = struct.unpack(">II", d[:8])
NAMES = 10
rows = []
for i in range(n):
    r = d[0x20 + i * rs:0x20 + (i + 1) * rs]
    if r[:4] != b"BITM":
        rows.append((i, "BAD", [], r[:0x12].hex()))
        continue
    names = []
    for k in range(NAMES):
        b = r[0x12 + k * 0x22:0x12 + (k + 1) * 0x22]
        s = b.decode("utf-16-be", "replace").split("\0")[0]
        names.append(s)
    tail = r[0x12 + NAMES * 0x22:]
    rows.append((i, r[4:0x12].hex(), names, tail.hex()))
out = open(sys.argv[2], "w", encoding="utf-8") if len(sys.argv) > 2 else sys.stdout
for i, h, names, tail in rows:
    out.write("%d\t%s\t%s\t%s\n" % (i, h, "\t".join(names), tail))
