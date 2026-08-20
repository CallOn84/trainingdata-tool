#!/usr/bin/env python3
"""Two-parameter fit: scale the input score before treating it as mu, in
addition to tuning s.

calibrate_s2.py showed no single s fits both ends of the real D-vs-Q curve.
That's because s alone can't fix a *shape* mismatch: the formula's
transition is anchored at mu=+-1, so tuning s only stretches the curve
vertically, it can't move where the decisive transition happens. Adding a
scale factor k (mu = k * score) lets the transition point move too, which
is the actual degree of freedom that was missing.

Grid-searches (k, s) against real self-play (Q, D) pairs, minimizing mean
squared error in D across |Q| bands (weighted equally per band, so the huge
near-equal bucket doesn't drown out the decisive tail).
"""
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


def predicted_d(score, k, s):
    mu = k * score
    w = logistic((mu - 1.0) / s)
    l = logistic((-mu - 1.0) / s)
    return max(0.0, 1.0 - w - l)


def predicted_q(score, k, s):
    mu = k * score
    w = logistic((mu - 1.0) / s)
    l = logistic((-mu - 1.0) / s)
    return w - l


def main():
    pattern = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 800

    files = sorted(glob.glob(pattern))[:limit]
    print(f"Scanning {len(files)} files...", file=sys.stderr)

    bands = [(0.0, 0.05), (0.05, 0.15), (0.15, 0.25), (0.25, 0.35),
             (0.35, 0.45), (0.45, 0.55), (0.55, 0.65), (0.65, 0.75),
             (0.75, 0.85), (0.85, 0.95)]
    buckets = {b: [] for b in bands}

    for f in files:
        try:
            for q, d in read_root_qd(f):
                aq = abs(q)
                for lo, hi in bands:
                    if lo <= aq < hi:
                        buckets[(lo, hi)].append((q, d))
                        break
        except Exception:
            pass

    # Per-band real averages (equal weight per band in the fit).
    band_stats = []
    for b in bands:
        pts = buckets[b]
        if len(pts) < 50:
            continue
        avg_q = sum(abs(q) for q, _ in pts) / len(pts)
        avg_d = sum(d for _, d in pts) / len(pts)
        band_stats.append((b, len(pts), avg_q, avg_d))

    print("\nReal data per band:")
    for b, n, aq, ad in band_stats:
        print(f"  |Q| in [{b[0]:.2f},{b[1]:.2f})  n={n:<6}  avgQ={aq:.4f}  avgD={ad:.4f}")

    best = None
    results = []
    k = 0.2
    while k <= 6.01:
        s = 0.05
        while s <= 3.01:
            sse = 0.0
            for b, n, aq, ad in band_stats:
                pred = predicted_d(aq, k, s)
                sse += (pred - ad) ** 2
            mse = sse / len(band_stats)
            results.append((mse, k, s))
            if best is None or mse < best[0]:
                best = (mse, k, s)
            s += 0.05
        k += 0.1

    results.sort()
    print("\nTop 10 (k, s) fits by mean squared error in D:")
    print("  rank   k       s       RMSE_D")
    for i, (mse, k, s) in enumerate(results[:10]):
        print(f"  {i+1:<6} {k:.2f}    {s:.2f}    {math.sqrt(mse):.4f}")

    mse, k, s = best
    print(f"\nBest fit: k={k:.2f}, s={s:.2f} (RMSE in D = {math.sqrt(mse):.4f})")
    print("\nPer-band check at best fit:")
    print("  band            real_D    pred_D    diff      real_Q   pred_Q(at same score)")
    for b, n, aq, ad in band_stats:
        pd = predicted_d(aq, k, s)
        pq = predicted_q(aq, k, s)
        print(f"  [{b[0]:.2f},{b[1]:.2f})     {ad:.4f}    {pd:.4f}    "
              f"{pd-ad:+.4f}   {aq:.4f}   {pq:.4f}")


if __name__ == "__main__":
    main()
