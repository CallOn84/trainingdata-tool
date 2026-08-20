#!/usr/bin/env python3
"""Pack directories of V6 chunks into .tar archives, lc0-training style.

lc0 ships its training runs as plain `.tar` files whose members are
`<dir>/<chunk>.gz`, so extracting one recreates the directory layout the
loaders expect. This produces the same thing from a converted tree.

    chunks/sup01-0/game_000000.gz     ->   sup01-0.tar
    chunks/sup01-1/game_000500.gz     ->   sup01-1.tar

IMPORTANT -- these archives are for storage and transfer, not for training
directly. The TF pipeline in tf/ globs loose `.gz` files
(train.py:fast_get_chunks walks one level of subdirectories, and
chunkparser.py opens each chunk with gzip.open); nothing in it reads tars.
Extract before training:

    tar -xf sup01-0.tar -C /path/to/chunks

Compression: the members are already gzip-compressed chunks, so re-compressing
the tar buys almost nothing and costs a lot of CPU -- which is exactly why lc0
distributes plain .tar. `--compress gz` is available if you want it anyway;
measure before assuming it helps.

Usage:
    py scripts/pack_chunks.py CHUNK_ROOT --output-dir ARCHIVES [--group N]
"""
import argparse
import os
import sys
import tarfile
import time
from pathlib import Path


def chunk_dirs(root):
    """Immediate subdirectories holding at least one .gz."""
    out = []
    for entry in sorted(os.scandir(root), key=lambda e: e.name):
        if not entry.is_dir():
            continue
        if any(f.endswith(".gz") for f in os.listdir(entry.path)):
            out.append(Path(entry.path))
    return out


def pack(dirs, out_path, compress, verify):
    """Write one archive covering `dirs`. Members are stored as
    `<dirname>/<file>.gz` so extraction recreates the layout."""
    mode = {"none": "w", "gz": "w:gz"}[compress]
    tmp_path = out_path.with_suffix(out_path.suffix + ".partial")
    written = 0
    with tarfile.open(tmp_path, mode) as tar:
        for d in dirs:
            for name in sorted(os.listdir(d)):
                if not name.endswith(".gz"):
                    continue
                tar.add(os.path.join(d, name), arcname=f"{d.name}/{name}")
                written += 1

    if verify:
        with tarfile.open(tmp_path, "r:*") as tar:
            members = sum(1 for m in tar if m.isfile())
        if members != written:
            tmp_path.unlink(missing_ok=True)
            raise RuntimeError(
                f"{out_path.name}: wrote {written} chunks but archive holds "
                f"{members} -- archive discarded, sources untouched")

    # Rename only once the archive is complete and verified, so an interrupted
    # run never leaves a truncated .tar looking finished.
    os.replace(tmp_path, out_path)
    return written


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("chunk_root", help="Directory containing sup*-N/ folders")
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--group", type=int, default=1,
                    help="Chunk directories per archive (default: 1). Raise it "
                         "to trade archive count for archive size")
    ap.add_argument("--compress", choices=["none", "gz"], default="none",
                    help="Members are already gzipped; 'none' (default) "
                         "matches how lc0 ships training data")
    ap.add_argument("--no-verify", action="store_true",
                    help="Skip re-reading each archive to confirm its member "
                         "count. Verification roughly doubles read I/O")
    ap.add_argument("--resume", action="store_true",
                    help="Skip archives that already exist")
    ap.add_argument("--limit", type=int, default=None)
    args = ap.parse_args()

    root = Path(args.chunk_root)
    if not root.is_dir():
        ap.error(f"not a directory: {root}")
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    dirs = chunk_dirs(root)
    if not dirs:
        ap.error(f"no subdirectories containing .gz files under {root}")

    groups = [dirs[i:i + args.group] for i in range(0, len(dirs), args.group)]
    if args.limit:
        groups = groups[:args.limit]

    ext = ".tar" if args.compress == "none" else ".tar.gz"
    print(f"{len(dirs)} chunk dirs -> {len(groups)} archive(s) in {out_dir}",
          flush=True)

    t0 = time.time()
    total = skipped = 0
    for i, group in enumerate(groups, 1):
        name = group[0].name if len(group) == 1 else \
            f"{group[0].name}_to_{group[-1].name}"
        out_path = out_dir / (name + ext)
        if args.resume and out_path.is_file():
            skipped += 1
            continue
        written = pack(group, out_path, args.compress, not args.no_verify)
        total += written
        size = out_path.stat().st_size / 1024 ** 2
        print(f"[{i}/{len(groups)}] {out_path.name}: {written} chunks, "
              f"{size:.1f} MB", flush=True)

    el = time.time() - t0
    print(f"Packed {total} chunks into {len(groups)-skipped} archive(s) "
          f"in {el/60:.1f}m" + (f" ({skipped} skipped)" if skipped else ""),
          flush=True)
    print("Sources were not modified. To use for training, extract first:",
          flush=True)
    print(f"  tar -xf {out_dir}/<archive>{ext} -C <chunk-dir>", flush=True)


if __name__ == "__main__":
    main()
