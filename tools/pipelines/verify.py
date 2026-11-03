#!/usr/bin/env python3
"""Pipeline precache recording and coverage check (docs/RENDERING.md, "Pipeline precache").

Plays scripted routes (tools/routes/*.txt, CF_INPUT_CLOCK=frames) with the dev build, each run
with its own fresh shader cache (CF_CACHE_DIR), scratch APPDATA / NAND (a copy of a fixture
save, CF_FIXTURE) / card folders, muted, off screen, physical controllers hidden, and counts the
pipelines a draw needed that the precache had not built ("MISS (not precached)" lines of
CF_PIPELINE_LOG=1). Also reports the startup shader hold ("game starts after ... s").

    py -3.14 tools/pipelines/verify.py --work <scratch dir> [--routes town_day,museum,...]
        [--presets default,enh,fps120,...] [--list <pipelines.bin> | --no-list]
        [--exe build/windows-clang/cfrecomp.exe] [--disc "<US DATA/DATA>"] [--warm]

Recording: run with --no-list (or the current list) and merge every run's pipeline_cache.db:
    py -3.14 tools/pipelines/pipelines.py build -o data/pipelines.bin [data/pipelines.bin] <work>/*/*/cache/pipeline_cache.db
Verification: run with the new list; every route should report ~0 misses.
--warm: a second start of each run with the cache the first left (warm hold time).

Runs one game at a time and ends only the process it started.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROUTES = ROOT / "tools" / "routes"


def main_checkout() -> Path:
    # Worktrees live in <main>/.claude/worktrees/<name>: the discs and fixtures are in the main checkout.
    for d in [ROOT, *ROOT.parents]:
        if (d / "US DATA").is_dir():
            return d
    return ROOT


# Route name -> (route file, guest frames to play, extra environment). Town routes start from the
# user-town fixture (continue.txt: in town at the house by ~frame 5000).
def routes():
    fx = {"CF_FIXTURE": "user-town"}
    day = {"CF_CLOCK_START": "2026-10-27 10:00"}
    return {
        # Boot, strap screen, title (its town flyover), the new-game bus scene, name/town keyboards,
        # town arrival (new_game.txt, its own deterministic town).
        "new_game": ("new_game.txt", 15000, {**day}),
        # Saved town by day, walking around (the fixture's town).
        "town_day": ("precache_town.txt", 8000, {**fx, **day}),
        # Evening and night (street lights, lit windows, night sky).
        "town_night": ("precache_town.txt", 8000, {**fx, "CF_CLOCK_START": "2026-10-27 21:30"}),
        # Rain and snow (CF_WEATHER_FAKE, docs/CALENDAR.md; snow needs the snow season).
        "town_rain": ("precache_town.txt", 8000, {**fx, **day, "CF_WEATHER_FAKE": "rain"}),
        "town_snow": ("precache_town.txt", 8000, {**fx, "CF_CLOCK_START": "2027-01-15 11:00", "CF_WEATHER_FAKE": "heavysnow"}),
        # Buildings: the player's house; Nook's Cranny (Tom Nook, shop interior); the museum
        # (to_museum.txt walks the new_game town: its own deterministic town, no fixture).
        "house": ("precache_house.txt", 7800, {**fx, **day}),
        "nook": ("precache_nook.txt", 7800, {**fx, **day}),
        "museum": ("to_museum.txt", 18500, {**day}),
    }


# F1 setting presets (settings.ini sections): what changes shader variants.
PRESETS = {
    "default": {},
    "enh": {"enhanced_lighting": {"enabled": 1}},
    "fps120": {"video": {"target_fps": 120, "match_display_refresh": "false"}},
    "msaa4": {"video": {"msaa": 4}},
    # Performance: 2400p (resolution_scale 5 = 5 x 480p, more pixels than 4K) at 120 fps, no VSync.
    "perf4k120": {"video": {"target_fps": 120, "match_display_refresh": "false", "vsync": "false", "resolution_scale": 5}},
    "perf4k": {"video": {"resolution_scale": 5, "vsync": "false"}},
    "perf120": {"video": {"target_fps": 120, "match_display_refresh": "false", "vsync": "false"}},
}


def write_ini(path: Path, ini: dict):
    lines = []
    for sec, kv in ini.items():
        lines.append(f"[{sec}]")
        lines += [f"{k}={v}" for k, v in kv.items()]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_once(a, d: Path, route, frames, extra, ini, cache: Path, tag: str):
    app = d / "appdata"
    for sub in ("appdata", "card", "nand"):
        p = d / sub
        if p.exists():
            shutil.rmtree(p)
        p.mkdir(parents=True)
    cfg = app / "Animal Crossing City Folk Recompiled"
    cfg.mkdir(parents=True)
    if ini:
        write_ini(cfg / "settings.ini", ini)
    cache.mkdir(parents=True, exist_ok=True)
    e = dict(os.environ)
    e.update({
        "APPDATA": str(app), "CF_CACHE_DIR": str(cache), "CF_CARD_DIR": str(d / "card"), "CF_NAND_DIR": str(d / "nand"),
        "CF_OFFSCREEN": "1", "CF_BACKGROUND": "1", "CF_AUDIO": "0", "CF_INPUT_CLOCK": "frames",
        "AURORA_FIFO_INLINE": "1", "CF_PIPELINE_LOG": "1", "CF_LOG_TIME": "1",
        "CF_RNG_SEED": "12345678", "CF_CLOCK_START": "2026-10-27 10:00", "CF_CLOCK_FRAMES": "1",
        "SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_RAWINPUT": "0", "SDL_JOYSTICK_WGI": "0", "SDL_XINPUT_ENABLED": "0",
        "SDL_JOYSTICK_DIRECTINPUT": "0", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "0",
        "CF_QUIT_AFTER": str(int(frames / 60 * 2.5 + a.startup_seconds)),
    })
    if a.no_list:
        e["CF_PRECACHE"] = "0"
    elif a.list:
        e["CF_PIPELINE_LIST"] = a.list
    e.update(extra)
    e.update(dict(kv.split("=", 1) for kv in a.env if "=" in kv))
    cmd = [a.exe, "--disc", a.disc, "--input", "@" + str(ROUTES / route)]
    log_path = d / f"log{tag}.txt"
    t0 = time.time()
    with open(log_path, "w", encoding="utf-8") as log:
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        p = subprocess.Popen(cmd, env=e, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
        while p.poll() is None:
            time.sleep(3)
            text = log_path.read_text(encoding="utf-8", errors="replace")
            seen = re.findall(r"\[frame\] (\d+) presents", text)
            if seen and int(seen[-1]) >= frames:
                break
            if time.time() - log_path.stat().st_mtime > a.stall_seconds:
                print(f"{d.name}: no log output for {a.stall_seconds:.0f} s, ended (hang)", flush=True)
                break
        if p.poll() is None:
            p.kill()  # only the process this run started
            p.wait()
    text = log_path.read_text(encoding="utf-8", errors="replace")
    misses = [l for l in text.splitlines() if "MISS (not precached)" in l]
    start = re.search(r"game starts after ([0-9.]+) s of shader preparation \(([0-9.]+) s after startup; (\d+) pipelines", text)
    seen = re.findall(r"\[frame\] (\d+) presents", text)
    r = {"run": d.name + tag, "misses": len(misses), "seconds": round(time.time() - t0),
         "hold_s": float(start.group(1)) if start else None, "pipelines": int(start.group(3)) if start else None,
         "presents": int(seen[-1]) if seen else 0}
    r["complete"] = r["presents"] >= frames
    (d / f"misses{tag}.txt").write_text("\n".join(misses) + "\n", encoding="utf-8")
    print(json.dumps(r), flush=True)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--exe", default=str(ROOT / "build/windows-clang/cfrecomp.exe"))
    ap.add_argument("--disc", default=str(main_checkout() / "US DATA" / "DATA"))
    ap.add_argument("--list", help="precache list (CF_PIPELINE_LIST; default: the one next to the exe)")
    ap.add_argument("--no-list", action="store_true", help="no shipped list (CF_PRECACHE=0): record from nothing")
    ap.add_argument("--routes", default="")
    ap.add_argument("--presets", default="default")
    ap.add_argument("--cache-from", help="start from a copy of this cache folder (a warm cache) instead of an empty one")
    ap.add_argument("--warm", action="store_true", help="start each run a second time with its cache (warm hold)")
    ap.add_argument("--env", action="append", default=[], help="extra VAR=value for every run")
    ap.add_argument("--file", help="ad-hoc route file (run name 'adhoc'; with --frames, fixture save)")
    ap.add_argument("--frames", type=int, default=9000)
    ap.add_argument("--startup-seconds", type=float, default=180)
    ap.add_argument("--stall-seconds", type=float, default=300)
    a = ap.parse_args()
    rs = routes()
    if a.file:
        rs["adhoc"] = (str(Path(a.file).resolve()), a.frames, {"CF_FIXTURE": "user-town"})
        a.routes = a.routes or "adhoc"
    want = [r for r in a.routes.split(",") if r] or list(rs)
    results = []
    for preset in [p for p in a.presets.split(",") if p]:
        for name in want:
            route, frames, extra = rs[name]
            d = Path(a.work) / preset / name
            d.mkdir(parents=True, exist_ok=True)
            cache = d / "cache"
            if cache.exists():
                shutil.rmtree(cache)
            if a.cache_from:
                shutil.copytree(a.cache_from, cache)
            r = run_once(a, d, route, frames, extra, PRESETS[preset], cache, "")
            r["preset"] = preset
            results.append(r)
            if a.warm:
                # Only the startup hold matters: a short run with the cache the first one left.
                w = run_once(a, d, route, 600, extra, PRESETS[preset], cache, "_warm")
                w["preset"] = preset
                results.append(w)
    out = Path(a.work) / "results.json"
    old = json.loads(out.read_text(encoding="utf-8")) if out.exists() else []
    out.write_text(json.dumps(old + results, indent=1), encoding="utf-8")
    for preset in sorted({r["preset"] for r in results}):
        rr = [r for r in results if r["preset"] == preset and not r["run"].endswith("_warm")]
        print(f"{preset}: {sum(r['misses'] for r in rr)} misses over {len(rr)} routes "
              f"({', '.join(str(r['run']) + ' ' + str(r['misses']) for r in rr)})")


if __name__ == "__main__":
    sys.exit(main())
