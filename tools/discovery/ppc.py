"""DOL / REL readers and PowerPC helpers for function discovery (docs/DISCOVERY.md).

Only reads the user's own disc files; nothing here embeds game data.
"""
import struct
from pathlib import Path

# REL relocation types (SDK OSLink / ELF PPC numbering)
R_PPC_NONE = 0
R_PPC_ADDR32 = 1
R_PPC_ADDR24 = 2
R_PPC_ADDR16 = 3
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_ADDR14 = 7
R_PPC_REL24 = 10
R_PPC_REL14 = 11
R_DOLPHIN_NOP = 201
R_DOLPHIN_SECTION = 202
R_DOLPHIN_END = 203

REL_TYPE_NAMES = {
    0: "NONE", 1: "ADDR32", 2: "ADDR24", 3: "ADDR16", 4: "ADDR16_LO", 5: "ADDR16_HI",
    6: "ADDR16_HA", 7: "ADDR14", 8: "ADDR14_BRTAKEN", 9: "ADDR14_BRNTAKEN", 10: "REL24",
    11: "REL14", 12: "REL14_BRTAKEN", 13: "REL14_BRNTAKEN", 201: "DOLPHIN_NOP",
    202: "DOLPHIN_SECTION", 203: "DOLPHIN_END",
}


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


class Dol:
    """main.dol: 7 text + 11 data sections."""

    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        self.sections = []  # (index, addr, file_off, size, is_text)
        for i in range(18):
            off, addr, size = be32(d, i * 4), be32(d, 0x48 + i * 4), be32(d, 0x90 + i * 4)
            if size:
                self.sections.append((i, addr, off, size, i < 7))
        self.bss_addr = be32(d, 0xD8)
        self.bss_size = be32(d, 0xDC)
        self.entry = be32(d, 0xE0)

    def text_ranges(self):
        return [(a, a + s) for _, a, _, s, t in self.sections if t]

    def data_ranges(self):
        return [(a, a + s, o) for _, a, o, s, t in self.sections if not t]

    def has(self, addr):
        for _, a, _, s, _ in self.sections:
            if a <= addr < a + s:
                return True
        return False

    def is_text(self, addr):
        for _, a, _, s, t in self.sections:
            if t and a <= addr < a + s:
                return True
        return False

    def word(self, addr):
        for _, a, o, s, _ in self.sections:
            if a <= addr < a + s:
                return be32(self.data, o + addr - a)
        raise KeyError(f"{addr:08X} outside main.dol")


class RelSection:
    def __init__(self, index, offset, size, exec_):
        self.index, self.offset, self.size, self.exec = index, offset, size, exec_

    @property
    def bss(self):
        return self.offset == 0 and self.size != 0


class Rel:
    """An OSLink REL module (version 1-3)."""

    def __init__(self, path):
        self.path = Path(path)
        self.name = self.path.stem
        d = self.data = self.path.read_bytes()
        (self.id, _next, _prev, nsec, secoff, self.name_off, self.name_size, self.version,
         self.bss_size, self.rel_off, self.imp_off, self.imp_size) = struct.unpack_from(">12I", d, 0)
        (self.prolog_sec, self.epilog_sec, self.unresolved_sec, self.bss_sec) = struct.unpack_from(">4B", d, 0x30)
        self.prolog, self.epilog, self.unresolved = struct.unpack_from(">3I", d, 0x34)
        self.align = self.bss_align = self.fix_size = 0
        if self.version >= 2:
            self.align, self.bss_align = struct.unpack_from(">2I", d, 0x40)
        if self.version >= 3:
            self.fix_size = be32(d, 0x48)
        self.sections = []
        for i in range(nsec):
            off, size = struct.unpack_from(">2I", d, secoff + i * 8)
            self.sections.append(RelSection(i, off & ~3, size, bool(off & 1)))
        # imports: module id -> relocation list [(section, offset, type, target_section, addend)]
        self.relocs = {}
        for i in range(self.imp_size // 8):
            mod, off = struct.unpack_from(">2I", d, self.imp_off + i * 8)
            lst = self.relocs.setdefault(mod, [])
            pos, sec = 0, None
            while True:
                o, t, s, add = struct.unpack_from(">HBBI", d, off)
                off += 8
                if t == R_DOLPHIN_END:
                    break
                if t == R_DOLPHIN_SECTION:
                    sec, pos = s, 0
                    continue
                pos += o
                if t == R_DOLPHIN_NOP:
                    continue
                lst.append((sec, pos, t, s, add))

    def code_sections(self):
        return [s for s in self.sections if s.exec and s.size and not s.bss]

    def sec_word(self, sec, off):
        s = self.sections[sec]
        return be32(self.data, s.offset + off)


def branch_target(w, pc):
    """Target of an I-form branch (b/bl/ba/bla) or None."""
    if w >> 26 != 18:
        return None
    li = sext(w & 0x03FFFFFC, 26)
    return (li if w & 2 else pc + li) & 0xFFFFFFFF


def is_bl(w):
    return w >> 26 == 18 and (w & 3) == 1


def is_b(w):
    return w >> 26 == 18 and (w & 3) == 0


def bc_target(w, pc):
    if w >> 26 != 16:
        return None
    bd = sext(w & 0xFFFC, 16)
    return (bd if w & 2 else pc + bd) & 0xFFFFFFFF


def is_blr(w):
    return w == 0x4E800020


def is_bctr(w):
    return w == 0x4E800420
