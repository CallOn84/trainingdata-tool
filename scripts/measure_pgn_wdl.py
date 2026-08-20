#!/usr/bin/env python3
"""Measure the full W/D/L distribution as a function of eval, from the real
outcomes of the games being converted.

measure_pgn_draw_rate.py answers "how often was it a draw at this eval".
This answers the bigger question: "what were the actual win/draw/loss
frequencies", which gives BOTH training targets empirically:

    Q(eval) = W - L        D(eval) = D

That matters because the two things we could otherwise use are both
suspect:

  * lc0's cp = 90*tan(1.5637541897*Q) is a *display* convention for
    rendering Q as a centipawn-looking number. Inverting it gives a Q, but
    it was never fitted to real outcome frequencies, so there is no reason
    to expect it to be calibrated.

  * An engine's own WDL (Stockfish's UCI_ShowWDL) *is* fitted to outcomes,
    but to that engine's outcomes under its own conditions.

The games in hand settle it directly. Evals in Fishtest/cutechess comments
are self-relative (each engine annotates its own move from its own point of
view), so the result is converted to the mover's perspective to match.

Usage:
    py scripts/measure_pgn_wdl.py games.pgn[.gz] [--limit N]
"""
import argparse
import gzip
import math
import re
import sys

SCORE_RE = re.compile(r"\{\s*([+-]?\d+(?:\.\d+)?)/\d+")
MOVE_TOKEN_RE = re.compile(r"\{[^}]*\}|\S+")

# Signed eval bands, in the decimal form the PGN uses.
EDGES = [-5, -3, -2, -1.5, -1.0, -0.75, -0.5, -0.25, -0.1,
         0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2, 3, 5]


def band_of(v):
    for i, e in enumerate(EDGES):
        if v < e:
            return i
    return len(EDGES)


def band_label(i):
    lo = "-inf" if i == 0 else f"{EDGES[i - 1]:g}"
    hi = "+inf" if i == len(EDGES) else f"{EDGES[i]:g}"
    return f"[{lo}, {hi})"


def open_maybe_gzip(path):
    if str(path).endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return open(path, "r", encoding="utf-8", errors="replace")


def lc0_q_from_cp(cp):
    """lc0's display convention, inverted -- NOT a calibrated model."""
    return math.atan(cp / 90.0) / 1.5637541897


def model_wl(eval_pawns, scale, spread):
    """lc0's WDL_mu model: search.cc reports score = 100*mu, so mu is the
    eval in pawns and the same logistic pair WDLRescale() uses applies
    directly. Mirrors wdl::ScoreToWDL in src/WdlConversion.h."""
    mu = scale * eval_pawns
    w = 1.0 / (1.0 + math.exp(-(mu - 1.0) / spread))
    l = 1.0 / (1.0 + math.exp(-(-mu - 1.0) / spread))
    return w, l


def fit_rmse(bands, scale, spread):
    """RMSE of the model's (W, L) against the measured (W, L), weighted by
    band population so a thin tail band can't dominate the fit."""
    se = 0.0
    tot = 0
    for avg, n, w, d, l in bands:
        mw, ml = model_wl(avg, scale, spread)
        se += n * ((mw - w) ** 2 + (ml - l) ** 2)
        tot += n
    return math.sqrt(se / (2 * tot)) if tot else float("nan")


def grid_search(bands):
    best = None
    scale = 0.20
    while scale <= 4.0001:
        spread = 0.05
        while spread <= 1.2001:
            r = fit_rmse(bands, scale, spread)
            if best is None or r < best[2]:
                best = (scale, spread, r)
            spread += 0.01
        scale += 0.01
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pgn")
    ap.add_argument("--limit", type=int, default=8000)
    args = ap.parse_args()

    nb = len(EDGES) + 1
    win = [0] * nb
    draw = [0] * nb
    loss = [0] * nb
    sum_eval = [0.0] * nb

    result = None
    stm_white = True     # side to move at the start of the movetext
    movetext = []
    games = 0

    def flush():
        nonlocal result, movetext
        if result is None or not movetext:
            result = None
            movetext = []
            return
        text = " ".join(movetext)
        white_to_move = stm_white
        for tok in MOVE_TOKEN_RE.finditer(text):
            s = tok.group(0)
            if s.startswith("{"):
                m = SCORE_RE.match(s if s.startswith("{") else "{" + s)
                if not m:
                    continue
                v = float(m.group(1))
                # The eval belongs to the move just played, i.e. the side
                # that was to move *before* this token flipped it below.
                mover_was_white = not white_to_move
                if result == "1/2-1/2":
                    outcome = 0
                elif result == "1-0":
                    outcome = 1 if mover_was_white else -1
                elif result == "0-1":
                    outcome = -1 if mover_was_white else 1
                else:
                    continue
                b = band_of(v)
                sum_eval[b] += v
                if outcome > 0:
                    win[b] += 1
                elif outcome < 0:
                    loss[b] += 1
                else:
                    draw[b] += 1
            elif re.match(r"^\d+\.+$", s):
                continue
            elif s in ("1-0", "0-1", "1/2-1/2", "*"):
                continue
            else:
                white_to_move = not white_to_move
        result = None
        movetext = []

    with open_maybe_gzip(args.pgn) as f:
        for line in f:
            if line.startswith("[Result "):
                flush()
                m = re.search(r'"([^"]*)"', line)
                result = m.group(1) if m else None
                stm_white = True
            elif line.startswith("[FEN "):
                m = re.search(r'"([^"]*)"', line)
                if m:
                    parts = m.group(1).split()
                    stm_white = (len(parts) < 2 or parts[1] == "w")
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

    print(f"Scanned {games} games from {args.pgn}")
    print("\nEmpirical W/D/L by eval (mover's perspective):\n")
    print("  band                n        avg_eval   W       D       L     "
          "  Q_real   Q_lc0formula  diff")
    bands = []
    for i in range(nb):
        n = win[i] + draw[i] + loss[i]
        if n < 200:
            continue
        w, d, l = win[i] / n, draw[i] / n, loss[i] / n
        avg = sum_eval[i] / n
        bands.append((avg, n, w, d, l))
        q_real = w - l
        q_lc0 = lc0_q_from_cp(avg * 100.0)
        print(f"  {band_label(i):<18} {n:<8} {avg:+7.3f}   "
              f"{w:.3f}   {d:.3f}   {l:.3f}   {q_real:+.3f}   "
              f"{q_lc0:+.3f}        {q_lc0 - q_real:+.3f}")

    print("\nQ_real  = (wins - losses) / n, straight from the game results.")
    print("Q_lc0formula = atan(cp/90)/1.5637541897, lc0's display convention.")
    print("A large 'diff' means the display convention is not a calibrated")
    print("win-probability model and should not be used as one.")

    if not bands:
        print("\nNot enough data in any band to fit.")
        return

    scale, spread, rmse = grid_search(bands)
    print(f"\nBest fit of lc0's WDL_mu model to those frequencies:")
    print(f"    -wdl-scale {scale:.2f}  -wdl-spread {spread:.2f}"
          f"   (RMSE {rmse:.3f} on (W, L))")
    print("\nModel vs measured, at the fitted values:\n")
    print("  avg_eval    W_real  W_model    D_real  D_model    L_real  L_model")
    for avg, n, w, d, l in bands:
        mw, ml = model_wl(avg, scale, spread)
        md = max(0.0, 1.0 - mw - ml)
        print(f"  {avg:+7.3f}     {w:.3f}   {mw:.3f}      "
              f"{d:.3f}   {md:.3f}      {l:.3f}   {ml:.3f}")
    print("\nPass these to trainingdata-tool -pgn-eval-mode. Re-fit whenever")
    print("the PGN source changes (engines, time control, book, adjudication)")
    print("-- this curve is a property of that data, not a constant.")


if __name__ == "__main__":
    main()
