"""Uniform view of main.dol and the RELs for discovery passes.

A code location is (module id, address): the DOL is module 0 with absolute addresses,
a REL is its header module id with offsets into its (single) executable section.
"""
import struct
from pathlib import Path

import symfile
from ppc import Dol, Rel, be32


def dol_section_names(dol, syms):
    """Section index -> dtk section name, from the symbols dtk placed in each section."""
    names = {}
    for idx, addr, off, size, _ in dol.sections:
        for s in syms:
            if addr <= s.addr < addr + size and s.section not in names.values():
                names[idx] = s.section
                break
    return names


class Module:
    def __init__(self, mid, name, kind):
        self.id, self.name, self.kind = mid, name, kind
        self.syms = []
        self.code = []  # [(lo, hi)] code ranges in this module's address space

    # filled by subclasses
    def word(self, a):
        raise NotImplementedError

    def is_code(self, a):
        return any(lo <= a < hi for lo, hi in self.code)

    def code_sec_name(self, a):
        raise NotImplementedError

    def is_code_sym(self, s):
        if self.kind == "rel":
            return s.section == self.secnames[self.text.index]
        return self.is_code(s.addr)


class DolModule(Module):
    def __init__(self, path, syms):
        super().__init__(0, "main", "dol")
        self.dol = Dol(path)
        self.syms = syms
        self.secnames = dol_section_names(self.dol, syms)
        self.code = self.dol.text_ranges()
        self._secs = [(a, a + s, o, i) for i, a, o, s, _ in self.dol.sections]

    def word(self, a):
        return self.dol.word(a)

    def has(self, a):
        return self.dol.has(a)

    def sec_name(self, a):
        for lo, hi, _, i in self._secs:
            if lo <= a < hi:
                return self.secnames.get(i, f".sec{i}")
        if self.dol.bss_addr <= a < self.dol.bss_addr + self.dol.bss_size:
            return ".bss"
        return None

    def code_sec_name(self, a):
        return self.sec_name(a)


class RelModule(Module):
    def __init__(self, path, syms, splits_path=None):
        self.rel = Rel(path)
        super().__init__(self.rel.id, self.rel.name, "rel")
        self.syms = syms
        cs = self.rel.code_sections()
        assert len(cs) == 1, f"{self.name}: {len(cs)} code sections"
        self.text = cs[0]
        self.code = [(0, self.text.size)]
        # dtk names the non-empty sections in index order
        order = [s.index for s in self.rel.sections if s.size]
        names = None
        if splits_path and Path(splits_path).exists():
            names = []
            in_secs = False
            for ln in Path(splits_path).read_text(encoding="utf-8").splitlines():
                if ln.strip() == "Sections:":
                    in_secs = True
                    continue
                if in_secs:
                    if not ln.startswith("\t") or not ln.strip():
                        break
                    names.append(ln.split()[0])
        if not names or len(names) != len(order):
            defaults = {1: ".text", 2: ".ctors", 3: ".dtors", 4: ".rodata", 5: ".data", 6: ".bss"}
            names = [defaults.get(i, f".sec{i}") for i in order]
        self.secnames = dict(zip(order, names))
        self.secindex = {v: k for k, v in self.secnames.items()}
        # (section, offset) -> (module, type, target section, addend)
        self.reloc_at = {}
        for mod, lst in self.rel.relocs.items():
            for sec, off, typ, tsec, add in lst:
                self.reloc_at[(sec, off)] = (mod, typ, tsec, add)

    def word(self, a):
        return be32(self.rel.data, self.text.offset + a)

    def sec_word(self, sec, off):
        s = self.rel.sections[sec]
        return be32(self.rel.data, s.offset + off)

    def code_sec_name(self, a):
        return self.secnames[self.text.index]


def load(disc, cfg):
    """disc: extracted partition (sys/, files/). cfg: dtk output config dir."""
    disc, cfg = Path(disc), Path(cfg)
    dol = DolModule(disc / "sys" / "main.dol", symfile.parse(cfg / "symbols.txt"))
    rels = {}
    for p in sorted((disc / "files" / "rels").glob("*.rel")):
        name = p.stem
        m = RelModule(p, symfile.parse(cfg / name / "symbols.txt"), cfg / name / "splits.txt")
        rels[m.id] = m
    return dol, rels
