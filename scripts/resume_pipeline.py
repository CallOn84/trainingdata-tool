#!/usr/bin/env python3
"""Pick up the overnight pipeline after its parent died mid-run.

The first driver streamed the converter's stdout through the parent process;
the parent died around 01:14 while the converter carried on as an orphan, so
the rescore and pack stages would never have fired. This one never holds a
pipe to a child: each stage writes straight to its own log file and the
parent only polls, so a dead parent is the only thing that can stop it.

Stage 1a (the 28 normal PGNs) is already running as an orphan; wait it out
rather than starting a second converter over the same output.
"""
import subprocess, sys, time
from pathlib import Path

SCRIPTS = Path(r"C:\Users\Contrad\Documents\Code\repos\lc0-training\training-data-tool\scripts")
TOOL    = Path(r"C:\Users\Contrad\Documents\Code\repos\lc0-training\training-data-tool\build\trainingdata-tool.exe")
PGNDIR  = Path(r"C:\Users\Contrad\Documents\fishtest-pgns\new-pgns")
ROOT    = Path(r"C:\Users\Contrad\Documents\training-data\Fishtest-Redo")
TARS    = Path(r"C:\Users\Contrad\Documents\training-data\Fishtest-Redo-tars")
SYZYGY  = Path(r"C:\Users\Contrad\Documents\syzygy\3-4-5")
LOGDIR  = Path(r"C:\Users\Contrad\Documents\training-data\logs")
SUSPECT = "6a736ab02cf557d58a2e1f56.pgn.gz"

def log(m): print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)

def converter_running():
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq trainingdata-tool.exe"],
                       capture_output=True, text=True)
    return "trainingdata-tool.exe" in r.stdout

def run(name, cmd, logfile, limit):
    log(f"{name}: starting -> {logfile.name}")
    t0 = time.time()
    with open(logfile, "w", encoding="utf-8", errors="replace") as fh:
        p = subprocess.Popen(cmd, stdout=fh, stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=limit)
        except subprocess.TimeoutExpired:
            p.kill()
            log(f"{name}: TIMED OUT after {limit}s -- killed, partial output kept")
            return False
    log(f"{name}: rc={p.returncode} in {int(time.time()-t0)}s")
    return p.returncode == 0

def main():
    log("=" * 60)
    log("resume driver starting")

    # 1. Let the orphaned converter finish.
    waited = 0
    while converter_running() and waited < 4 * 3600:
        time.sleep(30); waited += 30
        if waited % 600 == 0:
            n = len([d for d in ROOT.iterdir() if d.is_dir()])
            log(f"  waiting on orphaned converter: {n} dirs, {waited//60} min")
    log(f"converter finished after waiting {waited//60} min")

    # 2. The suspect file, separately and time-boxed.
    susp = PGNDIR / SUSPECT
    if susp.is_file() and not any(ROOT.glob("fishtest-suspect-*")):
        run("STAGE 1b suspect",
            [str(TOOL), "-pgn-eval-mode", "-wdl-spread", "0.85",
             "-visit-budget", "850", "-threads", "8", "-chunks-per-dir", "6000",
             "-output", f"{ROOT.as_posix()}/fishtest-suspect-", str(susp)],
            LOGDIR / "convert-suspect.log", 45 * 60)

    dirs = [d for d in ROOT.iterdir() if d.is_dir()]
    log(f"{len(dirs)} chunk directories present")
    if not dirs:
        log("nothing to do"); return 1

    run("STAGE 2 rescore",
        [sys.executable, str(SCRIPTS / "rescore_all.py"), str(ROOT),
         "--syzygy", str(SYZYGY), "--replace", "--resume"],
        LOGDIR / "rescore.log", 10 * 3600)

    run("STAGE 3 pack",
        [sys.executable, str(SCRIPTS / "pack_chunks.py"), str(ROOT),
         "--output-dir", str(TARS), "--resume"],
        LOGDIR / "pack.log", 6 * 3600)

    log("PIPELINE COMPLETE")
    return 0

if __name__ == "__main__":
    sys.exit(main())
