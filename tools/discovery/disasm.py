"""Disassemble a range of main.dol or a REL's .text (dev aid; needs `pip install capstone`).

    python tools/discovery/disasm.py <main.dol | x.rel> <addr | .text offset> [count]
"""
import sys
import struct

import capstone

from ppc import Dol, Rel


def main():
    path, addr = sys.argv[1], int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 32
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    if path.lower().endswith(".rel"):
        r = Rel(path)
        s = r.code_sections()[0]
        rd = lambda a: struct.unpack_from(">I", r.data, s.offset + a)[0]
        relocs = {}
        for mod, lst in r.relocs.items():
            for sec, off, t, ts, add in lst:
                if sec == s.index:
                    relocs[off] = (mod, t, ts, add)
    else:
        d = Dol(path)
        rd = d.word
        relocs = {}
    for a in range(addr, addr + n * 4, 4):
        w = rd(a)
        ins = list(md.disasm(struct.pack(">I", w), a))
        txt = f"{ins[0].mnemonic} {ins[0].op_str}" if ins else ".word"
        extra = f"   ; reloc mod {relocs[a][0]} type {relocs[a][1]} sec {relocs[a][2]} +{relocs[a][3]:X}" if a in relocs else ""
        print(f"{a:08X}  {w:08X}  {txt}{extra}")


if __name__ == "__main__":
    main()
