"""Contact sheet of CF_SHOT frames (route work): py tools/routes/sheet.py <shot dir> <out.png>
[--cols N] [--width W] [--from F] [--to T] [--one FRAME out.png]

Each tile is labelled with its frame number. --one converts a single frame to PNG (full size).
"""
import argparse
import os
import re

from PIL import Image, ImageDraw


def frames(d):
    out = []
    for n in os.listdir(d):
        m = re.match(r"s(\d+)\.(ppm|png)$", n)
        if m:
            out.append((int(m.group(1)), os.path.join(d, n)))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("out")
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--from", dest="first", type=int, default=0)
    ap.add_argument("--to", dest="last", type=int, default=1 << 30)
    ap.add_argument("--one", type=int, default=None)
    a = ap.parse_args()
    fs = frames(a.dir)
    if a.one is not None:
        for f, p in fs:
            if f == a.one:
                Image.open(p).convert("RGB").save(a.out)
                return
        raise SystemExit("frame not found")
    fs = [(f, p) for f, p in fs if a.first <= f <= a.last]
    if not fs:
        raise SystemExit("no frames")
    im0 = Image.open(fs[0][1])
    w = a.width
    h = int(im0.height * w / im0.width)
    rows = (len(fs) + a.cols - 1) // a.cols
    sheet = Image.new("RGB", (a.cols * w, rows * (h + 14)), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for i, (f, p) in enumerate(fs):
        x, y = (i % a.cols) * w, (i // a.cols) * (h + 14)
        sheet.paste(Image.open(p).convert("RGB").resize((w, h)), (x, y + 14))
        d.text((x + 2, y + 1), str(f), fill=(255, 255, 0))
    sheet.save(a.out)


if __name__ == "__main__":
    main()
