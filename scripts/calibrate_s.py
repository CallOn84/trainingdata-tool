#!/usr/bin/env python3
"""One-off: sample real V6 self-play chunks, find near-equal positions
(|Q| small), and report the average real D there -- used to calibrate
PawnScoreToWDL's spread parameter against real data instead of guessing."""
import glob
import gzip
import struct
import sys

STRUCT_FMT = "<" "I" "I" "1858f" "104Q" "8B" "15f" "I" "H" "H" "f" "I"
STRUCT_SIZE = 8356
FLOATS_START = 2 + 1858 + 104 + 8  # index of root_q in the unpacked tuple


def read_root_qd(filename):
    with gzip.open(filename, "rb") as f:
        while True:
            data = f.read(STRUCT_SIZE)
            if not data or len(data) != STRUCT_SIZE:
                return
            unpacked = struct.unpack(STRUCT_FMT, data)
            root_q = unpacked[FLOATS_START]
            root_d = unpacked[FLOATS_START + 2]
            yield root_q, root_d


def main():
    pattern = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    q_threshold = float(sys.argv[3]) if len(sys.argv) > 3 else 0.03

    files = sorted(glob.glob(pattern))[:limit]
    print(f"Scanning {len(files)} files (|Q| < {q_threshold} threshold)...",
          file=sys.stderr)

    near_equal_ds = []
    total_positions = 0
    for i, f in enumerate(files):
        try:
            for q, d in read_root_qd(f):
                total_positions += 1
                if abs(q) < q_threshold:
                    near_equal_ds.append(d)
        except Exception as e:
            print(f"  skip {f}: {e}", file=sys.stderr)
        if (i + 1) % 500 == 0:
            print(f"  {i + 1}/{len(files)} files, "
                  f"{len(near_equal_ds)} near-equal positions so far",
                  file=sys.stderr)

    print(f"Total positions scanned: {total_positions}")
    print(f"Near-equal (|Q|<{q_threshold}) positions: {len(near_equal_ds)}")
    if near_equal_ds:
        avg_d = sum(near_equal_ds) / len(near_equal_ds)
        near_equal_ds.sort()
        median_d = near_equal_ds[len(near_equal_ds) // 2]
        print(f"Average real D at near-equal Q: {avg_d:.4f}")
        print(f"Median real D at near-equal Q:  {median_d:.4f}")
        print(f"Min/Max: {min(near_equal_ds):.4f} / {max(near_equal_ds):.4f}")


if __name__ == "__main__":
    main()
