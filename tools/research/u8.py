"""U8 archive lister/extractor. u8.py list <arc> | u8.py extract <arc> <outdir>"""
import os, struct, sys


def entries(d):
    assert d[:4] == b"\x55\xAA\x38\x2D", d[:4].hex()
    root = struct.unpack(">I", d[4:8])[0]
    n = struct.unpack(">I", d[root + 8:root + 12])[0]
    strtab = root + n * 12
    nodes = [struct.unpack(">BBHII", d[root + i * 12:root + i * 12 + 12]) for i in range(n)]

    def name(off):
        e = d.index(b"\0", strtab + off)
        return d[strtab + off:e].decode("shift_jis", "replace")

    out = []
    stack = [(n, "")]
    for i in range(1, n):
        while stack and i >= stack[-1][0]:
            stack.pop()
        t, hi, lo, a, b = nodes[i]
        nm = name((hi << 16) | lo)
        path = (stack[-1][1] + "/" + nm) if stack else nm
        if t == 1:
            stack.append((b, path))
        else:
            out.append((path.lstrip("/"), a, b))
    return out


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    d = open(sys.argv[2], "rb").read()
    if d[:4] == b"Yaz0":
        raise SystemExit("Yaz0")
    es = entries(d)
    if sys.argv[1] == "list":
        for p, a, b in es:
            print("%8d %s" % (b, p))
    else:
        for p, a, b in es:
            q = os.path.join(sys.argv[3], p)
            os.makedirs(os.path.dirname(q), exist_ok=True)
            open(q, "wb").write(d[a:a + b])
