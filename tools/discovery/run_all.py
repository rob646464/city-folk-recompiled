#!/usr/bin/env python3
"""Rebuild every table in config/ (docs/DISCOVERY.md "Re-running").

    python tools/discovery/run_all.py --dtk <dtk.exe> --refs <ref maps dir> [--fetch-refs]
        [--us "US DATA/DATA"] [--pal "PAL DATA/DATA"] [--jp "JAPAN DATA/DATA"] [--kr "KOREA DATA/DATA"] [--work build/discovery]

USA: discover (dtk) -> sdk_names -> strnames -> discover (names merged) -> verify.
PAL / Japan / Korea: discover (dtk) -> version_map (USA names) -> discover -> verify.
Then docs/SDK_NAMES.md.
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def run(*args):
    print(">", " ".join(str(a) for a in args), flush=True)
    subprocess.run([sys.executable, *map(str, args)], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dtk", required=True)
    ap.add_argument("--refs", required=True)
    ap.add_argument("--fetch-refs", action="store_true")
    ap.add_argument("--us", default=str(ROOT / "US DATA" / "DATA"))
    ap.add_argument("--pal", default=str(ROOT / "PAL DATA" / "DATA"))
    ap.add_argument("--jp", default=str(ROOT / "JAPAN DATA" / "DATA"))
    ap.add_argument("--kr", default=str(ROOT / "KOREA DATA" / "DATA"))
    ap.add_argument("--work", default=str(ROOT / "build" / "discovery"))
    a = ap.parse_args()
    work = Path(a.work)
    d = HERE
    run(d / "discover.py", "--disc", a.us, "--id", "RUUE01", "--dtk", a.dtk, "--work", work / "RUUE01")
    run(d / "sdk_names.py", "--id", "RUUE01", "--refs", a.refs, *(["--fetch"] if a.fetch_refs else []))
    run(d / "strnames.py", "--disc", a.us, "--id", "RUUE01", "--apply")
    run(d / "discover.py", "--disc", a.us, "--id", "RUUE01", "--work", work / "RUUE01", "--skip-dtk")
    run(d / "verify.py", "--disc", a.us, "--cfg", ROOT / "config" / "RUUE01")
    ids = ["RUUE01"]
    for vid, disc in (("RUUP01", a.pal), ("RUUJ01", a.jp), ("RUUK01", a.kr)):
        if not (Path(disc) / "sys" / "main.dol").exists():
            print("skipping", vid, "(no disc at", disc + ")")
            continue
        run(d / "discover.py", "--disc", disc, "--id", vid, "--dtk", a.dtk, "--work", work / vid)
        run(d / "version_map.py", "--base-disc", a.us, "--disc", disc, "--id", vid)
        run(d / "discover.py", "--disc", disc, "--id", vid, "--work", work / vid, "--skip-dtk")
        run(d / "verify.py", "--disc", disc, "--cfg", ROOT / "config" / vid)
        ids.append(vid)
    run(d / "sdk_report.py", "--ids", ",".join(ids), "--refs", a.refs)


if __name__ == "__main__":
    main()
