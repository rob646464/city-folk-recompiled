#!/usr/bin/env python3
"""List HLE (SDK) functions referenced from recompiled code: the HLE surface to implement.

    python tools/recomp/hle_refs.py <main.dol> <symbols.txt> <splits.txt>
"""
import re
import struct
import sys
from pathlib import Path

# Keep in sync with src/recompiler/recompiler.cpp (kHleTuPrefixes / kHleTuExclude).
HLE_TU_PREFIXES = ("os/", "gx/", "vi/", "pad/", "dvd/", "ar/", "card/", "ai/", "dsp/", "db/", "si/",
                   "exi/", "TRK_MINNOW_DOLPHIN/", "OdemuExi2/", "amcstubs/", "odenotstub/")
HLE_TU_EXCLUDE = ("os/OSAlloc.c", "os/OSArena.c", "pad/Padclamp.c")


def bits(w, start, end):
    return (w >> (31 - end)) & ((1 << (end - start + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Dol:
    def __init__(self, data):
        self.data = data
        offs = struct.unpack(">18I", data[0:72])
        addrs = struct.unpack(">18I", data[72:144])
        sizes = struct.unpack(">18I", data[144:216])
        self.sections = [(a, o, s) for a, o, s in zip(addrs, offs, sizes) if s]

    def word(self, addr):
        for a, o, s in self.sections:
            if a <= addr < a + s:
                return struct.unpack_from(">I", self.data, o + addr - a)[0]
        raise KeyError(hex(addr))

    def has(self, addr):
        return any(a <= addr < a + s for a, o, s in self.sections)


def load_functions(path):
    rx = re.compile(r"^(\S+) = \.(text|init):0x([0-9A-Fa-f]+); // type:function(?: size:0x([0-9A-Fa-f]+))?")
    funcs = {}
    for line in path.read_text().splitlines():
        m = rx.match(line)
        if m:
            funcs[int(m.group(3), 16)] = (m.group(1), int(m.group(4) or "0", 16))
    return funcs


def load_hle_ranges(path):
    ranges, cur = [], None
    for line in path.read_text().splitlines():
        if line and not line.startswith(("\t", " ")) and line.endswith(":") and line != "Sections:":
            cur = line[:-1]
        elif cur and cur.startswith(HLE_TU_PREFIXES) and cur not in HLE_TU_EXCLUDE:
            m = re.search(r"^\s+\.(?:text|init)\s+start:0x(\w+) end:0x(\w+)", line)
            if m:
                ranges.append((int(m.group(1), 16), int(m.group(2), 16), cur))
    return ranges


def main():
    dol = Dol(Path(sys.argv[1]).read_bytes())
    funcs = {k: v for k, v in load_functions(Path(sys.argv[2])).items() if v[1] > 0 and dol.has(k)}
    hle = {}
    for lo, hi, tu in load_hle_ranges(Path(sys.argv[3])):
        for a, (name, _) in funcs.items():
            if lo <= a < hi:
                hle[a] = (name, tu)
    refs = {}
    for a, (name, size) in funcs.items():
        if a in hle:
            continue
        his = {}
        for pc in range(a, a + size, 4):
            w = dol.word(pc)
            op = w >> 26
            t = None
            if op == 18:
                t = (pc + sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF
            elif op == 15:
                his[bits(w, 6, 10)] = (w & 0xFFFF) << 16
            elif op in (14, 24) and bits(w, 11, 15) in his:
                lo = sext(w & 0xFFFF, 16) if op == 14 else (w & 0xFFFF)
                t = (his[bits(w, 11, 15)] + lo) & 0xFFFFFFFF
            if t in hle:
                refs.setdefault(t, set()).add(name)
    for t in sorted(refs, key=lambda t: hle[t][1]):
        print(f"{t:08X} {hle[t][1]:40} {hle[t][0]:36} {len(refs[t])}")
    print(len(refs), file=sys.stderr)


main()
