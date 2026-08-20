#!/usr/bin/env python3
"""Finish prematurely-adjudicated PGN games with Stockfish.

Fishtest/cutechess-cli test games are usually stopped early by adjudication
(a sustained eval imbalance) rather than played out to an actual checkmate,
stalemate, or rule-based draw. That's fine for measuring engine strength,
but it means the *true* number of plies remaining -- the ground truth the
moves-left-head (MLH/M) training target is computed from in PGNGame.cpp --
never gets recorded for the tail of those games, since the recorded game
simply stops short of the real end.

This script reads a PGN (optionally gzip-compressed), and for every game
whose header says it ended by adjudication (configurable), keeps playing
both sides with Stockfish -- at a shallow, fast depth by default, since we
only need a real conclusion, not a strong one -- until the position is
actually checkmate, stalemate, or a claimable rule draw (50-move/
repetition/insufficient material), or a safety ply cap is hit. Each added
move gets an eval comment in the exact same self-relative
"{SCORE/DEPTH TIMEs}" / "{+M<N>/DEPTH TIMEs}" format the real Fishtest
comments use, so the output PGN needs no changes to be read straight back
in with trainingdata-tool's `-pgn-eval-mode`.

Games that are already real conclusions, or whose termination isn't in the
configured set, are copied straight through unchanged.

Usage:
    py scripts/finish_games.py INPUT.pgn[.gz] [INPUT2.pgn[.gz] ...] \
        --stockfish "C:\\path\\to\\stockfish.exe" [--depth 10] [--workers 8]

Output defaults to "<input-stem>-finished.pgn" next to each input file
(always plain text -- trainingdata-tool doesn't read gzip PGNs).
"""

import argparse
import gzip
import io
import multiprocessing
import os
import sys
import time
from pathlib import Path

import chess
import chess.engine
import chess.pgn

DEFAULT_STOCKFISH = (
    r"C:\Users\Contrad\Documents\Stockfish\stockfish-windows-x86-64-avx2"
    r"\stockfish\stockfish-windows-x86-64-avx2.exe"
)

# Per-worker globals, set once by _init_worker so each process pays the
# Stockfish startup cost exactly once instead of per game.
_ENGINE = None
_DEPTH = None
_MAX_EXTRA_PLIES = None


def _init_worker(stockfish_path, depth, max_extra_plies, threads, hash_mb):
    global _ENGINE, _DEPTH, _MAX_EXTRA_PLIES
    _DEPTH = depth
    _MAX_EXTRA_PLIES = max_extra_plies
    _ENGINE = chess.engine.SimpleEngine.popen_uci(stockfish_path)
    _ENGINE.configure({"Hash": hash_mb, "Threads": threads})


def _format_eval_comment(pov_score, depth, elapsed_s):
    """Render a python-chess PovScore in Fishtest's own comment format."""
    if pov_score.is_mate():
        n = pov_score.mate()
        sign = "+" if n > 0 else "-"
        return f"{sign}M{abs(n)}/{depth} {elapsed_s:.3f}s"
    pawns = pov_score.score() / 100.0
    return f"{pawns:+.2f}/{depth} {elapsed_s:.3f}s"


def _finish_game(game):
    """Play out `game` to a real conclusion in place. Returns stats dict."""
    node = game.end()
    board = node.board()
    extra_plies = 0
    resolved = board.is_game_over(claim_draw=True)

    while not resolved and extra_plies < _MAX_EXTRA_PLIES:
        mover = board.turn
        start = time.time()
        info = _ENGINE.analyse(board, chess.engine.Limit(depth=_DEPTH))
        elapsed = time.time() - start
        pv = info.get("pv")
        if not pv:
            break  # No legal moves found by search; bail out safely.
        move = pv[0]
        score = info["score"].pov(mover)
        comment = _format_eval_comment(score, _DEPTH, elapsed)

        node = node.add_main_variation(move)
        node.comment = comment
        board.push(move)
        extra_plies += 1
        resolved = board.is_game_over(claim_draw=True)

    if resolved:
        game.headers["Result"] = board.result(claim_draw=True)
        if game.headers.get("Termination") not in (None, "", "normal"):
            game.headers["OriginalTermination"] = game.headers["Termination"]
        game.headers["Termination"] = "normal"
        game.headers["FinishedBy"] = f"stockfish-depth{_DEPTH}"
    elif extra_plies > 0:
        game.headers["FinishAttempt"] = (
            f"stockfish-depth{_DEPTH}-capped-at-{_MAX_EXTRA_PLIES}-plies-unresolved"
        )

    return {"extended": extra_plies > 0, "extra_plies": extra_plies, "resolved": resolved}


def _process_one(args):
    game_text, only_terminations, skip_draws = args
    game = chess.pgn.read_game(io.StringIO(game_text))
    if game is None:
        return None, {"skipped": True}

    termination = game.headers.get("Termination", "")
    already_over = game.end().board().is_game_over(claim_draw=True)
    # A drawn adjudication has no "almost mate" being cut short -- the real
    # result is already a draw either way, so playing it out with a shallow
    # engine just burns time (often grinding to the ply cap) for a target
    # that isn't wrong to begin with. Only decisive (non-draw) results are
    # worth finishing.
    is_draw = game.headers.get("Result", "") not in ("1-0", "0-1")
    draw_skipped = skip_draws and is_draw and not already_over
    needs_finish = (not already_over) and (not draw_skipped) and (
        not only_terminations or termination in only_terminations
    )

    stats = {"extended": False, "extra_plies": 0, "resolved": already_over,
              "skipped": False, "draw_skipped": draw_skipped}
    if needs_finish:
        stats = _finish_game(game)
        stats["skipped"] = False
        stats["draw_skipped"] = False

    exporter = chess.pgn.StringExporter(headers=True, variations=False, comments=True)
    return game.accept(exporter), stats


def _open_maybe_gzip(path):
    if str(path).endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return open(path, "r", encoding="utf-8", errors="replace")


def count_games(path):
    """Fast pre-scan for the progress bar's total -- just counts "[Event "
    header lines rather than fully parsing every game."""
    n = 0
    with _open_maybe_gzip(path) as f:
        for line in f:
            if line.startswith("[Event "):
                n += 1
    return n


def _format_duration(seconds):
    seconds = max(0, int(seconds))
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    if h:
        return f"{h}h{m:02d}m"
    if m:
        return f"{m}m{s:02d}s"
    return f"{s}s"


def _print_progress(current, total, extended, resolved, capped, start_time):
    elapsed = time.time() - start_time
    width = 30
    if total:
        frac = min(1.0, current / total)
        filled = int(width * frac)
        bar = "#" * filled + "-" * (width - filled)
        pct = f"{frac * 100:5.1f}%"
        count = f"{current}/{total}"
        rate = current / elapsed if elapsed > 0 else 0
        eta = _format_duration((total - current) / rate) if rate > 0 else "?"
    else:
        bar = "-" * width
        pct = "  ?  "
        count = str(current)
        eta = "?"
    msg = (f"\r[{bar}] {pct} {count} games | {extended} extended "
           f"({resolved} ok, {capped} capped) | elapsed "
           f"{_format_duration(elapsed)} | ETA {eta}")
    sys.stderr.write(msg.ljust(140))
    sys.stderr.flush()


def iter_game_texts(path):
    with _open_maybe_gzip(path) as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                return
            exporter = chess.pgn.StringExporter(headers=True, variations=False,
                                                 comments=True)
            yield game.accept(exporter)


def finish_pgn_file(input_path, output_path, stockfish_path, depth,
                     max_extra_plies, workers, threads, hash_mb,
                     only_terminations, limit, skip_draws=True,
                     show_progress=True):
    total = extended = resolved = capped = skipped = draws_skipped = 0
    t0 = time.time()
    last_print = 0.0

    progress_total = None
    if show_progress:
        print("Counting games...", file=sys.stderr)
        progress_total = count_games(input_path)
        if limit is not None:
            progress_total = min(progress_total, limit)

    def texts():
        nonlocal total
        for text in iter_game_texts(input_path):
            if limit is not None and total >= limit:
                return
            total += 1
            yield text, only_terminations, skip_draws

    # Write to a .partial and rename only on success. A run killed midway
    # (closed shell, reboot) then leaves no file that could be mistaken for
    # a complete one, so --resume can safely skip whatever finished.
    partial_path = Path(str(output_path) + ".partial")

    with multiprocessing.Pool(
        processes=workers,
        initializer=_init_worker,
        initargs=(stockfish_path, depth, max_extra_plies, threads, hash_mb),
    ) as pool, open(partial_path, "w", encoding="utf-8") as out:
        for game_text, stats in pool.imap(_process_one, texts(), chunksize=4):
            if game_text is None:
                skipped += 1
                continue
            out.write(game_text)
            out.write("\n\n")
            if stats.get("draw_skipped"):
                draws_skipped += 1
            if stats["extended"]:
                extended += 1
                if stats["resolved"]:
                    resolved += 1
                else:
                    capped += 1
            if show_progress and time.time() - last_print >= 0.5:
                _print_progress(total, progress_total, extended, resolved,
                                 capped, t0)
                last_print = time.time()

    if show_progress:
        _print_progress(total, progress_total, extended, resolved, capped, t0)
        sys.stderr.write("\n")

    os.replace(partial_path, output_path)

    elapsed = time.time() - t0
    # flush=True on every line: the progress bar writes to stderr with \r and
    # keeps that stream hot, but stdout is block-buffered when redirected to a
    # log, so without this these summaries are lost entirely if the run is
    # killed -- which then leaves no way to tell which files completed.
    print(f"Done: {total} games -> {output_path}", flush=True)
    print(f"  {extended} games extended: {resolved} reached a real "
          f"conclusion, {capped} hit the {max_extra_plies}-ply cap unresolved",
          flush=True)
    if skip_draws:
        print(f"  {draws_skipped} drawn adjudications left untouched "
              f"(no mate distance to correct)", flush=True)
    print(f"  {skipped} unparseable games skipped, {elapsed:.0f}s total",
          flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="Input .pgn or .pgn.gz file(s)")
    ap.add_argument("--stockfish", default=DEFAULT_STOCKFISH,
                     help="Path to the Stockfish binary")
    ap.add_argument("--depth", type=int, default=10,
                     help="Search depth per continuation move (default: 10, "
                          "matches trainingdata-tool's own default)")
    ap.add_argument("--max-extra-plies", type=int, default=300,
                     help="Safety cap on plies added per game (default: 300)")
    ap.add_argument("--workers", type=int, default=max(1, os.cpu_count() - 1),
                     help="Parallel worker processes, one Stockfish each "
                          "(default: cpu_count - 1)")
    ap.add_argument("--threads", type=int, default=1,
                     help="UCI Threads per Stockfish instance (default: 1; "
                          "parallelism comes from --workers instead)")
    ap.add_argument("--hash", type=int, default=64, dest="hash_mb",
                     help="UCI Hash MB per Stockfish instance (default: 64)")
    ap.add_argument("--only-termination", action="append",
                     default=None,
                     help="Only finish games with this [Termination] value "
                          "(repeatable; default: just 'adjudication')")
    ap.add_argument("--include-draws", action="store_true",
                     help="Also finish games that were adjudicated as a "
                          "draw (default: skip them -- a drawn result has "
                          "no mate distance being cut short, so finishing "
                          "it just burns engine time for no benefit)")
    ap.add_argument("--output", help="Output path (single input file only)")
    ap.add_argument("--output-dir", help="Directory for outputs (multiple inputs)")
    ap.add_argument("--limit", type=int, default=None,
                     help="Stop after this many games (per input file, for testing)")
    ap.add_argument("--resume", action="store_true",
                    help="Skip inputs whose finished output already exists. "
                         "Outputs are written to a .partial and renamed on "
                         "completion, so a killed run costs at most one file. "
                         "An output left by a run that predates .partial may "
                         "be truncated -- delete the last one written before "
                         "resuming over it")
    ap.add_argument("--no-progress", action="store_true",
                     help="Disable the progress bar (and its game pre-count pass)")
    args = ap.parse_args()

    if not Path(args.stockfish).is_file():
        ap.error(f"Stockfish binary not found: {args.stockfish}")

    only_terminations = set(args.only_termination) if args.only_termination else {"adjudication"}

    if args.output and len(args.inputs) != 1:
        ap.error("--output can only be used with a single input file")

    for input_str in args.inputs:
        input_path = Path(input_str)
        if not input_path.is_file():
            print(f"Skipping missing file: {input_path}", file=sys.stderr)
            continue

        if args.output:
            output_path = Path(args.output)
        else:
            stem = input_path.name
            for suffix in (".pgn.gz", ".pgn"):
                if stem.endswith(suffix):
                    stem = stem[: -len(suffix)]
                    break
            out_dir = Path(args.output_dir) if args.output_dir else input_path.parent
            out_dir.mkdir(parents=True, exist_ok=True)
            output_path = out_dir / f"{stem}-finished.pgn"

        if args.resume and output_path.is_file():
            print(f"Resume: '{output_path}' already exists, skipping "
                  f"'{input_path}'", flush=True)
            continue

        print(f"Finishing '{input_path}' -> '{output_path}' "
              f"(depth={args.depth}, workers={args.workers}, "
              f"only Termination in {sorted(only_terminations)}, "
              f"draws {'included' if args.include_draws else 'skipped'})",
              flush=True)
        finish_pgn_file(
            input_path=input_path,
            output_path=output_path,
            stockfish_path=args.stockfish,
            depth=args.depth,
            max_extra_plies=args.max_extra_plies,
            workers=args.workers,
            threads=args.threads,
            hash_mb=args.hash_mb,
            only_terminations=only_terminations,
            limit=args.limit,
            skip_draws=not args.include_draws,
            show_progress=not args.no_progress,
        )


if __name__ == "__main__":
    main()
