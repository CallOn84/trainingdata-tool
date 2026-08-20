#!/usr/bin/env python3
"""Bucket real self-play (Q, D) by |Q| band, compare real average D against
our PawnScoreToWDL formula's prediction (fed the SAME real Q as if it were
a "score_pawns" input) at a few candidate s values -- a real fit-quality
check across the whole curve, not just one point at Q=0."""
import glob
import gzip
import math
import struct
import sys

STRUCT_FMT = "<" "I" "I" "1858f" "104Q" "8B" "15f" "I" "H" "H" "f" "I"
STRUCT_SIZE = 8356
FLOATS_START = 2 + 1858 + 104 + 8


def read_root_qd(filename):
    with gzip.open(filename, "rb") as f:
        while True:
            data = f.read(STRUCT_SIZE)
            if not data or len(data) != STRUCT_SIZE:
                return
            unpacked = struct.unpack(STRUCT_FMT, data)
            yield unpacked[FLOATS_START], unpacked[FLOATS_START + 2]


def logistic(a):
    if a > 20:
        return 1.0
    if a < -20:
        return 0.0
    return 1.0 / (1.0 + math.exp(-a))


def predicted_d(q, s):
    # Mirrors PawnScoreToWDL: treat the real Q itself as if it were the
    # "mu" input (a stand-in for what a pawn score implying this Q would
    # be), same as feeding score_pawns=q into the formula directly.
    w = logistic((q - 1.0) / s)
    l = logistic((-q - 1.0) / s)
    return max(0.0, 1.0 - w - l)


def main():
    pattern = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 800
    candidate_s = [float(x) for x in sys.argv[3].split(",")] if len(sys.argv) > 3 \
        else [1.4, 0.357, 0.28, 0.32]

    files = sorted(glob.glob(pattern))[:limit]
    print(f"Scanning {len(files)} files...", file=sys.stderr)

    buckets = {}  # band -> list of (q, d)
    bands = [(0.0, 0.03), (0.05, 0.15), (0.2, 0.3), (0.4, 0.5), (0.6, 0.7),
             (0.8, 0.9)]

    for f in files:
        try:
            for q, d in read_root_qd(f):
                aq = abs(q)
                for lo, hi in bands:
                    if lo <= aq < hi:
                        buckets.setdefault((lo, hi), []).append((q, d))
                        break
        except Exception:
            pass

    header = "band            n       real_avg_D  " + "  ".join(
        f"s={s:<6}" for s in candidate_s)
    print(header)
    for lo, hi in bands:
        pts = buckets.get((lo, hi), [])
        if not pts:
            continue
        real_avg = sum(d for _, d in pts) / len(pts)
        preds = []
        for s in candidate_s:
            pred_avg = sum(predicted_d(q, s) for q, _ in pts) / len(pts)
            preds.append(pred_avg)
        pred_str = "  ".join(f"{p:.4f}  " for p in preds)
        print(f"[{lo:.2f},{hi:.2f})   {len(pts):<6}  {real_avg:.4f}      {pred_str}")


if __name__ == "__main__":
    main()
