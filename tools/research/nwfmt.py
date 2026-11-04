"""Readers for the Nintendo Wii file formats City Folk's furniture uses (U8 archives, NW4R BRRES:
MDL0 geometry / materials / shaders, TEX0 + PLT0 textures, animation folder names), written from
the public format descriptions (no code of other projects).

Used by tools/research/furniture_lights.py. Reads the user's own disc files; nothing extracted is
ever written into the repository.
"""
import struct

import numpy as np


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def bes16(b, o):
    return struct.unpack_from(">h", b, o)[0]


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def bes32(b, o):
    return struct.unpack_from(">i", b, o)[0]


def bef32(b, o):
    return struct.unpack_from(">f", b, o)[0]


def cstr(b, o, maxlen=256):
    e = b.find(b"\0", o, o + maxlen)
    if e < 0:
        e = o + maxlen
    return b[o:e].decode("latin-1")


# ---- U8 archives ---------------------------------------------------------------------------
def u8_files(data):
    """{path: bytes} of a U8 archive (magic 55 AA 38 2D)."""
    if be32(data, 0) != 0x55AA382D:
        raise ValueError("not a U8 archive")
    root = be32(data, 4)
    n = be32(data, root + 8)
    strtab = root + n * 12
    out = {}
    stack = []  # (end index, name)
    for i in range(1, n):
        while stack and i >= stack[-1][0]:
            stack.pop()
        path = "/".join(p for _, p in stack)
        o = root + i * 12
        name = cstr(data, strtab + (be32(data, o) & 0xFFFFFF))
        a, b = be32(data, o + 4), be32(data, o + 8)
        if data[o] == 1:
            stack.append((b, name))
        else:
            out[(path + "/" if path else "") + name] = data[a:a + b]
    return out


# ---- BRRES -----------------------------------------------------------------------------------
def index_group(b, g):
    """[(name, absolute data offset)] of an NW4R index group at g."""
    n = be32(b, g + 4)
    out = []
    for k in range(1, n + 1):
        e = g + 8 + k * 16
        name_off, data_off = be32(b, e + 8), be32(b, e + 12)
        out.append((cstr(b, g + name_off) if name_off else "", g + data_off if data_off else 0))
    return out


def brres(data):
    """{folder name: [(entry name, absolute offset)]}"""
    if data[:4] != b"bres":
        raise ValueError("not a BRRES")
    root = be16(data, 0xC)
    if data[root:root + 4] != b"root":
        raise ValueError("no root")
    return {folder: index_group(data, off) for folder, off in index_group(data, root + 8)}


# ---- GX display-list helpers -------------------------------------------------------------------
def bp_writes(b, start, end):
    """[(reg, value)] of the BP writes (0x61 rr vvvvvv) in a padded display list."""
    out = []
    o = start
    while o + 5 <= end:
        op = b[o]
        if op == 0x61:
            out.append((b[o + 1], (b[o + 2] << 16) | (b[o + 3] << 8) | b[o + 4]))
            o += 5
        elif op == 0x10:  # XF load: header (count-1)<<16 | addr, then words
            n = (be32(b, o + 1) >> 16) + 1
            o += 5 + 4 * n
        elif op == 0x08:
            o += 6
        else:
            o += 1
    return out


def xf_writes(b, start, end):
    out = {}
    o = start
    while o + 5 <= end:
        op = b[o]
        if op == 0x10:
            hdr = be32(b, o + 1)
            n, addr = (hdr >> 16) + 1, hdr & 0xFFFF
            for k in range(n):
                out[addr + k] = be32(b, o + 5 + 4 * k)
            o += 5 + 4 * n
        elif op == 0x61:
            o += 5
        elif op == 0x08:
            o += 6
        else:
            o += 1
    return out


# ---- textures ------------------------------------------------------------------------------------
FMT_BLOCK = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16), 6: (4, 4, 32),
             8: (8, 8, 4), 9: (8, 4, 8), 10: (4, 4, 16), 14: (8, 8, 4)}


def _rgb565(v):
    v = np.asarray(v, dtype=np.uint32)
    r = ((v >> 11) & 31) * 255 // 31
    g = ((v >> 5) & 63) * 255 // 63
    bb = (v & 31) * 255 // 31
    return np.stack([r, g, bb, np.full_like(r, 255)], -1).astype(np.uint8)


def _rgb5a3(v):
    v = np.asarray(v, dtype=np.uint32)
    opaque = (v & 0x8000) != 0
    r = np.where(opaque, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
    g = np.where(opaque, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
    bb = np.where(opaque, (v & 31) * 255 // 31, (v & 15) * 17)
    a = np.where(opaque, 255, ((v >> 12) & 7) * 255 // 7)
    return np.stack([r, g, bb, a], -1).astype(np.uint8)


def _ia8(v):
    v = np.asarray(v, dtype=np.uint32)
    i, a = v & 0xFF, v >> 8
    return np.stack([i, i, i, a], -1).astype(np.uint8)


def decode_palette(data, fmt, count):
    v = np.frombuffer(data[:count * 2], dtype=">u2")
    return [_ia8, _rgb565, _rgb5a3][fmt](v)


def decode_texture(data, fmt, w, h, palette=None):
    """RGBA uint8 (h, w, 4) of a GX texture (formats 0-6, 8-10 with palette, 14 CMPR)."""
    bw, bh, bpp = FMT_BLOCK[fmt]
    bx, by = (w + bw - 1) // bw, (h + bh - 1) // bh
    W, H = bx * bw, by * bh
    out = np.zeros((H, W, 4), np.uint8)
    if fmt == 14:  # CMPR: 8x8 blocks of four 4x4 DXT1 sub-blocks
        o = 0
        for yb in range(by):
            for xb in range(bx):
                for sub in range(4):
                    c0, c1 = be16(data, o), be16(data, o + 2)
                    bits = be32(data, o + 4)
                    o += 8
                    p = _rgb565([c0, c1]).astype(np.int32)
                    if c0 > c1:
                        p2 = (2 * p[0] + p[1]) // 3
                        p3 = (p[0] + 2 * p[1]) // 3
                        p2[3] = p3[3] = 255
                    else:
                        p2 = (p[0] + p[1]) // 2
                        p2[3] = 255
                        p3 = np.array([0, 0, 0, 0])
                    pal = np.stack([p[0], p[1], p2, p3]).astype(np.uint8)
                    idx = np.array([(bits >> (30 - 2 * k)) & 3 for k in range(16)]).reshape(4, 4)
                    y0, x0 = yb * 8 + (sub // 2) * 4, xb * 8 + (sub % 2) * 4
                    out[y0:y0 + 4, x0:x0 + 4] = pal[idx]
        return out[:h, :w]
    nbytes = bx * by * bw * bh * bpp // 8
    raw = np.frombuffer(data[:nbytes], np.uint8)
    if fmt == 6:  # RGBA8: AR block then GB block per 4x4 tile
        t = raw.reshape(by, bx, 2, 16, 2)
        a, r = t[:, :, 0, :, 0], t[:, :, 0, :, 1]
        g, b_ = t[:, :, 1, :, 0], t[:, :, 1, :, 1]
        px = np.stack([r, g, b_, a], -1).reshape(by, bx, 4, 4, 4)
    else:
        if bpp == 4:
            vals = np.stack([raw >> 4, raw & 15], -1).reshape(-1)
        elif bpp == 8:
            vals = raw
        else:
            vals = raw.view(">u2")
        vals = vals.reshape(by, bx, bh, bw)
        if fmt == 0:
            i = (vals * 17).astype(np.uint8)
            px = np.stack([i, i, i, i], -1)
        elif fmt == 1:
            i = vals.astype(np.uint8)
            px = np.stack([i, i, i, i], -1)
        elif fmt == 2:
            i, a = ((vals & 15) * 17).astype(np.uint8), ((vals >> 4) * 17).astype(np.uint8)
            px = np.stack([i, i, i, a], -1)
        elif fmt == 3:
            px = _ia8(vals)
        elif fmt == 4:
            px = _rgb565(vals)
        elif fmt == 5:
            px = _rgb5a3(vals)
        else:  # C4 / C8 / C14X2
            pal = palette if palette is not None else np.zeros((1 << 14, 4), np.uint8)
            px = pal[np.clip(vals.astype(np.int64) & 0x3FFF, 0, len(pal) - 1)]
    px = px.reshape(by, bx, bh, bw, 4).transpose(0, 2, 1, 3, 4).reshape(H, W, 4)
    return px[:h, :w]


def tex0(b, o):
    """(width, height, format, image bytes) of a TEX0 at o (first mip only)."""
    data_off = bes32(b, o + 0x10)
    w, h, fmt = be16(b, o + 0x1C), be16(b, o + 0x1E), be32(b, o + 0x20)
    return w, h, fmt, b[o + data_off:]


def plt0(b, o):
    data_off = bes32(b, o + 0x10)
    fmt, count = be32(b, o + 0x18), be16(b, o + 0x1C)
    return fmt, count, b[o + data_off:o + data_off + count * 2]


# ---- MDL0 ----------------------------------------------------------------------------------------
class Material:
    pass


class Mdl0:
    """Geometry, nodes, materials and TEV of one MDL0 (version 9-11)."""

    def __init__(self, b, o):
        self.b, self.o = b, o
        ver = be32(b, o + 8)
        ns = 14 if ver >= 10 else 11
        names = (["defs", "bones", "verts", "norms", "colors", "uvs", "furv", "furp", "mats", "shaders", "objs", "texlinks",
                  "pltlinks", "user"] if ns == 14 else
                 ["defs", "bones", "verts", "norms", "colors", "uvs", "mats", "shaders", "objs", "texlinks", "pltlinks"])
        self.sec = {}
        for k, n in enumerate(names):
            so = bes32(b, o + 0x10 + 4 * k)
            self.sec[n] = index_group(b, o + so) if so else []
        self.name = cstr(b, o + bes32(b, o + 0x10 + 4 * ns))
        info = o + 0x10 + 4 * ns + 4
        mtx_tab = info + bes32(b, info + 0x24)
        n_ids = be32(b, mtx_tab)
        self.mtx_node = [bes32(b, mtx_tab + 4 + 4 * k) for k in range(n_ids)]
        self.bbox = ([bef32(b, info + 0x28 + 4 * k) for k in range(3)], [bef32(b, info + 0x34 + 4 * k) for k in range(3)])
        self._nodes()
        self._arrays()
        self._materials()
        self._draw_lists()

    # nodes: frame (bind) matrices
    def _nodes(self):
        b = self.b
        self.nodes = []
        for name, no in self.sec["bones"]:
            m = np.array([bef32(b, no + 0x70 + 4 * k) for k in range(12)], np.float64).reshape(3, 4)
            self.nodes.append((name, m))

    def node_mtx(self, node):
        if 0 <= node < len(self.nodes):
            return self.nodes[node][1]
        return np.hstack([np.eye(3), np.zeros((3, 1))])

    def _arrays(self):
        b = self.b
        self.arr = {}
        for kind in ("verts", "norms", "colors", "uvs"):
            lst = []
            for name, ao in self.sec[kind]:
                data_off, comp, fmt = bes32(b, ao + 8), be32(b, ao + 0x14), be32(b, ao + 0x18)
                if kind == "colors":
                    stride, count = b[ao + 0x1C], be16(b, ao + 0x1E)
                    vals = []
                    for i in range(count):
                        p = ao + data_off + i * stride
                        if fmt == 0:
                            vals.append(_rgb565([be16(b, p)])[0])
                        elif fmt in (1, 2):
                            vals.append(np.array([b[p], b[p + 1], b[p + 2], 255], np.uint8))
                        elif fmt == 3:
                            v = be16(b, p)
                            vals.append(np.array([(v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17], np.uint8))
                        elif fmt == 4:
                            v = (b[p] << 16) | (b[p + 1] << 8) | b[p + 2]
                            vals.append(np.array([((v >> 18) & 63) * 255 // 63, ((v >> 12) & 63) * 255 // 63, ((v >> 6) & 63) * 255 // 63,
                                                  (v & 63) * 255 // 63], np.uint8))
                        else:
                            vals.append(np.array([b[p], b[p + 1], b[p + 2], b[p + 3]], np.uint8))
                    lst.append(np.array(vals, np.float64).reshape(-1, 4) / 255.0)
                    continue
                frac, stride, count = b[ao + 0x1C], b[ao + 0x1D], be16(b, ao + 0x1E)
                if kind == "norms":
                    ncomp = 3
                    frac = {1: 6, 3: 14}.get(fmt, 0)
                elif kind == "verts":
                    ncomp = 3 if comp == 1 else 2
                else:
                    ncomp = 2 if comp == 1 else 1
                size = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4}[fmt]
                code = {0: "u1", 1: "i1", 2: ">u2", 3: ">i2", 4: ">f4"}[fmt]
                if stride == 0:
                    stride = size * ncomp
                raw = np.frombuffer(b, dtype=np.uint8, count=count * stride, offset=ao + data_off).reshape(count, stride)
                v = raw[:, :size * ncomp].copy().view(code).reshape(count, ncomp).astype(np.float64)
                if fmt != 4:
                    v /= float(1 << frac)
                lst.append(v)
            self.arr[kind] = lst

    def _materials(self):
        b, o = self.b, self.o
        tex_names = {}
        self.materials = []
        shaders = {so: name for name, so in self.sec["shaders"]}
        for name, mo in self.sec["mats"]:
            m = Material()
            m.name, m.offset = name, mo
            m.xlu = bool(be32(b, mo + 0x10) & 0x80000000)
            m.n_texgen, m.n_chan, m.n_stage = b[mo + 0x14], b[mo + 0x15], b[mo + 0x16]
            m.cull = be32(b, mo + 0x18)
            sh = mo + bes32(b, mo + 0x28)
            ntex, ref = be32(b, mo + 0x2C), mo + bes32(b, mo + 0x30)
            m.textures = []
            for k in range(ntex):
                r = ref + k * 0x34
                tn = cstr(b, r + bes32(b, r)) if bes32(b, r) else ""
                pn = cstr(b, r + bes32(b, r + 4)) if bes32(b, r + 4) else ""
                m.textures.append((tn, pn))
            dl = mo + bes32(b, mo + 0x3C)
            # light channels (MDL0 v10/11 material: after the texture SRT / effect blocks, at +0x3F0,
            # 0x14 bytes each: valid flags, material colour, ambient colour, colour / alpha control
            # in the XF channel-control bit layout)
            m.chan = []
            base = mo + (0x3F0 if be32(b, o + 8) >= 10 else 0x3E8)
            for k in range(min(m.n_chan, 2)):
                c = base + k * 0x14
                m.chan.append(dict(flags=be32(b, c), mat=bytes(b[c + 4:c + 8]), amb=bytes(b[c + 8:c + 12]), color=be32(b, c + 12),
                                   alpha=be32(b, c + 16)))
            m.bp = dict(bp_writes(b, dl, dl + 0x20 + 0x80))
            # TEV colour registers (BP 0xE0..0xE7: ra / bg pairs; bit 23 = konst)
            m.regs, m.konst = {}, {}
            for reg, val in bp_writes(b, dl + 0x20, dl + 0xA0):
                if 0xE0 <= reg <= 0xE7:
                    k = (reg - 0xE0) // 2
                    is_k = (val >> 23) & 1
                    tgt = m.konst if is_k else m.regs
                    if (reg - 0xE0) % 2 == 0:
                        tgt.setdefault(k, [0, 0, 0, 0])
                        tgt[k][0] = _s11(val & 0x7FF)
                        tgt[k][3] = _s11((val >> 12) & 0x7FF)
                    else:
                        tgt.setdefault(k, [0, 0, 0, 0])
                        tgt[k][2] = _s11(val & 0x7FF)
                        tgt[k][1] = _s11((val >> 12) & 0x7FF)
            m.blend = m.bp.get(0x41, 0)
            m.shader_name = shaders.get(sh, "")
            stages = []
            sbp = bp_writes(b, sh + 0x20, sh + be32(b, sh))
            col = {r: v for r, v in sbp if 0xC0 <= r <= 0xDF}
            order = {r: v for r, v in sbp if 0x28 <= r <= 0x2F}
            ksel = {r: v for r, v in sbp if 0xF6 <= r <= 0xFD}
            for s in range(m.n_stage):
                cv, av = col.get(0xC0 + 2 * s, 0), col.get(0xC1 + 2 * s, 0)
                ov = order.get(0x28 + s // 2, 0) >> (12 * (s & 1))
                kv = ksel.get(0xF6 + s // 2, 0) >> (10 * (s & 1))
                stages.append(dict(c=[(cv >> 12) & 15, (cv >> 8) & 15, (cv >> 4) & 15, cv & 15], cbias=(cv >> 16) & 3,
                                   csub=(cv >> 18) & 1, cshift=(cv >> 20) & 3, cdest=(cv >> 22) & 3,
                                   a=[(av >> 13) & 7, (av >> 10) & 7, (av >> 7) & 7, (av >> 4) & 7],
                                   texmap=ov & 7, texcoord=(ov >> 3) & 7, tex_on=(ov >> 6) & 1, chan=(ov >> 7) & 7,
                                   kcsel=(kv >> 4) & 31, kasel=(kv >> 9) & 31))
            m.stages = stages
            self.materials.append(m)

    def _draw_lists(self):
        """material index -> [(object index, node)] from the DrawOpa / DrawXlu byte code."""
        b = self.b
        self.draws = []
        for name, do in self.sec["defs"]:
            if not name.startswith("Draw"):
                continue
            o = do
            while True:
                op = b[o]
                if op == 1 or op == 0:
                    break
                if op == 4:  # DRAW mat u16, obj u16, node u16, prio u8
                    mat, obj, node = be16(b, o + 1), be16(b, o + 3), be16(b, o + 5)
                    self.draws.append((mat, obj, node, name == "DrawXlu"))
                    o += 8
                else:
                    break
        self.objects = [(name, oo) for name, oo in self.sec["objs"]]

    def triangles(self, obj_index, node):
        """(positions (n,3,3) model space, uv0 (n,3,2) or None, colors (n,3,4) or None) of one object."""
        b = self.b
        name, oo = self.objects[obj_index]
        lo, hi = be32(b, oo + 0x0C), be32(b, oo + 0x10)
        single = bes32(b, oo + 0x08)
        prim_off = oo + 0x24 + bes32(b, oo + 0x2C)
        prim_size = bes32(b, oo + 0x28)
        ids = [bes16(b, oo + 0x48 + 2 * k) for k in range(12)]  # pos, nrm, clr0, clr1, uv0..7
        mlist_off = oo + bes32(b, oo + 0x64)
        mlist = [bes16(b, mlist_off + 4 + 2 * k) for k in range(be32(b, mlist_off))] if bes32(b, oo + 0x64) else []
        attrs = []  # (kind, size) in vertex order
        if lo & 1:
            attrs.append(("pnmtx", 1))
        for k in range(8):
            if (lo >> (1 + k)) & 1:
                attrs.append(("tmtx", 1))
        for k, kind in enumerate(("pos", "nrm", "clr0", "clr1")):
            t = (lo >> (9 + 2 * k)) & 3
            if t:
                attrs.append((kind, t))
        for k in range(8):
            t = (hi >> (2 * k)) & 3
            if t:
                attrs.append(("uv%d" % k, t))
        pos = self.arr["verts"][ids[0]] if ids[0] >= 0 else None
        uv = self.arr["uvs"][ids[4]] if ids[4] >= 0 and ids[4] < len(self.arr["uvs"]) else None
        clr = self.arr["colors"][ids[2]] if ids[2] >= 0 and ids[2] < len(self.arr["colors"]) else None
        slot_mtx = {}
        tris_p, tris_t, tris_c = [], [], []
        o, end = prim_off, prim_off + prim_size
        while o < end:
            op = b[o]
            if op == 0:
                o += 1
                continue
            if op in (0x20, 0x28, 0x30, 0x38):  # indexed XF load (matrices)
                idx, addr = be16(b, o + 1), be16(b, o + 3) & 0xFFF
                if op == 0x20:
                    slot_mtx[addr // 12] = idx
                o += 5
                continue
            if op & 0xF8 in (0x80, 0x90, 0x98, 0xA0):
                cnt = be16(b, o + 1)
                o += 3
                verts = []
                for _ in range(cnt):
                    v = {}
                    for kind, t in attrs:
                        if kind in ("pnmtx", "tmtx"):
                            if kind == "pnmtx":
                                v["pnmtx"] = b[o]
                            o += 1
                        elif t == 1:  # direct: only expected for nothing here; skip safely
                            o += 0
                        elif t == 2:
                            v[kind] = b[o]
                            o += 1
                        else:
                            v[kind] = be16(b, o)
                            o += 2
                    verts.append(v)
                prim = op & 0xF8
                if prim == 0x90:
                    tl = [verts[i:i + 3] for i in range(0, cnt - 2, 3)]
                elif prim == 0x98:
                    tl = [[verts[i], verts[i + 1], verts[i + 2]] if i % 2 == 0 else [verts[i + 1], verts[i], verts[i + 2]] for i in range(cnt - 2)]
                elif prim == 0xA0:
                    tl = [[verts[0], verts[i], verts[i + 1]] for i in range(1, cnt - 1)]
                else:
                    tl = [[q[0], q[1], q[2]] for q in (verts[i:i + 4] for i in range(0, cnt - 3, 4))] + \
                         [[q[0], q[2], q[3]] for q in (verts[i:i + 4] for i in range(0, cnt - 3, 4))]
                for t in tl:
                    ps, ts, cs = [], [], []
                    for v in t:
                        p = pos[v.get("pos", 0)] if pos is not None else np.zeros(3)
                        if single >= 0:
                            m = self.node_mtx(self.mtx_node[single] if single < len(self.mtx_node) else single)
                        else:
                            slot = v.get("pnmtx", 0) // 3
                            mid = slot_mtx.get(slot, mlist[slot] if slot < len(mlist) else 0)
                            nd = self.mtx_node[mid] if 0 <= mid < len(self.mtx_node) else -1
                            m = self.node_mtx(nd)
                        p3 = np.append(p[:3] if len(p) >= 3 else np.append(p, 0), 1.0)
                        ps.append(m @ p3)
                        ts.append(uv[v["uv0"]] if uv is not None and "uv0" in v else np.zeros(2))
                        cs.append(clr[v["clr0"]] if clr is not None and "clr0" in v else np.ones(4))
                    tris_p.append(ps)
                    tris_t.append(ts)
                    tris_c.append(cs)
                continue
            if op == 0x08:
                o += 6
                continue
            if op == 0x10:
                o += 5 + 4 * ((be32(b, o + 1) >> 16) + 1)
                continue
            if op == 0x61:
                o += 5
                continue
            break
        P = np.array(tris_p, np.float64).reshape(-1, 3, 3)
        T = np.array(tris_t, np.float64).reshape(-1, 3, 2)
        C = np.array(tris_c, np.float64).reshape(-1, 3, 4)
        return P, T, C


def _s11(v):
    return v - 0x800 if v & 0x400 else v
