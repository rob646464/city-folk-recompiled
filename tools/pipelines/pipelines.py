#!/usr/bin/env python3
"""Pipeline precache lists (data/pipelines.bin; docs/RENDERING.md, "Pipeline precache").

The list holds Aurora pipeline configs (GX TEV/vertex/blend state structs, generated
metadata, no game data) in the format aurora_pipeline_cache_seed reads
(extern/aurora/include/aurora/pipeline_cache.h):

    "AUPC", u32 format 2, u32 flags (1 = zstd payload), u32 rows, u64 raw payload size,
    u32 CRC-32 of the raw payload, u32 reserved (0),
    payload: rows of {u8 type, u32 config version, u32 config size, u32 first frame used,
                      u32 CRC-32 of the config, config}

Format 1 (no CRCs: 24-byte header, 13-byte row headers) is still read. Aurora validates every
row (CRC, then field ranges) and skips damaged ones (patch 0017).

    python tools/pipelines/pipelines.py build -o data/pipelines.bin <pipeline_cache.db | .bin>...
    python tools/pipelines/pipelines.py info data/pipelines.bin

`build` merges every input (Aurora's pipeline_cache.db from sweep runs, older lists), keeps
only the newest config version per type, drops duplicates (keeping the earliest first-use
frame, which orders the background compile) and writes a zstd-compressed list.
"""
import argparse
import sqlite3
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"AUPC"
FORMAT = 2
FLAG_ZSTD = 1
TYPES = {0: "clear", 1: "gx", 2: "rml"}


def zstd():
    try:
        from compression import zstd as z  # Python 3.14+
        return z
    except ImportError:
        return None


def read_db(path: Path):
    con = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)
    try:
        for typ, version, size, config, first in con.execute(
                "SELECT type, config_version, config_size, config, first_frame_used FROM pipeline_cache"):
            if len(config) == size:
                yield typ, version, bytes(config), first
    finally:
        con.close()


def read_bin(path: Path):
    data = path.read_bytes()
    magic, fmt, flags, count, raw = struct.unpack_from("<4sIIIQ", data, 0)
    if magic != MAGIC or fmt not in (1, 2):
        raise SystemExit(f"{path}: not a pipeline list")
    header = 24 if fmt == 1 else 32
    payload = data[header:]
    if flags & FLAG_ZSTD:
        z = zstd()
        if z is None:
            raise SystemExit("zstd needs Python 3.14+")
        payload = z.decompress(payload)
    if fmt >= 2:
        (crc,) = struct.unpack_from("<I", data, 24)
        if zlib.crc32(payload) != crc:
            print(f"{path}: payload checksum mismatch", file=sys.stderr)
    off = 0
    for _ in range(count):
        typ, version, size, first = struct.unpack_from("<BIII", payload, off)
        off += 13
        if fmt >= 2:
            (row_crc,) = struct.unpack_from("<I", payload, off)
            off += 4
        config = payload[off:off + size]
        off += size
        if fmt >= 2 and zlib.crc32(config) != row_crc:
            print(f"{path}: row with a bad checksum skipped", file=sys.stderr)
            continue
        yield typ, version, config, first


def read_any(path: Path):
    return read_bin(path) if path.read_bytes()[:4] == MAGIC else read_db(path)


def build(inputs, out: Path, compress=True):
    rows = {}
    newest = {}
    for p in inputs:
        n = 0
        for typ, version, config, first in read_any(Path(p)):
            if typ not in (0, 1):  # RmlUi is not used
                continue
            newest[typ] = max(newest.get(typ, 0), version)
            key = (typ, version, config)
            rows[key] = min(rows.get(key, first), first)
            n += 1
        print(f"{p}: {n} rows")
    kept = sorted(((k, f) for k, f in rows.items() if k[1] == newest[k[0]]), key=lambda kf: (kf[1], kf[0][0]))
    payload = bytearray()
    for (typ, version, config), first in kept:
        payload += struct.pack("<BIIII", typ, version, len(config), first, zlib.crc32(config)) + config
    flags = 0
    body = bytes(payload)
    z = zstd() if compress else None
    if z is not None:
        body = z.compress(body, level=19)
        flags |= FLAG_ZSTD
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(struct.pack("<4sIIIQII", MAGIC, FORMAT, flags, len(kept), len(payload), zlib.crc32(payload), 0)
                    + body)
    counts = {TYPES[t]: sum(1 for (k, _) in kept if k[0] == t) for t in sorted(newest)}
    print(f"{out}: {len(kept)} configs {counts}, versions {newest}, {len(payload)} bytes raw, "
          f"{out.stat().st_size} bytes on disk")


def info(path: Path):
    by = {}
    for typ, version, config, first in read_any(path):
        k = (TYPES.get(typ, typ), version, len(config))
        by[k] = by.get(k, 0) + 1
    for (t, v, s), n in sorted(by.items(), key=str):
        print(f"{t} version {v} size {s}: {n}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("-o", "--out", required=True)
    b.add_argument("--raw", action="store_true", help="no zstd compression")
    b.add_argument("inputs", nargs="+")
    i = sub.add_parser("info")
    i.add_argument("path")
    a = ap.parse_args()
    if a.cmd == "build":
        build(a.inputs, Path(a.out), not a.raw)
    else:
        info(Path(a.path))


if __name__ == "__main__":
    sys.exit(main())
