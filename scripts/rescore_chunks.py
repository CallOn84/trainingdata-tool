#!/usr/bin/env python3
"""Rescore a whole tree of V6 chunks with Syzygy tablebases.

`rescore_chunk` handles exactly one chunk per invocation (--chunk_path) and
writes `<stem>_rescored.gz` beside the input. That is the safe variant: unlike
lc0's standalone `rescorer`, it never deletes anything. But one process per
file does not scale to a million chunks on its own -- hence this driver.

What rescoring does: positions that reach a tablebase are given their true
game-theoretic result, so a "won" position that is actually a draw gets
relabelled, and the moves-left target is corrected to the real distance. That
is exactly what makes finished games worth finishing.

Usage:
    py scripts/rescore_chunks.py CHUNK_DIR --syzygy PATH [--replace]

The default leaves both files in place (`game_000000.gz` and
`game_000000_rescored.gz`), which doubles disk. `--replace` moves the rescored
file over the original once it has been written successfully, keeping disk
flat. Nothing is deleted on failure either way.
"""
import argparse
import concurrent.futures
import gzip
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

# V6 record layout (trainingdata_v6.h): version, input_format, probabilities,
# planes, then 8 single-byte fields, then the float block starting at root_q.
FRAME_SIZE = 8356
FLOAT_OFF = 4 + 4 + 1858 * 4 + 104 * 8 + 8
N_FLOATS = 15
I_PLIES_LEFT = 6
I_RESULT_Q = 7

DEFAULT_RESCORER = (r"C:\Users\Contrad\Documents\Code\repos\lc0-training"
                    r"\official-training-branch\build\windows\rescore_chunk.exe")
DEFAULT_SYZYGY = r"C:\Users\Contrad\Documents\syzygy\3-4-5"

SUFFIX = "_rescored.gz"


def find_chunks(root, resume):
    """Every chunk under `root`, skipping rescorer output and, with --resume,
    anything already carrying a finished rescored twin."""
    out = []
    for dirpath, _, filenames in os.walk(root):
        names = set(filenames)
        for name in filenames:
            if not name.endswith(".gz") or name.endswith(SUFFIX):
                continue
            if resume and (name[:-3] + SUFFIX) in names:
                continue
            out.append(Path(dirpath) / name)
    return sorted(out)


def read_frames(path):
    """The 15-float block of every frame in a chunk."""
    data = gzip.open(path, "rb").read()
    return [struct.unpack("<15f", data[i * FRAME_SIZE + FLOAT_OFF:
                                       i * FRAME_SIZE + FLOAT_OFF + 60])
            for i in range(len(data) // FRAME_SIZE)]


def outcome(frames):
    """W/D/L for the game, read off result_q at ply 0. Matches how the
    standalone rescorer tallies its 'Original L: D: W:' line."""
    if not frames:
        return None
    q = frames[0][I_RESULT_Q]
    return "W" if q > 0.5 else ("L" if q < -0.5 else "D")


class Stats:
    """Aggregates in the shape the standalone lc0 rescorer reports, so the two
    can be compared directly."""

    def __init__(self):
        self.games = 0
        self.positions = 0
        self.changed = 0
        self.result_q_changed = 0
        self.plies_left_changed = 0
        self.before = {"W": 0, "D": 0, "L": 0}
        self.after = {"W": 0, "D": 0, "L": 0}
        self.empty = 0

    def add(self, other):
        self.games += other.games
        self.positions += other.positions
        self.changed += other.changed
        self.result_q_changed += other.result_q_changed
        self.plies_left_changed += other.plies_left_changed
        self.empty += other.empty
        for k in "WDL":
            self.before[k] += other.before[k]
            self.after[k] += other.after[k]


def compare(before, after):
    st = Stats()
    st.games = 1
    st.positions = len(before)
    if not before:
        st.empty = 1
        return st
    ob, oa = outcome(before), outcome(after)
    if ob:
        st.before[ob] += 1
    if oa:
        st.after[oa] += 1
    for x, y in zip(before, after):
        diff = [k for k in range(N_FLOATS) if abs(x[k] - y[k]) > 1e-6]
        if diff:
            st.changed += 1
        if I_RESULT_Q in diff:
            st.result_q_changed += 1
        if I_PLIES_LEFT in diff:
            st.plies_left_changed += 1
    return st


def rescore_one(args):
    path, rescorer, syzygy, replace, extra, want_stats = args
    before = read_frames(path) if want_stats else None

    cmd = [rescorer, f"--chunk_path={path}", f"--syzygy_paths={syzygy}"] + extra
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return path, "timeout", None
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        return path, (tail[-1] if tail else f"exit {proc.returncode}"), None

    produced = path.with_name(path.name[:-3] + SUFFIX)
    if not produced.is_file():
        return path, "no output file produced", None

    st = compare(before, read_frames(produced)) if want_stats else None
    if replace:
        # Only after a confirmed successful write -- a failed rescore must
        # never cost the original chunk.
        os.replace(produced, path)
    return path, None, st


def _progress(done, total, failed, t0):
    frac = done / total if total else 1.0
    bar = "#" * int(30 * frac) + "-" * (30 - int(30 * frac))
    el = time.time() - t0
    eta = (el / done) * (total - done) if done else 0
    sys.stderr.write(
        f"\r[{bar}] {100*frac:5.1f}% {done}/{total} | {failed} failed | "
        f"elapsed {el/60:.0f}m | ETA {eta/60:.0f}m   ")
    sys.stderr.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("chunk_dir", help="Directory tree of .gz chunks")
    ap.add_argument("--rescorer", default=DEFAULT_RESCORER)
    ap.add_argument("--syzygy", default=DEFAULT_SYZYGY)
    ap.add_argument("--workers", type=int, default=max(1, os.cpu_count() - 1))
    ap.add_argument("--replace", action="store_true",
                    help="Move each rescored chunk over its original once it "
                         "is written (keeps disk usage flat)")
    ap.add_argument("--resume", action="store_true",
                    help="Skip chunks that already have a _rescored.gz twin. "
                         "Has no effect with --replace, which leaves no twin")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--dist-temp", type=float, default=None)
    ap.add_argument("--dist-offset", type=float, default=None)
    ap.add_argument("--dtz-boost", type=float, default=None)
    ap.add_argument("--no-stats", action="store_true",
                    help="Skip the before/after summary. Computing it reads "
                         "each chunk twice, so this is the faster option when "
                         "you only care that the run completed")
    ap.add_argument("--no-progress", action="store_true")
    args = ap.parse_args()

    if not Path(args.rescorer).is_file():
        ap.error(f"rescorer not found: {args.rescorer}")
    if not Path(args.syzygy).is_dir():
        ap.error(f"syzygy directory not found: {args.syzygy}")

    extra = []
    if args.dist_temp is not None:
        extra.append(f"--dist_temp={args.dist_temp}")
    if args.dist_offset is not None:
        extra.append(f"--dist_offset={args.dist_offset}")
    if args.dtz_boost is not None:
        extra.append(f"--dtz_boost={args.dtz_boost}")

    print(f"Scanning {args.chunk_dir}...", flush=True)
    chunks = find_chunks(args.chunk_dir, args.resume)
    if args.limit:
        chunks = chunks[:args.limit]
    if not chunks:
        print("No chunks to rescore.")
        return
    print(f"{len(chunks)} chunks, {args.workers} workers, syzygy={args.syzygy}",
          flush=True)

    want_stats = not args.no_stats
    work = [(c, args.rescorer, args.syzygy, args.replace, extra, want_stats)
            for c in chunks]
    t0 = time.time()
    done = 0
    failures = []
    totals = Stats()
    with concurrent.futures.ThreadPoolExecutor(args.workers) as pool:
        for path, err, st in pool.map(rescore_one, work):
            done += 1
            if err:
                failures.append((path, err))
            elif st is not None:
                totals.add(st)
            if not args.no_progress and (done % 25 == 0 or done == len(work)):
                _progress(done, len(work), len(failures), t0)
    if not args.no_progress:
        sys.stderr.write("\n")

    el = time.time() - t0
    if want_stats:
        b, a = totals.before, totals.after
        print(f"Games processed: {totals.games}", flush=True)
        print(f"Positions processed: {totals.positions}", flush=True)
        # Deliberately not the same quantity as the standalone rescorer's
        # "Rescores performed", which counts tablebase applications including
        # ones that write back the value already there. This counts frames
        # whose stored data actually differs afterwards.
        print(f"Rescores performed (frames actually changed): "
              f"{totals.changed}", flush=True)
        print(f"  of which result_q changed: {totals.result_q_changed}",
              flush=True)
        print(f"  of which plies_left changed: {totals.plies_left_changed}",
              flush=True)
        if totals.empty:
            print(f"Empty chunks (no frames): {totals.empty}", flush=True)
        print(f"Original L: {b['L']} D: {b['D']} W: {b['W']}", flush=True)
        print(f"After    L: {a['L']} D: {a['D']} W: {a['W']}", flush=True)
        moved = ((a['L'] - b['L']), (a['D'] - b['D']), (a['W'] - b['W']))
        if any(moved):
            print(f"Outcome shift  L: {moved[0]:+d} D: {moved[1]:+d} "
                  f"W: {moved[2]:+d}", flush=True)
        else:
            print("Outcome shift: none -- no game changed W/D/L category",
                  flush=True)
    print(f"Done: {done - len(failures)}/{done} chunks rescored in {el/60:.1f}m",
          flush=True)
    if failures:
        print(f"{len(failures)} FAILED (originals untouched):", flush=True)
        for path, err in failures[:20]:
            print(f"  {path}: {err}", flush=True)
        if len(failures) > 20:
            print(f"  ... and {len(failures)-20} more", flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
