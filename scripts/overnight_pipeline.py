#!/usr/bin/env python3
"""Convert every PGN, then rescore with Syzygy, then pack to tars.

Two things shape the structure:

1. The writer's file counter lives in the process, so continuous numbering
   across inputs means ONE invocation covering all the PGNs. Separate
   invocations would restart at game_000000 and collide -- which is the bug
   that destroyed the previous run.

2. One input (SUSPECT below) hung the previous run: it spun on a single core
   for four hours without writing. It is converted last, in its own
   invocation under a timeout, so it cannot hold up the other 28. Its output
   goes to a separate prefix precisely because a second invocation restarts
   numbering.

A hang is no longer destructive: whatever was written before it stays valid
and correctly numbered, so the run can simply be resumed.
"""
import subprocess, sys, time
from pathlib import Path

TOOL    = Path(r"C:\Users\Contrad\Documents\Code\repos\lc0-training\training-data-tool\build\trainingdata-tool.exe")
SCRIPTS = Path(r"C:\Users\Contrad\Documents\Code\repos\lc0-training\training-data-tool\scripts")
PGNDIR  = Path(r"C:\Users\Contrad\Documents\fishtest-pgns\new-pgns")
ROOT    = Path(r"C:\Users\Contrad\Documents\training-data\Fishtest-Redo")
TARS    = Path(r"C:\Users\Contrad\Documents\training-data\Fishtest-Redo-tars")
SYZYGY  = Path(r"C:\Users\Contrad\Documents\syzygy\3-4-5")

THREADS = 8
SUSPECT = "6a736ab02cf557d58a2e1f56.pgn.gz"   # hung the previous run
CONVERT_LIMIT = 8 * 3600
SUSPECT_LIMIT = 45 * 60

def log(m): print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)

def convert(pgns, prefix, limit, label):
    if not pgns:
        log(f"{label}: nothing to do"); return
    cmd = [str(TOOL), "-pgn-eval-mode", "-wdl-spread", "0.85",
           "-visit-budget", "850", "-threads", str(THREADS),
           "-chunks-per-dir", "6000", "-output", prefix] + [str(p) for p in pgns]
    log(f"{label}: {len(pgns)} file(s) -> {prefix}")
    t0 = time.time()
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, bufsize=1)
        for line in proc.stdout:
            line = line.rstrip()
            if "Finished" in line or "All inputs" in line or "Processing" in line:
                log(f"  {line}")
        proc.wait(timeout=max(1, limit - int(time.time() - t0)))
        log(f"{label}: rc={proc.returncode} in {int(time.time()-t0)}s")
    except subprocess.TimeoutExpired:
        proc.kill()
        log(f"{label}: TIMED OUT after {int(time.time()-t0)}s -- killed, "
            f"output written so far is valid and kept")

def stage(name, cmd, limit):
    log(f"{name}: starting")
    t0 = time.time()
    try:
        rc = subprocess.run(cmd, timeout=limit).returncode
        log(f"{name}: rc={rc} in {int(time.time()-t0)}s")
        return rc == 0
    except subprocess.TimeoutExpired:
        log(f"{name}: TIMED OUT after {limit}s")
        return False

def main():
    log("=" * 64)
    log("overnight pipeline starting")
    ROOT.mkdir(parents=True, exist_ok=True)

    allp = sorted(PGNDIR.glob("*.pgn.gz"))
    good = [p for p in allp if p.name != SUSPECT]
    susp = [p for p in allp if p.name == SUSPECT]
    log(f"{len(allp)} PGNs found ({len(good)} normal, {len(susp)} suspect)")

    if not any(ROOT.glob("fishtest-data-*")):
        convert(good, f"{ROOT.as_posix()}/fishtest-data-", CONVERT_LIMIT,
                "STAGE 1a convert")
    else:
        log("STAGE 1a: output already present, skipping")

    if susp and not any(ROOT.glob("fishtest-suspect-*")):
        convert(susp, f"{ROOT.as_posix()}/fishtest-suspect-", SUSPECT_LIMIT,
                "STAGE 1b convert suspect")

    dirs = [d for d in ROOT.iterdir() if d.is_dir()]
    total = sum(1 for d in dirs for _ in d.glob("*.gz"))
    log(f"conversion done: {len(dirs)} directories, {total} chunk files")
    if not dirs:
        log("nothing produced -- stopping"); return 1

    stage("STAGE 2 rescore",
          [sys.executable, str(SCRIPTS / "rescore_all.py"), str(ROOT),
           "--syzygy", str(SYZYGY), "--replace", "--resume"], 10 * 3600)
    stage("STAGE 3 pack",
          [sys.executable, str(SCRIPTS / "pack_chunks.py"), str(ROOT),
           "--output-dir", str(TARS), "--resume"], 6 * 3600)

    log("PIPELINE COMPLETE")
    return 0

if __name__ == "__main__":
    sys.exit(main())
