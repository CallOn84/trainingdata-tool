#!/usr/bin/env python3
"""Measure the draw rate of a network from real V6 training chunks, and
report the WDL `spread` parameter it implies.

Per the lc0 v0.30 WDL-rescale blog post, WDLDrawRateReference is "the
initial draw rate estimation for your chosen neural network", looked up by
"running Lc0 with the network of your choice (supporting the WDL output) at
default settings from the startpos". This script recovers the same quantity
offline from training chunks the net already produced, so you don't have to
run the engine.

The draw rate is not a free knob in the WDL model -- it *determines* the
spread. lc0 computes scale_reference = 1/log((1+r)/(1-r)) from the draw
rate r (see AccurateWDLRescaleParams in search/classic/params.cc), and that
scale is exactly the spread our reconstruction uses:

    D(at an equal position) = 1 - 2*logistic(-1/s)

inverts to s = 1/log((1+D)/(1-D)), i.e. the same expression. So measure the
draw rate, convert, and use it -- don't fit it.

Reports the rate at several |Q| thresholds and, separately, restricted to
early-game positions (high plies_left), which is the closest offline analog
to "the draw rate at startpos".
"""
import glob
import gzip
import math
import struct
import sys

STRUCT_FMT = "<" "I" "I" "1858f" "104Q" "8B" "15f" "I" "H" "H" "f" "I"
STRUCT_SIZE = 8356
FLOATS_START = 2 + 1858 + 104 + 8
ROOT_Q = FLOATS_START
ROOT_D = FLOATS_START + 2
PLIES_LEFT = FLOATS_START + 6


def read_positions(filename):
    with gzip.open(filename, "rb") as f:
        while True:
            data = f.read(STRUCT_SIZE)
            if not data or len(data) != STRUCT_SIZE:
                return
            u = struct.unpack(STRUCT_FMT, data)
            yield u[ROOT_Q], u[ROOT_D], u[PLIES_LEFT]


def spread_from_draw_rate(r):
    """lc0's scale_reference: 1/log((1+r)/(1-r))."""
    r = min(max(r, 1e-6), 1.0 - 1e-6)
    return 1.0 / math.log((1.0 + r) / (1.0 - r))


def draw_rate_from_spread(s):
    """Inverse: the D an equal position gets at this spread."""
    return 1.0 - 2.0 / (1.0 + math.exp(1.0 / s))


def summarize(label, ds):
    if not ds:
        print(f"  {label:<34} (no positions)")
        return
    avg = sum(ds) / len(ds)
    ds_sorted = sorted(ds)
    med = ds_sorted[len(ds_sorted) // 2]
    print(f"  {label:<34} n={len(ds):<7} avg={avg:.4f}  median={med:.4f}  "
          f"-> spread={spread_from_draw_rate(avg):.4f}")


def main():
    pattern = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 800

    files = sorted(glob.glob(pattern))[:limit]
    print(f"Scanning {len(files)} files...", file=sys.stderr)

    by_threshold = {t: [] for t in (0.01, 0.03, 0.05, 0.10)}
    early = []      # near-equal AND early in the game
    total = 0
    max_plies = 0

    for f in files:
        try:
            for q, d, plies in read_positions(f):
                total += 1
                max_plies = max(max_plies, plies)
                aq = abs(q)
                for t in by_threshold:
                    if aq < t:
                        by_threshold[t].append(d)
                if aq < 0.05 and plies > 150:
                    early.append(d)
        except Exception:
            pass

    print(f"\nTotal positions: {total}")
    print(f"Max plies_left seen: {max_plies:.0f}")
    print("\nDraw rate at near-equal positions (all game phases):")
    for t in sorted(by_threshold):
        summarize(f"|Q| < {t}", by_threshold[t])

    print("\nEarly-game only (|Q| < 0.05 and plies_left > 150):")
    print("  -- closest offline analog to the startpos draw rate the")
    print("     lc0 blog tells you to read off the engine")
    summarize("early near-equal", early)

    print("\nReference points:")
    for r in (0.50, 0.58, 0.65):
        print(f"  draw rate {r:.2f} -> spread {spread_from_draw_rate(r):.4f}")
    for s in (0.45, 0.3773):
        print(f"  spread {s:.4f} -> draw rate {draw_rate_from_spread(s):.4f}")


if __name__ == "__main__":
    main()
