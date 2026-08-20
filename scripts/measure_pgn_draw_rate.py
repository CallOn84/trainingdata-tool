#!/usr/bin/env python3
"""Measure the empirical draw rate as a function of eval, directly from the
PGNs being converted.

This is the *right* target for training-data generation, and it is not the
same thing as lc0's WDLDrawRateReference. That parameter describes the net
you are running -- it is looked up by running that net from startpos and
reading its WDL output. But when generating supervised training data we are
not running a net at all, and the games are not lc0 self-play: they are
Stockfish/Fishtest games with their own opening book, time control, and
adjudication rules, and therefore their own draw-rate characteristic.
Borrowing an lc0 net's draw rate would target the wrong distribution (and
would be circular if that net is the one being trained).

So instead: read the evals already in the PGN comments, pair each position
with the actual result of the game it came from, and bucket. The fraction
of drawn games at eval e IS D(e) for this data, by definition. That's the
ground truth the value head should be learning.

Caveat worth knowing: games stopped early by adjudication never reach a
real result, so their recorded outcome reflects the adjudicator, not play.
Run scripts/finish_games.py first if you want outcomes that were actually
played out.

Usage:
    py scripts/measure_pgn_draw_rate.py games.pgn[.gz] [--limit N]
"""
import argparse
import gzip
import math
import re
import sys

# Same comment format PGNGame.cpp parses: "-0.76/18 1.813s", "+M27/18 ...".
SCORE_RE = re.compile(r"\{\s*([+-]?\d+(?:\.\d+)?)/\d+")
MATE_RE = re.compile(r"\{\s*([+-])M\d+/\d+")

BANDS = [(0.00, 0.10), (0.10, 0.25), (0.25, 0.50), (0.50, 0.75),
         (0.75, 1.00), (1.00, 1.50), (1.50, 2.00), (2.00, 3.00),
         (3.00, 5.00), (5.00, 1e9)]


def open_maybe_gzip(path):
    if str(path).endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return open(path, "r", encoding="utf-8", errors="replace")


def spread_from_draw_rate(r):
    r = min(max(r, 1e-6), 1.0 - 1e-6)
    return 1.0 / math.log((1.0 + r) / (1.0 - r))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pgn")
    ap.add_argument("--limit", type=int, default=20000,
                    help="max games to scan (default 20000)")
    args = ap.parse_args()

    # band -> [n_positions, n_from_drawn_games]
    counts = {b: [0, 0] for b in BANDS}
    result = None
    games = 0
    movetext = []

    def flush():
        nonlocal result, movetext
        if result is None or not movetext:
            return
        text = " ".join(movetext)
        is_draw = (result == "1/2-1/2")
        for m in SCORE_RE.finditer(text):
            v = abs(float(m.group(1)))
            for b in BANDS:
                if b[0] <= v < b[1]:
                    counts[b][0] += 1
                    if is_draw:
                        counts[b][1] += 1
                    break
        # Mate scores are decisive by construction; count them in the top band.
        n_mate = len(MATE_RE.findall(text))
        if n_mate:
            top = BANDS[-1]
            counts[top][0] += n_mate
            if is_draw:
                counts[top][1] += n_mate
        result = None
        movetext = []

    with open_maybe_gzip(args.pgn) as f:
        for line in f:
            if line.startswith("[Result "):
                flush()
                m = re.search(r'"([^"]*)"', line)
                result = m.group(1) if m else None
            elif line.startswith("["):
                continue
            elif line.strip():
                movetext.append(line.strip())
                if line.rstrip().endswith(("1-0", "0-1", "1/2-1/2")):
                    games += 1
                    flush()
                    if games >= args.limit:
                        break
        flush()

    print(f"Scanned {games} games from {args.pgn}\n")
    print("  |eval| band        positions   drawn      D (empirical)")
    total_n = total_d = 0
    for b in BANDS:
        n, d = counts[b]
        total_n += n
        total_d += d
        if n == 0:
            continue
        hi = "inf" if b[1] > 1e8 else f"{b[1]:.2f}"
        print(f"  [{b[0]:.2f}, {hi:>4})      {n:<11} {d:<10} {d / n:.4f}")

    if total_n:
        print(f"\n  overall                {total_n:<11} {total_d:<10} "
              f"{total_d / total_n:.4f}")
        near = counts[BANDS[0]]
        if near[0]:
            r = near[1] / near[0]
            print(f"\nDraw rate at near-equal evals (|eval| < 0.10): {r:.4f}")
            print(f"  -> implied WDL spread = {spread_from_draw_rate(r):.4f}")
            print("  (spread = 1/log((1+r)/(1-r)), same expression lc0 uses")
            print("   for scale_reference in AccurateWDLRescaleParams)")


if __name__ == "__main__":
    main()
