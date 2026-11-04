"""Furniture light candidates for Enhanced lighting (docs/RENDERING.md "Furniture lights").

    py tools/research/furniture_lights.py [--disc "<US DATA/DATA>"] [--out <dir>] [--only n,n,...]

Reads the user's own disc (Item/item.arc item.bin records + Item/Ftr/Ftr<n>.brres models) and
writes a review contact sheet (index.html + PNG renders, on and off) into <dir>
(default out/furniture_lights of the main checkout; gitignored). Nothing extracted goes into the
repository; the runtime (src/game/cf_furniture_light.cpp) derives the same parameters from the
loaded model, and only our own exception numbers are committed.

Candidates:
 (a) furniture the game itself switches: the item record's light kind (record + 0x187 >> 6 & 3,
     read by daFTR_c, d_a_ftrNP fn_15_6B8) and the glow materials d_a_ftrNP's helpers drive by
     their exact names (lp_m: lamps; mn_m / tv_m / wa_m / wb_m / fr_m: screens, water, fire; the
     helper sets the material's TEV konst colour 3 alpha 0..1 = off..on).
 (b) a scan of every material: lighting ignored (colour channel unlit or no TEV stage reads the
     rasterised colour), the lamp-style konst bypass, additive blending, or a light-like name in
     the material / texture / animation names.
"""
import argparse
import html
import math
import os
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nwfmt  # noqa: E402

GAME_GLOW = {"lp_m": "lamp", "mn_m": "screen", "tv_m": "screen", "wa_m": "water", "wb_m": "water", "fr_m": "fire"}


def item_records(arc):
    b = nwfmt.u8_files(arc)["item/item.bin"]
    n, rs = nwfmt.be32(b, 0), nwfmt.be32(b, 4)
    out = {}
    for i in range(n):
        r = b[0x20 + i * rs:0x20 + (i + 1) * rs]
        if r[:4] != b"BITM" or r[0x166] != 3:
            continue
        num = nwfmt.be16(r, 8)
        name = r[0x12 + 0x22:0x12 + 2 * 0x22].decode("utf-16-be", "replace").split("\0")[0].strip("\u3000 ")
        ordinal = num - 0x9C4 if num < 0xE07 else num - 0xE07 + 718
        out[ordinal] = dict(number=num, code=0x9000 + num * 4, name=name, action=r[0x171], light_kind=(r[0x187] >> 6) & 3)
    return out


# ---- material analysis ------------------------------------------------------------------------
# Name hints (material, texture, model, animation and item names). Screens are not guessed from
# names ("screen" is also a folding screen): the game's own tv_m / mn_m names and unlit animated
# screen materials (tp_m) find them.
HINTS = [("fireplug", None), ("candle", "fire"), ("fire", "fire"), ("flame", "fire"), ("torch", "fire"), ("brazier", "fire"),
         ("hibachi", "fire"), ("stove", "fire"), ("lavalamp", "lava"), ("lava lamp", "lava"), ("lamp", "lamp"), ("light", "lamp"),
         ("lantern", "lamp"), ("neon", "neon"), ("glow", "lamp"), ("aquarium", "water")]
WATER_WORDS = ("water", "pool", "bath", "rotenburo", "trough", "sink", "toilet", "urinal", "wc0", "wc1", "sprinkler", "merlion",
               "syon", "fountain", "sys_k")


def name_hint(text):
    text = text.lower()
    for key, kind in HINTS:
        if key in text:
            return kind
    return None


def is_water(text):
    text = text.lower()
    return any(w in text for w in WATER_WORDS)


def material_flags(m):
    f = {}
    if m.name in GAME_GLOW:
        f["game"] = GAME_GLOW[m.name]
    lit = bool(m.chan) and (m.chan[0]["color"] & 2) != 0
    reads_ras = any(x in (10, 11) for s in m.stages for x in s["c"])
    if m.n_stage and (not lit or not reads_ras):
        f["unlit"] = True
    # lamp-style bypass: a stage lerps the raster colour toward another input by a konst / register
    for s in m.stages:
        a, b_, c, d = s["c"]
        if a == 10 and b_ in (11, 12, 2, 4, 6, 14) and c in (14, 3, 5, 7, 1):
            f["bypass"] = True
    blend = m.blend
    if blend & 1 and ((blend >> 5) & 7) == 1:
        f["additive"] = True
    h = name_hint(" ".join([m.name] + [t for t, _ in m.textures]))
    if h:
        f["hint"] = h
    return f


# ---- rendering --------------------------------------------------------------------------------
class Tex:
    """Textures of a model's brres, with the furniture's shared textures (Ftr/roomAlways.arc
    int_common.brres: f1/f2 fire, tv.0 screen, gl glass, wt/w2 water, ...) as a fallback."""
    common = None

    def __init__(self, d, folders):
        self.src = [(d, dict(folders.get("Textures(NW4R)", [])), dict(folders.get("Palettes(NW4R)", [])))]
        if Tex.common:
            self.src.append(Tex.common)
        self.cache = {}

    def get(self, name, pal):
        key = (name, pal)
        if key in self.cache:
            return self.cache[key]
        img = None
        for d, texs, plts in self.src:
            if name not in texs:
                continue
            w, h, fmt, data = nwfmt.tex0(d, texs[name])
            palette = None
            if fmt in (8, 9, 10):
                pn = pal if pal in plts else (name if name in plts else None)
                if pn is None:
                    for dd, _, pp in self.src:
                        if pal in pp:
                            d2, pn2 = dd, pal
                            pf, pc, pd = nwfmt.plt0(d2, pp[pn2])
                            palette = nwfmt.decode_palette(pd, pf, pc)
                            break
                    if palette is None and plts:
                        pn = next(iter(plts))
                if pn is not None and palette is None:
                    pf, pc, pd = nwfmt.plt0(d, plts[pn])
                    palette = nwfmt.decode_palette(pd, pf, pc)
            try:
                img = nwfmt.decode_texture(data, fmt, w, h, palette).astype(np.float32) / 255.0
            except Exception:
                img = None
            break
        self.cache[key] = img
        return img


def collect(mdl, tex):
    """[(material, P (n,3,3), T (n,3,2), C (n,3,4), texture or None)] of every draw."""
    out = []
    for mat_i, obj_i, node, xlu in mdl.draws:
        if mat_i >= len(mdl.materials) or obj_i >= len(mdl.objects):
            continue
        m = mdl.materials[mat_i]
        try:
            P, T, C = mdl.triangles(obj_i, node)
        except Exception:
            continue
        img = tex.get(*m.textures[0]) if m.textures else None
        out.append((m, P, T, C, img))
    return out


def tri_area(P):
    return 0.5 * np.linalg.norm(np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]), axis=1)


def part_stats(parts):
    """Centre (area-weighted), extent and mean texture colour of a set of draws."""
    cen, area_sum, lo, hi = np.zeros(3), 0.0, np.full(3, 1e9), np.full(3, -1e9)
    col, wsum = np.zeros(3), 0.0
    for m, P, T, C, img in parts:
        if not len(P):
            continue
        a = tri_area(P) + 1e-6
        cen += (P.mean(1) * a[:, None]).sum(0)
        area_sum += a.sum()
        lo, hi = np.minimum(lo, P.reshape(-1, 3).min(0)), np.maximum(hi, P.reshape(-1, 3).max(0))
        if img is not None:
            h, w = img.shape[:2]
            # sample each triangle at its corners and centre
            uv = np.concatenate([T, T.mean(1, keepdims=True)], 1).reshape(-1, 2)
            x = (np.floor(uv[:, 0] * w).astype(int)) % w
            y = (np.floor(uv[:, 1] * h).astype(int)) % h
            s = img[y, x]
            vc = np.concatenate([C, C.mean(1, keepdims=True)], 1).reshape(-1, 4)
            rgb = s[:, :3] * (vc[:, :3] if m.name != "lp_m" else 1.0)
            wt = np.repeat(a, 4) * np.maximum(s[:, 3], 0.05)
            col += (rgb * wt[:, None]).sum(0)
            wsum += wt.sum()
    if area_sum <= 0:
        return None
    return dict(centre=cen / area_sum, lo=lo, hi=hi, color=(col / wsum) if wsum > 0 else np.array([1.0, 0.85, 0.6]))


def render(parts, glow_mats, on, light, size=220, bbox=None):
    """Software render, 3/4 view from the front-right, with the glow materials on or off."""
    yaw, pitch = math.radians(-35), math.radians(28)
    Ry = np.array([[math.cos(yaw), 0, math.sin(yaw)], [0, 1, 0], [-math.sin(yaw), 0, math.cos(yaw)]])
    Rx = np.array([[1, 0, 0], [0, math.cos(pitch), -math.sin(pitch)], [0, math.sin(pitch), math.cos(pitch)]])
    R = Rx @ Ry
    lo, hi = bbox
    corners = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]) @ R.T
    clo, chi = corners.min(0), corners.max(0)
    scale = (size * 0.86) / max(chi[0] - clo[0], chi[1] - clo[1], 1e-3)
    off = np.array([size / 2 - (clo[0] + chi[0]) / 2 * scale, size / 2 + (clo[1] + chi[1]) / 2 * scale])

    def proj(p):
        v = p @ R.T
        return np.stack([v[..., 0] * scale + off[0], -v[..., 1] * scale + off[1], v[..., 2]], -1)

    img = np.zeros((size, size, 3), np.float32)
    bg = np.array([0.10, 0.10, 0.13]) if on else np.array([0.18, 0.18, 0.2])
    img[:] = bg
    zbuf = np.full((size, size), -1e9, np.float32)
    ldir = np.array([0.4, 0.8, 0.45])
    ldir /= np.linalg.norm(ldir)
    ys, xs = np.mgrid[0:size, 0:size]
    order = sorted(parts, key=lambda p: p[0].xlu)
    for m, P, T, C, tex in order:
        if not len(P):
            continue
        S = proj(P)
        N = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
        N /= np.linalg.norm(N, axis=1, keepdims=True) + 1e-9
        glow = m.name in glow_mats
        for t in range(len(P)):
            s = S[t]
            x0, x1 = int(max(0, np.floor(s[:, 0].min()))), int(min(size - 1, np.ceil(s[:, 0].max())))
            y0, y1 = int(max(0, np.floor(s[:, 1].min()))), int(min(size - 1, np.ceil(s[:, 1].max())))
            if x1 < x0 or y1 < y0:
                continue
            px, py = xs[y0:y1 + 1, x0:x1 + 1] + 0.5, ys[y0:y1 + 1, x0:x1 + 1] + 0.5
            (ax, ay), (bx, by), (cx, cy) = s[0, :2], s[1, :2], s[2, :2]
            den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            if abs(den) < 1e-9:
                continue
            w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den
            w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
            if not inside.any():
                continue
            z = w0 * s[0, 2] + w1 * s[1, 2] + w2 * s[2, 2]
            zb = zbuf[y0:y1 + 1, x0:x1 + 1]
            vis = inside & (z > zb)
            if not vis.any():
                continue
            u = w0 * T[t, 0, 0] + w1 * T[t, 1, 0] + w2 * T[t, 2, 0]
            v = w0 * T[t, 0, 1] + w1 * T[t, 1, 1] + w2 * T[t, 2, 1]
            vc = w0[..., None] * C[t, 0] + w1[..., None] * C[t, 1] + w2[..., None] * C[t, 2]
            if tex is not None:
                h, w = tex.shape[:2]
                tx = tex[(np.floor(v * h).astype(int)) % h, (np.floor(u * w).astype(int)) % w]
            else:
                tx = np.ones(vis.shape + (4,), np.float32)
            a = tx[..., 3] * (vc[..., 3] if not glow else 1.0)
            if not m.xlu:
                vis &= a > 0.5
            nl = abs(float(N[t] @ ldir))
            shade = 0.55 + 0.45 * nl
            if glow:
                col = tx[..., :3] * (1.15 if on else 0.55 * shade)
            else:
                col = tx[..., :3] * vc[..., :3] * shade * (0.75 if on else 1.0)
            dst = img[y0:y1 + 1, x0:x1 + 1]
            if m.xlu:
                al = np.clip(a, 0, 1)[..., None] * (0.6 if not glow else 0.8)
                dst[vis] = (dst * (1 - al) + col * al)[vis]
            else:
                dst[vis] = col[vis]
                zb[vis] = z[vis]
    if on and light is not None:
        p = proj(np.array(light["model_pos"]))
        # the light's reach drawn as a soft pool in its colour behind the model (screen space)
        r = min(light["radius"] * scale * 0.5, size * 0.6)
        d = np.sqrt((xs - p[0]) ** 2 + (ys - p[1]) ** 2)
        halo = (np.exp(-(d / max(r * 0.25, 2.0)) ** 2) * 0.35 + np.exp(-(d / max(r, 4.0)) ** 2) * 0.25) * min(light["intensity"], 1.6) / 1.4
        empty = (zbuf < -1e8)[..., None]
        img += halo[..., None] * np.array(light["color"])[None, None, :] * np.where(empty, 1.0, 0.35)
        # marker
        cx, cy = int(round(p[0])), int(round(p[1]))
        for k in range(-5, 6):
            for (yy, xx) in ((cy, cx + k), (cy + k, cx)):
                if 0 <= yy < size and 0 <= xx < size:
                    img[yy, xx] = [1.0, 0.2, 0.9]
    return Image.fromarray((np.clip(img, 0, 1) ** (1 / 1.0) * 255).astype(np.uint8))


# ---- proposals ---------------------------------------------------------------------------------
def propose(kind, stats, bbox, rec):
    lo, hi = np.array(bbox[0]), np.array(bbox[1])
    size = float(np.max(hi - lo))
    c = np.clip(stats["color"], 0, None)
    mx = max(float(c.max()), 1e-3)
    col = c / mx
    # light colour: the glowing part's mean colour, kept fairly saturated, never black
    col = 0.35 + 0.65 * col
    flicker = kind in ("fire",)
    if kind == "screen":
        col = 0.45 * col + 0.55 * np.array([0.62, 0.80, 1.0])
    if kind == "fire":
        col = 0.6 * col + 0.4 * np.array([1.0, 0.62, 0.3])
    col = col / max(col.max(), 1e-3)
    radius = float(np.clip(110.0 + 2.2 * size, 120.0, 320.0))
    # water (flowing water, pools, baths): listed, but no light proposed (suspected not a light)
    intensity = {"lamp": 1.4, "screen": 0.8, "fire": 1.6, "water": 0.0, "neon": 1.0, "lava": 0.9}.get(kind, 1.0)
    if rec and rec.get("light_kind") == 2:
        intensity *= 1.15
    return dict(model_pos=[float(x) for x in stats["centre"]], color=[round(float(x), 3) for x in col], radius=round(radius),
                intensity=round(intensity, 2), flicker=flicker, kind=kind)


def main():
    ap = argparse.ArgumentParser()
    here = Path(__file__).resolve()
    main_checkout = Path(r"C:/Users/rmelv/Desktop/Animal Crossing City Folk Recompiled")
    ap.add_argument("--disc", default=str(main_checkout / "US DATA" / "DATA"))
    ap.add_argument("--out", default=str(main_checkout / "out" / "furniture_lights"))
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    files = Path(a.disc) / "files"
    recs = item_records((files / "Item" / "item.arc").read_bytes())
    common = nwfmt.u8_files((files / "Ftr" / "roomAlways.arc").read_bytes()).get("int_common.brres")
    if common:
        cf = nwfmt.brres(common)
        Tex.common = (common, dict(cf.get("Textures(NW4R)", [])), dict(cf.get("Palettes(NW4R)", [])))
    out = Path(a.out)
    (out / "img").mkdir(parents=True, exist_ok=True)
    only = {int(x) for x in a.only.split(",") if x}
    rows = []
    n_files = 0
    for fn in sorted(os.listdir(files / "Item" / "Ftr"), key=lambda s: int(re.sub(r"\D", "", s))):
        n = int(re.sub(r"\D", "", fn))
        if only and n not in only:
            continue
        n_files += 1
        d = (files / "Item" / "Ftr" / fn).read_bytes()
        folders = nwfmt.brres(d)
        anim_names = [nm for f, items in folders.items() if f.startswith("Anm") for nm, _ in items]
        tex = Tex(d, folders)
        rec = recs.get(n)
        for mname, mo in folders.get("3DModels(NW4R)", []):
            try:
                mdl = nwfmt.Mdl0(d, mo)
            except Exception as e:
                print("Ftr%d %s: %s" % (n, mname, e))
                continue
            flags = {m.name: material_flags(m) for m in mdl.materials}
            model_text = " ".join([mdl.name, rec["name"] if rec else ""] + anim_names)
            model_hint = name_hint(model_text)
            water = is_water(model_text)
            found = []  # (material, why, source, kind)
            for mn, f in flags.items():
                generic = mn.startswith(("df_", "ev_", "ev")) or mn in ("df_m", "ev_m")
                guess = "water" if water else (f.get("hint") or model_hint or ("screen" if mn.startswith("tp_") else "lamp"))
                if "game" in f:
                    found.append((mn, "game helper material '%s' (%s)" % (mn, f["game"]), "a", f["game"]))
                elif f.get("bypass"):
                    found.append((mn, "lamp-style konst bypass of lighting", "b", guess))
                elif f.get("additive"):
                    found.append((mn, "additive blending", "b", guess))
                elif f.get("unlit") and not generic:
                    found.append((mn, "unlit special material", "b", guess))
                elif f.get("unlit") and f.get("hint"):
                    found.append((mn, "unlit material named '%s'" % f["hint"], "b", f["hint"]))
                elif f.get("hint") in ("fire", "neon", "lava") and not generic:
                    found.append((mn, "material / texture name '%s'" % f["hint"], "b", f["hint"]))
            if not found and model_hint in ("fire", "lava", "neon"):
                found.append(("", "model / item name '%s' (whole model)" % model_hint, "b", model_hint))
            if rec and rec["light_kind"] and not any(s == "a" for _, _, s, _ in found):
                found.append(("", "item record light kind %d (no lp_m material)" % rec["light_kind"], "a", "lamp"))
            if not found:
                continue
            parts = collect(mdl, tex)
            glow_names = {mn for mn, _, _, _ in found if mn}
            glow_parts = [p for p in parts if p[0].name in glow_names] or parts
            st = part_stats(glow_parts)
            allst = part_stats(parts)
            if st is None or allst is None:
                continue
            kinds = [k for _, _, _, k in found]
            if model_hint == "fire" or "fire" in kinds:
                kind = "fire"
            elif "screen" in kinds:
                kind = "screen"
            elif water or kinds == ["water"] * len(kinds):
                kind = "water"
            else:
                kind = next((k for mn, _, s, k in found if s == "a"), found[0][3])
            prop = propose(kind, st, mdl.bbox, rec)
            bbox = (np.minimum(allst["lo"], mdl.bbox[0]), np.maximum(allst["hi"], mdl.bbox[1]))
            tag = "Ftr%d" % n
            render(parts, glow_names, False, prop, bbox=bbox).save(out / "img" / (tag + "_off.png"))
            render(parts, glow_names, True, prop, bbox=bbox).save(out / "img" / (tag + "_on.png"))
            src = "".join(sorted({s for _, _, s, _ in found}))
            rows.append(dict(n=n, tag=tag, model=mdl.name, rec=rec, found=found, prop=prop, src=src,
                             anims=anim_names, size=[round(float(x), 1) for x in (np.array(mdl.bbox[1]) - np.array(mdl.bbox[0]))]))
            print("%s %-28s %-24s %s %s" % (tag, mdl.name, rec["name"] if rec else "?", src, [w for _, w, _, _ in found]))
    write_html(out, rows, n_files)
    # machine-readable proposals (compared with the runtime's CF_FURN_LOG lines)
    import json
    (out / "candidates.json").write_text(json.dumps([dict(ftr=r["n"], model=r["model"], item=r["rec"]["number"] if r["rec"] else None,
                                                          name=r["rec"]["name"] if r["rec"] else None, **r["prop"]) for r in rows], indent=1),
                                         encoding="utf-8")
    print("%d candidates of %d models -> %s" % (len(rows), n_files, out / "index.html"))


def write_html(out, rows, n_files):
    def colhex(c):
        return "#%02x%02x%02x" % tuple(int(max(0, min(1, x)) * 255) for x in c)

    na = sum(1 for r in rows if "a" in r["src"])
    nb = sum(1 for r in rows if r["src"] == "b")
    h = ["<!doctype html><html><head><meta charset='utf-8'><title>Furniture lights</title><style>",
         ":root{--bg:#f6f6f4;--fg:#1d1d1f;--card:#fff;--mut:#666;--bd:#ddd}",
         "@media (prefers-color-scheme: dark){:root{--bg:#18181b;--fg:#eee;--card:#232327;--mut:#aaa;--bd:#333}}",
         "body{background:var(--bg);color:var(--fg);font:13px/1.35 system-ui,sans-serif;margin:16px}",
         ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(330px,1fr));gap:10px}",
         ".c{background:var(--card);border:1px solid var(--bd);border-radius:8px;padding:8px}",
         ".c img{width:160px;height:160px;image-rendering:auto;border-radius:4px}",
         ".m{color:var(--mut)}.sw{display:inline-block;width:14px;height:14px;border-radius:3px;vertical-align:-3px;border:1px solid #0004}",
         ".tag{display:inline-block;padding:0 6px;border-radius:9px;font-size:11px;margin-right:4px}",
         ".a{background:#2d7d46;color:#fff}.b{background:#7a4fb5;color:#fff}",
         "input{padding:4px 8px;margin:0 8px 10px 0}</style></head><body>",
         "<h2>Furniture light candidates</h2>",
         "<p class='m'>%d candidates out of %d furniture models: %d found by (a) the game's own switch / glow data, %d by (b) the "
         "material scan only. Left: off, right: on (proposed light: halo in its colour, magenta cross = position). "
         "Renders are a simple software preview of the model (not the game's renderer).</p>" % (len(rows), n_files, na, nb),
         "<input id=q placeholder='filter (name, Ftr number, kind, a/b)' oninput='f()'>",
         "<div class=grid id=g>"]
    for r in sorted(rows, key=lambda r: (r["src"] != "a" and r["src"] != "ab", r["n"])):
        rec, p = r["rec"], r["prop"]
        name = rec["name"] if rec else "(no item record)"
        ident = ("item %04X, code %04X, action %02X, record light kind %d" % (rec["number"], rec["code"], rec["action"], rec["light_kind"])) if rec else ""
        why = "<br>".join(html.escape(w) for _, w, _, _ in r["found"])
        srcs = "".join("<span class='tag %s'>%s</span>" % (s, "(a) game" if s == "a" else "(b) scan") for s in r["src"])
        key = " ".join([name, r["tag"], r["model"], p["kind"], r["src"]]).lower()
        h.append("<div class=c data-k='%s'><b>%s</b> %s<br><span class=m>%s &middot; %s &middot; %s</span><br>"
                 "<img src='img/%s_off.png' title='off'> <img src='img/%s_on.png' title='on'><br>"
                 "kind <b>%s</b> &middot; colour <span class=sw style='background:%s'></span> %s &middot; radius %d &middot; "
                 "intensity %.2f%s<br><span class=m>size %s; light at %s (model space)<br>%s</span>%s</div>"
                 % (html.escape(key, quote=True), html.escape(name), srcs, r["tag"], html.escape(r["model"]), ident, r["tag"], r["tag"],
                    p["kind"], colhex(p["color"]), ", ".join("%.2f" % x for x in p["color"]), p["radius"], p["intensity"],
                    " &middot; <b>flicker</b>" if p["flicker"] else "", "x".join(str(x) for x in r["size"]),
                    ", ".join("%.0f" % x for x in p["model_pos"]), why,
                    ("<br><span class=m>anims: %s</span>" % html.escape(", ".join(r["anims"]))) if r["anims"] else ""))
    h.append("</div><script>function f(){const q=document.getElementById('q').value.toLowerCase();"
             "for(const c of document.querySelectorAll('.c'))c.style.display=c.dataset.k.includes(q)?'':'none'}</script></body></html>")
    (out / "index.html").write_text("\n".join(h), encoding="utf-8")


if __name__ == "__main__":
    main()
