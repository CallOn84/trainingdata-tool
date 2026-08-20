#!/usr/bin/env python3
"""Run lc0's standalone rescorer over every chunk directory, and total up the
statistics it reports per directory.

The rescorer takes one directory per invocation -- its GetFileList() skips
subdirectories outright, so pointing it at the parent of sup01-0/, sup01-1/ ...
finds nothing. This walks them and runs it on each, then sums the per-directory
summaries into one report in the same shape.

It is much faster than driving rescore_chunk per file: one process handles a
whole directory and takes --threads, instead of paying process startup and
tablebase init 5,000 times.

Safety: --delete-files=false is passed on every invocation and is not
overridable here. The rescorer's own default is *true*, and its remove() sits
outside the try/catch, so it deletes inputs on failure as well as success.

Usage:
    py scripts/rescore_all.py CHUNK_ROOT --output-root OUT [--threads N]
    py scripts/rescore_all.py CHUNK_ROOT --replace [--threads N]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_RESCORER = (r"C:\Users\Contrad\Documents\Code\repos\lc0-training"
                    r"\training-data-tool\lc0\build-rescorer\rescorer.exe")
DEFAULT_SYZYGY = r"C:\Users\Contrad\Documents\syzygy\3-4-5"

# Lines the rescorer prints at the end of each directory.
PATTERNS = {
    "games": re.compile(r"^Games processed:\s*(\d+)", re.M),
    "positions": re.compile(r"^Positions processed:\s*(\d+)", re.M),
    "rescores": re.compile(r"^Rescores performed:\s*(\d+)", re.M),
    "outcome_change": re.compile(r"^Cumulative outcome change:\s*(\d+)", re.M),
    "secondary": re.compile(r"^Secondary rescores performed:\s*(\d+)", re.M),
    "secondary_dtz": re.compile(
        r"^Secondary rescores performed used dtz:\s*(\d+)", re.M),
}
BEFORE_RE = re.compile(r"^Original L:\s*(\d+) D:\s*(\d+) W:\s*(\d+)", re.M)
AFTER_RE = re.compile(r"^After L:\s*(\d+) D:\s*(\d+) W:\s*(\d+)", re.M)


def chunk_dirs(root):
    out = []
    for entry in sorted(os.scandir(root), key=lambda e: e.name):
        if entry.is_dir() and any(f.endswith(".gz")
                                  for f in os.listdir(entry.path)):
            out.append(Path(entry.path))
    return out


def natural_key(path):
    """sup01-2 sorts before sup01-10, and sup02-* after all sup01-*."""
    return [int(t) if t.isdigit() else t
            for t in re.split(r"(\d+)", path.name)]


def parse(text):
    stats = {k: int(m.group(1)) if (m := rx.search(text)) else 0
             for k, rx in PATTERNS.items()}
    b = BEFORE_RE.search(text)
    a = AFTER_RE.search(text)
    stats["before"] = tuple(int(x) for x in b.groups()) if b else (0, 0, 0)
    stats["after"] = tuple(int(x) for x in a.groups()) if a else (0, 0, 0)
    stats["parsed"] = bool(b and a)
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("chunk_root")
    ap.add_argument("--rescorer", default=DEFAULT_RESCORER)
    ap.add_argument("--syzygy", default=DEFAULT_SYZYGY)
    ap.add_argument("--output-root",
                    help="Rescored chunks are written under here, mirroring "
                         "the input directory names")
    ap.add_argument("--replace", action="store_true",
                    help="Write to a temporary directory and move the result "
                         "over the input once it succeeds, keeping disk flat")
    ap.add_argument("--threads", type=int, default=max(1, os.cpu_count() - 1))
    ap.add_argument("--resume", action="store_true",
                    help="Skip directories whose output directory already "
                         "exists and is non-empty (--output-root only)")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--dist-temp", type=float, default=None)
    ap.add_argument("--dist-offset", type=float, default=None)
    ap.add_argument("--dtz-boost", type=float, default=None)
    args = ap.parse_args()

    if bool(args.output_root) == bool(args.replace):
        ap.error("pass exactly one of --output-root or --replace")
    if not Path(args.rescorer).is_file():
        ap.error(f"rescorer not found: {args.rescorer}")
    if not Path(args.syzygy).is_dir():
        ap.error(f"syzygy directory not found: {args.syzygy}")

    root = Path(args.chunk_root)
    dirs = sorted(chunk_dirs(root), key=natural_key)
    if not dirs:
        ap.error(f"no subdirectories containing .gz files under {root}")
    if args.limit:
        dirs = dirs[:args.limit]

    extra = []
    if args.dist_temp is not None:
        extra.append(f"--dist-temp={args.dist_temp}")
    if args.dist_offset is not None:
        extra.append(f"--dist-offset={args.dist_offset}")
    if args.dtz_boost is not None:
        extra.append(f"--dtz-boost={args.dtz_boost}")

    print(f"{len(dirs)} chunk directories, {args.threads} threads, "
          f"syzygy={args.syzygy}", flush=True)

    totals = {k: 0 for k in PATTERNS}
    before = [0, 0, 0]
    after = [0, 0, 0]
    failures = []
    t0 = time.time()

    for i, d in enumerate(dirs, 1):
        if args.replace:
            out_dir = d.parent / (d.name + ".rescored.tmp")
            if out_dir.exists():
                shutil.rmtree(out_dir)
        else:
            out_dir = Path(args.output_root) / d.name
            if args.resume and out_dir.is_dir() and any(out_dir.iterdir()):
                print(f"[{i}/{len(dirs)}] {d.name}: exists, skipping",
                      flush=True)
                continue
        out_dir.mkdir(parents=True, exist_ok=True)

        cmd = [args.rescorer, "rescore", f"--input={d}", f"--output={out_dir}",
               f"--syzygy-paths={args.syzygy}", "--delete-files=false",
               f"--threads={args.threads}"] + extra
        proc = subprocess.run(cmd, capture_output=True, text=True)
        text = (proc.stdout or "") + (proc.stderr or "")

        if proc.returncode != 0:
            failures.append((d.name, f"exit {proc.returncode}"))
            print(f"[{i}/{len(dirs)}] {d.name}: FAILED (exit "
                  f"{proc.returncode}) -- input untouched", flush=True)
            continue

        st = parse(text)
        if not st["parsed"]:
            failures.append((d.name, "could not parse summary"))
            print(f"[{i}/{len(dirs)}] {d.name}: ran but summary unparseable "
                  f"-- input untouched", flush=True)
            continue

        for k in PATTERNS:
            totals[k] += st[k]
        for j in range(3):
            before[j] += st["before"][j]
            after[j] += st["after"][j]

        if args.replace:
            # Swap only after a successful, parsed run.
            backup = d.parent / (d.name + ".old.tmp")
            os.replace(d, backup)
            os.replace(out_dir, d)
            shutil.rmtree(backup)

        el = time.time() - t0
        eta = (el / i) * (len(dirs) - i)
        print(f"[{i}/{len(dirs)}] {d.name}: {st['games']} games, "
              f"{st['rescores']} rescores | elapsed {el/60:.0f}m "
              f"ETA {eta/60:.0f}m", flush=True)

    el = time.time() - t0
    print()
    print("=" * 58, flush=True)
    print(f"Games processed: {totals['games']}", flush=True)
    print(f"Positions processed: {totals['positions']}", flush=True)
    print(f"Rescores performed: {totals['rescores']}", flush=True)
    print(f"Cumulative outcome change: {totals['outcome_change']}", flush=True)
    print(f"Secondary rescores performed: {totals['secondary']}", flush=True)
    print(f"Secondary rescores performed used dtz: {totals['secondary_dtz']}",
          flush=True)
    print(f"Original L: {before[0]} D: {before[1]} W: {before[2]}", flush=True)
    print(f"After    L: {after[0]} D: {after[1]} W: {after[2]}", flush=True)
    moved = [after[j] - before[j] for j in range(3)]
    if any(moved):
        print(f"Outcome shift  L: {moved[0]:+d} D: {moved[1]:+d} "
              f"W: {moved[2]:+d}", flush=True)
    else:
        print("Outcome shift: none -- no game changed W/D/L category",
              flush=True)
    print(f"Completed in {el/60:.1f}m", flush=True)

    if failures:
        print(f"\n{len(failures)} directories FAILED (inputs untouched):",
              flush=True)
        for name, err in failures[:20]:
            print(f"  {name}: {err}", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
