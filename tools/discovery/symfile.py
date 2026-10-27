"""Reader/writer for decomp-toolkit style symbols.txt lines (the format LM's recompiler reads).

    <name> = <section>:0x<addr>; // type:<type> [size:0x<n>] [scope:<s>] [...]
"""
import re

LINE = re.compile(r"^(\S+) = ([^:]+):0x([0-9A-Fa-f]+); // type:(\w+)(.*)$")


class Sym:
    __slots__ = ("name", "section", "addr", "type", "size", "attrs")

    def __init__(self, name, section, addr, type_, size=None, attrs=""):
        self.name, self.section, self.addr, self.type, self.size, self.attrs = name, section, addr, type_, size, attrs

    def line(self):
        s = f"{self.name} = {self.section}:0x{self.addr:08X}; // type:{self.type}"
        if self.size is not None:
            s += f" size:0x{self.size:X}"
        if self.attrs:
            s += " " + self.attrs
        return s

    @property
    def is_func(self):
        return self.type == "function"

    @property
    def end(self):
        return self.addr + (self.size or 0)


def parse(path):
    syms = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            m = LINE.match(ln.rstrip("\n"))
            if not m:
                continue
            name, sec, addr, typ, rest = m.groups()
            size = None
            attrs = []
            for tok in rest.split():
                if tok.startswith("size:0x"):
                    size = int(tok[7:], 16)
                else:
                    attrs.append(tok)
            syms.append(Sym(name, sec, int(addr, 16), typ, size, " ".join(attrs)))
    return syms


def functions(syms, sections=None):
    out = [s for s in syms if s.is_func and (sections is None or s.section in sections)]
    out.sort(key=lambda s: (s.section, s.addr))
    return out
