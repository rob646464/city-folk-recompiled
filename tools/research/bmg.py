"""BMG (MESGbmg1) reader for City Folk message files. Usage: bmg.py file.bmg [...]"""
import struct, sys


def parse(d):
    assert d[:8] == b"MESGbmg1", d[:8]
    nsec = struct.unpack(">I", d[12:16])[0]
    enc = d[16]
    o = 0x20
    sec = {}
    for _ in range(nsec):
        m = d[o:o + 4]
        s = struct.unpack(">I", d[o + 4:o + 8])[0]
        sec[m] = d[o:o + s]
        o += s
    inf = sec[b"INF1"]
    n, esz = struct.unpack(">HH", inf[8:12])
    dat = sec[b"DAT1"][8:]
    mids = None
    if b"MID1" in sec:
        mid = sec[b"MID1"]
        mn = struct.unpack(">H", mid[8:10])[0]
        mids = [struct.unpack(">I", mid[16 + 4 * i:20 + 4 * i])[0] for i in range(mn)]
    out = []
    for i in range(n):
        e = inf[16 + i * esz:16 + (i + 1) * esz]
        off = struct.unpack(">I", e[:4])[0]
        attr = e[4:]
        out.append((mids[i] if mids and i < len(mids) else i, attr, text(dat, off, enc)))
    return enc, out


def text(dat, off, enc):
    s = []
    p = off
    if enc == 2:  # UTF-16BE
        while p + 1 < len(dat):
            c = struct.unpack(">H", dat[p:p + 2])[0]
            if c == 0:
                break
            if c == 0x1A:
                ln = dat[p + 2]
                s.append("{" + dat[p + 3:p + ln].hex() + "}")
                p += ln
                continue
            s.append(chr(c))
            p += 2
    else:
        while p < len(dat):
            c = dat[p]
            if c == 0:
                break
            if c == 0x1A:
                ln = dat[p + 1]
                s.append("{" + dat[p + 2:p + ln].hex() + "}")
                p += ln
                continue
            s.append(chr(c))
            p += 1
    return "".join(s)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    for f in sys.argv[1:]:
        enc, out = parse(open(f, "rb").read())
        print("##", f, "enc", enc, "n", len(out))
        for mid, attr, t in out:
            print("%08x|%s|%s" % (mid, attr.hex(), t.replace("\n", "\\n")))
